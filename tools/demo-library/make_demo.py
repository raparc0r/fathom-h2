#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional terms under GPLv3 section 7: see LICENSE.
"""A made-up music library, for screenshots and demos with nothing real in them.

    python3 tools/demo-library/make_demo.py DIR

Writes DIR/<Artist>/<Album>/NN - Title.flac: digital silence of a realistic
length (a few KB each), tagged, with an abstract cover drawn here and embedded
like a real one, plus two playlists in DIR/playlists. Every artist, album and
title is invented. Needs flac and metaflac.
"""
import math
import os
import struct
import subprocess
import sys
import zlib

ALBUMS = [
    ("Glass Harbour", "Low Tide Hymns", 2019, "Ambient", (0x1D, 0x4E, 0x6B), (0x7F, 0xD1, 0xC4), "circles",
     ["Shoreline", "Salt in the Gears", "Lantern Weather", "Undertow", "Harbour Lights", "Still Water"]),
    ("The Paper Moths", "Overgrown", 2022, "Indie", (0x5B, 0x2A, 0x86), (0xF2, 0xA6, 0x5A), "stripes",
     ["Moss on the Radio", "Overgrown", "Ivy League", "Paper Lungs", "Greenhouse Static"]),
    ("Mina Okafor", "Slow Weather", 2021, "Soul", (0x8C, 0x2F, 0x39), (0xFF, 0xD1, 0xA8), "sun",
     ["Slow Weather", "Copper Sky", "Rain on Tin", "Hold the Light", "Afterglow", "Small Hours", "Monsoon"]),
    ("Kestrel Vale", "Northern Lines", 2018, "Electronic", (0x10, 0x1C, 0x3A), (0x9F, 0xA8, 0xFF), "grid",
     ["Northern Lines", "Signal Fires", "Polar Drift", "Kilometre Zero", "Aurora Loop"]),
    ("Ruth & the Vessels", "Night Ferry", 2020, "Folk", (0x23, 0x3D, 0x2E), (0xE8, 0xD9, 0xA0), "waves",
     ["Night Ferry", "The Long Crossing", "Rope and Anchor", "Gulls", "Landfall", "Lighthouse Keeper"]),
    ("Odile Brun", "Cartographies", 2023, "Jazz", (0x3A, 0x2A, 0x1A), (0xF4, 0xB9, 0x42), "rings",
     ["Cartographies", "Contour", "Legend", "Scale 1:50 000", "Compass Rose"]),
]
LENGTHS = [214, 187, 263, 241, 302, 176, 229, 338, 195, 256]   # seconds, in turn


def png(w, h, pixel):
    """A PNG from pixel(x, y) -> (r, g, b)."""
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        for x in range(w):
            raw.extend(pixel(x, y))
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b""))


def cover(dark, light, motif, size=300):
    def mix(a, b, t):
        t = max(0.0, min(1.0, t))
        return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))
    c = size / 2

    def pixel(x, y):
        base = mix(dark, tuple(min(255, v + 30) for v in dark), y / size)
        dx, dy = x - c, y - c
        r = math.hypot(dx, dy)
        on = False
        if motif == "circles":
            on = any(abs(math.hypot(x - cx, y - cy) - rr) < 3 for cx, cy, rr in
                     ((110, 120, 70), (190, 170, 50), (150, 210, 90)))
        elif motif == "stripes":
            on = (x + y) % 40 < 14 and y > size * 0.45
        elif motif == "sun":
            on = r < 70 or (r > 90 and r < 96)
            if y > size * 0.68:
                on = int(y) % 12 < 4
        elif motif == "grid":
            on = (x % 30 < 2 or y % 30 < 2) and r < 120
            if abs(dy - dx * 0.3) < 2:
                on = True
        elif motif == "waves":
            on = abs(math.sin(x / 22.0 + (y // 30)) * 8 - (y % 30) + 15) < 2.5 and y > 80
        elif motif == "rings":
            on = int(r) % 18 < 5 and r < 130
        return mix(base, light, 0.85) if on else base
    return png(size, size, pixel)


def flac(path, seconds, tags, picture):
    samples = seconds * 44100 * 4
    with subprocess.Popen(["flac", "--silent", "--force", "--force-raw-format", "--endian=little",
                           "--sign=signed", "--channels=2", "--bps=16", "--sample-rate=44100",
                           "-o", path, "-"], stdin=subprocess.PIPE) as p:
        zero = bytes(1 << 20)
        while samples > 0:
            n = min(samples, len(zero))
            p.stdin.write(zero[:n])
            samples -= n
        p.stdin.close()
        if p.wait():
            sys.exit("flac failed for " + path)
    args = ["metaflac", "--remove-all-tags"] + ["--set-tag=%s=%s" % kv for kv in tags]
    args += ["--import-picture-from=3|image/png|||" + picture, path]
    subprocess.run(args, check=True)


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__.strip().splitlines()[2].strip())
    root = sys.argv[1]
    os.makedirs(os.path.join(root, "playlists"), exist_ok=True)
    n = 0
    all_tracks = []
    for artist, album, year, genre, dark, light, motif, titles in ALBUMS:
        d = os.path.join(root, artist, album)
        os.makedirs(d, exist_ok=True)
        art = os.path.join(d, "cover.png")
        with open(art, "wb") as f:
            f.write(cover(dark, light, motif))
        for i, title in enumerate(titles, 1):
            name = "%02d - %s.flac" % (i, title)
            flac(os.path.join(d, name), LENGTHS[n % len(LENGTHS)],
                 [("ARTIST", artist), ("ALBUMARTIST", artist), ("ALBUM", album), ("TITLE", title),
                  ("TRACKNUMBER", str(i)), ("DATE", str(year)), ("GENRE", genre)], art)
            all_tracks.append(os.path.join("..", artist, album, name))
            n += 1
        os.remove(art)
    with open(os.path.join(root, "playlists", "Morning.m3u8"), "w") as f:
        f.write("#EXTM3U\n" + "\n".join(all_tracks[0::3]) + "\n")
    with open(os.path.join(root, "playlists", "Long drive.m3u8"), "w") as f:
        f.write("#EXTM3U\n" + "\n".join(all_tracks[1::2]) + "\n")
    print("%d tracks in %d albums under %s" % (n, len(ALBUMS), root))


if __name__ == "__main__":
    main()
