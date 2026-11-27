/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* A flush without a click (audio/pcmring.h): the copy of what was written,
 * taken back as the kernel's rewind takes it, and the fade-out in each wire
 * format. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio/pcmring.h"
#include "test_util.h"

int main(void)
{
    /* frames of one int32 each, numbered: what's put comes back where it was */
    pcmring_t r;
    CHECKF(pcmring_init(&r, 10, 4) == 0, "init");
    int32_t in[25], out[10];
    for (int i = 0; i < 25; i++) in[i] = i;
    pcmring_put(&r, in, 7);
    CHECKF(pcmring_get(&r, 7, 3, out) == 0 && out[0] == 0 && out[2] == 2, "the start");
    pcmring_put(&r, in + 7, 6);                       /* 13 in all: it wraps, 3..12 kept */
    CHECKF(pcmring_get(&r, 10, 10, out) == 0 && out[0] == 3 && out[9] == 12, "wrapped: %d..%d", out[0], out[9]);
    CHECKF(pcmring_get(&r, 11, 1, out) == -1, "not kept any more");
    CHECKF(pcmring_get(&r, 2, 3, out) == -1, "more than back");
    /* taken back 4 (the kernel's rewind): 8 is next, then written again over */
    pcmring_unput(&r, 4);
    CHECKF(pcmring_get(&r, 1, 1, out) == 0 && out[0] == 8, "after the rewind: %d", out[0]);
    pcmring_put(&r, in + 20, 2);
    CHECKF(pcmring_get(&r, 3, 3, out) == 0 && out[0] == 8 && out[1] == 20 && out[2] == 21, "written over");
    pcmring_put(&r, in, 25);                          /* more than it holds at once: the last 10 */
    CHECKF(pcmring_get(&r, 10, 10, out) == 0 && out[0] == 15 && out[9] == 24, "a big put");
    pcmring_reset(&r);
    CHECKF(pcmring_get(&r, 1, 1, out) == -1, "reset: nothing");
    pcmring_free(&r);
    CHECKF(pcmring_init(&r, 0, 4) == -1 && pcmring_get(&r, 1, 1, out) == -1, "no ring");

    /* the fade, each format: full at the start, silence at the end, falling,
     * the sign kept; stereo, both channels alike */
    enum { N = 64 };
    int16_t s16[2 * N];
    for (int i = 0; i < 2 * N; i++) s16[i] = i % 2 ? -30000 : 30000;
    pcm_fade_out(s16, N, 2, PCMRING_S16);
    CHECKF(s16[0] == 30000 && s16[1] == -30000, "s16 starts full: %d %d", s16[0], s16[1]);
    CHECKF(abs(s16[2 * N - 2]) < 30000 / N + 1 && abs(s16[2 * N - 1]) < 30000 / N + 1, "s16 ends silent: %d", s16[2 * N - 2]);
    int falls = 1;
    for (int i = 1; i < N; i++) falls &= s16[2 * i] <= s16[2 * i - 2] && s16[2 * i] >= 0 && s16[2 * i + 1] == -s16[2 * i];
    CHECKF(falls, "s16 falls, channels alike");
    int32_t s24[N], s32[N];
    for (int i = 0; i < N; i++) { s24[i] = -8000000; s32[i] = 2000000000; }
    pcm_fade_out(s24, N, 1, PCMRING_S24_4);
    pcm_fade_out(s32, N, 1, PCMRING_S32);
    CHECKF(s24[0] == -8000000 && s24[N - 1] > -8000000 / N - 1 && s24[N - 1] <= 0, "s24 in 4 bytes: %d", s24[N - 1]);
    CHECKF(s32[0] == 2000000000 && s32[N - 1] < 2000000000 / N + 1 && s32[N - 1] >= 0, "s32: %d", s32[N - 1]);
    uint8_t s3[3 * N];
    for (int i = 0; i < N; i++) { s3[3 * i] = 0x00; s3[3 * i + 1] = 0x00; s3[3 * i + 2] = 0x90; }   /* -7340032 */
    pcm_fade_out(s3, N, 1, PCMRING_S24_3);
    int32_t first = s3[0] | (s3[1] << 8) | (s3[2] << 16), last = s3[3 * N - 3] | (s3[3 * N - 2] << 8) | (s3[3 * N - 1] << 16);
    if (first & 0x800000) first -= 0x1000000;
    if (last & 0x800000) last -= 0x1000000;
    CHECKF(first == -7340032 && last <= 0 && last > -7340032 / N - 1, "s24 in 3 bytes: %d .. %d", first, last);
    pcm_fade_out(s16, 0, 2, PCMRING_S16);             /* (nothing: nothing done) */

    return t_result();
}
