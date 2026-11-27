/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/*
 * Battery history (app/core/batlog.c), driven as the player drives it: a
 * battery drained at known rates for each activity, noted every 10 s, the
 * log written, the H2 switched off and on, the clock set by Wi-Fi.
 */
#define _DEFAULT_SOURCE
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "core/batlog.h"

/* batlog.c itself, not the library's copy of it, so that a read can be made
 * to fail here (g_read_fails): its fread is the one below */
static size_t test_fread(void *p, size_t size, size_t n, FILE *f);
#define fread test_fread
#include "../app/core/batlog.c"
#include "test_util.h"
#undef fread
static int g_read_fails;
static size_t test_fread(void *p, size_t size, size_t n, FILE *f) { return g_read_fails ? 0 : fread(p, size, n, f); }

static char g_path[600];

/* A battery and an H2: the wall clock (as the H2 has it) and the monotonic one */
typedef struct {
    batlog_t log;
    int64_t mono, wall_true;     /* (the true time, for setting the clock) */
    double level;                /* percent, exactly */
} h2_t;

static const double RATE[BL_ACTS] = {4.0, 12.0, 1.5};   /* percent an hour: playing (screen off), screen on, paused */

static int flags_of(int act, int charging)
{
    int f = charging ? BL_CHARGING : 0;
    if (act == BL_ACT_SCREEN) f |= BL_SCREEN | BL_PLAYING;
    else if (act == BL_ACT_PLAYING) f |= BL_PLAYING;
    return f | BL_OUT_WIRED << BL_OUT_SHIFT;
}

static int pct_of(double level) { return (int)ceil(level - 1e-9); }

static void start(h2_t *h, int64_t wall_as_h2_has_it)
{
    h->mono = 8;                 /* (the player starts a few seconds in) */
    batlog_open(&h->log, g_path, wall_as_h2_has_it, h->mono);
}

/* `secs` of one activity on battery (or charging at 30 %/h) */
static void run(h2_t *h, int secs, int act, int charging)
{
    for (int s = 0; s < secs; s += 10) {
        h->mono += 10, h->wall_true += 10;
        h->level += charging ? 30.0 * 10 / 3600 : -RATE[act] * 10 / 3600;
        if (h->level > 100) h->level = 100;
        if (h->level < 0) h->level = 0;
        batlog_note(&h->log, h->mono, pct_of(h->level), flags_of(act, charging));
    }
}

static void report(h2_t *h, int days, batrep_t *rep)
{
    batrec_t *r;
    int n = batlog_load(g_path, &h->log, h->mono, &r);
    CHECK(n > 0);
    batlog_report(r, n, days, rep);
    free(r);
}

static int near(int got, double want, double tol) { return got > 0 && fabs(got - want) <= want * tol; }
/* (the first sample on battery comes 10 s after unplugging, or starting) */
static int secs(int64_t got, int64_t want) { return got <= want && got >= want - 20; }

