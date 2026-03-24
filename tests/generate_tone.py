# run this once: python generate_tone.py
import numpy as np
import wave, struct

SAMPLE_RATE = 44100
DURATION    = 10        # seconds
FREQUENCY   = 440       # Hz — standard A4 tone, easy to see in Audacity

samples = np.sin(2 * np.pi * FREQUENCY *
                 np.linspace(0, DURATION, int(SAMPLE_RATE * DURATION)))
samples = (samples * 32767).astype(np.int16)

with wave.open("test_audio.wav", "w") as wf:
    wf.setnchannels(1)
    wf.setsampwidth(2)
    wf.setframerate(SAMPLE_RATE)
    wf.writeframes(samples.tobytes())

print("Generated test_audio.wav — 440Hz tone, 10 seconds")