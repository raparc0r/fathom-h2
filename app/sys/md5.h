/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/*
 * MD5 (RFC 1321), for HiBy's update files, whose update.txt lists each image's
 * MD5. Not for anything that needs to resist forgery.
 */
#ifndef FATHOM_MD5_H
#define FATHOM_MD5_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t h[4];
    uint64_t len;           /* bytes so far */
    uint8_t buf[64];
    size_t n;               /* bytes in buf */
} md5_t;

void md5_init(md5_t *m);
void md5_update(md5_t *m, const void *data, size_t len);
void md5_final(md5_t *m, uint8_t out[16]);
/* out[16] as 32 lower-case hex digits and a NUL. */
void md5_hex(const uint8_t d[16], char hex[33]);

#endif
