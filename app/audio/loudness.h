/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/*
 * Integrated loudness (EBU R128 / ITU-R BS.1770-4) and sample peak, for
 * volume levelling (ReplayGain 2.0: gain = -18 LUFS - loudness). Feed it the
 * decoder's frames (interleaved int32, left-justified), then read the result.
 * Up to 2 channels (both weighted 1.0).
 */
#ifndef FATHOM_LOUDNESS_H
#define FATHOM_LOUDNESS_H

#include <stdint.h>

typedef struct loudness loudness_t;

loudness_t *loudness_new(unsigned rate, int channels);
void        loudness_free(loudness_t *l);
void        loudness_add(loudness_t *l, const int32_t *frames, long n);
/* Integrated loudness in LUFS (gated), or -HUGE_VAL for silence. */
double      loudness_lufs(const loudness_t *l);
/* Highest absolute sample, 0..1 of full scale. */
double      loudness_peak(const loudness_t *l);

#define LOUDNESS_REFERENCE (-18.0)   /* ReplayGain 2.0 */

#endif
