/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* The monotonic clock (from the kernel's start; it doesn't jump when the date
 * is set), read in one place: every file had its own copy of these few lines.
 * For measuring how long something took and for deadlines, never for dates. */
#ifndef FATHOM_CLOCK_H
#define FATHOM_CLOCK_H

#include <stdint.h>
#include <time.h>

static inline int64_t clock_us(void)            /* microseconds */
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

static inline int64_t clock_ms(void)            /* milliseconds */
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static inline int64_t clock_s(void)             /* whole seconds */
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec;
}

static inline double clock_fsec(void)           /* seconds, with their fraction */
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + t.tv_nsec / 1e9;
}

/* The milliseconds' low 32 bits, for times kept in a uint32_t and compared by
 * subtraction (they wrap every 49 days, which a difference doesn't mind). */
static inline uint32_t clock_ms32(void) { return (uint32_t)clock_ms(); }

/* The same, never 0: for a field where 0 says "not set". */
static inline uint32_t clock_ms32_set(void)
{
    uint32_t ms = clock_ms32();
    return ms ? ms : 1;
}

#endif
