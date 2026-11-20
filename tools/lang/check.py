#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""
Find the UI's words that stay in English with a full translation.

    python3 tools/lang/check.py [build/sim/fathom-sim]

Makes a language translating every text tools/lang/strings.py lists as "~text",
walks the simulator's menus with it (fathom-sim --lang-check, on a copy of
build/sim-data) and prints each text shown without the mark, with where it was.
Texts that are the library's own (titles, names, folders, playlists, as in
build/sim-data/library.db, cut short or not) are left out; station names and
numbers have no mark either: those are expected.
"""
import os
import re
import shutil
import sqlite3
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def library_texts(db):
    """Every name and title in the library."""
    names = set()
    if not os.path.exists(db):
        return names
    con = sqlite3.connect('file:%s?mode=ro' % db, uri=True)
    for sql in ('SELECT title, artist, album_artist, album, genre, composer, year FROM tracks',
                'SELECT name FROM artists', 'SELECT title FROM albums', 'SELECT name FROM genres',
                'SELECT name FROM composers', 'SELECT name FROM folders', 'SELECT name FROM playlists'):
        try:
            for row in con.execute(sql):
                for v in row:
                    if v is not None:
                        names.add(str(v).strip())
        except sqlite3.Error:
            pass
    con.close()
    return names


def is_music(text, music):
    t = re.sub(r'^\u2039 ', '', text)                 # a header: "‹ Name"
    parts = t.split(' \u00b7 ')                       # "Title · Artist"
    for i, p in enumerate(parts):
        p = re.sub(r'(\.\.\.|\u2026)$', '', p).strip()   # cut short
        if p in music or (i == len(parts) - 1 and len(p) >= 3 and any(m.startswith(p) for m in music)):
            continue
        return False
    return True


def main():
    sim = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'build', 'sim', 'fathom-sim'))
    texts = subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'lang', 'strings.py')],
                           check=True, capture_output=True, text=True).stdout
    lines = []
    for line in texts.splitlines():
        if line.startswith('#') or ' = ' not in line:
            continue
        en = line.rstrip()[:-2] if line.rstrip().endswith(' =') else line.split(' = ')[0]
        lines.append('%s = ~%s' % (en, en))
    with tempfile.TemporaryDirectory() as tmp:
        data = os.path.join(ROOT, 'build', 'sim-data')
        if os.path.isdir(data):   # (the walk may change the library: never the real one)
            shutil.copytree(data, os.path.join(tmp, 'build', 'sim-data'))
        else:
            os.makedirs(os.path.join(tmp, 'build', 'sim-data'))
        lang = os.path.join(tmp, 'check.txt')
        with open(lang, 'w', encoding='utf-8') as f:
            f.write('\n'.join(lines) + '\n')
        out = subprocess.run([sim, '--lang-check', lang], cwd=tmp, capture_output=True,
                             text=True, errors='replace', timeout=600)
        if out.returncode:
            sys.stderr.write(out.stderr)
            return out.returncode
    music = library_texts(os.path.join(ROOT, 'build', 'sim-data', 'library.db'))
    seen = {}
    for line in out.stdout.splitlines():
        where, _, text = line.partition('\t')
        if '~' in text or not any(c.isalpha() for c in text) or is_music(text, music):
            continue
        seen.setdefault(text, where)
    for text, where in sorted(seen.items(), key=lambda kv: kv[1]):
        print('%-50s %s' % (where[:50], text.replace('\n', '\\n')))
    return 0


if __name__ == '__main__':
    sys.exit(main())
