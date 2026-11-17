#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""
Check a tap recorded by fathom-bitperfect against the samples the files hold.

    python3 tools/bitperfect/compare.py TAP.raw REF.raw:BITS:FORMAT [...]

One REF per track, in the order played: the .raw make-bitperfect wrote (16 or
24 bits per sample, little endian, interleaved), its bit depth, and the format
the card ran at for it as fathom-bitperfect printed it (S16_LE, S24_LE,
S24_3LE or S32_LE). The bytes the card should have received are worked out
from each sample (left-justified to 32 bits, then as the format packs it) and
compared with the tap, byte for byte.
"""
import struct
import sys

PACK = {'S16_LE': 2, 'S24_LE': 4, 'S24_3LE': 3, 'S32_LE': 4}


def expected(ref, bits, fmt):
    nb = bits // 8
    n = len(ref) // nb
    out = bytearray()
    for i in range(n):
        raw = int.from_bytes(ref[i * nb:(i + 1) * nb], 'little', signed=True)
        lj = (raw << (32 - bits)) & 0xffffffff          # left-justified int32 bits
        sj = lj - (1 << 32) if lj & 0x80000000 else lj   # as signed
        if fmt == 'S16_LE':
            out += struct.pack('<h', sj >> 16)
        elif fmt == 'S24_LE':
            out += struct.pack('<i', sj >> 8)
        elif fmt == 'S32_LE':
            out += struct.pack('<i', sj)
        elif fmt == 'S24_3LE':
            out += (lj >> 8).to_bytes(3, 'little')
        else:
            raise SystemExit('unknown format ' + fmt)
    return bytes(out)


def main(tap_path, specs):
    tap = open(tap_path, 'rb').read()
    pos, ok = 0, True
    for spec in specs:
        path, bits, fmt = spec.rsplit(':', 2)
        want = expected(open(path, 'rb').read(), int(bits), fmt)
        got = tap[pos:pos + len(want)]
        frame = 2 * PACK[fmt]
        if got == want:
            print('%-28s %s: %d frames bit-perfect' % (path.split('/')[-1], fmt, len(want) // frame))
        else:
            ok = False
            i = next((k for k in range(min(len(got), len(want))) if got[k] != want[k]), min(len(got), len(want)))
            print('%-28s %s: DIFFERS at frame %d of %d (tap has %d bytes of %d)'
                  % (path.split('/')[-1], fmt, i // frame, len(want) // frame, len(got), len(want)))
            print('    want %s\n    got  %s' % (want[i - i % frame:i - i % frame + 2 * frame].hex(' '),
                                                 got[i - i % frame:i - i % frame + 2 * frame].hex(' ')))
        pos += len(want)
    if pos != len(tap):
        ok = False
        print('tap has %d bytes more than the tracks' % (len(tap) - pos))
    print('BIT-PERFECT' if ok else 'NOT bit-perfect')
    return 0 if ok else 1


if __name__ == '__main__':
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    sys.exit(main(sys.argv[1], sys.argv[2:]))
