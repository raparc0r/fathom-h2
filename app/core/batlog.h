/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/*
 * Battery history (Settings > Power > Battery): a log on the card of the
 * charge and of what drains it, and what it says.
 *
 * The H2's PMIC gives the charge in percent, not the current: everything here
 * comes from when each percent went and what the H2 was doing meanwhile. An
 * 8-byte record is kept whenever the percentage or the state (charging, the
 * screen, playing, the output, Wi-Fi, Bluetooth) changes, held in RAM and
 * written every 10 minutes, when charging starts or stops, and at power-off.
 * About 3 KB a day; 60 days are kept (up to 256 KB: a heavy user keeps less).
 *
 * Times: records carry the wall clock as this start of the H2 had it (its
 * base: the clock at CLOCK_MONOTONIC 0, plus the monotonic seconds). The H2's
 * clock stops while it's off and starts again from the last one saved, so how
 * long it was off is unknown, until Wi-Fi sets the clock: a CLOCK record then
 * gives this start's true base, and the analysis moves its records to it.
 */
#ifndef FATHOM_BATLOG_H
#define FATHOM_BATLOG_H

#include <stdint.h>

#define BL_CHARGING 0x01
#define BL_SCREEN   0x02     /* the screen on */
#define BL_PLAYING  0x04
#define BL_WIFI     0x08
#define BL_BT       0x10
#define BL_OUT_SHIFT 5       /* the output, 2 bits: */
enum { BL_OUT_WIRED, BL_OUT_LINE, BL_OUT_BT, BL_OUT_USB };

enum { BL_SAMPLE, BL_BOOT, BL_OFF, BL_CLOCK };   /* a record's kind */

typedef struct {
    int64_t t;               /* wall seconds (in the file: 32 bits) */
    uint8_t pct, flags, kind;
    uint8_t known;           /* BOOT, after loading: why the time before it is known (BL_KNOWN_*), 0: not */
} batrec_t;
enum { BL_KNOWN_RESTART = 1, BL_KNOWN_CLOCK = 2 };   /* the player again, the H2 on; the clock set by Wi-Fi */

#define BATLOG_BUF 256
#define BATLOG_KEEP_DAYS 60
#define BATLOG_TRIM_BYTES (256 * 1024)   /* past this, trimmed to BATLOG_KEEP_DAYS and 3/4 of this at most */

typedef struct {
    char path[512];
    batrec_t buf[BATLOG_BUF];
    int n;                   /* records not written yet */
    int64_t base;            /* wall clock at monotonic 0 */
    int pct, flags;          /* the last recorded (-1: none) */
    int64_t mono;            /* the last note's monotonic seconds */
    int64_t flushed;         /* when it was last written (monotonic) */
    int off;                 /* switched off (batlog_off): nothing more is noted in this start */
} batlog_t;

/* At start (the clock as good as it gets: restored or set): trims the file if
 * it's grown past BATLOG_TRIM_BYTES (a flush does too), and notes the start. */
void batlog_open(batlog_t *b, const char *path, int64_t wall_now, int64_t mono_now);
/* Every few seconds: the charge and the state (BL_* | output << BL_OUT_SHIFT).
 * Recorded if either changed; written every 10 minutes, and at once when
 * charging starts or stops. */
void batlog_note(batlog_t *b, int64_t mono_now, int pct, int flags);
/* The wall clock was just set (NTP): this start's records move to it. */
void batlog_clock_set(batlog_t *b, int64_t wall_now, int64_t mono_now);
/* Switching off: noted and written, and the log closed: nothing is noted
 * after it until batlog_open (should the H2 stay on after all). */
void batlog_off(batlog_t *b, int64_t mono_now);
/* Write what's held: 0, or -1 (the card's away: kept for the next try). */
int  batlog_flush(batlog_t *b);

/* The file and what's held (live may be NULL), times corrected and in order,
 * the last a sample at now (live's state): *out malloc'd, the count or -1. */
int  batlog_load(const char *path, const batlog_t *live, int64_t mono_now, batrec_t **out);

/* ---------- what it says ---------- */

enum { BL_ACT_PLAYING, BL_ACT_SCREEN, BL_ACT_PAUSED, BL_ACTS };   /* playing, screen off; screen on; paused, screen off */

typedef struct {
    int64_t start, end;      /* wall seconds */
    int pct_start, pct_end;
    int64_t on_s;            /* on battery and switched on */
    int used;                /* percent gone while on */
    int64_t act_s[BL_ACTS];
    int min_per_pct[BL_ACTS];   /* minutes a percent lasts at each, 0: too little to say */
    int full_h10;            /* tenths of hours a full charge lasts at this mix, 0: too little to say */
    int from_full;           /* unplugged at 95 % or more */
    double ata[BL_ACTS][BL_ACTS], atb[BL_ACTS];   /* (for adding spans together) */
} batspan_t;

#define BATREP_COLS 278
#define BATREP_SPANS 24
#define BATREP_GAPS 8

typedef struct {
    int days;                /* the graph's: 1 or 7 */
    int64_t t0, t1;          /* the graph's first and last second (t1: now) */
    int8_t pct[BATREP_COLS];    /* each column's charge, -1: off or before the log */
    uint8_t charging[BATREP_COLS];
    int gap[BATREP_GAPS], ngaps;   /* columns where the H2 was off for a time not known */
    int have_since;
    batspan_t since;         /* since unplugged at 95 % or more */
    batspan_t span[BATREP_SPANS];  /* each time on battery between charges, newest first */
    int nspans, total_spans;
    int avg_full_h10;        /* a full charge, on average, 0: too little to say */
    int clock_ok;            /* the wall clock was set (dates mean something) */
} batrep_t;

/* records (from batlog_load, in order, the last at now) into a report */
void batlog_report(const batrec_t *r, int n, int days, batrep_t *rep);

/* ---------- an empty battery, before the player is up ---------- */

/* One reading for the screens that come before the player (device/main.c
 * early_battery), by the player's own rule: low is under low_uv and not
 * charging. status and volt: the PMIC's "status" and "voltage_now" as read
 * (NULL: couldn't be). A reading that isn't whole (either one missing, or a
 * voltage that isn't a number above 0) is not a low one. *run: the low
 * readings in a row so far (0 before the first). 1 at the third in a row,
 * once; 0 otherwise. */
int batlog_low_step(int *run, const char *status, const char *volt, long low_uv);

#endif
