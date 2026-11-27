#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional terms under GPLv3 section 7: see LICENSE.
"""
A character pack: the characters of a language, drawn
once here so the player has nothing to work out.

    python3 tools/fonts/mkpack.py ja|ko|zh-hans|zh-hant OUT.bin [--sample OUT.png] [--zip OUT.zip] [--sizes 16,18,20:R 22:M]

Japanese: JIS X 0208 (kana, symbols, 6,355 kanji), the half-width katakana and
the characters Windows added to it (name variants such as 髙 and 﨑), from
Python's own tables: nothing to download for the list. In the file the
commonest come first, so that they share pages: kana and symbols, the
half-width katakana, the 2,965 everyday kanji (JIS level 1), the rest of the
symbols, level 2, the added kanji. Latin, Greek and Cyrillic are left out: the
player's other fonts have them.

Korean: every Hangul syllable (11,172) and every compatibility jamo, with the
symbols of KS X 1001. The 2,350 syllables of KS X 1001, the everyday ones,
come first. No hanja: nine in ten of KS X 1001's are in the Japanese pack, and
names in Korean seldom use them.

Chinese, two packs. Simplified (zh-hans): GB2312, its 3,755 everyday
characters (level 1) first, then the 3,008 of level 2. Traditional (zh-hant):
Big5 with the seven characters Windows added, its 5,401 everyday characters
first, then the 7,659 others. Each in its own region's shapes (Noto Sans SC,
Noto Sans TC): about half of either is also in the Japanese pack, drawn a
little differently there.

The pictures are drawn by tools/fonts/packdraw.c (compiled here with cc, with
the outline code the player uses), from Noto Sans JP, KR, SC or TC in
build/fonts-src/noto-sans-{jp,kr,sc,tc}-{regular,medium}/ (SIL Open Font
License; fetched by hand from github.com/notofonts/noto-cjk,
Sans/SubsetOTF/{JP,KR,SC,TC}: not kept in git).
Then the file is read back and checked, and with --sample a picture of some
text in each size is written, to look at. With --zip the pack as it is
published: a zip of the file and the fonts' licence (the same bytes every
time), and the line for app/net/packfetch.c's list: the sizes and the digest
the player checks a download against.
"""
import hashlib
import os
import struct
import subprocess
import sys
import zipfile
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
STB = os.path.join(ROOT, 'build', '_deps', 'lvgl-src', 'src', 'libs', 'tiny_ttf')
def fonts(lang):
    """The language's two source fonts: R(egular) and M(edium)."""
    f = LANGS[lang]['font']                       # "NotoSansJP"
    d = 'noto-sans-' + f[-2:].lower()             # "noto-sans-jp"
    return {'R': os.path.join(ROOT, 'build', 'fonts-src', d + '-regular', f + '-Regular.otf'),
            'M': os.path.join(ROOT, 'build', 'fonts-src', d + '-medium', f + '-Medium.otf')}


PAGE = 4096


def one(b, enc):
    try:
        s = bytes(b).decode(enc)
    except UnicodeDecodeError:
        return None
    return ord(s) if len(s) == 1 else None


def wanted(u):
    """Not what the player's other fonts already draw, nor a private-use code."""
    return u is not None and u >= 0x2000 and not 0xE000 <= u <= 0xF8FF and u <= 0xFFFF


def japanese():
    rows = {}
    for r in range(1, 95):
        rows[r] = [u for u in (one([0xA0 + r, 0xA0 + c], 'euc_jp') for c in range(1, 95)) if u]
    base = {u for v in rows.values() for u in v}
    halfwidth = [u for u in (one([b], 'shift_jis') for b in range(0xA1, 0xE0)) if u]
    extra = []
    for hi in list(range(0x81, 0xA0)) + list(range(0xE0, 0xFD)):
        for lo in list(range(0x40, 0x7F)) + list(range(0x80, 0xFD)):
            u = one([hi, lo], 'cp932')
            if u and u not in base and u not in extra:
                extra.append(u)
    kanji = lambda u: 0x4E00 <= u <= 0x9FFF or 0xF900 <= u <= 0xFAFF
    order = []
    for r in range(1, 9):
        order += rows[r]                       # kana, symbols
    order += halfwidth
    for r in range(16, 48):
        order += rows[r]                       # level 1
    for r in range(9, 16):
        order += rows[r]
    order += [u for u in extra if not kanji(u)]
    for r in range(48, 95):
        order += rows[r]                       # level 2
    order += [u for u in extra if kanji(u)]
    seen, out = set(), []
    for u in order:
        if wanted(u) and u not in seen:
            seen.add(u)
            out.append(u)
    return out


