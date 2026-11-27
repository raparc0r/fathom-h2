/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* A theme's own files (app/ui/assets.c): what a made-up theme on the card may cost at start-up. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#include "ui/assets.h"
#include "ui/fonts.h"
#include "test_util.h"

static char g_dir[PATH_MAX];      /* the card's themes folder */

static void put32(unsigned char *p, uint32_t v) { p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16); p[2] = (unsigned char)(v >> 8); p[3] = (unsigned char)v; }

static void chunk(FILE *f, const char *type, const unsigned char *data, size_t n)
{
    unsigned char b[4];
    put32(b, (uint32_t)n);
    fwrite(b, 1, 4, f);
    fwrite(type, 1, 4, f);
    if (n) fwrite(data, 1, n, f);
    uLong crc = crc32(crc32(0, (const Bytef *)type, 4), data, (uInt)n);
    put32(b, (uint32_t)crc);
    fwrite(b, 1, 4, f);
}

/* An RGBA PNG of w x h in the folder `theme` of the themes folder: every pixel
 * transparent black (pixels 1), or pixel data cut short (pixels 0: a decoder
 * fails on it, the header alone reads fine); `pad` bytes of text before them. */
static void write_png(const char *theme, const char *name, uint32_t w, uint32_t h, int pixels, size_t pad)
{
    char path[PATH_MAX + 128];
    snprintf(path, sizeof path, "%s/%s/%s", g_dir, theme, name);
    FILE *f = fopen(path, "wb");
    if (!f) { CHECKF(0, "cannot write %s", path); return; }
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    unsigned char ihdr[13] = {0};
    put32(ihdr, w);
    put32(ihdr + 4, h);
    ihdr[8] = 8;    /* bits */
    ihdr[9] = 6;    /* RGBA */
    chunk(f, "IHDR", ihdr, sizeof ihdr);
    if (pad) {
        unsigned char *text = calloc(1, pad);
        memcpy(text, "Comment", 8);
        chunk(f, "tEXt", text, pad);
        free(text);
    }
    /* the rows (a filter byte and the pixels, all zero), deflated a little at a
     * time: the test keeps no buffer the size of the picture itself */
    size_t cap = 256 * 1024, n = 0;
    unsigned char *z = malloc(cap);
    static const unsigned char zeros[4096];
    z_stream s;
    memset(&s, 0, sizeof s);
    deflateInit(&s, Z_BEST_COMPRESSION);
    s.next_out = z;
    s.avail_out = (uInt)cap;
    unsigned long long left = pixels ? (unsigned long long)h * (1 + 4ull * w) : 8;
    while (left) {
        size_t now = left < sizeof zeros ? (size_t)left : sizeof zeros;
        s.next_in = (Bytef *)zeros;
        s.avail_in = (uInt)now;
        deflate(&s, Z_NO_FLUSH);
        left -= now;
    }
    int r = deflate(&s, Z_FINISH);
    n = cap - s.avail_out;
    deflateEnd(&s);
    CHECKF(r == Z_STREAM_END, "the picture's data fits the test's buffer");
    chunk(f, "IDAT", z, n);
    chunk(f, "IEND", NULL, 0);
    free(z);
    fclose(f);
}

/* A theme in its own folder; `extra` is appended to its theme.ini. */
static void write_theme(const char *theme, const char *extra)
{
    char path[PATH_MAX + 128];
    snprintf(path, sizeof path, "%s/%s", g_dir, theme);
    mkdir(path, 0755);
    snprintf(path, sizeof path, "%s/%s/theme.ini", g_dir, theme);
    FILE *f = fopen(path, "w");
    if (!f) { CHECKF(0, "cannot write %s", path); return; }
    fprintf(f, "[theme]\nname = %s\n[color]\nbg = #000000\nsurface = #101010\ntext = #FFFFFF\n"
               "text_muted = #CCCCCC\nselect_bg = #FFFFFF\nselect_fg = #000000\nsignal = #FFFFFF\n%s",
            theme, extra);
    fclose(f);
}

static const theme_t *find(const theme_t *t, int n, const char *name)
{
    for (int i = 0; i < n; i++)
        if (!strcmp(t[i].name, name)) return &t[i];
    return NULL;
}

/* A copy of one of our fonts in a theme's folder, made `size` bytes long (0: as it is) */
static void copy_font(const char *theme, const char *name, const char *ours, long size)
{
    char from[PATH_MAX], to[PATH_MAX + 128];
    static char buf[256 * 1024];
    snprintf(from, sizeof from, "%s/fonts/%s", FATHOM_ASSETS_DIR, ours);
    snprintf(to, sizeof to, "%s/%s/%s", g_dir, theme, name);
    FILE *in = fopen(from, "rb"), *out = fopen(to, "wb");
    size_t n = in ? fread(buf, 1, sizeof buf, in) : 0;
    CHECKF(in && out && n > 0 && n < sizeof buf && fwrite(buf, 1, n, out) == n, "copying %s", ours);
    if (in) fclose(in);
    if (out) {
        fflush(out);
        if (size) CHECKF(ftruncate(fileno(out), size) == 0, "making %s %ld bytes", name, size);
        fclose(out);
    }
}

