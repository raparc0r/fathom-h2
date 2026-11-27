/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/*
 * Cover art (app/library/artdec.c, library.c's covers): big images decoded
 * small within the H2's limits, and where covers are found.
 *
 *   - progressive JPEGs too big for libjpeg's memory, read from their first
 *     pass: the same picture as libjpeg's own 1/8 decode (interleaved and
 *     per-component DC scans, restarts, 4:2:0, 4:4:4, grey)
 *   - PNGs a row at a time (every colour type and depth, every filter, the
 *     data split over many IDAT chunks): exactly the box average
 *   - a FLAC's picture of any type (the front cover first, never an icon)
 *   - the library: a cover in a disc folder's album folder, another track's
 *     when the first has none, and a coverless album looked at again once an
 *     image appears in its folder
 */
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>
#include <jpeglib.h>
#include <sqlite3.h>

#include "core/clock.h"
#include "library/artdec.h"
#include "library/library.h"
#include "library/tags.h"
#include "test_util.h"

static char g_tmp[512];

/* The H2's limits (device/CMakeLists.txt) */
static const art_limits_t H2 = {4194304L, 1200000LL, 4194304L, 0};
static const art_limits_t BIG = {1L << 30, 1LL << 30, 1L << 30, 0};
static const art_limits_t MATTE = {4194304L, 1200000LL, 4194304L, 1};   /* transparency over white (a station's logo) */

/* A picture with detail everywhere: gradients and a pattern */
static unsigned char *picture(int w, int h)
{
    unsigned char *p = malloc((size_t)w * h * 3);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            unsigned char *q = p + 3 * ((size_t)y * w + x);
            q[0] = (unsigned char)(x * 255 / w);
            q[1] = (unsigned char)(y * 255 / h);
            q[2] = (unsigned char)(((x / 37) ^ (y / 23)) & 1 ? 220 : 40);
        }
    return p;
}

/* ---------- JPEG ---------- */

typedef enum { J_BASELINE, J_PROGRESSIVE, J_PROG_PER_COMPONENT } jkind_t;

static long write_jpeg(const char *path, const unsigned char *rgb, int w, int h, jkind_t kind, int grey,
                       int h_samp, int restart)
{
    struct jpeg_compress_struct c;
    struct jpeg_error_mgr e;
    c.err = jpeg_std_error(&e);
    jpeg_create_compress(&c);
    FILE *f = fopen(path, "wb");
    jpeg_stdio_dest(&c, f);
    c.image_width = (JDIMENSION)w, c.image_height = (JDIMENSION)h;
    c.input_components = 3, c.in_color_space = JCS_RGB;
    jpeg_set_defaults(&c);
    if (grey) jpeg_set_colorspace(&c, JCS_GRAYSCALE);
    jpeg_set_quality(&c, 90, TRUE);
    if (!grey) {
        c.comp_info[0].h_samp_factor = h_samp, c.comp_info[0].v_samp_factor = h_samp;
        c.comp_info[1].h_samp_factor = c.comp_info[1].v_samp_factor = 1;
        c.comp_info[2].h_samp_factor = c.comp_info[2].v_samp_factor = 1;
    }
    c.restart_interval = (unsigned)restart;
    static jpeg_scan_info per[6];
    if (kind == J_PROGRESSIVE) jpeg_simple_progression(&c);
    if (kind == J_PROG_PER_COMPONENT && !grey) {    /* each component's DC on its own, then its AC */
        for (int i = 0; i < 3; i++) {
            per[i] = (jpeg_scan_info){1, {i, 0, 0, 0}, 0, 0, 0, 0};
            per[3 + i] = (jpeg_scan_info){1, {i, 0, 0, 0}, 1, 63, 0, 0};
        }
        c.scan_info = per, c.num_scans = 6;
    }
    jpeg_start_compress(&c, TRUE);
    while (c.next_scanline < c.image_height) {
        JSAMPROW row = (JSAMPROW)(rgb + (size_t)c.next_scanline * w * 3);
        jpeg_write_scanlines(&c, &row, 1);
    }
    jpeg_finish_compress(&c);
    jpeg_destroy_compress(&c);
    long n = ftell(f);
    fclose(f);
    return n;
}

/* What the first pass gives exactly: each block's DC from libjpeg's own
 * coefficients (all the memory it wants), as a block average, chroma the
 * nearest block, then YCbCr to RGB */
static unsigned char *jpeg_dc_reference(const char *path, int *w, int *h)
{
    struct jpeg_decompress_struct c;
    struct jpeg_error_mgr e;
    c.err = jpeg_std_error(&e);
    jpeg_create_decompress(&c);
    FILE *f = fopen(path, "rb");
    jpeg_stdio_src(&c, f);
    jpeg_read_header(&c, TRUE);
    jvirt_barray_ptr *coef = jpeg_read_coefficients(&c);
    int nf = c.num_components, hmax = c.max_h_samp_factor, vmax = c.max_v_samp_factor;
    *w = ((int)c.image_width + 7) / 8, *h = ((int)c.image_height + 7) / 8;
    unsigned char *img = malloc((size_t)*w * *h * 3);
    for (int y = 0; y < *h; y++)
        for (int x = 0; x < *w; x++) {
            int v[3];
            for (int i = 0; i < nf; i++) {
                jpeg_component_info *ci = &c.comp_info[i];
                int bx = x * ci->h_samp_factor / hmax, by = y * ci->v_samp_factor / vmax;
                JBLOCKARRAY rows = (*c.mem->access_virt_barray)((j_common_ptr)&c, coef[i], (JDIMENSION)by, 1, FALSE);
                int dc = rows[0][bx][0] * ci->quant_table->quantval[0];
                int s = ((dc + 4) >> 3) + 128;
                v[i] = s < 0 ? 0 : s > 255 ? 255 : s;
            }
            unsigned char *p = img + 3 * ((size_t)y * *w + x);
            if (nf == 1) { p[0] = p[1] = p[2] = (unsigned char)v[0]; continue; }
            double yy = v[0], cb = v[1] - 128, cr = v[2] - 128;
            double rgb[3] = {yy + 1.402 * cr, yy - 0.344136 * cb - 0.714136 * cr, yy + 1.772 * cb};
            for (int k = 0; k < 3; k++) p[k] = (unsigned char)(rgb[k] < 0 ? 0 : rgb[k] > 255 ? 255 : rgb[k] + 0.5);
        }
    jpeg_finish_decompress(&c);
    jpeg_destroy_decompress(&c);
    fclose(f);
    return img;
}

/* libjpeg's own 1/8 decode (each block's DC alone), with all the memory it wants */
static unsigned char *jpeg_eighth(const char *path, int *w, int *h)
{
    struct jpeg_decompress_struct c;
    struct jpeg_error_mgr e;
    c.err = jpeg_std_error(&e);
    jpeg_create_decompress(&c);
    FILE *f = fopen(path, "rb");
    jpeg_stdio_src(&c, f);
    jpeg_read_header(&c, TRUE);
    c.scale_num = 1, c.scale_denom = 8, c.out_color_space = JCS_RGB;
    c.do_fancy_upsampling = FALSE;                  /* (nearest chroma, as the first-pass reader does) */
    jpeg_start_decompress(&c);
    *w = (int)c.output_width, *h = (int)c.output_height;
    unsigned char *img = malloc((size_t)*w * *h * 3);
    while (c.output_scanline < c.output_height) {
        JSAMPROW row = img + (size_t)c.output_scanline * *w * 3;
        jpeg_read_scanlines(&c, &row, 1);
    }
    jpeg_finish_decompress(&c);
    jpeg_destroy_decompress(&c);
    fclose(f);
    return img;
}

static void compare(const unsigned char *a, const unsigned char *b, size_t n, double *mean, int *max)
{
    long long sum = 0;
    *max = 0;
    for (size_t i = 0; i < n; i++) {
        int d = abs((int)a[i] - (int)b[i]);
        sum += d;
        if (d > *max) *max = d;
    }
    *mean = (double)sum / (double)n;
}

/* A progressive JPEG written by hand, side x side at 4:4:4 and all one grey,
 * as small as one can be (a bit a block, for "as the block before"): its
 * tables and its header; with ac_first, a scan of what the first pass has no
 * use for; with twice, its header once more; then the first pass's scan,
 * whole. Its length (o: room for side * side / 170 + 200 bytes). */
