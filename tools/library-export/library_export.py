#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""
library-export: read tags from a music folder into a tab-separated file for
tools/shuffle-sim. Only tags are read; file paths are NOT written to the output.

    python3 tools/library-export/library_export.py /path/to/Music [-o OUT.library.tsv]

Columns: id, artist, album_artist, album, title, genre, disc, track, duration_ms
The default output (library-export/<folder>.library.tsv) is git-ignored.
Requires: mutagen (pip install -r tools/library-export/requirements.txt)
"""
import argparse
import os
import sys

try:
    import mutagen
except ImportError:
    sys.exit("mutagen is missing: pip install -r tools/library-export/requirements.txt")

AUDIO_EXT = {".flac", ".mp3", ".m4a", ".mp4", ".aac", ".alac", ".ogg", ".oga", ".opus",
             ".wav", ".aif", ".aiff", ".wv", ".ape", ".dsf", ".dff", ".wma"}

# Tag names per container family: easy names first, then raw ID3 / MP4 / APE keys.
KEYS = {
    "artist":       ["artist", "TPE1", "\xa9ART", "Artist"],
    "album_artist": ["albumartist", "album artist", "TPE2", "aART", "Album Artist", "ALBUMARTIST"],
    "album":        ["album", "TALB", "\xa9alb", "Album"],
    "title":        ["title", "TIT2", "\xa9nam", "Title"],
    "genre":        ["genre", "TCON", "\xa9gen", "Genre"],
    "disc":         ["discnumber", "TPOS", "disk", "Disc"],
    "track":        ["tracknumber", "TRCK", "trkn", "Track"],
}


def first_value(tags, names):
    if tags is None:
        return ""
    for name in names:
        try:
            v = tags.get(name)
        except Exception:  # some tag classes raise on odd keys
            v = None
        if v is None:
            continue
        if isinstance(v, (list, tuple)):
            if not v:
                continue
            v = v[0]
        if hasattr(v, "text"):          # ID3 frame
            v = v.text[0] if v.text else ""
        if isinstance(v, tuple):        # MP4 trkn/disk = (n, total)
            v = v[0]
        return str(v)
    return ""


def number(s):
    s = (s or "").split("/")[0].strip()
    return int(s) if s.isdigit() else 0


def clean(s):
    return " ".join(str(s).replace("\t", " ").split())


def read_file(path):
    try:
        f = mutagen.File(path, easy=True)
        if f is None:
            return None
        tags = f.tags
        if not first_value(tags, KEYS["title"]):
            raw = mutagen.File(path)        # containers without an "easy" mapping (DSF, WAV, APE)
            if raw is not None and raw.tags is not None:
                tags = raw.tags
        length = getattr(getattr(f, "info", None), "length", 0) or 0
    except Exception as e:  # unreadable files are skipped, not fatal
        print(f"skip ({type(e).__name__}): {os.path.basename(path)}", file=sys.stderr)
        return None
    row = {k: clean(first_value(tags, names)) for k, names in KEYS.items()}
    if not row["title"]:
        row["title"] = clean(os.path.splitext(os.path.basename(path))[0])
    row["disc"] = number(row["disc"])
    row["track"] = number(row["track"])
    row["duration_ms"] = int(length * 1000)
    return row


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("folder")
    ap.add_argument("-o", "--output")
    args = ap.parse_args()

    root = os.path.abspath(args.folder)
    if not os.path.isdir(root):
        sys.exit(f"not a folder: {root}")
    out = args.output
    if not out:
        repo = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
        os.makedirs(os.path.join(repo, "library-export"), exist_ok=True)
        out = os.path.join(repo, "library-export", f"{os.path.basename(root) or 'music'}.library.tsv")

    out_dir = os.path.dirname(os.path.abspath(out))
    if os.path.commonpath([out_dir, root]) == root:
        sys.exit("refusing to write inside the music folder; choose another output path")
    os.makedirs(out_dir, exist_ok=True)

    files = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = sorted(d for d in dirnames if not d.startswith("."))
        for name in sorted(filenames):
            if not name.startswith(".") and os.path.splitext(name)[1].lower() in AUDIO_EXT:
                files.append(os.path.join(dirpath, name))

    rows = 0
    with open(out, "w", encoding="utf-8") as fp:
        fp.write("id\tartist\talbum_artist\talbum\ttitle\tgenre\tdisc\ttrack\tduration_ms\n")
        for i, path in enumerate(files, 1):
            r = read_file(path)
            if r is None:
                continue
            rows += 1
            fp.write(f"{rows}\t{r['artist']}\t{r['album_artist']}\t{r['album']}\t{r['title']}\t"
                     f"{r['genre']}\t{r['disc']}\t{r['track']}\t{r['duration_ms']}\n")
            if i % 500 == 0:
                print(f"{i}/{len(files)} files", file=sys.stderr)
    print(f"{rows} tracks written to {out}")


if __name__ == "__main__":
    main()
