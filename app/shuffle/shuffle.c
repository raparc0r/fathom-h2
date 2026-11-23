/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
#include "shuffle.h"

#include <stdlib.h>
#include <string.h>

#include "../core/rng.h"

/* Scores and positions are fixed-point (Q32: 1.0 is 1 << 32), not doubles:
 * the H2's build has no hardware floating point, and a shuffle of 50,000
 * songs spent most of its 4 s in the soft-float routines these sorts and sums
 * called. The options' weights (doubles, three of them) are turned to Q16
 * once. */
typedef int64_t fx_t;
#define FX_ONE ((fx_t)1 << 32)
#define FRESH_SECONDS (30 * 86400)
#define JITTER_PCT 15                  /* +-0.15 of a step */

typedef struct {
    uint32_t kind;   /* 0 = standalone, 1 = grouped */
    uint32_t key;    /* track index (standalone) or album/unit key (grouped) */
    uint16_t disc, track;
    uint32_t idx;    /* index into tracks */
} member_t;

typedef struct {
    uint32_t first;  /* range in the members array */
    uint32_t count;
    uint32_t artist;
    uint32_t album;
    fx_t     score;  /* higher = earlier within its album */
    fx_t     fresh;  /* 0 = heard just now .. 1 = not heard for 30+ days / never */
    fx_t     pos;    /* position on the 0..1 line */
    uint8_t  tail;   /* contains a track from the previous cycle's tail */
    uint8_t  gap_artist;
    uint8_t  gap_album;
} unit_t;

void shuf_default_opts(shuf_opts_t *o, int64_t now)
{
    memset(o, 0, sizeof *o);
    o->mode = SHUF_MODE_SONGS;
    o->freshness = 1;
    o->w_fresh = 0.6;
    o->w_skip = 0.4;
    o->w_recent = 0.6;
    o->skip_interludes = 1;
    o->interlude_ms = 45000;
    o->keep_units = 0;
    o->exclude_spoken = 0;
    o->now = now;
    o->seam = 30;
    o->max_artist_gap = 4;
    o->max_album_gap = 8;
    o->repair_window = 64;
    o->first = -1;
}

/* ---- sorting helpers (qsort has no context argument, so use a file-static pointer;
 *      shuf_build is not re-entrant across threads) ---- */

static const unit_t *g_units;

static int cmp_member(const void *a, const void *b)
{
    const member_t *x = a, *y = b;
    if (x->kind != y->kind) return x->kind < y->kind ? -1 : 1;
    if (x->key != y->key) return x->key < y->key ? -1 : 1;
    if (x->disc != y->disc) return x->disc < y->disc ? -1 : 1;
    if (x->track != y->track) return x->track < y->track ? -1 : 1;
    return x->idx < y->idx ? -1 : (x->idx > y->idx);
}

static int cmp_artist_album_score(const void *a, const void *b)
{
    const unit_t *x = &g_units[*(const uint32_t *)a], *y = &g_units[*(const uint32_t *)b];
    if (x->artist != y->artist) return x->artist < y->artist ? -1 : 1;
    if (x->album != y->album) return x->album < y->album ? -1 : 1;
    if (x->score != y->score) return x->score > y->score ? -1 : 1;
    return x->first < y->first ? -1 : (x->first > y->first);
}

static int cmp_pos(const void *a, const void *b)
{
    const unit_t *x = &g_units[*(const uint32_t *)a], *y = &g_units[*(const uint32_t *)b];
    if (x->pos != y->pos) return x->pos < y->pos ? -1 : 1;
    return x->first < y->first ? -1 : (x->first > y->first);
}

/* ---- sorting by a 64-bit key, stable (LSD radix, 11 bits a pass, only the
 * bits in use) ----
 * qsort over the units went through a comparison function and, for each
 * comparison, two units picked from all over a few megabytes: with 50,000
 * songs most of a shuffle's 1.9 s of ordering on the H2. Keys laid side by
 * side and counted into place are several times quicker; qsort stays for
 * the small sorts and when there's no memory for the keys. */
typedef struct { uint64_t key; uint32_t v; } kv_t;

