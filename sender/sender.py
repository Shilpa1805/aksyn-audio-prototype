import asyncio
import wave
import websockets
import struct
import time
import sys
import os

# ── Packet format (identical to original) ─────────────────────
HEADER_FMT  = "!IQHBBh"
HEADER_SIZE = struct.calcsize(HEADER_FMT)
ENCODING_PCM = 0

def pack_packet(seq, audio_bytes, sample_rate, channels):
    ts_us = time.time_ns() // 1000
    header = struct.pack(
        HEADER_FMT,
        seq,
        ts_us,
        sample_rate,
        channels,
        ENCODING_PCM,
        len(audio_bytes)
    )
    return header + audio_bytes

async def stream_wav(uri: str, wav_path: str):
    wf = wave.open(wav_path, "rb")

    sample_rate = wf.getframerate()
    channels    = wf.getnchannels()
    chunk       = 1024
    frame_ms    = 1000 * chunk / sample_rate

    print(f"[SENDER] File     : {os.path.basename(wav_path)}")
    print(f"[SENDER] Format   : {sample_rate}Hz, {channels}ch, "
          f"{wf.getsampwidth()*8}-bit PCM")
    print(f"[SENDER] Frame    : {chunk} samples = {frame_ms:.1f}ms per packet")
    print(f"[SENDER] Connecting to {uri} ...\n")

    async with websockets.connect(uri, max_size=2**20) as ws:
        print(f"[SENDER] Connected. Streaming — press Ctrl+C to stop.\n")
        seq = 0

        try:
            while True:
                raw = wf.readframes(chunk)

                if not raw:
                    # Loop the file so stream runs long enough to measure
                    print(f"[SENDER] End of file — looping...")
                    wf.rewind()
                    raw = wf.readframes(chunk)

                packet = pack_packet(seq, raw, sample_rate, channels)
                await ws.send(packet)

                # Sleep to simulate real-time capture timing
                # This is critical — without it we flood the socket
                await asyncio.sleep(chunk / sample_rate)

                seq += 1

        except KeyboardInterrupt:
            print(f"\n[SENDER] Stopped after {seq} packets "
                  f"({seq * frame_ms / 1000:.1f}s of audio).")
        finally:
            wf.close()

if __name__ == "__main__":
    uri      = sys.argv[1] if len(sys.argv) > 1 else "ws://localhost:9001"
    wav_path = sys.argv[2] if len(sys.argv) > 2 else "test_audio.wav"

    if not os.path.exists(wav_path):
        print(f"[SENDER] ERROR: '{wav_path}' not found.")
        print(f"[SENDER] Usage: python sender.py ws://localhost:9001 path/to/audio.wav")
        sys.exit(1)

    asyncio.run(stream_wav(uri, wav_path))
