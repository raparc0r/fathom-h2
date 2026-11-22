/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
#include "ui/fonts.h"

#include <stdlib.h>
#include <string.h>

/* ---------- cmap coverage ---------- */

typedef struct { uint32_t lo, hi; } range_t;

struct font_cov {
    range_t *r;
    int n, cap;
};

static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static int cov_add(font_cov_t *c, uint32_t lo, uint32_t hi)
{
    if (c->n && c->r[c->n - 1].hi + 1 >= lo && c->r[c->n - 1].lo <= lo) {
        if (hi > c->r[c->n - 1].hi) c->r[c->n - 1].hi = hi;
        return 0;
    }
    if (c->n == c->cap) {
        int cap = c->cap ? c->cap * 2 : 64;
        range_t *r = realloc(c->r, (size_t)cap * sizeof *r);
        if (!r) return -1;
        c->r = r;
        c->cap = cap;
    }
    c->r[c->n++] = (range_t){lo, hi};
    return 0;
}

static int cmp_range(const void *a, const void *b)
{
    const range_t *x = a, *y = b;
    return x->lo < y->lo ? -1 : x->lo > y->lo;
}

/* Merge overlapping or adjacent ranges after sorting. */
static void cov_normalise(font_cov_t *c)
{
    if (!c->n) return;
    qsort(c->r, (size_t)c->n, sizeof *c->r, cmp_range);
    int w = 0;
    for (int i = 1; i < c->n; i++) {
        if (c->r[i].lo <= c->r[w].hi + 1) {
            if (c->r[i].hi > c->r[w].hi) c->r[w].hi = c->r[i].hi;
        } else {
            c->r[++w] = c->r[i];
        }
    }
    c->n = w + 1;
}

/* Format 4: BMP segments. A code point is covered when it maps to a glyph other than 0. */
static int parse_fmt4(font_cov_t *c, const uint8_t *t, size_t avail)
{
    if (avail < 14) return -1;
    size_t len = be16(t + 2);
    if (len > avail) len = avail;
    int segx2 = be16(t + 6);
    size_t ends = 14, starts = ends + (size_t)segx2 + 2, deltas = starts + (size_t)segx2,
           offs = deltas + (size_t)segx2;
    if (offs + (size_t)segx2 > len) return -1;
    for (int s = 0; s < segx2; s += 2) {
        uint32_t end = be16(t + ends + s), start = be16(t + starts + s);
        uint16_t delta = be16(t + deltas + s), ro = be16(t + offs + s);
        if (start > end) continue;
        uint32_t run_lo = 0;
        int in_run = 0;
        for (uint32_t cp = start; cp <= end && cp != 0xFFFF; cp++) {
            uint16_t g;
            if (!ro) {
                g = (uint16_t)(cp + delta);
            } else {
                size_t at = offs + (size_t)s + ro + 2 * (cp - start);
                if (at + 2 > len) { g = 0; }
                else {
                    g = be16(t + at);
                    if (g) g = (uint16_t)(g + delta);
                }
            }
            if (g && !in_run) { run_lo = cp; in_run = 1; }
            if (!g && in_run) { if (cov_add(c, run_lo, cp - 1)) return -1; in_run = 0; }
        }
        if (in_run && cov_add(c, run_lo, end == 0xFFFF ? 0xFFFE : end)) return -1;
    }
    return 0;
}

/* Format 12: groups over all planes. */
static int parse_fmt12(font_cov_t *c, const uint8_t *t, size_t avail)
{
    if (avail < 16) return -1;
    uint32_t n = be32(t + 12);
    if ((size_t)n > (avail - 16) / 12) return -1;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *g = t + 16 + 12 * i;
        uint32_t lo = be32(g), hi = be32(g + 4), glyph = be32(g + 8);
        if (lo > hi || hi > 0x10FFFF) continue;
        if (glyph == 0) { if (lo == hi) continue; lo++; }   /* first code maps to .notdef */
        if (cov_add(c, lo, hi)) return -1;
    }
    return 0;
}

font_cov_t *font_cov_new(const void *ttf, size_t len)
{
    const uint8_t *f = ttf;
    if (len < 12) return NULL;
    int ntab = be16(f + 4);
    if (12 + (size_t)ntab * 16 > len) return NULL;
    const uint8_t *cmap = NULL;
    size_t cmap_len = 0;
    for (int i = 0; i < ntab; i++) {
        const uint8_t *e = f + 12 + 16 * i;
        if (memcmp(e, "cmap", 4)) continue;
        size_t off = be32(e + 8), l = be32(e + 12);
        if (off > len || l > len - off) return NULL;
        cmap = f + off;
        cmap_len = l;
    }
    if (!cmap || cmap_len < 4) return NULL;

    /* Prefer a full-Unicode table (format 12), then BMP (format 4). */
    const uint8_t *best = NULL;
    size_t best_avail = 0;
    int best_fmt = 0, n = be16(cmap + 2);
    for (int i = 0; i < n && 4 + 8 * (size_t)i + 8 <= cmap_len; i++) {
        const uint8_t *e = cmap + 4 + 8 * i;
        int plat = be16(e), enc = be16(e + 2);
        size_t off = be32(e + 4);
        if (off + 2 > cmap_len) continue;
        int unicode = plat == 0 || (plat == 3 && (enc == 1 || enc == 10));
        int fmt = be16(cmap + off);
        if (!unicode || (fmt != 4 && fmt != 12)) continue;
        if (fmt > best_fmt) { best = cmap + off; best_avail = cmap_len - off; best_fmt = fmt; }
    }
    if (!best) return NULL;

    font_cov_t *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    int err = best_fmt == 12 ? parse_fmt12(c, best, best_avail) : parse_fmt4(c, best, best_avail);
    if (err || !c->n) { font_cov_free(c); return NULL; }
    cov_normalise(c);
    return c;
}

