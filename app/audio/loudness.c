/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/*
 * ITU-R BS.1770-4: K-weighting (a high shelf, then a high pass), mean square
 * over 400 ms blocks every 100 ms, an absolute gate at -70 LUFS and a relative
 * gate 10 LU below the mean of the blocks that pass it. The filter
 * coefficients are worked out for any sample rate from the 48 kHz design (as
 * libebur128 does).
 *
 * The filters run in fixed point: Fathom is built soft-float, and emulated doubles made
 * measuring a track take longer than playing it. Samples are taken at 24 bits,
 * coefficients in Q28, products and sums in 64 bits (a 32x32 multiply on
 * MIPS); floating point is used once per 100 ms block. The error is far below
 * the -70 LUFS gate.
 *
 * The arithmetic is the same as 0.8.3's to the bit (tests/test_loudness.c and
 * tools/rg-bench check it against a frozen copy), in about half the
 * instructions: each channel goes through a sub-block at a time with its
 * filter state in registers (0.8.3's went through pointers into the struct,
 * so every sample loaded and stored all of it, and spilled its sums), and the
 * filters take fewer steps (kweight).
 */
#include "loudness.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct { double b0, b1, b2, a1, a2; } biquad_t;
typedef struct { int32_t b0, b1, b2, a1, a2; } qbiquad_t;   /* Q28 */
#define QS 28

/* One channel's filter state: input, shelf output (the high pass's input),
 * high pass output, each the last two samples, times 16 (kweight). */
typedef struct { int32_t x1, x2, s1, s2, y1, y2; } kstate_t;

struct loudness {
    int channels;
    qbiquad_t shelf;
    int32_t hp_a1, hp_a2;      /* the high pass's denominator, Q28 (its numerator is 1, -2, 1) */
    kstate_t st[2];
    long sub_len, sub_n;       /* 100 ms sub-blocks */
    int64_t sub_sum;           /* sum of squares, 24-bit samples (2^46 each) */
    int32_t peak_raw;
    double last[3];            /* the previous three sub-blocks' mean squares */
    int have;                  /* how many of them there are */
    double *blocks;            /* 400 ms block mean squares */
    long nblocks, cap;
};

static int32_t q28(double v) { return (int32_t)lrint(v * (1 << QS)); }

/* The 48 kHz design (BS.1770), re-derived for `rate`. */
static void design(loudness_t *l, double rate)
{
    double f0 = 1681.974450955533, g = 3.999843853973347, q = 0.7071752369554196;
    double k = tan(M_PI * f0 / rate);
    double vh = pow(10.0, g / 20.0), vb = pow(vh, 0.4996667741545416);
    double a0 = 1.0 + k / q + k * k;
    biquad_t shelf = {
        (vh + vb * k / q + k * k) / a0,
        2.0 * (k * k - vh) / a0,
        (vh - vb * k / q + k * k) / a0,
        2.0 * (k * k - 1.0) / a0,
        (1.0 - k / q + k * k) / a0,
    };
    l->shelf = (qbiquad_t){q28(shelf.b0), q28(shelf.b1), q28(shelf.b2), q28(shelf.a1), q28(shelf.a2)};

    /* the high pass: b0, b1, b2 = 1, -2, 1 as BS.1770 and libebur128 have it
     * (unity gain at high frequencies), exact in Q28, so its products are shifts */
    f0 = 38.13547087602444;
    q = 0.5003270373238773;
    k = tan(M_PI * f0 / rate);
    a0 = 1.0 + k / q + k * k;
    l->hp_a1 = q28(2.0 * (k * k - 1.0) / a0);
    l->hp_a2 = q28((1.0 - k / q + k * k) / a0);
}

loudness_t *loudness_new(unsigned rate, int channels)
{
    if (rate < 10 || channels < 1 || channels > 2) return NULL;   /* (a sub-block of at least a sample) */
    loudness_t *l = calloc(1, sizeof *l);
    if (!l) return NULL;
    l->channels = channels;
    design(l, rate);
    l->sub_len = (long)(rate / 10);
    return l;
}

void loudness_free(loudness_t *l)
{
    if (!l) return;
    free(l->blocks);
    free(l);
}

