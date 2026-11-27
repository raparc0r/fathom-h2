/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/*
 * Parametric EQ. The coefficients are worked out once in double precision
 * (Robert Bristow-Johnson's "Audio EQ Cookbook": peaking and shelving filters,
 * shelves with Q, as AutoEQ and Equalizer APO use them), then every sample goes
 * through integer biquads: Q28 coefficients, 64-bit sums, and the sum's
 * rounding remainder fed back into the next sample ("error feedback"). Without
 * that, a 20 Hz filter at 192 kHz would turn its rounding into audible noise;
 * with it the noise stays near the bottom of 32-bit samples.
 *
 * The preamp (negative in AutoEQ profiles: room for the boosts) comes first;
 * the output saturates instead of wrapping.
 */
#include "audio/eq.h"
#include "audio/pcm.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define FRAC 28                              /* coefficients: Q4.28 */

typedef struct {
    int32_t b0, b1, b2, a1, a2;
} coef_t;

typedef struct {
    int32_t x1, x2, y1, y2;
    int64_t err;
} state_t;

struct eq {
    unsigned rate;
    int channels, n;
    int32_t pre;                             /* preamp, Q2.30 */
    coef_t c[EQ_MAX_BANDS];
    state_t s[EQ_MAX_BANDS][EQ_MAX_CH];
};

/* ---- profiles ---- */

