/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* FNV-1a, written once (the library's signatures and its covers' sums, the
 * shuffle's works, the player's saved queue each had the loop). What is
 * hashed, and from which starting value, is each caller's: several of the
 * results are kept on the card (a listing's signature, a cover's sum, a found
 * cover's file name), so neither may change. */
#ifndef FATHOM_HASH_H
#define FATHOM_HASH_H

#include <stddef.h>
#include <stdint.h>

#define FNV64_START 14695981039346656037ULL   /* the usual starting values */
#define FNV32_START 2166136261u

/* h carried on over the n bytes at p */
static inline uint64_t fnv64(uint64_t h, const void *p, size_t n)
{
    const unsigned char *b = p;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 1099511628211ULL;
    return h;
}

static inline uint32_t fnv32(uint32_t h, const void *p, size_t n)
{
    const unsigned char *b = p;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 16777619u;
    return h;
}

#endif