static void test_rates(void)
{
    unlink(g_path);
    h2_t h = {.wall_true = 1795000000, .level = 100};
    start(&h, h.wall_true);
    run(&h, 600, BL_ACT_PAUSED, 1);                   /* full, on the charger */
    /* a day's mix, in 20-minute turns: 3 h playing, 1 h screen on, 2 h paused */
    static const int turns[] = {BL_ACT_PLAYING, BL_ACT_SCREEN, BL_ACT_PLAYING, BL_ACT_PAUSED, BL_ACT_PLAYING,
                                BL_ACT_PAUSED, BL_ACT_PLAYING, BL_ACT_SCREEN, BL_ACT_PAUSED, BL_ACT_PLAYING,
                                BL_ACT_PAUSED, BL_ACT_PLAYING, BL_ACT_SCREEN, BL_ACT_PAUSED, BL_ACT_PLAYING,
                                BL_ACT_PAUSED, BL_ACT_PLAYING, BL_ACT_PLAYING};
    for (size_t i = 0; i < sizeof turns / sizeof turns[0]; i++) run(&h, 1200, turns[i], 0);
    batrep_t rep;
    report(&h, 1, &rep);
    const batspan_t *s = &rep.since;
    printf("rates: on %lld s, used %d %%, a full charge %.1f h; a percent: playing %d min, screen %d min, paused %d min\n",
           (long long)s->on_s, s->used, s->full_h10 / 10.0, s->min_per_pct[0], s->min_per_pct[1], s->min_per_pct[2]);
    CHECK(rep.have_since && s->from_full);
    CHECK(secs(s->on_s, 6 * 3600));
    CHECK(abs(s->used - 27) <= 1);                    /* 3 h x 4 + 1 h x 12 + 2 h x 1.5 */
    CHECK(near(s->min_per_pct[BL_ACT_PLAYING], 15, 0.2));
    CHECK(near(s->min_per_pct[BL_ACT_SCREEN], 5, 0.2));
    CHECK(near(s->min_per_pct[BL_ACT_PAUSED], 40, 0.3));
    CHECK(s->act_s[BL_ACT_SCREEN] == 3600);
    /* the graph: full at first, charging, then down to about 68 now */
    CHECK(rep.pct[BATREP_COLS - 1] == pct_of(h.level));
    int charging_cols = 0;
    for (int c = 0; c < BATREP_COLS; c++) charging_cols += rep.charging[c];
    CHECK(charging_cols > 0);
    CHECK(rep.ngaps == 0);
    CHECK(rep.nspans == 1 && rep.total_spans == 1);
    /* the same from the file alone, after switching off */
    batlog_off(&h.log, h.mono);
    batrec_t *r;
    int n = batlog_load(g_path, NULL, 0, &r);
    batrep_t again;
    batlog_report(r, n, 1, &again);
    free(r);
    CHECK(again.since.used == s->used && again.since.on_s == s->on_s);
}

static void test_off_and_clock(void)
{
    unlink(g_path);
    h2_t h = {.wall_true = 1795000000, .level = 100};
    start(&h, h.wall_true);
    run(&h, 600, BL_ACT_PAUSED, 1);
    run(&h, 3 * 3600, BL_ACT_PLAYING, 0);
    batlog_off(&h.log, h.mono);
    int64_t h2_clock = h.log.base + h.mono;           /* what it saves (clock.last) */
    /* off for 5 hours, losing a percent; on again, its clock where it stopped */
    h.wall_true += 5 * 3600;
    h.level -= 1;
    start(&h, h2_clock + 1);
    run(&h, 3600, BL_ACT_PLAYING, 0);
    batrep_t rep;
    report(&h, 1, &rep);
    printf("off, clock not set: on %lld s, used %d, gaps %d\n", (long long)rep.since.on_s, rep.since.used, rep.ngaps);
    CHECK(secs(rep.since.on_s, 4 * 3600));            /* the time off isn't counted */
    CHECK(rep.ngaps == 1);                            /* shown as a break */
    CHECK(rep.since.used == 16);                      /* 12 + 4 while on (the percent lost while off isn't) */
    /* Wi-Fi sets the clock: the second start moves 5 hours on */
    batlog_clock_set(&h.log, h.wall_true, h.mono);
    run(&h, 600, BL_ACT_PLAYING, 0);
    report(&h, 1, &rep);
    int empty = 0;
    for (int c = 0; c < BATREP_COLS; c++) empty += rep.pct[c] < 0;
    printf("clock set: %d of %d columns off (5 h of 24), gaps %d, now at %lld (true %lld)\n", empty, BATREP_COLS,
           rep.ngaps, (long long)rep.t1, (long long)h.wall_true);
    CHECK(rep.t1 == h.wall_true);
    CHECK(rep.ngaps == 0);
    /* off 5 h, and before the log 24 - 9 h 20 min: 19 h 40 min of 24 */
    CHECK(abs(empty - BATREP_COLS * 1180 / 1440) <= 3);
    CHECK(secs(rep.since.on_s, 4 * 3600 + 600));
}

