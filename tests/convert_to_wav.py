# tests/convert_to_wav.py
# Run: python tests/convert_to_wav.py input.mp3 output.wav

import sys
from pydub import AudioSegment

input_path  = sys.argv[1]   # e.g. song.mp3
output_path = sys.argv[2]   # e.g. song.wav

AudioSegment.from_file(input_path).export(output_path, format="wav")
print(f"Converted {input_path} → {output_path}")