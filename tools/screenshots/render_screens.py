#!/usr/bin/env python3
"""Render every PuttingWatch screen to PNG, for the user guide.

    python3 tools/screenshots/render_screens.py [--out docs/screens] [--montage FILE]

This compiles the real src/ui.cpp and src/update_ui.cpp, plus LVGL itself, as a
desktop program (tools/screenshots/screens.cpp) with a software-rendered display
and fake clock/battery/update engine, then converts what it draws to PNGs. The
captures therefore match the watch pixel for pixel - same fonts, layout, colours
and QR codes - without needing the hardware. Re-run it after any layout change.

Needs LVGL's sources, which PlatformIO has already fetched into
.pio/libdeps/esp32c6/lvgl once the firmware has been built (`pio run -e esp32c6`).
The LVGL objects are cached in .pio/screenshots/, so only the first run is slow.

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


def find_lvgl():
    for env in ("esp32c6", "esp32c6-ldf", "esp32s3"):
        p = ROOT / ".pio" / "libdeps" / env / "lvgl"
        if (p / "lvgl.h").exists():
            return p
    sys.exit("LVGL not found under .pio/libdeps - run `pio run -e esp32c6` once first.")


def host_lv_conf():
    """The firmware's lv_conf.h, except it uses the C library's malloc: the
    firmware's builtin pool is sized for the watch, not for a full-frame desktop render."""
    src = (ROOT / "include" / "lv_conf.h").read_text()
    needle = "#define LV_USE_STDLIB_MALLOC    LV_STDLIB_BUILTIN"
    if needle not in src:
        sys.exit("include/lv_conf.h changed shape: update host_lv_conf() in render_screens.py")
    out = BUILD / "hostconf"
    out.mkdir(parents=True, exist_ok=True)
    conf = out / "lv_conf.h"
    new = src.replace(needle, "#define LV_USE_STDLIB_MALLOC    LV_STDLIB_CLIB")
    if not conf.exists() or conf.read_text() != new:
        conf.write_text(new)
    return out, conf


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.stderr.write(f"$ {' '.join(map(str, cmd))}\n{r.stdout}{r.stderr}")
        raise SystemExit(f"command failed ({r.returncode})")


def compile_all(jobs, force_after):
    """jobs: (compiler, flags, source, object). Skips fresh objects."""
    todo = []
    for cc, flags, src, obj in jobs:
        if (not obj.exists()) or obj.stat().st_mtime < max(src.stat().st_mtime, force_after):
            todo.append((cc, flags, src, obj))
    if not todo:
        return
    print(f"compiling {len(todo)} file(s)...", flush=True)
    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
        list(pool.map(lambda j: run([j[0], *j[1], "-c", str(j[2]), "-o", str(j[3])]), todo))


def build():
    lvgl = find_lvgl()
    hostconf, conf_file = host_lv_conf()
    conf_mtime = conf_file.stat().st_mtime
    obj_lv, obj_app = BUILD / "obj_lvgl", BUILD / "obj_app"
    obj_lv.mkdir(parents=True, exist_ok=True)
    obj_app.mkdir(parents=True, exist_ok=True)

    inc_lvgl = ["-DLV_CONF_INCLUDE_SIMPLE", f"-I{hostconf}", f"-I{lvgl}", f"-I{lvgl}/src"]
    cflags = ["-O1", "-w", "-std=gnu11", *inc_lvgl]
    # The app's own sources: the shim directory FIRST so <Arduino.h> etc. resolve
    # to the host stand-ins, and the host lv_conf.h before include/'s.
    inc_app = [
        "-DLV_CONF_INCLUDE_SIMPLE", f"-I{HERE / 'shim'}", f"-I{hostconf}", f"-I{lvgl}", f"-I{lvgl}/src",
        f"-I{ROOT / 'include'}", f"-I{ROOT / 'src'}", f"-I{ROOT / 'src' / 'assets'}",
        f"-I{ROOT / 'lib' / 'PuttingCore'}", f"-I{ROOT / 'lib' / 'UpdateCore'}",
        f"-I{ROOT / 'lib' / 'Battery' / 'src'}",
    ]
    cxxflags = ["-O1", "-std=gnu++17", "-Wno-deprecated-declarations", *inc_app]
    c_app_flags = ["-O1", "-w", "-std=gnu11", *inc_app]

    jobs = []
    for src in sorted((lvgl / "src").rglob("*.c")):
        obj = obj_lv / (str(src.relative_to(lvgl / "src")).replace(os.sep, "_") + ".o")
        jobs.append((CC, cflags, src, obj))
    app = [
        (CXX, cxxflags, HERE / "screens.cpp"),
        (CXX, cxxflags, ROOT / "src" / "ui.cpp"),
        (CXX, cxxflags, ROOT / "src" / "update_ui.cpp"),
        (CXX, cxxflags, ROOT / "lib" / "UpdateCore" / "UpdateCore.cpp"),
        (CC, c_app_flags, ROOT / "src" / "assets" / "Honu.c"),
        (CC, c_app_flags, ROOT / "src" / "assets" / "HonuBottom.c"),
    ]
    app_objs = []
    for cc, flags, src in app:
        obj = obj_app / (src.name + ".o")
        app_objs.append(obj)
        jobs.append((cc, flags, src, obj))

    # Anything the app sources include may have changed too: rebuild the app
    # objects whenever any header/source they depend on is newer (cheap: 6 files).
    newest_dep = max(p.stat().st_mtime for p in [
        *(ROOT / "src").glob("*.h"), *(ROOT / "include").glob("*.h"),
        *(ROOT / "lib" / "UpdateCore").glob("*.h"), *(HERE / "shim").glob("*.h"), HERE / "screens.cpp",
    ])
    lv_jobs = [j for j in jobs if j[3].parent == obj_lv]
    app_jobs = [j for j in jobs if j[3].parent == obj_app]
    compile_all(lv_jobs, conf_mtime)
    compile_all(app_jobs, max(conf_mtime, newest_dep))

    exe = BUILD / "screens"
    all_objs = sorted(obj_lv.glob("*.o")) + app_objs
    if not exe.exists() or exe.stat().st_mtime < max(o.stat().st_mtime for o in all_objs):
        print("linking...", flush=True)
        run([CXX, "-o", str(exe), *map(str, all_objs), "-lm"])
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