static void put16(unsigned char *p, unsigned v) { p[0] = (unsigned char)(v >> 8); p[1] = (unsigned char)v; }

/* A font file that is only a cmap table, its one subtable (format 4) `sub`
 * bytes long at the file's very end: where that subtable starts. */
static unsigned char *bare_font(unsigned char *f, size_t sub)
{
    memset(f, 0, 40 + sub);
    put16(f + 4, 1);                                  /* one table */
    memcpy(f + 12, "cmap", 4);
    put32(f + 20, 28);                                /* at 28, */
    put32(f + 24, (uint32_t)(12 + sub));              /* this long */
    put16(f + 28 + 2, 1);                             /* one encoding: */
    put16(f + 28 + 4, 3), put16(f + 28 + 6, 1);       /* Windows, Unicode BMP, */
    put32(f + 28 + 8, 12);                            /* its subtable right after */
    put16(f + 40, 4);
    put16(f + 42, (unsigned)sub);
    return f + 40;
}

/* ---- a font's table of characters, made to take memory and time ---- */
static void test_cmap(void)
{
    /* 500 segments that each say "0 to 65,534" and share one list of glyphs,
     * every other one missing: 15,000 ranges a segment, 7.7 million in all
     * (61 MB), from a table of 64 KB. Segments come in order: the first counts. */
    enum { N = 500, L = 65534, G = 16 + 8 * N };
    static unsigned char file[40 + L];
    unsigned char *t = bare_font(file, L);
    put16(t + 6, 2 * N);
    for (int i = 0; i < N; i++) {
        put16(t + 14 + 2 * i, 0xFFFE);                /* end; start and delta stay 0 */
        put16(t + 16 + 6 * N + 2 * i, (unsigned)(2 * N - 2 * i));   /* to the glyphs after the segments */
    }
    for (int cp = 0; G + 2 * cp + 1 < L; cp += 2) put16(t + G + 2 * cp, 1);
    long before = t_peak_kb();
    font_cov_t *c = font_cov_new(file, sizeof file);
    long grew = t_peak_kb() - before;
    CHECKF(grew < 8 * 1024, "a 64 KB table of characters took %ld KB", grew);
    CHECKF(c && font_cov_has(c, 0) && !font_cov_has(c, 1) && font_cov_has(c, 2) && !font_cov_has(c, 0xFFF0),
          "and reads as its first segment says");
    font_cov_free(c);

    /* An odd count of segment bytes (3): the second "segment" took a byte from
     * past the table's end, here the first byte after the font itself. What
     * the font covers can't depend on that byte. */
    static unsigned char odd[40 + 28 + 1];
    int differ = 0, loaded = 0;
    for (int cp = -1; cp < 0x200; cp++) {
        int has[2] = {0, 0};
        for (int k = 0; k < 2; k++) {
            t = bare_font(odd, 28);
            put16(t + 6, 3);
            put16(t + 14, 0x41), put16(t + 19, 0x41), put16(t + 22, 1);   /* (as a count of 3 lays it out) "A" alone, */
            put16(t + 16, 0x100), t[21] = 0x01;                           /* then U+0100, its glyph by the byte outside */
            odd[40 + 28] = k ? 0xFF : 0x00;
            c = font_cov_new(odd, 40 + 28);
            if (c) loaded++;
            has[k] = c && cp >= 0 ? font_cov_has(c, (uint32_t)cp) : 0;
            font_cov_free(c);
        }
        if (has[0] != has[1]) differ++;
    }
    CHECKF(loaded && !differ, "a byte outside the font changes what it covers (%d code points)", differ);

    /* A subtable said to start nearly 4 GB into the table: not there. (Where
     * size_t is 32 bits, as on the H2, "offset + 2" wrapped to 0 and the two
     * bytes before the table were read as its format; a 64-bit computer never
     * saw that, so here this only shows such a font is refused.) */
    t = bare_font(odd, 28);
    put32(odd + 28 + 8, 0xFFFFFFFEu);
    CHECKF(font_cov_new(odd, 40 + 28) == NULL, "a subtable 4 GB away");
    put32(odd + 28 + 8, 0xFFFFFFFFu);
    CHECKF(font_cov_new(odd, 40 + 28) == NULL, "a subtable 4 GB away");
}
static theme_t g_themes[UI_MAX_THEMES];