/*
 * One channel (every `stride`th sample from x) through both filters, n
 * samples: returns the sum of the squared output, raises *peak.
 *
 * Each filter's sum is rounded and shifted as 0.8.3 did it, (sum + 2^27) >> 28,
 * so the output is the same to the bit, but with fewer steps:
 *  - the high pass's numerator is (1, -2, 1), exact in Q28, so
 *      (2^28 (s - 2 s1 + s2) - a1 y1 - a2 y2 + 2^27) >> 28
 *        = s - 2 s1 + s2 + ((2^27 - a1 y1 - a2 y2) >> 28)
 *    (the first part is a whole multiple of 2^28): two multiplies, not five;
 *  - the filters' inputs are kept 16 times larger (state too), which makes
 *    the same sum 2^4 times larger: (16 sum + 2^31) >> 32 is the top word
 *    alone (one mfhi, no 64-bit shift). It fits: the shelf's impulse response
 *    sums to at most 2.25 in magnitude, the whole filter's to 3.43 and
 *    s - 2 s1 + s2's to 6.34 (at any rate), so from 24-bit samples the
 *    largest is 6.34 * 2^23 * 16 < 2^30.
 * On MIPS the sums stay in HI/LO (madd/msub, the rounding constant loaded
 * first, as in eq.c); GCC's own code for the 64-bit sums spills them.
 */
#if defined(__mips__) && !defined(__mips64)
#define MAC_ASM 1
/* hi = (2^31 + b0 X + b1 X1 + b2 X2 - a1 S1 - a2 S2) >> 32 */
#define SHELF(hi, X, X1, X2, S1, S2)                                                                     \
    __asm__(".set push\n\t.set reorder\n\t"                                                             \
            "mthi $zero\n\tmtlo %[r]\n\t"                                                                \
            "madd %[b0], %[x]\n\tmadd %[b1], %[x1]\n\tmadd %[b2], %[x2]\n\t"                              \
            "msub %[a1], %[s1]\n\tmsub %[a2], %[s2]\n\t"                                                 \
            "mfhi %[h]\n\t.set pop"                                                                      \
            : [h] "=r"(hi)                                                                               \
            : [r] "r"(rnd), [b0] "r"(b0), [x] "r"(X), [b1] "r"(b1), [x1] "r"(X1), [b2] "r"(b2), [x2] "r"(X2), \
              [a1] "r"(a1), [s1] "r"(S1), [a2] "r"(a2), [s2] "r"(S2)                                     \
            : "hi", "lo")
/* hi = (2^31 - h1 Y1 - h2 Y2) >> 32 */
#define HIGHPASS(hi, Y1, Y2)                                                                             \
    __asm__(".set push\n\t.set reorder\n\t"                                                             \
            "mthi $zero\n\tmtlo %[r]\n\t"                                                                \
            "msub %[h1], %[y1]\n\tmsub %[h2], %[y2]\n\t"                                                 \
            "mfhi %[h]\n\t.set pop"                                                                      \
            : [h] "=r"(hi)                                                                               \
            : [r] "r"(rnd), [h1] "r"(h1), [y1] "r"(Y1), [h2] "r"(h2), [y2] "r"(Y2)                       \
            : "hi", "lo")
/* (sh:sl) += y * y */
#define SQUARE(y)                                                                                        \
    __asm__(".set push\n\t.set reorder\n\t"                                                             \
            "mthi %[sh]\n\tmtlo %[sl]\n\tmadd %[v], %[v]\n\tmflo %[sl]\n\tmfhi %[sh]\n\t.set pop"         \
            : [sl] "+r"(sl), [sh] "+r"(sh)                                                               \
            : [v] "r"(y)                                                                                 \
            : "hi", "lo")
#else
#define SHELF(hi, X, X1, X2, S1, S2)                                                                     \
    hi = (int32_t)((rnd + (int64_t)b0 * (X) + (int64_t)b1 * (X1) + (int64_t)b2 * (X2) - (int64_t)a1 * (S1) - \
                    (int64_t)a2 * (S2)) >> 32)
#define HIGHPASS(hi, Y1, Y2) hi = (int32_t)((rnd - (int64_t)h1 * (Y1) - (int64_t)h2 * (Y2)) >> 32)
#define SQUARE(y) sum += (int64_t)(y) * (y)
#endif

/* One sample: in, then the state's older values (X2, S2, Y2) are replaced by
 * the new ones, so two steps with the roles swapped need no moves. */
#define STEP(p, X1, X2, S1, S2, Y1, Y2)                                                                  \
    do {                                                                                                 \
        int32_t in = *(p) >> 8, X, s, r, T, y;                                                           \
        if (in > mx) mx = in;                                                                            \
        if (in < mn) mn = in;                                                                            \
        X = in * 16;                                                                                     \
        SHELF(s, X, X1, X2, S1, S2);                                                                     \
        X2 = X;                                                                                          \
        T = s * 16 - 2 * S1 + S2;            /* 16 (s - 2 s1 + s2) */                                    \
        S2 = s * 16;                                                                                     \
        HIGHPASS(r, Y1, Y2);                                                                             \
        y = (T >> 4) + r;                    /* (T is a multiple of 16: exact) */                        \
        Y2 = y * 16;                                                                                     \
        SQUARE(y);                                                                                       \
    } while (0)

