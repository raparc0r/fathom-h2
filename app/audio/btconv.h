/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Bluetooth output format: 16-bit stereo at the link's rate (44.1 or 48 kHz,
 * fixed while the headphones are connected: changing it mid-link drops many
 * of them). From the track's own format, in up to three steps:
 *   1. the track's rate family (44.1/48/32 kHz): whole-ratio decimation (2, 4)
 *      through a windowed-sinc low-pass, flat to 20 kHz;
 *   2. if the family isn't the link's: a polyphase rational resampler
 *      (48 -> 44.1 kHz is 147/160), flat to 20 kHz, nothing folding below it;
 *   3. (optionally) the EQ, at the link's rate: a quarter of the work it is
 *      at 192 kHz;
 *   4. TPDF dither to 16 bits. */
#ifndef FATHOM_BTCONV_H
#define FATHOM_BTCONV_H

#include <stdint.h>

#include "audio/eq.h"

typedef struct btconv btconv_t;

/* A track rate's family: 44100, 48000 or 32000; 0 if no whole ratio gets there. */
unsigned btconv_rate_for(unsigned in_rate);

/* in_rate must have a family; link_rate 32000, 44100 or 48000; channels 1 or 2. */
btconv_t *btconv_create(unsigned in_rate, int channels, unsigned link_rate);
void btconv_destroy(btconv_t *c);
unsigned btconv_out_rate(const btconv_t *c);
double btconv_ratio(const btconv_t *c);      /* input frames per output frame */
void btconv_reset(btconv_t *c);              /* forget the filters' history (seek, flush) */
/* The EQ to run at the link's rate before the dither (built for btconv_out_rate()
 * and 2 channels; not owned; NULL = none). */
void btconv_set_eq(btconv_t *c, eq_t *eq);

/* Left-justified int32 frames in; interleaved stereo S16 out. out must hold
 * btconv_max_out(c, n) frames. Returns the frames written. */
long btconv_max_out(const btconv_t *c, long n);
long btconv_run(btconv_t *c, const int32_t *in, long n, int16_t *out);

#endif
