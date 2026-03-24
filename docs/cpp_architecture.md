# C++ Production Architecture — AKSYN Audio Prototype

## Why C++ for Production, Python for Prototype

The Python prototype served as a rapid iteration environment for validating the
packet design, jitter buffer logic, and delay measurement approach. It is not
suitable for production audio for two reasons:

1. **Python's GIL** causes contention when the audio callback thread and the
   network thread compete. Above ~48 kHz this produces audible dropouts.
2. **asyncio's event loop** introduces non-deterministic sleep drift — visible
   in the delay log as a ~800 ms startup backlog on the first 100 packets before
   the loop stabilises. PortAudio's C callback model runs on a dedicated
   real-time thread with no event loop overhead, eliminating this entirely.

The C++ implementation (`audio_sender.cpp`, `audio_receiver.cpp`,
`aksyn_audio_common.h/cpp`) uses the identical packet format and logic as the
Python prototype, making the migration a drop-in replacement of the transport
layer with no protocol changes.

---

## Third-Party Libraries

| Library | Purpose | Why chosen over alternatives |
|---|---|---|
| **PortAudio** | Audio capture (sender) and playback (receiver) | Cross-platform real-time callback model; dedicated audio thread avoids OS scheduler jitter. Alternative: RtAudio — fewer platforms supported. |
| **Raw UDP sockets** (Winsock2 / POSIX) | Network transport | UDP chosen over TCP: TCP's retransmit-on-loss behaviour actively increases latency in real-time audio. Our sequence numbers handle loss detection without TCP overhead. WebSocket/libwebsockets considered but adds ~3–5 ms framing overhead per packet. |
| **libopus** *(production path, not in prototype)* | Audio codec | 20 ms frames, built-in FEC (Forward Error Correction) handles up to 10% packet loss with no audible artefact. Reduces bandwidth ~10× vs PCM16. The `encoding` field in `AudioPacketHeader` is already reserved for this — switching is a one-field change. |
| **vcpkg** | Dependency management | Declarative manifest (`vcpkg.json`), integrates with CMake toolchain file. PortAudio pulled in with a single line. |

---

## Packet Structure (`aksyn_audio_common.h`)

```cpp
#pragma pack(push, 1)
struct AudioPacketHeader {
    uint32_t magic;           // 'AKSN' — rejects stray UDP datagrams
    uint16_t version;         // protocol version — forward-compatible
    uint16_t header_bytes;    // sizeof(AudioPacketHeader) — self-describing

    uint32_t seq;             // sequence number — detect drops / reorder
    uint64_t capture_mono_us; // sender monotonic timestamp at capture (µs)

    uint32_t sample_rate;     // e.g. 48000 — receiver auto-configures
    uint16_t channels;        // 1 = mono, 2 = stereo
    uint16_t frame_samples;   // samples per channel in this packet

    uint8_t  encoding;        // 0 = PCM16LE, 1 = OPUS (reserved)
    uint8_t  flags;           // reserved
    uint16_t payload_bytes;   // audio bytes following header

    uint64_t stream_nonce;    // random per-run ID — proof of liveness
};
#pragma pack(pop)
```

Total header size: **40 bytes**.

Binary struct chosen over JSON: JSON adds ~3× size overhead and requires string
parsing at 100 packets/sec. At 48 kHz with 480-sample frames the sender
produces one packet every 10 ms — binary keeps the transport layer thin and
deterministic.

---

## Class Design

