/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* The integer gain: against double precision, saturation, unity. */
#include "audio/gain.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

int main(void)
{
    enum { N = 100000 };
    static int32_t s[N], o[N];
    srand(7);
    for (int i = 0; i < N; i++) o[i] = (int32_t)(((uint32_t)rand() << 17) ^ ((uint32_t)rand() << 2) ^ (uint32_t)rand());
    const double gains[] = {0.5, 0.0316227766, 0.891250938, 1.0, 1.258925412, 3.16227766};   /* -30 .. +10 dB */
    for (size_t g = 0; g < sizeof gains / sizeof gains[0]; g++) {
        for (int i = 0; i < N; i++) s[i] = o[i];
        gain_run(s, N, gain_q28(gains[g]));
        double worst = 0;
        long clipped = 0;
        for (int i = 0; i < N; i++) {
            double want = o[i] * gains[g];
            if (want >= 2147483647.0 || want <= -2147483648.0) {
                clipped++;
                CHECK(s[i] == (want > 0 ? INT32_MAX : INT32_MIN), "gain %.3f: %d clipped to %d", gains[g], o[i], s[i]);
                continue;
            }
            /* the product's rounding (half a step), plus the gain's own: Q28 holds it to
             * 2^-29, which moves a sample by up to |input| * 2^-29 (a level change of
             * at most 0.0000005 dB at -30 dB) */
            double err = fabs(s[i] - want), allow = 0.5 + fabs((double)o[i]) * ldexp(1.0, -29) + 1e-6;
            if (err > worst) worst = err;
            CHECK(err <= allow, "gain %.6f: %d -> %d, want %.1f", gains[g], o[i], s[i], want);
        }
        printf("gain %8.5f: worst error %.2f of a 32-bit step, %ld clipped\n", gains[g], worst, clipped);
    }
    for (int i = 0; i < N; i++) s[i] = o[i];
    gain_run(s, N, gain_q28(1.0));
    int same = 1;
    for (int i = 0; i < N; i++) same &= s[i] == o[i];
    CHECK(same, "unity isn't bit for bit");

    /* the balance: each channel its own gain, the left untouched */
    for (int i = 0; i < N; i++) s[i] = o[i];
    gain_run2(s, N / 2, gain_q28(1.0), gain_q28(0.5));
    int left = 1, right = 1;
    for (int i = 0; i + 1 < N; i += 2) {
        left &= s[i] == o[i];
        right &= fabs(s[i + 1] - o[i + 1] * 0.5) <= 0.5 + 1e-6;
    }
    CHECK(left && right, "balance: left %d, right %d", left, right);
    printf(fails ? "%d failures\n" : "gain ok\n", fails);
    return fails != 0;
}
