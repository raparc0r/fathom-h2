/* SPDX-License-Identifier: GPL-3.0-or-later */
/* EBU R128 integrated loudness (app/audio/loudness.c) against reference signals. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "audio/loudness.h"

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

int main(void)
{
    double peak;
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