static size_t hand_progressive(unsigned char *o, int side, int ac_first, int twice)
{
    static const unsigned char dqt[5] = {0xff, 0xdb, 0, 67, 0};
    static const unsigned char dht[22] = {0xff, 0xc4, 0, 20, 0x00, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x00};   /* one code, of 1 bit */
    static const unsigned char ac_scan[10] = {0xff, 0xda, 0, 8, 1, 1, 0x00, 1, 63, 0};
    static const unsigned char dc_scan[14] = {0xff, 0xda, 0, 12, 3, 1, 0, 2, 0, 3, 0, 0, 0, 0};
    const unsigned char sof[19] = {0xff, 0xc2, 0, 17, 8, (unsigned char)(side >> 8), (unsigned char)side, (unsigned char)(side >> 8),
                                   (unsigned char)side, 3, 1, 0x11, 0, 2, 0x11, 0, 3, 0x11, 0};
    size_t n = 0, blocks = (size_t)((side + 7) / 8) * (size_t)((side + 7) / 8), bytes = (3 * blocks + 7) / 8;
    o[n++] = 0xff, o[n++] = 0xd8;
    memcpy(o + n, dqt, sizeof dqt), n += sizeof dqt;
    memset(o + n, 1, 64), n += 64;
    memcpy(o + n, sof, sizeof sof), n += sizeof sof;
    memcpy(o + n, dht, sizeof dht), n += sizeof dht;
    if (ac_first) memcpy(o + n, ac_scan, sizeof ac_scan), n += sizeof ac_scan;
    if (twice) memcpy(o + n, sof, sizeof sof), n += sizeof sof;
    memcpy(o + n, dc_scan, sizeof dc_scan), n += sizeof dc_scan;
    memset(o + n, 0, bytes), n += bytes;
    o[n++] = 0xff, o[n++] = 0xd9;
    return n;
}

static void test_jpeg(void)
{
    char path[600];
    int W = 3000, Hh = 2000;
    unsigned char *src = picture(W, Hh);
    static const struct { const char *name; jkind_t kind; int grey, samp, restart; } cases[] = {
        {"progressive 4:2:0", J_PROGRESSIVE, 0, 2, 0},
        {"progressive 4:4:4", J_PROGRESSIVE, 0, 1, 0},
        {"progressive grey", J_PROGRESSIVE, 1, 1, 0},
        {"progressive, restarts", J_PROGRESSIVE, 0, 2, 7},
        {"progressive, one component a scan", J_PROG_PER_COMPONENT, 0, 2, 3},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        snprintf(path, sizeof path, "%s/p%zu.jpg", g_tmp, i);
        long size = write_jpeg(path, src, W, Hh, cases[i].kind, cases[i].grey, cases[i].samp, cases[i].restart);
        FILE *f = fopen(path, "rb");
        unsigned char *img = NULL;
        int w = 0, h = 0, rw, rh;
        int r = art_decode(f, 0, -1, 204, &H2, &img, &w, &h);
        fclose(f);
        unsigned char *ref = jpeg_dc_reference(path, &rw, &rh), *ref8 = jpeg_eighth(path, &rw, &rh);
        double mean = 99, mean8 = 99;
        int max = 999, max8 = 999;
        CHECK(r == 0 && w == rw && h == rh);
        if (r == 0 && w == rw && h == rh) {
            compare(img, ref, (size_t)w * h * 3, &mean, &max);
            compare(img, ref8, (size_t)w * h * 3, &mean8, &max8);
        }
        printf("jpeg %-34s %.1f MB: %dx%d, vs its DCs: mean %.2f max %d; vs libjpeg 1/8: mean %.2f max %d\n",
               cases[i].name, size / 1048576.0, w, h, mean, max, mean8, max8);
        CHECK(mean < 1.0);                          /* (the first pass drops the DC's last bit) */
        CHECK(max <= 8);
        free(img);
        free(ref);
        free(ref8);
    }
    /* baseline: libjpeg itself, shrunk while decoding */
    snprintf(path, sizeof path, "%s/b.jpg", g_tmp);
    write_jpeg(path, src, W, Hh, J_BASELINE, 0, 2, 0);
    FILE *f = fopen(path, "rb");
    unsigned char *img = NULL;
    int w, h;
    CHECK(art_decode(f, 0, -1, 204, &H2, &img, &w, &h) == 0 && w == 375 && h == 250);
    fclose(f);
    free(img);
    /* a small progressive one: libjpeg's (it fits), at full size */
    snprintf(path, sizeof path, "%s/s.jpg", g_tmp);
    unsigned char *sm = picture(600, 600);
    write_jpeg(path, sm, 600, 600, J_PROGRESSIVE, 0, 2, 0);
    f = fopen(path, "rb");
    CHECK(art_decode(f, 0, -1, 204, &H2, &img, &w, &h) == 0 && w == 300 && h == 300);   /* (1/2: 300 >= 204) */
    fclose(f);
    free(img);
    free(sm);
    /* not an image, a truncated one */
    snprintf(path, sizeof path, "%s/p0.jpg", g_tmp);
    f = fopen(path, "rb");
    unsigned char *head = malloc(5000);
    CHECK(fread(head, 1, 5000, f) == 5000);
    fclose(f);
    CHECK(art_decode_mem(head, 5000, 204, &H2, &img, &w, &h) == -1);
    CHECK(art_decode_mem((const unsigned char *)"not an image at all", 19, 204, &H2, &img, &w, &h) == -1);
    /* a PNG of grey at 0 bits a sample: refused (it was 8 % 0: SIGFPE on the H2, UBSan here) */
    static const unsigned char zero_depth[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n', 0, 0, 0, 13, 'I', 'H', 'D', 'R',
                                               0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 'I', 'E', 'N', 'D'};
    CHECK(art_decode_mem(zero_depth, sizeof zero_depth, 204, &H2, &img, &w, &h) == -1);
    /* a progressive JPEG's first pass keeps to the limit on a picture's
     * pixels too: 8000 x 8000 is 1000 x 1000 of them, which a cover may be
     * and a station's logo (400,000) may not. (Its 2 bytes a block were
     * allocated whatever it said: 80 MB for a file of 126 bytes that said
     * 30,000 x 30,000.) */
    unsigned char *hand = malloc(9000 * 9000 / 170 + 200);
    static uint16_t large[LIB_ART_LARGE * LIB_ART_LARGE], small[LIB_ART_SMALL * LIB_ART_SMALL];
    int side = 0;
    size_t hn = hand_progressive(hand, 8000, 0, 0);
    CHECK(art_decode_mem(hand, hn, 204, &H2, &img, &w, &h) == 0 && w == 1000 && h == 1000 && img[0] == 128 && img[3 * 1000 * 1000 - 1] == 128);
    free(img);
    CHECK(library_logo_decode(hand, hn, large, small, &side) == -1);
    hn = hand_progressive(hand, 4800, 0, 0);            /* (600 x 600: a logo still) */
    CHECK(library_logo_decode(hand, hn, large, small, &side) == 0 && side == 600);
    hn = hand_progressive(hand, 9000, 0, 0);            /* (1125 x 1125: over a cover's 1,200,000 on the H2) */
    CHECK(art_decode_mem(hand, hn, 204, &H2, &img, &w, &h) == -1);
    hn = hand_progressive(hand, 8, 0, 0);               /* (that file: one block's, its header saying 30,000 each way) */
    hand[76] = hand[78] = 30000 >> 8, hand[77] = hand[79] = 30000 & 255;
    CHECK(hn < 130 && art_decode_mem(hand, hn, 204, &H2, &img, &w, &h) == -1);
    /* its header twice (after a scan, where libjpeg doesn't look): damaged.
     * Each one's blocks were allocated again, the ones before never freed. */
    hn = hand_progressive(hand, 1600, 1, 0);
    CHECK(art_decode_mem(hand, hn, 204, &H2, &img, &w, &h) == 0 && w == 200 && h == 200);
    free(img);
    hn = hand_progressive(hand, 1600, 1, 1);
    CHECK(art_decode_mem(hand, hn, 204, &H2, &img, &w, &h) == -1);
    free(hand);
    free(head);
    free(src);
}

/* ---------- PNG ---------- */

static void put32(unsigned char *p, uint32_t v) { p[0] = (unsigned char)(v >> 24), p[1] = (unsigned char)(v >> 16), p[2] = (unsigned char)(v >> 8), p[3] = (unsigned char)v; }

static void chunk(FILE *f, const char *type, const unsigned char *d, uint32_t n)
{
    unsigned char b[8];
    put32(b, n);
    memcpy(b + 4, type, 4);
    fwrite(b, 1, 8, f);
    if (n) fwrite(d, 1, n, f);
    uLong crc = crc32(crc32(0, (const Bytef *)type, 4), d, n);
    put32(b, (uint32_t)crc);
    fwrite(b, 1, 4, f);
}

