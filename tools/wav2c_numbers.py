#!/usr/bin/env python3
"""Convert assets/{four..twelve}.wav (Piper output) into
src/assets/numbers_audio.c - spoken click-threshold values.

Each WAV is resampled to 16 kHz mono 16-bit, silence-trimmed, normalised, and
given short raised-cosine fades. Run this after (re)generating the WAVs:

    python3 tools/wav2c_numbers.py

stdlib only. Handles any input rate / mono or stereo / 16-bit PCM WAV.
"""
import math
import os
import struct
import sys
import wave

RATE = 16000  # must match Clicker::kSampleRate

# Any of these whose assets/<word>.wav exists gets compiled in; the generated
# table is sized to the largest one present.
WORDS = {
    "one": 1, "two": 2, "three": 3, "four": 4, "five": 5, "six": 6,
    "seven": 7, "eight": 8, "nine": 9, "ten": 10, "eleven": 11, "twelve": 12,
    "thirteen": 13, "fourteen": 14, "fifteen": 15, "sixteen": 16,
    "seventeen": 17, "eighteen": 18, "nineteen": 19, "twenty": 20,
}

HERE = os.path.dirname(os.path.abspath(__file__))
ASSETS = os.path.join(HERE, "..", "assets")
OUT = os.path.join(HERE, "..", "src", "assets", "numbers_audio.c")


def read_wav_mono(path):
    with wave.open(path, "rb") as w:
        assert w.getsampwidth() == 2, f"{path}: expected 16-bit PCM"
        ch, sr, n = w.getnchannels(), w.getframerate(), w.getnframes()
        raw = w.readframes(n)
    s = struct.unpack("<%dh" % (len(raw) // 2), raw)
    if ch == 2:
        s = [(s[i] + s[i + 1]) // 2 for i in range(0, len(s), 2)]
    return list(s), sr


def resample(s, src_rate, dst_rate):
    if src_rate == dst_rate:
        return s
    ratio = src_rate / dst_rate
    if ratio > 1.0:  # downsampling - cheap box pre-filter to tame aliasing
        k = int(math.ceil(ratio))
        s = [sum(s[max(0, i - k + 1):i + 1]) / min(k, i + 1) for i in range(len(s))]
    out = []
    n_out = int(len(s) / ratio)
    for i in range(n_out):
        x = i * ratio
        i0 = int(x)
        i1 = min(i0 + 1, len(s) - 1)
        f = x - i0
        out.append(s[i0] * (1 - f) + s[i1] * f)
    return out


def trim(s, pad_ms=6):
    peak = max((abs(v) for v in s), default=1)
    thr = max(150, peak * 0.02)
    lo, hi = 0, len(s)
    while lo < len(s) and abs(s[lo]) < thr:
        lo += 1
    while hi > lo and abs(s[hi - 1]) < thr:
        hi -= 1
    pad = int(RATE * pad_ms / 1000)
    return s[max(0, lo - pad):min(len(s), hi + pad)]


def normalize(s, target=0.80):  # headroom - the codec + amp path clips near full-scale
    peak = max((abs(v) for v in s), default=1)
    g = (target * 32767.0) / peak if peak else 1.0
    return [max(-32768, min(32767, int(round(v * g)))) for v in s]


def fade(s, ms=5):
    n = min(int(RATE * ms / 1000), len(s) // 2)
    for i in range(n):
        e = 0.5 - 0.5 * math.cos(math.pi * i / n)
        s[i] = int(s[i] * e)
        s[-1 - i] = int(s[-1 - i] * e)
    return s


def main():
    samples = {}
    for word, num in WORDS.items():
        path = os.path.join(ASSETS, f"{word}.wav")
        if not os.path.exists(path):
            print(f"  (missing {word}.wav - skipping)", file=sys.stderr)
            continue
        s, sr = read_wav_mono(path)
        s = fade(normalize(trim(resample(s, sr, RATE))))
        samples[num] = s
        print(f"  {word}: {len(s)} frames ({len(s) / RATE * 1000:.0f} ms, {len(s) * 2} B)")

    total = sum(len(s) for s in samples.values()) * 2
    count = (max(samples) + 1) if samples else 1  # valid indices 0 .. count-1
    lines = ['#include "numbers_audio.h"', ""]
    for num, s in sorted(samples.items()):
        lines.append(f"static const int16_t num{num}_pcm[] = {{")
        for i in range(0, len(s), 16):
            lines.append("    " + ",".join(str(v) for v in s[i:i + 16]) + ",")
        lines.append("};")
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
