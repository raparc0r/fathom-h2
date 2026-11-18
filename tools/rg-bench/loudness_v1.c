/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * FROZEN: app/audio/loudness.c as of 0.8.3, names prefixed v1_, for comparison
 * (tools/rg-bench, tests/test_loudness.c). Don't change it.
 *
 * ITU-R BS.1770-4: K-weighting (a high shelf, then a high pass), mean square
 * over 400 ms blocks every 100 ms, an absolute gate at -70 LUFS and a relative
 * gate 10 LU below the mean of the blocks that pass it. The filter
 * coefficients are worked out for any sample rate from the 48 kHz design (as
 * libebur128 does).
 *
 * The filters run in fixed point: the H2 has no FPU, and emulated doubles made
 * measuring a track take longer than playing it. Samples are taken at 24 bits,
 * coefficients in Q28, products and sums in 64 bits (a 32x32 multiply on
 * MIPS); floating point is used once per 100 ms block. The error is far below
 * the -70 LUFS gate.
 */
#include "loudness_v1.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct { double b0, b1, b2, a1, a2; } biquad_t;
typedef struct { int32_t b0, b1, b2, a1, a2; } qbiquad_t;   /* Q28 */
#define QS 28

struct v1_loudness {
    int channels;
    qbiquad_t shelf, hp;
    int32_t x1[2][2], x2[2][2], y1[2][2], y2[2][2];   /* per stage and channel: direct form I */
    long sub_len, sub_n;       /* 100 ms sub-blocks */
    int64_t sub_sum;           /* sum of squares, 24-bit samples (2^46 each) */
    int32_t peak_raw;
    double last[3];            /* the previous three sub-blocks' mean squares */
    int have;                  /* how many of them there are */
    double *blocks;            /* 400 ms block mean squares */
    long nblocks, cap;
};

static qbiquad_t q28(const biquad_t *f)
{
    qbiquad_t q;
    q.b0 = (int32_t)lrint(f->b0 * (1 << QS));
    q.b1 = (int32_t)lrint(f->b1 * (1 << QS));
    q.b2 = (int32_t)lrint(f->b2 * (1 << QS));
    q.a1 = (int32_t)lrint(f->a1 * (1 << QS));
    q.a2 = (int32_t)lrint(f->a2 * (1 << QS));
    return q;
}

/* The 48 kHz design (BS.1770), re-derived for `rate`. */
static void design(v1_loudness_t *l, double rate)
{
    biquad_t shelf, hp;
    double f0 = 1681.974450955533, g = 3.999843853973347, q = 0.7071752369554196;
    double k = tan(M_PI * f0 / rate);
    double vh = pow(10.0, g / 20.0), vb = pow(vh, 0.4996667741545416);
    double a0 = 1.0 + k / q + k * k;
    shelf.b0 = (vh + vb * k / q + k * k) / a0;
    shelf.b1 = 2.0 * (k * k - vh) / a0;
    shelf.b2 = (vh - vb * k / q + k * k) / a0;
    shelf.a1 = 2.0 * (k * k - 1.0) / a0;
    shelf.a2 = (1.0 - k / q + k * k) / a0;

    f0 = 38.13547087602444;
    q = 0.5003270373238773;
    k = tan(M_PI * f0 / rate);
    a0 = 1.0 + k / q + k * k;
    hp.b0 = 1.0;           /* as BS.1770 and libebur128: unity gain at high frequencies */
    hp.b1 = -2.0;
    hp.b2 = 1.0;
    hp.a1 = 2.0 * (k * k - 1.0) / a0;
    hp.a2 = (1.0 - k / q + k * k) / a0;
    l->shelf = q28(&shelf);
    l->hp = q28(&hp);
}

/* One direct-form-I biquad step in Q28, rounded. */
static inline int32_t qrun(const qbiquad_t *f, int32_t *x1, int32_t *x2, int32_t *y1, int32_t *y2, int32_t x)
{
    int64_t acc = (int64_t)f->b0 * x + (int64_t)f->b1 * *x1 + (int64_t)f->b2 * *x2
                - (int64_t)f->a1 * *y1 - (int64_t)f->a2 * *y2;
    int32_t y = (int32_t)((acc + (1 << (QS - 1))) >> QS);
    *x2 = *x1; *x1 = x;
    *y2 = *y1; *y1 = y;
    return y;
}

v1_loudness_t *v1_loudness_new(unsigned rate, int channels)
{
    if (!rate || channels < 1 || channels > 2) return NULL;
    v1_loudness_t *l = calloc(1, sizeof *l);
    if (!l) return NULL;
    l->channels = channels;
    design(l, rate);
    l->sub_len = (long)(rate / 10);
    return l;
}

void v1_loudness_free(v1_loudness_t *l)
{
    if (!l) return;
    free(l->blocks);
    free(l);
}

static void end_sub(v1_loudness_t *l)
{
    /* mean square of full scale = 1: 24-bit samples squared are 2^46 at full scale */
    double ms = (double)l->sub_sum / (70368744177664.0 * (double)l->sub_len);
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
    l->sub_sum = 0;
    l->sub_n = 0;
}

void v1_loudness_add(v1_loudness_t *l, const int32_t *frames, long n)
{
    for (long i = 0; i < n; i++) {
        for (int c = 0; c < l->channels; c++) {
            int32_t x = frames[i * l->channels + c] >> 8;            /* 24 bits */
            int32_t a = x < 0 ? -x : x;
            if (a > l->peak_raw) l->peak_raw = a;
            int32_t y = qrun(&l->shelf, &l->x1[0][c], &l->x2[0][c], &l->y1[0][c], &l->y2[0][c], x);
            y = qrun(&l->hp, &l->x1[1][c], &l->x2[1][c], &l->y1[1][c], &l->y2[1][c], y);
            l->sub_sum += (int64_t)y * y;
        }
        if (++l->sub_n == l->sub_len) end_sub(l);
    }
}

static double lufs_of(double ms) { return -0.691 + 10.0 * log10(ms); }

double v1_loudness_lufs(const v1_loudness_t *l)
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

double v1_loudness_peak(const v1_loudness_t *l) { return l->peak_raw / 8388608.0; }
