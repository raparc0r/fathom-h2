/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* The library's pixel code: a decoded picture made into the RGB565 squares
 * the screen shows (a cover cropped and shrunk, a station's logo fitted whole
 * or kept sharp), and the limits a cover is decoded within. Out of library.c,
 * as it was there. */
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE
#define _GNU_SOURCE
#include "library/library_int.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef FATHOM_JPEG_MAX_MEMORY
#define FATHOM_JPEG_MAX_MEMORY (64L * 1024 * 1024)   /* desktop; the H2 build uses 4 MB */
#endif

#ifndef FATHOM_ART_MAX_FILE
#define FATHOM_ART_MAX_FILE (64L * 1024 * 1024)   /* encoded bytes held in RAM; the H2 uses 4 MB */
#endif

#ifndef FATHOM_ART_MAX_PIXELS
#define FATHOM_ART_MAX_PIXELS (16LL * 1024 * 1024)   /* desktop; the H2 build sets ~2.4 MP */
#endif

/* RGB888 -> RGB565 with a 4x4 ordered dither, so gradients don't band on the panel. */
static void to_rgb565(const unsigned char *rgb, int size, uint16_t *out)
{
    static const int bayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++) {
            const unsigned char *p = rgb + 3 * (y * size + x);
            int d = bayer[y & 3][x & 3];
            int r = p[0] + (d >> 1) - 4, g = p[1] + (d >> 2) - 2, b = p[2] + (d >> 1) - 4;
            r = r < 0 ? 0 : r > 255 ? 255 : r;
            g = g < 0 ? 0 : g > 255 ? 255 : g;
            b = b < 0 ? 0 : b > 255 ? 255 : b;
            out[y * size + x] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        }
}

/*
 * Shrink an RGB888 picture (sw x sh, row stride `stride` bytes) to dw x dh
 * (written at `out`, row stride `ostride` bytes) by averaging the source
 * pixels under each output pixel. Integer only: fast on the H2's FPU-light
 * core, and for downscaling album art it looks the same as a windowed filter.
 * (A cover's square, and a logo fitted whole: neither is over 136 pixels
 * across, of pictures decoded small, so the products fit an int.)
 */
static void area_downscale(const unsigned char *in, int sw, int sh, int stride, unsigned char *out, int dw, int dh, int ostride)
{
    for (int oy = 0; oy < dh; oy++) {
        int y0 = oy * sh / dh, y1 = (oy + 1) * sh / dh;
        if (y1 <= y0) y1 = y0 + 1;
        for (int ox = 0; ox < dw; ox++) {
            int x0 = ox * sw / dw, x1 = (ox + 1) * sw / dw;
            if (x1 <= x0) x1 = x0 + 1;
            unsigned r = 0, g = 0, b = 0, n = (unsigned)((y1 - y0) * (x1 - x0));
            for (int y = y0; y < y1; y++) {
                const unsigned char *p = in + (size_t)y * stride + (size_t)x0 * 3;
                for (int x = x0; x < x1; x++, p += 3) { r += p[0]; g += p[1]; b += p[2]; }
            }
            unsigned char *o = out + (size_t)oy * ostride + (size_t)ox * 3;
            o[0] = (unsigned char)((r + n / 2) / n);
            o[1] = (unsigned char)((g + n / 2) / n);
            o[2] = (unsigned char)((b + n / 2) / n);
        }
    }
}

const art_limits_t ART_LIMITS = {FATHOM_JPEG_MAX_MEMORY, FATHOM_ART_MAX_PIXELS, FATHOM_ART_MAX_FILE, 0};

/* Crop to a square and shrink to size x size RGB565: 0, -1 */
int art_square(const unsigned char *img, int w, int h, int size, uint16_t *pixels)
{
    int side = w < h ? w : h;
    const unsigned char *crop = img + 3 * ((size_t)((h - side) / 2) * (size_t)w + (size_t)((w - side) / 2));
    unsigned char *rs = malloc((size_t)size * (size_t)size * 3);
    if (!rs) return -1;
    area_downscale(crop, side, side, w * 3, rs, size, size, size * 3);
    to_rgb565(rs, size, pixels);
    free(rs);
    return 0;
}

/* Whole inside a size x size square, the rest white (a station's logo: a wide
 * one isn't cut at its sides, and white is what its transparent parts are
 * laid over), as RGB565: 0, -1 */
