#!/usr/bin/env python3
"""Extract the pixel data from a GIMP "Export as C-Source" image (as used by the C++ firmware's
src/assets/Honu.c and HonuBottom.c) into a raw RGB565 file: width*height little-endian u16,
exactly the panel's byte order after the CO5300 driver's big-endian swap is applied at blit time
(the data itself is stored as GIMP wrote it: LE).

    python3 tools/extract_c_image.py <image.c> <out.rgb565>

stdlib only.
"""
import re
import sys

SIMPLE = {"n": 10, "t": 9, "r": 13, "a": 7, "b": 8, "f": 12, "v": 11, "\\": 92, '"': 34, "'": 39, "?": 63}


def unescape(lit: str) -> bytes:
    out = bytearray()
    i = 0
    while i < len(lit):
        c = lit[i]
        if c != "\\":
            out.append(ord(c))
            i += 1
            continue
        i += 1
        c = lit[i]
        if c in "01234567":  # up to 3 octal digits
            j = i
            while j < len(lit) and j < i + 3 and lit[j] in "01234567":
                j += 1
            out.append(int(lit[i:j], 8) & 0xFF)
            i = j
        elif c == "x":
            j = i + 1
            while j < len(lit) and lit[j] in "0123456789abcdefABCDEF":
                j += 1
            out.append(int(lit[i + 1:j], 16) & 0xFF)
            i = j
        else:
            out.append(SIMPLE[c])
            i += 1
    return bytes(out)


def main():
    src, dst = sys.argv[1], sys.argv[2]
    text = open(src, encoding="latin-1").read()
    m = re.search(r"gimp_image\s*=\s*\{\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,", text)
    w, h, bpp = (int(x) for x in m.groups())
    assert bpp == 2, f"expected 16-bit pixels, got {bpp} bytes/pixel"
    body = text[m.end():text.index("};", m.end())]
    lits = re.findall(r'"((?:[^"\\]|\\.)*)"', body)
    data = b"".join(unescape(l) for l in lits[1:])  # lits[0] is the comment string
    data = data[: w * h * 2]  # drop the array's trailing NUL
    assert len(data) == w * h * 2, f"got {len(data)} bytes, expected {w * h * 2}"
    open(dst, "wb").write(data)
    print(f"{src}: {w}x{h} -> {dst} ({len(data)} bytes)")


if __name__ == "__main__":
    main()