static void radix_sort(kv_t *a, kv_t *tmp, size_t n)
{
    uint64_t any = 0;
    for (size_t i = 0; i < n; i++) any |= a[i].key;
    kv_t *from = a, *to = tmp;
    for (int shift = 0; shift < 64 && (any >> shift); shift += 11) {
        uint32_t count[2048] = {0};
        for (size_t i = 0; i < n; i++) count[(from[i].key >> shift) & 2047]++;
        uint32_t at = 0;
        for (int d = 0; d < 2048; d++) { uint32_t c = count[d]; count[d] = at; at += c; }
        for (size_t i = 0; i < n; i++) to[count[(from[i].key >> shift) & 2047]++] = from[i];
        kv_t *sw = from; from = to; to = sw;
    }
    if (from != a) memcpy(a, from, n * sizeof *a);
}

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : (x > y);
}

/* ---- scoring ---- */

static fx_t freshness_of(const shuf_track_t *t, int64_t now)
{
    if (t->last_played <= 0) return FX_ONE;
    int64_t secs = now - t->last_played;
    if (secs <= 0) return 0;
    return secs >= FRESH_SECONDS ? FX_ONE : (secs << 32) / FRESH_SECONDS;
}

static fx_t skip_rate_of(const shuf_track_t *t)
{
    unsigned total = (unsigned)t->play_count + t->skip_count;
    return total ? ((fx_t)t->skip_count << 32) / total : 0;
}

/* x * w, w in Q16 */
static fx_t fx_w(fx_t x, int32_t w16) { return (x * w16) >> 16; }

/* Uniform in 0..span (span up to 1.0) */
static fx_t fx_rand(rng_t *r, fx_t span) { return (fx_t)(((uint64_t)span * rng_u32(r)) >> 32); }

/* Spread `count` items evenly on 0..1 with a random offset and small jitter. */
static fx_t spread_pos(rng_t *r, fx_t offset, uint32_t i, uint32_t count)
{
    fx_t step = FX_ONE / count;
    fx_t unit = (step * ((int64_t)(rng_u32(r) >> 8) - (1 << 23))) >> 23;   /* -1..1 of a step */
    return offset + (fx_t)i * step + unit * JITTER_PCT / 100;
}

/* Units required between two units of a group holding `group` of `total` units. */
static uint8_t gap_for(uint32_t total, uint32_t group, uint32_t cap)
{
    if (group == 0) return 0;
    uint32_t spacing = total / group;          /* ideal distance */
    uint32_t g = spacing > 0 ? (spacing - 1) / 2 : 0;  /* leave slack */
    return (uint8_t)(g < cap ? g : cap);
}

static int violates(const unit_t *u, const uint32_t *ord, uint32_t i, uint32_t cand,
                    uint32_t seam, uint32_t lookback)
{
    const unit_t *c = &u[cand];
    if (i < seam && c->tail) return 1;
    for (uint32_t d = 1; d <= lookback && d <= i; d++) {
        const unit_t *p = &u[ord[i - d]];
        if (p->artist == c->artist && d <= c->gap_artist) return 1;
        if (p->album == c->album && d <= c->gap_album) return 1;
    }
    return 0;
}