def korean():
    rows = {}
    for r in range(1, 95):
        rows[r] = [u for u in (one([0xA0 + r, 0xA0 + c], 'euc_kr') for c in range(1, 95)) if u]
    hanja = lambda u: 0x4E00 <= u <= 0x9FFF or 0xF900 <= u <= 0xFAFF
    order = []
    for r in range(1, 16):
        order += rows[r]                       # symbols, jamo, full-width letters
    order += list(range(0x3131, 0x318F))       # every compatibility jamo
    for r in range(16, 41):
        order += rows[r]                       # KS X 1001's 2,350 syllables: the everyday ones
    order += list(range(0xAC00, 0xD7A4))       # every syllable
    seen, out = set(), []
    for u in order:
        if wanted(u) and not hanja(u) and u not in seen:
            seen.add(u)
            out.append(u)
    return out


def chinese_simplified():
    rows = {}
    for r in range(1, 95):
        rows[r] = [u for u in (one([0xA0 + r, 0xA0 + c], 'gb2312') for c in range(1, 95)) if u]
    order = []
    for r in range(1, 10):
        order += rows[r]                       # punctuation, symbols, full-width letters, kana, bopomofo
    for r in range(16, 56):
        order += rows[r]                       # level 1: the everyday characters, by sound
    for r in range(10, 16):
        order += rows[r]
    for r in range(56, 95):
        order += rows[r]                       # level 2
    seen, out = set(), []
    for u in order:
        if wanted(u) and u not in seen:
            seen.add(u)
            out.append(u)
    return out


def chinese_traditional():
    def lead(his):
        got = []
        for hi in his:
            for lo in list(range(0x40, 0x7F)) + list(range(0xA1, 0xFF)):
                u = one([hi, lo], 'cp950')
                if u:
                    got.append(u)
        return got
    # symbols and bopomofo; level 1 (the everyday characters, by strokes); level 2; what Windows added
    order = lead(range(0xA1, 0xA4)) + lead(range(0xA4, 0xC7)) + lead(range(0xC7, 0xC9)) + lead(range(0xC9, 0xFA))
    seen, out = set(), []
    for u in order:
        if wanted(u) and u not in seen:
            seen.add(u)
            out.append(u)
    return out