static void test_restart_and_backwards(void)
{
    unlink(g_path);
    h2_t h = {.wall_true = 1795000000, .level = 90};
    start(&h, h.wall_true);
    run(&h, 1800, BL_ACT_PLAYING, 0);
    /* the player starts again (the H2 stays on): the same base, the time counted */
    batlog_flush(&h.log);
    int64_t mono = h.mono;
    batlog_open(&h.log, g_path, h.log.base + mono + 5, mono + 5);
    h.mono = mono + 5;
    run(&h, 1800, BL_ACT_PLAYING, 0);
    batrep_t rep;
    report(&h, 1, &rep);
    CHECK(rep.ngaps == 0);
    printf("restart: %d spans, the last %lld s on battery, gaps %d\n", rep.nspans, (long long)rep.span[0].on_s, rep.ngaps);
    CHECK(secs(rep.span[0].on_s, 3600 + 5));          /* (the restart's 5 s counted: the H2 was on) */
    /* a crash, then a start with the clock saved 5 minutes before it: never backwards */
    batlog_flush(&h.log);
    int64_t saved = h.log.base + h.mono - 300;
    start(&h, saved);
    run(&h, 600, BL_ACT_PLAYING, 0);
    batrec_t *r;
    int n = batlog_load(g_path, &h.log, h.mono, &r);
    int backwards = 0;
    for (int i = 1; i < n; i++) backwards += r[i].t < r[i - 1].t;
    free(r);
    CHECK(backwards == 0);
    report(&h, 1, &rep);
    CHECK(rep.ngaps == 1);
}

static void test_top_up_and_history(void)
{
    unlink(g_path);
    h2_t h = {.wall_true = 1795000000, .level = 100};
    start(&h, h.wall_true);
    run(&h, 300, BL_ACT_PAUSED, 1);
    run(&h, 12 * 3600, BL_ACT_PLAYING, 0);            /* 100 -> 52 */
    run(&h, 2400, BL_ACT_PAUSED, 1);                  /* topped up to 72 */
    run(&h, 3 * 3600, BL_ACT_PLAYING, 0);             /* -> 60 */
    batrep_t rep;
    report(&h, 7, &rep);
    printf("top-up: since full %d %% in %.1f h (from %d to %d), %d spans, average full %.1f h\n", rep.since.used,
           rep.since.on_s / 3600.0, rep.since.pct_start, rep.since.pct_end, rep.nspans, rep.avg_full_h10 / 10.0);
    CHECK(rep.have_since && rep.since.pct_start == 100 && rep.since.pct_end == pct_of(h.level));
    CHECK(secs(rep.since.on_s, 15 * 3600));
    CHECK(abs(rep.since.used - 60) <= 1);
    CHECK(rep.nspans == 2 && rep.span[0].pct_start < 95 && rep.span[1].from_full);
    CHECK(near(rep.avg_full_h10, 250, 0.05));         /* 4 %/h: 25 h from full */
    CHECK(near(rep.since.full_h10, 250, 0.05));
    CHECK(rep.days == 7);
}

/* 150 days of a busy log: a record every 3 minutes. The days kept aren't a
 * constant: the file is trimmed when it passes its limit (to 60 days, or less
 * when 60 don't fit), then grows until it passes again. So right after each
 * trim: 60 days at most; and never far past the limit. (This once checked the
 * age at day 150 alone, true only because a trim happened to come just
 * before: the file holds 51 to 68 days in turn.) */
