#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""
Subset a TrueType (glyf) font to the characters the player needs, with the
standard library only. Fonts are kept whole in RAM on the H2 (32 MiB), so a
pixel font carrying every CJK glyph (Ark Pixel: 4,490 glyphs, 567 KB) is cut
down to Latin, Greek, Cyrillic, punctuation and a few symbols.

    python3 tools/fonts/subset_ttf.py IN.ttf OUT.ttf [--keep CODE,CODE,...]

With --keep (hex code points), only those characters are kept: an icon font
cut down to the icons the UI shows.

Keeps the glyph outlines and metrics untouched (and any glyphs composites
use); writes one cmap (format 12), post format 3, and drops layout tables
(GPOS, GSUB, GDEF), vertical metrics and hinting the UI doesn't use. Adds
"Fathom subset" to the font's name records; the copyright is kept.
"""
import struct
import sys

KEEP_RANGES = [
    (0x20, 0x7E), (0xA0, 0x24F),            # ASCII, Latin-1, Latin Extended-A and B
    (0x370, 0x3FF), (0x400, 0x4FF),         # Greek, Cyrillic
    (0x1E00, 0x1EFF),                       # Latin Extended Additional (Vietnamese)
    (0x2010, 0x205E), (0x20A0, 0x20BF),     # punctuation, currency
    (0x2122, 0x2122), (0x2190, 0x2195),     # trademark, arrows
    (0x25A0, 0x25FF), (0x2605, 0x2606),     # shapes, stars
    (0x2665, 0x2665), (0x266A, 0x266B),     # heart, notes
]
TABLES_KEPT = ['OS/2', 'head', 'hhea', 'maxp', 'name']   # rebuilt: cmap glyf hmtx loca post


def read_tables(d):
    n = struct.unpack('>H', d[4:6])[0]
    t = {}
    for i in range(n):
        tag, _, off, ln = struct.unpack('>4sIII', d[12 + 16 * i:28 + 16 * i])
        t[tag.decode('latin-1')] = d[off:off + ln]
    return t


def cmap_map(cm):
    """code point -> glyph id, from the best subtable (format 12, else 4)."""
    n = struct.unpack('>H', cm[2:4])[0]
    subs = []
    for i in range(n):
        pid, eid, so = struct.unpack('>HHI', cm[4 + 8 * i:12 + 8 * i])
        subs.append((struct.unpack('>H', cm[so:so + 2])[0], so))
    out = {}
    for fmt, st in sorted(subs, key=lambda s: s[0] != 12):
        if fmt == 12:
            ng = struct.unpack('>I', cm[st + 12:st + 16])[0]
            for g in range(ng):
                a, b, s0 = struct.unpack('>III', cm[st + 16 + 12 * g:st + 28 + 12 * g])
                for c in range(a, b + 1):
                    out[c] = s0 + (c - a)
            return out
        if fmt == 4:
            sx2 = struct.unpack('>H', cm[st + 6:st + 8])[0]
            seg = sx2 // 2
            e = struct.unpack('>%dH' % seg, cm[st + 14:st + 14 + sx2])
            s = struct.unpack('>%dH' % seg, cm[st + 16 + sx2:st + 16 + 2 * sx2])
            dl = struct.unpack('>%dh' % seg, cm[st + 16 + 2 * sx2:st + 16 + 3 * sx2])
            ro_off = st + 16 + 3 * sx2
            ro = struct.unpack('>%dH' % seg, cm[ro_off:ro_off + sx2])
            for k in range(seg):
                for c in range(s[k], e[k] + 1):
                    if c == 0xFFFF:
                        continue
                    if ro[k] == 0:
                        g = (c + dl[k]) & 0xFFFF
                    else:
                        a = ro_off + 2 * k + ro[k] + 2 * (c - s[k])
                        g = struct.unpack('>H', cm[a:a + 2])[0]
                        g = (g + dl[k]) & 0xFFFF if g else 0
                    if g:
                        out[c] = g
            return out
    raise SystemExit('no format 4 or 12 cmap')


def components(b):
    """Glyph ids a composite glyph uses, and their byte offsets in it."""
    out, p = [], 10
    while True:
        flags, gi = struct.unpack('>HH', b[p:p + 4])
        out.append((p + 2, gi))
        p += 4 + (4 if flags & 1 else 2)
        p += 2 if flags & 8 else 4 if flags & 0x40 else 8 if flags & 0x80 else 0
        if not flags & 0x20:
            return out


def checksum(data):
    data += b'\0' * (-len(data) % 4)
    return sum(struct.unpack('>%dI' % (len(data) // 4), data)) & 0xFFFFFFFF


def rename(name):
    """Append " (Fathom subset)" to family/full names (IDs 1, 4) in UTF-16 records."""
    fmt, count, so = struct.unpack('>HHH', name[:6])
    recs, strings = [], b''
    for i in range(count):
        pid, eid, lid, nid, ln, off = struct.unpack('>HHHHHH', name[6 + 12 * i:18 + 12 * i])
        s = name[so + off:so + off + ln]
        if nid in (1, 4) and pid in (0, 3):
            s += ' (Fathom subset)'.encode('utf-16-be')
        recs.append(struct.pack('>HHHHHH', pid, eid, lid, nid, len(s), len(strings)))
        strings += s
    return struct.pack('>HHH', 0, count, 6 + 12 * count) + b''.join(recs) + strings


def main(src, dst, keep_only=None):
    d = open(src, 'rb').read()
    t = read_tables(d)
    nglyphs = struct.unpack('>H', t['maxp'][4:6])[0]
    locfmt = struct.unpack('>h', t['head'][50:52])[0]
    if locfmt == 0:
        offs = [2 * o for o in struct.unpack('>%dH' % (nglyphs + 1), t['loca'][:2 * (nglyphs + 1)])]
    else:
        offs = list(struct.unpack('>%dI' % (nglyphs + 1), t['loca'][:4 * (nglyphs + 1)]))
    glyf = t['glyf']
    glyph = lambda g: glyf[offs[g]:offs[g + 1]]

    cmap = cmap_map(t['cmap'])
    if keep_only:
        missing = [c for c in keep_only if c not in cmap]
        if missing:
            raise SystemExit('not in the font: ' + ', '.join('%04x' % c for c in missing))
        keep_cps = sorted(keep_only)
    else:
        keep_cps = sorted(c for c in cmap if any(a <= c <= b for a, b in KEEP_RANGES))
    keep = {0} | {cmap[c] for c in keep_cps}
    todo = list(keep)
    while todo:                                    # glyphs composites are made of
        b = glyph(todo.pop())
        if len(b) >= 10 and struct.unpack('>h', b[:2])[0] < 0:
            for _, gi in components(b):
                if gi not in keep:
                    keep.add(gi)
                    todo.append(gi)
    order = sorted(keep)
    new_id = {g: i for i, g in enumerate(order)}

    new_glyf, loca = b'', []
    for g in order:
        b = bytearray(glyph(g))
        if len(b) >= 10 and struct.unpack('>h', b[:2])[0] < 0:
            for off, gi in components(bytes(b)):
                struct.pack_into('>H', b, off, new_id[gi])
        b += b'\0' * (-len(b) % 4)
        loca.append(len(new_glyf))
        new_glyf += bytes(b)
    loca.append(len(new_glyf))

    nh = struct.unpack('>H', t['hhea'][34:36])[0]
    def metric(g):
        if g < nh:
            return t['hmtx'][4 * g:4 * g + 4]
        aw = t['hmtx'][4 * (nh - 1):4 * (nh - 1) + 2]
        lsb_off = 4 * nh + 2 * (g - nh)
        return aw + t['hmtx'][lsb_off:lsb_off + 2]
    hmtx = b''.join(metric(g) for g in order)

    groups = []                                    # cmap format 12
    for c in keep_cps:
        g = new_id[cmap[c]]
        if groups and c == groups[-1][1] + 1 and g == groups[-1][2] + (c - groups[-1][0]):
            groups[-1][1] = c
        else:
            groups.append([c, c, g])
    sub = struct.pack('>HHIII', 12, 0, 16 + 12 * len(groups), 0, len(groups))
    sub += b''.join(struct.pack('>III', a, b2, g) for a, b2, g in groups)
    cmap_t = struct.pack('>HHHHI', 0, 1, 3, 10, 12) + sub

    head = bytearray(t['head'])
    struct.pack_into('>I', head, 8, 0)
    struct.pack_into('>h', head, 50, 1)
    hhea = bytearray(t['hhea'])
    struct.pack_into('>H', hhea, 34, len(order))
    maxp = bytearray(t['maxp'])
    struct.pack_into('>H', maxp, 4, len(order))
    post = bytearray(t['post'][:32])
    struct.pack_into('>I', post, 0, 0x00030000)

    out = {
        'OS/2': t['OS/2'], 'cmap': cmap_t, 'glyf': new_glyf, 'head': bytes(head), 'hhea': bytes(hhea),
        'hmtx': hmtx, 'loca': struct.pack('>%dI' % len(loca), *loca), 'maxp': bytes(maxp),
        'name': rename(t['name']), 'post': bytes(post),
    }
    tags = sorted(out)
    n = len(tags)
    es = max(k for k in range(16) if 2 ** k <= n)
    sr = 16 * 2 ** es
    font = struct.pack('>IHHHH', 0x00010000, n, sr, es, 16 * n - sr)
    offset, body, dirs = 12 + 16 * n, b'', b''
    for tag in tags:
        data = out[tag]
        dirs += struct.pack('>4sIII', tag.encode('latin-1'), checksum(data), offset + len(body), len(data))
        body += data + b'\0' * (-len(data) % 4)
    font += dirs + body
    adj = (0xB1B0AFBA - checksum(font)) & 0xFFFFFFFF
    head_off = offset + sum(len(out[x]) + (-len(out[x]) % 4) for x in tags[:tags.index('head')])
    font = font[:head_off + 8] + struct.pack('>I', adj) + font[head_off + 12:]
    open(dst, 'wb').write(font)
    print('%s: %d of %d glyphs (%d characters), %d bytes' % (dst, len(order), nglyphs, len(keep_cps), len(font)))


if __name__ == '__main__':
    args = sys.argv[1:]
    keep = None
    if len(args) == 4 and args[2] == '--keep':
        keep = [int(x, 16) for x in args[3].split(',') if x]
        args = args[:2]
    if len(args) != 2:
        sys.exit('usage: subset_ttf.py IN.ttf OUT.ttf [--keep CODE,CODE,...]')
    main(args[0], args[1], keep)
