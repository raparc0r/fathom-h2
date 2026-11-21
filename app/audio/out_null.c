/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Null audio output: accepts everything instantly and counts it (tests). */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "audio/output.h"

static int n_open(void *ctx, const audio_format_t *fmt) { out_null_stats_t *s = ctx; s->fmt = *fmt; s->opens++; return 0; }
/* Each write takes a moment too (a real output makes the playback thread
 * wait): that thread is real-time on the H2, and one that never waited kept
 * every other thread of a test off its one core until the track's end. */
static long n_write(void *ctx, const int32_t *f, long n)
{
    (void)f;
    ((out_null_stats_t *)ctx)->frames += (unsigned long long)n;
    nanosleep(&(struct timespec){0, 200 * 1000L}, NULL);
    return n;
}
static long n_delay(void *ctx) { (void)ctx; return 0; }
static void n_nop(void *ctx) { (void)ctx; }
/* A drain takes a moment, as a real output's does (it plays out what's queued):
 * meanwhile the player queues the next track. An instant one let a short track
 * end before that, on the H2's one core (the engine's thread is real-time). */
static void n_drain(void *ctx) { (void)ctx; nanosleep(&(struct timespec){0, 30 * 1000000L}, NULL); }
static void n_pause(void *ctx, int on) { (void)ctx; (void)on; }
static void n_describe(void *ctx, char *out, int cap) { (void)ctx; snprintf(out, (size_t)cap, "null"); }

void out_null_create(audio_output_t *out, out_null_stats_t *stats)
{
    memset(stats, 0, sizeof *stats);
    *out = (audio_output_t){n_open, n_write, n_delay, n_drain, n_nop, n_pause, n_nop, n_describe, stats, NULL, NULL, NULL};
}