static void test_trim(void)
{
    unlink(g_path);
    h2_t h = {.wall_true = 1790000000, .level = 100};
    long most = 0, last = 0;
    int trims = 0;
    for (int day = 0; day <= 150; day++) {
        for (int at = 0; at < 2; at++) {              /* after the start, after switching off: trimmed? */
            if (at == 0) start(&h, h.wall_true);
            else {
                for (int k = 0; k < 480; k++) {
                    h.mono += 180, h.wall_true += 180;
                    batlog_note(&h.log, h.mono, 50 + k % 40, flags_of(k % 3, 0));
                }
                batlog_off(&h.log, h.mono);
            }
            struct stat st;
            if (stat(g_path, &st)) continue;
            if (st.st_size < last) {
                trims++;
                batrec_t *r;
                int n = batlog_load(g_path, NULL, 0, &r);
                CHECK(n > 0 && r[0].kind == BL_BOOT && h.wall_true - r[0].t <= (int64_t)(BATLOG_KEEP_DAYS + 1) * 86400);
                CHECK(st.st_size <= BATLOG_TRIM_BYTES * 3 / 4 + 4096);
                if (day > 130) printf("trim: day %d, %ld bytes -> %ld, the oldest %.1f days back\n", day, last,
                                      (long)st.st_size, n > 0 ? (h.wall_true - r[0].t) / 86400.0 : -1.0);
                free(r);
            }
            last = (long)st.st_size, most = last > most ? last : most;
        }
        h.wall_true += 24 * 3600 - 480 * 180;
    }
    printf("trim: %d trims, at most %ld bytes\n", trims, most);
    CHECK(most <= BATLOG_TRIM_BYTES + 4096);          /* (trimmed at each start past it) */
    CHECK(trims >= 5);
}

/* Heavy use: past the limit within the 60 days, a record every 15 s for 16 h a
 * day. The trim used to keep every start inside the 60 days: nothing went. */
static void test_trim_heavy(void)
{
    unlink(g_path);
    h2_t h = {.wall_true = 1790000000, .level = 100};
    long most = 0;
    int64_t off = 0;
    for (int day = 0; day < 30; day++) {
        start(&h, h.wall_true);
        for (int k = 0; k < 16 * 240; k++) {
            h.mono += 15, h.wall_true += 15;
            batlog_note(&h.log, h.mono, 20 + k % 80, flags_of(k % 3, 0));
        }
        batlog_off(&h.log, h.mono);
        off = h.wall_true;
        h.wall_true += 8 * 3600;
        struct stat st;
        if (stat(g_path, &st) == 0 && st.st_size > most) most = (long)st.st_size;
    }
    start(&h, h.wall_true);
    batrec_t *r;
    int n = batlog_load(g_path, NULL, 0, &r);
    printf("trim, heavy use: at most %ld bytes, %d records, the oldest %.1f days back\n", most, n,
           n > 0 ? (h.wall_true - r[0].t) / 86400.0 : -1.0);
    CHECK(most <= BATLOG_TRIM_BYTES + BATLOG_BUF * 8 + 8);
    CHECK(n > BATLOG_TRIM_BYTES / 8 / 2 && r[0].kind == BL_BOOT);
    int ordered = 1;
    for (int i = 1; i < n; i++) ordered &= r[i].t >= r[i - 1].t;
    CHECK(ordered && r[n - 1].kind == BL_OFF && off - r[n - 1].t < 60);
    free(r);
}

/* One start of 12 days, its clock 40 days behind until Wi-Fi sets it on the
 * 11th: trimmed as it's written (no start to trim at), cut inside the start,
 * the records kept still moved to the true time. */
static void test_trim_one_start(void)
{
    unlink(g_path);
    h2_t h = {.wall_true = 1790000000, .level = 100};
    int64_t first = h.wall_true;
    start(&h, h.wall_true - 40 * 86400);
    long most = 0;
    for (int k = 0; k < 12 * 8640; k++) {
        h.mono += 10, h.wall_true += 10;
        if (k == 11 * 8640) batlog_clock_set(&h.log, h.wall_true, h.mono);
        batlog_note(&h.log, h.mono, 20 + k % 80, flags_of(k % 3, 0));
        struct stat st;
        if (k % 600 == 0 && stat(g_path, &st) == 0 && st.st_size > most) most = (long)st.st_size;
    }
    batlog_off(&h.log, h.mono);
    batrec_t *r;
    int n = batlog_load(g_path, NULL, 0, &r);
    printf("trim, one start: at most %ld bytes, %d records, from %.1f days in to %.0f s before now\n", most, n,
           n > 0 ? (r[0].t - first) / 86400.0 : -1.0, n > 0 ? (double)(h.wall_true - r[n - 1].t) : -1.0);
    CHECK(most <= BATLOG_TRIM_BYTES + BATLOG_BUF * 8 + 8);
    CHECK(n > BATLOG_TRIM_BYTES / 8 / 2 && r[0].kind == BL_BOOT);
    CHECK(r[0].t > first && r[0].t < h.wall_true - 86400);   /* (true times, not 40 days back) */
    int ordered = 1;
    for (int i = 1; i < n; i++) ordered &= r[i].t >= r[i - 1].t;
    CHECK(ordered && h.wall_true - r[n - 1].t < 60);
    free(r);
}

