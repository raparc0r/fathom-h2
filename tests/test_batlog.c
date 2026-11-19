/* SPDX-License-Identifier: GPL-3.0-or-later */
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

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #c); failures++; } } while (0)

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

static void test_trim(void)
{
    unlink(g_path);
    h2_t h = {.wall_true = 1790000000, .level = 100};
    /* 150 days of a busy log: a record every 3 minutes */
    for (int day = 0; day < 150; day++) {
        start(&h, h.wall_true);
        for (int k = 0; k < 480; k++) {
            h.mono += 180, h.wall_true += 180;
            batlog_note(&h.log, h.mono, 50 + k % 40, flags_of(k % 3, 0));
        }
        batlog_off(&h.log, h.mono);
        h.wall_true += 24 * 3600 - 480 * 180;
    }
    struct stat st;
    stat(g_path, &st);
    long before = (long)st.st_size;
    start(&h, h.wall_true);
    stat(g_path, &st);
    batrec_t *r;
    int n = batlog_load(g_path, NULL, 0, &r);
    printf("trim: %ld bytes -> %ld, the oldest %.0f days back\n", before, (long)st.st_size,
           n > 0 ? (h.wall_true - r[0].t) / 86400.0 : -1.0);
    CHECK(st.st_size <= BATLOG_TRIM_BYTES && before <= BATLOG_TRIM_BYTES + 4096);   /* (trimmed at each start past it) */
    CHECK(n > 0 && r[0].kind == BL_BOOT && h.wall_true - r[0].t <= (int64_t)(BATLOG_KEEP_DAYS + 1) * 86400);
    free(r);
}

int main(void)
{
    snprintf(g_path, sizeof g_path, "%s/fathom-batlog-%d.log", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", (int)getpid());
    test_rates();
    test_off_and_clock();
    test_restart_and_backwards();
    test_top_up_and_history();
    test_trim();
    unlink(g_path);
    if (failures) { fprintf(stderr, "%d failures\n", failures); return 1; }
    printf("batlog: ok\n");
    return 0;
}