static int paeth(int a, int b, int c)
{
    int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

/* A PNG of colour type ct and depth d from rgb (grey: its red; palette: index
 * of a 16-entry palette by red / 16), each row's filter (row % 5), the data
 * over IDAT chunks of `idat` bytes. The RGB the file holds into *expect. */
static void write_png(const char *path, const unsigned char *rgb, int w, int h, int ct, int d, int interlace, size_t idat,
                      unsigned char *expect)
{
    static const int chans[7] = {1, 0, 3, 1, 2, 0, 4};
    int ch = chans[ct];
    size_t bits = (size_t)ch * d, rowbytes = ((size_t)w * bits + 7) / 8, bpp = bits >= 8 ? bits / 8 : 1;
    unsigned char *raw = calloc(1, rowbytes), *prev = calloc(1, rowbytes), *data = malloc((rowbytes + 1) * h);
    unsigned char pal[48];
    for (int i = 0; i < 16; i++) pal[3 * i] = (unsigned char)(i * 17), pal[3 * i + 1] = (unsigned char)(255 - i * 17), pal[3 * i + 2] = (unsigned char)(i * 9);
    for (int y = 0; y < h; y++) {
        memset(raw, 0, rowbytes);
        for (int x = 0; x < w; x++) {
            const unsigned char *s = rgb + 3 * ((size_t)y * w + x);
            unsigned char *e = expect + 3 * ((size_t)y * w + x);
            int v[4];
            if (ct == 3) {
                int idx = s[0] / 16 & ((1 << d) - 1);
                v[0] = idx;
                memcpy(e, pal + 3 * idx, 3);
            } else if (ct == 0 || ct == 4) {
                int max = (1 << (d > 8 ? 8 : d)) - 1;
                int g = s[0] * max / 255;
                v[0] = g, v[1] = 200;
                e[0] = e[1] = e[2] = (unsigned char)(d >= 8 ? g : g * 255 / max);
            } else {
                v[0] = s[0], v[1] = s[1], v[2] = s[2], v[3] = 128;
                memcpy(e, s, 3);
            }
            for (int c = 0; c < ch; c++) {
                if (d == 8) raw[(size_t)x * ch + c] = (unsigned char)v[c];
                else if (d == 16) raw[2 * ((size_t)x * ch + c)] = (unsigned char)v[c], raw[2 * ((size_t)x * ch + c) + 1] = 0x5a;
                else raw[(size_t)x * d / 8] |= (unsigned char)(v[0] << (8 - d - (int)((size_t)x * d % 8)));
            }
        }
        int ft = y % 5;
        unsigned char *o = data + (size_t)y * (rowbytes + 1);
        o[0] = (unsigned char)ft;
        for (size_t i = 0; i < rowbytes; i++) {
            int a = i >= bpp ? raw[i - bpp] : 0, up = prev[i], ul = i >= bpp ? prev[i - bpp] : 0, f = 0;
            if (ft == 1) f = a; else if (ft == 2) f = up; else if (ft == 3) f = (a + up) >> 1; else if (ft == 4) f = paeth(a, up, ul);
            o[1 + i] = (unsigned char)(raw[i] - f);
        }
        memcpy(prev, raw, rowbytes);
    }
    uLongf zn = compressBound((uLong)((rowbytes + 1) * h));
    unsigned char *z = malloc(zn);
    compress2(z, &zn, data, (uLong)((rowbytes + 1) * h), 6);
    FILE *f = fopen(path, "wb");
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    unsigned char hd[13];
    put32(hd, (uint32_t)w), put32(hd + 4, (uint32_t)h);
    hd[8] = (unsigned char)d, hd[9] = (unsigned char)ct, hd[10] = hd[11] = 0, hd[12] = (unsigned char)interlace;
    chunk(f, "IHDR", hd, 13);
    chunk(f, "tEXt", (const unsigned char *)"Comment\0made by test_art", 24);
    if (ct == 3) chunk(f, "PLTE", pal, 48);
    for (size_t at = 0; at < zn; at += idat) chunk(f, "IDAT", z + at, (uint32_t)(zn - at < idat ? zn - at : idat));
    chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(raw), free(prev), free(data), free(z);
}

static void box(const unsigned char *src, int w, int h, int k, unsigned char *out)
{
    int ow = w / k, oh = h / k;
    for (int oy = 0; oy < oh; oy++)
        for (int ox = 0; ox < ow; ox++)
            for (int c = 0; c < 3; c++) {
                unsigned s = 0;
                for (int y = 0; y < k; y++)
                    for (int x = 0; x < k; x++) s += src[3 * ((size_t)(oy * k + y) * w + ox * k + x) + c];
                out[3 * ((size_t)oy * ow + ox) + c] = (unsigned char)((s + (unsigned)(k * k) / 2) / (unsigned)(k * k));
            }
}

static void test_png(void)
{
    char path[600];
    static const struct { int ct, d; } kinds[] = {{2, 8}, {6, 8}, {2, 16}, {0, 8}, {0, 16}, {0, 1}, {0, 4}, {4, 8}, {3, 8}, {3, 4}, {3, 2}};
    int W = 2100, H = 1500;                          /* short side 1500, min 204: shrunk by 7 */
    unsigned char *src = picture(W, H), *expect = malloc((size_t)W * H * 3);
    unsigned char *want = malloc((size_t)(W / 7) * (H / 7) * 3);
    for (size_t i = 0; i < sizeof kinds / sizeof kinds[0]; i++) {
        snprintf(path, sizeof path, "%s/t%zu.png", g_tmp, i);
        write_png(path, src, W, H, kinds[i].ct, kinds[i].d, 0, 8192, expect);
        box(expect, W, H, 7, want);
        FILE *f = fopen(path, "rb");
        unsigned char *img = NULL;
        int w = 0, h = 0;
        int r = art_decode(f, 0, -1, 204, &H2, &img, &w, &h);
        fclose(f);
        CHECK(r == 0 && w == W / 7 && h == H / 7);
        int same = r == 0 && w == W / 7 && h == H / 7 && !memcmp(img, want, (size_t)w * h * 3);
        CHECK(same);
        printf("png colour type %d, %2d bits: %dx%d, %s\n", kinds[i].ct, kinds[i].d, w, h, same ? "as expected" : "DIFFERENT");
        free(img);
    }
    /* a small one: as it is */
    snprintf(path, sizeof path, "%s/small.png", g_tmp);
    write_png(path, src, 150, 120, 2, 8, 0, 1 << 20, expect);
    FILE *f = fopen(path, "rb");
    unsigned char *img = NULL;
    int w, h;
    CHECK(art_decode(f, 0, -1, 204, &H2, &img, &w, &h) == 0 && w == 150 && h == 120);
    fclose(f);
    free(img);
    /* interlaced: whole, within the limits (stb_image) */
    write_png(path, src, 300, 300, 2, 8, 1, 1 << 20, expect);
    f = fopen(path, "rb");
    CHECK(art_decode(f, 0, -1, 204, &H2, &img, &w, &h) == -1);   /* (our writer's rows aren't Adam7: stb refuses it) */
    fclose(f);
    free(img);
    free(src), free(expect), free(want);
}

/* ---------- interlaced PNGs: whole, by stb_image, in memory that is counted ---------- */

/* An interlaced PNG of w x h, every sample 0 (colour type ct at d bits), into
 * b: its length. (All zeros, so only the length of each of Adam7's passes has
 * to be right, and that packs to very little.) */
static size_t interlaced_png(unsigned char *b, size_t cap, uint32_t w, uint32_t h, int ct, int d)
{
    static const uint32_t x0[7] = {0, 4, 0, 2, 0, 1, 0}, y0[7] = {0, 0, 4, 0, 2, 0, 1},
                          dx[7] = {8, 8, 4, 4, 2, 2, 1}, dy[7] = {8, 8, 8, 4, 4, 2, 2};
    int ch = ct == 6 ? 4 : ct == 4 ? 2 : ct == 2 ? 3 : 1;
    size_t raw = 0;
    for (int p = 0; p < 7; p++) {
        size_t pw = (w - x0[p] + dx[p] - 1) / dx[p], ph = (h - y0[p] + dy[p] - 1) / dy[p];
        if (pw && ph) raw += ((pw * (size_t)ch * (size_t)d + 7) / 8 + 1) * ph;
    }
    unsigned char *zeros = calloc(1, raw);
    uLongf zn = compressBound((uLong)raw);
    unsigned char *z = malloc(zn);
    compress2(z, &zn, zeros, (uLong)raw, 1);
    free(zeros);
    unsigned char hd[13];
    put32(hd, w), put32(hd + 4, h);
    hd[8] = (unsigned char)d, hd[9] = (unsigned char)ct, hd[10] = hd[11] = 0, hd[12] = 1;
    FILE *f = fmemopen(b, cap, "wb");
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    chunk(f, "IHDR", hd, 13);
    chunk(f, "IDAT", z, (uint32_t)zn);
    chunk(f, "IEND", NULL, 0);
    size_t n = (size_t)ftell(f);
    fclose(f);
    free(z);
    return n;
}

/* (run first: the memory check needs a process that hasn't held much yet) */
static void test_whole(void)
{
    unsigned char *img = NULL;
    int w = 0, h = 0;
    /* a picture already in memory is decoded from there: 24 MB behind it (never
     * touched, so not yet memory at all) were copied before it was looked at */
    size_t cap = 24u << 20;
    unsigned char *b = calloc(1, cap);
    size_t n = interlaced_png(b, cap, 64, 64, 2, 8);
    CHECK(n > 0 && n < 4096);
    long before = t_peak_kb();
    CHECK(art_decode_mem(b, cap, 204, &BIG, &img, &w, &h) == 0 && w == 64 && h == 64);
    long grew = t_peak_kb() - before;
    if (grew >= 12 * 1024) fprintf(stderr, "decoding from memory took %ld KB\n", grew);
    CHECK(grew < 12 * 1024);
    free(img), img = NULL;

    /* the H2's 1.2 million pixels are 3.6 MB as 8-bit RGB: that is still read */
    n = interlaced_png(b, cap, 1000, 1200, 2, 8);
    CHECK(art_decode_mem(b, n, 204, &H2, &img, &w, &h) == 0 && w == 1000 && h == 1200);
    free(img), img = NULL;
    /* the same pixels at 16 bits with transparency are 9.6 MB, three times over: refused
     * (as many bytes as the limit allows are read: 600 x 750 of them) */
    n = interlaced_png(b, cap, 1000, 1200, 6, 16);
    CHECK(art_decode_mem(b, n, 204, &H2, &img, &w, &h) == -1 && img == NULL);
    free(img), img = NULL;
    n = interlaced_png(b, cap, 600, 750, 6, 16);
    CHECK(art_decode_mem(b, n, 204, &H2, &img, &w, &h) == 0 && w == 600 && h == 750);
    free(img), img = NULL;
    /* 8-bit with transparency over white (a logo) is 4 bytes a pixel as it's read */
    n = interlaced_png(b, cap, 1000, 1200, 6, 8);
    CHECK(art_decode_mem(b, n, 204, &MATTE, &img, &w, &h) == -1 && img == NULL);
    free(img), img = NULL;
    n = interlaced_png(b, cap, 1000, 900, 6, 8);
    CHECK(art_decode_mem(b, n, 204, &MATTE, &img, &w, &h) == 0 && w == 1000 && h == 900 && img[0] == 255);   /* (clear: white) */
    free(img), img = NULL;
    /* and from a file, the same limit */
    char path[600];
    snprintf(path, sizeof path, "%s/deep.png", g_tmp);
    n = interlaced_png(b, cap, 1000, 1200, 6, 16);
    FILE *f = fopen(path, "wb");
    fwrite(b, 1, n, f);
    fclose(f);
    f = fopen(path, "rb");
    CHECK(art_decode(f, 0, -1, 204, &H2, &img, &w, &h) == -1 && img == NULL);
    fclose(f);
    free(img);
    free(b);
}

/* ---------- transparency, and logos fitted whole ---------- */

/* A PNG as it is given: colour type ct (3: a palette of n entries with their
 * transparency in trns; else 4 or 6, 8 bits), rows unfiltered, `raw` its
 * w * h * channels bytes */
static void write_png_raw(const char *path, const unsigned char *raw, int w, int h, int ct, int interlace,
                          const unsigned char *pal, const unsigned char *trns, int n)
{
    int ch = ct == 6 ? 4 : ct == 4 ? 2 : ct == 2 ? 3 : 1;
    size_t rowbytes = (size_t)w * ch;
    unsigned char *data = malloc((rowbytes + 1) * h);
    for (int y = 0; y < h; y++) {
        data[(size_t)y * (rowbytes + 1)] = 0;
        memcpy(data + (size_t)y * (rowbytes + 1) + 1, raw + (size_t)y * rowbytes, rowbytes);
    }
    uLongf zn = compressBound((uLong)((rowbytes + 1) * h));
    unsigned char *z = malloc(zn);
    compress2(z, &zn, data, (uLong)((rowbytes + 1) * h), 6);
    FILE *f = fopen(path, "wb");
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    unsigned char hd[13];
    put32(hd, (uint32_t)w), put32(hd + 4, (uint32_t)h);
    hd[8] = 8, hd[9] = (unsigned char)ct, hd[10] = hd[11] = 0, hd[12] = (unsigned char)interlace;
    chunk(f, "IHDR", hd, 13);
    if (ct == 3) chunk(f, "PLTE", pal, (uint32_t)n * 3);
    if (ct == 3 && trns) chunk(f, "tRNS", trns, (uint32_t)n);
    chunk(f, "IDAT", z, (uint32_t)zn);
    chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(data), free(z);
}

static int decode_file(const char *path, const art_limits_t *lim, unsigned char **img, int *w, int *h)
{
    FILE *f = fopen(path, "rb");
    int r = f ? art_decode(f, 0, -1, 204, lim, img, w, h) : -1;
    if (f) fclose(f);
    return r;
}

#define PX(img, w, x, y) ((img) + 3 * ((size_t)(y) * (w) + (x)))
static int is_rgb(const unsigned char *p, int r, int g, int b) { return p[0] == r && p[1] == g && p[2] == b; }
#define RGB565(r, g, b) ((uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))

/* ---------- files made to take time ---------- */

static void test_slow(void)
{
    unsigned char *img = NULL;
    int w = 0, h = 0;
    /* A progressive JPEG too big for libjpeg's memory (8000 x 1200: 150,000
     * blocks a component), then 20,000 scans of 10 bytes, each the first pass
     * of its first component and no data: each walked every block (3,000
     * million in all: 8 seconds on a computer, far longer on an H2) before the
     * file was refused. */
    size_t cap = 1024 + 20000 * 10, n = 0;
    unsigned char *b = malloc(cap);
    static const unsigned char head[] = {
        0xFF, 0xD8,
        0xFF, 0xDB, 0, 67, 0,                          /* DQT: table 0 (64 values follow) */
    };
    memcpy(b, head, sizeof head), n = sizeof head;
    memset(b + n, 16, 64), n += 64;
    static const unsigned char sof[] = {
        0xFF, 0xC2, 0, 17, 8, 1200 >> 8, 1200 & 255, 8000 >> 8, 8000 & 255, 3, 1, 0x11, 0, 2, 0x11, 0, 3, 0x11, 0,
        0xFF, 0xC4, 0, 20, 0x00, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,   /* DHT, DC 0: the code "0" is a difference of 0 */
    };
    memcpy(b + n, sof, sizeof sof), n += sizeof sof;
    static const unsigned char sos[] = {0xFF, 0xDA, 0, 8, 1, 1, 0x00, 0, 0, 0x00};   /* component 1, Ss 0, Se 0, Ah 0 */
    for (int i = 0; i < 20000; i++) memcpy(b + n, sos, sizeof sos), n += sizeof sos;
    b[n++] = 0xFF, b[n++] = 0xD9;
    double t0 = clock_fsec();
    CHECK(art_decode_mem(b, n, 204, &H2, &img, &w, &h) == -1 && img == NULL);
    double took = clock_fsec() - t0;
    if (took >= 2.0) fprintf(stderr, "a JPEG of 20,000 empty scans took %.1f s\n", took);
    CHECK(took < 2.0);
    free(img), img = NULL;
    free(b);

    /* A PNG of 9000 x 9000 (1 bit a pixel: 10 MB of rows from a 10 KB file) was
     * unpacked row by row to make 204 x 204 of it; at the format's 65,535 a
     * side and 8 bytes a pixel that is 34 GB. Over 64 million pixels: refused. */
    char path[600];
    snprintf(path, sizeof path, "%s/vast.png", g_tmp);
    for (int k = 0; k < 2; k++) {
        uint32_t side = k ? 8000 : 9000;
        size_t raw = (size_t)side * (side / 8 + 1);
        unsigned char *zeros = calloc(1, raw);
        uLongf zn = compressBound((uLong)raw);
        unsigned char *z = malloc(zn);
        compress2(z, &zn, zeros, (uLong)raw, 1);
        unsigned char hd[13] = {0};
        put32(hd, side), put32(hd + 4, side);
        hd[8] = 1;                                     /* 1 bit, grey */
        FILE *f = fopen(path, "wb");
        fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
        chunk(f, "IHDR", hd, 13);
        chunk(f, "IDAT", z, (uint32_t)zn);
        chunk(f, "IEND", NULL, 0);
        fclose(f);
        free(zeros), free(z);
        int r = decode_file(path, &H2, &img, &w, &h);
        if (k) CHECK(r == 0 && w == 8000 / 39 && h == 8000 / 39);   /* (64 million exactly: still read, shrunk by 39) */
        else CHECK(r == -1 && img == NULL);
        free(img), img = NULL;
    }
    unlink(path);
}

static unsigned char *slurp(const char *path, long *n);

static void test_logo(void)
{
    char path[600];
    unsigned char *img = NULL;
    int w = 0, h = 0;
    /* RGBA: a transparent corner (black underneath, as editors leave it), a half-clear band, the rest red */
    enum { W = 64, H = 48 };
    unsigned char *rgba = calloc(W * H, 4);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            unsigned char *p = rgba + 4 * (y * W + x);
            if (x < 16 && y < 16) continue;                       /* 0, 0, 0, 0 */
            if (y >= 40) { p[0] = 0, p[1] = 0, p[2] = 200, p[3] = 128; continue; }
            p[0] = 200, p[1] = 10, p[2] = 20, p[3] = 255;
        }
    snprintf(path, sizeof path, "%s/logo-rgba.png", g_tmp);
    write_png_raw(path, rgba, W, H, 6, 0, NULL, NULL, 0);
    CHECK(decode_file(path, &MATTE, &img, &w, &h) == 0 && w == W && h == H);
    if (img) {
        CHECK(is_rgb(PX(img, w, 3, 3), 255, 255, 255));           /* the corner: white */
        CHECK(is_rgb(PX(img, w, 40, 20), 200, 10, 20));           /* opaque: as it is */
        CHECK(is_rgb(PX(img, w, 30, 44), 127, 127, 227));         /* half clear: half way to white */
    }
    free(img);
    img = NULL;
    CHECK(decode_file(path, &H2, &img, &w, &h) == 0);             /* without the matte: its colours as they are */
    if (img) CHECK(is_rgb(PX(img, w, 3, 3), 0, 0, 0) && is_rgb(PX(img, w, 30, 44), 0, 0, 200));
    free(img);
    img = NULL;

    /* grey with alpha */
    unsigned char *ga = calloc(W * H, 2);
    for (int i = 0; i < W * H; i++) ga[2 * i] = 60, ga[2 * i + 1] = (unsigned char)(i % W < 8 ? 0 : 255);
    snprintf(path, sizeof path, "%s/logo-ga.png", g_tmp);
    write_png_raw(path, ga, W, H, 4, 0, NULL, NULL, 0);
    CHECK(decode_file(path, &MATTE, &img, &w, &h) == 0);
    if (img) CHECK(is_rgb(PX(img, w, 2, 10), 255, 255, 255) && is_rgb(PX(img, w, 20, 10), 60, 60, 60));
    free(img);
    img = NULL;
    free(ga);

    /* a palette with a transparent entry (tRNS) */
    static const unsigned char pal[6] = {9, 9, 9, 0, 120, 240}, trns[2] = {0, 255};
    unsigned char *idx = calloc(W * H, 1);
    for (int i = 0; i < W * H; i++) idx[i] = (unsigned char)(i % W >= 32);
    snprintf(path, sizeof path, "%s/logo-pal.png", g_tmp);
    write_png_raw(path, idx, W, H, 3, 0, pal, trns, 2);
    CHECK(decode_file(path, &MATTE, &img, &w, &h) == 0);
    if (img) CHECK(is_rgb(PX(img, w, 5, 5), 255, 255, 255) && is_rgb(PX(img, w, 50, 5), 0, 120, 240));
    free(img);
    img = NULL;
    CHECK(decode_file(path, &H2, &img, &w, &h) == 0);
    if (img) CHECK(is_rgb(PX(img, w, 5, 5), 9, 9, 9));
    free(img);
    img = NULL;
    free(idx);

    /* decoded whole (an interlaced PNG, through stb_image): the same. One
     * pixel, which is laid out the same interlaced or not. */
    static const unsigned char clear[4] = {0, 0, 0, 0}, solid[4] = {10, 20, 30, 255};
    snprintf(path, sizeof path, "%s/logo-1.png", g_tmp);
    write_png_raw(path, clear, 1, 1, 6, 1, NULL, NULL, 0);
    CHECK(decode_file(path, &MATTE, &img, &w, &h) == 0 && w == 1 && h == 1);
    if (img) CHECK(is_rgb(img, 255, 255, 255));
    free(img);
    img = NULL;
    CHECK(decode_file(path, &H2, &img, &w, &h) == 0);
    if (img) CHECK(is_rgb(img, 0, 0, 0));
    free(img);
    img = NULL;
    write_png_raw(path, solid, 1, 1, 6, 1, NULL, NULL, 0);
    CHECK(decode_file(path, &MATTE, &img, &w, &h) == 0);
    if (img) CHECK(is_rgb(img, 10, 20, 30));
    free(img);
    img = NULL;

    /* a station's logo: the RGBA one above, whole inside each square. 64x48,
     * under the size that fills the large one: there it is as it is, sharp, in
     * the middle (from 36 to 99 across, 44 to 91 down), white around it (its
     * corners aren't of one colour: one is clear); in the small one 34x25,
     * white above and below. (It was stretched to 136x102 for the large one,
     * which was then never used: too small to show, the caller found.) */
    static uint16_t large[LIB_ART_LARGE * LIB_ART_LARGE], small[LIB_ART_SMALL * LIB_ART_SMALL];
    long n = 0;
    snprintf(path, sizeof path, "%s/logo-rgba.png", g_tmp);
    unsigned char *enc = slurp(path, &n);
    int side = 0;
    CHECK(enc && library_logo_decode(enc, (size_t)n, large, small, &side) == 0 && side == 48);
    const uint16_t white = 0xFFFF, red = RGB565(200, 10, 20);
    CHECK(large[0] == white && large[10 * LIB_ART_LARGE + 60] == white);              /* above the picture */
    CHECK(large[(LIB_ART_LARGE - 5) * LIB_ART_LARGE + 60] == white);                 /* below it */
    CHECK(large[68 * LIB_ART_LARGE + 20] == white && large[68 * LIB_ART_LARGE + 110] == white);   /* beside it */
    CHECK(large[43 * LIB_ART_LARGE + 90] == white && large[92 * LIB_ART_LARGE + 90] == white);     /* just above, just below */
    uint16_t mid = large[68 * LIB_ART_LARGE + 90];                                   /* red (dithered: within a step) */
    CHECK(abs((mid >> 11) - (red >> 11)) <= 1 && abs(((mid >> 5) & 63) - ((red >> 5) & 63)) <= 1 && abs((mid & 31) - (red & 31)) <= 1);
    CHECK(large[68 * LIB_ART_LARGE + 99] != white && large[68 * LIB_ART_LARGE + 100] == white);   /* its last column, and past it */
    CHECK(small[0] == white && small[33 * LIB_ART_SMALL + 17] == white && small[17 * LIB_ART_SMALL + 20] != white);
    memset(small, 0, sizeof small);
    CHECK(enc && library_logo_decode(enc, (size_t)n, NULL, small, NULL) == 0 && small[0] == white);   /* the small one alone */
    free(enc);
    /* a tall one: white at its sides */
    unsigned char *tall = malloc(40 * 160 * 3);
    for (int i = 0; i < 40 * 160; i++) tall[3 * i] = 0, tall[3 * i + 1] = 90, tall[3 * i + 2] = 0;
    snprintf(path, sizeof path, "%s/logo-tall.png", g_tmp);
    write_png_raw(path, tall, 40, 160, 2, 0, NULL, NULL, 0);
    enc = slurp(path, &n);
    CHECK(enc && library_logo_decode(enc, (size_t)n, large, small, &side) == 0 && side == 40);
    CHECK(large[68 * LIB_ART_LARGE + 10] == white && large[68 * LIB_ART_LARGE + 125] == white && large[68 * LIB_ART_LARGE + 68] != white);
    CHECK(large[68] != white && large[135 * LIB_ART_LARGE + 68] != white);           /* top to bottom */
    free(enc);
    free(tall);
    /* not a picture; and one over a logo's limits (400,000 pixels decoded) */
    CHECK(library_logo_decode((const unsigned char *)"<svg xmlns='http://www.w3.org/2000/svg'/>", 41, large, small, &side) == -1);
    unsigned char *wide = calloc(2000 * 201, 3);
    snprintf(path, sizeof path, "%s/logo-wide.png", g_tmp);
    write_png_raw(path, wide, 2000, 201, 2, 0, NULL, NULL, 0);
    enc = slurp(path, &n);
    CHECK(enc && library_logo_decode(enc, (size_t)n, large, small, &side) == -1);
    free(enc);
    free(wide);
    free(rgba);
}