/* The file's headers in it: one, however many flushes */
static int headers(void)
{
    FILE *f = fopen(g_path, "rb");
    if (!f) return -1;
    unsigned char b[8];
    int n = 0;
    while (fread(b, 1, 8, f) == 8) n += !memcmp(b, "FBATLOG1", 8);
    fclose(f);
    return n;
}

/* The file again, a header after every `every` records (as 0.9.0 wrote it on
 * the H2, one at each flush), and an unknown record */
static void add_stray_headers(int every)
{
    FILE *f = fopen(g_path, "rb");
    struct stat st;
    if (!f || fstat(fileno(f), &st)) { CHECK(0); if (f) fclose(f); return; }
    unsigned char *d = malloc((size_t)st.st_size), *o = malloc((size_t)st.st_size * 2 + 16);
    size_t got = fread(d, 1, (size_t)st.st_size, f), w = 8;
    fclose(f);
    memcpy(o, d, 8);
    for (size_t i = 8, k = 0; i + 8 <= got; i += 8, k++) {
        if (k && k % (size_t)every == 0) memcpy(o + w, "FBATLOG1", 8), w += 8;
        memcpy(o + w, d + i, 8), w += 8;
    }
    memcpy(o + w, "\x10\x20\x30\x40\x32\x00\x09\x00", 8), w += 8;   /* (a kind not known) */
    f = fopen(g_path, "wb");
    CHECK(f && fwrite(o, 1, w, f) == w);
    if (f) fclose(f);
    free(d), free(o);
}

/* A night's playing, written at every 10 minutes: a header once; and a file
 * with stray headers reads as the same history, and loses them when trimmed */
static void test_stray_headers(void)
{
    unlink(g_path);
    h2_t h = {.wall_true = 1790000000, .level = 100};
    start(&h, h.wall_true);
    run(&h, 9 * 3600, BL_ACT_PLAYING, 0);
    batlog_off(&h.log, h.mono);
    CHECK(headers() == 1);
    batrec_t *r;
    int n = batlog_load(g_path, NULL, 0, &r);
    batrep_t a, b;
    batlog_report(r, n, 1, &a);
    free(r);
    CHECK(a.nspans == 1 && near(a.avg_full_h10, 250, 0.1));   /* (4 %/h: 25 h) */

    add_stray_headers(3);
    CHECK(headers() > 5);
    int m = batlog_load(g_path, NULL, 0, &r);
    batlog_report(r, m, 1, &b);
    free(r);
    CHECK(m == n && b.nspans == a.nspans && b.avg_full_h10 == a.avg_full_h10 && b.span[0].used == a.span[0].used);

    /* past the limit: trimmed at the next start, without them */
    for (int day = 0; day < 40; day++) {
        start(&h, h.wall_true);
        for (int k = 0; k < 1000; k++) {
            h.mono += 15, h.wall_true += 15;
            batlog_note(&h.log, h.mono, 20 + k % 80, flags_of(k % 3, 0));
        }
        batlog_off(&h.log, h.mono);
        h.wall_true += 86400;
    }
    add_stray_headers(5);
    struct stat st;
    CHECK(stat(g_path, &st) == 0 && st.st_size > BATLOG_TRIM_BYTES);
    start(&h, h.wall_true);
    CHECK(headers() == 1);
    n = batlog_load(g_path, NULL, 0, &r);
    int known = n > 0;
    for (int i = 0; i < n; i++) known &= r[i].kind <= BL_CLOCK && r[i].t > 1700000000;
    CHECK(known);
    free(r);
}

