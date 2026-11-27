/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* What every stage of the sound path does to a sample at its end: a 64-bit
 * result brought back into 32 bits, clipped, never wrapped. One copy, where
 * the gain, the stereo width, the crossfeed, the equalizer and the
 * Bluetooth converter each had their own. */
#ifndef FATHOM_PCM_H
#define FATHOM_PCM_H

#include <stdint.h>

static inline int32_t pcm_sat32(int64_t v)
{
    return v > INT32_MAX ? INT32_MAX : v < INT32_MIN ? INT32_MIN : (int32_t)v;
}

#endif
