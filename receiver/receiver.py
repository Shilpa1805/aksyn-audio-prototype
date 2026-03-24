import asyncio
import json
import os
import struct
import time
import wave
from collections import deque

import numpy as np
import pyaudio
import websockets

# ── Audio config (must match sender) ─────────────────────────
SAMPLE_RATE  = 44100
CHANNELS     = 1
FORMAT       = pyaudio.paInt16
CHUNK        = 1024
HEADER_FMT   = "!IQHBBh"
HEADER_SIZE  = struct.calcsize(HEADER_FMT)

# ── Adaptive Jitter Buffer thresholds ────────────────────────
AJB_WINDOW   = 50          # packets used for rolling std dev
AJB_MIN      = 1           # minimum buffer depth (frames)
AJB_MAX      = 4           # maximum buffer depth (frames)

# ── Output paths ─────────────────────────────────────────────
RESULTS_DIR    = os.path.join(os.path.dirname(__file__), "..", "results")
RECORDINGS_DIR = os.path.join(os.path.dirname(__file__), "..", "recordings")
os.makedirs(RESULTS_DIR, exist_ok=True)
os.makedirs(RECORDINGS_DIR, exist_ok=True)


def unpack_header(data: bytes):
    seq, ts_us, sr, ch, enc, plen = struct.unpack_from(HEADER_FMT, data, 0)
    payload = data[HEADER_SIZE: HEADER_SIZE + plen]
    return seq, ts_us, sr, ch, enc, payload


# ── Adaptive Jitter Buffer ────────────────────────────────────
class AdaptiveJitterBuffer:
    """
    Dynamically adjusts pre-buffer depth based on rolling inter-arrival jitter.

    Thresholds
    ----------
    jitter σ < 10ms  → depth 1   (stable, minimum latency)
    10 ≤ σ < 30ms    → depth 2   (normal)
    30 ≤ σ < 60ms    → depth 3   (bursty network)
    σ ≥ 60ms         → depth 4   (unstable, maximum smoothing)
    """

    def __init__(self):
        self.depth         = 2           # current target depth (frames)
        self.buffer        = deque()
        self._arrivals     = deque(maxlen=AJB_WINDOW)  # wall-clock ms per packet
        self._playing      = False
        self.jitter_ms     = 0.0
        self.underruns     = 0

    # ----------------------------------------------------------
    def push(self, payload: bytes):
        now_ms = time.time_ns() / 1_000_000.0
        self._arrivals.append(now_ms)
        self.buffer.append(payload)
        self._adapt()

    # ----------------------------------------------------------
    def _adapt(self):
        if len(self._arrivals) < 10:
            return

        arr = list(self._arrivals)
        intervals = [arr[i+1] - arr[i] for i in range(len(arr) - 1)]
        self.jitter_ms = float(np.std(intervals))

        if self.jitter_ms < 10.0:
            new = 1
        elif self.jitter_ms < 30.0:
            new = 2
        elif self.jitter_ms < 60.0:
            new = 3
        else:
            new = AJB_MAX

        if new != self.depth:
            print(f"\n  [AJB] jitter={self.jitter_ms:.1f}ms  "
                  f"buffer depth {self.depth} → {new}\n")
            self.depth = new

    # ----------------------------------------------------------
    def ready(self) -> bool:
        return len(self.buffer) >= self.depth

    def pop(self) -> bytes | None:
        """Pop one frame; returns None on underrun (caller should insert silence)."""
        if self.buffer:
            return self.buffer.popleft()
        self.underruns += 1
        print(f"  [AJB] UNDERRUN #{self.underruns} — inserting silence")
        return None              # caller handles silence

    def status_dict(self, seq: int) -> dict:
        return {
            "type"      : "status",
            "depth"     : self.depth,
            "jitter_ms" : round(self.jitter_ms, 2),
            "packets"   : seq,
        }


