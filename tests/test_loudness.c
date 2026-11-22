/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* EBU R128 integrated loudness (app/audio/loudness.c) against reference signals,
 * 0.8.3's measurement (to the bit) and a double-precision one. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "audio/loudness.h"
#include "loudness_ref.h"
#include "loudness_v1.h"

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

/* A sine of `hz` at `dbfs` peak in both channels (or only the left), `secs` long. */
static double measure(unsigned rate, double hz, double dbfs, double secs, int both, double *peak)
{
    loudness_t *l = loudness_new(rate, 2);
    double amp = pow(10.0, dbfs / 20.0) * 2147483647.0;
    int32_t buf[2 * 1024];
    long total = (long)(rate * secs), done = 0;
    while (done < total) {
        long n = total - done < 1024 ? total - done : 1024;
        for (long i = 0; i < n; i++) {
            int32_t v = (int32_t)lrint(amp * sin(2 * M_PI * hz * (double)(done + i) / rate));
            buf[2 * i] = v;
            buf[2 * i + 1] = both ? v : 0;
        }
        loudness_add(l, buf, n);
        done += n;
    }
    double lufs = loudness_lufs(l);
    if (peak) *peak = loudness_peak(l);
    loudness_free(l);
    return lufs;
}

/* ---------- against 0.8.3's measurement (tools/rg-bench/loudness_v1.c) and
 * a double-precision one (loudness_ref.c) ---------- */

static uint32_t g_rng = 0x2545F491u;
static double white(void)                     /* RMS 1 */
{
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5;
    return ((double)g_rng / 4294967296.0 - 0.5) * 3.4641016;
}

enum { SINE, WHITE, PINK, MIX };

/* `secs` of a signal (the sine's peak or the noise's RMS at `dbfs`, quantised
 * to `bits`) through all three, in pieces of odd sizes (across sub-blocks'
 * ends). The new and the old must agree to the bit; both near the reference. */
static void compare(int sig, unsigned rate, int ch, int bits, double dbfs, double secs)
{
    loudness_t *ln = loudness_new(rate, ch);
    v1_loudness_t *lo = v1_loudness_new(rate, ch);
    loudness_ref_t *lr = loudness_ref_new(rate, ch);
    double amp = pow(10.0, dbfs / 20.0), pk[2][7] = {{0}};
    long total = (long)(rate * secs), done = 0, piece = 1;
    int32_t buf[2 * 3000];
    while (done < total) {
        long n = piece < total - done ? piece : total - done;
        piece = piece * 7 % 2999 + 1;
        for (long i = 0; i < n; i++) {
            double t = (double)(done + i) / rate;
            for (int c = 0; c < ch; c++) {
                double v;
                if (sig == SINE) v = sin(2 * M_PI * 1000.0 * t);
                else if (sig == MIX) v = 0.5 * sin(2 * M_PI * 55.0 * t + c) + 0.3 * sin(2 * M_PI * 440.0 * t) +
                                         0.2 * sin(2 * M_PI * 7000.0 * t + 2 * c);
                else if (sig == WHITE) v = white();
                else {                                /* Paul Kellet's pink filter */
                    double w = white(), *k = pk[c];
                    k[0] = 0.99886 * k[0] + w * 0.0555179;
                    k[1] = 0.99332 * k[1] + w * 0.0750759;
                    k[2] = 0.96900 * k[2] + w * 0.1538520;
                    k[3] = 0.86650 * k[3] + w * 0.3104856;
                    k[4] = 0.55000 * k[4] + w * 0.5329522;
                    k[5] = -0.7616 * k[5] - w * 0.0168980;
                    v = (k[0] + k[1] + k[2] + k[3] + k[4] + k[5] + k[6] + w * 0.5362) * 0.2;
                    k[6] = w * 0.115926;
                }
                v = fmin(fmax(v * amp, -1.0), 0.999999);
                int32_t q = (int32_t)lrint(v * (double)(1 << (bits - 1)));
                buf[i * ch + c] = (int32_t)((uint32_t)q << (32 - bits));
            }
        }
        loudness_add(ln, buf, n);
        v1_loudness_add(lo, buf, n);
        loudness_ref_add(lr, buf, n);
        done += n;
    }
    double vn = loudness_lufs(ln), vo = v1_loudness_lufs(lo), vr = loudness_ref_lufs(lr);
    printf("%d at %6u Hz, %d ch, %2d bits, %+5.1f dB: %8.4f LUFS (0.8.3: %8.4f, double: %8.4f)\n", sig, rate, ch,
           bits, dbfs, vn, vo, vr);
    CHECK(vn == vo);
    CHECK(loudness_peak(ln) == v1_loudness_peak(lo));
    CHECK(fabs(loudness_peak(ln) - loudness_ref_peak(lr)) < 2.0 / 8388608.0);
    CHECK(fabs(vn - vr) < 0.02);
    loudness_free(ln);
    v1_loudness_free(lo);
    loudness_ref_free(lr);
}

