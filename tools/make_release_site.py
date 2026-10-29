#!/usr/bin/env python3
"""Assemble the GitHub Pages site for one firmware release.

Run by .github/workflows/release.yml; stdlib only, so it can also be run by
hand to preview the site:

    python3 tools/make_release_site.py --version 0.1.0 \\
        --base-url http://localhost:8000/ \\
        --firmware .pio/build/esp32c6/firmware.bin \\
        --factory  .pio/build/esp32c6/firmware.factory.bin \\
        --template release/index.template.html --out site

Output (the site holds only the newest release; older ones live on as GitHub
Release assets):

    manifest.json              what the watch reads to check for / fetch an update
    webtools-manifest.json     what the USB install page (ESP Web Tools) reads
    index.html                 the USB install page
    firmware/<ver>/firmware.bin          app image, flashed over the air
    firmware/<ver>/firmware.factory.bin  bootloader + partitions + app, flashed
                                         over USB at offset 0
    guide/, screens/           the user guide (docs/guide) and the screenshots it
                               shows (docs/screens), copied as they are

manifest.json is a contract with firmware already in the field - only ever add
optional fields to it, never rename or repurpose one. "schema" is there so a
future incompatible change can be detected rather than misread.
"""
import argparse
import datetime
import hashlib
import html
import json
import os
import re
import shutil
import sys

VERSION_RE = re.compile(r"^\d+\.\d+\.\d+(-[0-9A-Za-z.]+)?$")


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()


def write_json(path, obj):
    with open(path, "w") as f:
        json.dump(obj, f, indent=2)
        f.write("\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--version", required=True, help="e.g. 0.1.0 (no leading v)")
    ap.add_argument("--base-url", required=True, help="public site root, e.g. https://owner.github.io/Repo/")
    ap.add_argument("--firmware", required=True, help="app image for OTA")
    ap.add_argument("--factory", required=True, help="merged image for USB install")
    ap.add_argument("--template", required=True, help="install page template")
    ap.add_argument("--out", required=True, help="output directory (must not exist)")
    ap.add_argument("--min-version", default="0.0.0",
                    help="oldest installed version allowed to update over the air to this one")
    ap.add_argument("--notes", default="", help="one-line release summary")
    ap.add_argument("--repo-url", default="", help="GitHub repo URL, for the install page footer")
    a = ap.parse_args()

    for name, v in (("--version", a.version), ("--min-version", a.min_version)):
        if not VERSION_RE.match(v):
            sys.exit(f"error: {name} '{v}' is not MAJOR.MINOR.PATCH[-prerelease]")
    for p in (a.firmware, a.factory, a.template):
        if not os.path.isfile(p):
            sys.exit(f"error: {p} not found")
    if os.path.exists(a.out):
        sys.exit(f"error: {a.out} already exists")

    base = a.base_url if a.base_url.endswith("/") else a.base_url + "/"
    rel_dir = f"firmware/{a.version}"
    os.makedirs(os.path.join(a.out, rel_dir))
    shutil.copy(a.firmware, os.path.join(a.out, rel_dir, "firmware.bin"))
    shutil.copy(a.factory, os.path.join(a.out, rel_dir, "firmware.factory.bin"))

    released = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    notes = a.notes.strip()[:200]

    write_json(os.path.join(a.out, "manifest.json"), {
        "schema": 1,
        "version": a.version,
        "min_version": a.min_version,
        "released": released,
        "url": f"{base}{rel_dir}/firmware.bin",
        "size": os.path.getsize(a.firmware),
        "sha256": sha256_of(a.firmware),
        "notes": notes,
    })

    write_json(os.path.join(a.out, "webtools-manifest.json"), {
        "name": "Honu Watch",
        "version": a.version,
        "new_install_prompt_erase": True,
        "builds": [{
            "chipFamily": "ESP32-C6",
            "parts": [{"path": f"{rel_dir}/firmware.factory.bin", "offset": 0}],
        }],
    })

    with open(a.template) as f:
        page = f.read()
    notes_html = f'<p class="notes">{html.escape(notes)}</p>' if notes else ""
    for key, val in (("{{VERSION}}", html.escape(a.version)),
                     ("{{RELEASED}}", released[:10]),
                     ("{{NOTES_HTML}}", notes_html),
                     ("{{REPO_URL}}", html.escape(a.repo_url, quote=True))):
        page = page.replace(key, val)
    with open(os.path.join(a.out, "index.html"), "w") as f:
        f.write(page)

    # The user guide and its screenshots are published alongside, at the same
    # relative positions they have in the repo, so the guide's ../screens/ image
    # paths work unchanged both when browsing the repo and on the site.
    repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    for name in ("guide", "screens"):
        src = os.path.join(repo, "docs", name)
        if os.path.isdir(src):
            shutil.copytree(src, os.path.join(a.out, name))

    print(f"site for {a.version} written to {a.out}")


if __name__ == "__main__":
    main()
