/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* Integer arithmetic throughout the signal path: Fathom is built soft-float
 * (the X1000's FPU unused), and a float MAC there costs a library call. Samples are
 * Q31, coefficients Q30, sums 64-bit (MIPS madd). The filters are designed in
 * floating point once; the 48 <-> 44.1 kHz table is designed once per process. */
#include "audio/btconv.h"
#include "audio/pcm.h"

#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define PASS_HZ 20000.0     /* flat up to here */
#define ATTEN_DB 100.0      /* below 16-bit's noise floor */
#define HB_TAPS 255         /* longest halfband */
#define Q 30                /* coefficient scale: 1.0 = 1 << Q */

struct btconv {
    unsigned in_rate, fam_rate, out_rate;
    int factor, channels;
    /* 1: decimation by factor, as halving stages (one or two halfbands) */
    struct halfband {
        int n, c, nk;                /* taps (4m+3), centre, non-zero taps on one side */
        int32_t mid, k[HB_TAPS / 4 + 1];   /* centre tap; taps at c +- 1, 3, 5, ... */
        int32_t hist[2][2 * HB_TAPS];      /* the last n inputs (Q30) per channel, twice */
        int pos, phase;
    } hb[2];
    int n_hb;
    /* 2: rational L/M (up L, down M), polyphase: phase p's taps are poly[p * K ..] */
    int L, M, K;
    const int32_t *poly;             /* shared, designed once (see rational_table) */
    int32_t *rh[2];                  /* the last K inputs per channel, twice */
    int rpos, rphase;
    /* 3 */
    eq_t *eq;
    int32_t *tmp;                    /* the link-rate frames before the dither */
    long tmp_cap;
    /* 4 */
    uint32_t rng;
};

unsigned btconv_rate_for(unsigned in_rate)
{
    static const unsigned outs[] = {44100, 48000, 32000};
    for (unsigned i = 0; i < 3; i++)
        for (unsigned f = 1; f <= 4; f *= 2)
            if (in_rate == outs[i] * f) return outs[i];
    return 0;
}

static double bessel_i0(double x)
{
    double s = 1, t = 1;
    for (int k = 1; k < 60; k++) {
        t *= (x / (2 * k)) * (x / (2 * k));
        s += t;
        if (t < 1e-12 * s) break;
    }
    return s;
}

/* What is kept flat of sound whose lowest rate on its way is `rate`: 20 kHz,
 * or 0.45 of the rate where it can't carry that (32 kHz: to 14.4 kHz). With
 * 20 kHz for every rate, a 32 kHz filter's stop band fell below its pass band:
 * no filter came out, and 32 kHz sound couldn't play over Bluetooth. */
static double pass_for(double rate) { return rate / 2 > PASS_HZ ? PASS_HZ : 0.45 * rate; }

/* Taps a Kaiser low-pass needs at rate fs: flat to pass, ATTEN_DB down from stop. */
static int taps_for(double fs, double pass, double stop)
{
    double dw = 2 * M_PI * (stop - pass) / fs;
    return ((int)ceil((ATTEN_DB - 8) / (2.285 * dw)) + 1) | 1;
}

/* Kaiser-windowed sinc low-pass at rate fs, flat to pass, ATTEN_DB down
 * from stop: n taps (odd; taps_for(), or n_force) into h as Q30, scaled to
 * gain. Returns n. */
static int lowpass(int32_t *h, int max, double fs, double pass, double stop, double gain, int n_force)
{
    int n = n_force ? n_force : taps_for(fs, pass, stop);
    if (n > max) n = max;
    n |= 1;
    if (n > max) n -= 2;
    double beta = 0.1102 * (ATTEN_DB - 8.7), i0b = bessel_i0(beta);
    double fc = (pass + stop) / 2 / fs, sum = 0;
    double *v = malloc((size_t)n * sizeof *v);
    if (!v) return 0;
    for (int i = 0; i < n; i++) {
        double m = i - (n - 1) / 2.0;
        double sinc = m == 0 ? 2 * fc : sin(2 * M_PI * fc * m) / (M_PI * m);
        double r = 2.0 * i / (n - 1) - 1;
        v[i] = sinc * bessel_i0(beta * sqrt(fmax(0, 1 - r * r))) / i0b;
        sum += v[i];
    }
    for (int i = 0; i < n; i++) h[i] = (int32_t)lrint(v[i] / sum * gain * (double)(1 << Q));
    free(v);
    return n;
}

/* The two inner loops. On MIPS32 the running sum stays in HI/LO (madd) for the
 * whole loop: compiled C moves it out after every madd, and each move waits for
 * the multiply to finish. */
/* sum of s[t] * h[t], t < n */
static inline int64_t dot(const int32_t *s, const int32_t *h, int n)
{
#if defined(__mips__) && !defined(__mips64)
    uint32_t lo, hi;
    int32_t a, b;
    __asm__ volatile(
        ".set push\n\t.set reorder\n\t"
        "mtlo $zero\n\tmthi $zero\n\t"
        "blez %[n], 2f\n"
        "1:\n\t"
        "lw %[a], 0(%[s])\n\t"
        "lw %[b], 0(%[h])\n\t"
        "addiu %[s], %[s], 4\n\t"
        "addiu %[h], %[h], 4\n\t"
        "addiu %[n], %[n], -1\n\t"
        "madd %[a], %[b]\n\t"
        "bgtz %[n], 1b\n"
        "2:\n\t"
        "mflo %[lo]\n\tmfhi %[hi]\n\t"
        ".set pop"
        : [lo] "=r"(lo), [hi] "=r"(hi), [a] "=&r"(a), [b] "=&r"(b), [s] "+r"(s), [h] "+r"(h), [n] "+r"(n)
        :
        : "hi", "lo", "memory");
    return (int64_t)(((uint64_t)hi << 32) | lo);
#else
    int64_t acc = 0;
    for (int t = 0; t < n; t++) acc += (int64_t)s[t] * h[t];
    return acc;
#endif
}

/* sum of (lo[-2j] + hi[2j]) * k[j], j < n: a halfband's taps either side of
 * its centre (lo = centre - 1, hi = centre + 1) */
static inline int64_t dot_sym(const int32_t *lo_p, const int32_t *hi_p, const int32_t *k, int n)
{
#if defined(__mips__) && !defined(__mips64)
    uint32_t lo, hi;
    int32_t a, b, c;
    __asm__ volatile(
        ".set push\n\t.set reorder\n\t"
        "mtlo $zero\n\tmthi $zero\n\t"
        "blez %[n], 2f\n"
        "1:\n\t"
        "lw %[a], 0(%[l])\n\t"
        "lw %[b], 0(%[u])\n\t"
        "lw %[c], 0(%[k])\n\t"
        "addu %[a], %[a], %[b]\n\t"
        "addiu %[l], %[l], -8\n\t"
        "addiu %[u], %[u], 8\n\t"
        "addiu %[k], %[k], 4\n\t"
        "addiu %[n], %[n], -1\n\t"
        "madd %[a], %[c]\n\t"
        "bgtz %[n], 1b\n"
        "2:\n\t"
        "mflo %[lo]\n\tmfhi %[hi]\n\t"
        ".set pop"
        : [lo] "=r"(lo), [hi] "=r"(hi), [a] "=&r"(a), [b] "=&r"(b), [c] "=&r"(c),
          [l] "+r"(lo_p), [u] "+r"(hi_p), [k] "+r"(k), [n] "+r"(n)
        :
        : "hi", "lo", "memory");
    return (int64_t)(((uint64_t)hi << 32) | lo);
#else
    int64_t acc = 0;
    for (int j = 0; j < n; j++) acc += (int64_t)(lo_p[-2 * j] + hi_p[2 * j]) * k[j];
    return acc;
#endif
}

static unsigned gcd(unsigned a, unsigned b) { while (b) { unsigned t = a % b; a = b; b = t; } return a; }

/* The polyphase table for fam -> link, designed once (it takes a while
 * with floats emulated) and shared. */
static const int32_t *rational_table(unsigned fam, unsigned link, int *L, int *M, int *K)
{
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    /* (room for every pair: 3 families to 3 link rates, 6 that differ; with
     * 3, a fourth pair's table was made again at each start and never freed) */
    static struct { unsigned fam, link; int L, M, K; int32_t *poly; } cache[6];
    unsigned g = gcd(fam, link);
    *L = (int)(link / g);
    *M = (int)(fam / g);
    pthread_mutex_lock(&mu);
    for (int i = 0; i < 6; i++)
        if (cache[i].poly && cache[i].fam == fam && cache[i].link == link) {
            *K = cache[i].K;
            pthread_mutex_unlock(&mu);
            return cache[i].poly;
        }
    double fs = (double)fam * *L;                          /* the upsampled rate */
    double lower = fam < link ? fam : link, pass = pass_for(lower);
    int max = *L * 96;
    int32_t *h = malloc((size_t)max * sizeof *h);
    int n = h ? lowpass(h, max, fs, pass, lower - pass, *L, 0) : 0;
    int k = (n + *L - 1) / *L;
    int32_t *poly = n ? calloc((size_t)*L * k, sizeof *poly) : NULL;
    if (poly)
        for (int p = 0; p < *L; p++)                      /* phase p, tap j: h[p + j L] */
            for (int j = 0; j < k; j++) {
                int i = p + j * *L;
                poly[p * k + j] = i < n ? h[i] : 0;
            }
    free(h);
    for (int i = 0; poly && i < 6; i++)
        if (!cache[i].poly) {
            cache[i].fam = fam;
            cache[i].link = link;
            cache[i].K = k;
            cache[i].poly = poly;
            break;
        }
    pthread_mutex_unlock(&mu);
    *K = k;
    return poly;
}

static void hb_design(struct halfband *b, double fs, double pass)
{
    int32_t h[HB_TAPS];
    int n = taps_for(fs, pass, fs / 2 - pass);
    while ((n - 3) % 4) n += 2;                    /* 4m + 3: the outermost taps aren't zero */
    if (n > HB_TAPS) n = HB_TAPS - (HB_TAPS - 3) % 4;
    n = lowpass(h, HB_TAPS, fs, pass, fs / 2 - pass, 1.0, n);
    b->n = n;
    b->c = (n - 1) / 2;
    b->mid = h[b->c];
    b->nk = 0;
    for (int j = 1; j <= b->c; j += 2) b->k[b->nk++] = h[b->c + j];
}

/* One input frame (Q31) into a halving stage; 1 and the output in y when one falls due. */
static int hb_run(struct halfband *b, const int32_t x[2], int32_t y[2])
{
    for (int ch = 0; ch < 2; ch++) {
        b->hist[ch][b->pos] = x[ch] >> 1;          /* Q30: pairs below add without overflow */
        b->hist[ch][b->pos + b->n] = x[ch] >> 1;
    }
    b->pos = b->pos + 1 == b->n ? 0 : b->pos + 1;
    if (++b->phase < 2) return 0;
    b->phase = 0;
    for (int ch = 0; ch < 2; ch++) {
        const int32_t *s = &b->hist[ch][b->pos];   /* the window, oldest first */
        int64_t acc = (int64_t)s[b->c] * b->mid + dot_sym(&s[b->c - 1], &s[b->c + 1], b->k, b->nk);
        y[ch] = pcm_sat32(acc >> (Q - 1));             /* Q30 x Q30 back to Q31 */
    }
    return 1;
}

btconv_t *btconv_create(unsigned in_rate, int channels, unsigned link_rate)
{
    unsigned fam = btconv_rate_for(in_rate);
    if (!fam || channels < 1 || channels > 2 || btconv_rate_for(link_rate) != link_rate) return NULL;
    btconv_t *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->in_rate = in_rate;
    c->fam_rate = fam;
    c->out_rate = link_rate;
    c->factor = (int)(in_rate / fam);
    c->channels = channels;
    c->rng = 0x12345678u;
    /* 1: halving stages. Each is a halfband (cutoff at a quarter of its input
     * rate: every other tap is zero, and the taps are symmetric). The last
     * stops at fam - 20 kHz, where aliases would land below 20 kHz; a first
     * one (from 4 x fam) stops at 2 fam - 20 kHz, beyond what the last removes.
     * (20 kHz, or what fam can carry: pass_for.) */
    if (c->factor == 4) hb_design(&c->hb[c->n_hb++], 4.0 * fam, pass_for(fam));
    if (c->factor >= 2) hb_design(&c->hb[c->n_hb++], 2.0 * fam, pass_for(fam));
    /* 2 */
    if (fam != link_rate) {
        c->poly = rational_table(fam, link_rate, &c->L, &c->M, &c->K);
        for (int ch = 0; ch < 2; ch++) c->rh[ch] = calloc((size_t)2 * c->K, sizeof(int32_t));
        if (!c->poly || !c->rh[0] || !c->rh[1]) {
            btconv_destroy(c);
            return NULL;
        }
    }
    return c;
}

void btconv_destroy(btconv_t *c)
{
    if (!c) return;
    free(c->rh[0]);
    free(c->rh[1]);
    free(c->tmp);
    free(c);
}

unsigned btconv_out_rate(const btconv_t *c) { return c->out_rate; }
double btconv_ratio(const btconv_t *c) { return (double)c->in_rate / c->out_rate; }

long btconv_max_out(const btconv_t *c, long n)
{
    long m = n / c->factor + 1;
    if (c->L) m = m * c->L / c->M + 2;
    return m;
}

void btconv_reset(btconv_t *c)
{
    for (int i = 0; i < c->n_hb; i++) {
        memset(c->hb[i].hist, 0, sizeof c->hb[i].hist);
        c->hb[i].pos = c->hb[i].phase = 0;
    }
    for (int ch = 0; ch < 2 && c->rh[0]; ch++) memset(c->rh[ch], 0, (size_t)2 * c->K * sizeof(int32_t));
    c->rpos = c->rphase = 0;
}

static inline uint32_t rnd(btconv_t *c)
{
    c->rng ^= c->rng << 13;
    c->rng ^= c->rng >> 17;
    c->rng ^= c->rng << 5;
    return c->rng;
}

/* Q31 to 16 bits, with TPDF dither (two uniform values, +-1 LSB peak) and rounding. */
static inline int16_t to16(btconv_t *c, int32_t x)
{
    uint32_t r = rnd(c);
    int32_t d = (int32_t)(r & 0xffff) - (int32_t)(r >> 16);    /* (-65536, 65536): +-1 LSB of 16 */
    int64_t v = ((int64_t)x + d + 0x8000) >> 16;
    return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}

/* Sum of n Q31 samples times Q30 taps, back to Q31. */
static inline int32_t fir(const int32_t *s, const int32_t *h, int n)
{
    return pcm_sat32(dot(s, h, n) >> Q);
}

/* Step 2 for one family-rate frame: outputs as many frames as fall due. */
static long rational(btconv_t *c, const int32_t x[2], int32_t *out)
{
    c->rpos = c->rpos == 0 ? c->K - 1 : c->rpos - 1;   /* newest first: rh[rpos .. rpos + K - 1] */
    for (int ch = 0; ch < 2; ch++) {
        c->rh[ch][c->rpos] = x[ch];
        c->rh[ch][c->rpos + c->K] = x[ch];
    }
    long w = 0;
    while (c->rphase < c->L) {                 /* outputs at phases rphase, rphase + M, ... */
        const int32_t *h = &c->poly[c->rphase * c->K];
        for (int ch = 0; ch < 2; ch++) out[2 * w + ch] = fir(&c->rh[ch][c->rpos], h, c->K);
        w++;
        c->rphase += c->M;
    }
    c->rphase -= c->L;                         /* the next input advances one input sample */
    return w;
}

void btconv_set_eq(btconv_t *c, eq_t *eq) { c->eq = eq; }

long btconv_run(btconv_t *c, const int32_t *in, long n, int16_t *out)
{
    long need = btconv_max_out(c, n) * 2;
    if (need > c->tmp_cap) {
        int32_t *t = realloc(c->tmp, (size_t)need * sizeof *t);
        if (!t) return 0;
        c->tmp = t;
        c->tmp_cap = need;
    }
    int32_t *f = c->tmp;
    long w = 0;
    for (long i = 0; i < n; i++) {
        int32_t x[2];
        x[0] = in[i * c->channels];
        x[1] = c->channels == 2 ? in[i * c->channels + 1] : x[0];
        int due = 1;
        for (int s = 0; s < c->n_hb && due; s++) due = hb_run(&c->hb[s], x, x);
        if (!due) continue;
        if (c->L) {
            w += rational(c, x, f + 2 * w);
        } else {
            f[2 * w] = x[0];
            f[2 * w + 1] = x[1];
            w++;
        }
    }
    if (c->eq) eq_run(c->eq, f, w);
    for (long i = 0; i < 2 * w; i++) out[i] = to16(c, f[i]);
    return w;
}
