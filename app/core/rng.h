/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* PCG32 random number generator (O'Neill, pcg-random.org) with unbiased helpers. */
#ifndef FATHOM_RNG_H
#define FATHOM_RNG_H

#include <stdint.h>

typedef struct {
    uint64_t state;
    uint64_t inc;
} rng_t;

void     rng_seed(rng_t *r, uint64_t seed, uint64_t stream);
uint32_t rng_u32(rng_t *r);
/* Uniform integer in [0, bound) without modulo bias (Lemire's method). bound > 0. */
uint32_t rng_below(rng_t *r, uint32_t bound);
/* Uniform double in [0, 1). */
double   rng_unit(rng_t *r);
/* Seed from the operating system's entropy source; falls back to time if unavailable. */
uint64_t rng_os_seed(void);

#endif
