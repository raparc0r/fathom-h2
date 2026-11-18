/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * BS.1770-4 integrated loudness in double precision throughout (the filters
 * included), as libebur128 does it: the yardstick for the fixed-point
 * measurement (app/audio/loudness.c). Far too slow for the H2's soft float;
 * for the host, the tests, and rg-bench --ref.
 */
#include "loudness_ref.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

struct loudness_ref {
    int channels;
    double sb0, sb1, sb2, sa1, sa2, ha1, ha2;
    double z[2][4];            /* per channel: shelf and high pass, direct form II transposed */
    long sub_len, sub_n;
    double sub_sum, last[3];
    int have;
    double *blocks;
    long nblocks, cap;
    double peak;
};

loudness_ref_t *loudness_ref_new(unsigned rate, int channels)
{
    if (rate < 10 || channels < 1 || channels > 2) return NULL;
    loudness_ref_t *l = calloc(1, sizeof *l);
    if (!l) return NULL;
    l->channels = channels;
    l->sub_len = (long)(rate / 10);
    double f0 = 1681.974450955533, g = 3.999843853973347, q = 0.7071752369554196;
    double k = tan(M_PI * f0 / rate);
    double vh = pow(10.0, g / 20.0), vb = pow(vh, 0.4996667741545416);
    double a0 = 1.0 + k / q + k * k;
    l->sb0 = (vh + vb * k / q + k * k) / a0;
    l->sb1 = 2.0 * (k * k - vh) / a0;
    l->sb2 = (vh - vb * k / q + k * k) / a0;
    l->sa1 = 2.0 * (k * k - 1.0) / a0;
    l->sa2 = (1.0 - k / q + k * k) / a0;
    f0 = 38.13547087602444;
    q = 0.5003270373238773;
    k = tan(M_PI * f0 / rate);
    a0 = 1.0 + k / q + k * k;
    l->ha1 = 2.0 * (k * k - 1.0) / a0;
    l->ha2 = (1.0 - k / q + k * k) / a0;
    return l;
}

void loudness_ref_free(loudness_ref_t *l)
{
    if (!l) return;
    free(l->blocks);
    free(l);
}

static void end_sub(loudness_ref_t *l)
{
    double ms = l->sub_sum / (double)l->sub_len;
    l->sub_sum = 0;
    l->sub_n = 0;
    if (l->have < 3) { l->last[l->have++] = ms; return; }
    if (l->nblocks == l->cap) {
        long nc = l->cap ? l->cap * 2 : 1024;
        double *nb = realloc(l->blocks, (size_t)nc * sizeof *nb);
        if (!nb) return;
        l->blocks = nb;
        l->cap = nc;
    }
    l->blocks[l->nblocks++] = (l->last[0] + l->last[1] + l->last[2] + ms) / 4.0;
    memmove(l->last, l->last + 1, 2 * sizeof l->last[0]);
    l->last[2] = ms;
}

void loudness_ref_add(loudness_ref_t *l, const int32_t *frames, long n)
{
    for (long i = 0; i < n; i++) {
        for (int c = 0; c < l->channels; c++) {
            double x = frames[i * l->channels + c] / 2147483648.0, *z = l->z[c];
            if (fabs(x) > l->peak) l->peak = fabs(x);
            double s = l->sb0 * x + z[0];
            z[0] = l->sb1 * x - l->sa1 * s + z[1];
            z[1] = l->sb2 * x - l->sa2 * s;
            double y = s + z[2];
            z[2] = -2.0 * s - l->ha1 * y + z[3];
            z[3] = s - l->ha2 * y;
            l->sub_sum += y * y;
        }
        if (++l->sub_n == l->sub_len) end_sub(l);
    }
}

static double lufs_of(double ms) { return -0.691 + 10.0 * log10(ms); }

double loudness_ref_lufs(const loudness_ref_t *l)
{
    double sum = 0, gate;
    long n = 0;
    for (long i = 0; i < l->nblocks; i++)
        if (lufs_of(l->blocks[i]) > -70.0) { sum += l->blocks[i]; n++; }
    if (!n) return -HUGE_VAL;
    gate = lufs_of(sum / n) - 10.0;
    sum = 0;
    n = 0;
    for (long i = 0; i < l->nblocks; i++)
        if (lufs_of(l->blocks[i]) > -70.0 && lufs_of(l->blocks[i]) > gate) { sum += l->blocks[i]; n++; }
    return n ? lufs_of(sum / n) : -HUGE_VAL;
}

double loudness_ref_peak(const loudness_ref_t *l) { return l->peak; }
