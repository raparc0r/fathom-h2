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

#define WIDTH_MONO 0
#define WIDTH_NORMAL 100
#define WIDTH_MAX 200

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
        s[2 * i] = nl > INT32_MAX ? INT32_MAX : nl < INT32_MIN ? INT32_MIN : (int32_t)nl;
        s[2 * i + 1] = nr > INT32_MAX ? INT32_MAX : nr < INT32_MIN ? INT32_MIN : (int32_t)nr;
    }
}

#endif