/* ---------- FLAC pictures and the library ---------- */

static void put24(unsigned char *p, uint32_t v) { p[0] = (unsigned char)(v >> 16), p[1] = (unsigned char)(v >> 8), p[2] = (unsigned char)v; }

static unsigned char *slurp(const char *path, long *n)
{
    size_t len = 0;
    unsigned char *b = (unsigned char *)t_slurp(path, &len);
    if (!b) { printf("FAIL: can't read %s\n", path); exit(1); }
    *n = (long)len;
    return b;
}

/* A FLAC with no audio: STREAMINFO, tags, and pictures of these types from
 * these files (types[i] < 0: none) */
static void write_flac(const char *path, const char *album, const char *title, int track, const int *types,
                       const char *const *images, int n)
{
    FILE *f = fopen(path, "wb");
    fwrite("fLaC", 1, 4, f);
    unsigned char h[4], si[34] = {0};
    h[0] = 0, put24(h + 1, 34);
    fwrite(h, 1, 4, f);
    si[0] = 0x10, si[2] = 0x10;                      /* block sizes 4096 */
    uint32_t rate = 44100;                           /* 20 bits rate, 3 channels-1, 5 bps-1, 36 samples */
    si[10] = (unsigned char)(rate >> 12), si[11] = (unsigned char)(rate >> 4);
    si[12] = (unsigned char)((rate & 15) << 4 | (1 << 1) | 0);
    si[13] = (unsigned char)(15 << 4);
    si[17] = 0xAC, si[16] = 0x44;                    /* 44100 samples */
    fwrite(si, 1, 34, f);
    char tags[3][128];
    snprintf(tags[0], 128, "ALBUM=%s", album);
    snprintf(tags[1], 128, "TITLE=%s", title);
    snprintf(tags[2], 128, "TRACKNUMBER=%d", track);
    uint32_t vl = 4 + 3 + 4;
    for (int i = 0; i < 3; i++) vl += 4 + (uint32_t)strlen(tags[i]);
    h[0] = 4 | (n ? 0 : 0x80), put24(h + 1, vl);
    fwrite(h, 1, 4, f);
    unsigned char l[4] = {3, 0, 0, 0};
    fwrite(l, 1, 4, f);
    fwrite("art", 1, 3, f);
    l[0] = 3;
    fwrite(l, 1, 4, f);
    for (int i = 0; i < 3; i++) {
        uint32_t tl = (uint32_t)strlen(tags[i]);
        unsigned char b[4] = {(unsigned char)tl, (unsigned char)(tl >> 8), 0, 0};
        fwrite(b, 1, 4, f);
        fwrite(tags[i], 1, tl, f);
    }
    for (int i = 0; i < n; i++) {
        long len;
        unsigned char *img = slurp(images[i], &len);
        const char *mime = "image/png";
        uint32_t bl = 4 + 4 + (uint32_t)strlen(mime) + 4 + 4 + 16 + 4 + (uint32_t)len;
        h[0] = (unsigned char)(6 | (i == n - 1 ? 0x80 : 0)), put24(h + 1, bl);
        fwrite(h, 1, 4, f);
        unsigned char b[4];
        put32(b, (uint32_t)types[i]); fwrite(b, 1, 4, f);
        put32(b, (uint32_t)strlen(mime)); fwrite(b, 1, 4, f);
        fwrite(mime, 1, strlen(mime), f);
        put32(b, 4); fwrite(b, 1, 4, f);
        fwrite("desc", 1, 4, f);
        unsigned char dims[16] = {0};
        fwrite(dims, 1, 16, f);
        put32(b, (uint32_t)len); fwrite(b, 1, 4, f);
        fwrite(img, 1, (size_t)len, f);
        free(img);
    }
    fclose(f);
}

