/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The parametric EQ: AutoEQ files, an exact pass-through when flat, the
 * integer filters against the double-precision design, low-frequency noise at
 * high rates, saturation. */
#include "audio/eq.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static const char *AUTOEQ =
    "Preamp: -6.4 dB\r\n"
    "Filter 1: ON LSC Fc 105 Hz Gain 6.0 dB Q 0.70\r\n"
    "Filter 2: ON PK Fc 2400 Hz Gain -3.1 dB Q 1.41\r\n"
    "Filter 3: OFF PK Fc 3000 Hz Gain 9.0 dB Q 2.00\r\n"
    "Filter 4: ON PK Fc 6000 Hz Gain 4.5 dB Q 3.00\r\n"
    "Filter 5: ON HSC Fc 10000 Hz Gain -2.0 dB Q 0.70\r\n";

/* Level (dB, relative to amplitude amp) of frequency f in channel 0 of s. */
static double level_db(const int32_t *s, int ch, long n, double f, double fs, double amp)
{
    double w = 2 * M_PI * f / fs, re = 0, im = 0, norm = 0;
    for (long i = 0; i < n; i++) {
        double win = 0.5 - 0.5 * cos(2 * M_PI * i / (n - 1));
        re += s[i * ch] / 2147483648.0 * win * cos(w * i);
        im -= s[i * ch] / 2147483648.0 * win * sin(w * i);
        norm += win;
    }
    return 20 * log10(2 * sqrt(re * re + im * im) / norm / amp + 1e-30);
}

/* A sine of amplitude amp at f through the profile at rate; its level after. */
static double through(const eq_profile_t *p, unsigned rate, double f, double amp)
{
    long n = rate, skip = rate / 4;          /* a second; the filters settle first */
    int32_t *s = malloc(n * 2 * sizeof *s);
    for (long i = 0; i < n; i++) s[2 * i] = s[2 * i + 1] = (int32_t)lround(amp * 2147483647.0 * sin(2 * M_PI * f * i / rate));
    eq_t *e = eq_create(p, rate, 2);
    eq_run(e, s, n);
    double db = level_db(s + 2 * skip, 2, n - skip, f, rate, amp);
    eq_destroy(e);
    free(s);
    return db;
}

