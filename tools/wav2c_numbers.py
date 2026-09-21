#!/usr/bin/env python3
"""Convert assets/{one..twenty_five}.wav (Piper output) into
src/assets/numbers_audio.c - spoken click-threshold values.

Each WAV is resampled to 16 kHz mono 16-bit, silence-trimmed, normalised, and
given short raised-cosine fades (see tools/wav2c_common.py). Run this after
(re)generating the WAVs:

    python3 tools/wav2c_numbers.py

stdlib only. Handles any input rate / mono or stereo / 16-bit PCM WAV.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from wav2c_common import RATE, emit_pcm_array, load_sample

# Any of these whose assets/<word>.wav exists gets compiled in; the generated
# table is sized to the largest one present.
WORDS = {
    "one": 1, "two": 2, "three": 3, "four": 4, "five": 5, "six": 6,
    "seven": 7, "eight": 8, "nine": 9, "ten": 10, "eleven": 11, "twelve": 12,
    "thirteen": 13, "fourteen": 14, "fifteen": 15, "sixteen": 16,
    "seventeen": 17, "eighteen": 18, "nineteen": 19, "twenty": 20,
    "twenty_one": 21, "twenty_two": 22, "twenty_three": 23, "twenty_four": 24,
    "twenty_five": 25,
}

HERE = os.path.dirname(os.path.abspath(__file__))
ASSETS = os.path.join(HERE, "..", "assets")
OUT = os.path.join(HERE, "..", "src", "assets", "numbers_audio.c")


def main():
    samples = {}
    for word, num in WORDS.items():
        path = os.path.join(ASSETS, f"{word}.wav")
        if not os.path.exists(path):
            print(f"  (missing {word}.wav - skipping)", file=sys.stderr)
            continue
        s = load_sample(path)
        samples[num] = s
        print(f"  {word}: {len(s)} frames ({len(s) / RATE * 1000:.0f} ms, {len(s) * 2} B)")

    total = sum(len(s) for s in samples.values()) * 2
    count = (max(samples) + 1) if samples else 1  # valid indices 0 .. count-1
    lines = ['#include "numbers_audio.h"', ""]
    for num, s in sorted(samples.items()):
        lines += emit_pcm_array(f"num{num}_pcm", s)
        lines.append("")
    lines.append(f"const NumberSample kNumberSamples[{count}] = {{")
    for n in range(count):
        if n in samples:
            lines.append(f"    [{n}] = {{ num{n}_pcm, (int)(sizeof(num{n}_pcm) / sizeof(int16_t)) }},")
        else:
            lines.append(f"    [{n}] = {{ 0, 0 }},")
    lines.append("};")
    lines.append(f"const int kNumberSamplesCount = {count};")
    with open(OUT, "w") as f:
        f.write("\n".join(lines) + "\n")
    print(f"wrote {OUT}  (count={count}, {total} B / {total / 1024:.0f} KB of PCM)")


if __name__ == "__main__":
    main()