static const char *skip_ws(const char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

/* The number after `key` (case-insensitive) in line, or NAN. */
static double field(const char *line, const char *key)
{
    size_t k = strlen(key);
    for (const char *p = line; *p; p++) {
        if (strncasecmp(p, key, k) || (p > line && p[-1] != ' ' && p[-1] != '\t')) continue;
        const char *v = skip_ws(p + k);
        char *end;
        double d = strtod(v, &end);
        if (end != v) return d;
    }
    return NAN;
}

int eq_parse(const char *text, eq_profile_t *p, char *err, size_t cap)
{
    memset(p, 0, sizeof *p);
    int lineno = 0, filters = 0, preamp = 0;
    for (const char *l = text; l && *l;) {
        const char *nl = strchr(l, '\n');
        size_t len = nl ? (size_t)(nl - l) : strlen(l);
        char line[256];
        snprintf(line, sizeof line, "%.*s", (int)(len < sizeof line ? len : sizeof line - 1), l);
        l = nl ? nl + 1 : NULL;
        lineno++;
        char *cr = strchr(line, '\r');
        if (cr) *cr = '\0';
        const char *s = skip_ws(line);
        if (!strncasecmp(s, "Preamp:", 7)) {
            char *end;
            double d = strtod(s + 7, &end);
            if (end == s + 7) { snprintf(err, cap, "line %d: no preamp value", lineno); return -1; }
            p->preamp_db = d;
            preamp = 1;
            continue;
        }
        if (strncasecmp(s, "Filter", 6)) continue;            /* comments, other settings */
        const char *colon = strchr(s, ':');
        if (!colon) continue;
        const char *f = skip_ws(colon + 1);
        filters++;
        if (!strncasecmp(f, "OFF", 3)) continue;
        if (strncasecmp(f, "ON", 2)) continue;
        f = skip_ws(f + 2);
        eq_band_t b = {0};
        if (!strncasecmp(f, "PK ", 3) || !strncasecmp(f, "PEQ ", 4)) b.type = EQ_PEAK;
        else if (!strncasecmp(f, "LSC ", 4) || !strncasecmp(f, "LS ", 3)) b.type = EQ_LOW_SHELF;
        else if (!strncasecmp(f, "HSC ", 4) || !strncasecmp(f, "HS ", 3)) b.type = EQ_HIGH_SHELF;
        else { snprintf(err, cap, "line %d: filter type not supported (PK, LSC and HSC are)", lineno); return -1; }
        b.fc = field(f, "Fc");
        b.gain_db = field(f, "Gain");
        b.q = field(f, "Q");
        if (isnan(b.q)) b.q = b.type == EQ_PEAK ? 1.0 : 0.7071;   /* shelves without Q: Butterworth */
        if (isnan(b.fc) || isnan(b.gain_db) || b.fc <= 0 || b.q <= 0) {
            snprintf(err, cap, "line %d: needs Fc, Gain (and Q)", lineno);
            return -1;
        }
        if (fabs(b.gain_db) > 30) { snprintf(err, cap, "line %d: gain beyond 30 dB", lineno); return -1; }
        if (p->n == EQ_MAX_BANDS) { snprintf(err, cap, "more than %d filters", EQ_MAX_BANDS); return -1; }
        p->band[p->n++] = b;
    }
    if (!filters && !preamp) { snprintf(err, cap, "no Preamp or Filter lines"); return -1; }
    if (fabs(p->preamp_db) > 30) { snprintf(err, cap, "preamp beyond 30 dB"); return -1; }
    return 0;
}

int eq_load(const char *path, eq_profile_t *p, char *err, size_t cap)
{
    FILE *f = fopen(path, "rbe");
    if (!f) { snprintf(err, cap, "can't open it"); return -1; }
    char text[8192];
    size_t n = fread(text, 1, sizeof text - 1, f);
    fclose(f);
    text[n] = '\0';
    return eq_parse(text, p, err, cap);
}

/* ---- coefficients (double, once) ---- */

static void design(const eq_band_t *b, unsigned rate, double out[5])   /* b0 b1 b2 a1 a2, a0 = 1 */
{
    double A = pow(10.0, b->gain_db / 40.0), w0 = 2 * M_PI * b->fc / rate;
    double cw = cos(w0), alpha = sin(w0) / (2 * b->q), sa = 2 * sqrt(A) * alpha;
    double b0, b1, b2, a0, a1, a2;
    if (b->type == EQ_PEAK) {
        b0 = 1 + alpha * A; b1 = -2 * cw; b2 = 1 - alpha * A;
        a0 = 1 + alpha / A; a1 = -2 * cw; a2 = 1 - alpha / A;
    } else if (b->type == EQ_LOW_SHELF) {
        b0 = A * ((A + 1) - (A - 1) * cw + sa);
        b1 = 2 * A * ((A - 1) - (A + 1) * cw);
        b2 = A * ((A + 1) - (A - 1) * cw - sa);
        a0 = (A + 1) + (A - 1) * cw + sa;
        a1 = -2 * ((A - 1) + (A + 1) * cw);
        a2 = (A + 1) + (A - 1) * cw - sa;
    } else {
        b0 = A * ((A + 1) + (A - 1) * cw + sa);
        b1 = -2 * A * ((A - 1) + (A + 1) * cw);
        b2 = A * ((A + 1) + (A - 1) * cw - sa);
        a0 = (A + 1) - (A - 1) * cw + sa;
        a1 = 2 * ((A - 1) - (A + 1) * cw);
        a2 = (A + 1) - (A - 1) * cw - sa;
    }
    out[0] = b0 / a0; out[1] = b1 / a0; out[2] = b2 / a0; out[3] = a1 / a0; out[4] = a2 / a0;
}

/* A band too close to Nyquist (or above it) at this rate is left out. */
static int usable(const eq_band_t *b, unsigned rate) { return b->fc < 0.45 * rate; }

static int32_t q28(double v)
{
    double s = v * (1 << FRAC);
    return s >= 2147483647.0 ? INT32_MAX : s <= -2147483648.0 ? INT32_MIN : (int32_t)lround(s);
}

eq_t *eq_create(const eq_profile_t *p, unsigned rate, int channels)
{
    if (!p || !rate || channels < 1 || channels > EQ_MAX_CH) return NULL;
    eq_t *e = calloc(1, sizeof *e);
    if (!e) return NULL;
    e->rate = rate;
    e->channels = channels;
    double pre = pow(10.0, p->preamp_db / 20.0) * (1 << 30);
    e->pre = pre >= 2147483647.0 ? INT32_MAX : (int32_t)lround(pre);
    for (int i = 0; i < p->n; i++) {
        if (!usable(&p->band[i], rate) || p->band[i].gain_db == 0) continue;
        double c[5];
        design(&p->band[i], rate, c);
        e->c[e->n++] = (coef_t){q28(c[0]), q28(c[1]), q28(c[2]), q28(c[3]), q28(c[4])};
    }
    return e;
}

void eq_destroy(eq_t *e) { free(e); }
void eq_reset(eq_t *e) { if (e) memset(e->s, 0, sizeof e->s); }
unsigned eq_rate(const eq_t *e) { return e->rate; }
int eq_channels(const eq_t *e) { return e->channels; }

void eq_run(eq_t *e, int32_t *s, long frames)
{
    const int ch = e->channels;
    const int unity = e->pre == (1 << 30);
    for (int c = 0; c < ch; c++) {
        int32_t *x = s + c;
        if (!unity)
            for (long i = 0; i < frames; i++) x[i * ch] = pcm_sat32(((int64_t)x[i * ch] * e->pre) >> 30);
        for (int b = 0; b < e->n; b++) {
            const coef_t k = e->c[b];
            state_t st = e->s[b][c];
            for (long i = 0; i < frames; i++) {
                int32_t in = x[i * ch], out;
#if defined(__mips__) && !defined(__mips64)
                /* the sum stays in HI/LO: madd/msub, the remainder loaded first */
                uint32_t lo, hi;
                __asm__ volatile(
                    ".set push\n\t.set reorder\n\t"
                    "mthi $zero\n\tmtlo %[e]\n\t"
                    "madd %[b0], %[in]\n\tmadd %[b1], %[x1]\n\tmadd %[b2], %[x2]\n\t"
                    "msub %[a1], %[y1]\n\tmsub %[a2], %[y2]\n\t"
                    "mflo %[lo]\n\tmfhi %[hi]\n\t"
                    ".set pop"
                    : [lo] "=r"(lo), [hi] "=r"(hi)
                    : [e] "r"((uint32_t)st.err), [b0] "r"(k.b0), [in] "r"(in), [b1] "r"(k.b1), [x1] "r"(st.x1),
                      [b2] "r"(k.b2), [x2] "r"(st.x2), [a1] "r"(k.a1), [y1] "r"(st.y1), [a2] "r"(k.a2), [y2] "r"(st.y2)
                    : "hi", "lo");
                st.err = lo & ((1u << FRAC) - 1);
                int32_t top = (int32_t)hi >> (31 - (32 - FRAC));   /* the bits above the result: all sign? */
                if (top != 0 && top != -1) out = (int32_t)hi < 0 ? INT32_MIN : INT32_MAX;
                else out = (int32_t)((hi << (32 - FRAC)) | (lo >> FRAC));
#else
                int64_t acc = (int64_t)k.b0 * in + (int64_t)k.b1 * st.x1 + (int64_t)k.b2 * st.x2
                            - (int64_t)k.a1 * st.y1 - (int64_t)k.a2 * st.y2 + st.err;
                int64_t y = acc >> FRAC;                       /* floor */
                st.err = acc & ((1LL << FRAC) - 1);           /* the remainder (y is the floor), into the next sum */
                out = pcm_sat32(y);
#endif
                st.x2 = st.x1; st.x1 = in;
                st.y2 = st.y1; st.y1 = out;
                x[i * ch] = out;
            }
            e->s[b][c] = st;
        }
    }
}

void eq_curve_db(const eq_profile_t *p, unsigned rate, const double *f, int n, float *db)
{
    double c[EQ_MAX_BANDS][5];
    int m = 0;
    for (int i = 0; i < p->n && i < EQ_MAX_BANDS; i++)
        if (usable(&p->band[i], rate) && p->band[i].gain_db != 0) design(&p->band[i], rate, c[m++]);
    for (int k = 0; k < n; k++) {
        double w = 2 * M_PI * f[k] / rate, c1 = cos(w), s1 = sin(w), c2 = cos(2 * w), s2 = sin(2 * w), g = 1;
        for (int i = 0; i < m; i++) {   /* the power gains multiplied: one logarithm a point */
            double nr = c[i][0] + c[i][1] * c1 + c[i][2] * c2, ni = -c[i][1] * s1 - c[i][2] * s2;
            double dr = 1 + c[i][3] * c1 + c[i][4] * c2, di = -c[i][3] * s1 - c[i][4] * s2;
            g *= (nr * nr + ni * ni) / (dr * dr + di * di);
        }
        db[k] = (float)(10 * log10(g));
    }
}

double eq_response_db(const eq_profile_t *p, unsigned rate, double f)
{
    double w = 2 * M_PI * f / rate, db = p->preamp_db;
    for (int i = 0; i < p->n; i++) {
        if (!usable(&p->band[i], rate) || p->band[i].gain_db == 0) continue;
        double c[5];
        design(&p->band[i], rate, c);
        /* |H(e^jw)| = |b0 + b1 z^-1 + b2 z^-2| / |1 + a1 z^-1 + a2 z^-2| */
        double nr = c[0] + c[1] * cos(w) + c[2] * cos(2 * w), ni = -c[1] * sin(w) - c[2] * sin(2 * w);
        double dr = 1 + c[3] * cos(w) + c[4] * cos(2 * w), di = -c[3] * sin(w) - c[4] * sin(2 * w);
        db += 10 * log10((nr * nr + ni * ni) / (dr * dr + di * di));
    }
    return db;
}
