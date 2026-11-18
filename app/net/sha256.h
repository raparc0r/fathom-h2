/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * SHA-256 (FIPS 180-4), for checking updates against the release's
 * SHA256SUMS and GitHub's digest. Our own: the desktop build has no mbedTLS,
 * and it's a hundred lines.
 */
#ifndef FATHOM_SHA256_H
#define FATHOM_SHA256_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t h[8];
    uint64_t len;           /* bytes so far */
    uint8_t buf[64];
    size_t n;               /* bytes in buf */
} sha256_t;

void sha256_init(sha256_t *s);
void sha256_update(sha256_t *s, const void *data, size_t len);
void sha256_final(sha256_t *s, uint8_t out[32]);
/* out[32] as 64 lower-case hex digits and a NUL. */
void sha256_hex(const uint8_t d[32], char hex[65]);

#endif
