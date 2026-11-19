/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Cover images decoded small (artdec.h). */
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE
#include "library/artdec.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <zlib.h>

#include "stb/stb_image.h"

#ifdef FATHOM_HAVE_LIBJPEG
#include <setjmp.h>
#include <jpeglib.h>
#endif

/* ---------- anything stb_image reads: whole, within the limits ---------- */

static int whole(FILE *f, long off, long len, const art_limits_t *lim, unsigned char **rgb, int *w, int *h)
{
    if (len < 0) {
        struct stat st;
        if (fstat(fileno(f), &st) || st.st_size <= off) return -1;
        len = (long)st.st_size - off;
    }
    if (len < 8 || len > lim->max_file || fseek(f, off, SEEK_SET)) return -1;
    unsigned char *enc = malloc((size_t)len);
    int ch, r = -1;
    if (enc && fread(enc, 1, (size_t)len, f) == (size_t)len &&
        stbi_info_from_memory(enc, (int)len, w, h, &ch) && (long long)*w * *h <= lim->max_pixels) {
        *rgb = stbi_load_from_memory(enc, (int)len, w, h, &ch, 3);
        r = *rgb ? 0 : -1;
    }
    free(enc);
    return r;
}

/* ---------- PNG, a row at a time ---------- */

static uint32_t be32(const unsigned char *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

typedef struct {
    uint32_t w, h;
    int depth, ctype, channels;
    size_t rowbytes, bpp;            /* bytes a row; bytes a pixel for the filters (at least 1) */
    unsigned char pal[256 * 3];
    int k, ow, oh;                   /* shrunk by k: ow x oh */
    unsigned char *cur, *prev;       /* this row and the one above, each after its filter byte */
    uint32_t *sum;                   /* k rows' pixels added up, ow * 3 */
    unsigned char *out;
    uint32_t y;                      /* rows done */
} png_t;

static int paeth(int a, int b, int c)
{
    int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

/* One sample of the row (channel ch of pixel x), as 8 bits */
static int png_sample(const png_t *p, const unsigned char *r, uint32_t x, int ch)
{
    if (p->depth == 8) return r[x * (size_t)p->channels + (size_t)ch];
    if (p->depth == 16) return r[2 * (x * (size_t)p->channels + (size_t)ch)];
    size_t bit = (size_t)x * (size_t)p->depth;       /* (one channel: gray or a palette index) */
    int v = (r[bit / 8] >> (8 - p->depth - (int)(bit % 8))) & ((1 << p->depth) - 1);
    return p->ctype == 3 ? v : v * 255 / ((1 << p->depth) - 1);
}

static void png_row(png_t *p, int filter)
{
    unsigned char *c = p->cur, *u = p->prev;
    size_t n = p->rowbytes, b = p->bpp;
    for (size_t i = 0; i < n; i++) {
        int a = i >= b ? c[i - b] : 0, up = u[i], ul = i >= b ? u[i - b] : 0;
        switch (filter) {
        case 1: c[i] = (unsigned char)(c[i] + a); break;
        case 2: c[i] = (unsigned char)(c[i] + up); break;
        case 3: c[i] = (unsigned char)(c[i] + ((a + up) >> 1)); break;
        case 4: c[i] = (unsigned char)(c[i] + paeth(a, up, ul)); break;
        default: break;
        }
    }
    uint32_t oy = p->y / (uint32_t)p->k;
    if (oy < (uint32_t)p->oh) {
        for (uint32_t x = 0; x < (uint32_t)(p->ow * p->k); x++) {
            uint32_t *s = p->sum + 3 * (x / (uint32_t)p->k);
            if (p->ctype == 3) {
                const unsigned char *e = p->pal + 3 * png_sample(p, c, x, 0);
                s[0] += e[0], s[1] += e[1], s[2] += e[2];
            } else if (p->ctype == 2 || p->ctype == 6) {
                s[0] += (uint32_t)png_sample(p, c, x, 0), s[1] += (uint32_t)png_sample(p, c, x, 1);
                s[2] += (uint32_t)png_sample(p, c, x, 2);
            } else {                                  /* gray, with or without alpha */
                uint32_t g = (uint32_t)png_sample(p, c, x, 0);
                s[0] += g, s[1] += g, s[2] += g;
            }
        }
        if ((p->y + 1) % (uint32_t)p->k == 0) {
            uint32_t kk = (uint32_t)(p->k * p->k);
            unsigned char *o = p->out + (size_t)oy * (size_t)p->ow * 3;
            for (size_t i = 0; i < (size_t)p->ow * 3; i++) o[i] = (unsigned char)((p->sum[i] + kk / 2) / kk);
            memset(p->sum, 0, (size_t)p->ow * 3 * sizeof *p->sum);
        }
    }
    unsigned char *t = p->prev;
    p->prev = p->cur;
    p->cur = t;
    p->y++;
}

/* 0, -1, or 1: interlaced (not read here) */
static int png_stream(FILE *f, long off, int min_side, const art_limits_t *lim, unsigned char **rgb, int *w, int *h)
{
    png_t p;
    memset(&p, 0, sizeof p);
    unsigned char hd[8], *row = NULL, *in = NULL;
    z_stream z;
    memset(&z, 0, sizeof z);
    int zok = 0, r = -1, have_hdr = 0;
    size_t at = 0;                                    /* bytes of the row (filter byte first) so far */
    if (fseek(f, off + 8, SEEK_SET)) return -1;
    for (;;) {
        if (fread(hd, 1, 8, f) != 8) goto out;
        uint32_t len = be32(hd);
        if (!memcmp(hd + 4, "IHDR", 4)) {
            unsigned char b[13];
            if (len != 13 || fread(b, 1, 13, f) != 13) goto out;
            p.w = be32(b), p.h = be32(b + 4), p.depth = b[8], p.ctype = b[9];
            if (b[12]) { r = 1; goto out; }           /* interlaced */
            static const int chans[7] = {1, 0, 3, 1, 2, 0, 4};
            if (p.ctype > 6 || !(p.channels = chans[p.ctype]) || !p.w || !p.h || p.w > 65535 || p.h > 65535) goto out;
            if (p.depth != 8 && p.depth != 16 && !((p.ctype == 0 || p.ctype == 3) && p.depth < 8 && (8 % p.depth) == 0))
                goto out;
            size_t bits = (size_t)p.channels * (size_t)p.depth;
            p.rowbytes = ((size_t)p.w * bits + 7) / 8;
            p.bpp = bits >= 8 ? bits / 8 : 1;
            uint32_t s = p.w < p.h ? p.w : p.h;
            p.k = min_side > 0 && s / (uint32_t)min_side > 1 ? (int)(s / (uint32_t)min_side) : 1;
            p.ow = (int)(p.w / (uint32_t)p.k), p.oh = (int)(p.h / (uint32_t)p.k);
            if ((long long)p.ow * p.oh > lim->max_pixels) goto out;
            row = calloc(2, p.rowbytes + 1);
            p.sum = calloc((size_t)p.ow * 3, sizeof *p.sum);
            p.out = malloc((size_t)p.ow * (size_t)p.oh * 3);
            in = malloc(16384);
            if (!row || !p.sum || !p.out || !in || inflateInit(&z) != Z_OK) goto out;
            zok = 1;
            p.cur = row + 1, p.prev = row + p.rowbytes + 2;   /* (each after a filter byte's room) */
            have_hdr = 1;
            if (fseek(f, 4, SEEK_CUR)) goto out;      /* its CRC */
            continue;
        }
        if (!have_hdr) goto out;                      /* IHDR comes first */
        if (!memcmp(hd + 4, "PLTE", 4) && len <= sizeof p.pal) {
            if (fread(p.pal, 1, len, f) != len || fseek(f, 4, SEEK_CUR)) goto out;
            continue;
        }
        if (!memcmp(hd + 4, "IDAT", 4)) {
            uint32_t left = len;
            while (left) {
                uInt n = left < 16384 ? (uInt)left : 16384;
                if (fread(in, 1, n, f) != n) goto out;
                left -= n;
                z.next_in = in, z.avail_in = n;
                while (z.avail_in && p.y < p.h) {
                    /* the filter byte goes before cur (cur[-1]), the row into cur */
                    z.next_out = at == 0 ? p.cur - 1 : p.cur + at - 1;
                    z.avail_out = (uInt)(p.rowbytes + 1 - at);
                    int zr = inflate(&z, Z_NO_FLUSH);
                    at = p.rowbytes + 1 - z.avail_out;
                    if (at == p.rowbytes + 1) { int ft = p.cur[-1]; png_row(&p, ft); at = 0; }
                    if (zr == Z_STREAM_END) break;
                    if (zr != Z_OK && zr != Z_BUF_ERROR) goto out;
                }
            }
            if (fseek(f, 4, SEEK_CUR)) goto out;
            if (p.y >= p.h) break;
            continue;
        }
        if (!memcmp(hd + 4, "IEND", 4)) break;
        if (fseek(f, (long)len + 4, SEEK_CUR)) goto out;   /* anything else, and its CRC */
    }
    if (p.y >= (uint32_t)(p.oh * p.k)) {              /* (the rows past the last whole k aren't needed) */
        *rgb = p.out, *w = p.ow, *h = p.oh;
        p.out = NULL;
        r = 0;
    }
out:
    if (zok) inflateEnd(&z);
    free(row);
    free(in);
    free(p.sum);
    free(p.out);
    return r;
}

#ifdef FATHOM_HAVE_LIBJPEG
/* ---------- a progressive JPEG's first pass: the picture at 1/8 ---------- */

typedef struct {
    FILE *f;
    uint32_t bits;
    int nbits, marker;               /* a marker met in the coded data (0: none) */
    int eof;                         /* the file ended in it: truncated */
} br_t;

static int br_byte(br_t *b)
{
    if (b->marker) return 0;
    int c = getc(b->f);
    if (c == EOF) { b->marker = 0xD9; b->eof = 1; return 0; }
    if (c != 0xFF) return c;
    int d;
    do d = getc(b->f); while (d == 0xFF);
    if (d == 0) return 0xFF;
    b->marker = d == EOF ? 0xD9 : d;
    return 0;
}

static int br_bits(br_t *b, int n)
{
    while (b->nbits < n) { b->bits = b->bits << 8 | (uint32_t)br_byte(b); b->nbits += 8; }
    b->nbits -= n;
    int v = (int)((b->bits >> b->nbits) & ((1u << n) - 1));
    b->bits &= (1u << b->nbits) - 1;
    return v;
}

typedef struct { int mincode[17], maxcode[17], valptr[17]; unsigned char vals[256]; int ok; } huff_t;

static int huff_decode(br_t *b, const huff_t *t)
{
    int code = 0;
    for (int l = 1; l <= 16; l++) {
        code = code << 1 | br_bits(b, 1);
        if (code <= t->maxcode[l]) return t->vals[t->valptr[l] + code - t->mincode[l]];
    }
    return -1;
}

typedef struct {
    int id, hs, vs, tq, td, pred, done;
    int bw, bh;                      /* blocks kept a row and a column (whole MCUs) */
    int16_t *dc;
} jcomp_t;

static int rd16(FILE *f) { int a = getc(f), b = getc(f); return a == EOF || b == EOF ? -1 : a << 8 | b; }

/* The bytes of a coded scan skipped, up to the next marker (not a restart) */
static int skip_scan(FILE *f)
{
    for (;;) {
        int c = getc(f);
        if (c == EOF) return 0xD9;
        if (c != 0xFF) continue;
        int d;
        do d = getc(f); while (d == 0xFF);
        if (d == EOF) return 0xD9;
        if (d != 0 && (d < 0xD0 || d > 0xD7)) return d;
    }
}

/* The next marker, whatever it is (a restart's RSTn among them) */
static int next_marker(FILE *f)
{
    for (;;) {
        int c = getc(f);
        if (c == EOF) return 0xD9;
        if (c != 0xFF) continue;
        int d;
        do d = getc(f); while (d == 0xFF);
        if (d == EOF) return 0xD9;
        if (d != 0) return d;
    }
}

static int jpeg_dc(FILE *f, long off, unsigned char **rgb, int *w, int *h)
{
    jcomp_t c[4];
    huff_t dct[4];
    int q0[4] = {0}, nf = 0, X = 0, Y = 0, hmax = 1, vmax = 1, ri = 0, adobe_rgb = 0, r = -1;
    memset(c, 0, sizeof c);
    memset(dct, 0, sizeof dct);
    if (fseek(f, off, SEEK_SET) || getc(f) != 0xFF || getc(f) != 0xD8) return -1;
    int m = 0;
    for (;;) {
        if (!m) {                                     /* the next marker */
            int x = getc(f);
            if (x != 0xFF) goto out;
            do m = getc(f); while (m == 0xFF);
            if (m == EOF) goto out;
        }
        int marker = m;
        m = 0;
        if (marker == 0xD9) break;                     /* EOI */
        if (marker >= 0xD0 && marker <= 0xD7) continue;
        int len = rd16(f);
        if (len < 2) goto out;
        long end = ftell(f) + len - 2;
        if (marker == 0xDB) {                          /* DQT: each table's first (DC) value */
            while (ftell(f) < end) {
                int pq = getc(f);
                if (pq == EOF) goto out;
                int t = pq & 3, big = pq >> 4;
                int v = big ? rd16(f) : getc(f);
                q0[t] = v;
                if (fseek(f, big ? 126 : 63, SEEK_CUR)) goto out;
            }
        } else if (marker == 0xC2) {                   /* SOF2: progressive, Huffman */
            if (getc(f) != 8) goto out;
            Y = rd16(f), X = rd16(f), nf = getc(f);
            if (X <= 0 || Y <= 0 || (nf != 1 && nf != 3)) goto out;
            for (int i = 0; i < nf; i++) {
                c[i].id = getc(f);
                int s = getc(f);
                c[i].hs = s >> 4, c[i].vs = s & 15, c[i].tq = getc(f) & 3;
                if (c[i].hs < 1 || c[i].hs > 4 || c[i].vs < 1 || c[i].vs > 4) goto out;
                if (c[i].hs > hmax) hmax = c[i].hs;
                if (c[i].vs > vmax) vmax = c[i].vs;
            }
            int mx = (X + 8 * hmax - 1) / (8 * hmax), my = (Y + 8 * vmax - 1) / (8 * vmax);
            for (int i = 0; i < nf; i++) {
                c[i].bw = mx * c[i].hs, c[i].bh = my * c[i].vs;
                if (!(c[i].dc = calloc((size_t)c[i].bw * (size_t)c[i].bh, sizeof *c[i].dc))) goto out;
            }
        } else if (marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC) {
            goto out;                                  /* another kind of JPEG: libjpeg's */
        } else if (marker == 0xC4) {                   /* DHT: the DC tables */
            while (ftell(f) < end) {
                int tc = getc(f);
                unsigned char cnt[17];
                int total = 0;
                if (tc == EOF || fread(cnt + 1, 1, 16, f) != 16) goto out;
                for (int l = 1; l <= 16; l++) total += cnt[l];
                if (total > 256) goto out;
                huff_t *t = (tc >> 4) == 0 ? &dct[tc & 3] : NULL;
                unsigned char vals[256];
                if (fread(vals, 1, (size_t)total, f) != (size_t)total) goto out;
                if (!t) continue;
                memcpy(t->vals, vals, (size_t)total);
                int code = 0, k = 0;
                for (int l = 1; l <= 16; l++) {
                    t->valptr[l] = k, t->mincode[l] = code;
                    code += cnt[l], k += cnt[l];
                    t->maxcode[l] = cnt[l] ? code - 1 : -1;
                    code <<= 1;
                }
                t->ok = 1;
            }
        } else if (marker == 0xDD) {                   /* DRI */
            ri = rd16(f);
        } else if (marker == 0xEE && len >= 14) {      /* Adobe: its transform (0: RGB, not YCbCr) */
            unsigned char a[12];
            if (fread(a, 1, 12, f) == 12 && !memcmp(a, "Adobe", 5)) adobe_rgb = a[11] == 0;
        } else if (marker == 0xDA) {                   /* SOS */
            if (!nf) goto out;
            int ns = getc(f);
            jcomp_t *sc[4];
            if (ns < 1 || ns > nf) goto out;
            for (int i = 0; i < ns; i++) {
                int id = getc(f), t = getc(f);
                sc[i] = NULL;
                for (int j = 0; j < nf; j++) if (c[j].id == id) sc[i] = &c[j];
                if (!sc[i]) goto out;
                sc[i]->td = (t >> 4) & 3;
            }
            int ss = getc(f), se = getc(f), a = getc(f);
            int ah = a >> 4, al = a & 15;
            if (fseek(f, end, SEEK_SET)) goto out;
            if (ss != 0 || se != 0 || ah != 0) { m = skip_scan(f); continue; }   /* AC or a refinement */
            br_t b = {f, 0, 0, 0, 0};
            for (int i = 0; i < ns; i++) { sc[i]->pred = 0; if (!dct[sc[i]->td].ok) goto out; }
            int mx, my;
            if (ns == 1) {                             /* one component: its own blocks, one at a time */
                mx = ((X * sc[0]->hs + hmax - 1) / hmax + 7) / 8;
                my = ((Y * sc[0]->vs + vmax - 1) / vmax + 7) / 8;
            } else {
                mx = (X + 8 * hmax - 1) / (8 * hmax), my = (Y + 8 * vmax - 1) / (8 * vmax);
            }
            long n = 0;
            for (int my_ = 0; my_ < my; my_++)
                for (int mx_ = 0; mx_ < mx; mx_++, n++) {
                    if (ri && n && n % ri == 0) {      /* a restart: the next RSTn */
                        b.nbits = 0, b.bits = 0;
                        if (!b.marker) b.marker = next_marker(f);
                        if (b.marker < 0xD0 || b.marker > 0xD7) goto out;
                        b.marker = 0;
                        for (int i = 0; i < ns; i++) sc[i]->pred = 0;
                    }
                    for (int i = 0; i < ns; i++) {
                        jcomp_t *k = sc[i];
                        int bh_ = ns == 1 ? 1 : k->vs, bw_ = ns == 1 ? 1 : k->hs;
                        for (int v = 0; v < bh_; v++)
                            for (int u = 0; u < bw_; u++) {
                                int s = huff_decode(&b, &dct[k->td]);
                                if (s < 0 || s > 15) goto out;
                                int d = 0;
                                if (s) {
                                    d = br_bits(&b, s);
                                    if (d < (1 << (s - 1))) d -= (1 << s) - 1;
                                }
                                k->pred += d;
                                int by = ns == 1 ? my_ : my_ * k->vs + v, bx = ns == 1 ? mx_ : mx_ * k->hs + u;
                                if (bx < k->bw && by < k->bh) k->dc[(size_t)by * (size_t)k->bw + (size_t)bx] = (int16_t)(k->pred * (1 << al));
                            }
                    }
                }
            if (b.eof) goto out;                       /* cut short: not a whole picture */
            for (int i = 0; i < ns; i++) sc[i]->done = 1;
            int all = 1;
            for (int i = 0; i < nf; i++) all &= c[i].done;
            if (all) break;                            /* the rest of the file isn't needed */
            m = b.marker ? b.marker : skip_scan(f);
            continue;
        }
        if (fseek(f, end, SEEK_SET)) goto out;
    }
    for (int i = 0; i < nf; i++) if (!c[i].done || !q0[c[i].tq]) goto out;
    int ow = (X + 7) / 8, oh = (Y + 7) / 8;
    unsigned char *o = malloc((size_t)ow * (size_t)oh * 3);
    if (!o) goto out;
    for (int y = 0; y < oh; y++)
        for (int x = 0; x < ow; x++) {
            int v[3];
            for (int i = 0; i < nf; i++) {
                int bx = x * c[i].hs / hmax, by = y * c[i].vs / vmax;
                int dc = c[i].dc[(size_t)by * (size_t)c[i].bw + (size_t)bx];
                int s = ((dc * q0[c[i].tq] + 4) >> 3) + 128;   /* a block's average: libjpeg's 1x1 IDCT */
                v[i] = s < 0 ? 0 : s > 255 ? 255 : s;
            }
            unsigned char *p = o + 3 * ((size_t)y * (size_t)ow + (size_t)x);
            if (nf == 1) {
                p[0] = p[1] = p[2] = (unsigned char)v[0];
            } else if (adobe_rgb) {
                p[0] = (unsigned char)v[0], p[1] = (unsigned char)v[1], p[2] = (unsigned char)v[2];
            } else {                                   /* YCbCr (JFIF), fixed point */
                int yy = v[0] << 16, cb = v[1] - 128, cr = v[2] - 128;
                int rr = (yy + 91881 * cr + 32768) >> 16;
                int gg = (yy - 22554 * cb - 46802 * cr + 32768) >> 16;
                int bb = (yy + 116130 * cb + 32768) >> 16;
                p[0] = (unsigned char)(rr < 0 ? 0 : rr > 255 ? 255 : rr);
                p[1] = (unsigned char)(gg < 0 ? 0 : gg > 255 ? 255 : gg);
                p[2] = (unsigned char)(bb < 0 ? 0 : bb > 255 ? 255 : bb);
            }
        }
    *rgb = o, *w = ow, *h = oh;
    r = 0;
out:
    for (int i = 0; i < 4; i++) free(c[i].dc);
    return r;
}

/* ---------- JPEG through libjpeg, shrunk while decoding ---------- */

typedef struct { struct jpeg_error_mgr pub; jmp_buf jb; } jerr_t;
static void jpeg_fail(j_common_ptr c) { longjmp(((jerr_t *)c->err)->jb, 1); }
static void jpeg_quiet(j_common_ptr c, int level) { (void)c; (void)level; }

enum { JPEG_OK = 0, JPEG_FAILED = -1, JPEG_TOO_BIG = 1 };

static int jpeg_scaled(FILE *f, long off, int min_side, const art_limits_t *lim, unsigned char **rgb, int *w, int *h)
{
    struct jpeg_decompress_struct c;
    jerr_t err;
    unsigned char *volatile img = NULL;
    volatile int progressive = 0;
    c.err = jpeg_std_error(&err.pub);
    err.pub.error_exit = jpeg_fail;
    err.pub.emit_message = jpeg_quiet;               /* (warnings: "corrupt data" and the like) */
    if (setjmp(err.jb)) {
        jpeg_destroy_decompress(&c);
        free(img);
        return progressive ? JPEG_TOO_BIG : JPEG_FAILED;   /* (out of its memory, most likely) */
    }
    jpeg_create_decompress(&c);
    /* A progressive JPEG keeps every DCT coefficient of the full image
     * whatever the output scale (about 12 MB for a 2000x2000 cover): capped,
     * so libjpeg fails cleanly instead of the kernel killing the player. */
    c.mem->max_memory_to_use = lim->jpeg_memory;
    if (fseek(f, off, SEEK_SET)) { jpeg_destroy_decompress(&c); return JPEG_FAILED; }
    jpeg_stdio_src(&c, f);                           /* streamed: the encoded file is never held in RAM */
    if (jpeg_read_header(&c, TRUE) != JPEG_HEADER_OK) { jpeg_destroy_decompress(&c); return JPEG_FAILED; }
    if (c.progressive_mode) {
        progressive = 1;
        long long coef = 0;                          /* what libjpeg would keep: 64 coefficients a block */
        for (int i = 0; i < c.num_components; i++)
            coef += (long long)c.comp_info[i].width_in_blocks * c.comp_info[i].height_in_blocks * 64 * 2;
        if (coef > lim->jpeg_memory * 9 / 10) { jpeg_destroy_decompress(&c); return JPEG_TOO_BIG; }
    }
    unsigned short_side = c.image_width < c.image_height ? c.image_width : c.image_height;
    unsigned denom = 1;
    while (denom < 8 && short_side / (denom * 2) >= (unsigned)min_side) denom *= 2;
    c.scale_num = 1;
    c.scale_denom = denom;
    c.out_color_space = JCS_RGB;
    c.dct_method = JDCT_IFAST;
    jpeg_start_decompress(&c);
    *w = (int)c.output_width;
    *h = (int)c.output_height;
    if ((long long)*w * *h > lim->max_pixels) { jpeg_destroy_decompress(&c); return JPEG_FAILED; }
    img = malloc((size_t)*w * (size_t)*h * 3);
    if (!img) { jpeg_destroy_decompress(&c); return JPEG_FAILED; }
    while (c.output_scanline < c.output_height) {
        JSAMPROW row = img + (size_t)c.output_scanline * (size_t)*w * 3;
        jpeg_read_scanlines(&c, &row, 1);
    }
    jpeg_finish_decompress(&c);
    jpeg_destroy_decompress(&c);
    *rgb = img;
    return JPEG_OK;
}
#endif

/* ---------- which ---------- */

int art_decode(FILE *f, long off, long len, int min_side, const art_limits_t *lim,
               unsigned char **rgb, int *w, int *h)
{
    unsigned char m[8];
    *rgb = NULL;
    *w = *h = 0;
    if (!f || fseek(f, off, SEEK_SET) || fread(m, 1, 8, f) != 8) return -1;
#ifdef FATHOM_HAVE_LIBJPEG
    if (m[0] == 0xFF && m[1] == 0xD8) {
        int r = jpeg_scaled(f, off, min_side, lim, rgb, w, h);
        if (r == JPEG_TOO_BIG) r = jpeg_dc(f, off, rgb, w, h);
        return r == JPEG_OK ? 0 : -1;
    }
#endif
    if (!memcmp(m, "\x89PNG\r\n\x1a\n", 8)) {
        int r = png_stream(f, off, min_side, lim, rgb, w, h);
        if (r <= 0) return r;
    }
    return whole(f, off, len, lim, rgb, w, h);
}

int art_decode_mem(const unsigned char *enc, size_t len, int min_side, const art_limits_t *lim,
                   unsigned char **rgb, int *w, int *h)
{
    *rgb = NULL;
    if (!enc || len < 8) return -1;
    FILE *f = fmemopen((void *)enc, len, "rb");
    if (!f) return -1;
    int r = art_decode(f, 0, (long)len, min_side, lim, rgb, w, h);
    fclose(f);
    return r;
}
