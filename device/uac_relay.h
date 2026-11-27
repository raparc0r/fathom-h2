/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* USB DAC mode: the computer's audio (USB Audio Class 2 gadget) to the H2's DAC. */
#ifndef FATHOM_UAC_RELAY_H
#define FATHOM_UAC_RELAY_H

typedef struct uac_relay uac_relay_t;

typedef struct {
    unsigned rate;               /* what the computer sends, 0 = nothing */
    int      playing;            /* audio arrived in the last half second */
    unsigned long long frames;   /* copied to the DAC since the start */
    unsigned overruns;           /* audio lost because the relay fell behind */
    int      bits;               /* bits the computer's samples use: 16, 24 or 32; 0 = not known yet */
} uac_relay_status_t;

/* Starts copying on a thread; dac is the DAC's PCM device (the player has let go of it). */
uac_relay_t *uac_relay_start(const char *dac);
void uac_relay_status(uac_relay_t *r, uac_relay_status_t *st);
void uac_relay_stop(uac_relay_t *r);     /* closes both sides, frees r */

#endif
