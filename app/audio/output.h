/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Audio output backends: ALSA hw: on the H2, SDL on the desktop, null for tests. */
#ifndef FATHOM_OUTPUT_H
#define FATHOM_OUTPUT_H

#include "audio/decoder.h"
#include "audio/eq.h"

typedef struct {
    /* Prepare for a stream format. Backends that can play it exactly must.
     * 0; -1: not this format (the track can't be played here); -2: the
     * device isn't there (unplugged, the link down: playback pauses). */
    int  (*open)(void *ctx, const audio_format_t *fmt);
    /* Left-justified int32 frames. Blocks until accepted. Returns frames or -1. */
    long (*write)(void *ctx, const int32_t *frames, long n);
    long (*delay)(void *ctx);           /* frames written but not yet heard */
    void (*drain)(void *ctx);           /* play out everything written */
    void (*flush)(void *ctx);           /* drop everything written (seek, stop) */
    void (*pause)(void *ctx, int on);
    void (*close)(void *ctx);
    /* What the hardware actually runs at, for the signal-path screen. */
    void (*describe)(void *ctx, char *out, int cap);
    void *ctx;
    /* Optional: an output that runs the EQ itself, where it's cheaper (Bluetooth:
     * at the link's rate), takes the profile (NULL = off) and returns 1; the
     * engine then doesn't. NULL or 0: the engine runs it. */
    int  (*set_eq)(void *ctx, const eq_profile_t *p);
} audio_output_t;

int  out_sdl_create(audio_output_t *out);
void out_sdl_destroy(audio_output_t *out);

/* ALSA PCM device (Linux; e.g. "/dev/snd/pcmC0D0p"), bit-perfect, kernel ioctls only. */
int  out_alsa_create(audio_output_t *out, const char *device);
/* For engine_switch_output(): arg is the new device path (output closed). */
void out_alsa_set_device(void *ctx, void *device);
/* Bit-perfect checks (tools/bitperfect): append every byte written to the card,
 * in the card's own format, to f (NULL: stop). */
#include <stdio.h>
void out_alsa_set_tap(void *ctx, FILE *f);

/* Real-time pacing without sound (the H2 before its sound card worked). */
void out_clock_create(audio_output_t *out);

/* Accepts everything instantly and counts it (tests). */
typedef struct { unsigned long long frames; audio_format_t fmt; int opens; } out_null_stats_t;
void out_null_create(audio_output_t *out, out_null_stats_t *stats);

#endif
