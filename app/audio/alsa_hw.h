/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* The kernel's PCM parameters filled in by hand (no alsa-lib on the H2): the
 * few helpers both users of the raw interface need, the playback output
 * (out_alsa.c) and the USB DAC mode's capture (device/uac_relay.c). */
#ifndef FATHOM_ALSA_HW_H
#define FATHOM_ALSA_HW_H

#include <limits.h>
#include <string.h>

#include <sound/asound.h>

static inline struct snd_mask *hp_mask(struct snd_pcm_hw_params *p, int n)
{
    return &p->masks[n - SNDRV_PCM_HW_PARAM_FIRST_MASK];
}

static inline struct snd_interval *hp_int(struct snd_pcm_hw_params *p, int n)
{
    return &p->intervals[n - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
}

/* Everything allowed: the start of a request */
static inline void hp_any(struct snd_pcm_hw_params *p)
{
    memset(p, 0, sizeof *p);
    for (int n = SNDRV_PCM_HW_PARAM_FIRST_MASK; n <= SNDRV_PCM_HW_PARAM_LAST_MASK; n++)
        memset(hp_mask(p, n)->bits, 0xff, sizeof hp_mask(p, n)->bits);
    for (int n = SNDRV_PCM_HW_PARAM_FIRST_INTERVAL; n <= SNDRV_PCM_HW_PARAM_LAST_INTERVAL; n++) {
        hp_int(p, n)->min = 0;
        hp_int(p, n)->max = UINT_MAX;
    }
    p->rmask = ~0u;
    p->info = ~0u;
}

static inline void hp_mask_set(struct snd_pcm_hw_params *p, int n, unsigned v)
{
    struct snd_mask *m = hp_mask(p, n);
    memset(m->bits, 0, sizeof m->bits);
    m->bits[v >> 5] |= 1u << (v & 31);
}

static inline void hp_int_set(struct snd_pcm_hw_params *p, int n, unsigned v)
{
    struct snd_interval *i = hp_int(p, n);
    i->min = i->max = v;
    i->integer = 1;
}

/* The software parameters' boundary for a buffer of `buffer` frames: the
 * largest multiple by a power of two that the kernel accepts */
static inline unsigned long alsa_boundary(unsigned long buffer)
{
    unsigned long boundary = buffer;
    while (boundary * 2 <= LONG_MAX - buffer) boundary *= 2;
    return boundary;
}

#endif