/* Switching off closes the log: in the seconds the H2 takes to go down the
 * screen comes on for "Powering off" and the music stops, and neither is
 * noted after the off record (the history then read as on again, for as long
 * as the H2 was off). The next start opens it again. */
static void test_closed_by_off(void)
{
    unlink(g_path);
    h2_t h = {.wall_true = 1795000000, .level = 80};
    start(&h, h.wall_true);
    run(&h, 1800, BL_ACT_PLAYING, 0);
    batlog_off(&h.log, h.mono);
    batlog_note(&h.log, h.mono + 1, pct_of(h.level), flags_of(BL_ACT_SCREEN, 0));
    batlog_note(&h.log, h.mono + 3, pct_of(h.level) - 1, flags_of(BL_ACT_PAUSED, 0));
    batlog_off(&h.log, h.mono + 4);                   /* (asked twice: said once) */
    batlog_flush(&h.log);
    batrec_t *r;
    int n = batlog_load(g_path, NULL, 0, &r), offs = 0;
    for (int i = 0; i < n; i++) offs += r[i].kind == BL_OFF;
    CHECK(n > 2 && r[n - 1].kind == BL_OFF && offs == 1);
    free(r);
    h.wall_true += 3600;
    start(&h, h.wall_true);
    run(&h, 1800, BL_ACT_PLAYING, 0);
    batlog_flush(&h.log);
    int m = batlog_load(g_path, NULL, 0, &r);
    CHECK(m > n + 1 && r[n].kind == BL_BOOT && r[m - 1].kind == BL_SAMPLE);
    free(r);
}

static void raw(unsigned char *o, int64_t t, int pct, int flags, int kind, int last)
{
    uint32_t u = (uint32_t)t;
    o[0] = (unsigned char)u, o[1] = (unsigned char)(u >> 8), o[2] = (unsigned char)(u >> 16), o[3] = (unsigned char)(u >> 24);
    o[4] = (unsigned char)pct, o[5] = (unsigned char)flags, o[6] = (unsigned char)kind, o[7] = (unsigned char)last;
}

/* Damaged records in the middle of a start (a card's bad block, a file put
 * back by a repair tool): a charge over 100, a time before its start or years
 * after it or behind the record before it, a start with a charge, a last byte
 * that isn't 0. None is taken: the history reads as it did without them.
 * (Taken, one far ahead moved every later start after it, and the graph
 * showed nothing.) */