long shuf_build(const shuf_track_t *t, size_t n, const shuf_opts_t *o,
                uint64_t seed, const uint32_t *prev_tail, size_t prev_tail_n,
                uint32_t *out)
{
    if (n == 0) return 0;
    if (n > UINT32_MAX / 2) return -1;

    member_t *m = malloc(n * sizeof *m);
    unit_t *u = malloc(n * sizeof *u);
    uint32_t *ord = malloc(n * sizeof *ord);
    uint32_t *tail = NULL;
    if (prev_tail && prev_tail_n) tail = malloc(prev_tail_n * sizeof *tail);
    if (!m || !u || !ord || (prev_tail_n && prev_tail && !tail)) {
        free(m); free(u); free(ord); free(tail);
        return -1;
    }

    rng_t rng;
    rng_seed(&rng, seed, 0x46415448u /* "FATH" */);
    const int32_t w_fresh = (int32_t)(o->w_fresh * 65536.0), w_skip = (int32_t)(o->w_skip * 65536.0),
                  w_recent = (int32_t)(o->w_recent * 65536.0);

    /* 1. Eligible tracks -> members, grouped into units. */
    uint32_t nm = 0;
    for (uint32_t i = 0; i < n; i++) {
        const shuf_track_t *x = &t[i];
        int grouped = (o->mode == SHUF_MODE_ALBUMS) || (o->keep_units && x->unit != 0);
        if (x->flags & SHUF_F_NEVER) continue;
        if (o->exclude_spoken && (x->flags & SHUF_F_SPOKEN) && (int64_t)i != o->first) continue;   /* (one picked plays) */
        if (o->skip_interludes && !grouped && x->duration_ms > 0 &&
            x->duration_ms < o->interlude_ms && (int64_t)i != o->first)
            continue;
        member_t *mm = &m[nm++];
        mm->kind = grouped ? 1 : 0;
        mm->key = grouped ? (o->mode == SHUF_MODE_ALBUMS ? x->album : x->unit) : i;
        mm->disc = x->disc;
        mm->track = x->track;
        mm->idx = i;
    }
    if (nm == 0) {
        free(m); free(u); free(ord); free(tail);
        return 0;
    }
    /* (grouped members by album or unit, disc, track; standalone ones each
     * their own: the same order cmp_member gives, keys side by side) */
    kv_t *kv = malloc(2 * (size_t)nm * sizeof *kv), *kv2 = kv ? kv + nm : NULL;
    member_t *ms = kv ? malloc((size_t)nm * sizeof *ms) : NULL;
    if (kv && ms) {
        int fits = 1;
        for (uint32_t i = 0; i < nm; i++) {
            fits &= m[i].key < 0x80000000u;
            kv[i].key = m[i].kind ? (1ULL << 63) | ((uint64_t)m[i].key << 32) | ((uint64_t)m[i].disc << 16) | m[i].track
                                  : (uint64_t)m[i].key;
            kv[i].v = i;
        }
        if (fits) {
            radix_sort(kv, kv2, nm);
            for (uint32_t i = 0; i < nm; i++) ms[i] = m[kv[i].v];
            memcpy(m, ms, (size_t)nm * sizeof *m);
        } else qsort(m, nm, sizeof *m, cmp_member);
    } else qsort(m, nm, sizeof *m, cmp_member);
    free(ms);

    if (tail) {
        memcpy(tail, prev_tail, prev_tail_n * sizeof *tail);
        qsort(tail, prev_tail_n, sizeof *tail, cmp_u32);
    }

    uint32_t nu = 0;
    for (uint32_t i = 0; i < nm; i++) {
        if (i == 0 || m[i].kind == 0 || m[i].kind != m[i - 1].kind || m[i].key != m[i - 1].key) {
            unit_t *x = &u[nu++];
            memset(x, 0, sizeof *x);
            x->first = i;
            x->artist = t[m[i].idx].artist;
            x->album = t[m[i].idx].album;
        }
        u[nu - 1].count++;
    }

    /* 2. Scores (random + freshness - skips) and tail marks. */
    int32_t first_unit = -1;
    for (uint32_t k = 0; k < nu; k++) {
        fx_t fresh = 0, skip = 0;
        for (uint32_t j = 0; j < u[k].count; j++) {
            const shuf_track_t *x = &t[m[u[k].first + j].idx];
            fresh += freshness_of(x, o->now);
            skip += skip_rate_of(x);
            if (tail && bsearch(&x->id, tail, prev_tail_n, sizeof *tail, cmp_u32))
                u[k].tail = 1;
            if ((int64_t)m[u[k].first + j].idx == o->first)
                first_unit = (int32_t)k;
        }
        u[k].fresh = fresh / u[k].count;
        u[k].score = (fx_t)rng_u32(&rng);
        if (o->freshness)
            u[k].score += fx_w(u[k].fresh, w_fresh) - fx_w(skip / u[k].count, w_skip);
        ord[k] = k;
    }

    /* 3. Within each artist, spread albums; then spread artists over the cycle. */
    g_units = u;
    /* by artist, album, then best score first (as cmp_artist_album_score):
     * by score, then, keeping that order, by artist and album */
    if (kv && nu <= nm) {
        for (uint32_t k = 0; k < nu; k++) {
            kv[k].key = (uint64_t)((FX_ONE << 4) - u[k].score);   /* (higher scores first; scores lie well within +-16) */
            kv[k].v = k;
        }
        radix_sort(kv, kv2, nu);
        for (uint32_t k = 0; k < nu; k++) kv[k].key = (uint64_t)u[kv[k].v].artist << 32 | u[kv[k].v].album;
        radix_sort(kv, kv2, nu);
        for (uint32_t k = 0; k < nu; k++) ord[k] = kv[k].v;
    } else qsort(ord, nu, sizeof *ord, cmp_artist_album_score);
    for (uint32_t a = 0; a < nu;) {
        uint32_t a_end = a;
        while (a_end < nu && u[ord[a_end]].artist == u[ord[a]].artist) a_end++;
        for (uint32_t b = a; b < a_end;) {
            uint32_t b_end = b;
            while (b_end < a_end && u[ord[b_end]].album == u[ord[b]].album) b_end++;
            uint32_t cnt = b_end - b;
            uint8_t gap = gap_for(nu, cnt, o->max_album_gap);
            fx_t off = fx_rand(&rng, FX_ONE / cnt);
            for (uint32_t i = 0; i < cnt; i++) {  /* best score first */
                u[ord[b + i]].pos = spread_pos(&rng, off, i, cnt);
                u[ord[b + i]].gap_album = gap;
            }
            b = b_end;
        }
        qsort(ord + a, a_end - a, sizeof *ord, cmp_pos);
        uint32_t cnt = a_end - a;
        uint8_t gap = gap_for(nu, cnt, o->max_artist_gap);
        fx_t off = fx_rand(&rng, FX_ONE / cnt);
        for (uint32_t i = 0; i < cnt; i++) {
            unit_t *x = &u[ord[a + i]];
            x->pos = spread_pos(&rng, off, i, cnt);
            x->gap_artist = gap;
            /* recently heard songs move later in the whole cycle; the repair pass re-spaces */
            if (o->freshness)
                x->pos += fx_w(FX_ONE - x->fresh, w_recent);
        }
        a = a_end;
    }
    /* along the 0..1 line (positions run from a little below 0 to under 3) */
    if (kv && nu <= nm) {
        for (uint32_t k = 0; k < nu; k++) {
            kv[k].key = (uint64_t)(u[ord[k]].pos + (FX_ONE << 4));
            kv[k].v = ord[k];
        }
        radix_sort(kv, kv2, nu);
        for (uint32_t k = 0; k < nu; k++) ord[k] = kv[k].v;
    } else qsort(ord, nu, sizeof *ord, cmp_pos);
    free(kv);
    kv = NULL;

    /* 4. A requested first track goes to the front. */
    uint32_t fixed = 0;
    if (first_unit >= 0) {
        uint32_t p = 0;
        while (ord[p] != (uint32_t)first_unit) p++;
        memmove(ord + 1, ord, p * sizeof *ord);
        ord[0] = (uint32_t)first_unit;
        u[first_unit].tail = 0;
        fixed = 1;
    }

    /* 5. Seam: no unit from the previous cycle's tail in the first `seam` positions. */
    uint32_t seam = o->seam;
    if (seam > nu / 3) seam = nu / 3;
    if (tail && seam > 0) {
        uint32_t next = seam;
        for (uint32_t i = fixed; i < seam; i++) {
            if (!u[ord[i]].tail) continue;
            while (next < nu && u[ord[next]].tail) next++;
            if (next >= nu) break;
            uint32_t tmp = ord[i]; ord[i] = ord[next]; ord[next] = tmp;
            next++;
        }
    }

    /* 6. Repair: enforce minimum artist/album gaps with forward swaps. */
    uint32_t lookback = o->max_artist_gap > o->max_album_gap ? o->max_artist_gap : o->max_album_gap;
    for (uint32_t i = fixed; i < nu; i++) {
        if (!violates(u, ord, i, ord[i], seam, lookback)) continue;
        uint32_t lim = i + o->repair_window;
        if (lim >= nu) lim = nu - 1;
        for (uint32_t k = i + 1; k <= lim; k++) {
            if (!violates(u, ord, i, ord[k], seam, lookback)) {
                uint32_t tmp = ord[i]; ord[i] = ord[k]; ord[k] = tmp;
                break;
            }
        }
    }

    /* 7. Expand units into track indices. The first unit, if it holds the
     * requested track, from that track on (an album shuffled from its song 7:
     * 7, 8... then the next album, as an album started part way; its 1-6
     * wait for the next cycle). It began with the unit's first track, not
     * the one chosen, and turning shuffle on mid-song played 2 next. */
    long w = 0;
    for (uint32_t i = 0; i < nu; i++) {
        const unit_t *x = &u[ord[i]];
        uint32_t j0 = 0;
        if (i == 0 && fixed)
            for (uint32_t j = 0; j < x->count; j++)
                if ((int64_t)m[x->first + j].idx == o->first) { j0 = j; break; }
        for (uint32_t j = j0; j < x->count; j++)
            out[w++] = m[x->first + j].idx;
    }

    g_units = NULL;
    free(m); free(u); free(ord); free(tail);
    return w;
}