int main(void)
{
    eq_profile_t p;
    char err[128];
    CHECK(eq_parse(AUTOEQ, &p, err, sizeof err) == 0, "parse: %s", err);
    CHECK(p.n == 4 && fabs(p.preamp_db + 6.4) < 1e-9, "parse: %d bands, preamp %.2f", p.n, p.preamp_db);
    CHECK(p.band[0].type == EQ_LOW_SHELF && p.band[0].fc == 105 && p.band[0].q == 0.70, "parse: band 1");
    CHECK(p.band[3].type == EQ_HIGH_SHELF && p.band[3].gain_db == -2.0, "parse: band 5");
    CHECK(eq_parse("Filter 1: ON LP Fc 100 Hz\n", &p, err, sizeof err) < 0, "an unsupported type is refused");
    CHECK(eq_parse("hello\n", &p, err, sizeof err) < 0, "not a profile");
    CHECK(eq_parse("Preamp: 0.0 dB\n", &p, err, sizeof err) == 0 && p.n == 0, "a flat profile (all bands removed)");
    /* what the H2's editor writes reads back the same */
    CHECK(eq_parse("Preamp: -3.5 dB\nFilter 1: ON LSC Fc 105 Hz Gain 2.5 dB Q 0.70\n"
                   "Filter 2: ON PK Fc 1000 Hz Gain -1.0 dB Q 1.41\nFilter 3: ON HSC Fc 10000 Hz Gain 1.5 dB Q 0.70\n",
                   &p, err, sizeof err) == 0 && p.n == 3 && p.band[1].q == 1.41 && p.band[2].type == EQ_HIGH_SHELF,
          "the editor's own format: %s", err);

    /* flat: every sample through unchanged */
    eq_profile_t flat = {0};
    eq_t *e = eq_create(&flat, 44100, 2);
    int32_t s[4096], c[4096];
    srand(1);
    for (int i = 0; i < 4096; i++) s[i] = c[i] = (int32_t)((unsigned)rand() << 1 ^ (unsigned)rand());
    eq_run(e, s, 2048);
    CHECK(!memcmp(s, c, sizeof s), "a flat profile isn't bit for bit");
    eq_destroy(e);

    /* the integer filters follow the design, at 44.1 and 192 kHz */
    eq_parse(AUTOEQ, &p, err, sizeof err);
    unsigned rates[] = {44100, 96000, 192000};
    double fs[] = {30, 105, 1000, 2400, 6000, 10000, 16000};
    for (int r = 0; r < 3; r++)
        for (int i = 0; i < 7; i++) {
            double want = eq_response_db(&p, rates[r], fs[i]), got = through(&p, rates[r], fs[i], 0.25);
            CHECK(fabs(got - want) < 0.02, "%u Hz, %.0f Hz: %.3f dB, designed %.3f", rates[r], fs[i], got, want);
        }
    printf("response: within 0.02 dB of the design at 44.1, 96 and 192 kHz\n");

    /* low-frequency filters at a high rate: what's left of the output once the
     * two test tones are fitted out is the filters' noise and distortion (the
     * tones' exact level and phase may differ from the ideal design by a hair:
     * Q28 coefficients; the response check above bounds that) */
    eq_profile_t low = {.preamp_db = -6, .n = 2,
                        .band = {{EQ_LOW_SHELF, 20, 6, 0.7}, {EQ_PEAK, 40, 5, 2}}};
    unsigned rate = 192000;
    long n = rate;
    int32_t *x = malloc(n * sizeof *x);
    const double tf[2] = {31, 997};
    for (long i = 0; i < n; i++)
        x[i] = (int32_t)lround((0.3 * sin(2 * M_PI * tf[0] * i / rate) + 0.1 * sin(2 * M_PI * tf[1] * i / rate)) * 2147483647.0);
    eq_t *el = eq_create(&low, rate, 1);
    eq_run(el, x, n);
    /* least squares over [n/4, n): x ~ sum a_k sin + b_k cos (+ dc) */
    long i0 = n / 4, m = n - i0;
    double basis[5], ata[5][5] = {{0}}, atb[5] = {0};
    for (long i = i0; i < n; i++) {
        double t = (double)i / rate;
        basis[0] = sin(2 * M_PI * tf[0] * t); basis[1] = cos(2 * M_PI * tf[0] * t);
        basis[2] = sin(2 * M_PI * tf[1] * t); basis[3] = cos(2 * M_PI * tf[1] * t); basis[4] = 1;
        for (int r = 0; r < 5; r++) {
            atb[r] += basis[r] * x[i];
            for (int c2 = 0; c2 < 5; c2++) ata[r][c2] += basis[r] * basis[c2];
        }
    }
    for (int k = 0; k < 5; k++)                       /* Gauss-Jordan, 5x5 */
        for (int r = 0; r < 5; r++) {
            if (r == k) continue;
            double f = ata[r][k] / ata[k][k];
            for (int c2 = 0; c2 < 5; c2++) ata[r][c2] -= f * ata[k][c2];
            atb[r] -= f * atb[k];
        }
    double coef[5], res2 = 0;
    for (int k = 0; k < 5; k++) coef[k] = atb[k] / ata[k][k];
    for (long i = i0; i < n; i++) {
        double t = (double)i / rate;
        double fit = coef[0] * sin(2 * M_PI * tf[0] * t) + coef[1] * cos(2 * M_PI * tf[0] * t) +
                     coef[2] * sin(2 * M_PI * tf[1] * t) + coef[3] * cos(2 * M_PI * tf[1] * t) + coef[4];
        res2 += (x[i] - fit) * (x[i] - fit);
    }
    double noise = 10 * log10(res2 / m) - 20 * log10(2147483648.0);
    CHECK(noise < -130, "low filters at 192 kHz: noise and distortion at %.1f dBFS", noise);
    printf("20 Hz shelf + 40 Hz peak at 192 kHz: noise and distortion %.1f dBFS (rms)\n", noise);
    eq_destroy(el);
    free(x);

    /* saturation: a boost past full scale clips, it doesn't wrap */
    eq_profile_t loud = {.preamp_db = 0, .n = 1, .band = {{EQ_PEAK, 1000, 12, 1}}};
    int32_t y[2 * 4410];
    for (int i = 0; i < 4410; i++) y[2 * i] = y[2 * i + 1] = (int32_t)lround(0.9 * 2147483647.0 * sin(2 * M_PI * 1000 * i / 44100.0));
    eq_t *eh = eq_create(&loud, 44100, 2);
    eq_run(eh, y, 4410);
    int wrapped = 0;
    for (int i = 4410 / 2; i < 4410; i++) {
        double want = sin(2 * M_PI * 1000 * i / 44100.0);   /* clipped, the sign stays */
        if ((want > 0.5 && y[2 * i] < 0) || (want < -0.5 && y[2 * i] > 0)) wrapped++;
    }
    CHECK(!wrapped, "%d samples wrapped round", wrapped);
    eq_destroy(eh);

    printf(fails ? "%d failures\n" : "eq ok\n", fails);
    return fails != 0;
}