static void test_damaged_records(void)
{
    unlink(g_path);
    h2_t h = {.wall_true = 1795000000, .level = 100};
    start(&h, h.wall_true);
    int64_t base = h.log.base;
    run(&h, 6 * 3600, BL_ACT_PLAYING, 0);
    batlog_off(&h.log, h.mono);
    h.wall_true += 2 * 3600;
    start(&h, h.wall_true);
    run(&h, 3600, BL_ACT_PLAYING, 0);
    batlog_off(&h.log, h.mono);
    batrec_t *r;
    int n = batlog_load(g_path, NULL, 0, &r);
    batrep_t a, b;
    batlog_report(r, n, 1, &a);
    free(r);

    FILE *f = fopen(g_path, "rb");
    struct stat st;
    if (!f || fstat(fileno(f), &st)) { CHECK(0); if (f) fclose(f); return; }
    unsigned char *d = malloc((size_t)st.st_size), bad[8 * 8];
    size_t got = fread(d, 1, (size_t)st.st_size, f), at = 8 + 8 * 10;   /* (after the first start's tenth record: 1 h in) */
    fclose(f);
    int f0 = flags_of(BL_ACT_PLAYING, 0);
    raw(bad + 0, base + 3700, 200, f0, BL_SAMPLE, 0);                /* 200 % */
    raw(bad + 8, base + 3700, 101, f0, BL_OFF, 0);
    raw(bad + 16, base + 20LL * 366 * 86400, 50, f0, BL_SAMPLE, 0);  /* twenty years on */
    raw(bad + 24, base - 100000, 50, f0, BL_SAMPLE, 0);              /* before its start */
    raw(bad + 32, base + 60, 50, f0, BL_OFF, 0);                     /* behind the record before it */
    raw(bad + 40, base + 3700, 50, f0, BL_BOOT, 0);                  /* a start has no charge */
    raw(bad + 48, base + 3700, 0, f0, BL_CLOCK, 0);                  /* nor a clock a state */
    raw(bad + 56, base + 3700, 50, f0, BL_SAMPLE, 7);                /* the last byte is always 0 */
    f = fopen(g_path, "wb");
    CHECK(got > at && f && fwrite(d, 1, at, f) == at && fwrite(bad, 1, sizeof bad, f) == sizeof bad &&
          fwrite(d + at, 1, got - at, f) == got - at);
    if (f) fclose(f);
    free(d);

    int m = batlog_load(g_path, NULL, 0, &r);
    batlog_report(r, m, 1, &b);
    int over = 0, ordered = 1;
    for (int i = 0; i < m; i++) over += r[i].pct > 100;
    for (int i = 1; i < m; i++) ordered &= r[i].t >= r[i - 1].t;
    free(r);
    printf("damaged records: %d read of %d good and 8 bad\n", m, n);
    CHECK(m == n && !over && ordered);
    CHECK(b.t1 == a.t1 && b.nspans == a.nspans && b.span[0].used == a.span[0].used && b.span[0].on_s == a.span[0].on_s);
    CHECK(!memcmp(a.pct, b.pct, sizeof a.pct));
}

/* A file that doesn't begin with the header (cut short as it was first
 * written, or damaged since) can't be read back, and was added to all the
 * same: nothing to show, for good. It starts again at the next write. */
static void test_bad_header(void)
{
    static const char *const heads[] = {"FBATL", "not a battery log at all, and longer than a header", "FBATLOG2........"};
    for (int k = 0; k < 3; k++) {
        FILE *f = fopen(g_path, "wb");
        CHECK(f && fputs(heads[k], f) >= 0);
        if (f) fclose(f);
        h2_t h = {.wall_true = 1795000000, .level = 70};
        start(&h, h.wall_true);
        run(&h, 1800, BL_ACT_PLAYING, 0);
        CHECK(batlog_flush(&h.log) == 0);
        batrec_t *r;
        int n = batlog_load(g_path, NULL, 0, &r);
        CHECK(headers() == 1 && n > 2 && r[0].kind == BL_BOOT);
        free(r);
        /* and a good one is added to, not started again */
        run(&h, 1800, BL_ACT_PLAYING, 0);
        batlog_off(&h.log, h.mono);
        int m = batlog_load(g_path, NULL, 0, &r);
        CHECK(headers() == 1 && m > n && r[0].kind == BL_BOOT && r[m - 1].kind == BL_OFF);
        free(r);
    }
}

/* A file past the trim size whose first bytes can't be read just now (the
 * card's read failed) is left alone: it was taken for "not ours" and
 * removed, the whole history with it. One that was read and isn't ours
 * still goes. */
