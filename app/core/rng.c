/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
#include "rng.h"

#include <stdio.h>
#include <time.h>

void rng_seed(rng_t *r, uint64_t seed, uint64_t stream)
{
    r->state = 0;
    r->inc = (stream << 1u) | 1u;
    rng_u32(r);
    r->state += seed;
    rng_u32(r);
}

uint32_t rng_u32(rng_t *r)
{
    uint64_t old = r->state;
    r->state = old * 6364136223846793005ULL + r->inc;
    uint32_t xorshifted = (uint32_t)(((old >> 18u) ^ old) >> 27u);
    uint32_t rot = (uint32_t)(old >> 59u);
    return (xorshifted >> rot) | (xorshifted << ((-rot) & 31));
}

uint32_t rng_below(rng_t *r, uint32_t bound)
{
    uint64_t m = (uint64_t)rng_u32(r) * bound;
    uint32_t low = (uint32_t)m;
    if (low < bound) {
        uint32_t threshold = (uint32_t)(-bound) % bound;
        while (low < threshold) {
            m = (uint64_t)rng_u32(r) * bound;
            low = (uint32_t)m;
        }
    }
    return (uint32_t)(m >> 32);
}

double rng_unit(rng_t *r)
{
    /* 53 random bits -> [0, 1) */
    uint64_t hi = rng_u32(r) >> 5;   /* 27 bits */
    uint64_t lo = rng_u32(r) >> 6;   /* 26 bits */
    return (double)((hi << 26) | lo) * (1.0 / 9007199254740992.0);
}

double rng_range(rng_t *r, double lo, double hi)
{
    return lo + (hi - lo) * rng_unit(r);
}

uint64_t rng_os_seed(void)
{
    uint64_t seed = 0;
    FILE *f = fopen("/dev/urandom", "rbe");
    if (f) {
        size_t got = fread(&seed, 1, sizeof seed, f);
        fclose(f);
        if (got == sizeof seed)
            return seed;
    }
    return (uint64_t)time(NULL) ^ ((uint64_t)clock() << 32);
}