# ── Delay monitor ─────────────────────────────────────────────
class DelayMonitor:
    def __init__(self):
        self.delays   = []
        self.log_path = os.path.join(RESULTS_DIR, "delay_log.csv")
        with open(self.log_path, "w") as f:
            f.write("seq,capture_us,receive_us,delay_ms,jitter_ms,ajb_depth\n")

    def record(self, seq, ts_capture_us, ajb_depth: int):
        ts_recv_us = time.time_ns() // 1000
        delay_ms   = (ts_recv_us - ts_capture_us) / 1000.0
        self.delays.append(delay_ms)

        window = self.delays[-20:]
        jitter = float(np.std(window)) if len(window) > 1 else 0.0

        with open(self.log_path, "a") as f:
            f.write(f"{seq},{ts_capture_us},{ts_recv_us},"
                    f"{delay_ms:.2f},{jitter:.2f},{ajb_depth}\n")

        if seq % 10 == 0:
            avg = np.mean(self.delays)
            print(f"  [SEQ {seq:05d}]  delay: {delay_ms:6.1f}ms  "
                  f"jitter: ±{jitter:.1f}ms  avg: {avg:.1f}ms  "
                  f"buf: {ajb_depth}f")

    def summary(self):
        if not self.delays:
            return
        print("\n" + "="*60)
        print(f"  DELAY SUMMARY ({len(self.delays)} packets)")
        print(f"  Mean   : {np.mean(self.delays):.1f} ms")
        print(f"  Median : {np.median(self.delays):.1f} ms")
        print(f"  Jitter : {np.std(self.delays):.1f} ms  (std dev)")
        print(f"  Min    : {np.min(self.delays):.1f} ms")
        print(f"  Max    : {np.max(self.delays):.1f} ms")
        print(f"  Log    : {self.log_path}")
        print("="*60)


# ── Main receiver coroutine ───────────────────────────────────
SILENCE = b"\x00" * (CHUNK * 2)   # 1024 mono int16 zero samples

async def receive_audio(host="0.0.0.0", port=9001):
    pa      = pyaudio.PyAudio()
    monitor = DelayMonitor()
    ajb     = AdaptiveJitterBuffer()
    stream  = None
    frames_for_wav: list[bytes] = []

    def make_stream():
        return pa.open(
            format=FORMAT,
            channels=CHANNELS,
            rate=SAMPLE_RATE,
            output=True,
            frames_per_buffer=CHUNK,
        )

    print(f"[RECEIVER] Listening on ws://{host}:{port}")
    print(f"[RECEIVER] Adaptive Jitter Buffer  depth range: {AJB_MIN}–{AJB_MAX} frames")
    print(f"[RECEIVER] Rolling window: {AJB_WINDOW} packets")
    print(f"[RECEIVER] Recording to: {RECORDINGS_DIR}\n")
    print(f"  {'SEQ':>8}  {'DELAY':>10}  {'JITTER':>10}  {'AVG':>10}  {'BUF':>4}")
    print(f"  {'-'*50}")

    async def handler(ws):
        nonlocal stream
        seq_count = 0
        try:
            async for message in ws:
                seq, ts_us, sr, ch, enc, payload = unpack_header(message)
                monitor.record(seq, ts_us, ajb.depth)
                frames_for_wav.append(payload)
                ajb.push(payload)

                # Start playback once adaptive buffer is satisfied
                if stream is None and ajb.ready():
                    stream = make_stream()
                    print(f"\n[RECEIVER] Jitter buffer ready "
                          f"(depth={ajb.depth}) — playback started.\n")

                if stream is not None:
                    frame = ajb.pop()
                    try:
                        stream.write(frame if frame else SILENCE)
                    except Exception:
                        pass

                # Send telemetry back to sender every 50 packets
                seq_count += 1
                if seq_count % 50 == 0:
                    try:
                        await ws.send(json.dumps(ajb.status_dict(seq)))
                    except Exception:
                        pass

        except websockets.exceptions.ConnectionClosed:
            print("\n[RECEIVER] Connection closed.")
        finally:
            if stream:
                stream.stop_stream()
                stream.close()
            pa.terminate()
            monitor.summary()
            _save_wav(frames_for_wav)
            print(f"[RECEIVER] AJB underruns: {ajb.underruns}")

    def _save_wav(frames):
        if not frames:
            return
        path = os.path.join(RECORDINGS_DIR, f"recording_{int(time.time())}.wav")
        with wave.open(path, "wb") as wf:
            wf.setnchannels(CHANNELS)
            wf.setsampwidth(pa.get_sample_size(FORMAT))
            wf.setframerate(SAMPLE_RATE)
            wf.writeframes(b"".join(frames))
        print(f"[RECEIVER] WAV saved → {path}")

    async with websockets.serve(handler, host, port, max_size=2**20):
        await asyncio.Future()   # run forever


if __name__ == "__main__":
    asyncio.run(receive_audio())