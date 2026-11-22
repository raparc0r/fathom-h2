/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* Faster without the pitch going up (audio/tempo.h): length, pitch, smoothness, and its cost. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "audio/tempo.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define RATE 44100

/* A tone through it in uneven chunks: the frames out */
static long run(int ch, int speed, double hz, long frames, int32_t **out_p)
{
    int32_t *in = malloc((size_t)frames * ch * sizeof *in);
    for (long i = 0; i < frames; i++)
        for (int c = 0; c < ch; c++) in[i * ch + c] = (int32_t)(0.5 * 2147483647.0 * sin(2 * M_PI * hz * i / RATE + c));
    long cap = frames * 100 / speed + 4 * TEMPO_MAX_SEQ;
    int32_t *out = malloc((size_t)cap * ch * sizeof *out);
    tempo_t *t = tempo_new(RATE, ch, speed);
    long w = 0, at = 0;
    unsigned r = 12345;
    while (at < frames) {
        r = r * 1103515245 + 12345;
        long n = 1 + (long)(r >> 16) % 3000;
        if (n > frames - at) n = frames - at;
        w += tempo_run(t, in + at * ch, n, out + w * ch, cap - w);
        at += n;
    }
    tempo_free(t);
    free(in);
    *out_p = out;
    return w;
}

static void tone(int ch, int speed)
{
    long frames = 3 * RATE;
    int32_t *out;
    long w = run(ch, speed, 440.0, frames, &out);
    double want = (double)frames * 100 / speed;
    CHECK(fabs(w - want) < want * 0.02 + 3000, "%d ch %d%%: %ld frames out, ~%.0f wanted", ch, speed, w, want);
    /* the pitch: rising zero crossings per second, after the first piece */
    long up = 0, from = RATE / 10;
    for (long i = from + 1; i < w; i++) up += out[(i - 1) * ch] < 0 && out[i * ch] >= 0;
    double hz = up * (double)RATE / (double)(w - from - 1);
    CHECK(fabs(hz - 440) < 440 * 0.02, "%d ch %d%%: %.1f Hz", ch, speed, hz);
    /* no clicks: no step much bigger than the tone's own */
    double amp = 0.5 * 2147483647.0, step = 2 * M_PI * 440 / RATE * amp, worst = 0;
    for (long i = 1; i < w; i++) {
        double d = fabs((double)out[i * ch] - (double)out[(i - 1) * ch]);
        if (d > worst) worst = d;
    }
    CHECK(worst < 2.5 * step, "%d ch %d%%: a jump of %.2f x the tone's step", ch, speed, worst / step);
    printf("%d ch %3d%%: %ld frames (%.0f wanted), %.1f Hz, worst step %.2f x\n", ch, speed, w, want, hz, worst / step);
    free(out);
}

/* An episode at 352.8 or 384 kHz (a DXD file): the piece is capped at
 * TEMPO_MAX_SEQ, so its fade and search must shrink with it */
static void high_rate(unsigned rate)
{
    enum { CHUNK = 1152 };
    static int32_t in[CHUNK * 2], out[(CHUNK * 2 + 4 * TEMPO_MAX_SEQ) * 2];
    tempo_t *t = tempo_new(rate, 2, 150);
    CHECK(t != NULL, "%u Hz: no tempo", rate);
    if (!t) return;
    long w = 0, frames = 2L * rate;
    for (long at = 0; at < frames; at += CHUNK) {
        for (long i = 0; i < CHUNK; i++)            /* a full-scale square: the match's sums at their biggest */
            in[2 * i] = in[2 * i + 1] = ((at + i) % 800) < 400 ? INT32_MAX : INT32_MIN;
        w += tempo_run(t, in, CHUNK, out, CHUNK * 2 + 4 * TEMPO_MAX_SEQ);
    }
    tempo_free(t);
    CHECK(labs(w - frames * 100 / 150) < 3 * TEMPO_MAX_SEQ, "%u Hz: %ld frames out, ~%ld wanted", rate, w, frames * 100 / 150);
    printf("%u Hz at 150%%: %ld frames\n", rate, w);
}

int main(void)
{
    high_rate(192000);
    high_rate(352800);
    high_rate(384000);
    for (int ch = 1; ch <= 2; ch++) {
        tone(ch, 125);
        tone(ch, 150);
        tone(ch, 200);
    }
    tone(2, 100);
    /* its cost: a minute of stereo at 1.5x, in the engine's chunks (speech-like
     * input made chunk by chunk, not timed; nothing kept: the H2 has 24 MB) */
    enum { CHUNK = 1152 };
    static int32_t in[CHUNK * 2], out[(CHUNK + 4 * TEMPO_MAX_SEQ) * 2];
    unsigned r = 1;
    int32_t lp = 0;
    tempo_t *t = tempo_new(RATE, 2, 150);
    double s = 0;
    long w = 0, frames = 60L * RATE;
    for (long at = 0; at < frames; at += CHUNK) {
        for (long i = 0; i < CHUNK; i++) {          /* filtered noise and a 200 Hz buzz, in syllables */
            r = r * 1103515245 + 12345;
            int32_t noise = (int32_t)(r >> 1) - (1 << 30);
            lp += (noise >> 3) - (lp >> 3);
            long k = at + i;
            int32_t buzz = ((k % 220) < 110 ? 1 : -1) * (1 << 28);
            in[2 * i] = in[2 * i + 1] = (k / 8820) % 2 ? (lp >> 1) + buzz : (lp >> 4);
        }
        clock_t c0 = clock();
        w += tempo_run(t, in, CHUNK, out, CHUNK + 4 * TEMPO_MAX_SEQ);
        s += (double)(clock() - c0) / CLOCKS_PER_SEC;
    }
    tempo_free(t);
    CHECK(labs(w - frames * 100 / 150) < 3 * TEMPO_MAX_SEQ, "speech-like: %ld frames out", w);
    printf("cost: %.2f s for 60 s of stereo at 1.5x: %.1f %% of real time\n", s, s / 60 * 100);
    if (fails) { printf("%d failed\n", fails); return 1; }
    printf("tempo: ok\n");
    return 0;
}