static inline __attribute__((always_inline)) int64_t kweight(const loudness_t *l, kstate_t *st, const int32_t *x,
                                                             const int stride, long n, int32_t *peak)
{
    const int32_t b0 = l->shelf.b0, b1 = l->shelf.b1, b2 = l->shelf.b2, a1 = l->shelf.a1, a2 = l->shelf.a2;
    const int32_t h1 = l->hp_a1, h2 = l->hp_a2;
    int32_t x1 = st->x1, x2 = st->x2, s1 = st->s1, s2 = st->s2, y1 = st->y1, y2 = st->y2;
    int32_t mx = 0, mn = 0;
#ifdef MAC_ASM
    uint32_t sl = 0, sh = 0;     /* the sum of squares, a 64-bit HI:LO between samples */
    const uint32_t rnd = 1u << 31;
#else
    int64_t sum = 0;
    const int64_t rnd = (int64_t)1 << 31;
#endif
    const int32_t *end = x + (n & ~1L) * stride;
    for (; x != end; x += 2 * stride) {
        STEP(x, x1, x2, s1, s2, y1, y2);              /* the new values into x2, s2, y2 */
        STEP(x + stride, x2, x1, s2, s1, y2, y1);     /* and back */
    }
    if (n & 1) {
        STEP(x, x1, x2, s1, s2, y1, y2);
        int32_t t;
        t = x1; x1 = x2; x2 = t;
        t = s1; s1 = s2; s2 = t;
        t = y1; y1 = y2; y2 = t;
    }
    *st = (kstate_t){x1, x2, s1, s2, y1, y2};
    if (mx > *peak) *peak = mx;
    if (-mn > *peak) *peak = -mn;
#ifdef MAC_ASM
    return (int64_t)(((uint64_t)sh << 32) | sl);
#else
    return sum;
#endif
}

static void end_sub(loudness_t *l)
{
    /* mean square of full scale = 1: 24-bit samples squared are 2^46 at full scale */
    double ms = (double)l->sub_sum / (70368744177664.0 * (double)l->sub_len);
    l->sub_sum = 0;
    l->sub_n = 0;              /* (even when there's no memory for the block: loudness_add counts on it) */
    if (l->have == 3) {
        double block = (l->last[0] + l->last[1] + l->last[2] + ms) / 4.0;
        if (l->nblocks == l->cap) {
            long nc = l->cap ? l->cap * 2 : 1024;
            double *nb = realloc(l->blocks, (size_t)nc * sizeof *nb);
            if (!nb) return;
            l->blocks = nb;
            l->cap = nc;
        }
        l->blocks[l->nblocks++] = block;
        memmove(l->last, l->last + 1, 2 * sizeof l->last[0]);
        l->last[2] = ms;
    } else {
        l->last[l->have++] = ms;
    }
}

void loudness_add(loudness_t *l, const int32_t *frames, long n)
{
    const int ch = l->channels;
    while (n > 0) {
        long m = l->sub_len - l->sub_n;                   /* up to the end of this sub-block */
        if (m > n) m = n;
        if (ch == 2) {                                    /* (the stride a constant in each) */
            l->sub_sum += kweight(l, &l->st[0], frames, 2, m, &l->peak_raw);
            l->sub_sum += kweight(l, &l->st[1], frames + 1, 2, m, &l->peak_raw);
        } else {
            l->sub_sum += kweight(l, &l->st[0], frames, 1, m, &l->peak_raw);
        }
        frames += m * ch;
        n -= m;
        if ((l->sub_n += m) == l->sub_len) end_sub(l);
    }
}

static double lufs_of(double ms) { return -0.691 + 10.0 * log10(ms); }

double loudness_lufs(const loudness_t *l)
{
    double sum = 0;
    long n = 0;
    for (long i = 0; i < l->nblocks; i++)
        if (lufs_of(l->blocks[i]) > -70.0) { sum += l->blocks[i]; n++; }
    if (!n) return -HUGE_VAL;
    double gate = lufs_of(sum / n) - 10.0;
    sum = 0;
    n = 0;
    for (long i = 0; i < l->nblocks; i++)
        if (lufs_of(l->blocks[i]) > -70.0 && lufs_of(l->blocks[i]) > gate) { sum += l->blocks[i]; n++; }
    return n ? lufs_of(sum / n) : -HUGE_VAL;
}

double loudness_peak(const loudness_t *l) { return l->peak_raw / 8388608.0; }
