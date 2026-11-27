/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* The Bluetooth format converter: rates, a flat passband, no aliasing below
 * 20 kHz, and plain 16-bit output at the track's own rate. */
#include "audio/btconv.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "test_util.h"

/* Level of frequency f (Hz) in the left channel, dB relative to full scale. */
static double level_db(const int16_t *s, long n, double f, double fs)
{
    double w = 2 * M_PI * f / fs, re = 0, im = 0;
    for (long i = 0; i < n; i++) {
        double win = 0.5 - 0.5 * cos(2 * M_PI * i / (n - 1));   /* Hann */
        re += s[2 * i] / 32768.0 * win * cos(w * i);
        im -= s[2 * i] / 32768.0 * win * sin(w * i);
    }
    double amp = 2 * sqrt(re * re + im * im) / (n * 0.5);
    return 20 * log10(amp + 1e-12);
}

/* Converts a sine of amplitude amp at f Hz to the link rate; returns its level at f_out. */
static double through_to(unsigned rate, unsigned link, double f, double amp, double f_out)
{
    btconv_t *c = btconv_create(rate, 2, link);
    long n = rate;                                /* one second */
    int32_t *in = malloc(n * 2 * sizeof *in);
    int16_t *out = malloc(btconv_max_out(c, n) * 2 * sizeof *out);
    for (long i = 0; i < n; i++)
        in[2 * i] = in[2 * i + 1] = (int32_t)(amp * 2147483647.0 * sin(2 * M_PI * f * i / rate));
    long w = btconv_run(c, in, n, out);
    double skip = 4096;                           /* past the filter's start */
    double db = level_db(out + 2 * (long)skip, w - (long)skip, f_out, btconv_out_rate(c));
    free(in);
    free(out);
    btconv_destroy(c);
    return db;
}

static double through(unsigned rate, double f, double amp, double f_out)
{
    return through_to(rate, btconv_rate_for(rate), f, amp, f_out);
}

