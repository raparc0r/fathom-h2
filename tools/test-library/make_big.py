#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional terms under GPLv3 section 7: see LICENSE.
"""A big made-up library, for testing Fathom at sizes far beyond the owner's.

    python3 tools/test-library/make_big.py DIR [SONGS]      # default 50000

Writes DIR/Music/<Album artist>/<Album>/NN - Title.flac and a cover.jpg per
album: about 11 songs an album, 3 albums an artist; some compilations (Various
Artists, each song its own artist), guests ("feat."), two-disc albums,
genres, years, names with accents. Each song is one second of silence whose
header gives it a realistic length (2 to 7 minutes), so the files are tiny but
the library looks real; a cover is a 600x600 JPEG, as a scan meets them.
Made the same each time (a fixed seed). Needs flac and sips (macOS).
"""
import os
import random
import struct
import subprocess
import sys
import tempfile
import zlib

RATE = 44100
SYL = ['ka', 'lo', 'mi', 'ra', 'ven', 'dor', 'el', 'sa', 'tu', 'ri', 'no', 'bel', 'cor', 'an', 'is', 'mar',
       'ta', 'ze', 'qui', 'lu', 'fen', 'gra', 'ho', 'ji', 'wy', 'ost', 'pa', 'ul', 'ver', 'sh', 'ña', 'ü', 'é']
WORDS = ['Night', 'River', 'Glass', 'Static', 'Harbour', 'Ember', 'Velvet', 'Signal', 'Copper', 'Winter',
         'Hollow', 'Paper', 'Echo', 'Tide', 'Lantern', 'North', 'Silver', 'Quiet', 'Garden', 'Machine',
         'Orbit', 'Salt', 'Thunder', 'Cedar', 'Mirror', 'Golden', 'Satellite', 'Rain', 'Desert', 'Ghost',
         'Fever', 'Island', 'Neon', 'Ocean', 'Smoke', 'Wire', 'Honey', 'Iron', 'Stone', 'Light',
         'Été', 'Mañana', 'Über', 'Café', 'Señal', 'Nuit', 'Fjörd', 'Sólo']
GENRES = ['Rock', 'Pop', 'Jazz', 'Electronic', 'Folk', 'Hip-Hop', 'Classical', 'Soul', 'Ambient', 'Metal',
          'Blues', 'Country', 'Reggae', 'Latin', 'Indie', 'Funk', 'Soundtrack', 'World', 'Punk', 'R&B']


def name(r, lo, hi):
    return ''.join(r.choice(SYL) for _ in range(r.randint(lo, hi))).capitalize()


def artist(r):
    k = r.random()
    if k < 0.35:
        return name(r, 2, 3) + ' ' + name(r, 2, 4)
    if k < 0.7:
        return 'The ' + r.choice(WORDS) + ' ' + r.choice(WORDS) + 's'
    return r.choice(WORDS) + ' ' + name(r, 2, 3)


def title(r):
    n = r.randint(1, 4)
    return ' '.join(r.choice(WORDS) for _ in range(n))


def safe(s):
    return ''.join('_' if c in '/\\:*?"<>|' else c for c in s).strip(' .') or '_'


def base_frames(tmp):
    """One second of stereo 16-bit silence as FLAC: its STREAMINFO and frames."""
    raw = os.path.join(tmp, 'silence.raw')
    with open(raw, 'wb') as f:
        f.write(b'\0' * RATE * 4)
    out = os.path.join(tmp, 'silence.flac')
    subprocess.run(['flac', '-s', '-f', '--force-raw-format', '--endian=little', '--sign=signed', '--channels=2',
                    '--bps=16', '--sample-rate=%d' % RATE, '--no-padding', '--no-seektable', '-o', out, raw], check=True)
    d = open(out, 'rb').read()
    assert d[:4] == b'fLaC'
    i, si = 4, None
    while True:
        hdr = d[i]
        ln = int.from_bytes(d[i + 1:i + 4], 'big')
        if hdr & 0x7f == 0:
            si = bytearray(d[i + 4:i + 4 + ln])
        i += 4 + ln
        if hdr & 0x80:
            break
    return si, d[i:]


