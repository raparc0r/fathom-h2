/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Crossfeed for headphones: Bauer's filter in whole numbers (crossfeed.h). */
#include "audio/crossfeed.h"

#include <math.h>
#include <stdlib.h>

#define Q 30
#define ONE (1LL << Q)

struct crossfeed {
    int level;
    unsigned rate;
    int32_t a0_lo, b1_lo;          /* the other ear's low-pass */
    int32_t a0_hi, a1_hi, b1_hi;   /* this ear's high shelf */
    int32_t gain;                  /* the sum, back to its level (Q28: the sum may pass full scale) */
    int64_t lo[2], hi[2];          /* each channel's state: room above full scale (the shelf
                                    * overshoots on a signal that swings hard), only the
                                    * output saturated */
    int32_t prev[2];
};

static const struct { int cut; double feed; } DESIGN[XF_LEVELS] = {
    {0, 0}, {650, 9.5}, {700, 6.0}, {700, 4.5},
};

const char *crossfeed_name(int level)
{
    static const char *const N[XF_LEVELS] = {"Off", "Light", "Medium", "Strong"};
    return level >= 0 && level < XF_LEVELS ? N[level] : N[0];
}

void crossfeed_design(int level, int *cut_hz, double *feed_db)
{
    if (level < 0 || level >= XF_LEVELS) level = XF_OFF;
    if (cut_hz) *cut_hz = DESIGN[level].cut;
    if (feed_db) *feed_db = DESIGN[level].feed;
}

static int32_t q30(double v) { return (int32_t)lrint(v * (double)ONE); }

crossfeed_t *crossfeed_new(int level, unsigned rate)
{
    if (level <= XF_OFF || level >= XF_LEVELS || !rate) return NULL;
    crossfeed_t *x = calloc(1, sizeof *x);
    if (!x) return NULL;
    x->level = level;
    x->rate = rate;
    double fcut = DESIGN[level].cut, feed = DESIGN[level].feed;
    /* The other channel comes in feed dB below this one's in the bass (the
     * low-pass's level, against the shelf's low end), 3 dB down each, the
     * shelf's corner where its slope meets the low-pass's */
    double gb_lo = feed * -5.0 / 6.0 - 3.0, gb_hi = feed / 6.0 - 3.0;
    double g_lo = pow(10.0, gb_lo / 20.0), g_hi = 1.0 - pow(10.0, gb_hi / 20.0);
    double fc_hi = fcut * pow(2.0, (gb_lo - 20.0 * log10(g_hi)) / 12.0);
    double k = exp(-2.0 * M_PI * fcut / rate);
    x->a0_lo = q30(g_lo * (1.0 - k));
    x->b1_lo = q30(k);
    k = exp(-2.0 * M_PI * fc_hi / rate);
    x->a0_hi = q30(1.0 - g_hi * (1.0 - k));
    x->a1_hi = q30(-k);
    x->b1_hi = q30(k);
    x->gain = (int32_t)lrint(ldexp(1.0 / (1.0 - g_hi + g_lo), 28));
    return x;
}

void crossfeed_free(crossfeed_t *x) { free(x); }

void crossfeed_reset(crossfeed_t *x)
{
    if (!x) return;
    for (int c = 0; c < 2; c++) x->lo[c] = x->hi[c] = x->prev[c] = 0;
}

int crossfeed_level(const crossfeed_t *x) { return x ? x->level : XF_OFF; }
unsigned crossfeed_rate(const crossfeed_t *x) { return x ? x->rate : 0; }

static inline int32_t sat(int64_t v)
{
    return v > INT32_MAX ? INT32_MAX : v < INT32_MIN ? INT32_MIN : (int32_t)v;
}

/* (a product sum, rounded back to the samples' scale) */
static inline int64_t back(int64_t acc) { return (acc + (1LL << (Q - 1))) >> Q; }
static inline int64_t back28(int64_t acc) { return (acc + (1LL << 27)) >> 28; }

void crossfeed_run(crossfeed_t *x, int32_t *buf, long n)
{
    if (!x) return;
    /* The states stay within about twice full scale (the shelf's worst case,
     * a signal swinging from one end to the other each sample): 33 bits, so
     * every product below fits 64 (Q30 x 33 bits, Q28 x 34 bits). */
    int64_t lo0 = x->lo[0], lo1 = x->lo[1], hi0 = x->hi[0], hi1 = x->hi[1];
    int32_t p0 = x->prev[0], p1 = x->prev[1];
    for (long i = 0; i < n; i++) {
        int32_t l = buf[2 * i], r = buf[2 * i + 1];
        lo0 = back((int64_t)x->a0_lo * l + (int64_t)x->b1_lo * lo0);
        lo1 = back((int64_t)x->a0_lo * r + (int64_t)x->b1_lo * lo1);
        hi0 = back((int64_t)x->a0_hi * l + (int64_t)x->a1_hi * p0 + (int64_t)x->b1_hi * hi0);
        hi1 = back((int64_t)x->a0_hi * r + (int64_t)x->a1_hi * p1 + (int64_t)x->b1_hi * hi1);
        p0 = l;
        p1 = r;
        buf[2 * i] = sat(back28((int64_t)x->gain * (hi0 + lo1)));
        buf[2 * i + 1] = sat(back28((int64_t)x->gain * (hi1 + lo0)));
    }
    x->lo[0] = lo0; x->lo[1] = lo1; x->hi[0] = hi0; x->hi[1] = hi1; x->prev[0] = p0; x->prev[1] = p1;
}
