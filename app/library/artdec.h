/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/*
 * Cover images decoded small, straight from where they are (a cover.jpg, or
 * a picture inside a FLAC), in little memory whatever their size: the H2 has
 * 24 MB of RAM and a 3000x3000 cover is 27 MB decoded.
 *
 *   - JPEG: libjpeg shrinks by 1/2, 1/4 or 1/8 while decoding. A progressive
 *     JPEG keeps every coefficient of the whole image in libjpeg (3 bytes a
 *     pixel), so one too big for that is read from its first pass alone,
 *     which holds each 8x8 block's average: the picture at 1/8, in a few
 *     hundred KB, without reading the rest of the file.
 *   - PNG: unpacked a row at a time, rows averaged by a whole factor as they
 *     come, so only a few rows are ever held (interlaced ones: as below).
 *   - Anything else stb_image reads (and interlaced PNGs): whole, in memory,
 *     within the limits.
 *
 * A PNG's transparency is left out (its colours as they are) unless the limits
 * say `matte`: then it's laid over white, as a logo drawn for a web page is.
 */
#ifndef FATHOM_ARTDEC_H
#define FATHOM_ARTDEC_H

#include <stddef.h>
#include <stdio.h>

typedef struct {
    long jpeg_memory;       /* libjpeg's working memory for one image */
    long long max_pixels;   /* an image decoded whole (or a PNG's shrunk result) */
    long max_file;          /* encoded bytes read into memory for those */
    int matte;              /* 1: a PNG's transparent parts over white (0, as when left out: its colours as they are) */
} art_limits_t;

/* The image `len` bytes long (-1: to the end of the file) at `off` in f,
 * as RGB888 (*rgb malloc'd, *w x *h) with at least `min_side` pixels on its
 * short side if it has them, and never needlessly more (from a progressive
 * JPEG's first pass, its size / 8). 0, or -1: not an image, damaged, an
 * unsupported kind (CMYK, arithmetic coding) or over the limits. */
int art_decode(FILE *f, long off, long len, int min_side, const art_limits_t *lim,
               unsigned char **rgb, int *w, int *h);
/* The same from memory (a radio station's logo, an MP3's picture) */
int art_decode_mem(const unsigned char *enc, size_t len, int min_side, const art_limits_t *lim,
                   unsigned char **rgb, int *w, int *h);

#endif
