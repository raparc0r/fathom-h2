/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/*
 * What a server sends for a station's icon, damaged every way. The readers of
 * it (app/net/radioicon.c: an .ico file, a web page's <link> tags, an address
 * joined to its page's; app/net/radiodir.c: the directory's answer about a
 * stream, and its lists of stations, which name a logo and a site each;
 * app/player/radio.c: the stations' list, where those are kept) are each
 * given good input changed at random, in a block of memory exactly its size,
 * so that a byte read or written past it stops the run under
 * AddressSanitizer:
 *
 *   cmake -B build-asan -DCMAKE_BUILD_TYPE=Debug -DFATHOM_BUILD_SIM=OFF \
 *     -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -g -O1" \
 *     -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
 *   build-asan/test_fuzz_radioicon 300000
 *
 * The same cases every time: a case is its reader, its number and the seed.
 *
 *   test_fuzz_radioicon [CASES a reader [SEED [READER [FIRST CASE]]]]
 *
 * READER: ico, site, join, stream, stations, list; or cut (good inputs
 * cut at every length, an .ico's header with every awkward number in every
 * place) or long (inputs made to take the longest). A failure names its case
 * ("ico 1234"): `test_fuzz_radioicon 1 SEED ico 1234` runs it alone, and with
 * FUZZ_DUMP=FILE in the environment leaves its input there.
 */
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include <zlib.h>

#include "core/clock.h"
#include "net/radiodir.h"
#include "net/radioicon.h"
#include "player/radio.h"

/* ---------- which case, and what became of it ---------- */

typedef struct {
    const char *name;
    void (*one)(void);
    long n, read;                /* cases, and those the reader took for good input */
    double slow;                 /* the longest a case took (ms) */
    long slow_at;
} reader_t;

static reader_t *g_rd;
static const char *g_stage = "";
static long g_case = -1;
static uint64_t g_seed = 20261124;
static const char *g_dump;
static int fails;

#define CHECK(c) do { if (!(c)) { \
        printf("FAIL %s:%d: %s (case \"%s %ld\", seed %llu)\n", __FILE__, __LINE__, #c, g_stage, g_case, (unsigned long long)g_seed); \
        fails++; } } while (0)

static void say_case(const char *what)
{
    char m[200];
    int k = snprintf(m, sizeof m, "\nfuzz_radioicon: %s in case \"%s %ld\" (seed %llu)\n", what, g_stage, g_case, (unsigned long long)g_seed);
    if (k > 0 && write(2, m, (size_t)k) < 0) {}
}

static void died(int sig)
{
    say_case(sig == SIGALRM ? "stuck" : "stopped");
    _exit(3);
}

/* (AddressSanitizer calls this, if it's there, before its report) */
void __asan_on_error(void);
void __asan_on_error(void) { say_case("a bad read or write"); }

static double now_ms(void) { return clock_fsec() * 1e3; }

/* ---------- the same numbers every time ---------- */

static uint64_t g_r = 1;

static uint64_t rnd(void)
{
    g_r ^= g_r << 13, g_r ^= g_r >> 7, g_r ^= g_r << 17;
    return g_r * 0x2545F4914F6CDD1DULL;
}

static size_t rn(size_t n) { return n ? (size_t)((rnd() >> 11) % n) : 0; }
static int one_in(size_t n) { return rn(n) == 0; }
#define PICK(a) ((a)[rn(sizeof (a) / sizeof *(a))])

/* A case's numbers: from the seed, its reader and its own, so one can be run alone */
static void seed_case(const char *stage, long i)
{
    uint64_t z = g_seed;
    for (const char *c = stage; *c; c++) z = z * 1099511628211ULL + (unsigned char)*c;
    z += 0x9E3779B97F4A7C15ULL * ((uint64_t)i + 1);
    z = (z ^ z >> 30) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ z >> 27) * 0x94D049BB133111EBULL;
    g_r = (z ^ z >> 31) | 1;
}

/* ---------- the input being made ---------- */

#define WMAX (400 * 1024)
static unsigned char W[WMAX + 4096];   /* (spare room: the last small thing written when it's full) */
static size_t wn;

static void w_put(const void *p, size_t n)
{
    if (n > WMAX - wn) n = WMAX - wn;
    memcpy(W + wn, p, n);
    wn += n;
}

static void w_str(const char *s) { w_put(s, strlen(s)); }

