# AKSYN Audio Prototype

Real-time audio streaming prototype over UDP with C++ packet engine, jitter buffering, PLC, and delay/jitter measurement.

---

## Architecture

```
[audio_sender] ──UDP──> [audio_receiver]
     │                        │
  captures audio           jitter buffer (configurable depth)
  packs header             PLC on packet loss
  sends frames             logs delay + jitter to CSV
  optional: --drop --delay-ms (network impairment simulation)
```

**Packet header** (`aksyn_audio_common.h`, packed, 16 bytes):

| Field          | Type       | Description              |
|----------------|------------|--------------------------|
| `seq`          | `uint32_t` | Sequence number          |
| `timestamp_us` | `uint64_t` | Microsecond capture time |
| `sample_rate`  | `uint16_t` | e.g. 48000 Hz            |
| `channels`     | `uint8_t`  | 1 = mono                 |
| `encoding`     | `uint8_t`  | 0 = PCM16                |
| `payload_len`  | `uint16_t` | Audio bytes in frame     |

---

## Python Implementation

The easiest way to run the prototype is using the provided Python scripts. This is recommended for first-time users.

### 1. Prerequisites
- **Python 3.8+** installed on your system.
- Open a terminal and install the required dependencies:
  ```powershell
  pip install websockets pyaudio numpy miniaudio
  ```

### 2. Run the Receiver
Open a terminal and start the receiver. It will listen for incoming audio packets and play them back on your speakers.
```powershell
python receiver/receiver.py
```

### 3. Run the Sender
Open a **second** terminal and start the sender. It will read an audio file and stream it to the receiver.
```powershell
python sender/sender.py ws://localhost:9001 test_audio.wav
```
*(Note: You can pass any `.wav`, `.mp3`, or `.flac` file as the last argument).*

---

## C++ Implementation

For maximum performance and lowest latency, you can build the C++ versions of the sender and receiver.

### 1. Prerequisites (Windows)
- **Visual Studio** (with Desktop development with C++ installed).
- **CMake** installed and added to your system PATH.
- **vcpkg** installed for dependency management. *If you don't have vcpkg, [install it here](https://vcpkg.io/en/getting-started).*

### 2. Build Instructions
Open a PowerShell window in the project directory:
```powershell
# 1. Install PortAudio dependency via vcpkg
vcpkg install portaudio

# 2. Configure the project
# IMPORTANT: Ensure your VCPKG_ROOT environment variable is set to your vcpkg installation path
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"

# 3. Build the project
cmake --build build --config Release
```

### 3. Running the C++ Receiver
In one terminal, start the receiver:
```powershell
.\build\Release\audio_receiver.exe --port 9901 --prebuffer 3
```

| Flag          | Default | Description                                        |
|---------------|---------|----------------------------------------------------|
| `--port`      | 9901    | UDP listen port                                    |
| `--prebuffer` | 3       | Jitter buffer depth (frames to buffer before play) |

*The receiver will automatically write its delay measurements to `results/delay_log_cpp.csv` on each run.*

### 4. Running the C++ Sender
In a **second** terminal, start the sender:
```powershell
.\build\Release\audio_sender.exe --ip 127.0.0.1 --port 9901 --sr 48000 --ch 1 --frame 480
```

**Testing Network Resilience:**
You can simulate a poor network connection by injecting artificial packet loss and delay:
```powershell
.\build\Release\audio_sender.exe --ip 127.0.0.1 --port 9901 --sr 48000 --ch 1 --frame 480 --drop 2.0 --delay-ms 5
```

| Flag         | Default     | Description                            |
|--------------|-------------|----------------------------------------|
| `--ip`       | 127.0.0.1   | Receiver IP address                    |
| `--port`     | 9901        | UDP port                               |
| `--sr`       | 48000       | Sample rate (Hz)                       |
| `--ch`       | 1           | Channels (1=mono, 2=stereo)            |
| `--frame`    | 480         | Frame size in samples (10ms @ 48kHz)   |
| `--drop`     | 0.0         | Simulated packet loss rate (%)         |
| `--delay-ms` | 0           | Artificial per-packet delay (ms)       |

---

## NFR Notes

### NFR-1 — Clock synchronisation
Single-machine loopback uses a shared monotonic `timespec_get` clock on both sender and receiver, making clock sync a non-issue for this prototype. For cross-device deployment, NTP or PTP synchronisation would be required before timestamps are comparable across hosts.

### NFR-3 — Packet Loss Concealment (PLC)
On a dropped or late packet the receiver repeats the last successfully decoded frame rather than producing silence. This prevents audible buffer underruns and avoids crashes — the system degrades gracefully. The `--drop` flag on the sender enables controlled simulation of this path.

### NFR-4 — Buffer / latency trade-off
`--prebuffer N` is a runtime-configurable jitter buffer depth. Increasing N adds latency but absorbs more network jitter. Demo: run the same stream with `--prebuffer 1` vs `--prebuffer 5` and compare jitter readings in the CSV to show the trade-off explicitly.

### NFR-5 — Liveness proof
Each run generates a random `stream_nonce` printed by the sender at startup. The receiver detects and prints the same nonce from live packet metadata, proving the receiver is reacting to packets from that exact run and not replaying a recording.

---

## Delay Budget (theoretical model)

| Stage                         | Budget   |
|-------------------------------|----------|
| Audio capture (driver)        | ~10 ms   |
| Frame encode + pack           | ~1 ms    |
| UDP loopback RTT              | ~1 ms    |
| Jitter buffer (3 frames)      | ~30 ms   |
| Audio playback (driver)       | ~10 ms   |
| OS scheduler variance         | ~10 ms   |
| **Total (theoretical)**       | **~62 ms** |

---

## Validation thresholds

| Metric           | Pass criterion  | Rationale                               |
|------------------|-----------------|-----------------------------------------|
| Mean delay       | < 150 ms        | Human echo perception threshold (ITU-T) |
| Jitter (std dev) | < 30 ms         | Perceptible audio choppiness threshold  |
| Packet count     | ≥ 50 packets    | Minimum statistically meaningful sample |
| Packet loss      | System must not crash | PLC handles loss gracefully        |
| Liveness         | nonce match     | Proves live transport, not recording    |

Run the validation script against a captured CSV:

```powershell
python tests/validate.py
```

---

## Project structure

```
aksyn-audio-prototype/
├── cpp/
│   ├── audio_sender.cpp          # UDP sender with PortAudio capture
│   ├── audio_receiver.cpp        # UDP receiver with jitter buffer + PLC
│   ├── aksyn_audio_common.h      # Shared packet header + utils
│   ├── aksyn_audio_common.cpp    # Shared implementation
│   └── packet_demo.cpp           # Standalone demo (no audio device needed)
├── tests/
│   ├── validate.py               # PASS/FAIL validation against CSV log
│   ├── generate_tone.py          # Generates test WAV tones
│   └── convert_to_wav.py         # Converts raw PCM to WAV
├── results/                      # delay_log_cpp.csv lands here
├── recordings/                   # WAV recordings land here
├── docs/
│   ├── delay_budget.md
│   └── cpp_architecture.md
├── CMakeLists.txt
├── vcpkg.json
└── README.md
```
