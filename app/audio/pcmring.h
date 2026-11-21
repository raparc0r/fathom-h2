/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * What an output last wrote, in its own sample format, and a fade-out over it:
 * for a flush without a click (out_alsa.c's flush_soft). Dropping what's
 * queued stops the sound mid-waveform, which clicks; instead the output takes
 * back all but a little of what it queued (the kernel's rewind) and writes the
 * next few milliseconds of it again, faded to silence, from this copy.
 */
#ifndef FATHOM_PCMRING_H
#define FATHOM_PCMRING_H

#include <stddef.h>
#include <stdint.h>

/* The wire formats out_alsa writes */
enum { PCMRING_S16, PCMRING_S24_4, PCMRING_S32, PCMRING_S24_3 };

typedef struct {
    uint8_t *buf;
    unsigned cap;            /* frames it holds */
    unsigned frame_bytes;
    uint64_t written;        /* frames put since the reset */
} pcmring_t;

/* cap frames of frame_bytes each; 0, or -1 (no memory). */
int  pcmring_init(pcmring_t *r, unsigned cap, unsigned frame_bytes);
void pcmring_free(pcmring_t *r);
void pcmring_reset(pcmring_t *r);
void pcmring_put(pcmring_t *r, const void *frames, unsigned n);
/* The n frames that start `back` frames before the end into out; -1 if the
 * ring no longer has them all (back > cap, or n > back). */
int  pcmring_get(const pcmring_t *r, unsigned back, unsigned n, void *out);
/* Taken back: the last `n` frames are as if never put (the kernel's rewind). */
void pcmring_unput(pcmring_t *r, unsigned n);

/* Fades n frames of ch channels in place, from full to silence (linear). */
void pcm_fade_out(void *frames, unsigned n, unsigned ch, int format);

#endif
