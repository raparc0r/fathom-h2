/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Null audio output: accepts everything instantly and counts it (tests). */
#include <stdio.h>
#include <string.h>

#include "audio/output.h"

static int n_open(void *ctx, const audio_format_t *fmt) { out_null_stats_t *s = ctx; s->fmt = *fmt; s->opens++; return 0; }
static long n_write(void *ctx, const int32_t *f, long n) { (void)f; ((out_null_stats_t *)ctx)->frames += (unsigned long long)n; return n; }
static long n_delay(void *ctx) { (void)ctx; return 0; }
static void n_nop(void *ctx) { (void)ctx; }
static void n_pause(void *ctx, int on) { (void)ctx; (void)on; }
static void n_describe(void *ctx, char *out, int cap) { (void)ctx; snprintf(out, (size_t)cap, "null"); }

void out_null_create(audio_output_t *out, out_null_stats_t *stats)
{
    memset(stats, 0, sizeof *stats);
    *out = (audio_output_t){n_open, n_write, n_delay, n_nop, n_nop, n_pause, n_nop, n_describe, stats, NULL};
}