int main(void)
{
    CHECKF(btconv_rate_for(44100) == 44100, "44.1");
    CHECKF(btconv_rate_for(96000) == 48000, "96");
    CHECKF(btconv_rate_for(176400) == 44100, "176.4");
    CHECKF(btconv_rate_for(192000) == 48000, "192");
    CHECKF(btconv_rate_for(22050) == 0, "22.05 has no whole ratio up");

    double ref = 20 * log10(0.5);
    unsigned rates[] = {88200, 96000, 176400, 192000};
    for (int r = 0; r < 4; r++) {
        unsigned rate = rates[r], out = btconv_rate_for(rate);
        for (double f = 1000; f <= 20000; f += 19000) {
            double d = through(rate, f, 0.5, f) - ref;
            CHECKF(fabs(d) < 0.05, "%u Hz: %.0f Hz off by %.3f dB", rate, f, d);
        }
        /* a tone just above the band: its alias would land at out - f */
        double f = out - 19000.0 + 2000;          /* aliases to 17 kHz */
        double a = through(rate, f, 0.5, out - f) - ref;
        CHECKF(a < -95, "%u Hz: %.0f Hz aliases at %.1f dB", rate, f, a);
        printf("%u -> %u Hz: taps ok, alias of %.0f Hz at %.1f dB\n", rate, out, f, a);
    }

    /* ÷4 in two halving stages: what only the first one blocks (80 kHz at 192 kHz
     * would fold to 16 kHz after it; 75 kHz at 176.4 kHz to 13.2 kHz) */
    double a1 = through(192000, 80000, 0.5, 16000) - ref;
    double a2 = through(176400, 75000, 0.5, 88200 - 75000) - ref;
    CHECKF(a1 < -95 && a2 < -95, "first halving stage: %.1f, %.1f dB", a1, a2);
    printf("first halving stage: 80 kHz at 192 kHz %.1f dB, 75 kHz at 176.4 kHz %.1f dB\n", a1, a2);

    /* across families (the link stays at one rate): flat, and no images or
     * aliases folding below 20 kHz */
    struct { unsigned in, link; } rs[] = {{48000, 44100}, {96000, 44100}, {192000, 44100}, {44100, 48000}};
    for (int r = 0; r < 4; r++) {
        for (double f = 1000; f <= 20000; f += 19000) {
            double d = through_to(rs[r].in, rs[r].link, f, 0.5, f) - ref;
            CHECKF(fabs(d) < 0.05, "%u -> %u Hz: %.0f Hz off by %.3f dB", rs[r].in, rs[r].link, f, d);
        }
    }
    /* 44.1 -> 48: 1 kHz has an image at 43.1 kHz, which would fold to 4.9 kHz */
    double img = through_to(44100, 48000, 1000, 0.5, 48000 - 43100) - ref;
    CHECKF(img < -95, "44.1 -> 48 kHz: image at %.1f dB", img);
    /* 48 -> 44.1: 21 kHz in folds only above 20 kHz, and is attenuated */
    double fold = through_to(48000, 44100, 21000, 0.5, 21000) - ref;
    printf("48 -> 44.1 kHz: 21 kHz at %.1f dB; 44.1 -> 48: image at %.1f dB\n", fold, img);

    /* 32 kHz sound (and its doubles), and a link at 32 kHz: a rate that can't
     * carry 20 kHz. Flat as far as it goes (0.45 of the lower rate: 14.4 kHz),
     * nothing folded below that. Before, no filter came out for these and the
     * track couldn't play over Bluetooth. */
    struct { unsigned in, link; } lo[] = {{32000, 44100}, {32000, 48000}, {64000, 44100}, {128000, 32000},
                                          {64000, 32000}, {44100, 32000}, {48000, 32000}};
    CHECKF(btconv_rate_for(32000) == 32000 && btconv_rate_for(64000) == 32000, "32 kHz");
    int made = 1;
    for (int r = 0; r < 7; r++) {
        btconv_t *t = btconv_create(lo[r].in, 2, lo[r].link);
        CHECKF(t != NULL, "%u -> %u Hz: no converter", lo[r].in, lo[r].link);
        if (!t) made = 0;
        btconv_destroy(t);
    }
    for (int r = 0; r < 7 && made; r++) {
        for (double f = 1000; f <= 14000; f += 13000) {
            double d = through_to(lo[r].in, lo[r].link, f, 0.5, f) - ref;
            CHECKF(fabs(d) < 0.05, "%u -> %u Hz: %.0f Hz off by %.3f dB", lo[r].in, lo[r].link, f, d);
        }
    }
    /* 32 -> 44.1: 1 kHz has an image at 31 kHz, which would fold to 13.1 kHz;
     * 64 -> 32: 19 kHz would fold to 13 kHz; 48 -> 32: 19 kHz the same */
    if (made) {
        double i1 = through_to(32000, 44100, 1000, 0.5, 44100 - 31000) - ref;
        double i2 = through_to(64000, 32000, 19000, 0.5, 13000) - ref;
        double i3 = through_to(48000, 32000, 19000, 0.5, 13000) - ref;
        CHECKF(i1 < -95 && i2 < -95 && i3 < -90, "32 kHz: image at %.1f dB, folds at %.1f and %.1f dB", i1, i2, i3);
        printf("32 -> 44.1 kHz: image at %.1f dB; 64 -> 32: 19 kHz folds at %.1f dB; 48 -> 32: at %.1f dB\n", i1, i2, i3);
    }

    /* the EQ at the link's rate, after the conversion: 192 kHz in, a 1 kHz
     * peak of +6 dB under a -6 dB preamp: 1 kHz at 0 dB, 100 Hz at -6 dB */
    {
        eq_profile_t p = {.preamp_db = -6, .n = 1, .band = {{EQ_PEAK, 1000, 6, 1.0}}};
        double want[2] = {eq_response_db(&p, 44100, 1000), eq_response_db(&p, 44100, 100)};
        double got[2];
        for (int k = 0; k < 2; k++) {
            double f = k ? 100 : 1000;
            btconv_t *cv = btconv_create(192000, 2, 44100);
            eq_t *e = eq_create(&p, 44100, 2);
            btconv_set_eq(cv, e);
            long n = 192000;
            int32_t *in = malloc(n * 2 * sizeof *in);
            int16_t *out = malloc(btconv_max_out(cv, n) * 2 * sizeof *out);
            for (long i = 0; i < n; i++) in[2 * i] = in[2 * i + 1] = (int32_t)(0.5 * 2147483647.0 * sin(2 * M_PI * f * i / 192000));
            long w = btconv_run(cv, in, n, out);
            got[k] = level_db(out + 2 * 8192, w - 8192, f, 44100) - ref;
            free(in); free(out);
            eq_destroy(e);
            btconv_destroy(cv);
        }
        CHECKF(fabs(got[0] - want[0]) < 0.05 && fabs(got[1] - want[1]) < 0.05,
              "EQ after the conversion: 1 kHz %.2f dB (designed %.2f), 100 Hz %.2f (designed %.2f)", got[0], want[0], got[1], want[1]);
        printf("EQ at the link's rate: 1 kHz %.2f dB, 100 Hz %.2f dB\n", got[0], got[1]);
    }

    /* 44.1 kHz: the same samples, dithered to 16 bits */
    btconv_t *c = btconv_create(44100, 2, 44100);
    int32_t in[8] = {0x40000000, -0x40000000, 0x40000000, -0x40000000};
    int16_t o[8];
    long w = btconv_run(c, in, 2, o);
    CHECKF(w == 2 && abs(o[0] - 16384) <= 1 && abs(o[1] + 16384) <= 1, "44.1 passthrough: %d %d", o[0], o[1]);
    btconv_destroy(c);

    return t_result();
}
