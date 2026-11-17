#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""
Find the signal usbdac-play sent in what the H2's DAC received, and check it
bit for bit.

    python3 tools/usbdac/check.py TAP.raw REF.raw FORMAT

TAP.raw is /tmp/fathom-usbdac.raw from the H2 (the relay records it while
/tmp/fathom-usbdac-tap exists), REF.raw what usbdac-play wrote, FORMAT the
card's format as the player logged it (S24_LE, S32_LE, ...). Silence around
the signal is skipped; the signal itself must be there whole and exact.
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'bitperfect'))
from compare import PACK, expected  # noqa: E402


def main(tap_path, ref_path, fmt):
    tap = open(tap_path, 'rb').read()
    want = expected(open(ref_path, 'rb').read(), 24, fmt)
    frame = 2 * PACK[fmt]
    head = want[:16 * frame]
    at = tap.find(head)
    while at >= 0 and at % frame:
        at = tap.find(head, at + 1)
    if at < 0:
        print('signal not found in the tap (%d bytes)' % len(tap))
        return 1
    got = tap[at:at + len(want)]
    if got == want:
        print('%d frames bit-perfect (from frame %d of the tap)' % (len(want) // frame, at // frame))
        return 0
    i = next((k for k in range(min(len(got), len(want))) if got[k] != want[k]), min(len(got), len(want)))
    print('DIFFERS at frame %d of %d (tap has %d bytes of %d after the start)'
          % (i // frame, len(want) // frame, len(got), len(want)))
    return 1


if __name__ == '__main__':
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    sys.exit(main(*sys.argv[1:]))
