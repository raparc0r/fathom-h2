/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * make-logo: draw Fathom's boot screen (320x240): a drop falling into four
 * rings of ripples, and FATHOM below it, in Pixel Pastel's lavenders.
 *
 *   make-logo FONT.ttf OUT.png
 *
 * OUT.png (assets/logo.png) is what the player shows while it starts, and the
 * kernel's boot logo: png2ppm.py turns it into the kernel's format while
 * building, so it keeps to the kernel's 224 colours (every edge is one of 16
 * blends of its colour with the background). FONT is Atkinson Hyperlegible
 * Next Medium, as in the UI. The PNG is checked in; run this again only to
 * change the design.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype_htcw.h"

#define W 320
#define H 240
#define SS 4                 /* supersamples per axis */
#define LEVELS 15            /* edge blends: coverage in 1/15 steps */

static const uint32_t BG = 0x17142A;
static uint8_t g_rgb[H][W][3];

/* Blend `rgb` over the picture with coverage cov[y][x] (0..1), quantised. */
static void paint(float cov[H][W], uint32_t rgb)
{
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            float a = roundf(cov[y][x] * LEVELS) / LEVELS;
            if (a <= 0) continue;
            for (int c = 0; c < 3; c++) {
                float fg = (float)((rgb >> (16 - 8 * c)) & 0xFF);
                g_rgb[y][x][c] = (uint8_t)lroundf(g_rgb[y][x][c] * (1 - a) + fg * a);
            }
        }
}

static float g_cov[H][W];

/* An elliptical ring: centre, radii, stroke width (approximate distance). */
static void ring(float cx, float cy, float rx, float ry, float sw, uint32_t rgb)
{
    memset(g_cov, 0, sizeof g_cov);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            int in = 0;
            for (int sy = 0; sy < SS; sy++)
                for (int sx = 0; sx < SS; sx++) {
                    float px = x + (sx + 0.5f) / SS - cx, py = y + (sy + 0.5f) / SS - cy;
                    float f = px * px / (rx * rx) + py * py / (ry * ry) - 1;
                    float gx = 2 * px / (rx * rx), gy = 2 * py / (ry * ry);
                    float g = sqrtf(gx * gx + gy * gy);
                    if (g > 0 && fabsf(f / g) <= sw / 2) in++;
                }
            g_cov[y][x] = (float)in / (SS * SS);
        }
    paint(g_cov, rgb);
}

/* The drop: two cubic curves from the tip, round at the bottom. */
static void drop(uint32_t rgb)
{
    float pt[2][4][2] = {{{160, 30}, {170, 46}, {172, 56}, {160, 64}},
                         {{160, 64}, {148, 56}, {150, 46}, {160, 30}}};
    float poly[129][2];
    int n = 0;
    for (int c = 0; c < 2; c++)
        for (int i = 0; i < 64; i++) {
            float t = i / 64.0f, u = 1 - t;
            for (int k = 0; k < 2; k++)
                poly[n][k] = u * u * u * pt[c][0][k] + 3 * u * u * t * pt[c][1][k] +
                             3 * u * t * t * pt[c][2][k] + t * t * t * pt[c][3][k];
            n++;
        }
    memset(g_cov, 0, sizeof g_cov);
    for (int y = 20; y < 80; y++)
        for (int x = 130; x < 190; x++) {
            int in = 0;
            for (int sy = 0; sy < SS; sy++)
                for (int sx = 0; sx < SS; sx++) {
                    float px = x + (sx + 0.5f) / SS, py = y + (sy + 0.5f) / SS;
                    int odd = 0;
                    for (int i = 0, j = n - 1; i < n; j = i++)
                        if ((poly[i][1] > py) != (poly[j][1] > py) &&
                            px < (poly[j][0] - poly[i][0]) * (py - poly[i][1]) / (poly[j][1] - poly[i][1]) + poly[i][0])
                            odd = !odd;
                    in += odd;
                }
            g_cov[y][x] = (float)in / (SS * SS);
        }
    paint(g_cov, rgb);
}