int main(void)
{
    double peak;
    static const unsigned all[] = {44100, 48000, 88200, 96000, 176400, 192000};
    for (int i = 0; i < 6; i++) {
        compare(SINE, all[i], 2, 24, -20, 6);
        compare(PINK, all[i], 2, 24, -20, 6);
    }
    compare(SINE, 44100, 2, 16, -20, 6);
    compare(SINE, 48000, 1, 24, -23, 6);
    compare(WHITE, 44100, 2, 16, -10, 6);
    compare(WHITE, 192000, 2, 24, -3, 3);
    compare(PINK, 44100, 2, 16, -45, 6);
    compare(MIX, 44100, 2, 16, -1, 6);
    compare(MIX, 96000, 2, 24, 0, 3);
    compare(SINE, 44100, 2, 24, 0, 3);       /* full scale */

    /* EBU Tech 3341, case 1: 1 kHz at -23 dBFS in both channels reads -23 LUFS */
    static const unsigned rates[] = {44100, 48000, 96000, 192000};
    for (int i = 0; i < 4; i++) {
        double v = measure(rates[i], 1000, -23, 20, 1, NULL);
        printf("%6u Hz: 1 kHz at -23 dBFS: %.2f LUFS\n", rates[i], v);
        CHECK(fabs(v + 23.0) < 0.1);
    }
    /* case 2: -33 dBFS reads -33 */
    CHECK(fabs(measure(48000, 1000, -33, 20, 1, NULL) + 33.0) < 0.1);
    /* one channel only: 3 dB less */
    CHECK(fabs(measure(48000, 1000, -23, 20, 0, NULL) + 26.0) < 0.1);
    /* peak of a full-scale sine */
    measure(48000, 1000, 0, 2, 1, &peak);
    CHECK(fabs(peak - 1.0) < 0.001);
    /* silence: no loudness */
    CHECK(isinf(measure(48000, 1000, -200, 5, 1, NULL)));
    /* the relative gate: 10 s at -23 then 10 s at -60 (gated away) reads about -23 */
    loudness_t *l = loudness_new(48000, 2);
    int32_t buf[2 * 4800];
    for (int part = 0; part < 2; part++) {
        double amp = pow(10.0, (part ? -60.0 : -23.0) / 20.0) * 2147483647.0;
        for (int b = 0; b < 100; b++) {
            for (int i = 0; i < 4800; i++) {
                int32_t v = (int32_t)lrint(amp * sin(2 * M_PI * 1000.0 * (b * 4800 + i) / 48000.0));
                buf[2 * i] = buf[2 * i + 1] = v;
            }
            loudness_add(l, buf, 4800);
        }
    }
    double g = loudness_lufs(l);
    printf("gated: %.2f LUFS\n", g);
    CHECK(fabs(g + 23.0) < 0.2);
    loudness_free(l);
    if (failures) { fprintf(stderr, "loudness: %d failures\n", failures); return 1; }
    printf("loudness: all checks passed\n");
    return 0;
}
