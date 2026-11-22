#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""
Turn assets/logo.png (as make-logo writes it: 8-bit RGB, no filters) into the
plain PPM (P3) the kernel's boot logo is built from. Standard library only:
it runs in the Buildroot build (external.mk, before the kernel is compiled).

    python3 tools/logo/png2ppm.py logo.png logo_linux_clut224.ppm
"""
import struct
import sys
import zlib


def read_png(src):
    """(width, height, rows of RGB bytes) of an 8-bit RGB PNG with no filters."""
    d = open(src, 'rb').read()
    assert d[:8] == b'\x89PNG\r\n\x1a\n', 'not a PNG'
    p, idat, w, h = 8, b'', 0, 0
    while p < len(d):
        n, t = struct.unpack('>I4s', d[p:p + 8])
        body = d[p + 8:p + 8 + n]
        if t == b'IHDR':
            w, h, depth, ctype = struct.unpack('>IIBB', body[:10])
            assert depth == 8 and ctype == 2, 'needs 8-bit RGB (make-logo writes it so)'
        elif t == b'IDAT':
            idat += body
        p += 12 + n
    raw = zlib.decompress(idat)
    stride = 1 + 3 * w
    rows = []
    for y in range(h):
        row = raw[y * stride:(y + 1) * stride]
        assert row[0] == 0, 'filtered rows: write the PNG with make-logo'
        rows.append(row[1:])
    return w, h, rows


def main(src, dst):
    w, h, rows = read_png(src)
    colours = set()
    with open(dst, 'w') as f:
        f.write('P3\n# Fathom boot logo (assets/logo.png)\n%d %d\n255\n' % (w, h))
        for px in rows:
            colours.update(px[i:i + 3] for i in range(0, len(px), 3))
            f.write(' '.join(str(v) for v in px) + '\n')
    assert len(colours) <= 224, '%d colours: the kernel takes 224 at most' % len(colours)


if __name__ == '__main__':
    if len(sys.argv) != 3:
        sys.exit('usage: png2ppm.py logo.png out.ppm')
    main(sys.argv[1], sys.argv[2])
