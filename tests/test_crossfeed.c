/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Crossfeed (audio/crossfeed.h): the whole-number filter against the same
 * design in doubles, and what it's for: each preset's bass separation, wide
 * separation in the treble, mono at its own level, no wrap-around at full
 * scale, and nothing carried over a reset. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio/crossfeed.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define FS 2147483648.0

/* The same filter in doubles (bs2b's formulas), on samples in -1..1 */
typedef struct { double a0l, b1l, a0h, a1h, b1h, g, lo[2], hi[2], p[2]; } ref_t;

static void ref_init(ref_t *r, int level, unsigned rate)
{
    int cut;
    double feed;
    crossfeed_design(level, &cut, &feed);
    memset(r, 0, sizeof *r);
    double gb_lo = feed * -5.0 / 6.0 - 3.0, gb_hi = feed / 6.0 - 3.0;
    double g_lo = pow(10.0, gb_lo / 20.0), g_hi = 1.0 - pow(10.0, gb_hi / 20.0);
    double fc_hi = cut * pow(2.0, (gb_lo - 20.0 * log10(g_hi)) / 12.0);
    double k = exp(-2.0 * M_PI * cut / rate);
    r->a0l = g_lo * (1.0 - k);
    r->b1l = k;
    k = exp(-2.0 * M_PI * fc_hi / rate);
    r->a0h = 1.0 - g_hi * (1.0 - k);
    r->a1h = -k;
    r->b1h = k;
    r->g = 1.0 / (1.0 - g_hi + g_lo);
}

static void ref_run(ref_t *r, double *b, long n)
{
    for (long i = 0; i < n; i++) {
        double in[2] = {b[2 * i], b[2 * i + 1]};
        for (int c = 0; c < 2; c++) {
            r->lo[c] = r->a0l * in[c] + r->b1l * r->lo[c];
            r->hi[c] = r->a0h * in[c] + r->a1h * r->p[c] + r->b1h * r->hi[c];
            r->p[c] = in[c];
        }
        b[2 * i] = r->g * (r->hi[0] + r->lo[1]);
        b[2 * i + 1] = r->g * (r->hi[1] + r->lo[0]);
    }
}

/* A sine in the left channel (and in the right too if both), amp of full scale */
static void sine(int32_t *buf, long n, unsigned rate, double hz, double amp, int both)
{
    for (long i = 0; i < n; i++) {
        double v = amp * sin(2.0 * M_PI * hz * (double)i / rate);
        buf[2 * i] = (int32_t)lrint(v * (FS - 1));
        buf[2 * i + 1] = both ? buf[2 * i] : 0;
    }
}

/* RMS of one channel over frames from..n */
static double rms(const int32_t *buf, long from, long n, int ch)
{
    double s = 0;
    for (long i = from; i < n; i++) s += (double)buf[2 * i + ch] * buf[2 * i + ch];
    return sqrt(s / (double)(n - from));
}

static double db(double x) { return 20.0 * log10(x); }

static void test_level(int level, unsigned rate)
{
    int cut;
    double feed;
    crossfeed_design(level, &cut, &feed);
    long n = rate;                                   /* 1 s; the second half measured */
    int32_t *buf = malloc((size_t)n * 2 * sizeof *buf);
    double *ref = malloc((size_t)n * 2 * sizeof *ref);
    crossfeed_t *x = crossfeed_new(level, rate);
    CHECK(x && crossfeed_level(x) == level && crossfeed_rate(x) == rate, "new %d at %u", level, rate);
    if (!x || !buf || !ref) { crossfeed_free(x); free(buf); free(ref); return; }

    /* the bass: one channel reaches the other ear feed dB down */
    sine(buf, n, rate, 60, 0.5, 0);
    crossfeed_run(x, buf, n);
    double sep = db(rms(buf, n / 2, n, 1) / rms(buf, n / 2, n, 0));
    CHECK(fabs(sep + feed) < 0.6, "%s at %u: 60 Hz separation %.2f dB, not -%.1f", crossfeed_name(level), rate, sep, feed);

    /* the treble: far apart */
    crossfeed_reset(x);
    sine(buf, n, rate, 10000, 0.5, 0);
    crossfeed_run(x, buf, n);
    double hsep = db(rms(buf, n / 2, n, 1) / rms(buf, n / 2, n, 0));
    CHECK(hsep < -feed - 10, "%s at %u: 10 kHz separation %.2f dB", crossfeed_name(level), rate, hsep);

    /* mono, in the bass: its own level */
    crossfeed_reset(x);
    sine(buf, n, rate, 60, 0.5, 1);
    double in = rms(buf, n / 2, n, 0);
    crossfeed_run(x, buf, n);
    double gain = db(rms(buf, n / 2, n, 0) / in);
    CHECK(fabs(gain) < 0.2, "%s at %u: mono 60 Hz comes out %.2f dB", crossfeed_name(level), rate, gain);

    /* full-scale noise: as the doubles have it (no wrap-around), within a few
     * hundred-thousandths of full scale; and a reset forgets everything */
    srand(7);
    for (long i = 0; i < 2 * n; i++) buf[i] = (int32_t)(((int64_t)rand() << 1 ^ rand()) & 0xffffffff);
    for (long i = 0; i < 2 * n; i++) ref[i] = buf[i] / FS;
    int32_t *copy = malloc((size_t)n * 2 * sizeof *copy);
    if (copy) memcpy(copy, buf, (size_t)n * 2 * sizeof *buf);
    crossfeed_reset(x);
    crossfeed_run(x, buf, n);
    ref_t r;
    ref_init(&r, level, rate);
    ref_run(&r, ref, n);
    double worst = 0;
    for (long i = 0; i < 2 * n; i++) {
        double want = ref[i] > 1.0 ? 1.0 : ref[i] < -1.0 ? -1.0 : ref[i];
        double d = fabs(buf[i] / FS - want);
        if (d > worst) worst = d;
    }
    CHECK(worst < 2e-6, "%s at %u: differs from the doubles by %.2g of full scale", crossfeed_name(level), rate, worst);
    if (copy) {
        int32_t *again = malloc((size_t)n * 2 * sizeof *again);
        if (again) {
            memcpy(again, copy, (size_t)n * 2 * sizeof *copy);
            crossfeed_reset(x);
            crossfeed_run(x, again, n);
            CHECK(!memcmp(again, buf, (size_t)n * 2 * sizeof *buf), "%s at %u: a reset left something behind", crossfeed_name(level), rate);
            free(again);
        }
        free(copy);
    }
    printf("%-6s %6u Hz: 60 Hz separation %6.2f dB, 10 kHz %6.2f dB, mono %+.2f dB, worst %.1g\n",
           crossfeed_name(level), rate, sep, hsep, gain, worst);
    crossfeed_free(x);
    free(buf);
    free(ref);
}

int main(void)
{
    CHECK(crossfeed_new(XF_OFF, 44100) == NULL, "off: no filter");
    CHECK(crossfeed_new(XF_MEDIUM, 0) == NULL, "no rate: no filter");
    CHECK(!strcmp(crossfeed_name(XF_STRONG), "Strong") && !strcmp(crossfeed_name(99), "Off"), "names");
    crossfeed_run(NULL, NULL, 0);                    /* (off: nothing to do) */
    static const unsigned RATES[] = {44100, 96000, 192000};
    for (int l = XF_LIGHT; l < XF_LEVELS; l++)
        for (int k = 0; k < 3; k++) test_level(l, RATES[k]);
    printf("%s (%d failed)\n", fails ? "FAILED" : "crossfeed: ok", fails);
    return fails != 0;
}
