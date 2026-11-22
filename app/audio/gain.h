/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* A gain on left-justified int32 samples (volume levelling, a USB DAC's
 * software volume, the sleep timer's fade), in integer arithmetic: Fathom is
 * built soft-float (the X1000's FPU unused), so a float multiply per sample
 * would be a library call. The gain is
 * Q4.28 (up to 8x, +18 dB), each product rounded, the result saturated. */
#ifndef FATHOM_GAIN_H
#define FATHOM_GAIN_H

#include <math.h>
#include <stdint.h>

#define GAIN_FRAC 28

static inline int32_t gain_q28(double g)
{
    double q = g * (double)(1 << GAIN_FRAC);
    return q <= 0 ? 0 : q >= 2147483647.0 ? INT32_MAX : (int32_t)lround(q);
}

static inline void gain_run(int32_t *s, long n, int32_t k)
{
    for (long i = 0; i < n; i++) {
        int64_t v = ((int64_t)s[i] * k + (1 << (GAIN_FRAC - 1))) >> GAIN_FRAC;
        s[i] = v > INT32_MAX ? INT32_MAX : v < INT32_MIN ? INT32_MIN : (int32_t)v;
    }
}

/* Stereo with a gain per channel (the balance): n frames, interleaved L R. */
static inline void gain_run2(int32_t *s, long n, int32_t kl, int32_t kr)
{
    for (long i = 0; i < n; i++) {
        gain_run(s + 2 * i, 1, kl);
        gain_run(s + 2 * i + 1, 1, kr);
    }
}

#endif
