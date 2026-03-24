import asyncio
import json
import os
import struct
import sys
import tempfile
import time
import wave

import websockets

# ── Packet format ──────────────────────────────────────────────
HEADER_FMT   = "!IQHBBh"
HEADER_SIZE  = struct.calcsize(HEADER_FMT)
ENCODING_PCM = 0


def pack_packet(seq, audio_bytes, sample_rate, channels):
    ts_us  = time.time_ns() // 1000
    header = struct.pack(
        HEADER_FMT,
        seq,
        ts_us,
        sample_rate,
        channels,
        ENCODING_PCM,
        len(audio_bytes),
    )
    return header + audio_bytes


def to_wav_path(audio_path: str):
    """
    Returns (wav_path, is_temp).
    WAV files pass through directly.
    MP3/FLAC/OGG are decoded with miniaudio -> temp WAV (no ffmpeg needed).
    """
    ext = os.path.splitext(audio_path)[1].lower()
    if ext == ".wav":
        return audio_path, False

    try:
        import miniaudio
    except ImportError:
        print("[SENDER] ERROR: Non-WAV files need 'miniaudio'.")
        print("[SENDER]        Run:  .venv\\Scripts\\pip install miniaudio")
        sys.exit(1)

    print(f"[SENDER] Converting {ext.upper()} to WAV (in memory, no ffmpeg needed)...")
    decoded = miniaudio.decode_file(
        audio_path,
        output_format=miniaudio.SampleFormat.SIGNED16,
        nchannels=1,       # mono — must match receiver's CHANNELS = 1
        sample_rate=44100,
    )

    tmp = tempfile.NamedTemporaryFile(suffix=".wav", delete=False)
    with wave.open(tmp.name, "wb") as wf:
        wf.setnchannels(decoded.nchannels)
        wf.setsampwidth(2)  # 16-bit
        wf.setframerate(decoded.sample_rate)
        wf.writeframes(bytes(decoded.samples))
    tmp.close()

    print(f"[SENDER] Done  -> {decoded.num_frames} frames @ "
          f"{decoded.sample_rate}Hz, {decoded.nchannels}ch\n")
    return tmp.name, True


async def _listen_status(ws):
    """Read AJB telemetry JSON sent back by the receiver and print it."""
    try:
        async for msg in ws:
            try:
                s = json.loads(msg)
                if s.get("type") == "status":
                    print(f"  [SENDER] <- STATUS  "
                          f"depth={s['depth']}f  "
                          f"jitter={s['jitter_ms']}ms  "
                          f"pkts={s['packets']}")
            except (json.JSONDecodeError, KeyError):
                pass   # ignore non-JSON messages
    except Exception:
        pass


async def stream_audio(uri: str, audio_path: str):
    wav_path, is_temp = to_wav_path(audio_path)

    try:
        wf          = wave.open(wav_path, "rb")
        sample_rate = wf.getframerate()
        channels    = wf.getnchannels()
        chunk       = 1024
        frame_ms    = 1000 * chunk / sample_rate

        print(f"[SENDER] File   : {os.path.basename(audio_path)}")
        print(f"[SENDER] Format : {sample_rate}Hz, {channels}ch, "
              f"{wf.getsampwidth()*8}-bit PCM")
        print(f"[SENDER] Frame  : {chunk} samples = {frame_ms:.1f}ms per packet")
        print(f"[SENDER] Connecting to {uri} ...\n")

        async with websockets.connect(uri, max_size=2**20) as ws:
            print("[SENDER] Connected. Streaming — press Ctrl+C to stop.")
            print("[SENDER] Will print AJB status feedback from receiver every 50 packets.\n")

            # Run status listener concurrently
            listener = asyncio.create_task(_listen_status(ws))

            seq = 0
            try:
                while True:
                    raw = wf.readframes(chunk)
                    if not raw:
                        print("[SENDER] End of file — looping...")
                        wf.rewind()
                        raw = wf.readframes(chunk)

                    await ws.send(pack_packet(seq, raw, sample_rate, channels))
                    await asyncio.sleep(chunk / sample_rate)
                    seq += 1

            except KeyboardInterrupt:
                print(f"\n[SENDER] Stopped after {seq} packets "
                      f"({seq * frame_ms / 1000:.1f}s of audio).")
            finally:
                listener.cancel()
                wf.close()
    finally:
        if is_temp:
            os.unlink(wav_path)



if __name__ == "__main__":
    uri        = sys.argv[1] if len(sys.argv) > 1 else "ws://localhost:9001"
    audio_path = sys.argv[2] if len(sys.argv) > 2 else "test_audio.wav"

    if not os.path.exists(audio_path):
        print(f"[SENDER] ERROR: '{audio_path}' not found.")
        print("[SENDER] Usage: python sender.py ws://localhost:9001 path/to/audio.(wav|mp3|flac)")
        sys.exit(1)

    asyncio.run(stream_audio(uri, audio_path))
