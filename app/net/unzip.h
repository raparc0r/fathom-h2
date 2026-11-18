/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Reading a zip file on the card, an entry at a time, streamed (the release
 * zip is 16 MB; the H2 has 24 MB of RAM): stored and deflated entries
 * (zlib's inflate, already linked into the player for FFmpeg's http), each
 * checked against its CRC-32. No zip64, no encryption: a release zip has
 * neither. busybox's unzip was left out of the RAM disk to save memory.
 */
#ifndef FATHOM_UNZIP_H
#define FATHOM_UNZIP_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct {
    FILE *f;
    uint64_t file_size;
    uint32_t cd_off, cd_size;   /* the central directory */
    unsigned n;                 /* its entries */
    uint32_t next;              /* zip_next's position in it */
    unsigned seen;
} zip_t;

typedef struct {
    char name[512];             /* as stored ('/' separated; a folder ends in '/') */
    uint16_t method;            /* 0 stored, 8 deflated */
    uint16_t flags;
    uint32_t crc, csize, usize;
    uint32_t local_off;         /* its local header */
    int dir;
} zip_entry_t;

int  zip_open(zip_t *z, const char *path);       /* 0, or -1 (not a zip we read) */
void zip_close(zip_t *z);
/* The central directory's next entry: 1, 0 at the end, -1 if it's damaged.
 * Restart with zip_rewind. */
int  zip_next(zip_t *z, zip_entry_t *e);
void zip_rewind(zip_t *z);
/* An entry's contents, in pieces, to sink (a non-zero return stops it: -1).
 * 0 when all of it came out and its CRC matched; -1 otherwise, or on *abort. */
typedef int (*zip_sink_fn)(const void *data, size_t len, void *ctx);
int  zip_extract(zip_t *z, const zip_entry_t *e, zip_sink_fn sink, void *ctx, const volatile int *abort);

#endif
