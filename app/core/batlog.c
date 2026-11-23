/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* Battery history: the log and what it says (batlog.h). */
#include "core/batlog.h"
#include "core/fsafe.h"

#include <unistd.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static const char MAGIC[8] = {'F', 'B', 'A', 'T', 'L', 'O', 'G', '1'};

/* ---------- the file: 8 bytes a record, little-endian ---------- */

static void enc(const batrec_t *r, unsigned char *p)
{
    uint32_t t = (uint32_t)r->t;
    p[0] = (unsigned char)t, p[1] = (unsigned char)(t >> 8), p[2] = (unsigned char)(t >> 16), p[3] = (unsigned char)(t >> 24);
    p[4] = r->pct, p[5] = r->flags, p[6] = r->kind, p[7] = 0;
}

static void dec(const unsigned char *p, batrec_t *r)
{
    r->t = (int64_t)((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24);
    r->pct = p[4], r->flags = p[5], r->kind = p[6], r->known = 0;
}

/* A record, not a header written again or anything else: 0.9.0 and before
 * wrote the header at every flush on the H2 (in append mode, musl's ftell()
 * before the first write says 0), and each read back as a sample dated 2014.
 * Such files are read as if they had none, and trimmed without them. */
static int ours(const unsigned char *p)
{
    return memcmp(p, MAGIC, 8) && p[6] <= BL_CLOCK;
}

/* The file's records into *out (malloc'd): the count, 0 for none, -1 not ours */
static int read_file(const char *path, batrec_t **out)
{
    *out = NULL;
    FILE *f = fopen(path, "rbe");
    if (!f) return 0;
    unsigned char h[8];
    if (fread(h, 1, 8, f) != 8 || memcmp(h, MAGIC, 8)) { fclose(f); return -1; }
    struct stat st;
    long n = fstat(fileno(f), &st) == 0 ? (long)(st.st_size - 8) / 8 : 0;
    batrec_t *r = n > 0 ? malloc((size_t)n * sizeof *r) : NULL;
    long k = 0;
    unsigned char b[8];
    while (r && k < n && fread(b, 1, 8, f) == 8)
        if (ours(b)) dec(b, &r[k++]);
    fclose(f);
    *out = r;
    return (int)k;
}

/* Past BATLOG_TRIM_BYTES: the last BATLOG_KEEP_DAYS, and never more than
 * three quarters of the limit (a heavy user's 60 days can be megabytes; a
 * trim that kept them all grew without end). Read and copied in passes, not
 * loaded: a file grown big before this must not cost its size
 * in RAM. A cut inside a start gets a start record of its own, holding the
 * base then in effect (its start's, or a clock set since), so the analysis
 * moves the records kept as it did before. */
static void trim(const char *path, int64_t now)
{
    struct stat st;
    if (stat(path, &st) || st.st_size <= BATLOG_TRIM_BYTES) return;
    FILE *f = fopen(path, "rbe");
    if (!f) return;
    unsigned char h[8], b[8];
    if (fread(h, 1, 8, f) != 8 || memcmp(h, MAGIC, 8)) { fclose(f); remove(path); return; }   /* (not ours: started again) */
    long n = (long)(st.st_size - 8) / 8;
    long keep = BATLOG_TRIM_BYTES * 3 / 4 / 8;
    long lo = n - keep > n / 4 ? n - keep : n / 4;         /* (a quarter at least goes) */
    int64_t cut = now - (int64_t)BATLOG_KEEP_DAYS * 86400;
    /* the first start or sample past lo that's recent (or in the last
     * quarter: an H2 unused for 60 days keeps something) */
    long from = n;
    int64_t base = 0;
    int have_base = 0, at_boot = 0;
    for (long i = 0; i < n && fread(b, 1, 8, f) == 8; i++) {
        batrec_t r;
        dec(b, &r);
        if (i >= lo && (r.kind == BL_BOOT || r.kind == BL_SAMPLE) && (r.t >= cut || i >= n - n / 4)) {
            from = i, at_boot = r.kind == BL_BOOT;
            break;
        }
        if (r.kind == BL_BOOT || r.kind == BL_CLOCK) base = r.t, have_base = r.kind == BL_BOOT || have_base;
    }
    char tmp[600];
    snprintf(tmp, sizeof tmp, "%s.new", path);
    FILE *o = fseek(f, 8 + from * 8, SEEK_SET) == 0 ? fopen(tmp, "wbe") : NULL;
    if (!o) { fclose(f); return; }
    int ok = fwrite(MAGIC, 1, 8, o) == 8;
    if (ok && from < n && !at_boot && have_base) {
        batrec_t s = {.t = base, .kind = BL_BOOT};
        enc(&s, b);
        ok = fwrite(b, 1, 8, o) == 8;
    }
    unsigned char blk[4096];
    size_t got;
    while (ok && (got = fread(blk, 1, sizeof blk, f)) > 0) {
        size_t w = 0;
        for (size_t i = 0; i + 8 <= got; i += 8)      /* (no stray headers kept) */
            if (ours(blk + i)) memmove(blk + w, blk + i, 8), w += 8;
        ok = fwrite(blk, 1, w, o) == w;
    }
    ok = !ferror(f) && ok;
    fclose(f);
    int flushed = fflush(o) == 0 && fsync(fileno(o)) == 0;
    if (fclose(o) == 0 && flushed && ok) file_replace(tmp, path);
    else remove(tmp);
}

/* ---------- the log ---------- */

static void put(batlog_t *b, int64_t t, int pct, int flags, int kind)
{
    if (b->n == BATLOG_BUF) batlog_flush(b);
    if (b->n == BATLOG_BUF) {                         /* the card's away a long time: the oldest go */
        memmove(b->buf, b->buf + 1, (BATLOG_BUF - 1) * sizeof *b->buf);
        b->n--;
    }
    batrec_t *r = &b->buf[b->n++];
    r->t = t, r->pct = (uint8_t)pct, r->flags = (uint8_t)flags, r->kind = (uint8_t)kind, r->known = 0;
}

void batlog_open(batlog_t *b, const char *path, int64_t wall_now, int64_t mono_now)
{
    memset(b, 0, sizeof *b);
    snprintf(b->path, sizeof b->path, "%s", path);
    b->base = wall_now - mono_now;
    b->pct = b->flags = -1;
    b->mono = b->flushed = mono_now;
    trim(path, wall_now);
    put(b, b->base, 0, 0, BL_BOOT);
}

void batlog_note(batlog_t *b, int64_t mono_now, int pct, int flags)
{
    if (pct < 0 || pct > 100) return;
    b->mono = mono_now;
    int was = b->flags;
    if (pct != b->pct || flags != b->flags) {
        put(b, b->base + mono_now, pct, flags, BL_SAMPLE);
        b->pct = pct, b->flags = flags;
    }
    int charging_changed = was >= 0 && ((was ^ flags) & BL_CHARGING);
    if (b->n && (charging_changed || mono_now - b->flushed >= 600)) batlog_flush(b);
}

void batlog_clock_set(batlog_t *b, int64_t wall_now, int64_t mono_now)
{
    int64_t base = wall_now - mono_now;
    if (base - b->base < 3 && b->base - base < 3) return;   /* (as it was) */
    b->base = base;
    b->mono = mono_now;
    put(b, base, 0, 0, BL_CLOCK);
}

void batlog_off(batlog_t *b, int64_t mono_now)
{
    b->mono = mono_now;
    if (b->pct >= 0) put(b, b->base + mono_now, b->pct, b->flags, BL_OFF);
    batlog_flush(b);
}

int batlog_flush(batlog_t *b)
{
    if (!b->n) return 0;
    FILE *f = fopen(b->path, "abe");
    if (!f) return -1;
    /* (the file's size, not ftell(): in append mode that's 0 before the
     * first write on musl, and a header went in at every flush) */
    struct stat st;
    int ok = fstat(fileno(f), &st) == 0;
    long size = ok ? (long)st.st_size : 0;
    if (ok && size == 0) ok = fwrite(MAGIC, 1, 8, f) == 8, size = 8;
    unsigned char e[8];
    for (int i = 0; ok && i < b->n; i++) { enc(&b->buf[i], e); ok = fwrite(e, 1, 8, f) == 8; size += 8; }
    if (fclose(f) || !ok) return -1;
    b->n = 0;
    b->flushed = b->mono;
    if (size > BATLOG_TRIM_BYTES) trim(b->path, b->base + b->mono);   /* (a start of weeks: not only at the next) */
    return 0;
}

/* ---------- loading: times corrected, in order ---------- */

int batlog_load(const char *path, const batlog_t *live, int64_t mono_now, batrec_t **out)
{
    batrec_t *f;
    int nf = read_file(path, &f);
    if (nf < 0) nf = 0;
    int extra = live ? live->n + 1 : 0;
    batrec_t *r = malloc((size_t)(nf + extra + 1) * sizeof *r);
    if (!r) { free(f); *out = NULL; return -1; }
    int n = 0;
    for (int i = 0; i < nf; i++) r[n++] = f[i];
    free(f);
    if (live) {
        for (int i = 0; i < live->n; i++) r[n++] = live->buf[i];
        if (live->pct >= 0) {                         /* now */
            batrec_t *x = &r[n++];
            x->t = live->base + mono_now, x->pct = (uint8_t)live->pct, x->flags = (uint8_t)live->flags;
            x->kind = BL_SAMPLE, x->known = 0;
        }
    }
    /* Each start's records, moved to its last known base (a CLOCK's); then
     * kept in order: a start whose clock was never set begins where the
     * last one ended at the earliest. */
    int64_t prev_end = INT64_MIN, prev_base = INT64_MIN;
    int i = 0, w = 0;
    while (i < n) {
        int s = i, e = i + 1;
        while (e < n && r[e].kind != BL_BOOT) e++;
        int64_t start_base = r[s].kind == BL_BOOT ? r[s].t : 0, final_base = start_base;
        int corrected = 0;
        for (int k = s; k < e; k++) if (r[k].kind == BL_CLOCK) final_base = r[k].t, corrected = 1;
        int64_t in_effect = start_base;
        int first_w = w;
        for (int k = s; k < e; k++) {
            if (r[k].kind == BL_CLOCK) { in_effect = r[k].t; continue; }
            batrec_t x = r[k];
            if (r[s].kind == BL_BOOT) x.t = x.t - in_effect + final_base;
            r[w++] = x;
        }
        /* (a start's record holds the base, the clock at monotonic 0: from here
         * on, when it started, its first sample's time) */
        if (r[s].kind == BL_BOOT && first_w + 1 < w) r[first_w].t = r[first_w + 1].t;
        if (r[s].kind == BL_BOOT && first_w < w) {
            /* known: the clock was set in this start, or it's the same start of
             * the H2 as the last (the player started again: the same base) */
            int same = prev_base != INT64_MIN && start_base - prev_base < 3 && prev_base - start_base < 3;
            r[first_w].known = same ? BL_KNOWN_RESTART : corrected ? BL_KNOWN_CLOCK : 0;
            if (prev_end != INT64_MIN && r[first_w].t < prev_end) {
                int64_t d = prev_end - r[first_w].t;
                for (int k = first_w; k < w; k++) r[k].t += d;
                if (!same) r[first_w].known = 0;      /* (moved: how long it was off isn't known after all) */
            }
        }
        if (w > first_w) prev_end = r[w - 1].t;
        prev_base = final_base;
        i = e;
    }
    /* a start's own record says nothing of the charge: the next one's */
    for (int k = 0; k < w; k++)
        if (r[k].kind == BL_BOOT) {
            int j = k + 1;
            while (j < w && r[j].kind != BL_SAMPLE && r[j].kind != BL_OFF) j++;
            if (j < w && r[j].kind == BL_SAMPLE) r[k].pct = r[j].pct, r[k].flags = r[j].flags;
            else r[k].pct = k ? r[k - 1].pct : 0, r[k].flags = k ? r[k - 1].flags : 0;
        }
    *out = r;
    return w;
}

/* ---------- what it says ---------- */

static int act_of(int flags)
{
    return flags & BL_SCREEN ? BL_ACT_SCREEN : flags & BL_PLAYING ? BL_ACT_PLAYING : BL_ACT_PAUSED;
}

/* Least squares for each activity's rate (percent an hour) from the drops and
 * the time each activity had before each one, none below zero */
static void rates(const batspan_t *s, double rate[BL_ACTS])
{
    int on[BL_ACTS];
    for (int a = 0; a < BL_ACTS; a++) { on[a] = s->act_s[a] > 0; rate[a] = 0; }
    for (int pass = 0; pass < BL_ACTS; pass++) {
        int idx[BL_ACTS], m = 0;
        for (int a = 0; a < BL_ACTS; a++) if (on[a]) idx[m++] = a;
        if (!m) return;
        double A[BL_ACTS][BL_ACTS + 1];
        for (int i = 0; i < m; i++) {
            for (int j = 0; j < m; j++) A[i][j] = s->ata[idx[i]][idx[j]];
            A[i][m] = s->atb[idx[i]];
        }
        int ok = 1;
        for (int c = 0; c < m && ok; c++) {          /* Gauss with partial pivoting */
            int p = c;
            for (int r = c + 1; r < m; r++) if (fabs(A[r][c]) > fabs(A[p][c])) p = r;
            if (fabs(A[p][c]) < 1e-9) { ok = 0; break; }
            for (int k = 0; k <= m; k++) { double t = A[c][k]; A[c][k] = A[p][k]; A[p][k] = t; }
            for (int r = 0; r < m; r++) {
                if (r == c) continue;
                double f = A[r][c] / A[c][c];
                for (int k = c; k <= m; k++) A[r][k] -= f * A[c][k];
            }
        }
        if (!ok) return;
        int worst = -1;
        for (int i = 0; i < m; i++) {
            rate[idx[i]] = A[i][m] / A[i][i];
            if (rate[idx[i]] < 0 && (worst < 0 || rate[idx[i]] < rate[idx[worst]])) worst = i;
        }
        if (worst < 0) return;
        on[idx[worst]] = 0;
        rate[idx[worst]] = 0;
    }
}

static void finish(batspan_t *s)
{
    double rate[BL_ACTS];
    rates(s, rate);
    for (int a = 0; a < BL_ACTS; a++) {
        int64_t need = a == BL_ACT_SCREEN ? 900 : 1800;   /* (enough of it to say) */
        s->min_per_pct[a] = s->act_s[a] >= need && rate[a] > 0.05 ? (int)lround(60.0 / rate[a]) : 0;
        if (s->min_per_pct[a] > 999) s->min_per_pct[a] = 0;
    }
    s->full_h10 = s->used >= 5 && s->on_s >= 1800 ? (int)lround(s->on_s / 360.0 * 100.0 / s->used) : 0;
}

static void add_drop(batspan_t *s, const double seg[BL_ACTS], int drop)
{
    double h[BL_ACTS];
    for (int a = 0; a < BL_ACTS; a++) h[a] = seg[a] / 3600.0;
    for (int a = 0; a < BL_ACTS; a++) {
        for (int c = 0; c < BL_ACTS; c++) s->ata[a][c] += h[a] * h[c];
        s->atb[a] += h[a] * drop;
    }
}

void batlog_report(const batrec_t *r, int n, int days, batrep_t *rep)
{
    memset(rep, 0, sizeof *rep);
    rep->days = days == 7 ? 7 : 1;
    if (n <= 0) return;
    rep->t1 = r[n - 1].t;
    rep->t0 = rep->t1 - (int64_t)rep->days * 86400;
    rep->clock_ok = rep->t1 > 1700000000;

    /* the graph: each column's charge at its end */
    int64_t span_t = rep->t1 - rep->t0;
    int i = -1;
    for (int c = 0; c < BATREP_COLS; c++) {
        int64_t te = rep->t0 + span_t * (c + 1) / BATREP_COLS;
        while (i + 1 < n && r[i + 1].t <= te) i++;
        int off = i < 0 || r[i].kind == BL_OFF;     /* (before the log, or switched off) */
        rep->pct[c] = off ? -1 : (int8_t)r[i].pct;
        rep->charging[c] = !off && (r[i].flags & BL_CHARGING);
    }
    for (int k = 1; k < n && rep->ngaps < BATREP_GAPS; k++)
        if (r[k].kind == BL_BOOT && r[k].known != BL_KNOWN_RESTART && (!r[k].known || r[k - 1].kind != BL_OFF) &&
            r[k].t >= rep->t0 && r[k].t <= rep->t1) {       /* (off for a known time: its columns are empty) */
            int c = (int)((r[k].t - rep->t0) * BATREP_COLS / (span_t ? span_t : 1));
            rep->gap[rep->ngaps++] = c >= BATREP_COLS ? BATREP_COLS - 1 : c;
        }

    /* spans on battery, oldest first, into a list */
    batspan_t *all = calloc((size_t)n + 1, sizeof *all);
    if (!all) return;
    int ns = 0, open = 0, prev = r[0].pct;
    batspan_t cur;
    double seg[BL_ACTS] = {0};
    if (!(r[0].flags & BL_CHARGING)) {
        memset(&cur, 0, sizeof cur);
        cur.start = r[0].t, cur.pct_start = r[0].pct, cur.from_full = r[0].pct >= 95;
        open = 1;
    }
    for (int k = 0; k + 1 < n; k++) {
        const batrec_t *a = &r[k], *b = &r[k + 1];
        /* not counted: switched off, or a start after one whose length isn't
         * known (a player restart is: the H2 stayed on) */
        int gap = a->kind == BL_OFF || (b->kind == BL_BOOT && b->known != BL_KNOWN_RESTART);
        int a_chg = a->flags & BL_CHARGING, b_chg = b->flags & BL_CHARGING;
        if (open && !a_chg && !gap) {
            int64_t dt = b->t - a->t;
            if (dt > 0) {
                cur.on_s += dt;
                cur.act_s[act_of(a->flags)] += dt;
                seg[act_of(a->flags)] += (double)dt;
            }
            if (b->pct < prev) {
                cur.used += prev - b->pct;
                add_drop(&cur, seg, prev - b->pct);
                memset(seg, 0, sizeof seg);
            }
        }
        if (gap) memset(seg, 0, sizeof seg);
        if (open && b_chg) {                          /* plugged in */
            cur.end = gap ? a->t : b->t, cur.pct_end = gap ? a->pct : b->pct;
            all[ns++] = cur;
            open = 0;
        } else if (!open && !b_chg) {                 /* unplugged */
            memset(&cur, 0, sizeof cur);
            cur.start = b->t, cur.pct_start = b->pct, cur.from_full = b->pct >= 95;
            memset(seg, 0, sizeof seg);
            open = 1;
        }
        prev = b->pct;
    }
    if (open) {                                       /* the one going on now */
        cur.end = r[n - 1].t, cur.pct_end = r[n - 1].pct;
        all[ns++] = cur;
    }
    for (int k = 0; k < ns; k++) finish(&all[k]);

    /* since unplugged at 95 % or more: that span and every one since */
    int from = -1;
    for (int k = ns - 1; k >= 0; k--) if (all[k].from_full) { from = k; break; }
    if (from >= 0) {
        batspan_t s = all[from];
        for (int k = from + 1; k < ns; k++) {
            const batspan_t *x = &all[k];
            s.on_s += x->on_s, s.used += x->used;
            for (int a = 0; a < BL_ACTS; a++) {
                s.act_s[a] += x->act_s[a], s.atb[a] += x->atb[a];
                for (int c = 0; c < BL_ACTS; c++) s.ata[a][c] += x->ata[a][c];
            }
            s.end = x->end, s.pct_end = x->pct_end;
        }
        finish(&s);
        rep->since = s;
        rep->have_since = 1;
    }

    /* the list: newest first, what's worth a line */
    int sum = 0, cnt = 0;
    for (int k = ns - 1; k >= 0; k--) {
        const batspan_t *x = &all[k];
        if (x->on_s < 600 && x->used < 2) continue;
        rep->total_spans++;
        if (rep->nspans < BATREP_SPANS) rep->span[rep->nspans++] = *x;
        if (x->used >= 20 && x->full_h10 > 0) sum += x->full_h10, cnt++;
    }
    rep->avg_full_h10 = cnt ? (sum + cnt / 2) / cnt : 0;
    free(all);
}
