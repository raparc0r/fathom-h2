/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* The engine's one output, routed to the ALSA output (the H2's DAC, a USB DAC)
 * or to Bluetooth. Both change functions are for engine_switch_output(). */
#ifndef FATHOM_OUT_ROUTER_H
#define FATHOM_OUT_ROUTER_H

#include "audio/output.h"

void out_router_create(audio_output_t *out, const audio_output_t *alsa, const audio_output_t *bt);
void out_router_alsa(void *ctx, void *device);   /* ALSA, to this device (path) */
void out_router_bt(void *ctx, void *pcm);        /* Bluetooth, to this PCM (NULL: back to ALSA) */
int  out_router_on_bt(void);

#endif
