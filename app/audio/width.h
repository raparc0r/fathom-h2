/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/*
 * Stereo width: how far apart left and right are, as a percentage. What the
 * two channels share (their mean) stays where it is and what differs between
 * them is scaled: 0 is mono, 100 leaves the samples alone (the engine doesn't
 * call this then), above 100 hard-panned sound goes wider than it was.
 *
 *     L' = (a L + b R) / d,  R' = (b L + a R) / d
 *     a = 100 + width, b = 100 - width, d = 2 * max(100, width)
 *
 * Up to 100 each output is a mix of the two inputs that adds up to one, so
 * nothing can clip. Above 100 a channel takes some of the other one away,
 * and a sound on one side only would come out width/100 times as loud: the
 * whole is turned down by that much (150: 3.5 dB; 200: 6 dB), so full scale stays full
 * scale. On the engine's left-justified int32 samples, in whole numbers
 * (coefficients Q30, sums 64-bit, rounded, saturated), stereo only.
 */
#ifndef FATHOM_WIDTH_H
#define FATHOM_WIDTH_H

#include <stdint.h>

#include "audio/pcm.h"

#define WIDTH_MONO 0
#define WIDTH_NORMAL 100
#define WIDTH_MAX 200

/* The widths Settings > Sound > Stereo width stops at, percent (100 is shown
 * as Off: the samples untouched; wider in tens, as the owner asked,
 * 2026-11-23). The H2 and the simulator each had this list. */
static const int WIDTH_STEPS[] = {WIDTH_MONO, 50, 75, WIDTH_NORMAL, 110, 120, 130, 140, 150, 160, 170, 180, 190, WIDTH_MAX};
#define WIDTH_N_STEPS ((int)(sizeof WIDTH_STEPS / sizeof WIDTH_STEPS[0]))

/* Which of WIDTH_STEPS is nearest to pct (a saved value from before the list changed) */
static inline int width_step_nearest(int pct)
{
    int best = 0;
    for (int i = 1; i < WIDTH_N_STEPS; i++) {
        int d = WIDTH_STEPS[i] - pct, b = WIDTH_STEPS[best] - pct;
        if ((d < 0 ? -d : d) < (b < 0 ? -b : b)) best = i;
    }
    return best;
}

/* The two coefficients for a width (clamped to WIDTH_MONO..WIDTH_MAX). */
static inline void width_coeffs(int pct, int32_t *ka, int32_t *kb)
{
    int p = pct < WIDTH_MONO ? WIDTH_MONO : pct > WIDTH_MAX ? WIDTH_MAX : pct;
    int64_t d = 2 * (p > 100 ? p : 100);
    *ka = (int32_t)((((int64_t)(100 + p) << 30) + d / 2) / d);
    *kb = (int32_t)(((int64_t)(100 - p) * (1 << 30)) / d);      /* (below 0 when wider) */
}

/* n frames, interleaved L R. */
static inline void width_run(int32_t *s, long n, int32_t ka, int32_t kb)
{
    for (long i = 0; i < n; i++) {
        int64_t l = s[2 * i], r = s[2 * i + 1];
        int64_t nl = (ka * l + kb * r + (1 << 29)) >> 30, nr = (kb * l + ka * r + (1 << 29)) >> 30;
        s[2 * i] = pcm_sat32(nl);
        s[2 * i + 1] = pcm_sat32(nr);
    }
}

#endif
