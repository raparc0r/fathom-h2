/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/*
 * A character pack's pictures: every character of a list
 * drawn once, on the build machine, into square cells of 4 bits a pixel, with the
 * outline code the player draws its other text with (LVGL's tiny_ttf), so the
 * player has nothing to work out. Run by tools/fonts/mkpack.py, which makes the
 * list and checks the result.
 *
 *   packdraw LIST OUT PX:FONT [PX:FONT ...]
 *
 * LIST: one character a line, as hex, in the order the pictures are to be kept
 * (the commonest first: they share pages). A character a font lacks is left out
 * of the pack, and said on stderr.
 *
 * The file, little-endian, every part starting on a 4096-byte page:
 *   0   "FATHGLY1"
 *   8   u32 characters      12  u16 sizes      14  u16 0
 *   16  u32 where the index starts             20  u32 the file's length
 *   24  for each size, 16 bytes: u16 px (a cell is px by px), u16 bytes a row,
 *       u32 bytes a cell, u32 where its cells start, i8 how far the cell's foot is
 *       under the baseline (negative), u8 bits a pixel (4), u16 0
 *   index: for each character, by code point: u16 code point, u16 its number
 *       (the cell's place in each size) with bit 15 set if it is half a cell wide
 *   cells: number x bytes a cell; rows from the top, two pixels a byte, the left
 *       one in the high half
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype_htcw.h"

#define PAGE 4096u
#define MAX_SIZES 8
#define MAX_CHARS 32767

typedef struct { stbtt_fontinfo info; unsigned char *data; char path[1024]; } font_t;

static void le16(unsigned char *p, unsigned v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static void le32(unsigned char *p, unsigned long v) { le16(p, (unsigned)(v & 0xffff)); le16(p + 2, (unsigned)(v >> 16)); }

static int load(font_t *f, const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) { perror(path); return -1; }
    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    rewind(fp);
    f->data = malloc((size_t)len);
    if (!f->data || fread(f->data, 1, (size_t)len, fp) != (size_t)len) { fprintf(stderr, "cannot read %s\n", path); return -1; }
    fclose(fp);
    snprintf(f->path, sizeof f->path, "%s", path);
    if (!stbtt_InitFont(&f->info, f->data, stbtt_GetFontOffsetForIndex(f->data, 0))) {
        fprintf(stderr, "%s: not a font the outline code reads\n", path);
        return -1;
    }
    return 0;
}

static int cmp_index(const void *a, const void *b)
{
    const unsigned *x = a, *y = b;
    return x[0] < y[0] ? -1 : x[0] > y[0];
}

int main(int argc, char **argv)
{
    if (argc < 4 || argc > 3 + MAX_SIZES) {
        fprintf(stderr, "usage: packdraw LIST OUT PX:FONT [PX:FONT ...]\n");
        return 2;
    }
    int nsizes = argc - 3, px[MAX_SIZES];
    static font_t fonts[MAX_SIZES];
    font_t *use[MAX_SIZES];
    int nfonts = 0;
    for (int s = 0; s < nsizes; s++) {
        char *colon = strchr(argv[3 + s], ':');
        if (!colon || (px[s] = atoi(argv[3 + s])) < 8 || px[s] > 64) { fprintf(stderr, "%s: not PX:FONT\n", argv[3 + s]); return 2; }
        use[s] = NULL;
        for (int k = 0; k < nfonts; k++) if (!strcmp(fonts[k].path, colon + 1)) use[s] = &fonts[k];
        if (!use[s]) {
            if (load(&fonts[nfonts], colon + 1)) return 1;
            use[s] = &fonts[nfonts++];
        }
    }

    /* the list: only what every font in use has */
    static unsigned cps[MAX_CHARS];
    static unsigned char half[MAX_CHARS];
    int n = 0, lacking = 0;
    FILE *lf = fopen(argv[1], "r");
    if (!lf) { perror(argv[1]); return 1; }
    char line[64];
    while (fgets(line, sizeof line, lf)) {
        unsigned long cp = strtoul(line, NULL, 16);
        if (cp < 0x80 || cp > 0xffff) continue;
        int ok = 1;
        for (int k = 0; k < nfonts; k++) ok &= stbtt_FindGlyphIndex(&fonts[k].info, (int)cp) != 0;
        if (!ok) { lacking++; fprintf(stderr, "not in the font: U+%04lX\n", cp); continue; }
        if (n >= MAX_CHARS) { fprintf(stderr, "more than %d characters\n", MAX_CHARS); return 1; }
        /* half a cell wide: its advance is about half the square's */
        int adv, lsb, upm = (int)(1.0f / stbtt_ScaleForMappingEmToPixels(&fonts[0].info, 1.0f) + 0.5f);
        stbtt_GetGlyphHMetrics(&fonts[0].info, stbtt_FindGlyphIndex(&fonts[0].info, (int)cp), &adv, &lsb);
        half[n] = adv * 10 <= upm * 6;
        cps[n++] = (unsigned)cp;
    }
    fclose(lf);
    if (!n) { fprintf(stderr, "no character to draw\n"); return 1; }

    /* where things go */
    unsigned long index_off = PAGE, at = index_off + (unsigned long)n * 4;
    unsigned long cell_off[MAX_SIZES];
    unsigned row_bytes[MAX_SIZES], cell_bytes[MAX_SIZES];
    for (int s = 0; s < nsizes; s++) {
        at = (at + PAGE - 1) / PAGE * PAGE;
        row_bytes[s] = (unsigned)(px[s] + 1) / 2;
        cell_bytes[s] = row_bytes[s] * (unsigned)px[s];
        cell_off[s] = at;
        at += (unsigned long)n * cell_bytes[s];
    }
    unsigned long total = at;
    unsigned char *out = calloc(1, total);
    if (!out) { fprintf(stderr, "no memory\n"); return 1; }

    memcpy(out, "FATHGLY1", 8);
    le32(out + 8, (unsigned long)n);
    le16(out + 12, (unsigned)nsizes);
    le32(out + 16, index_off);
    le32(out + 20, total);

    /* the index, by code point */
    static unsigned idx[MAX_CHARS][2];
    for (int i = 0; i < n; i++) { idx[i][0] = cps[i]; idx[i][1] = (unsigned)i | (half[i] ? 0x8000u : 0); }
    qsort(idx, (size_t)n, sizeof idx[0], cmp_index);
    for (int i = 0; i < n; i++) {
        if (i && idx[i][0] == idx[i - 1][0]) { fprintf(stderr, "U+%04X is in the list twice\n", idx[i][0]); return 1; }
        le16(out + index_off + (unsigned long)i * 4, idx[i][0]);
        le16(out + index_off + (unsigned long)i * 4 + 2, idx[i][1]);
    }

    /* the cells. A square of the font's em: the baseline 0.88 of the way down, as
     * the fonts for Japanese are drawn (the square's foot 0.12 under it). */
    static unsigned char tmp[128 * 128];
    for (int s = 0; s < nsizes; s++) {
        const stbtt_fontinfo *info = &use[s]->info;
        float scale = stbtt_ScaleForMappingEmToPixels(info, (float)px[s]);
        int base = (px[s] * 88 + 50) / 100, clipped = 0;
        unsigned char *h = out + 24 + s * 16;
        le16(h, (unsigned)px[s]);
        le16(h + 2, row_bytes[s]);
        le32(h + 4, cell_bytes[s]);
        le32(h + 8, cell_off[s]);
        h[12] = (unsigned char)(signed char)-(px[s] - base);
        h[13] = 4;
        for (int i = 0; i < n; i++) {
            int g = stbtt_FindGlyphIndex(info, (int)cps[i]), x0, y0, x1, y1;
            stbtt_GetGlyphBitmapBox(info, g, scale, scale, &x0, &y0, &x1, &y1);
            int w = x1 - x0, hgt = y1 - y0;
            if (w <= 0 || hgt <= 0) continue;                    /* (a space) */
            if (w > 128 || hgt > 128) { fprintf(stderr, "U+%04X is %d by %d at %d px\n", cps[i], w, hgt, px[s]); return 1; }
            memset(tmp, 0, (size_t)w * (size_t)hgt);
            stbtt_MakeGlyphBitmap(info, tmp, w, hgt, w, scale, scale, g);
            unsigned char *cell = out + cell_off[s] + (unsigned long)i * cell_bytes[s];
            /* What sticks out of the square but would fit in it is moved in: the
             * tails of the full-width g j p q y and the low line, a row or two.
             * (No kanji or kana does. What is larger than the square, the
             * box-drawing lines made to join, loses its ends.) */
            int dx = 0, dy = 0, lost = 0;
            if (hgt <= px[s]) {
                if (base + y1 > px[s]) dy = px[s] - (base + y1);
                if (base + y0 + dy < 0) dy = -(base + y0);
            }
            if (w <= px[s]) {
                if (x1 > px[s]) dx = px[s] - x1;
                if (x0 + dx < 0) dx = -x0;
            }
            for (int y = 0; y < hgt; y++)
                for (int x = 0; x < w; x++) {
                    int v = (tmp[y * w + x] * 15 + 127) / 255, cx = x0 + dx + x, cy = base + y0 + dy + y;
                    if (!v) continue;
                    if (cx < 0 || cx >= px[s] || cy < 0 || cy >= px[s]) { lost = 1; continue; }
                    cell[(unsigned)cy * row_bytes[s] + (unsigned)cx / 2] |= (unsigned char)(cx & 1 ? v : v << 4);
                }
            clipped += lost;
        }
        fprintf(stderr, "%d px from %s: %d characters, %u bytes a cell, %d with ink outside the cell\n",
                px[s], use[s]->path, n, cell_bytes[s], clipped);
    }

    FILE *of = fopen(argv[2], "wb");
    if (!of || fwrite(out, 1, total, of) != total || fclose(of)) { perror(argv[2]); return 1; }
    fprintf(stderr, "%s: %d characters (%d of the list not in the font), %d sizes, %lu bytes\n", argv[2], n, lacking, nsizes, total);
    return 0;
}