static void w_fmt(const char *fmt, ...)
{
    char b[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    w_str(b);
}

/* unit over and over, `total` bytes of it */
static void w_rep(const char *unit, size_t total)
{
    size_t u = strlen(unit);
    for (size_t i = 0; i < total; i++) w_put(unit + i % u, 1);
}

static void w_insert(size_t at, const void *p, size_t n)
{
    if (n > WMAX - wn || at > wn) return;
    memmove(W + at + n, W + at, wn - at);
    memcpy(W + at, p, n);
    wn += n;
}

static void w_delete(size_t at, size_t n)
{
    if (at >= wn) return;
    if (n > wn - at) n = wn - at;
    memmove(W + at, W + at + n, wn - at - n);
    wn -= n;
}

static void noise(unsigned char *p, size_t n)
{
    for (size_t i = 0; i < n; i += 8) {
        uint64_t v = rnd();
        memcpy(p + i, &v, n - i < 8 ? n - i : 8);
    }
}

/* n bytes in a block exactly that long: what's past it isn't the reader's */
static void *exact(const void *p, size_t n)
{
    unsigned char *b = malloc(n);
    if (!b) b = malloc(1);       /* (a C library whose malloc(0) gives nothing) */
    if (!b) abort();
    if (n) memcpy(b, p, n);
    return b;
}

static void dump(const char *about)
{
    if (!g_dump) return;
    FILE *f = fopen(g_dump, "wb");
    if (f) { fwrite(W, 1, wn, f); fclose(f); }
    printf("case \"%s %ld\": %zu bytes in %s%s%s\n", g_stage, g_case, wn, g_dump, about[0] ? "; " : "", about);
}

/* ---------- damage of any kind ---------- */

static const unsigned char EDGE[] = {0, 1, 0x7f, 0x80, 0xff};

/* Bits turned, bytes set to the values at the edges of things, a piece copied elsewhere */
static void mut_bytes(void)
{
    if (!wn) return;
    switch (rn(5)) {
    case 0:
        for (size_t k = 1 + rn(8); k; k--) W[rn(wn)] ^= (unsigned char)(1u << rn(8));
        break;
    case 1:
        for (size_t k = 1 + rn(4); k; k--) W[rn(wn)] = PICK(EDGE);
        break;
    case 2:
        for (size_t k = 1 + rn(4); k; k--) W[rn(wn)] = (unsigned char)rnd();
        break;
    case 3: {
        size_t at = rn(wn), n = 1 + rn(16);
        memset(W + at, PICK(EDGE), n < wn - at ? n : wn - at);
        break;
    }
    default: {
        size_t n = 1 + rn(wn < 64 ? wn : 64), from = rn(wn - n + 1), to = rn(wn - n + 1);
        memmove(W + to, W + from, n);
        break;
    }
    }
}

/* Cut short (anywhere, or just before its end), or with more after it */
static void mut_len(void)
{
    switch (rn(4)) {
    case 0: {
        unsigned char more[64];
        size_t n = 1 + rn(sizeof more);
        if (one_in(2)) noise(more, n);
        else memset(more, PICK(EDGE), n);
        w_put(more, n);
        break;
    }
    case 1: wn -= rn(wn < 64 ? wn + 1 : 64); break;
    default: wn = rn(wn + 1); break;
    }
}

/* Text: a character that means something put in, or in another's place; a
 * word of the reader's own; a piece taken out or said twice */
static const char MARKS[] = "\"\"''<<>>==&;# \t\n\r|:/?\\{}[],.-0x\0";

static void mut_text(const char *const *words, size_t nwords)
{
    size_t at = rn(wn + 1);
    switch (rn(7)) {
    case 0:
        if (wn) W[rn(wn)] = (unsigned char)MARKS[rn(sizeof MARKS - 1)];
        break;
    case 1: w_insert(at, &MARKS[rn(sizeof MARKS - 1)], 1); break;
    case 2:
    case 3: {
        const char *w = words[rn(nwords)];
        if (one_in(3) && wn) {                       /* (over what's there) */
            size_t n = strlen(w);
            at = rn(wn);
            memcpy(W + at, w, n < wn - at ? n : wn - at);
        } else {
            w_insert(at, w, strlen(w));
        }
        break;
    }
    case 4: w_delete(at, 1 + rn(one_in(4) ? 200 : 4)); break;
    case 5: {
        unsigned char piece[256];
        size_t n = 1 + rn(wn < sizeof piece ? wn : sizeof piece), from = rn(wn - n + 1);
        if (!wn) break;
        memcpy(piece, W + from, n);
        w_insert(at, piece, n);
        break;
    }
    default: mut_bytes(); break;
    }
}

/* ---------- an .ico file ---------- */

static unsigned get16(const unsigned char *p) { return (unsigned)p[0] | (unsigned)p[1] << 8; }
static unsigned long get32(const unsigned char *p) { return (unsigned long)get16(p) | (unsigned long)get16(p + 2) << 16; }
static void le16(unsigned char *p, unsigned long v) { p[0] = (unsigned char)v, p[1] = (unsigned char)(v >> 8); }
static void le32(unsigned char *p, unsigned long v) { le16(p, v & 0xffff), le16(p + 2, v >> 16 & 0xffff); }
static void be32(unsigned char *p, unsigned long v) { p[0] = (unsigned char)(v >> 24), p[1] = (unsigned char)(v >> 16), p[2] = (unsigned char)(v >> 8), p[3] = (unsigned char)v; }
static unsigned long getbe32(const unsigned char *p) { return (unsigned long)p[0] << 24 | (unsigned long)p[1] << 16 | (unsigned long)p[2] << 8 | p[3]; }

/* The numbers a size, an offset or a count goes wrong at: none, one, a
 * header's 40 bytes, the sides a favicon has, where a byte, two and four run
 * out or turn negative; and those next to the one there, and the file's end */
static const unsigned long EDGE32[] = {
    0, 1, 2, 3, 7, 8, 15, 16, 17, 23, 24, 25, 31, 32, 33, 39, 40, 41, 47, 48, 64, 65, 108, 124, 127, 128, 129, 255, 256, 257,
    511, 512, 513, 1023, 1024, 1025, 4096, 0x7fff, 0x8000, 0xffff, 0x10000, 0x10001, 0xffffff, 0x1000000, 0x7fffffff,
    0x80000000, 0x80000001, 0xfffffff0, 0xfffffff4, 0xfffffffc, 0xfffffffe, 0xffffffff,
};
#define N_EDGE32 (sizeof EDGE32 / sizeof *EDGE32)

static unsigned long edge32(unsigned long cur)
{
    switch (rn(10)) {
    case 0: return cur + 1;
    case 1: return cur - 1;
    case 2: return cur * 2;
    case 3: return cur / 2;
    case 4: return (unsigned long)wn + rn(3) - 1;
    case 5: return (unsigned long)rnd();
    default: return PICK(EDGE32);
    }
}

/* A picture in the .ico being made: its directory entry, its bytes, and (a
 * PNG) where its chunks start */
typedef struct {
    size_t ent, at, len;
    int png, w, h, nchunk;
    size_t chunk[10];
} pic_t;
static pic_t g_pic[8];
static int g_npic;

static size_t png_chunk(pic_t *pc, unsigned char *out, const char *type, const unsigned char *d, size_t n)
{
    if (pc->nchunk < 10) pc->chunk[pc->nchunk++] = (size_t)(out - W);
    be32(out, (unsigned long)n);
    memcpy(out + 4, type, 4);
    if (n) memcpy(out + 8, d, n);
    be32(out + 8 + n, crc32(crc32(0, (const Bytef *)type, 4), d, (uInt)n));
    return 12 + n;
}

/* A PNG, w x h, of colour type ct at `depth` bits a sample: noise for a small
 * one's pixels (a big one's are all alike, to pack small). Its length, or 0 if
 * there's no room for it. */
static size_t png_make(pic_t *pc, unsigned char *out, size_t room, int w, int h, int ct, int depth)
{
    static const int chans[7] = {1, 0, 3, 1, 2, 0, 4};
    size_t row = ((size_t)w * (size_t)chans[ct] * (size_t)depth + 7) / 8 + 1, rawn = row * (size_t)h;
    unsigned char *raw = calloc(rawn, 1), hd[13], pal[768];
    uLongf zn = compressBound((uLong)rawn);
    unsigned char *z = malloc(zn);
    size_t n = 8;
    if (!raw || !z) abort();
    if (rawn <= 16384) noise(raw, rawn);
    else memset(raw, (int)rn(256), rawn);
    for (int y = 0; y < h; y++) raw[(size_t)y * row] = (unsigned char)rn(5);    /* each row's filter */
    compress2(z, &zn, raw, (uLong)rawn, 1);
    pc->nchunk = 0;
    if (room >= 8 + 25 + 780 + 268 + 32 + (size_t)zn + 36 + 12) {
        memcpy(out, "\x89PNG\r\n\x1a\n", 8);
        be32(hd, (unsigned long)w), be32(hd + 4, (unsigned long)h);
        hd[8] = (unsigned char)depth, hd[9] = (unsigned char)ct, hd[10] = hd[11] = hd[12] = 0;
        n += png_chunk(pc, out + n, "IHDR", hd, 13);
        if (ct == 3 || one_in(8)) {
            size_t np = ct == 3 && !one_in(4) ? (size_t)3 << depth : 3 * rn(257);
            noise(pal, sizeof pal);
            n += png_chunk(pc, out + n, "PLTE", pal, np);
            if (one_in(2)) n += png_chunk(pc, out + n, "tRNS", pal, rn(257));
        }
        if (one_in(8)) n += png_chunk(pc, out + n, "tEXt", (const unsigned char *)"Comment\0made for a test", 23);
        size_t parts = one_in(4) ? 2 + rn(2) : 1, done = 0;
        for (size_t i = 0; i < parts; i++) {             /* (the pixels in one piece, or in several) */
            size_t k = i + 1 == parts ? (size_t)zn - done : rn((size_t)zn - done + 1);
            n += png_chunk(pc, out + n, "IDAT", z + done, k);
            done += k;
        }
        n += png_chunk(pc, out + n, "IEND", NULL, 0);
    } else {
        n = 0;
    }
    free(raw);
    free(z);
    return n;
}

/* A Windows bitmap as an .ico holds one: a header of hs bytes (40, or a later
 * kind's longer one), w x h pixels of noise, then its mask (mask: 0 none, 1
 * whole, 2 part of one). alpha, at 32 bits: 0 none anywhere (the mask says
 * what shows), 1 noise, 2 all of it. */
static size_t bmp_make(unsigned char *d, int w, int h, int bpp, unsigned hs, int mask, int alpha)
{
    size_t stride = ((size_t)w * (size_t)bpp + 31) / 32 * 4, mstride = ((size_t)w + 31) / 32 * 4;
    size_t n = hs + stride * (size_t)h, mn = mask == 1 ? mstride * (size_t)h : mask == 2 ? rn(mstride * (size_t)h) : 0;
    memset(d, 0, hs);
    le32(d, hs), le32(d + 4, (unsigned long)w), le32(d + 8, (unsigned long)h * 2), le16(d + 12, 1), le16(d + 14, (unsigned)bpp);
    le32(d + 20, (unsigned long)(stride * (size_t)h));
    noise(d + hs, n - hs + mn);
    if (bpp == 32 && alpha != 1)
        for (size_t i = 0; i < (size_t)w * (size_t)h; i++) d[hs + i * 4 + 3] = alpha ? 255 : 0;
    return n + mn;
}

/* One picture added to the .ico in W (its entry is there already, blank) */
static void ico_add(int w, int h, int png, int bpp, unsigned hs, int mask, int alpha)
{
    pic_t *pc = &g_pic[g_npic];
    unsigned char *e = W + 6 + 16 * g_npic;
    memset(pc, 0, sizeof *pc);
    pc->ent = (size_t)(e - W), pc->at = wn, pc->png = png, pc->w = w, pc->h = h;
    if (png) {
        static const int KIND[][2] = {{2, 8}, {2, 8}, {6, 8}, {6, 8}, {0, 8}, {4, 8}, {3, 8}, {3, 4}, {3, 2}, {3, 1}, {0, 1}, {0, 4}, {2, 16}, {6, 16}, {0, 16}, {4, 16}};
        size_t k = rn(sizeof KIND / sizeof *KIND);
        pc->len = png_make(pc, W + wn, WMAX - wn, w, h, KIND[k][0], KIND[k][1]);
        bpp = 32;
    }
    if (!pc->len) {                                  /* (a bitmap; or a PNG there was no room for) */
        size_t stride = ((size_t)w * (size_t)bpp + 31) / 32 * 4, mstride = ((size_t)w + 31) / 32 * 4;
        size_t room = wn < WMAX ? WMAX - wn : 0;
        if (w > 256 || h > 256 || hs + (stride + mstride) * (size_t)h > room) w = h = pc->w = pc->h = 16;
        /* (W nearly full: not even that one fits. A picture of one pixel then,
         * which W's spare room past WMAX holds: without this the noise was
         * written past W's end, seed 777's case "ico 38731") */
        if (hs + (((size_t)w * (size_t)bpp + 31) / 32 * 4 + ((size_t)w + 31) / 32 * 4) * (size_t)h > room) w = h = pc->w = pc->h = 1;
        pc->png = 0;
        pc->len = bmp_make(W + wn, w, h, bpp, hs, mask, alpha);
    }
    e[0] = (unsigned char)w, e[1] = (unsigned char)h;         /* (256 is written 0) */
    le16(e + 4, 1), le16(e + 6, (unsigned)bpp);
    le32(e + 8, (unsigned long)pc->len), le32(e + 12, (unsigned long)pc->at);
    wn += pc->len;
    g_npic++;
}

static void ico_begin(int count)
{
    wn = 6 + 16 * (size_t)count;
    memset(W, 0, wn);
    le16(W + 2, 1), le16(W + 4, (unsigned)count);
    g_npic = 0;
}

static int ico_side(void)
{
    static const int D[] = {1, 2, 3, 7, 8, 9, 15, 16, 16, 16, 17, 24, 31, 32, 32, 33, 48, 64};
    return one_in(40) ? 255 + (int)rn(2) : one_in(10) ? 1 + (int)rn(96) : PICK(D);
}

/* A good .ico: a picture, or a few of several sizes */
static void gen_ico(void)
{
    static const unsigned HS[] = {40, 40, 40, 40, 40, 40, 52, 56, 108, 124};
    static const int BIG[][2] = {{4000, 1}, {1, 4000}, {65535, 1}, {1, 65535}, {700, 500}, {204, 204}, {408, 408}, {409, 407}, {1000, 2}, {632, 632}};
    int count = one_in(6) ? 2 + (int)rn(5) : 1;
    ico_begin(count);
    for (int i = 0; i < count; i++) {
        int w = ico_side(), h = one_in(6) ? ico_side() : w, png = one_in(5);
        if (png && one_in(25)) {                     /* (a PNG has no .ico's limit on its sides) */
            size_t b = rn(sizeof BIG / sizeof *BIG);
            w = BIG[b][0], h = BIG[b][1];
        }
        ico_add(w, h, png, one_in(3) ? 24 : 32, PICK(HS), (int)rn(3), (int)rn(3));
        if (one_in(6)) {                             /* (bytes that are nobody's between two) */
            unsigned char gap[8];
            noise(gap, sizeof gap);
            w_put(gap, 1 + rn(8));
        }
    }
}

static void mut_png(const pic_t *pc)
{
    static const char *const TYPES[] = {"IHDR", "PLTE", "tRNS", "IDAT", "IEND", "tEXt", "idat", "\0\0\0\0"};
    size_t c = pc->chunk[rn((size_t)pc->nchunk)];
    if (c + 12 > wn) return;
    switch (rn(5)) {
    case 0: be32(W + c, edge32(getbe32(W + c))); break;                      /* a chunk's length */
    case 1: memcpy(W + c + 4, PICK(TYPES), 4); break;                        /* ...its kind */
    case 2:                                                                 /* the picture's width or height */
        if (pc->at + 24 <= wn) {
            unsigned char *f = W + pc->at + 16 + 4 * rn(2);
            be32(f, one_in(3) ? rn(70000) : edge32(getbe32(f)));
        }
        break;
    case 3:                                                                 /* its depth, kind, packing, filter, interlacing */
        if (pc->at + 29 <= wn) {
            static const unsigned char B[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 16, 32, 255};
            W[pc->at + 24 + rn(5)] = PICK(B);
        }
        break;
    default: {                                                              /* a byte near the start of what a chunk holds */
        size_t at = c + 8 + rn(24);
        if (at < wn) W[at] = one_in(2) ? PICK(EDGE) : (unsigned char)rnd();
        break;
    }
    }
}

/* One number of the .ico changed: in its header, a picture's entry, or a bitmap's header */
static void mut_ico(void)
{
    static const unsigned BPP[] = {0, 1, 4, 8, 15, 16, 23, 24, 25, 31, 32, 33, 48, 64, 0xffff};
    if (!g_npic) { mut_bytes(); return; }
    const pic_t *pc = &g_pic[rn((size_t)g_npic)];
    unsigned char *e = W + pc->ent, *d = W + pc->at;
    if (pc->at + 40 > wn) { mut_bytes(); return; }
    switch (rn(18)) {
    case 0: {
        unsigned char *f = W + 2 * rn(3);            /* reserved, the kind, how many pictures */
        le16(f, one_in(2) ? edge32(get16(f)) : rn(70));
        break;
    }
    case 1: e[rn(4)] = one_in(2) ? PICK(EDGE) : (unsigned char)rnd(); break;       /* its sides as the entry gives them, colours */
    case 2: le16(e + 4 + 2 * rn(2), one_in(2) ? PICK(BPP) : edge32(get16(e + 6))); break;
    case 3: le32(e + 8, edge32(get32(e + 8))); break;                               /* how many bytes it is */
    case 4: le32(e + 12, edge32(get32(e + 12))); break;                             /* where it starts */
    case 5: le32(e + 8, (unsigned long)(wn - pc->at) + rn(3) - 1); break;           /* it ends where the file does, or a byte either side */
    case 6: {                                        /* it starts a header's length, or less, before the end */
        static const unsigned BACK[] = {0, 1, 4, 8, 12, 16, 20, 39, 40, 41, 44};
        unsigned back = PICK(BACK);
        le32(e + 12, (unsigned long)wn - back);
        if (one_in(2)) le32(e + 8, one_in(2) ? back : 40);
        break;
    }
    case 7: le32(e + 12, rn(wn + 2)); break;                                        /* it starts anywhere: in the directory, in another */
    case 8: le32(d, edge32(get32(d))); break;                                       /* the header's own length */
    case 9: le32(d + 4, one_in(3) ? rn(300) : edge32(get32(d + 4))); break;         /* the bitmap's width */
    case 10: le32(d + 8, one_in(3) ? rn(600) : edge32(get32(d + 8))); break;        /* ...its height (twice) */
    case 11: le16(d + 12 + 2 * rn(2), one_in(2) ? PICK(BPP) : edge32(get16(d + 14))); break;   /* planes, bits a pixel */
    case 12: le32(d + 16 + 4 * rn(6), edge32(0)); break;                            /* how it's packed, and the header's other numbers */
    case 13: {                                       /* other sides, the bytes as they were: too few, or too many */
        static const unsigned S[] = {1, 2, 15, 16, 17, 31, 32, 33, 64, 127, 128, 129, 255, 256};
        le32(d + 4, PICK(S));
        le32(d + 8, 2 * PICK(S));
        break;
    }
    case 14:
    case 15:
    case 16:
        if (pc->png && pc->nchunk) mut_png(pc);
        else mut_bytes();
        break;
    default: mut_bytes(); break;
    }
}

static void run_ico(int good)
{
    dump("");
    unsigned char *b = exact(W, wn);
    int side = -7, r = ricon_test_ico(b, wn, &side);
    CHECK(r == 0 || r == -1);
    CHECK(r != 0 || (side >= 1 && side <= 1024));
    CHECK(!good || r == 0);                          /* (one left as it was made) */
    g_rd->read += r == 0;
    free(b);
}

static void case_ico(void)
{
    int good = 0;
    if (one_in(40)) {                                /* nothing but an .ico's first bytes */
        wn = rn(300);
        noise(W, wn);
        if (wn >= 6) memcpy(W, "\0\0\1\0", 4), W[4] = (unsigned char)(1 + rn(3)), W[5] = 0;
    } else {
        gen_ico();
        good = one_in(12);
        if (!good) {
            for (size_t k = one_in(3) ? 2 + rn(3) : 1; k; k--) {
                if (one_in(4)) mut_bytes();
                else mut_ico();
            }
            if (one_in(4)) mut_len();
        }
    }
    run_ico(good);
}

/* ---------- a station's web page, and the addresses in it ---------- */

static char g_base[700];
static char g_val[8192];

/* Pages' own addresses, as a station's line or the directory may give them... */
static const char *const BASES[] = {
    "https://radio.example/", "http://radio.example", "https://radio.example/a/b/page.html?x=1#top", "https://radio.example/a/b/",
    "https://radio.example/a/b", "https://user:key@radio.example:8443/x", "http://h?/a/b", "http://h#/a/", "http://h/?a=b/c",
    "https://radio.example/a/b/?q=1/2", "https://", "http:///", "http://[::1]/", "https://radio.example//", "http://r/#",
    "https://radio.example/caf\xc3\xa9/", "http://://", "https://radio.example/a&amp;b/",
};
/* ...and what isn't one (radio_icon_ok keeps these from the readers, as things are) */
static const char *const ODD_BASES[] = {
    "", "radio.example", "ftp://radio.example/", "://", "HTTPS://RADIO.EXAMPLE/Live", "http:/radio.example", "//radio.example/",
    "https://radio.example/a b/", "x", "/", "http", "://x", "a://", "https://radio.example/%", "#", "?",
};
static const char URLCH[] = "/:?#&;=.@ %-_ax\\|\"'<>\t\x7f\x80\xff";

static void pick_base(void)
{
    size_t k;
    switch (rn(12)) {
    case 0: snprintf(g_base, sizeof g_base, "%s", PICK(ODD_BASES)); break;
    case 1:                                          /* as long as a site's address may be (255), and the lengths around that */
        snprintf(g_base, sizeof g_base, "%s", PICK(BASES));
        for (size_t to = 250 + rn(10), at = strlen(g_base); at < to;) g_base[at++] = one_in(9) ? '/' : 'a', g_base[at] = '\0';
        break;
    case 2:
        k = rn(80);
        for (size_t i = 0; i < k; i++) g_base[i] = URLCH[rn(sizeof URLCH - 1)];
        g_base[k] = '\0';
        break;
    default: snprintf(g_base, sizeof g_base, "%s", PICK(BASES)); break;
    }
    if (one_in(8) && g_base[0]) g_base[rn(strlen(g_base))] = URLCH[rn(sizeof URLCH - 1)];
}

static const char *const HREFS[] = {
    "/favicon.ico", "favicon.ico", "./img/icon.png", "../icon.png", "//cdn.example/i.png", "https://cdn.example/i.png",
    "HTTP://CDN.EXAMPLE/I.PNG", "http://", "https://", "//", "/", "", " ", "  /spaced.png  ", "/a b.png", "\t/tab.png\n",
    "data:image/png;base64,AAAA", "javascript:alert(1)", "mailto:x@example", "/i.png?v=1&amp;x=2", "/i.png?a=1&amp;", "/i.png&amp",
    "/i.png&am", "/i.png&", "&amp;", "&amp;&amp;&amp;", "&amp;amp;", "&amp;amp", "/i.png#frag", "#", "#&amp;", "&amp;#", "/logo.svg",
    "/logo.SVG?x=1", "/logo.svg#a", ".svg", "svg", "/i.png?x=.svg", "?x=1", "icons/px96.png", "./", ".", "..", "../", "./../x.png",
    "/\001ctl.png", "/caf\xc3\xa9.png", "/\xff\xfe.png", "http://[::1]/i.png", "///triple", "http:/one.png", "x:y", ":", "https:",
    "https:/", "https:x", "//cdn.example", "/#", "/?", "/ ", " http://cdn.example/sp.png ", "https://cdn.example/a.png#&amp;",
    "https://cdn.example/a.png &amp;", "http://cdn.example/|bar.png",
};
static const char *const RELS[] = {
    "icon", "icon", "icon", "shortcut icon", "apple-touch-icon", "apple-touch-icon", "apple-touch-icon-precomposed",
    "APPLE-TOUCH-ICON", "ICON", "Shortcut Icon", "mask-icon", "stylesheet", "alternate icon", "", "fluid-icon", "icon mask-icon",
    "ico", "iconx", " icon ", "preload", "apple-touch-icon mask-icon",
};
static const char *const SIZES[] = {
    "16x16", "32x32", "96x96", "180x180", "192x192", "600x600", "601x601", "any", "", "0", "-1", "99999999999999999999",
    "2147483647", "2147483648", "4294967297x1", " 48x48", "x", "16x16 32x32", "1e9", "+64", "0x40", "599",
};

/* A value of an awkward length: around what an attribute's room is (a rel's
 * 64, an address's 400, sizes' 32), an address's 420 and a tag's 2,000 */
static const char *long_val(void)
{
    static const size_t LEN[] = {30, 31, 32, 33, 62, 63, 64, 65, 398, 399, 400, 401, 418, 419, 420, 421, 500, 1990, 2000, 2001, 2100, 5000};
    static const char *const UNIT[] = {"a", "/x", "&amp;", "../", "%20", " ", "9", "\xc3\xa9", "&", "#", "?", "icon ", "//", "&amp", "a&amp;"};
    static const char *const HEAD[] = {"", "", "/", "https://h.example/", "//h/", "./", "http://", "icon"};
    const char *u = PICK(UNIT), *hd = PICK(HEAD);
    size_t n = PICK(LEN), k = strlen(hd), ul = strlen(u);
    if (k > n) k = n;
    memcpy(g_val, hd, k);
    for (size_t i = 0; k < n; i++) g_val[k++] = u[i % ul];
    if (one_in(3) && n >= 5) memcpy(g_val + n - 5, "&amp;", 5);      /* (one at the very end) */
    g_val[n] = '\0';
    return g_val;
}

static void put_attr(const char *name, const char *val)
{
    static const char *const EQ[] = {"=", "=", "=", "=", " = ", " =", "= ", "\n=\t", ""};
    w_str(one_in(8) ? "\n" : one_in(12) ? "" : " ");
    for (const char *c = name; *c; c++) {
        char ch = one_in(6) ? (char)(*c ^ 0x20) : *c;                /* (in either case) */
        w_put(&ch, 1);
    }
    w_str(PICK(EQ));
    static const char *const QUOTES[][2] = {
        {"\"", "\""}, {"\"", "\""}, {"\"", "\""}, {"\"", "\""}, {"\"", "\""}, {"'", "'"}, {"'", "'"}, {"", ""}, {"", ""},
        {"\"", ""}, {"'", ""}, {"\"", "'"},          /* (one never closed, or closed with the other) */
    };
    size_t q = rn(sizeof QUOTES / sizeof *QUOTES);
    w_str(QUOTES[q][0]), w_str(val), w_str(QUOTES[q][1]);
}

static const char *const PAGE_WORDS[] = {
    "<link", "<LINK", " rel=", " href=", " sizes=", "icon", "apple-touch-icon", "mask-icon", ">", "\"", "'", "&amp;", "&amp", "http://",
    "https://", "//", "../", ".svg", "#", "?", "=", " ", "<link rel=icon href=", "<link rel=\"icon\" href=\"/a.png\">", "\0", "x",
};

/* A page: a few <link> tags of every make, with other things between */
static void gen_page(void)
{
    static const char *const HEADS[] = {"", "<!doctype html><html><head><title>T</title>\n", "<head>", "\xef\xbb\xbf<html>", "<HTML><HEAD>\r\n"};
    static const char *const TAGS[] = {"<link", "<link", "<link", "<link", "<LINK", "<Link", "<link\n", "<link\t", "<link/", "<linkrel", "< link", "<a", "<meta", "<lin", "<", "<link<link"};
    static const char *const ENDS[] = {">", ">", ">", ">", " />", "/>", "", ">\n", " >", ">>"};
    static const char *const BETWEEN[] = {
        "", "", "\n", "<title>T</title>", "<meta charset=utf-8>", "<!-- <link rel=icon href=/c.png> -->", "<script>if(a>b){}</script>",
        "  \t\r\n", "<style>a>b{}</style>", "text", "<link>", "<link >", "<link rel>", "<link rel=>", "<link href>", ">", "\"", "'",
    };
    static const char *const JUNK[] = {" crossorigin", " type=\"image/png\"", " color=#000", " data-x=\">\"", " =", " ==", " rel", " href", " \"", " '", " a=b=c", " /"};
    size_t tags = one_in(30) ? 50 + rn(400) : rn(7), last = 0;
    wn = 0;
    w_str(PICK(HEADS));
    for (size_t i = 0; i < tags; i++) {
        last = wn;
        w_str(PICK(TAGS));
        for (size_t a = one_in(4) ? rn(6) : 2 + rn(2), k = rn(3); a; a--, k++) {
            if (one_in(8)) w_str(PICK(JUNK));
            else if (k % 3 == 0) put_attr("rel", one_in(25) ? long_val() : PICK(RELS));
            else if (k % 3 == 1) put_attr("href", one_in(8) ? long_val() : PICK(HREFS));
            else put_attr("sizes", one_in(25) ? long_val() : PICK(SIZES));
        }
        w_str(PICK(ENDS));
        if (one_in(40)) w_rep(one_in(2) ? " " : "x>", 1 + rn(3000));
        else w_str(PICK(BETWEEN));
    }
    if (one_in(2)) w_str("</head><body>radio</body></html>");
    else if (tags && one_in(2)) wn = last + rn(wn - last + 1);       /* the page ends inside its last tag */
}

static int is_address(const char *u, size_t n)
{
    if (n < 7 || (strncasecmp(u, "http://", 7) && strncasecmp(u, "https://", 8))) return 0;
    for (size_t i = 0; i < n; i++)
        if ((unsigned char)u[i] <= ' ' || u[i] == '#') return 0;
    return 1;
}

static void run_site(void)
{
    dump(g_base);
    char *page = exact(W, wn), *base = exact(g_base, strlen(g_base) + 1), urls[3 * 421 + 1];
    int k = ricon_test_site(page, wn, base, urls, sizeof urls), lines = 0;
    CHECK(k >= 0 && k <= 3);
    for (const char *u = urls; *u;) {                /* each one an address that can be fetched */
        const char *nl = strchr(u, '\n');
        CHECK(nl && nl - u < 420 && is_address(u, (size_t)(nl - u)));
        lines++;
        if (!nl) break;
        u = nl + 1;
    }
    CHECK(lines == k);
    g_rd->read += k > 0;
    free(page);
    free(base);
}

static void case_site(void)
{
    pick_base();
    if (one_in(40)) {
        wn = rn(400);
        noise(W, wn);
    } else {
        gen_page();
        if (!one_in(4))
            for (size_t k = one_in(3) ? 2 + rn(4) : 1; k; k--) mut_text(PAGE_WORDS, sizeof PAGE_WORDS / sizeof *PAGE_WORDS);
        if (one_in(5)) mut_len();
    }
    run_site();
}

static void run_join(const char *href, size_t cap)
{
    wn = 0;
    w_str(href);
    dump(g_base);
    char *base = exact(g_base, strlen(g_base) + 1), *h = exact(href, strlen(href) + 1), *out = malloc(cap);
    if (!out) abort();
    memset(out, 0xAA, cap);
    int r = ricon_test_join(base, h, out, cap);
    CHECK(r == 0 || r == -1);
    if (r == 0) {
        /* an address, and all of it: what it is with any amount of room, never one cut to fit */
        const char *end = memchr(out, '\0', cap);
        size_t room = strlen(g_base) + strlen(href) + 8;
        char *all = malloc(room);
        if (!all) abort();
        CHECK(end && is_address(out, (size_t)(end - out)));
        CHECK(ricon_test_join(base, h, all, room) == 0 && end && !strcmp(all, out));
        free(all);
    }
    g_rd->read += r == 0;
    free(base);
    free(h);
    free(out);
}

static void case_join(void)
{
    static const size_t CAPS[] = {1, 2, 3, 7, 8, 9, 20, 21, 22, 23, 24, 25, 26, 30, 64, 255, 256, 257, 400, 419, 420, 420, 420, 420, 421, 1024};
    const char *href;
    pick_base();
    switch (rn(8)) {
    case 0: href = long_val(); break;
    case 1: {
        size_t k = rn(64);
        for (size_t i = 0; i < k; i++) g_val[i] = URLCH[rn(sizeof URLCH - 1)];
        g_val[k] = '\0';
        href = g_val;
        break;
    }
    default:
        snprintf(g_val, sizeof g_val, "%s", PICK(HREFS));
        if (one_in(5) && g_val[0]) g_val[rn(strlen(g_val))] = URLCH[rn(sizeof URLCH - 1)];
        href = g_val;
        break;
    }
    run_join(href, one_in(3) ? 1 + rn(600) : PICK(CAPS));
}

/* ---------- the directory's answer about a stream ---------- */

/* What an address's place may hold, as it's written in JSON */
static const char *const J_URLS[] = {
    "https://radio.example/", "https://radio.example/", "http://radio.example/live?x=1", "https://img.example/logo.png",
    "https:\\/\\/radio.example\\/", "", "null", "radio.example", "ftp://radio.example/", "https://radio.example/a b",
    "https://radio.example/a|b", "http://", "https://radio.example/\\u00e9", "https://radio.example/\\ud83d\\ude00",
    "https://radio.example/\\ud83d", "https://radio.example/\\ud83d\\u0041", "https://radio.example/\\udc00",
    "https://radio.example/\\u12", "https://radio.example/\\u", "https://radio.example/\\", "\\u0068ttps://radio.example/",
    "https://radio.example/\\n", "https://radio.example/\\\"q\\\"", "https://radio.example/\\\\", "https://radio.example/\xc3\xa9",
    "https://radio.example/\xf0\x9f\x98\x80", "https://radio.example/\xff\xfe", "https://radio.example/\\u0000x", "\\ud83d\\ude00",
    "https://radio.example/\\x41", "https://radio.example/\\ud83d\\ude0", " https://radio.example/",
};
static const char *const J_OTHER[] = {
    "null", "12", "true", "false", "[]", "{}", "[\"https://radio.example/\"]", "{\"a\":\"https://radio.example/\"}", "-1e9", "\"", "",
    "nul", "0.5", "[null,[null,[null]]]", "tru\"e\"",
};
static const char *const J_WORDS[] = {
    "\"homepage\":", "\"favicon\":", "\"https://radio.example/\"", "[", "]", "{", "}", ",", ":", "\"", "\\", "\\u", "\\ud83d", "\\\"",
    "null", " ", "\n", "[{", "}]", "{}", "\"\"", "\\u0000", "\0", "http://", "https://", "1", "-", "e",
};

static void j_ws(void)
{
    static const char *const S[] = {"", "", "", "", " ", "\n", "\t", "\r\n", "  "};
    w_str(PICK(S));
}

/* An address as long as there's room for one (a line's 255, the reader's own
 * 511), and the lengths around those: plain, or of characters of several
 * bytes, or of ones written \uXXXX, that a cut mustn't fall inside */
static void j_long(void)
{
    static const size_t LEN[] = {230, 231, 232, 233, 234, 235, 487, 488, 489, 490, 491, 492, 600, 5000};
    static const char *const UNIT[] = {"a", "\xc3\xa9", "\xf0\x9f\x98\x80", "\\u00e9", "\\ud83d\\ude00", "\\/", "a\xe2\x82\xac"};
    w_str("\"https://radio.example/");               /* (22 bytes: with those, 252 to 257 and 509 to 514) */
    w_rep(PICK(UNIT), PICK(LEN));
    w_str(one_in(8) ? "" : "\"");
}

static void j_url(void)
{
    switch (rn(12)) {
    case 0: w_str(PICK(J_OTHER)); break;
    case 1: j_long(); break;
    default: w_fmt("\"%s\"", PICK(J_URLS)); break;
    }
}

static void j_station(void)
{
    static const char *const REST[] = {
        "\"changeuuid\":\"960e57c2-0601-11e8-ae97-52543be04c81\"", "\"stationuuid\":\"96062a7b-0601-11e8-ae97-52543be04c81\"",
        "\"name\":\"Radio \\u00e9 \\\"X\\\"\"", "\"url\":\"http://stream.example/live\"", "\"url_resolved\":\"http://stream.example/live\"",
        "\"tags\":\"jazz,pop\"", "\"votes\":123", "\"codec\":\"MP3\"", "\"bitrate\":128", "\"hls\":0", "\"lastcheckok\":1", "\"geo_lat\":null",
        "\"geo_long\":48.85", "\"has_extended_info\":false", "\"nested\":{\"homepage\":\"https://inner.example/\",\"a\":[1,2,{\"b\":null}]}",
        "\"bitrate\":9999", "\"bitrate\":10000", "\"bitrate\":-1", "\"bitrate\":99999999999999999999", "\"bitrate\":\"128\"", "\"hls\":1",
        "\"lastcheckok\":0", "\"codec\":\"UNKNOWN\"", "\"stationuuid\":\"not-hex\"", "\"name\":\"  \\n \"", "\"name\":\" a \\t b \"",
        "\"HOMEPAGE\":\"https://upper.example/\"", "\"homepage \":\"https://space.example/\"", "\"\":\"\"", "\"home\\u0070age\":\"https://esc.example/\"",
        "\"favicon\"", "\"homepage\":", "homepage:\"https://bare.example/\"", "\"a\":[[[[[[[[1]]]]]]]]",
    };
    static const char *const CODECS[] = {"MP3", "AAC", "AAC+", "OGG", "FLAC", "UNKNOWN", "mp3", "AAC,H.264", "WMA", "", "MP3,AAC", ","};
    static const char *const LONG[] = {"name", "codec", "stationuuid", "url", "url_resolved", "bitrate"};
    static const char *const UNIT[] = {"a", "\xc3\xa9", "\xf0\x9f\x98\x80", "\\u00e9", "9", "a ", "0123456789abcdef-", "\\t"};
    static const size_t LEN[] = {7, 8, 9, 14, 15, 16, 17, 30, 31, 32, 33, 38, 39, 40, 41, 62, 63, 64, 65, 66, 254, 255, 256, 257, 510, 511, 512, 513, 1023, 1024, 1025};
    size_t i = 0;
    w_str("{");
    if (one_in(2)) {                                 /* what a station needs to be listed: a codec that plays, a stream, a name */
        w_fmt("\"codec\":\"%s\",\"url\":\"http%s://stream.example/%zu\",\"name\":\"Radio %zu\"", PICK(CODECS), one_in(2) ? "s" : "", rn(9), rn(99));
        i = 1;
    }
    for (size_t n = i + rn(10); i < n; i++) {
        if (i) j_ws(), w_str(one_in(30) ? "" : ",");
        j_ws();
        size_t k = rn(12);
        if (k < 3) w_str("\"homepage\""), j_ws(), w_str(":"), j_ws(), j_url();
        else if (k < 6) w_str("\"favicon\""), j_ws(), w_str(":"), j_ws(), j_url();
        else if (k < 7) w_fmt("\"%s\":\"%s", PICK(LONG), one_in(2) ? "http://s.example/" : ""), w_rep(PICK(UNIT), PICK(LEN)), w_str("\"");
        else if (k < 8) w_fmt("\"%s\":%s", PICK(LONG), PICK(J_OTHER));
        else w_str(PICK(REST));
    }
    j_ws();
    w_str("}");
}

static void gen_answer(void)
{
    static const char *const NOT[] = {
        "", " ", "<html>not the directory</html>", "{}", "null", "[", "[,", "[]", " [ ] ", "[[]]", "[{}]", "\xef\xbb\xbf[]", "]", "[{]", "[}",
        "[\"", "[\"\\", "[1,2,3]", "[{\"homepage\":", "[{\"homepage\":\"", "[{\"homepage\":\"https://radio.example/", "[{\"homepage\":\"\\",
        "[{\"homepage\":\"\\u", "[{\"homepage\":\"\\ud83d\\u", "\n\t [\n", "[,,,,]", "[{},{},{}]", "[{\"favicon\":\"https://i.example/a.png\"}",
    };
    static const size_t DEPTH[] = {62, 63, 64, 65, 66, 67, 200, 5000, 20000, WMAX};
    static const char *const NEST[] = {"[", "{\"a\":", "[{\"homepage\":", "[[", "{\"homepage\":{", "[{\"a\":[", "\"", "\\"};
    wn = 0;
    switch (rn(20)) {
    case 0: w_str(PICK(NOT)); break;
    case 1: {                                        /* one thing inside another, deeper than is followed */
        const char *u = PICK(NEST);
        size_t d = PICK(DEPTH);
        w_str(one_in(2) ? "[" : "");
        for (size_t i = 0; i < d; i++) w_str(u);
        if (one_in(2)) w_str("\"https://radio.example/\"");
        if (one_in(2)) w_rep(u[0] == '[' ? "]" : "}", d);
        break;
    }
    default:
        j_ws();
        w_str("[");
        for (size_t n = one_in(25) ? RDIR_MAX_STATIONS - 4 + rn(12) : rn(5), i = 0; i < n; i++) {
            if (i) w_str(one_in(30) ? "" : one_in(30) ? ",," : ",");
            j_ws();
            if (one_in(15)) w_str(PICK(J_OTHER));
            else j_station();
        }
        j_ws();
        w_str(one_in(15) ? "" : "]");
        if (one_in(30)) w_str("trailing");
        break;
    }
}

static void run_stream(size_t sn, size_t in)
{
    char about[64];
    snprintf(about, sizeof about, "room for %zu and %zu", sn, in);
    dump(about);
    char *text = exact(W, wn), *site = malloc(sn), *icon = malloc(in);
    if (sn) memset(site, 0xAA, sn);
    if (in) memset(icon, 0xAA, in);
    int r = rdir_parse_stream(text, wn, site, sn, icon, in);
    CHECK(r == -1 || r == 0 || r == 1);
    if (sn) CHECK(memchr(site, '\0', sn) && (!site[0] || (r == 1 && radio_icon_ok(site))));
    if (in) CHECK(memchr(icon, '\0', in) && (!icon[0] || (r == 1 && radio_icon_ok(icon))));
    g_rd->read += r == 1 && ((sn && site[0]) || (in && icon[0]));
    free(text);
    free(site);
    free(icon);
}

static void case_stream(void)
{
    static const size_t ROOM[] = {256, 256, 256, 256, 256, 256, 0, 1, 2, 8, 23, 24, 25, 255, 257, 511, 512, 513, 600};
    if (one_in(40)) {
        wn = rn(300);
        noise(W, wn);
        if (wn) W[0] = '[';
    } else {
        gen_answer();
        if (!one_in(4))
            for (size_t k = one_in(3) ? 2 + rn(4) : 1; k; k--) mut_text(J_WORDS, sizeof J_WORDS / sizeof *J_WORDS);
        if (one_in(5)) mut_len();
    }
    run_stream(one_in(6) ? rn(300) : PICK(ROOM), one_in(6) ? rn(300) : PICK(ROOM));
}

static int str_in(const char *s, size_t cap) { return memchr(s, '\0', cap) != NULL; }

/* The same answers as Find stations reads them (rdir_parse_stations: it keeps
 * a station's logo and site too) */
static void run_stations(int max, int with_meta)
{
    char about[48];
    snprintf(about, sizeof about, "room for %d%s", max, with_meta ? "" : ", no details");
    dump(about);
    char *text = exact(W, wn);
    radio_station_t *st = malloc((size_t)max * sizeof *st);
    rdir_meta_t *meta = with_meta ? malloc((size_t)max * sizeof *meta) : NULL;
    if (max && (!st || (with_meta && !meta))) abort();
    if (max) memset(st, 0xAA, (size_t)max * sizeof *st);
    if (max && meta) memset(meta, 0xAA, (size_t)max * sizeof *meta);
    int n = rdir_parse_stations(text, wn, st, meta, max);
    CHECK(n >= -1 && n <= max);
    for (int i = 0; i < n; i++) {
        const radio_station_t *s = &st[i];
        int whole = str_in(s->name, sizeof s->name) && str_in(s->url, sizeof s->url) && str_in(s->info, sizeof s->info) &&
                    str_in(s->icon, sizeof s->icon) && str_in(s->site, sizeof s->site) &&
                    (!meta || (str_in(meta[i].detail, sizeof meta[i].detail) && str_in(meta[i].uuid, sizeof meta[i].uuid)));
        CHECK(whole);
        if (!whole) break;
        CHECK(s->name[0] && (!strncmp(s->url, "http://", 7) || !strncmp(s->url, "https://", 8)) && !s->url[strcspn(s->url, " \t\r\n")]);
        CHECK(!s->icon[0] || radio_icon_ok(s->icon));
        CHECK(!s->site[0] || radio_icon_ok(s->site));
        for (int k = 0; k < i; k++) CHECK(strcmp(st[k].url, s->url) != 0);   /* each stream once */
    }
    g_rd->read += n > 0;
    free(text);
    free(st);
    free(meta);
}

static void case_stations(void)
{
    static const int ROOM[] = {RDIR_MAX_STATIONS, RDIR_MAX_STATIONS, RDIR_MAX_STATIONS, 0, 1, 2, 3};
    if (one_in(40)) {
        wn = rn(300);
        noise(W, wn);
        if (wn) W[0] = '[';
    } else {
        gen_answer();
        if (!one_in(3))
            for (size_t k = one_in(3) ? 2 + rn(4) : 1; k; k--) mut_text(J_WORDS, sizeof J_WORDS / sizeof *J_WORDS);
        if (one_in(5)) mut_len();
    }
    run_stations(PICK(ROOM), !one_in(4));
}

/* ---------- the stations' list ---------- */

static const char *const L_WORDS[] = {
    "|", " | ", "=", " = ", "icon:", "site:", "info:", " | icon: ", " | site: ", " | info: ", "http://", "https://", "\n", "\r", "#", " ", "\t",
    "https://img.example/logo.png", "= http://stream.example/live", "\0", ":", "| icon: https://img.example/a.png | site: https://radio.example/",
};

/* An address of n bytes */
static void l_long(size_t n)
{
    static const char *const UNIT[] = {"a", "a", "\xc3\xa9", "/x", "=", "a|", "a "};
    w_str("https://img.example/");                   /* (20 bytes) */
    w_rep(PICK(UNIT), n > 20 ? n - 20 : 0);
}

static void l_part(void)
{
    static const char *const SEP[] = {" | ", " | ", " | ", "|", " |", "| ", "||", " |\t", " |  "};
    static const char *const URLS[] = {
        "https://img.example/logo.png", "http://radio.example/", "https://radio.example/a/b?x=1&y=2", "", "img.example/logo.png",
        "ftp://img.example/logo.png", "https://img.example/a b.png", "HTTPS://img.example/logo.png", "https://", "http://x", "https://img.example/\xc3\xa9.png",
        "https://img.example/logo.png   ", "https://img.example/=.png", "https://img.example/logo.png\t",
    };
    static const char *const KEY[] = {"icon:", "icon:", "icon:", "site:", "site:", "site:", "icon", "site", "ICON:", "Site:", "icon :", "foo:", "", "icon:icon:", "info:icon:"};
    static const size_t LEN[] = {62, 63, 64, 65, 254, 255, 256, 257, 262, 263, 264, 265, 300, 1000};
    w_str(PICK(SEP));
    switch (rn(8)) {
    case 0: w_fmt("info: %s", one_in(2) ? "radiofrance:7" : "x"); break;
    case 1: w_str("info:"), w_rep("i", PICK(LEN)); break;
    case 2: w_str(PICK(KEY)), w_str(one_in(3) ? "" : " "), l_long(PICK(LEN)); break;
    default: w_str(PICK(KEY)), w_str(one_in(3) ? "" : " "), w_str(PICK(URLS)); break;
    }
}

static void gen_list(void)
{
    static const char *const NAMES[] = {"FIP", "Radio Example", "  spaced  ", "", "a=b", "x = y", "#not a note", "caf\xc3\xa9", "A | B", "= ", "http://name.example/"};
    static const char *const EQ[] = {" = ", " = ", " = ", "=", " =", "= ", "\t=\t", " == ", ""};
    static const char *const STREAMS[] = {
        "http://stream.example/live", "https://stream.example/live.mp3?x=1", "http://", "https://", "stream.example/live",
        "http://stream.example/a b", "ftp://stream.example/", "HTTP://stream.example/", "http://stream.example/live|", "https://s/=",
    };
    static const char *const OTHER[] = {"# a note\n", "\n", "\r\n", "   \n", "no stream here\n", "=\n", "= http://\n", "|\n", "#", " # = http://x\n", "\xef\xbb\xbf", "| icon: https://img.example/a.png\n"};
    static const char *const ENDS[] = {"\n", "\n", "\n", "\n", "\r\n", "\r", "", "\n\n", " \n", " |\n", "|", " | \n"};
    static const size_t LEN[] = {62, 63, 64, 65, 200, 510, 511, 512, 513, 514, 2000};
    wn = 0;
    for (size_t lines = one_in(10) ? RADIO_MAX - 2 + rn(6) : rn(6); lines; lines--) {
        if (one_in(5)) { w_str(PICK(OTHER)); continue; }
        if (one_in(20)) w_rep("n", PICK(LEN));
        else w_str(PICK(NAMES));
        w_str(PICK(EQ));
        if (one_in(20)) l_long(PICK(LEN));
        else w_str(PICK(STREAMS));
        for (size_t parts = one_in(3) ? 0 : 1 + rn(4); parts; parts--) l_part();
        w_str(PICK(ENDS));
    }
}

static void run_list(int max)
{
    char about[32];
    snprintf(about, sizeof about, "room for %d", max);
    dump(about);
    W[wn] = '\0';
    char *text = exact(W, wn + 1);
    radio_station_t *st = malloc((size_t)max * sizeof *st), *back = malloc(sizeof *back);
    if (!back || (max && !st)) abort();
    if (max) memset(st, 0xAA, (size_t)max * sizeof *st);
    int n = radio_parse(text, st, max);
    CHECK(n >= 0 && n <= max);
    for (int i = 0; i < n && i < max; i++) {
        const radio_station_t *s = &st[i];
        int whole = str_in(s->name, sizeof s->name) && str_in(s->url, sizeof s->url) && str_in(s->info, sizeof s->info) &&
                    str_in(s->icon, sizeof s->icon) && str_in(s->site, sizeof s->site);
        CHECK(whole);
        if (!whole) break;
        CHECK(s->name[0] && (!strncmp(s->url, "http://", 7) || !strncmp(s->url, "https://", 8)));
        CHECK(!s->icon[0] || radio_icon_ok(s->icon));
        CHECK(!s->site[0] || radio_icon_ok(s->site));
        /* written out as a line of the list (radio_line), its addresses read
         * back the same (under a plain name: one that has "= http://" in it
         * once its control characters are made spaces reads back as another
         * station, which is radio_line's to mind, not the reader's) */
        char line[1536];
        memset(line, 0xAA, sizeof line);
        *back = *s;
        snprintf(back->name, sizeof back->name, "N");
        radio_line(back, line, sizeof line);
        CHECK(str_in(line, 1200));
        memset(back, 0xAA, sizeof *back);
        CHECK(radio_parse(line, back, 1) == 1 && !strcmp(back->url, s->url) && !strcmp(back->icon, s->icon) && !strcmp(back->site, s->site));
    }
    g_rd->read += n > 0;
    free(text);
    free(st);
    free(back);
}

static void case_list(void)
{
    static const int ROOM[] = {RADIO_MAX, RADIO_MAX, RADIO_MAX, 0, 1, 1, 2, 3, RADIO_MAX - 1};
    if (one_in(40)) {
        wn = rn(300);
        noise(W, wn);
    } else {
        gen_list();
        if (!one_in(4))
            for (size_t k = one_in(3) ? 2 + rn(4) : 1; k; k--) mut_text(L_WORDS, sizeof L_WORDS / sizeof *L_WORDS);
        if (one_in(5)) mut_len();
    }
    run_list(PICK(ROOM));
}

/* ---------- the readers, and the passes that aren't by chance ---------- */

static reader_t READERS[] = {
    {"ico", case_ico, 0, 0, 0, 0}, {"site", case_site, 0, 0, 0, 0}, {"join", case_join, 0, 0, 0, 0},
    {"stream", case_stream, 0, 0, 0, 0}, {"stations", case_stations, 0, 0, 0, 0}, {"list", case_list, 0, 0, 0, 0},
};
#define N_READERS ((int)(sizeof READERS / sizeof *READERS))
static reader_t g_cut = {"cut", NULL, 0, 0, 0, 0}, g_long = {"long", NULL, 0, 0, 0, 0};

/* Good inputs cut at every length there is; and a small .ico's header and its
 * bitmap's (or its PNG's first chunk), with each awkward number written at
 * every place in them, as one, two and four bytes */
static void every_length(void)
{
    static const struct { int w, h, png, bpp; unsigned hs; int mask, alpha; } ICO[] = {
        {16, 16, 0, 32, 40, 1, 1}, {16, 16, 0, 24, 40, 1, 0}, {8, 8, 1, 32, 40, 0, 0}, {5, 3, 0, 32, 124, 0, 2},
        {1, 1, 0, 24, 40, 1, 0}, {33, 2, 0, 24, 40, 2, 0}, {16, 16, 0, 32, 40, 1, 0}, {2, 20, 1, 32, 40, 0, 0},
    };
    static const char *const PAGES[] = {
        "<!doctype html><html><head><title>T</title>\n<link rel=\"mask-icon\" href=\"/pin.svg\" color=\"#000\">\n"
        "<link rel=\"icon\" href=\"/fav-px16.png\" sizes=\"16x16\">\n<LINK HREF='img/touch-px180.png?v=1&amp;x=2' REL='apple-touch-icon'>\n"
        "<link rel=\"icon\" type=\"image/svg+xml\" href=\"/logo.svg\" sizes=\"any\">\n</head><body>radio</body></html>",
        "<head><link rel=icon href=//cdn.example/tiny-px16.png sizes=16x16>"
        "<link rel=\"shortcut icon\" sizes=\"96x96\" type=\"image/png\" href=\"icons/px96.png\"></head>",
        "<link rel=icon href=a.png><link rel=icon href=b.png sizes=64x64><link rel=icon href=c.png sizes=48x48>"
        "<link rel=icon href=d.png sizes=128x128><link rel=apple-touch-icon href=e.png><link rel=icon href=f.png?a=1&amp;b=2&amp;>",
        "<link href = \"https://cdn.example/a.png#x\" rel = 'ICON'/><link rel=\"icon\" href=\"../up.png\"><link rel=icon href=\"data:image/png;base64,AA==\">",
    };
    static const char *const ANSWERS[] = {
        "[{\"changeuuid\":\"960e57c2-0601-11e8-ae97-52543be04c81\",\"name\":\"Radio \\u00e9\",\"url\":\"http://stream.example/live\","
        "\"homepage\":\"https:\\/\\/radio.example\\/\",\"favicon\":\"https://img.example/\\ud83d\\ude00.png\",\"votes\":12,\"hls\":0,\"geo_lat\":null}]",
        "[{\"name\":\"L\",\"homepage\":\"\",\"favicon\":\"null\"},\n {\"name\":\"L2\",\"homepage\":\"https://bare.example/\","
        "\"favicon\":\"https://img.example/px68.png\",\"nested\":{\"a\":[1,2,{\"b\":null}]}}]",
        " [ { \"favicon\" : \"https://img.example/a.png\" } , { \"homepage\" : \"https://radio.example/\" } ] ",
    };
    static const char *const LISTS[] = {
        "# Your stations\nFIP = https://stream.example/fip.mp3 | info: radiofrance:7 | icon: https://img.example/fip.png | site: https://www.radiofrance.fr/fip\n"
        "Other = http://stream.example/other|site:https://radio.example/|icon:https://img.example/o.png\r\n\nLast=http://stream.example/last | icon: nothing",
        "a=b = http://s.example/1 | icon: https://i.example/1.png | icon: https://i.example/2.png |  | site:  https://r.example/  |",
    };
    static unsigned char keep[8192];
    size_t n;
    g_stage = "cut";
    g_case = 0;
    g_rd = &g_cut;
    seed_case("cut", 0);
    for (size_t c = 0; c < sizeof ICO / sizeof *ICO; c++) {
        ico_begin(1);
        ico_add(ICO[c].w, ICO[c].h, ICO[c].png, ICO[c].bpp, ICO[c].hs, ICO[c].mask, ICO[c].alpha);
        n = wn;
        if (n > sizeof keep) abort();
        memcpy(keep, W, n);
        run_ico(1);
        for (wn = 0; wn < n; wn++, g_case++, g_rd->n++) run_ico(0);
        size_t head = n < 62 ? n : 62;               /* the directory, and the picture's own header */
        for (size_t at = 0; at < head && c < (getenv("FATHOM_TEST_ON_H2") ? 1 : 4); at++)   /* (10,000 small decodes each: one is enough of the H2's time) */
            for (size_t v = 0; v < N_EDGE32; v++)
                for (size_t width = 1; width <= 4 && at + width <= n; width *= 2, g_case++, g_rd->n++) {
                    unsigned long x = EDGE32[v];
                    memcpy(W, keep, n);
                    wn = n;
                    if (width == 1) W[at] = (unsigned char)x;
                    else if (width == 2) le16(W + at, x & 0xffff);
                    else le32(W + at, x);
                    run_ico(0);
                }
    }
    for (size_t c = 0; c < sizeof PAGES / sizeof *PAGES; c++)
        for (size_t b = 0; b < 2; b++) {
            snprintf(g_base, sizeof g_base, "%s", b ? "https://radio.example/a/b/page.html?x=1" : "http://radio.example");
            wn = 0;
            w_str(PAGES[c]);
            n = wn;
            for (wn = 0; wn <= n; wn++, g_case++, g_rd->n++) run_site();
        }
    for (size_t c = 0; c < sizeof ANSWERS / sizeof *ANSWERS; c++) {
        wn = 0;
        w_str(ANSWERS[c]);
        n = wn;
        for (wn = 0; wn <= n; wn++, g_case++, g_rd->n++) run_stream(wn % 7 ? 256 : wn % 40, wn % 5 ? 256 : wn % 40);
    }
    for (size_t c = 0; c < sizeof LISTS / sizeof *LISTS; c++) {
        wn = 0;
        w_str(LISTS[c]);
        n = wn;
        for (wn = 0; wn <= n; wn++, g_case++, g_rd->n++) {
            memcpy(W, LISTS[c], n);                  /* (run_list ends it with a NUL where it's cut) */
            run_list(wn % 3 ? RADIO_MAX : 1);
        }
    }
}

/* What takes a reader longest, at the most a fetch hands it (a page's 96 KB,
 * an answer's 24 KB): none may take long, for each is read on the H2's one
 * core while a station plays */
static void timed(const char *what, void (*run)(void), double most_ms)
{
    double t0 = now_ms();
    run();
    double ms = now_ms() - t0;
    if (ms > g_rd->slow) g_rd->slow = ms, g_rd->slow_at = g_case;
    if (ms > most_ms) printf("FAIL: %s (case \"long %ld\") took %.0f ms\n", what, g_case, ms), fails++;
    else if (getenv("FUZZ_TIMES")) printf("  long %ld: %-58s %8.2f ms\n", g_case, what, ms);
    g_case++, g_rd->n++;
}

static void run_stream_usual(void) { run_stream(256, 256); }
static void run_stations_usual(void) { run_stations(RDIR_MAX_STATIONS, 1); }

static void the_longest(void)
{
    enum { PAGE = 96 * 1024 - 1, ANSWER = 24 * 1024 - 1 };
    double most = getenv("FATHOM_TEST_ON_H2") ? 20000 : 2000;
    g_stage = "long";
    g_case = 0;
    g_rd = &g_long;
    snprintf(g_base, sizeof g_base, "https://radio.example/a/b/page.html");
    wn = 0, w_rep("<link", PAGE);
    timed("a page of nothing but tags' starts", run_site, most);
    wn = 0, w_rep("<link", PAGE - 1), w_str(">");
    timed("...that all end at the page's last byte", run_site, most);
    wn = 0, w_rep("<link rel=icon href=/a.png>", PAGE);
    timed("a page of nothing but icons", run_site, most);
    wn = 0;
    while (wn + 2001 <= PAGE) w_rep("<link ", 1980), w_str(" href=a rel=icon   >");
    timed("...of tags as long as are read, starting every 6 bytes", run_site, most);
    wn = 0;
    while (wn + 2001 <= PAGE) w_rep("<link rel=icon href='", 1995), w_str("'>");
    timed("...of tags that are one long attribute", run_site, most);
    wn = 0, w_rep("<link rel=icon href=\"&amp;&amp;&amp;&amp;&amp;&amp;&amp;&amp;&amp;&amp;&amp;&amp;&amp;&amp;&amp;&amp;&amp;&amp;&amp;&amp;&amp;\">", PAGE);
    timed("...of addresses that are all &amp;", run_site, most);
    wn = 0, w_rep("[", ANSWER);
    timed("an answer of nothing but [", run_stream_usual, most);
    wn = 0, w_str("["), w_rep("{},", ANSWER - 1);
    timed("...of empty stations", run_stream_usual, most);
    wn = 0, w_str("[{"), w_rep("\"a\":1,", ANSWER - 30), w_str("\"homepage\":\"https://r.example/\"}]");
    timed("...of one station with thousands of members", run_stream_usual, most);
    memset(g_val, '[', 62), memset(g_val + 62, ']', 62), memcpy(g_val + 124, ",", 2);
    wn = 0, w_str("["), w_rep(g_val, ANSWER - 1);
    timed("...of lists 62 deep", run_stream_usual, most);
    wn = 0, w_str("[{\"homepage\":\""), w_rep("\\ud83d\\ude00", ANSWER - 20), w_str("\"}]");
    timed("...of one address of escapes", run_stream_usual, most);
    /* (more than is ever fetched: a list inside a list is followed 64 deep
     * and no further, whatever the answer's length; on the stack of a thread
     * of the H2's, 128 KB, each level followed would be a level deeper) */
    wn = 0, w_rep("[", WMAX);
    timed("...of 400 KB of nothing but [", run_stream_usual, most);
    wn = 0, w_str("["), w_rep("{\"codec\":\"MP3\",\"url\":\"http://stream.example/live\",\"name\":\"N\",\"favicon\":\"https://i.example/a.png\"},", RDIR_MAX_ANSWER - 2);
    timed("a list of stations that are all one station", run_stations_usual, most);
}

int main(int argc, char **argv)
{
    long cases = argc > 1 ? atol(argv[1]) : getenv("FATHOM_TEST_ON_H2") ? 400 : 20000;
    const char *only = argc > 3 ? argv[3] : NULL;
    long first = argc > 4 ? atol(argv[4]) : 0;
    if (argc > 2) g_seed = strtoull(argv[2], NULL, 0);
    g_dump = getenv("FUZZ_DUMP");
    setvbuf(stdout, NULL, _IOLBF, 0);
    static const int SIGS[] = {SIGSEGV, SIGBUS, SIGABRT, SIGFPE, SIGILL, SIGALRM};
    for (size_t i = 0; i < sizeof SIGS / sizeof *SIGS; i++) signal(SIGS[i], died);
    unsigned stuck_s = getenv("FATHOM_TEST_ON_H2") ? 600 : 120;      /* (a batch of cases never takes this long) */

    alarm(stuck_s);
    if (!only || !strcmp(only, "cut")) every_length();
    alarm(stuck_s);
    if (!only || !strcmp(only, "long")) the_longest();
    for (int r = 0; r < N_READERS; r++) {
        g_rd = &READERS[r];
        g_stage = g_rd->name;
        if (only && strcmp(only, g_rd->name)) continue;
        for (long i = first; i < first + cases; i++) {
            if (!(i & 63)) alarm(stuck_s);
            g_case = i;
            seed_case(g_stage, i);
            double t0 = now_ms();
            g_rd->one();
            double ms = now_ms() - t0;
            if (ms > g_rd->slow) g_rd->slow = ms, g_rd->slow_at = i;
            g_rd->n++;
        }
    }
    alarm(0);
    for (int r = -2; r < N_READERS; r++) {
        const reader_t *rd = r == -2 ? &g_cut : r == -1 ? &g_long : &READERS[r];
        if (!rd->n) continue;
        printf("%-8s %8ld cases, %8ld read as good", rd->name, rd->n, rd->read);
        if (rd != &g_cut) printf("; the longest %.1f ms (case %ld)", rd->slow, rd->slow_at);
        printf("\n");
    }
    if (!fails) printf("fuzz_radioicon: ok\n");
    return fails != 0;
}
