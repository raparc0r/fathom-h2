/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Playing faster without the pitch going up (podcasts at 1.25x, 1.5x...):
 * WSOLA, as SoundTouch's TDStretch does it. The sound is cut into 40 ms
 * pieces taken further apart than they're put back together; each piece is
 * moved (within 15 ms) to where it best matches the end of the last one, and
 * the two are cross-faded over 8 ms. Speech, where it's used, comes through
 * clean; music less so. Whole numbers only (the H2 has no FPU): the match is
 * found on a mono copy at a quarter of the rate.
 *
 * Samples are the engine's: interleaved, left-justified int32.
 */
#ifndef FATHOM_TEMPO_H
#define FATHOM_TEMPO_H

#include <stdint.h>

typedef struct tempo tempo_t;

/* For `channels` (1 or 2) at `rate`; speed in percent (100: unchanged). NULL: no memory. */
tempo_t *tempo_new(unsigned rate, int channels, int speed_pct);
void     tempo_free(tempo_t *t);
void     tempo_set_speed(tempo_t *t, int speed_pct);
/* Forget what's buffered (a seek, a new track). */
void     tempo_reset(tempo_t *t);
/* Take n frames of `in`; put what's ready into out (at most cap frames: enough
 * is n * 100 / speed + 2 * TEMPO_MAX_SEQ). Returns the frames written. */
#define TEMPO_MAX_SEQ 4096
long     tempo_run(tempo_t *t, const int32_t *in, long n, int32_t *out, long cap);
/* Frames taken in but not out yet (in input frames: for the position heard). */
long     tempo_latency(const tempo_t *t);

#endif