```
┌─────────────────────────────────────────────────────────────────────┐
│  SENDER (audio_sender.cpp)                                          │
│                                                                     │
│  PortAudio input callback (real-time thread)                        │
│    └─ pa_input_cb()                                                 │
│         ├─ Stamps capture_mono_us via mono_time_us()                │
│         ├─ Fills AudioPacketHeader (seq, nonce, format fields)      │
│         ├─ Applies drop / delay simulation (--drop, --delay-ms)     │
│         └─ aksyn::build_packet() → udp_send()                       │
└─────────────────────────────────────────────────────────────────────┘
                          │  UDP datagrams
                          ▼
┌─────────────────────────────────────────────────────────────────────┐
│  RECEIVER (audio_receiver.cpp)                                      │
│                                                                     │
│  Network thread                                                     │
│    └─ udp_recv() → aksyn::parse_packet()                            │
│         ├─ Validates magic, version, header_bytes                   │
│         ├─ Records net delay: recv_mono_us − capture_mono_us        │
│         ├─ aksyn::RunningStats::add()  → mean / p95 / stddev        │
│         ├─ aksyn::JitterBuffer::push() → ordered by seq             │
│         └─ Writes row to delay_log_cpp.csv                          │
│                                                                     │
│  PortAudio output callback (real-time thread)                       │
│    └─ pa_output_cb()                                                │
│         ├─ aksyn::JitterBuffer::pop_or_plc()                        │
│         │    ├─ Returns next expected seq frame if present          │
│         │    └─ PLC: repeats last good frame on loss (WebRTC model) │
│         └─ aksyn::WavWriter::write_pcm16() → saves to .wav         │
└─────────────────────────────────────────────────────────────────────┘
```

### Key classes in `aksyn_audio_common.h/cpp`

| Class | Responsibility |
|---|---|
| `RunningStats` | Thread-safe accumulator; `snapshot()` returns mean, stddev, p50, p95, min, max over all samples |
| `JitterBuffer` | Bounded deque ordered by `seq`; `pop_or_plc()` returns expected frame or PLC substitute; `prebuffer` and `max` are runtime-configurable |
| `WavWriter` | Writes a valid RIFF/WAVE PCM16 file; patches RIFF chunk sizes on `close()` |

---

## Key Design Decisions and Trade-offs

### TCP vs UDP
TCP retransmits lost segments. In real-time audio, a retransmit arrives too
late to be played — it either causes a buffer stall or is discarded. UDP is the
correct choice; our `seq` field provides all the loss-detection TCP would give
us, without the retransmit penalty.

### Buffer Depth vs Latency (the core trade-off)
The `prebuffer_frames` parameter is the single knob controlling this:

| `--prebuffer` | Added latency | Jitter absorbed |
|---|---|---|
| 1 frame | ~10 ms | ~10 ms |
| 3 frames (default) | ~30 ms | ~30 ms |
| 10 frames | ~100 ms | ~100 ms |

Deeper buffer absorbs more jitter at the cost of higher end-to-end delay.
Exposed as a runtime flag so it can be tuned per deployment without recompile.

### Proof of Liveness — Stream Nonce
`stream_nonce` is a `uint64_t` generated by `random_u64()` (seeded from
`std::random_device`) at sender startup and embedded in every packet header.
The receiver prints the detected nonce on first packet. Because the nonce is
random and unique per session, the receiver's terminal can only display the
correct value if it is receiving live UDP packets from that specific run — it
cannot be replicated by playing back a pre-recorded file.

### Format Negotiation
The receiver does **not** hardcode sample rate, channel count, or frame size.
It reads these from the first live packet and dynamically opens the PortAudio
output stream to match. This means the receiver auto-adapts to any sender
configuration with no restart required.

### Network Impairment Simulation
`audio_sender.cpp` includes built-in simulation flags:
- `--drop <pct>` — randomly drops `pct`% of packets before sending
- `--delay-ms <ms>` — injects a fixed extra delay per packet

This allows validation under realistic degraded conditions without any external
network tools (`tc netem`, etc.).

---

## Production Hardening Path (Beyond Prototype)

| Item | Change Required |
|---|---|
| Opus codec | Set `encoding = 1` in header; wrap payload with `libopus` encode/decode |
| Cross-device delay measurement | Add NTP sync or PTP; `capture_mono_us` already in every packet |
| WASAPI exclusive mode (Windows) | Pass `paWASAPI` host API params to PortAudio; reduces output latency to ~3 ms |
| Stereo / multi-channel | `channels` field already in header; receiver already reads it dynamically |
| Encrypted transport | Wrap UDP payload with DTLS (as in WebRTC) — header format unchanged |