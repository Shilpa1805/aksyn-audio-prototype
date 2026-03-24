import asyncio
import pyaudio
import websockets
import struct
import time
import wave
import os
import numpy as np
from collections import deque

# ── Audio config (must match sender) ─────────────────────────
SAMPLE_RATE  = 44100
CHANNELS     = 1
FORMAT       = pyaudio.paInt16
CHUNK        = 1024
HEADER_FMT   = "!IQHBBh"
HEADER_SIZE  = struct.calcsize(HEADER_FMT)

# ── Jitter buffer: hold N frames before playback starts ───────
JITTER_BUFFER_FRAMES = 2       # ~46ms pre-buffer — absorbs jitter

# ── Output paths ──────────────────────────────────────────────
RESULTS_DIR    = os.path.join(os.path.dirname(__file__), "..", "results")
RECORDINGS_DIR = os.path.join(os.path.dirname(__file__), "..", "recordings")
os.makedirs(RESULTS_DIR, exist_ok=True)
os.makedirs(RECORDINGS_DIR, exist_ok=True)

def unpack_header(data: bytes):
    seq, ts_us, sr, ch, enc, plen = struct.unpack_from(HEADER_FMT, data, 0)
    payload = data[HEADER_SIZE : HEADER_SIZE + plen]
    return seq, ts_us, sr, ch, enc, payload

class DelayMonitor:
    def __init__(self):
        self.delays   = []
        self.log_path = os.path.join(RESULTS_DIR, "delay_log.csv")
        with open(self.log_path, "w") as f:
            f.write("seq,capture_us,receive_us,delay_ms,jitter_ms\n")

    def record(self, seq, ts_capture_us):
        ts_recv_us = time.time_ns() // 1000
        delay_ms   = (ts_recv_us - ts_capture_us) / 1000.0
        self.delays.append(delay_ms)

        # Rolling jitter = std dev of last 20 samples
        window  = self.delays[-20:]
        jitter  = float(np.std(window)) if len(window) > 1 else 0.0

        with open(self.log_path, "a") as f:
            f.write(f"{seq},{ts_capture_us},{ts_recv_us},{delay_ms:.2f},{jitter:.2f}\n")

        # Print live table row every 10 packets
        if seq % 10 == 0:
            avg = np.mean(self.delays)
            print(f"  [SEQ {seq:05d}]  delay: {delay_ms:6.1f}ms  "
                  f"jitter: ±{jitter:.1f}ms  avg: {avg:.1f}ms")

    def summary(self):
        if not self.delays:
            return
        print("\n" + "="*55)
        print(f"  DELAY SUMMARY ({len(self.delays)} packets)")
        print(f"  Mean   : {np.mean(self.delays):.1f} ms")
        print(f"  Median : {np.median(self.delays):.1f} ms")
        print(f"  Std dev: {np.std(self.delays):.1f} ms  (jitter)")
        print(f"  Min    : {np.min(self.delays):.1f} ms")
        print(f"  Max    : {np.max(self.delays):.1f} ms")
        print(f"  Log    : {self.log_path}")
        print("="*55)

async def receive_audio(host="0.0.0.0", port=9001):
    pa      = pyaudio.PyAudio()
    monitor = DelayMonitor()
    buffer  = deque()
    frames_for_wav = []
    playing = False
    stream  = None

    def make_stream():
        return pa.open(
            format=FORMAT,
            channels=CHANNELS,
            rate=SAMPLE_RATE,
            output=True,
            frames_per_buffer=CHUNK
        )

    print(f"[RECEIVER] Listening on ws://{host}:{port}")
    print(f"[RECEIVER] Jitter buffer: {JITTER_BUFFER_FRAMES} frames "
          f"({1000*CHUNK*JITTER_BUFFER_FRAMES/SAMPLE_RATE:.0f}ms pre-buffer)")
    print(f"[RECEIVER] Recording to: {RECORDINGS_DIR}\n")
    print(f"  {'SEQ':>8}  {'DELAY':>10}  {'JITTER':>10}  {'AVG':>10}")
    print(f"  {'-'*45}")

    async def handler(ws):
        nonlocal playing, stream, buffer
        try:
            async for message in ws:
                seq, ts_us, sr, ch, enc, payload = unpack_header(message)
                monitor.record(seq, ts_us)
                frames_for_wav.append(payload)
                buffer.append(payload)

                # Start playback after jitter buffer fills
                if not playing and len(buffer) >= JITTER_BUFFER_FRAMES:
                    playing = True
                    stream  = make_stream()
                    print(f"\n[RECEIVER] Jitter buffer full — playback started.\n")

                if playing and stream:
                    try:
                        stream.write(buffer.popleft())
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

    def _save_wav(frames):
        if not frames:
            return
        path = os.path.join(RECORDINGS_DIR,
                            f"recording_{int(time.time())}.wav")
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