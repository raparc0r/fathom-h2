/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
#include "ui/fonts.h"
#include "core/clock.h"
#include "core/bytes.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "ui/glyphpack.h"

/* ---------- cmap coverage ---------- */

typedef struct { uint32_t lo, hi; } range_t;

struct font_cov {
    range_t *r;
    int n, cap;
};


/* More ranges than any font has (a few thousand at most): one made to take memory, refused. */
#define COV_MAX 65536

static int cov_add(font_cov_t *c, uint32_t lo, uint32_t hi)
{
    if (c->n && c->r[c->n - 1].hi + 1 >= lo && c->r[c->n - 1].lo <= lo) {
        if (hi > c->r[c->n - 1].hi) c->r[c->n - 1].hi = hi;
        return 0;
    }
    if (c->n >= COV_MAX) return -1;
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
    int segx2 = be16(t + 6) & ~1;                     /* (two bytes a segment: an odd count read a byte past the table) */
    size_t ends = 14, starts = ends + (size_t)segx2 + 2, deltas = starts + (size_t)segx2,
           offs = deltas + (size_t)segx2;
    if (offs + (size_t)segx2 > len) return -1;
    /* The segments come in order, each past the one before: so each code point
     * is looked at once, 65,536 in all. (A table of 32,767 segments that each
     * said 0 to 65,534, a glyph for every other one, was 2,000 million looks
     * and as many ranges as memory would hold.) */
    uint32_t next = 0;
    for (int s = 0; s < segx2; s += 2) {
        uint32_t end = be16(t + ends + s), start = be16(t + starts + s);
        uint16_t delta = be16(t + deltas + s), ro = be16(t + offs + s);
        if (start > end || end < next) continue;
        uint32_t run_lo = 0;
        int in_run = 0;
        for (uint32_t cp = start < next ? next : start; cp <= end && cp != 0xFFFF; cp++) {
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
        next = end + 1;
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
        if (off > cmap_len - 2) continue;             /* (not off + 2: near 4 GB that wraps where size_t is 32 bits) */
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

/* ---------- character packs ---------- */

#define MAX_PACKS 8                 /* (Japanese, Korean, two of Chinese, and room) */
#define MAX_PACK_FONTS 16

static char g_packs_dir[512];
static char g_packs_first[24];      /* "glyphs-ID.bin": asked before the others */
static glyphpack_t *g_packs[MAX_PACKS];
static int g_npacks, g_packs_looked;

typedef struct {
    lv_font_t font;            /* must be first: LVGL sees this */
    int px;
} packfont_t;
static packfont_t g_pack_fonts[MAX_PACK_FONTS];
static int g_npack_fonts;
static unsigned g_pack_seen_arrivals;
static int g_pack_missed;
static int g_packs_test_forget, g_packs_test_delay;
static int g_pack_wanted;
static unsigned g_pack_n_empty, g_pack_n_drawn, g_pack_n_redraws;
/* `touch /tmp/fathom-packs` before the first character from a pack: each time
 * characters were drawn empty, how many and how long until they were drawn
 * again goes to the log (measured on the H2) */
static int g_pack_timed;
static long g_pack_miss_at;
static unsigned g_pack_miss_empty0;

void ui_packs_dir(const char *dir)
{
    for (int i = 0; i < g_npacks; i++) glyphpack_close(g_packs[i]);
    g_npacks = 0;
    g_packs_looked = 0;
    g_pack_seen_arrivals = 0;
    g_pack_missed = 0;
    /* (boxes drawn with the packs as they were say nothing of the packs as they
     * are: a pack that has just come was wanted a frame ago) */
    g_pack_wanted = 0;
    snprintf(g_packs_dir, sizeof g_packs_dir, "%s", dir ? dir : "");
}

static int cmp_name(const void *a, const void *b) { return strcmp(a, b); }

void ui_packs_first(const char *id)
{
    char name[sizeof g_packs_first] = "";
    if (id && *id) snprintf(name, sizeof name, "glyphs-%s.bin", id);
    if (!strcmp(name, g_packs_first)) return;
    snprintf(g_packs_first, sizeof g_packs_first, "%s", name);
    /* the packs open are closed: looked for again, in the new order, when a character next needs one */
    char dir[sizeof g_packs_dir];
    snprintf(dir, sizeof dir, "%s", g_packs_dir);
    ui_packs_dir(dir);
}

/* The folder's packs, opened: once, the first time a character needs one */
static void packs_look(void)
{
    g_packs_looked = 1;
    g_pack_timed = access("/tmp/fathom-packs", F_OK) == 0;
    if (!g_packs_dir[0]) return;
    DIR *d = opendir(g_packs_dir);
    if (!d) return;
    char names[MAX_PACKS * 4][64];
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < MAX_PACKS * 4) {
        size_t len = strlen(e->d_name);
        if (len < 12 || len >= sizeof names[0] || strncmp(e->d_name, "glyphs-", 7) || strcmp(e->d_name + len - 4, ".bin")) continue;
        snprintf(names[n++], sizeof names[0], "%s", e->d_name);
    }
    closedir(d);
    qsort(names, (size_t)n, sizeof names[0], cmp_name);        /* (the same order at every start) */
    /* ...but for the one the user put first (shared characters are drawn as it has them) */
    for (int i = 1; i < n && g_packs_first[0]; i++)
        if (!strcmp(names[i], g_packs_first)) {
            char first[sizeof names[0]];
            memcpy(first, names[i], sizeof first);
            memmove(names[1], names[0], (size_t)i * sizeof names[0]);
            memcpy(names[0], first, sizeof first);
            break;
        }
    for (int i = 0; i < n && g_npacks < MAX_PACKS; i++) {
        char path[640];
        snprintf(path, sizeof path, "%s/%s", g_packs_dir, names[i]);
        glyphpack_t *p = glyphpack_open(path);
        if (!p) continue;
        if (g_packs_test_forget || g_packs_test_delay) glyphpack_test(p, g_packs_test_forget, g_packs_test_delay);
        g_packs[g_npacks++] = p;
    }
}

void ui_packs_test(int forget, int page_delay_ms)
{
    g_packs_test_forget = forget;
    g_packs_test_delay = page_delay_ms;
    for (int i = 0; i < g_npacks; i++) glyphpack_test(g_packs[i], forget, page_delay_ms);
}

/* What a glyph's id says: which pack, which of its sizes, which cell */
#define PACK_GID(pack, size, number) ((uint32_t)(pack) << 28 | (uint32_t)(size) << 24 | (uint32_t)(number))

static bool pack_glyph_dsc(const lv_font_t *font, lv_font_glyph_dsc_t *out, uint32_t cp, uint32_t next)
{
    (void)next;
    /* (below U+2000 the built-in fonts have it or nobody does: no pack is opened for a stray letter) */
    if (cp < 0x2000) return false;
    if (!g_packs_looked) packs_look();
    const packfont_t *pf = (const packfont_t *)font;
    for (int i = 0; i < g_npacks; i++) {
        int half = 0, number = glyphpack_find(g_packs[i], cp, &half);
        if (number < 0) continue;
        int sizes[GLYPHPACK_MAX_SIZES], s = glyphpack_size_for(g_packs[i], pf->px);
        glyphpack_sizes(g_packs[i], sizes);
        memset(out, 0, sizeof *out);
        out->box_h = (uint16_t)sizes[s];
        out->box_w = (uint16_t)(half ? (sizes[s] + 1) / 2 : sizes[s]);
        out->adv_w = out->box_w;
        out->ofs_y = (int16_t)glyphpack_foot(g_packs[i], s);
        out->format = LV_FONT_GLYPH_FORMAT_A8;
        out->gid.index = PACK_GID(i, s, number);
        glyphpack_want(g_packs[i], s, number);      /* (laid out now, drawn in a moment: its page asked for ahead) */
        return true;
    }
    /* Japanese, Chinese or Korean with no pack for it: boxes are drawn, and the
     * UI may say once where packs are (kana, the shared ideographs, half-width
     * katakana, Hangul's syllables and its jamo) */
    if ((cp >= 0x3040 && cp <= 0x30ff) || (cp >= 0x4e00 && cp <= 0x9fff) || (cp >= 0xff61 && cp <= 0xff9f) ||
        (cp >= 0xac00 && cp <= 0xd7a3) || (cp >= 0x3131 && cp <= 0x318e))
        g_pack_wanted = 1;
    return false;
}

int ui_packs_wanted(int clear)
{
    int w = g_pack_wanted;
    if (clear) g_pack_wanted = 0;
    return w;
}

/* Cells drawn empty because their page wasn't in memory: a timer looks, 30
 * times a second and only while there are any, for what the packs' threads
 * have brought in, and has the screen drawn again. Then it goes. */
static lv_timer_t *g_pack_timer;

static void pack_arrivals_cb(lv_timer_t *t)
{
    (void)t;
    unsigned arrived = 0;
    int pending = 0;
    for (int i = 0; i < g_npacks; i++) {
        arrived += glyphpack_arrivals(g_packs[i]);
        pending += glyphpack_pending(g_packs[i]);
    }
    if (arrived != g_pack_seen_arrivals && g_pack_missed) {
        g_pack_seen_arrivals = arrived;
        g_pack_missed = 0;                        /* (drawn again now: a cell still not there counts anew) */
        g_pack_n_redraws++;
        if (g_pack_timed)
            fprintf(stderr, "packs: %u drawn empty, drawn again after %ld ms (%u pages in all, %u characters drawn)\n",
                    g_pack_n_empty - g_pack_miss_empty0, (long)clock_ms() - g_pack_miss_at, arrived, g_pack_n_drawn);
        lv_obj_invalidate(lv_screen_active());
        lv_obj_invalidate(lv_layer_top());
        lv_obj_invalidate(lv_layer_sys());
    }
    if (!pending && !g_pack_missed) {
        lv_timer_delete(g_pack_timer);
        g_pack_timer = NULL;
    }
}

static const void *pack_glyph_bitmap(lv_font_glyph_dsc_t *g, lv_draw_buf_t *buf)
{
    uint32_t id = g->gid.index;
    int pack = (int)(id >> 28), row_bytes = 0;
    if (!buf || pack >= g_npacks) return NULL;
    uint32_t stride = buf->header.stride;
    g->entry = NULL;
    /* never waiting for the card: a cell whose page isn't in memory is left
     * empty this time (its place is kept: the width doesn't depend on it) */
    const uint8_t *cell = glyphpack_cell_ready(g_packs[pack], (int)(id >> 24 & 15), (int)(id & 0xffffff), &row_bytes);
    if (!cell) {
        for (int y = 0; y < g->box_h; y++) memset(buf->data + (size_t)y * stride, 0, g->box_w);
        if (!g_pack_missed && g_pack_timed) { g_pack_miss_at = (long)clock_ms(); g_pack_miss_empty0 = g_pack_n_empty; }
        g_pack_missed = 1;
        g_pack_n_empty++;
        if (!g_pack_timer) g_pack_timer = lv_timer_create(pack_arrivals_cb, 30, NULL);
        return buf;
    }
    for (int y = 0; y < g->box_h; y++) {
        uint8_t *row = buf->data + (size_t)y * stride;
        const uint8_t *src = cell + (size_t)y * (size_t)row_bytes;
        for (int x = 0; x < g->box_w; x++)
            row[x] = (uint8_t)(((src[x / 2] >> (x & 1 ? 0 : 4)) & 15) * 17);
    }
    g_pack_n_drawn++;
    return buf;
}

void ui_packs_counts(unsigned *empty, unsigned *drawn, unsigned *redraws)
{
    if (empty) *empty = g_pack_n_empty;
    if (drawn) *drawn = g_pack_n_drawn;
    if (redraws) *redraws = g_pack_n_redraws;
}

const lv_font_t *ui_font_pack(int px)
{
    for (int i = 0; i < g_npack_fonts; i++)
        if (g_pack_fonts[i].px == px) return &g_pack_fonts[i].font;
    if (g_npack_fonts == MAX_PACK_FONTS) return NULL;
    packfont_t *pf = &g_pack_fonts[g_npack_fonts++];
    memset(pf, 0, sizeof *pf);
    pf->px = px;
    pf->font.get_glyph_dsc = pack_glyph_dsc;
    pf->font.get_glyph_bitmap = pack_glyph_bitmap;
    pf->font.line_height = px + px / 4;
    pf->font.base_line = px / 4;
    return &pf->font;
}