static int64_t album_id_of(const char *db, const char *title)
{
    sqlite3 *raw;
    sqlite3_stmt *s = NULL;
    int64_t id = -1;
    if (sqlite3_open(db, &raw) != SQLITE_OK) return -1;
    if (sqlite3_prepare_v2(raw, "SELECT id FROM albums WHERE title=?", -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_text(s, 1, title, -1, SQLITE_STATIC);
        if (sqlite3_step(s) == SQLITE_ROW) id = sqlite3_column_int64(s, 0);
    }
    sqlite3_finalize(s);
    sqlite3_close(raw);
    return id;
}

static int album_art(const char *db_path, const char *album)
{
    sqlite3 *db;
    int art = -1;
    if (sqlite3_open(db_path, &db) != SQLITE_OK) return -1;
    sqlite3_stmt *q;
    if (sqlite3_prepare_v2(db, "SELECT art FROM albums WHERE title=?", -1, &q, NULL) == SQLITE_OK) {
        sqlite3_bind_text(q, 1, album, -1, SQLITE_STATIC);
        if (sqlite3_step(q) == SQLITE_ROW) art = sqlite3_column_int(q, 0);
        sqlite3_finalize(q);
    }
    sqlite3_close(db);
    return art;
}

static void mk(const char *fmt, const char *a)
{
    char p[700];
    snprintf(p, sizeof p, fmt, g_tmp, a);
    mkdir(p, 0755);
}

static void test_library(void)
{
    char p[700], q[700], db[600], art[600], music[600];
    unsigned char *src = picture(400, 400), *expect = malloc(400 * 400 * 3);
    char red[600], icon[600], big[600];
    snprintf(red, sizeof red, "%s/red.png", g_tmp);
    write_png(red, src, 400, 400, 2, 8, 0, 1 << 20, expect);
    snprintf(icon, sizeof icon, "%s/icon.png", g_tmp);
    write_png(icon, src, 32, 32, 2, 8, 0, 1 << 20, expect);
    /* a progressive JPEG over the H2's 4 MB, as a disc's album cover */
    unsigned char *bsrc = picture(3000, 3000);
    snprintf(big, sizeof big, "%s/big.jpg", g_tmp);
    long bigsize = write_jpeg(big, bsrc, 3000, 3000, J_PROGRESSIVE, 0, 2, 0);
    free(bsrc);

    /* FLAC picture types: "other" alone counts; the front cover wins; an icon never */
    const int t_other[] = {0}, t_both[] = {1, 0, 3}, t_icon[] = {1};
    const char *one[] = {red}, *three[] = {icon, red, red};
    long off, len;
    snprintf(p, sizeof p, "%s/types.flac", g_tmp);
    write_flac(p, "x", "x", 1, t_other, one, 1);
    CHECK(tags_picture_at(p, &off, &len) == 0 && len > 1000);
    tags_t t;
    CHECK(tags_read(p, &t) == 0 && t.has_picture);
    write_flac(p, "x", "x", 1, t_both, three, 3);
    CHECK(tags_picture_at(p, &off, &len) == 0);
    long first_red = off;
    write_flac(p, "x", "x", 1, t_icon, one, 1);
    CHECK(tags_picture_at(p, &off, &len) == -1);
    CHECK(tags_read(p, &t) == 0 && !t.has_picture);
    unsigned char *enc;
    size_t n;
    CHECK(tags_picture(p, &enc, &n) == -1);
    write_flac(p, "x", "x", 1, t_both, three, 3);
    CHECK(tags_picture(p, &enc, &n) == 0 && (long)n == len);
    free(enc);
    CHECK(first_red > 0);
    unlink(p);

    snprintf(music, sizeof music, "%s/music", g_tmp);
    mkdir(music, 0755);
    /* A: the cover only as an "other" picture */
    mk("%s/music/%s", "A");
    snprintf(p, sizeof p, "%s/A/01.flac", music);
    write_flac(p, "Album A", "a1", 1, t_other, one, 1);
    /* B: two discs, the cover beside them */
    mk("%s/music/%s", "B");
    mk("%s/music/B/%s", "CD1");
    mk("%s/music/B/%s", "CD2");
    snprintf(p, sizeof p, "%s/B/CD1/01.flac", music);
    write_flac(p, "Album B", "b1", 1, NULL, NULL, 0);
    snprintf(p, sizeof p, "%s/B/CD2/01.flac", music);
    write_flac(p, "Album B", "b2", 2, NULL, NULL, 0);
    snprintf(p, sizeof p, "%s/B/cover.jpg", music);
    long bl;
    unsigned char *bb = slurp(big, &bl);
    FILE *f = fopen(p, "wb");
    fwrite(bb, 1, (size_t)bl, f);
    fclose(f);
    free(bb);
    /* C: the first track has no picture, the second has */
    mk("%s/music/%s", "C");
    snprintf(p, sizeof p, "%s/C/01.flac", music);
    write_flac(p, "Album C", "c1", 1, NULL, NULL, 0);
    snprintf(p, sizeof p, "%s/C/02.flac", music);
    write_flac(p, "Album C", "c2", 2, t_other, one, 1);
    /* D: none, until a cover.png appears */
    mk("%s/music/%s", "D");
    snprintf(p, sizeof p, "%s/D/01.flac", music);
    write_flac(p, "Album D", "d1", 1, NULL, NULL, 0);

    snprintf(db, sizeof db, "%s/lib.db", g_tmp);
    snprintf(art, sizeof art, "%s/art", g_tmp);
    mkdir(art, 0755);
    library_t *lib = library_open(db, art);
    CHECK(lib && library_scan(lib, music, NULL, NULL) == 6);
    CHECK(!library_needs_art(lib));                 /* (D: looked at by this version) */
    library_close(lib);
    printf("library: A %d, B %d (a %.1f MB progressive cover.jpg above its discs), C %d, D %d (1: a cover)\n",
           album_art(db, "Album A"), album_art(db, "Album B"), bigsize / 1048576.0, album_art(db, "Album C"),
           album_art(db, "Album D"));
    CHECK(album_art(db, "Album A") == 1);
    CHECK(album_art(db, "Album B") == 1);
    CHECK(album_art(db, "Album C") == 1);
    CHECK(album_art(db, "Album D") == 2);
    /* covers found before found covers had a signature (0): kept, noted */
    {
        sqlite3 *rw;
        CHECK(sqlite3_open(db, &rw) == SQLITE_OK &&
              sqlite3_exec(rw, "UPDATE albums SET art_sig=0 WHERE art=1", NULL, NULL, NULL) == SQLITE_OK);
        sqlite3_close(rw);
        lib = library_open(db, art);
        CHECK(lib && library_scan(lib, music, NULL, NULL) == 6);
        library_close(lib);
        CHECK(album_art(db, "Album A") == 1 && album_art(db, "Album B") == 1 && album_art(db, "Album C") == 1);
        sqlite3_stmt *z = NULL;
        CHECK(sqlite3_open(db, &rw) == SQLITE_OK &&
              sqlite3_prepare_v2(rw, "SELECT COUNT(*) FROM albums WHERE art=1 AND art_sig=0", -1, &z, NULL) == SQLITE_OK);
        CHECK(sqlite3_step(z) == SQLITE_ROW && sqlite3_column_int(z, 0) == 0);
        sqlite3_finalize(z);
        sqlite3_close(rw);
    }
    /* a library from before: D's fingerprint unknown, so a start scans once */
    sqlite3 *raw;
    CHECK(sqlite3_open(db, &raw) == SQLITE_OK && sqlite3_exec(raw, "UPDATE albums SET art_sig=0 WHERE art=2", NULL, NULL, NULL) == SQLITE_OK);
    sqlite3_close(raw);
    lib = library_open(db, art);
    CHECK(lib && library_needs_art(lib));
    CHECK(library_scan(lib, music, NULL, NULL) == 6 && !library_needs_art(lib));
    library_close(lib);
    /* a scan's cover pass cut short (the player stopped or restarted mid-way):
     * the albums it hadn't reached yet, never looked at, get a scan at the next
     * start, and their covers */
    CHECK(sqlite3_open(db, &raw) == SQLITE_OK &&
          sqlite3_exec(raw, "UPDATE albums SET art=0, art_sig=0 WHERE title='Album A'", NULL, NULL, NULL) == SQLITE_OK);
    sqlite3_close(raw);
    lib = library_open(db, art);
    CHECK(lib && library_needs_art(lib));
    CHECK(library_scan(lib, music, NULL, NULL) == 6 && !library_needs_art(lib));
    library_close(lib);
    CHECK(album_art(db, "Album A") == 1);
    /* nothing changed: D isn't looked at again (no change to count) */
    lib = library_open(db, art);
    CHECK(lib && library_scan(lib, music, NULL, NULL) == 6);
    library_close(lib);
    CHECK(album_art(db, "Album D") == 2);
    /* an image put in its folder (no track changes): found at the next scan */
    snprintf(q, sizeof q, "%s/D/cover.png", music);
    write_png(q, src, 400, 400, 2, 8, 0, 1 << 20, expect);
    lib = library_open(db, art);
    CHECK(lib && library_scan(lib, music, NULL, NULL) == 6);
    library_close(lib);
    CHECK(album_art(db, "Album D") == 1);
    /* its cover.png replaced by another picture: the cover follows */
    {
        int64_t dd = album_id_of(db, "Album D");
        size_t px_n = (size_t)LIB_ART_SMALL * LIB_ART_SMALL;
        uint16_t *pxa = malloc(px_n * 2), *pxb = malloc(px_n * 2);
        unsigned char *flat = malloc(400 * 400 * 3), *flat_expect = malloc(400 * 400 * 3);
        for (int i = 0; i < 400 * 400; i++) flat[3 * i] = 220, flat[3 * i + 1] = 20, flat[3 * i + 2] = 20;
        lib = library_open(db, art);
        CHECK(lib && pxa && pxb && flat && flat_expect && library_art(lib, dd, LIB_ART_SMALL, pxa) == 0);
        library_close(lib);
        snprintf(q, sizeof q, "%s/D/cover.png", music);
        write_png(q, flat, 400, 400, 2, 8, 0, 1 << 20, flat_expect);
        lib = library_open(db, art);
        CHECK(lib && library_scan(lib, music, NULL, NULL) == 6);
        CHECK(library_art(lib, dd, LIB_ART_SMALL, pxb) == 0 && memcmp(pxa, pxb, px_n * 2));
        library_close(lib);
        CHECK(album_art(db, "Album D") == 1);
        /* ...and removed: no cover, no thumbnails */
        unlink(q);
        lib = library_open(db, art);
        CHECK(lib && library_scan(lib, music, NULL, NULL) == 6);
        CHECK(library_art(lib, dd, LIB_ART_SMALL, pxb) == -1);
        library_close(lib);
        CHECK(album_art(db, "Album D") == 2);
        free(pxa), free(pxb), free(flat), free(flat_expect);
    }
    /* The covers' store (large.pack, small.pack): no file per album; a cover
     * from before it (a file per album and size) is read where it is, then
     * moved in by a scan and its file removed; a slot a power cut left half
     * written reads as no cover. */
    {
        int64_t aa = album_id_of(db, "Album A");
        size_t ns = (size_t)LIB_ART_SMALL * LIB_ART_SMALL * 2, nl = (size_t)LIB_ART_LARGE * LIB_ART_LARGE * 2;
        uint16_t *s0 = malloc(ns), *l0 = malloc(nl), *s1 = malloc(ns), *l1 = malloc(nl);
        char small[700], large[700], olds[700], oldl[700];
        snprintf(small, sizeof small, "%s/small.pack", art);
        snprintf(large, sizeof large, "%s/large.pack", art);
        snprintf(olds, sizeof olds, "%s/%lld-%d.rgb565", art, (long long)aa, LIB_ART_SMALL);
        snprintf(oldl, sizeof oldl, "%s/%lld-%d.rgb565", art, (long long)aa, LIB_ART_LARGE);
        lib = library_open(db, art);
        CHECK(lib && s0 && l0 && s1 && l1 && !library_art_legacy(lib));
        CHECK(library_art(lib, aa, LIB_ART_SMALL, s0) == 0 && library_art(lib, aa, LIB_ART_LARGE, l0) == 0);
        CHECK(library_art(lib, 0, LIB_ART_SMALL, s1) == -1 && library_art(lib, aa + 1000, LIB_ART_SMALL, s1) == -1);
        CHECK(library_art(lib, aa, 77, s1) == -1);                   /* (no such size) */
        library_close(lib);
        struct stat pst;
        CHECK(stat(small, &pst) == 0 && pst.st_size > 0 && stat(large, &pst) == 0 && pst.st_size > 0);
        CHECK(access(olds, F_OK) != 0);
        /* as 0.15.1 left it: the files, an empty store */
        FILE *of = fopen(olds, "wb");
        CHECK(of && fwrite(s0, 1, ns, of) == ns);
        if (of) fclose(of);
        of = fopen(oldl, "wb");
        CHECK(of && fwrite(l0, 1, nl, of) == nl);
        if (of) fclose(of);
        CHECK(truncate(small, 0) == 0 && truncate(large, 0) == 0);
        lib = library_open(db, art);
        CHECK(lib && library_art_legacy(lib));
        CHECK(library_art(lib, aa, LIB_ART_SMALL, s1) == 0 && !memcmp(s0, s1, ns));   /* read where it is */
        CHECK(library_art(lib, aa, LIB_ART_LARGE, l1) == 0 && !memcmp(l0, l1, nl));
        CHECK(access(olds, F_OK) == 0);                                              /* (not moved by a look) */
        CHECK(library_scan(lib, music, NULL, NULL) == 6);                            /* moved in */
        CHECK(!library_art_legacy(lib) && access(olds, F_OK) != 0 && access(oldl, F_OK) != 0);
        memset(s1, 0, ns);
        memset(l1, 0, nl);
        CHECK(library_art(lib, aa, LIB_ART_SMALL, s1) == 0 && !memcmp(s0, s1, ns));
        CHECK(library_art(lib, aa, LIB_ART_LARGE, l1) == 0 && !memcmp(l0, l1, nl));
        library_close(lib);
        /* a slot cut half way: its last bytes never written */
        int fd = open(large, O_RDWR);
        off_t at = (off_t)aa * (off_t)(8 + nl) + 8 + (off_t)nl - 64;
        char zeros[64] = {0};
        l0[nl / 2 - 1] ^= 0x5555;                                    /* (so they weren't zeros already) */
        CHECK(fd >= 0 && pwrite(fd, &l0[nl / 2 - 32], 64, at) == 64 && pwrite(fd, zeros, 32, at) == 32);
        if (fd >= 0) close(fd);
        lib = library_open(db, art);
        CHECK(lib && library_art(lib, aa, LIB_ART_LARGE, l1) == -1);
        CHECK(library_art(lib, aa, LIB_ART_SMALL, s1) == 0);         /* (the other is whole) */
        library_close(lib);
        /* ...put right for the tests that follow: the album looked at again */
        {
            sqlite3 *rdb = NULL;
            CHECK(sqlite3_open(db, &rdb) == SQLITE_OK &&
                  sqlite3_exec(rdb, "UPDATE albums SET art=0, art_sig=0 WHERE title='Album A'", NULL, NULL, NULL) == SQLITE_OK);
            sqlite3_close(rdb);
        }
        lib = library_open(db, art);
        CHECK(lib && library_needs_art(lib) && library_scan(lib, music, NULL, NULL) == 6);
        CHECK(library_art(lib, aa, LIB_ART_LARGE, l1) == 0);
        library_close(lib);
        free(s0), free(l0), free(s1), free(l1);
    }
    /* D removed, then a coverless E: E takes D's id (the highest), and mustn't
     * show D's cover (its thumbnails go with it) */
    int64_t did = album_id_of(db, "Album D");
    snprintf(q, sizeof q, "%s/D/01.flac", music);
    unlink(q);
    snprintf(q, sizeof q, "%s/D/cover.png", music);
    unlink(q);
    snprintf(q, sizeof q, "%s/D", music);
    rmdir(q);
    lib = library_open(db, art);
    CHECK(lib && library_scan(lib, music, NULL, NULL) == 5);   /* D gone */
    library_close(lib);
    mk("%s/music/%s", "E");
    snprintf(p, sizeof p, "%s/E/01.flac", music);
    write_flac(p, "Album E", "e1", 1, NULL, NULL, 0);
    lib = library_open(db, art);
    CHECK(lib && library_scan(lib, music, NULL, NULL) == 6);
    int64_t eid = album_id_of(db, "Album E");
    uint16_t *px = malloc((size_t)LIB_ART_SMALL * LIB_ART_SMALL * 2);
    CHECK(did > 0 && eid == did && px && library_art(lib, eid, LIB_ART_SMALL, px) == -1);
    free(px);
    library_close(lib);

    /* E: a cover found online (net/coverfind.h), kept in its own folder */
    {
        char found_dir[600], jpg[1200], none[1200];
        snprintf(found_dir, sizeof found_dir, "%s/covers", g_tmp);
        library_set_found_covers(found_dir);
        lib = library_open(db, art);
        lib_album_t *list = NULL, e;
        int nl = lib ? library_albums_without_cover(lib, &list) : -1;
        CHECK(nl == 1 && list && list[0].id == eid && !strcmp(list[0].title, "Album E"));
        free(list);
        CHECK(lib && library_album_info(lib, eid, &e) == 0 && library_album_cover(lib, eid) == LIB_COVER_NONE);
        CHECK(library_found_cover_path(e.key, "jpg", jpg, sizeof jpg) == 0 &&
              library_found_cover_path(e.key, "none", none, sizeof none) == 0);
        unsigned char *fsrc = picture(500, 500);
        long fl = write_jpeg(jpg, fsrc, 500, 500, J_BASELINE, 0, 2, 0);
        free(fsrc);
        long el;
        unsigned char *fenc = slurp(jpg, &el);
        uint16_t *lg = malloc((size_t)LIB_ART_LARGE * LIB_ART_LARGE * 2), *sm = malloc((size_t)LIB_ART_SMALL * LIB_ART_SMALL * 2);
        uint16_t *back = malloc((size_t)LIB_ART_SMALL * LIB_ART_SMALL * 2);
        CHECK(fl > 0 && fenc && lg && sm && back && library_art_decode(fenc, (size_t)el, LIB_ART_LARGE, lg) == 0 &&
              library_art_decode(fenc, (size_t)el, LIB_ART_SMALL, sm) == 0);
        free(fenc);
        CHECK(library_store_found_cover(lib, eid, lg, sm) == 0 && library_album_cover(lib, eid) == LIB_COVER_FOUND);
        CHECK(library_art(lib, eid, LIB_ART_SMALL, back) == 0 && !memcmp(back, sm, (size_t)LIB_ART_SMALL * LIB_ART_SMALL * 2));
        nl = library_albums_without_cover(lib, &list);
        CHECK(nl == 0);
        free(list);
        /* a scan, nothing changed: kept */
        CHECK(library_scan(lib, music, NULL, NULL) == 6 && album_art(db, "Album E") == LIB_COVER_FOUND);
        library_close(lib);
        /* its track rewritten (looked at again): the found cover again */
        snprintf(p, sizeof p, "%s/E/01.flac", music);
        sleep(1);
        write_flac(p, "Album E", "e1", 1, NULL, NULL, 0);
        lib = library_open(db, art);
        CHECK(lib && library_scan(lib, music, NULL, NULL) == 6);
        CHECK(album_art(db, "Album E") == LIB_COVER_FOUND && library_art(lib, eid, LIB_ART_SMALL, back) == 0);
        library_close(lib);
        /* a new library (the database lost): found again at the first scan */
        char db2[600], art2[600];
        snprintf(db2, sizeof db2, "%s/lib2.db", g_tmp);
        snprintf(art2, sizeof art2, "%s/art2", g_tmp);
        mkdir(art2, 0755);
        library_t *l2 = library_open(db2, art2);
        CHECK(l2 && library_scan(l2, music, NULL, NULL) == 6);
        library_close(l2);
        CHECK(album_art(db2, "Album E") == LIB_COVER_FOUND);
        /* its own cover put in its folder: that wins; taken away: the found one again */
        snprintf(q, sizeof q, "%s/E/cover.png", music);
        write_png(q, src, 400, 400, 2, 8, 0, 1 << 20, expect);
        lib = library_open(db, art);
        CHECK(lib && library_scan(lib, music, NULL, NULL) == 6);
        library_close(lib);
        CHECK(album_art(db, "Album E") == LIB_COVER_OWN);
        unlink(q);
        lib = library_open(db, art);
        CHECK(lib && library_scan(lib, music, NULL, NULL) == 6);
        CHECK(album_art(db, "Album E") == LIB_COVER_FOUND);
        /* removed: no cover, marked so the batch leaves it, and so it stays */
        CHECK(library_remove_found_cover(lib, eid) == 0 && library_album_cover(lib, eid) == LIB_COVER_NONE);
        struct stat st;
        CHECK(stat(jpg, &st) != 0 && stat(none, &st) == 0 && library_art(lib, eid, LIB_ART_SMALL, back) == -1);
        CHECK(library_scan(lib, music, NULL, NULL) == 6 && album_art(db, "Album E") == LIB_COVER_NONE);

        /* A: a cover of its own, another picked over it (Find another cover) */
        int64_t aid = album_id_of(db, "Album A");
        lib_album_t aa;
        char ajpg[1200], aover[1200], anone[1200];
        uint16_t *own = malloc((size_t)LIB_ART_SMALL * LIB_ART_SMALL * 2);
        CHECK(own && library_album_info(lib, aid, &aa) == 0 && library_album_cover(lib, aid) == LIB_COVER_OWN &&
              library_art(lib, aid, LIB_ART_SMALL, own) == 0);
        library_found_cover_path(aa.key, "jpg", ajpg, sizeof ajpg);
        library_found_cover_path(aa.key, "over", aover, sizeof aover);
        library_found_cover_path(aa.key, "none", anone, sizeof anone);
        {                                              /* the picture found (not A's own) */
            unsigned char *other = picture(500, 500);
            for (int i = 0; i < 500 * 500 * 3; i += 3) other[i] = 255;
            write_jpeg(ajpg, other, 500, 500, J_BASELINE, 0, 2, 0);
            free(other);
        }
        CHECK(library_store_found_cover(lib, aid, lg, sm) == 0 && library_album_cover(lib, aid) == LIB_COVER_PICKED);
        CHECK(stat(aover, &st) == 0);
        CHECK(library_art(lib, aid, LIB_ART_SMALL, back) == 0 && !memcmp(back, sm, (size_t)LIB_ART_SMALL * LIB_ART_SMALL * 2));
        CHECK(library_scan(lib, music, NULL, NULL) == 6 && album_art(db, "Album A") == LIB_COVER_PICKED);
        library_close(lib);
        /* its track rewritten (looked at again): the picked one still wins over its own */
        snprintf(p, sizeof p, "%s/A/01.flac", music);
        sleep(1);
        write_flac(p, "Album A", "a1", 1, t_other, one, 1);
        lib = library_open(db, art);
        CHECK(lib && library_scan(lib, music, NULL, NULL) == 6 && album_art(db, "Album A") == LIB_COVER_PICKED);
        CHECK(library_art(lib, aid, LIB_ART_SMALL, back) == 0 && memcmp(back, own, (size_t)LIB_ART_SMALL * LIB_ART_SMALL * 2));
        /* removed: back to its own, as it was; nothing marked */
        CHECK(library_remove_found_cover(lib, aid) == 0 && library_album_cover(lib, aid) == LIB_COVER_OWN);
        CHECK(library_art(lib, aid, LIB_ART_SMALL, back) == 0 && !memcmp(back, own, (size_t)LIB_ART_SMALL * LIB_ART_SMALL * 2));
        CHECK(stat(ajpg, &st) != 0 && stat(aover, &st) != 0 && stat(anone, &st) != 0);
        CHECK(library_scan(lib, music, NULL, NULL) == 6 && album_art(db, "Album A") == LIB_COVER_OWN);
        free(own);
        library_close(lib);
        library_set_found_covers(NULL);
        free(lg), free(sm), free(back);
    }
    free(src), free(expect);
}

/* An album in one file, split by its sheet, its cover only in the file's
 * own tags: found. (Its tracks' paths are the file's and then "::01@...";
 * the picture's place was asked of the file, and the file then opened by the
 * track's whole path, which is no file: every such album had no cover unless
 * a picture lay beside it.) */
static void test_cue_cover(void)
{
    char p[700], db[600], art[600], music[600], red[600];
    unsigned char *src = picture(400, 400), *expect = malloc(400 * 400 * 3);
    snprintf(red, sizeof red, "%s/cue-red.png", g_tmp);
    write_png(red, src, 400, 400, 2, 8, 0, 1 << 20, expect);
    const int t_front[] = {3};
    const char *one[] = {red};
    snprintf(music, sizeof music, "%s/cuemusic", g_tmp);
    mkdir(music, 0755);
    snprintf(p, sizeof p, "%s/Q", music);
    mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/Q/whole.flac", music);
    write_flac(p, "Album Q", "whole", 1, t_front, one, 1);
    snprintf(p, sizeof p, "%s/Q/whole.cue", music);
    FILE *f = fopen(p, "wb");
    if (f) {
        fputs("TITLE \"Album Q\"\nFILE \"whole.flac\" WAVE\n  TRACK 01 AUDIO\n    TITLE \"First\"\n    INDEX 01 00:00:00\n"
              "  TRACK 02 AUDIO\n    TITLE \"Second\"\n    INDEX 01 00:00:15\n", f);
        fclose(f);
    }
    snprintf(db, sizeof db, "%s/cue.db", g_tmp);
    snprintf(art, sizeof art, "%s/cue-art", g_tmp);
    mkdir(art, 0755);
    library_t *lib = library_open(db, art);
    CHECK(lib && library_scan(lib, music, NULL, NULL) == 2);
    CHECK(album_art(db, "Album Q") == LIB_COVER_OWN);
    if (lib) library_close(lib);
    free(src);
    free(expect);
}

int main(void)
{
    t_mkdtemp(g_tmp, sizeof g_tmp, "fathom-art");
    test_whole();
    test_jpeg();
    test_png();
    test_logo();
    test_slow();
    test_library();
    test_cue_cover();
    t_rm_rf(g_tmp);
    return t_result();
}