static void test_trim_read_fails(void)
{
    unlink(g_path);
    h2_t h = {.wall_true = 1790000000, .level = 100};
    start(&h, h.wall_true);
    run(&h, 600, BL_ACT_PLAYING, 0);
    batlog_flush(&h.log);
    FILE *f = fopen(g_path, "ab");                      /* (grown past the limit with no trim on the way) */
    unsigned char rec[8];
    for (int k = 0; f && k < BATLOG_TRIM_BYTES / 8; k++) {
        raw(rec, h.log.base + h.mono + k, 50, flags_of(BL_ACT_PLAYING, 0), BL_SAMPLE, 0);
        fwrite(rec, 1, 8, f);
    }
    if (f) fclose(f);
    struct stat before, after;
    CHECK(stat(g_path, &before) == 0 && before.st_size > BATLOG_TRIM_BYTES);
    g_read_fails = 1;
    start(&h, h.wall_true);
    g_read_fails = 0;
    CHECK(stat(g_path, &after) == 0 && after.st_size == before.st_size);
    start(&h, h.wall_true);                             /* read this time: trimmed, as ever */
    CHECK(stat(g_path, &after) == 0 && after.st_size < before.st_size && after.st_size > 8 && headers() == 1);
    /* not ours at all, and that big: removed at the start */
    f = fopen(g_path, "wb");
    for (int k = 0; f && k < BATLOG_TRIM_BYTES / 8 + 100; k++) fwrite("NOTOURS!", 1, 8, f);
    if (f) fclose(f);
    start(&h, h.wall_true);
    CHECK(stat(g_path, &after) != 0);
}

/* The screens before the player (the first scan): off at the third low
 * reading in a row, not charging; a reading that couldn't be made, or that
 * says nothing, is not a low one and starts the count again. */
static void test_low_step(void)
{
    const long low = 3450000;
    int run = 0;
    CHECK(batlog_low_step(&run, "Discharging\n", "3440000\n", low) == 0 && run == 1);
    CHECK(batlog_low_step(&run, "Discharging\n", "3430000\n", low) == 0 && run == 2);
    CHECK(batlog_low_step(&run, "Discharging\n", "3420000\n", low) == 1);
    CHECK(batlog_low_step(&run, "Discharging\n", "3410000\n", low) == 0);   /* (said once) */
    /* a good reading between two low ones: from nothing again */
    run = 0;
    CHECK(batlog_low_step(&run, "Discharging\n", "3440000\n", low) == 0);
    CHECK(batlog_low_step(&run, "Discharging\n", "3440000\n", low) == 0);
    CHECK(batlog_low_step(&run, "Discharging\n", "3450000\n", low) == 0 && run == 0);   /* (at the mark: not under it) */
    CHECK(batlog_low_step(&run, "Discharging\n", "3440000\n", low) == 0 && run == 1);
    /* charging: never, however low */
    run = 2;
    CHECK(batlog_low_step(&run, "Charging\n", "3300000\n", low) == 0 && run == 0);
    /* plugged in and not charging (full, or held): the voltage decides, as in the player */
    run = 2;
    CHECK(batlog_low_step(&run, "Not charging\n", "3300000\n", low) == 1);
    run = 2;
    CHECK(batlog_low_step(&run, "Full\n", "4150000\n", low) == 0 && run == 0);
    /* a reading that couldn't be made, or isn't a voltage */
    run = 2;
    CHECK(batlog_low_step(&run, NULL, "3300000\n", low) == 0 && run == 0);
    run = 2;
    CHECK(batlog_low_step(&run, "Discharging\n", NULL, low) == 0 && run == 0);
    run = 2;
    CHECK(batlog_low_step(&run, "Discharging\n", "\n", low) == 0 && run == 0);
    run = 2;
    CHECK(batlog_low_step(&run, "Discharging\n", "0\n", low) == 0 && run == 0);
    run = 2;
    CHECK(batlog_low_step(&run, "Discharging\n", "-5\n", low) == 0 && run == 0);
    run = 2;
    CHECK(batlog_low_step(&run, "Discharging\n", "what\n", low) == 0 && run == 0);
}

int main(void)
{
    snprintf(g_path, sizeof g_path, "%s/fathom-batlog-%d.log", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", (int)getpid());
    test_rates();
    test_off_and_clock();
    test_restart_and_backwards();
    test_top_up_and_history();
    test_trim();
    test_trim_heavy();
    test_trim_one_start();
    test_stray_headers();
    test_closed_by_off();
    test_damaged_records();
    test_bad_header();
    test_trim_read_fails();
    test_low_step();
    unlink(g_path);
    return t_result();
}
