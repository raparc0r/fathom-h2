#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""
Pixel Pastel's Home icons, drawn on a 10x10 grid (Ark Pixel's grid, shown at
2x on the H2). Writes them as C (app/ui/pixel_icons.h: one bit string per row)
and, with --preview OUT.ppm, a big preview to look at.

    python3 tools/icons/pixel_icons.py app/ui/pixel_icons.h [--preview OUT.ppm]
"""
import sys

# Home items in the UI's order: now playing, shuffle, search, albums, artists,
# album artists, songs, playlists, favourites, recently added, history, radio,
# settings, then genres, composers, years, folders
ICONS = {
    'nowplaying': [
        '..........',
        '.XX.......',
        '.X.XX.....',
        '.X...XX...',
        '.X.....XX.',
        '.X.....XX.',
        '.X...XX...',
        '.X.XX.....',
        '.XX.......',
        '..........'],
    'shuffle': [
        '..........',
        '.......X..',
        'XX....XXX.',
        '..X..X.X..',
        '...XX.....',
        '...XX.....',
        '..X..X.X..',
        'XX....XXX.',
        '.......X..',
        '..........'],
    'search': [
        '..XXXX....',
        '.X....X...',
        'X......X..',
        'X......X..',
        'X......X..',
        'X......X..',
        '.X....X...',
        '..XXXX.X..',
        '.......XX.',
        '........XX'],
    'albums': [
        '...XXXX...',
        '.XX....XX.',
        '.X......X.',
        'X...XX...X',
        'X..X..X..X',
        'X..X..X..X',
        'X...XX...X',
        '.X......X.',
        '.XX....XX.',
        '...XXXX...'],
    'artists': [
        '...XXXX...',
        '..X....X..',
        '..X....X..',
        '..X....X..',
        'X..XXXX..X',
        '.X......X.',
        '..XXXXXX..',
        '....XX....',
        '....XX....',
        '..XXXXXX..'],
    'albumartists': [
        '..........',
        '.XX...XX..',
        'X..X.X..X.',
        'X..X.X..X.',
        '.XX...XX..',
        '..........',
        'XXXX.XXXX.',
        'X..X.X..X.',
        'X..X.X..X.',
        '..........'],
    'songs': [
        '...XXXXXXX',
        '...X.....X',
        '...XXXXXXX',
        '...X.....X',
        '...X.....X',
        '...X.....X',
        '.XXX...XXX',
        'X..X..X..X',
        'X..X..X..X',
        '.XX....XX.'],
    'playlists': [
        'XXXXXXXX..',
        '..........',
        'XXXXXXXX..',
        '..........',
        'XXXXX...X.',
        '........XX',
        'XXXXX...X.',
        '......XXX.',
        '......XXX.',
        '..........'],
    'favourites': [
        '..........',
        '.XX...XX..',
        'X..X.X..X.',
        'X...X...X.',
        'X.......X.',
        '.X.....X..',
        '..X...X...',
        '...X.X....',
        '....X.....',
        '..........'],
    'recent': [
        '....X.....',
        '....X.....',
        '....X.....',
        '..X.X.X...',
        '...XXX....',
        '....X.....',
        'X.......X.',
        'X.......X.',
        'XXXXXXXXX.',
        '..........'],
    'history': [
        '..XXXXX...',
        '.X.....X..',
        'X...X...X.',
        'X...X...X.',
        'X...XXX.X.',
        'X.......X.',
        'X.......X.',
        '.X.....X..',
        '..XXXXX...',
        '..........'],
    'radio': [
        '.......X..',
        '......X...',
        '.....X....',
        'XXXXXXXXXX',
        'X........X',
        'X.XXX..X.X',
        'X.XXX.X.XX',
        'X.XXX..X.X',
        'X........X',
        'XXXXXXXXXX'],
    'settings': [
        '....X.....',
        '.X.XXX.X..',
        '..XX.XX...',
        'XXX...XXX.',
        '.X.....X..',
        'XXX...XXX.',
        '..XX.XX...',
        '.X.XXX.X..',
        '....X.....',
        '..........'],
    'genres': [
        '..........',
        '.XXX..XXX.',
        '.X.X..X.X.',
        '.XXX..XXX.',
        '..........',
        '..........',
        '.XXX..XXX.',
        '.X.X..X.X.',
        '.XXX..XXX.',
        '..........'],
    'composers': [
        '.......XX.',
        '......X.X.',
        '.....X..X.',
        '....X..X..',
        '...X..X...',
        '..X..X....',
        '.X.XX.....',
        '.XX.......',
        'X.........',
        '..........'],
    'years': [
        '..X....X..',
        '.XXXXXXXX.',
        '.X......X.',
        '.XXXXXXXX.',
        '.X......X.',
        '.X.X.X..X.',
        '.X......X.',
        '.X.X.X..X.',
        '.XXXXXXXX.',
        '..........'],
    'folders': [
        '..........',
        '.XXX......',
        '.X..X.....',
        '.X...XXXX.',
        '.X......X.',
        '.X......X.',
        '.X......X.',
        '.X......X.',
        '.XXXXXXXX.',
        '..........'],
    # Home's "Update available!", shown only while there's one: not a Home item
    # Settings > Home can show or move (H_UPDATE, after the items)
    'update': [
        '...XXXX...',
        '.XX....XX.',
        '.X..XX..X.',
        'X..XXXX..X',
        'X.X.XX.X.X',
        'X...XX...X',
        'X...XX...X',
        '.X..XX..X.',
        '.XX....XX.',
        '...XXXX...'],
}
# (in the UI's H_* order: items added later come after settings)
ORDER = ['nowplaying', 'shuffle', 'search', 'albums', 'artists', 'albumartists', 'songs',
         'playlists', 'favourites', 'recent', 'history', 'radio', 'settings',
         'genres', 'composers', 'years', 'folders', 'update']


def main(out, preview):
    for name in ORDER:
        rows = ICONS[name]
        assert len(rows) == 10 and all(len(r) == 10 and set(r) <= set('.X') for r in rows), name
    with open(out, 'w') as f:
        f.write('/* Generated by tools/icons/pixel_icons.py: do not edit. */\n')
        f.write('#define PIXEL_ICON_GRID 10\n#define PIXEL_ICON_N %d\n' % len(ORDER))
        f.write('/* one row per entry, bit 9 = the leftmost pixel */\n')
        f.write('static const unsigned short PIXEL_ICONS[PIXEL_ICON_N][PIXEL_ICON_GRID] = {\n')
        for name in ORDER:
            bits = ', '.join('0x%03x' % int(r.replace('X', '1').replace('.', '0'), 2) for r in ICONS[name])
            f.write('    {%s},   /* %s */\n' % (bits, name))
        f.write('};\n')
    if preview:
        s, gap, cols = 8, 4, 6
        w, h = cols * (10 * s + gap * s), (len(ORDER) + cols - 1) // cols * (10 * s + gap * s)
        px = [[(23, 20, 42)] * w for _ in range(h)]
        for k, name in enumerate(ORDER):
            ox, oy = (k % cols) * (14 * s) + 2 * s, (k // cols) * (14 * s) + 2 * s
            for y, r in enumerate(ICONS[name]):
                for x, c in enumerate(r):
                    if c == 'X':
                        for yy in range(s):
                            for xx in range(s):
                                px[oy + y * s + yy][ox + x * s + xx] = (195, 211, 228)
        with open(preview, 'wb') as f:
            f.write(b'P6\n%d %d\n255\n' % (w, h))
            for row in px:
                f.write(bytes(v for p in row for v in p))


if __name__ == '__main__':
    a = sys.argv[1:]
    prev = a[a.index('--preview') + 1] if '--preview' in a else None
    main(a[0], prev)
