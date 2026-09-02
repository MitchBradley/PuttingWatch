#!/usr/bin/env python3
"""Convert the named voice-cue WAVs in assets/ into src/assets/cues_audio.c -
one-shot spoken cues played by Clicker::playCue().

Same conditioning as the numbers (tools/wav2c_common.py). Run after
(re)generating a cue WAV:

    python3 tools/wav2c_cues.py

stdlib only.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from wav2c_common import RATE, emit_pcm_array, load_sample

# cue name -> (assets/<wav>, CueId enum symbol in lib/Clicker/cues_audio.h).
# Keep the enum symbols in sync with that header.
CUES = {
    "ready": ("ready.wav", "CUE_READY"),
}

HERE = os.path.dirname(os.path.abspath(__file__))
ASSETS = os.path.join(HERE, "..", "assets")
OUT = os.path.join(HERE, "..", "src", "assets", "cues_audio.c")


def main():
    got = {}  # name -> (enum_symbol, samples)
    for name, (wav, sym) in CUES.items():
        path = os.path.join(ASSETS, wav)
        if not os.path.exists(path):
            print(f"  (missing {wav} - skipping)", file=sys.stderr)
            continue
        s = load_sample(path)
        got[name] = (sym, s)
        print(f"  {name}: {len(s)} frames ({len(s) / RATE * 1000:.0f} ms, {len(s) * 2} B)")

    total = sum(len(s) for _, s in got.values()) * 2
    lines = ['#include "cues_audio.h"', ""]
    for name, (_, s) in got.items():
        lines += emit_pcm_array(f"{name}_pcm", s)
        lines.append("")
    lines.append("const CueSample kCues[CUE_COUNT] = {")
    if got:
        for name, (sym, _) in got.items():
            lines.append(f"    [{sym}] = {{ {name}_pcm, (int)(sizeof({name}_pcm) / sizeof(int16_t)) }},")
    else:
        lines.append("    { 0, 0 },  // no cue WAVs present")
    lines.append("};")
    with open(OUT, "w") as f:
        f.write("\n".join(lines) + "\n")
    print(f"wrote {OUT}  ({total} B / {total / 1024:.0f} KB of PCM)")


if __name__ == "__main__":
    main()
