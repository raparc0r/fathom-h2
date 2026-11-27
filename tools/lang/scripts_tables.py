#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional terms under GPLv3 section 7: see LICENSE.
"""
The characters that tell Simplified Chinese from Traditional (and both from
Japanese), for app/library/scripts.c:

    python3 tools/lang/scripts_tables.py > app/library/scripts_tables.h

Simplified's: the everyday characters of GB2312 (level 1) that are neither in
Big5 nor in JIS X 0208 (with what Windows added): a name with one of them is
Simplified Chinese, whatever else is in the library. Traditional's: the
everyday characters of Big5 (level 1) that are neither in GB2312 nor among the
everyday kanji of Japanese (JIS level 1): 這, 們, 麼, 國, 學... Some of them
Japanese keeps for old names (齋藤, 櫻井, 國府田), so one or two in a library
with Japanese in it tell nothing: the count of different ones does
(library_scripts_want). From Python's own tables: nothing to download.
"""


def one(b, enc):
    try:
        s = bytes(b).decode(enc)
    except UnicodeDecodeError:
        return None
    return ord(s) if len(s) == 1 else None


def han(u):
    return u is not None and 0x4E00 <= u <= 0x9FFF


def double(enc, his, los):
    return {u for u in (one([hi, lo], enc) for hi in his for lo in los) if han(u)}


def main():
    gb1 = double('gb2312', range(0xB0, 0xD8), range(0xA1, 0xFF))               # rows 16 to 55
    gb = double('gb2312', range(0xB0, 0xF8), range(0xA1, 0xFF))
    big5_lo = list(range(0x40, 0x7F)) + list(range(0xA1, 0xFF))
    b51 = double('cp950', range(0xA4, 0xC7), big5_lo)                           # level 1
    b5 = double('cp950', range(0xA4, 0xFA), big5_lo)
    jis = double('euc_jp', range(0xA1, 0xFF), range(0xA1, 0xFF)) | \
        double('cp932', list(range(0x81, 0xA0)) + list(range(0xE0, 0xFD)), list(range(0x40, 0x7F)) + list(range(0x80, 0xFD)))
    jis1 = double('euc_jp', range(0xB0, 0xD0), range(0xA1, 0xFF))              # rows 16 to 47
    simp = sorted(gb1 - b5 - jis)
    trad = sorted(b51 - gb - jis1)
    print('/* SPDX-License-Identifier: GPL-3.0-or-later */')
    print('/* Additional terms under GPLv3 section 7: see LICENSE. */')
    print('/* Made by tools/lang/scripts_tables.py: not edited by hand. */')
    for name, t in (('SIMPLIFIED_ONLY', simp), ('TRADITIONAL_ONLY', trad)):
        print('static const uint16_t %s[%d] = {' % (name, len(t)))
        for i in range(0, len(t), 12):
            print('    ' + ' '.join('0x%04X,' % u for u in t[i:i + 12]))
        print('};')


if __name__ == '__main__':
    main()
