/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* Hardware volume through an ALSA integer mixer control (Linux only). */
#ifndef FATHOM_MIXER_ALSA_H
#define FATHOM_MIXER_ALSA_H

#include <stdio.h>

typedef struct {
    int  fd;
    char name[44];
    int  min, max, channels;
} mixer_t;

/* Opens `control` (e.g. "Master Playback Volume") on ctl_dev (NULL = card 0). */
int  mixer_open(mixer_t *m, const char *ctl_dev, const char *control);
int  mixer_get(mixer_t *m);              /* raw value of the first channel, or -1 */
int  mixer_set(mixer_t *m, int value);   /* all channels, clamped; returns the value set or -1 */
void mixer_close(mixer_t *m);

/* The control's dB range in 1/100 dB (from its TLV), or -1 if it has none. */
int  mixer_db_range(mixer_t *m, long *min_cdb, long *max_cdb);
/* The raw value closest to `cdb` (1/100 dB), assuming linear dB steps; -1 without a dB range. */
int  mixer_value_for_db(mixer_t *m, long cdb);
/* Name of the first integer "... Playback Volume" control on ctl_dev, into name (44 bytes). */
int  mixer_find_playback_volume(const char *ctl_dev, char *name);

/* Enumerated controls (e.g. the ES9018K2M's "DAC Filter"): the current item
 * and its name, the item count, and setting an item. -1 on error. */
int  mixer_enum_get(const char *ctl_dev, const char *control, char *name, int cap);
int  mixer_enum_count(const char *ctl_dev, const char *control);
int  mixer_enum_set(const char *ctl_dev, const char *control, int item);
/* Any boolean, integer or enumerated control, every channel to `value`. -1 on error. */
int  mixer_ctl_set(const char *ctl_dev, const char *control, int value);
/* Every control of the card, its type, range and value, one per line (diagnostics). */
void mixer_dump(const char *ctl_dev, FILE *out);

#endif