int font_cov_has(const font_cov_t *c, uint32_t cp)
{
    int lo = 0, hi = c->n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (cp < c->r[mid].lo) hi = mid - 1;
        else if (cp > c->r[mid].hi) lo = mid + 1;
        else return 1;
    }
    return 0;
}

void font_cov_free(font_cov_t *c)
{
    if (!c) return;
    free(c->r);
    free(c);
}

/* ---------- LVGL font wrapper ---------- */

/*
 * The wrapper is a copy of the tiny_ttf font with its own get_glyph_dsc. The
 * copy keeps tiny_ttf's descriptor, so tiny_ttf's bitmap callback works on it.
 * tiny_ttf itself logs an error for every missing glyph; checking coverage
 * first means it is only asked for glyphs it has.
 */
typedef struct {
    lv_font_t font;            /* must be first: LVGL sees this */
    lv_font_t *inner;          /* the tiny_ttf font (owns the glyph caches) */
    font_cov_t *cov;
} wrap_t;

/* Glyph id of the placeholder box ("tofu"); tiny_ttf glyph ids are 16-bit. */
#define TOFU_GID 0x7FFFFFFFu

/* Characters drawn with a near-identical glyph when the font lacks them. */
static uint32_t substitute(const wrap_t *w, uint32_t cp)
{
    static const struct { uint32_t from, to; } subs[] = {
        {0x2010, '-'}, {0x2011, '-'}, {0x2012, 0x2013}, {0x2012, '-'}, {0x2013, '-'},
        {0x2014, 0x2013}, {0x2014, '-'}, {0x2015, 0x2014}, {0x2212, '-'}, {0x00AD, 0},
        {0x2018, '\''}, {0x2019, '\''}, {0x201C, '"'}, {0x201D, '"'},
        {0x00A0, ' '}, {0x2007, ' '}, {0x2009, ' '}, {0x202F, ' '},
    };
    for (size_t i = 0; i < sizeof subs / sizeof *subs; i++)
        if (subs[i].from == cp && (subs[i].to == 0 || font_cov_has(w->cov, subs[i].to)))
            return subs[i].to ? subs[i].to : 0xFFFFFFFFu;
    return 0;
}

static bool wrap_glyph_dsc(const lv_font_t *font, lv_font_glyph_dsc_t *out, uint32_t cp, uint32_t next)
{
    const wrap_t *w = (const wrap_t *)font;
    if (next && !font_cov_has(w->cov, next)) next = 0;   /* no kerning against a missing glyph */

    if (cp < 0x20 || cp == 0x200B || cp == 0x200C || cp == 0x200D || cp == 0xFEFF ||
        font_cov_has(w->cov, cp))
        return w->inner->get_glyph_dsc(font, out, cp, next);

    uint32_t sub = substitute(w, cp);
    if (sub == 0xFFFFFFFFu) {                     /* soft hyphen and the like: draw nothing */
        memset(out, 0, sizeof *out);
        out->format = LV_FONT_GLYPH_FORMAT_NONE;
        return true;
    }
    if (sub) return w->inner->get_glyph_dsc(font, out, sub, next);

    /* Not covered: offer a placeholder box the size of a capital letter. LVGL
     * tries the fallback fonts first and only draws this when none of them has
     * the character. */
    lv_font_glyph_dsc_t h;
    if (!w->inner->get_glyph_dsc(font, &h, 'H', 0)) return false;
    memset(out, 0, sizeof *out);
    out->box_h = h.box_h;
    out->box_w = (uint16_t)(h.box_h * 2 / 3 > 3 ? h.box_h * 2 / 3 : 3);
    out->ofs_x = h.ofs_x;
    out->ofs_y = h.ofs_y;
    out->adv_w = (uint16_t)(out->box_w + 2 * (h.ofs_x > 1 ? h.ofs_x : 1));
    out->format = LV_FONT_GLYPH_FORMAT_A8;
    out->gid.index = TOFU_GID;
    out->is_placeholder = true;
    return true;
}

static const void *wrap_glyph_bitmap(lv_font_glyph_dsc_t *g, lv_draw_buf_t *buf)
{
    const wrap_t *w = (const wrap_t *)g->resolved_font;
    if (g->gid.index != TOFU_GID) return w->inner->get_glyph_bitmap(g, buf);
    if (!buf) return NULL;
    uint32_t stride = buf->header.stride;
    for (int y = 0; y < g->box_h; y++) {
        uint8_t *row = buf->data + (size_t)y * stride;
        int edge = y == 0 || y == g->box_h - 1;
        for (int x = 0; x < g->box_w; x++)
            row[x] = (edge || x == 0 || x == g->box_w - 1) ? 0xFF : 0;
    }
    g->entry = NULL;
    return buf;
}

lv_font_t *ui_font_ttf(const void *ttf, size_t len, int px, const lv_font_t *fallback)
{
    font_cov_t *cov = font_cov_new(ttf, len);
    if (!cov) return NULL;
    lv_font_t *inner = lv_tiny_ttf_create_data(ttf, len, px);
    wrap_t *w = calloc(1, sizeof *w);
    if (!inner || !w) {
        if (inner) lv_tiny_ttf_destroy(inner);
        free(w);
        font_cov_free(cov);
        return NULL;
    }
    w->font = *inner;
    w->font.get_glyph_dsc = wrap_glyph_dsc;
    w->font.get_glyph_bitmap = wrap_glyph_bitmap;
    w->font.fallback = fallback;
    w->inner = inner;
    w->cov = cov;
    return &w->font;
}
