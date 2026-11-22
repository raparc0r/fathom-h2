/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/*
 * alsa-tone: play a 1 kHz sine through the player's ALSA output (out_alsa.c),
 * at a given rate, bit depth, level and length. Bring-up tool for the H2.
 *
 *   alsa-tone [RATE [BITS [DBFS [SECONDS [L|R|LR [SILENCE]]]]]]
 *        defaults: 48000 16 -20 3 LR 0; SILENCE = seconds of silence before closing
 *   ALSA_TONE_DC=1 in the environment offsets everything by -1 LSB (never exact zero)
 *   ALSA_TONE_DEV=/dev/snd/pcmC1D0p plays on another card (default card 0)
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "audio/output.h"

int main(int argc, char **argv)
{
    unsigned rate = argc > 1 ? (unsigned)atoi(argv[1]) : 48000;
    int bits = argc > 2 ? atoi(argv[2]) : 16;
    double dbfs = argc > 3 ? atof(argv[3]) : -20;
    double secs = argc > 4 ? atof(argv[4]) : 3;
    const char *ch = argc > 5 ? argv[5] : "LR";
    int left = ch[0] == 'L', right = ch[0] == 'R' || (ch[0] && ch[1] == 'R');
    double hold = argc > 6 ? atof(argv[6]) : 0;
    int32_t dc = getenv("ALSA_TONE_DC") ? -(int32_t)(1u << (32 - (bits <= 16 ? 16 : 24))) : 0;

    audio_output_t out;
    if (out_alsa_create(&out, getenv("ALSA_TONE_DEV"))) { fprintf(stderr, "no ALSA device\n"); return 1; }
    audio_format_t f = {.rate = rate, .channels = 2, .bits = (uint8_t)bits};
    if (out.open(out.ctx, &f)) return 1;
    char d[128];
    out.describe(out.ctx, d, sizeof d);
    printf("%s, 1 kHz at %.0f dBFS for %.1f s, channels %s%s\n", d, dbfs, secs, ch, dc ? ", DC -1 LSB" : "");

    /* The toolchain is soft-float: sin() per sample can't keep up at 192 kHz.
     * Compute a whole number of cycles once (rate / gcd(rate, 1000) samples)
     * and loop over it; the fades are integer gains. */
    double amp = pow(10, dbfs / 20) * 2147483647.0;
    unsigned g1 = rate, g2 = 1000;
    while (g2) { unsigned t = g1 % g2; g1 = g2; g2 = t; }
    unsigned tlen = rate / g1;
    int32_t *table = malloc(tlen * sizeof *table);
    if (!table) return 1;
    for (unsigned i = 0; i < tlen; i++) table[i] = (int32_t)(amp * sin(2 * M_PI * 1000.0 * i / rate));

    int32_t buf[2 * 512];
    unsigned long total = (unsigned long)(secs * rate), ph = 0, fade = rate / 50;   /* 20 ms fades */
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    while (ph < total) {
        for (int i = 0; i < 512; i++, ph++) {
            unsigned long g = ph >= total ? 0                           /* past the end of the last chunk */
                            : ph < fade ? ph
                            : total - ph < fade ? total - ph : fade;
            int32_t v = (int32_t)((int64_t)table[ph % tlen] * (int64_t)g / (int64_t)fade);
            buf[2 * i] = (left ? v : 0) + dc;
            buf[2 * i + 1] = (right ? v : 0) + dc;
        }
        if (out.write(out.ctx, buf, 512) < 0) return 1;
    }
    free(table);
    if (hold > 0) {
        for (int i = 0; i < 2 * 512; i++) buf[i] = dc;
        for (unsigned long n = 0; n < (unsigned long)(hold * rate); n += 512)
            if (out.write(out.ctx, buf, 512) < 0) return 1;
    }
    out.drain(out.ctx);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    printf("played in %.2f s\n", (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9);
    out.close(out.ctx);
    return 0;
}