int main(void)
{
    char tmpl[PATH_MAX];
    t_mkdtemp(tmpl, sizeof tmpl, "fathom-assets");
    snprintf(g_dir, sizeof g_dir, "%s", tmpl);

    /* ---- a theme's icons: only a picture of an icon's size is ever decoded ---- */
    write_theme("Good", "[icons]\nplay = play.png\n");
    write_png("Good", "play.png", 16, 16, 1, 0);
    write_theme("Huge", "[icons]\nplay = play.png\n");
    write_png("Huge", "play.png", 2048, 2048, 1, 0);        /* 17 MB of pixels in a 17 KB file */
    write_theme("Cut", "[icons]\nplay = play.png\n");
    write_png("Cut", "play.png", 2048, 2048, 0, 0);         /* its header says so; its pixels don't decode */
    write_theme("Fat", "[icons]\nplay = play.png\n");
    write_png("Fat", "play.png", 16, 16, 1, 70 * 1024);     /* an icon's size, in a file no icon needs */

    long before = t_peak_kb();
    int n = ui_load_themes(FATHOM_ASSETS_DIR, g_dir, g_themes, UI_MAX_THEMES);
    long grew = t_peak_kb() - before;
    CHECKF(n == 4 + 4, "the built-in themes and the four made here: %d", n);
    CHECKF(grew < 4 * 1024, "reading the themes took %ld KB (a 2048 x 2048 icon decoded is over 30 MB)", grew);

    const theme_t *t = find(g_themes, n, "Good");
    CHECKF(t && t->status == THEME_OK, "a 16 x 16 icon is fine: %s", t ? t->note : "(no theme)");
    CHECKF(t && ui_theme_icon(t, THEME_ICON_PLAY), "and is drawn");
    t = find(g_themes, n, "Huge");
    CHECKF(t && t->status == THEME_WARN && strstr(t->note, "2048x2048"), "a huge icon is named with its size: %s", t ? t->note : "");
    CHECKF(t && !ui_theme_icon(t, THEME_ICON_PLAY), "and not used");
    t = find(g_themes, n, "Cut");
    CHECKF(t && t->status == THEME_WARN && strstr(t->note, "2048x2048"),
          "the size comes from the header (the pixels are never decoded): %s", t ? t->note : "");
    t = find(g_themes, n, "Fat");
    CHECKF(t && t->status == THEME_ERROR && strstr(t->note, "can't be read"), "an icon file over 64 KB is not read: %s", t ? t->note : "");
    CHECKF(t_peak_kb() - before < 4 * 1024, "nor does asking for the icons take memory");

    test_cmap();

    /* ---- a theme's own font: 2 MB at most (read whole at every start, kept once shown) ---- */
    char fdir[PATH_MAX + 16];
    snprintf(fdir, sizeof fdir, "%s/fonts-test", tmpl);
    mkdir(fdir, 0755);
    snprintf(g_dir, sizeof g_dir, "%s", fdir);
    write_theme("BigFont", "[font]\nfile = big.ttf\n");
    copy_font("BigFont", "big.ttf", "AtkinsonHyperlegibleNext-Regular.ttf", 12L * 1024 * 1024);
    /* (three themes with two font files each, three with one) */
    static const char *const own[] = {"Own1", "Own2", "Own3", "Solo1", "Own4", "Solo2", "Solo3"};
    enum { N_OWN = sizeof own / sizeof own[0] };
    for (int i = 0; i < N_OWN; i++) {
        int two = own[i][0] == 'O';
        write_theme(own[i], two ? "[font]\nfile = text.ttf\nfile_title = title.ttf\n" : "[font]\nfile = text.ttf\n");
        copy_font(own[i], "text.ttf", "AtkinsonHyperlegibleNext-Regular.ttf", 0);
        if (two) copy_font(own[i], "title.ttf", "AtkinsonHyperlegibleNext-Medium.ttf", 0);
    }
    before = t_peak_kb();
    n = ui_load_themes(FATHOM_ASSETS_DIR, g_dir, g_themes, UI_MAX_THEMES);
    grew = t_peak_kb() - before;
    CHECKF(n == 4 + 1 + N_OWN, "the built-in themes and those with fonts: %d", n);
    CHECKF(grew < 4 * 1024, "checking the themes' fonts took %ld KB (one is a 12 MB file)", grew);
    t = find(g_themes, n, "BigFont");
    CHECKF(t && t->status == THEME_ERROR && strstr(t->note, "big.ttf can't be read"), "a font file over 2 MB is not read: %s", t ? t->note : "");
    t = find(g_themes, n, "Own1");
    CHECKF(t && t->status == THEME_OK, "a font of ordinary size is fine: %s", t ? t->note : "");

    /* ---- the font files kept: twelve. One more is refused (it was read again for
     * each size of the set, and every copy kept), and a set that has no room for
     * both its files takes none: the place left is still there for a set of one ---- */
    lv_init();
    /* (ours are four: Atkinson's two, Noto, the icons) */
    static const int loads[N_OWN] = {1, 1, 1, 1, 0, 1, 0};   /* 6, 8, 10, 11, (13), 12, (13) files */
    for (int i = 0; i < N_OWN; i++) {
        t = find(g_themes, n, own[i]);
        const ui_fonts_t *f = t ? ui_theme_fonts(t) : NULL;
        if (loads[i]) CHECKF(f && f->body && f->title, "%s: its fonts load", own[i]);
        else CHECKF(!f, "%s: no room for its font files, so the built-in fonts are used", own[i]);
    }
    t = find(g_themes, n, "Own1");
    CHECKF(t && ui_theme_fonts(t), "and those loaded stay");

    if (t_rm_rf(tmpl)) printf("(could not remove %s)\n", tmpl);
    return t_result();
}
