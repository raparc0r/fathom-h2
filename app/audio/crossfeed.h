/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/*
 * Crossfeed for headphones: each ear also hears the other channel, low-passed
 * (the head shading what reaches the far ear), as it would from speakers, so
 * hard-panned recordings are less tiring on headphones.
 *
 * Bauer's stereophonic-to-binaural filter, as bs2b popularised it: the other
 * channel through a first-order low-pass, this channel through a first-order
 * high shelf that takes the low end down by as much as the low-pass adds,
 * and the sum scaled so a mono signal comes out at its own level in the bass.
 * Its levels: the low-frequency separation of each preset, in dB (more is
 * less crossfeed), at its cut-off.
 *
 *   Light   650 Hz, 9.5 dB (Jan Meier's)
 *   Medium  700 Hz, 6.0 dB (Chu Moy's)
 *   Strong  700 Hz, 4.5 dB (bs2b's default)
 *
 * Designed in doubles when a track opens; run in whole numbers on the
 * engine's left-justified int32 samples (coefficients Q30, sums 64-bit,
 * results saturated), stereo only.
 */
#ifndef FATHOM_CROSSFEED_H
#define FATHOM_CROSSFEED_H

#include <stdint.h>

enum { XF_OFF, XF_LIGHT, XF_MEDIUM, XF_STRONG, XF_LEVELS };

typedef struct crossfeed crossfeed_t;

/* For a stereo signal at `rate`; NULL for XF_OFF (or no memory). */
crossfeed_t *crossfeed_new(int level, unsigned rate);
void         crossfeed_free(crossfeed_t *x);
/* Forget the past (a seek, a new track): nothing carries over. */
void         crossfeed_reset(crossfeed_t *x);
/* n interleaved stereo frames, in place. */
void         crossfeed_run(crossfeed_t *x, int32_t *buf, long n);
int          crossfeed_level(const crossfeed_t *x);
unsigned     crossfeed_rate(const crossfeed_t *x);
/* The level's name ("Off", "Light", "Medium", "Strong") and its design. */
const char  *crossfeed_name(int level);
void         crossfeed_design(int level, int *cut_hz, double *feed_db);

#endif