static int art_fit(const unsigned char *img, int w, int h, int size, uint16_t *pixels)
{
    int big = w > h ? w : h;
    int dw = (int)((long long)w * size / big), dh = (int)((long long)h * size / big);
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    unsigned char *rs = malloc((size_t)size * (size_t)size * 3);
    if (!rs) return -1;
    memset(rs, 255, (size_t)size * (size_t)size * 3);
    int ox = (size - dw) / 2, oy = (size - dh) / 2;
    area_downscale(img, w, h, w * 3, rs + 3 * ((size_t)oy * size + (size_t)ox), dw, dh, size * 3);
    to_rgb565(rs, size, pixels);
    free(rs);
    return 0;
}

/* A small picture (a site's favicon: under LIB_LOGO_SHARP_MIN on its short
 * side) as a cover: blown up to the square it was a blur, so it was kept for
 * the list alone and Now Playing showed the radio's sign. Drawn sharp instead,
 * each of its pixels a whole number of the screen's (up to half the square's
 * width), in the middle of a square of its own corners' colour when the four
 * agree (an icon on a ground of its own) and white when they don't (what a
 * logo's transparent parts are laid over). One too wide or tall for that is
 * fitted as any logo. 0, -1 */
static int art_pad(const unsigned char *img, int w, int h, int size, uint16_t *pixels)
{
    int big = w > h ? w : h;
    if (big >= size) return art_fit(img, w, h, size, pixels);
    int k = LIB_LOGO_SHARP_MIN / big;
    if (k < 1) k = 1;
    unsigned char *rs = malloc((size_t)size * (size_t)size * 3);
    if (!rs) return -1;
    const unsigned char *c[4] = {img, img + 3 * (size_t)(w - 1), img + 3 * (size_t)(h - 1) * (size_t)w,
                                 img + 3 * ((size_t)(h - 1) * (size_t)w + (size_t)(w - 1))};
    int same = 1;
    for (int i = 1; i < 4 && same; i++)
        for (int ch = 0; ch < 3; ch++) same &= abs((int)c[i][ch] - (int)c[0][ch]) <= 8;
    unsigned char bg[3] = {255, 255, 255};
    if (same) memcpy(bg, c[0], 3);
    for (size_t i = 0; i < (size_t)size * (size_t)size; i++) memcpy(rs + 3 * i, bg, 3);
    int dw = w * k, dh = h * k, ox = (size - dw) / 2, oy = (size - dh) / 2;
    for (int y = 0; y < dh; y++)
        for (int x = 0; x < dw; x++)
            memcpy(rs + 3 * ((size_t)(oy + y) * size + (size_t)(ox + x)), img + 3 * ((size_t)(y / k) * (size_t)w + (size_t)(x / k)), 3);
    to_rgb565(rs, size, pixels);
    free(rs);
    return 0;
}

/* (the large one of a logo: fitted, or a small picture kept sharp) */
static int logo_large(const unsigned char *img, int w, int h, uint16_t *large)
{
    return (w < h ? w : h) < LIB_LOGO_SHARP_MIN ? art_pad(img, w, h, LIB_ART_LARGE, large)
                                                 : art_fit(img, w, h, LIB_ART_LARGE, large);
}

/* A station's logo comes from anywhere: less of everything than a cover gets */
static const art_limits_t LOGO_LIMITS = {1024L * 1024, 400000LL, 512L * 1024, 1};

int library_logo_decode(const unsigned char *enc, size_t len, uint16_t *large, uint16_t *small, int *short_side)
{
    unsigned char *img;
    int w, h;
    if (art_decode_mem(enc, len, LIB_ART_LARGE * 3 / 2, &LOGO_LIMITS, &img, &w, &h)) return -1;
    if (short_side) *short_side = w < h ? w : h;
    int r = art_fit(img, w, h, LIB_ART_SMALL, small);
    if (r == 0 && large) r = logo_large(img, w, h, large);
    free(img);
    return r;
}

int library_logo_rgb(const unsigned char *rgb, int w, int h, uint16_t *large, uint16_t *small)
{
    if (!rgb || w < 1 || h < 1 || w > 1024 || h > 1024) return -1;
    int r = art_fit(rgb, w, h, LIB_ART_SMALL, small);
    if (r == 0 && large) r = logo_large(rgb, w, h, large);
    return r;
}

int library_art_decode(const unsigned char *enc, size_t len, int size, uint16_t *pixels)
{
    unsigned char *img;
    int w, h;
    if (art_decode_mem(enc, len, size * 3 / 2, &ART_LIMITS, &img, &w, &h)) return -1;
    int r = art_square(img, w, h, size, pixels);
    free(img);
    return r;
}
