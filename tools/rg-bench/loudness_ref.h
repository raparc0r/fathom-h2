/* SPDX-License-Identifier: GPL-3.0-or-later */
/* BS.1770-4 integrated loudness in double precision (loudness_ref.c): the
 * yardstick for app/audio/loudness.c. Same interface; the peak is of the
 * 32-bit samples, 0..1. */
#ifndef FATHOM_LOUDNESS_REF_H
#define FATHOM_LOUDNESS_REF_H

#include <stdint.h>

typedef struct loudness_ref loudness_ref_t;

loudness_ref_t *loudness_ref_new(unsigned rate, int channels);
void            loudness_ref_free(loudness_ref_t *l);
void            loudness_ref_add(loudness_ref_t *l, const int32_t *frames, long n);
double          loudness_ref_lufs(const loudness_ref_t *l);
double          loudness_ref_peak(const loudness_ref_t *l);

#endif