/* Text centred on x = 160 with its baseline at `base`, `px` per em, extra `spacing`. */
static int text(const char *font_path, const char *s, float px, float spacing, float base, uint32_t rgb)
{
    FILE *f = fopen(font_path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    unsigned char *data = malloc((size_t)n);
    if (!data || fread(data, 1, (size_t)n, f) != (size_t)n) { fclose(f); return -1; }
    fclose(f);
    stbtt_fontinfo info;
    if (!stbtt_InitFont(&info, data, 0)) return -1;
    float scale = stbtt_ScaleForMappingEmToPixels(&info, px);

    float width = 0;                       /* advance of the whole string */
    for (const char *c = s; *c; c++) {
        int adv, lsb;
        stbtt_GetCodepointHMetrics(&info, *c, &adv, &lsb);
        width += adv * scale + (c[1] ? spacing + stbtt_GetCodepointKernAdvance(&info, c[0], c[1]) * scale : 0);
    }
    memset(g_cov, 0, sizeof g_cov);
    float x = 160 - width / 2;
    for (const char *c = s; *c; c++) {
        int adv, lsb, x0, y0, x1, y1;
        stbtt_GetCodepointHMetrics(&info, *c, &adv, &lsb);
        float fx = x - floorf(x);
        stbtt_GetCodepointBitmapBoxSubpixel(&info, *c, scale, scale, fx, 0, &x0, &y0, &x1, &y1);
        int gw = x1 - x0, gh = y1 - y0;
        unsigned char *bm = calloc((size_t)(gw * gh) + 1, 1);
        stbtt_MakeCodepointBitmapSubpixel(&info, bm, gw, gh, gw, scale, scale, fx, 0, *c);
        for (int j = 0; j < gh; j++)
            for (int i = 0; i < gw; i++) {
                int X = (int)floorf(x) + x0 + i, Y = (int)base + y0 + j;
                if (X >= 0 && X < W && Y >= 0 && Y < H) g_cov[Y][X] = fmaxf(g_cov[Y][X], bm[j * gw + i] / 255.0f);
            }
        free(bm);
        x += adv * scale + (c[1] ? spacing + stbtt_GetCodepointKernAdvance(&info, c[0], c[1]) * scale : 0);
    }
    free(data);
    paint(g_cov, rgb);
    return 0;
}

static void be32(unsigned char *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

static int chunk(FILE *f, const char *type, const unsigned char *d, uint32_t n)
{
    unsigned char b[4];
    be32(b, n);
    fwrite(b, 1, 4, f);
    fwrite(type, 1, 4, f);
    if (n) fwrite(d, 1, n, f);
    uLong crc = crc32(crc32(0, (const Bytef *)type, 4), d, n);
    be32(b, (uint32_t)crc);
    return fwrite(b, 1, 4, f) == 4 ? 0 : -1;
}

static int write_png(const char *path)
{
    static unsigned char raw[H * (1 + W * 3)];
    for (int y = 0; y < H; y++) {
        raw[y * (1 + W * 3)] = 0;          /* filter: none */
        memcpy(raw + y * (1 + W * 3) + 1, g_rgb[y], W * 3);
    }
    uLongf zn = compressBound(sizeof raw);
    unsigned char *z = malloc(zn);
    if (!z || compress2(z, &zn, raw, sizeof raw, 9) != Z_OK) return -1;
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    static const unsigned char sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    unsigned char ihdr[13] = {0};
    be32(ihdr, W);
    be32(ihdr + 4, H);
    ihdr[8] = 8;                           /* 8 bits, RGB */
    ihdr[9] = 2;
    fwrite(sig, 1, 8, f);
    int bad = chunk(f, "IHDR", ihdr, 13) || chunk(f, "IDAT", z, (uint32_t)zn) || chunk(f, "IEND", NULL, 0);
    free(z);
    return fclose(f) || bad ? -1 : 0;
}

int main(int argc, char **argv)
{
    if (argc != 3) { fprintf(stderr, "usage: %s FONT.ttf OUT.png\n", argv[0]); return 2; }
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            for (int c = 0; c < 3; c++) g_rgb[y][x][c] = (BG >> (16 - 8 * c)) & 0xFF;

    drop(0xC3D3E4);
    ring(160, 112, 24, 8, 5, 0xC3D3E4);
    ring(160, 114, 46, 15, 5, 0x9AA7C9);
    ring(160, 116, 70, 23, 5, 0x717BA6);
    ring(160, 118, 94, 31, 5, 0x4E5480);
    if (text(argv[1], "FATHOM", 28, 6, 206, 0xF1EDFF)) { fprintf(stderr, "cannot use font %s\n", argv[1]); return 1; }

    /* the kernel takes at most 224 colours */
    static uint32_t seen[W * H];
    int ncol = 0;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            uint32_t v = (uint32_t)g_rgb[y][x][0] << 16 | g_rgb[y][x][1] << 8 | g_rgb[y][x][2];
            int k = 0;
            while (k < ncol && seen[k] != v) k++;
            if (k == ncol) seen[ncol++] = v;
        }
    if (ncol > 224) { fprintf(stderr, "%d colours: the kernel takes 224 at most\n", ncol); return 1; }
    if (write_png(argv[2])) { fprintf(stderr, "cannot write %s\n", argv[2]); return 1; }
    printf("%s: %dx%d, %d colours\n", argv[2], W, H, ncol);
    return 0;
}