class Pack:
    def __init__(self, path):
        self.d = d = open(path, 'rb').read()
        assert d[:8] == b'FATHGLY1', 'not a pack'
        self.n, nsizes, zero, index_off, length = struct.unpack_from('<IHHII', d, 8)
        assert zero == 0 and length == len(d), 'the length in the file is not the file\'s'
        assert index_off % PAGE == 0
        self.sizes = []
        for s in range(nsizes):
            px, row, cell, off, foot, bits, pad = struct.unpack_from('<HHIIbBH', d, 24 + 16 * s)
            assert bits == 4 and row == (px + 1) // 2 and cell == row * px and off % PAGE == 0 and pad == 0
            assert off + self.n * cell <= len(d)
            self.sizes.append({'px': px, 'row': row, 'cell': cell, 'off': off, 'foot': foot})
        self.where, last = {}, -1
        numbers = set()
        for i in range(self.n):
            cp, num = struct.unpack_from('<HH', d, index_off + 4 * i)
            assert cp > last, 'the index is not in order at U+%04X' % cp
            last = cp
            assert num & 0x7FFF < self.n and num & 0x7FFF not in numbers
            numbers.add(num & 0x7FFF)
            self.where[cp] = (num & 0x7FFF, bool(num & 0x8000))

    def cell(self, s, cp):
        """The character's rows of 0..15, and whether it is half a cell wide."""
        z = self.sizes[s]
        num, half = self.where[cp]
        raw = self.d[z['off'] + num * z['cell']: z['off'] + (num + 1) * z['cell']]
        rows = []
        for y in range(z['px']):
            r = raw[y * z['row']:(y + 1) * z['row']]
            rows.append([(r[x // 2] >> (0 if x & 1 else 4)) & 15 for x in range(z['px'])])
        return rows, half


def check(pack, chars, blank_ok):
    assert set(pack.where) <= set(chars)
    for s, z in enumerate(pack.sizes):
        blank = [cp for cp in pack.where if not any(any(r) for r in pack.cell(s, cp)[0])]
        halves = sorted(cp for cp in pack.where if pack.where[cp][1])
        print('  %2d px: %d bytes a cell, cells from %d; no ink: %s; half-width: %d%s' % (
            z['px'], z['cell'], z['off'], ' '.join('U+%04X' % c for c in blank) or 'none',
            len(halves), ' (U+%04X to U+%04X)' % (halves[0], halves[-1]) if halves else ''))
        assert all(c in blank_ok for c in blank), 'characters with no picture'
    first = [cp for cp, (num, _) in pack.where.items() if num * pack.sizes[-1]['cell'] < 16 * PAGE]
    print('  in the first 16 pages of the largest size: %d characters' % len(first))


def png(path, w, h, px):
    raw = b''.join(b'\0' + bytes(c for p in px[y * w:(y + 1) * w] for c in p) for y in range(h))
    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
                + chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


SAMPLE_JA = ['東京の夜に聴く音楽', 'アルバム　ｱﾙﾊﾞﾑ　あるばむ', '山田太郎 ／ 髙橋 﨑山', '春夏秋冬、花鳥風月。', '♪「新しい曲」第１番 ★ ※']
SAMPLE_SC = ['北京的夜晚听音乐', '专辑　歌手　乐队', '张伟 ／ 王芳 乐队', '春夏秋冬，花鸟风月。', '♪「新歌」第１首 ★ ※']
SAMPLE_TC = ['臺北的夜晚聽音樂', '專輯　歌手　樂團', '陳怡君 ／ 林志明 樂團', '春夏秋冬，花鳥風月。', '♪「新歌」第１首 ★ ※']
SAMPLE_KO = ['서울의 밤에 듣는 음악', '봄 여름 가을 겨울', '김민준 ／ 이서연 밴드', 'ㄱㄴㄷㄹ ㅏㅑㅓㅕ 똠방각하 쀍', '♪「새 노래」제１번 ★ ※']

# (blank: the characters that are meant to have no ink: the wide space, Hangul's filler)
LANGS = {
    'ja': {'chars': japanese, 'font': 'NotoSansJP', 'name': 'Noto Sans JP', 'sample': SAMPLE_JA, 'blank': {0x3000}},
    'ko': {'chars': korean, 'font': 'NotoSansKR', 'name': 'Noto Sans KR', 'sample': SAMPLE_KO, 'blank': {0x3000, 0x3164}},
    'zh-hans': {'chars': chinese_simplified, 'font': 'NotoSansSC', 'name': 'Noto Sans SC', 'sample': SAMPLE_SC, 'blank': {0x3000}},
    'zh-hant': {'chars': chinese_traditional, 'font': 'NotoSansTC', 'name': 'Noto Sans TC', 'sample': SAMPLE_TC, 'blank': {0x3000}},
}


def sample(pack, path, texts, scale=2):
    bg, fg = (16, 16, 18), (236, 236, 236)
    W, lines = 300, []
    for s, z in enumerate(pack.sizes):
        for text in texts:
            lines.append((s, text))
    H = sum(pack.sizes[s]['px'] + 6 for s, _ in lines) + 8
    img = [bg] * (W * H)
    y = 6
    for s, text in lines:
        z, x = pack.sizes[s], 6
        for ch in text:
            cp = ord(ch)
            if cp == 0x20:
                x += z['px'] // 3
                continue
            if cp not in pack.where:
                x += z['px']
                continue
            rows, half = pack.cell(s, cp)
            for cy, row in enumerate(rows):
                for cx, v in enumerate(row):
                    if v and x + cx < W:
                        img[(y + cy) * W + x + cx] = tuple(bg[k] + (fg[k] - bg[k]) * v // 15 for k in range(3))
            x += (z['px'] + 1) // 2 if half else z['px']
        y += z['px'] + 6
    big = []
    for yy in range(H):
        row = []
        for xx in range(W):
            row += [img[yy * W + xx]] * scale
        big += row * scale
    png(path, W * scale, H * scale, big)


def font_name(path, wanted):
    """A font's own word for itself: its name table's entry `wanted` (0: the copyright)."""
    d = open(path, 'rb').read()
    for i in range(struct.unpack('>H', d[4:6])[0]):
        tag, _, off, _ = struct.unpack('>4sIII', d[12 + 16 * i:28 + 16 * i])
        if tag != b'name':
            continue
        count, strings = struct.unpack('>HH', d[off + 2:off + 6])
        for k in range(count):
            pid, _, _, nid, ln, o = struct.unpack('>6H', d[off + 6 + 12 * k:off + 18 + 12 * k])
            if pid == 3 and nid == wanted:
                return d[off + strings + o:off + strings + o + ln].decode('utf-16-be')
    return ''


def licence(lang):
    """What goes with the pack: whose the characters' shapes are, and the licence
    they come under (its text as the repository already carries it)."""
    ofl = open(os.path.join(ROOT, 'assets', 'fonts', 'OFL-AtkinsonHyperlegibleNext.txt'), encoding='utf-8').read()
    body = ofl[ofl.index('This Font Software is licensed under the SIL Open Font License'):]
    return ('The characters in this pack are drawn from %s (Regular and Medium),\n'
            'https://github.com/notofonts/noto-cjk\n\n' % LANGS[lang]['name']
            + font_name(fonts(lang)['R'], 0) + '\n\n' + body)


def write_zip(path, name, data, licence_name, text):
    """Stored the same way every time: fixed dates, the pack first."""
    with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for n, d in ((name, data), (licence_name, text.encode('utf-8'))):
            info = zipfile.ZipInfo(n, date_time=(2026, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            z.writestr(info, d, compresslevel=9)


def main():
    args = sys.argv[1:]
    if len(args) < 2 or args[0] not in LANGS:
        sys.exit(__doc__)
    lang = args[0]
    out, sample_png, zip_out, sizes = args[1], None, None, ['16,18,20:R', '22:M']
    rest = args[2:]
    while rest:
        a = rest.pop(0)
        if a == '--sample':
            sample_png = rest.pop(0)
        elif a == '--zip':
            zip_out = rest.pop(0)
        elif a == '--sizes':
            sizes = []
            while rest and not rest[0].startswith('--'):
                sizes.append(rest.pop(0))
    chars = LANGS[lang]['chars']()
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    work = os.path.join(ROOT, 'build', 'packs-work')
    os.makedirs(work, exist_ok=True)
    listing = os.path.join(work, lang + '.txt')
    with open(listing, 'w') as f:
        f.write(''.join('%04X\n' % u for u in chars))
    tool = os.path.join(work, 'packdraw')
    subprocess.run(['cc', '-O2', '-Wall', '-Wno-unused-function', '-I', STB, '-o', tool,
                    os.path.join(ROOT, 'tools', 'fonts', 'packdraw.c'), '-lm'], check=True)
    spec = []
    for group in sizes:
        pxs, font = group.split(':')
        spec += ['%d:%s' % (int(p), fonts(lang)[font]) for p in pxs.split(',')]
    print('%d characters in the list' % len(chars))
    subprocess.run([tool, listing, out] + spec, check=True)
    pack = Pack(out)
    print('%s: %d characters, %d bytes; as a zip about %d' % (out, pack.n, len(pack.d), len(zlib.compress(pack.d, 9))))
    check(pack, chars, LANGS[lang]['blank'])
    if sample_png:
        sample(pack, sample_png, LANGS[lang]['sample'])
        print(sample_png)
    if zip_out:
        name = os.path.basename(out)
        write_zip(zip_out, name, pack.d, 'LICENSE-%s.txt' % LANGS[lang]['font'], licence(lang))
        print('%s: %d bytes' % (zip_out, os.path.getsize(zip_out)))
        print('for app/net/packfetch.c:  %d, %d, "%s"' % (os.path.getsize(zip_out), len(pack.d), hashlib.sha256(pack.d).hexdigest()))


if __name__ == '__main__':
    main()
