/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The loudness measurement as it was up to 0.8.3 (loudness_v1.c, a frozen copy
 * of app/audio/loudness.c with its names prefixed), kept so rg-bench and the
 * tests can check the faster one against it on the same samples. */
#ifndef FATHOM_LOUDNESS_V1_H
#define FATHOM_LOUDNESS_V1_H

#include <stdint.h>

typedef struct v1_loudness v1_loudness_t;

v1_loudness_t *v1_loudness_new(unsigned rate, int channels);
void           v1_loudness_free(v1_loudness_t *l);
void           v1_loudness_add(v1_loudness_t *l, const int32_t *frames, long n);
double         v1_loudness_lufs(const v1_loudness_t *l);
double         v1_loudness_peak(const v1_loudness_t *l);

#endif
