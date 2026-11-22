/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
#include "out_router.h"
#include "audio/out_bt.h"

#include <stdio.h>

typedef struct {
    audio_output_t alsa, bt;
    int use_bt;
} router_t;

static router_t R;
static audio_output_t *cur(void) { return R.use_bt ? &R.bt : &R.alsa; }

static int r_open(void *c, const audio_format_t *f) { (void)c; return cur()->open(cur()->ctx, f); }
static long r_write(void *c, const int32_t *x, long n) { (void)c; return cur()->write(cur()->ctx, x, n); }
static long r_delay(void *c) { (void)c; return cur()->delay(cur()->ctx); }
static void r_drain(void *c) { (void)c; cur()->drain(cur()->ctx); }
static void r_flush(void *c) { (void)c; cur()->flush(cur()->ctx); }
static void r_pause(void *c, int on) { (void)c; cur()->pause(cur()->ctx, on); }
static void r_close(void *c) { (void)c; cur()->close(cur()->ctx); }
static void r_describe(void *c, char *o, int cap) { (void)c; cur()->describe(cur()->ctx, o, cap); }
static int r_can_cut(void *c) { (void)c; return cur()->can_cut ? cur()->can_cut(cur()->ctx) : 0; }
/* (Bluetooth has none: its flush) */
static void r_flush_soft(void *c)
{
    (void)c;
    if (cur()->flush_soft) cur()->flush_soft(cur()->ctx);
    else cur()->flush(cur()->ctx);
}
static int r_set_eq(void *c, const eq_profile_t *p)
{
    (void)c;
    return cur()->set_eq ? cur()->set_eq(cur()->ctx, p) : 0;
}

void out_router_create(audio_output_t *out, const audio_output_t *alsa, const audio_output_t *bt)
{
    R.alsa = *alsa;
    if (bt) R.bt = *bt;
    *out = (audio_output_t){r_open, r_write, r_delay, r_drain, r_flush, r_pause, r_close, r_describe, &R, r_set_eq, r_can_cut,
                            r_flush_soft};
}

void out_router_alsa(void *ctx, void *device)
{
    (void)ctx;
    R.use_bt = 0;
    out_alsa_set_device(R.alsa.ctx, device);
}

void out_router_bt(void *ctx, void *pcm)
{
    (void)ctx;
    if (!R.bt.ctx) return;
    R.use_bt = pcm != NULL;
    out_bt_set_pcm(R.bt.ctx, pcm);
}

int out_router_on_bt(void) { return R.use_bt; }
