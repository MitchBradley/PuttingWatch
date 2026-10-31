#!/usr/bin/env python3
"""Render every Honu Watch screen to PNG, for the user guide.

    python3 tools/screenshots/render_screens.py [--out docs/screens] [--montage FILE]

This compiles the real src/ui.cpp and src/update_ui.cpp, plus MiniUi and the
Arduino_GFX text/graphics code, as a desktop program
(tools/screenshots/screens.cpp) that draws into a RAM frame buffer with a fake
clock/battery/update engine, then converts what it draws to PNGs. The captures
therefore match the watch pixel for pixel - same fonts, layout, colours and QR
codes - without needing the hardware. Re-run it after any layout change.

If linking fails with "unknown architecture arm64e.x1-macos", that is a macOS
Command Line Tools / Xcode SDK mismatch on the machine, not this script; point
SDKROOT at an older SDK, e.g.
    SDKROOT=/Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk python3 ...

Standard library only.
"""
import argparse
import concurrent.futures
import os
import pathlib
import struct
import subprocess
import sys
import zlib

ROOT = pathlib.Path(__file__).resolve().parents[2]
HERE = pathlib.Path(__file__).resolve().parent
BUILD = ROOT / ".pio" / "screenshots"
WIDTH, HEIGHT = 410, 502

CC = os.environ.get("CC", "cc")
CXX = os.environ.get("CXX", "c++")


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.stderr.write(f"$ {' '.join(map(str, cmd))}\n{r.stdout}{r.stderr}")
        raise SystemExit(f"command failed ({r.returncode})")


def compile_all(jobs, newest_dep):
    """jobs: (compiler, flags, source, object). Skips fresh objects."""
    todo = [j for j in jobs if (not j[3].exists()) or j[3].stat().st_mtime < max(j[2].stat().st_mtime, newest_dep)]
    if not todo:
        return
    print(f"compiling {len(todo)} file(s)...", flush=True)
    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
        list(pool.map(lambda j: run([j[0], *j[1], "-c", str(j[2]), "-o", str(j[3])]), todo))


def build():
    obj = BUILD / "obj"
    obj.mkdir(parents=True, exist_ok=True)
    gfx = ROOT / "lib" / "Arduino_GFX" / "src"
    # The shim directory FIRST so <Arduino.h> etc. resolve to the host stand-ins.
    inc = [
        f"-I{HERE / 'shim'}", f"-I{gfx}", f"-I{ROOT / 'include'}", f"-I{ROOT / 'src'}",
        f"-I{ROOT / 'src' / 'assets'}", f"-I{ROOT / 'lib' / 'PuttingCore'}",
        f"-I{ROOT / 'lib' / 'UpdateCore'}", f"-I{ROOT / 'lib' / 'MiniUi'}",
        f"-I{ROOT / 'lib' / 'Qrcodegen'}", f"-I{ROOT / 'lib' / 'Battery' / 'src'}",
    ]
    cxxflags = ["-O1", "-std=gnu++17", "-w", *inc]
    cflags = ["-O1", "-w", "-std=gnu11", *inc]
    sources = [
        (CXX, cxxflags, HERE / "screens.cpp"),
        (CXX, cxxflags, ROOT / "src" / "ui.cpp"),
        (CXX, cxxflags, ROOT / "src" / "update_ui.cpp"),
        (CXX, cxxflags, ROOT / "lib" / "UpdateCore" / "UpdateCore.cpp"),
        (CXX, cxxflags, ROOT / "lib" / "MiniUi" / "MiniUi.cpp"),
        (CXX, cxxflags, gfx / "Arduino_GFX.cpp"),
        (CXX, cxxflags, gfx / "Arduino_G.cpp"),
        (CC, cflags, ROOT / "lib" / "Qrcodegen" / "qrcodegen.c"),
        (CC, cflags, ROOT / "src" / "assets" / "Honu.c"),
        (CC, cflags, ROOT / "src" / "assets" / "HonuBottom.c"),
    ]
    jobs, objs = [], []
    for cc, flags, src in sources:
        o = obj / (src.name + ".o")
        objs.append(o)
        jobs.append((cc, flags, src, o))

    # Anything the sources include may have changed too: rebuild everything
    # whenever any header they depend on is newer (the file count is small).
    newest_dep = max(p.stat().st_mtime for p in [
        *(ROOT / "src").glob("*.h"), *(ROOT / "src" / "assets").glob("*.h"),
        *(ROOT / "include").glob("*.h"), *(ROOT / "lib" / "UpdateCore").glob("*.h"),
        *(ROOT / "lib" / "MiniUi").glob("*.h"), *(HERE / "shim").glob("*.h"),
        gfx / "Arduino_GFX.h", HERE / "screens.cpp",
    ])
    compile_all(jobs, newest_dep)

    exe = BUILD / "screens"
    if not exe.exists() or exe.stat().st_mtime < max(o.stat().st_mtime for o in objs):
        print("linking...", flush=True)
        run([CXX, "-o", str(exe), *map(str, objs), "-lm"])
    return exe


def write_png(path, width, height, rgb):
    raw = b"".join(b"\x00" + rgb[y * width * 3:(y + 1) * width * 3] for y in range(height))

    def chunk(tag, data):
        body = tag + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(raw, 9))
           + chunk(b"IEND", b""))
    pathlib.Path(path).write_bytes(png)


def montage(frames, columns=6, shrink=3):
    """A contact sheet for eyeballing every screen at once (nearest-neighbour, 1/shrink)."""
    w, h = WIDTH // shrink, HEIGHT // shrink
    pad = 6
    rows = (len(frames) + columns - 1) // columns
    sheet_w, sheet_h = columns * (w + pad) + pad, rows * (h + pad) + pad
    sheet = bytearray(b"\x40\x40\x40" * (sheet_w * sheet_h))
    for i, rgb in enumerate(frames):
        ox, oy = pad + (i % columns) * (w + pad), pad + (i // columns) * (h + pad)
        for y in range(h):
            row = bytearray()
            for x in range(w):
                o = ((y * shrink) * WIDTH + x * shrink) * 3
                row += rgb[o:o + 3]
            d = ((oy + y) * sheet_w + ox) * 3
            sheet[d:d + len(row)] = row
    return sheet_w, sheet_h, bytes(sheet)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--out", default=str(ROOT / "docs" / "screens"), help="directory for the PNGs")
    ap.add_argument("--montage", help="also write a contact sheet of all screens to this PNG")
    args = ap.parse_args()

    exe = build()
    raw_dir = BUILD / "raw"
    raw_dir.mkdir(parents=True, exist_ok=True)
    for old in raw_dir.glob("*.rgb"):
        old.unlink()
    run([str(exe), str(raw_dir)])

    out = pathlib.Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    frames = []
    for rgb_file in sorted(raw_dir.glob("*.rgb")):
        rgb = rgb_file.read_bytes()
        assert len(rgb) == WIDTH * HEIGHT * 3, rgb_file
        write_png(out / (rgb_file.stem + ".png"), WIDTH, HEIGHT, rgb)
        frames.append(rgb)
    print(f"wrote {len(frames)} PNGs to {out}")
    if args.montage:
        w, h, sheet = montage(frames)
        write_png(args.montage, w, h, sheet)
        print(f"wrote contact sheet {args.montage}")


if __name__ == "__main__":
    main()