def streaminfo(si, seconds):
    s = bytearray(si)
    total = seconds * RATE
    # bytes 10..17: rate(20) ch-1(3) bps-1(5) total_samples(36); MD5 (18..33) zero: unknown
    v = int.from_bytes(s[10:18], 'big')
    v = (v & ~((1 << 36) - 1)) | total
    s[10:18] = v.to_bytes(8, 'big')
    s[18:34] = b'\0' * 16
    return bytes(s)


def vorbis(tags):
    vendor = b'fathom test library'
    b = struct.pack('<I', len(vendor)) + vendor + struct.pack('<I', len(tags))
    for k, v in tags:
        e = (k + '=' + v).encode('utf-8')
        b += struct.pack('<I', len(e)) + e
    return b


def flac(si, frames, seconds, tags):
    s = streaminfo(si, seconds)
    vc = vorbis(tags)
    return (b'fLaC' + bytes([0]) + len(s).to_bytes(3, 'big') + s +
            bytes([0x84]) + len(vc).to_bytes(3, 'big') + vc + frames)


def png(w, h, pixel):
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        for x in range(w):
            raw += bytes(pixel(x, y))
    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(bytes(raw), 6)) + chunk(b'IEND', b''))


def covers(tmp, n):
    """n different 600x600 JPEG covers: colour fields and noise, like a photo's size."""
    out = []
    for k in range(n):
        r = random.Random(1000 + k)
        a = [r.randrange(256) for _ in range(3)]
        b = [r.randrange(256) for _ in range(3)]
        cx, cy = r.randrange(600), r.randrange(600)
        def px(x, y):
            t = min(1.0, (((x - cx) ** 2 + (y - cy) ** 2) ** 0.5) / 500)
            n_ = (x * 7919 + y * 104729 + k * 31) % 23 - 11
            return [max(0, min(255, int(a[i] * (1 - t) + b[i] * t) + n_)) for i in range(3)]
        p = os.path.join(tmp, 'c%d.png' % k)
        open(p, 'wb').write(png(600, 600, px))
        j = os.path.join(tmp, 'c%d.jpg' % k)
        subprocess.run(['sips', '-s', 'format', 'jpeg', '-s', 'formatOptions', '85', p, '--out', j],
                       check=True, stdout=subprocess.DEVNULL)
        out.append(open(j, 'rb').read())
    return out


def main():
    root = sys.argv[1]
    want = int(sys.argv[2]) if len(sys.argv) > 2 else 50000
    r = random.Random(42)
    music = os.path.join(root, 'Music')
    os.makedirs(music, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        si, frames = base_frames(tmp)
        jpgs = covers(tmp, 40)
    songs = albums = 0
    artists = [artist(r) for _ in range(max(1, want // 33))]
    guests = [artist(r) for _ in range(300)]
    while songs < want:
        comp = r.random() < 0.08
        aa = 'Various Artists' if comp else r.choice(artists)
        album = title(r)
        year = str(r.randint(1960, 2025))
        genre = r.choice(GENRES)
        discs = 2 if r.random() < 0.06 else 1
        d = os.path.join(music, safe(aa), safe(album + ' (' + year + ')'))
        if os.path.exists(d):
            continue
        os.makedirs(d)
        open(os.path.join(d, 'cover.jpg'), 'wb').write(jpgs[albums % len(jpgs)])
        albums += 1
        for disc in range(1, discs + 1):
            for n in range(1, r.randint(6, 16) + 1):
                if songs >= want:
                    break
                t = title(r)
                a = r.choice(artists) if comp else aa
                if not comp and r.random() < 0.05:
                    a += ' feat. ' + r.choice(guests)
                tags = [('TITLE', t), ('ARTIST', a), ('ALBUMARTIST', aa), ('ALBUM', album),
                        ('TRACKNUMBER', str(n)), ('DATE', year), ('GENRE', genre)]
                if discs > 1:
                    tags.append(('DISCNUMBER', str(disc)))
                fn = ('%d-%02d' % (disc, n) if discs > 1 else '%02d' % n) + ' - ' + safe(t) + '.flac'
                open(os.path.join(d, fn), 'wb').write(flac(si, frames, r.randint(120, 420), tags))
                songs += 1
    print('%d songs, %d albums, %d album artists in %s' % (songs, albums, len(artists), music))


main()
