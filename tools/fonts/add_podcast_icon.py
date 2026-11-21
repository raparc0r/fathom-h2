#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""
Add Home's Podcasts icon to the cut-down icon font (assets/fonts/TablerIcons-Home.ttf),
drawn here in the Tabler Icons' style (a 24-unit grid, 2-unit strokes with
round ends): a dot, two arcs over it, and a stand below. The full Tabler
font isn't in the repo, so the icon is made rather than cut from it.

    python3 tools/fonts/add_podcast_icon.py IN.ttf OUT.ttf [CODEPOINT]

CODEPOINT (hex) is where it goes: E000 (the first private-use one) by
default. Run on a font that already has it, it's replaced.
Standard library only; the font's other glyphs are kept byte for byte.
"""
import math
import struct
import sys

STROKE = 1.0      # half the stroke width, in grid units
CX, CY = 12.0, 11.0


def arc_band(r, a0, a1, steps=28):
    """A stroked arc (angles in degrees, y up, a0 > a1 clockwise over the top), round ends."""
    pts = []
    for i in range(steps + 1):                       # outer edge, a0 -> a1
        a = math.radians(a0 + (a1 - a0) * i / steps)
        pts.append((CX + (r + STROKE) * math.cos(a), CY - (r + STROKE) * math.sin(a)))
    ex, ey = CX + r * math.cos(math.radians(a1)), CY - r * math.sin(math.radians(a1))
    for i in range(1, 8):                            # the round end at a1
        a = math.radians(a1) - math.pi * i / 8
        pts.append((ex + STROKE * math.cos(a), ey - STROKE * math.sin(a)))
    for i in range(steps + 1):                       # inner edge, back
        a = math.radians(a1 + (a0 - a1) * i / steps)
        pts.append((CX + (r - STROKE) * math.cos(a), CY - (r - STROKE) * math.sin(a)))
    sx, sy = CX + r * math.cos(math.radians(a0)), CY - r * math.sin(math.radians(a0))
    for i in range(1, 8):                            # the round end at a0
        a = math.radians(a0) + math.pi - math.pi * i / 8
        pts.append((sx + STROKE * math.cos(a), sy - STROKE * math.sin(a)))
    return pts


def disc(x, y, r, steps=20):
    return [(x + r * math.cos(2 * math.pi * i / steps), y - r * math.sin(2 * math.pi * i / steps)) for i in range(steps)]


def capsule(x, y0, y1, steps=8):
    """A vertical 2-unit stroke from y0 down to y1 (grid y grows downwards), round ends."""
    pts = []
    for i in range(steps + 1):                       # the top end
        a = math.pi * i / steps
        pts.append((x + STROKE * math.cos(a), y0 - STROKE * math.sin(a)))
    for i in range(steps + 1):                       # the bottom end
        a = math.pi + math.pi * i / steps
        pts.append((x + STROKE * math.cos(a), y1 - STROKE * math.sin(a)))
    return pts


def contours():
    return [arc_band(8.0, 220, -40), arc_band(4.5, 210, -30), disc(CX, CY, 1.6), capsule(CX, 15.0, 21.0)]


def to_font(pts):
    """Grid (x 2..22, y 1.5..22 downwards) to font units (1000/em, baseline 0, as the other icons)."""
    s = 1000 / 24.0
    out = [(round(x * s), round((21.5 - y) * s)) for x, y in pts]
    # clockwise (TrueType's outer contours), by the shoelace sum in y-up space
    area = sum(out[i][0] * out[(i + 1) % len(out)][1] - out[(i + 1) % len(out)][0] * out[i][1] for i in range(len(out)))
    if area > 0:
        out.reverse()
    dedup = [p for i, p in enumerate(out) if p != out[i - 1]]
    return dedup


def glyph_bytes(cs):
    allp = [p for c in cs for p in c]
    xmin, xmax = min(p[0] for p in allp), max(p[0] for p in allp)
    ymin, ymax = min(p[1] for p in allp), max(p[1] for p in allp)
    b = struct.pack('>hhhhh', len(cs), xmin, ymin, xmax, ymax)
    end, n = [], 0
    for c in cs:
        n += len(c)
        end.append(n - 1)
    b += struct.pack('>%dH' % len(end), *end)
    b += struct.pack('>H', 0)                         # no instructions
    b += bytes([0x01] * n)                            # all on the curve; two-byte deltas
    px = py = 0
    xs, ys = b'', b''
    for x, y in allp:
        xs += struct.pack('>h', x - px)
        ys += struct.pack('>h', y - py)
        px, py = x, y
    b += xs + ys
    while len(b) % 4:
        b += b'\0'
    return b, xmin, len(allp), len(cs)


def read_tables(d):
    n = struct.unpack('>H', d[4:6])[0]
    t = {}
    for i in range(n):
        tag, _, off, ln = struct.unpack('>4sIII', d[12 + 16 * i:28 + 16 * i])
        t[tag.decode('latin-1')] = bytearray(d[off:off + ln])
    return t


def checksum(b):
    b = bytes(b) + b'\0' * ((4 - len(b) % 4) % 4)
    return sum(struct.unpack('>%dI' % (len(b) // 4), b)) & 0xFFFFFFFF


def main(src, dst, cp):
    d = open(src, 'rb').read()
    t = read_tables(d)
    head, hhea, maxp = t['head'], t['hhea'], t['maxp']
    assert struct.unpack('>h', head[50:52])[0] == 1, 'long loca expected'
    ng = struct.unpack('>H', maxp[4:6])[0]
    nhm = struct.unpack('>H', hhea[34:36])[0]
    loca = list(struct.unpack('>%dI' % (ng + 1), t['loca'][:4 * (ng + 1)]))
    glyf = bytes(t['glyf'][:loca[ng]])
    # cmap format 12: its groups; the code point's old glyph (if any) is replaced
    c = t['cmap']
    so = struct.unpack('>I', c[8:12])[0]
    groups = []
    for g in range(struct.unpack('>I', c[so + 12:so + 16])[0]):
        s, e, gid = struct.unpack('>III', c[so + 16 + 12 * g:so + 28 + 12 * g])
        for k in range(s, e + 1):
            groups.append((k, gid + k - s))
    old = dict(groups).get(cp)
    gb, xmin, npts, ncont = glyph_bytes([to_font(c) for c in contours()])
    if old is not None:                               # replace it in place
        glyf = glyf[:loca[old]] + gb + glyf[loca[old + 1]:]
        delta = len(gb) - (loca[old + 1] - loca[old])
        loca = loca[:old + 1] + [o + delta for o in loca[old + 1:]]
        gid = old
    else:
        gid = ng
        glyf += gb
        loca.append(len(glyf))
        ng += 1
        groups.append((cp, gid))
    groups.sort()
    # hmtx: every glyph its own metrics (advance 1000, as the others)
    hm = t['hmtx']
    mets = [struct.unpack('>Hh', hm[4 * i:4 * i + 4]) for i in range(nhm)]
    lsbs = [struct.unpack('>h', hm[4 * nhm + 2 * i:4 * nhm + 2 * i + 2])[0] for i in range(ng - 1 - nhm + (old is not None))]
    mets += [(mets[-1][0], l) for l in lsbs]
    if old is not None:
        mets[gid] = (1000, xmin)
    else:
        mets.append((1000, xmin))
    t['hmtx'] = bytearray(b''.join(struct.pack('>Hh', a, l) for a, l in mets))
    struct.pack_into('>H', hhea, 34, len(mets))
    struct.pack_into('>H', maxp, 4, ng)
    if len(maxp) >= 10:
        struct.pack_into('>H', maxp, 6, max(struct.unpack('>H', maxp[6:8])[0], npts))
        struct.pack_into('>H', maxp, 8, max(struct.unpack('>H', maxp[8:10])[0], ncont))
    t['glyf'] = bytearray(glyf)
    t['loca'] = bytearray(struct.pack('>%dI' % len(loca), *loca))
    # cmap: one format 12 subtable (Unicode full), groups of consecutive codes
    runs = []
    for k, g in groups:
        if runs and runs[-1][1] + 1 == k and runs[-1][2] + (k - runs[-1][0]) == g:
            runs[-1][1] = k
        else:
            runs.append([k, k, g])
    sub = struct.pack('>HHIII', 12, 0, 16 + 12 * len(runs), 0, len(runs))
    sub += b''.join(struct.pack('>III', a, b, g) for a, b, g in runs)
    t['cmap'] = bytearray(struct.pack('>HH', 0, 1) + struct.pack('>HHI', 3, 10, 12) + sub)
    # write it out: the tables in tag order, checksums, head's adjustment
    tags = sorted(t)
    nt = len(tags)
    es = 1 << (nt.bit_length() - 1)
    out = struct.pack('>IHHHH', 0x00010000, nt, es * 16, es.bit_length() - 1, nt * 16 - es * 16)
    off = 12 + 16 * nt
    body, dirs = b'', b''
    struct.pack_into('>I', head, 8, 0)
    for tag in tags:
        data = bytes(t[tag])
        dirs += struct.pack('>4sIII', tag.encode('latin-1'), checksum(data), off + len(body), len(data))
        body += data + b'\0' * ((4 - len(data) % 4) % 4)
    font = bytearray(out + dirs + body)
    adj = (0xB1B0AFBA - checksum(font)) & 0xFFFFFFFF
    # head's place in the file: from the directory
    for i in range(nt):
        tag, _, o, _ = struct.unpack('>4sIII', font[12 + 16 * i:28 + 16 * i])
        if tag == b'head':
            struct.pack_into('>I', font, o + 8, adj)
    open(dst, 'wb').write(font)
    print('%s: U+%04X is glyph %d (%d points, %d contours), %d glyphs' % (dst, cp, gid, npts, ncont, ng))


if __name__ == '__main__':
    if len(sys.argv) < 3:
        sys.exit('usage: add_podcast_icon.py IN.ttf OUT.ttf [CODEPOINT]')
    main(sys.argv[1], sys.argv[2], int(sys.argv[3], 16) if len(sys.argv) > 3 else 0xE000)
