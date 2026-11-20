/* SPDX-License-Identifier: GPL-3.0-or-later */
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
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>
#include <jpeglib.h>
#include <sqlite3.h>

#include "library/artdec.h"
#include "library/library.h"
#include "library/tags.h"

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static char g_tmp[512];

/* The H2's limits (device/CMakeLists.txt) */
static const art_limits_t H2 = {4194304L, 1200000LL, 4194304L};
static const art_limits_t BIG = {1L << 30, 1LL << 30, 1L << 30};

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

/* ---------- FLAC pictures and the library ---------- */

static void put24(unsigned char *p, uint32_t v) { p[0] = (unsigned char)(v >> 16), p[1] = (unsigned char)(v >> 8), p[2] = (unsigned char)v; }

static unsigned char *slurp(const char *path, long *n)
{
    FILE *f = fopen(path, "rb");
    fseek(f, 0, SEEK_END);
    *n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc((size_t)*n);
    if (fread(b, 1, (size_t)*n, f) != (size_t)*n) *n = 0;
    fclose(f);
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
    free(src), free(expect);
}

static void rm_tree(const char *p)
{
    char cmd[700];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", p);
    if (system(cmd)) {}
}

int main(void)
{
    snprintf(g_tmp, sizeof g_tmp, "%s/fathom-art-XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    if (!mkdtemp(g_tmp)) { perror("mkdtemp"); return 1; }
    (void)BIG;
    test_jpeg();
    test_png();
    test_library();
    rm_tree(g_tmp);
    if (failures) { fprintf(stderr, "%d failures\n", failures); return 1; }
    printf("art: ok\n");
    return 0;
}
