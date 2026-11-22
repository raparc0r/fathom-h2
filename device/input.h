/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* H2 buttons (gpio-keys) and scroll wheel (rotary-encoder) through evdev,
 * and Bluetooth headphones' buttons (AVRCP). */
#ifndef FATHOM_INPUT_H
#define FATHOM_INPUT_H

#include <poll.h>
#include <stdint.h>

#define INPUT_MAX_FDS 6   /* keys, wheel, Bluetooth headphones' buttons, and spares */
/* what input_pollfds() can return: the devices and the watch on /dev/input */
#define INPUT_MAX_POLL (INPUT_MAX_FDS + 1)

typedef enum { IN_NONE, IN_UI, IN_POWER, IN_POWER_OFF, IN_VOLUME, IN_JACKS, IN_MEDIA } input_kind_t;
/* IN_MEDIA (the headphones' buttons): */
enum { IN_MEDIA_TOGGLE, IN_MEDIA_PLAY, IN_MEDIA_PAUSE, IN_MEDIA_NEXT, IN_MEDIA_PREV };

typedef struct {
    input_kind_t kind;
    int          ui_key;    /* ui_key_t when kind == IN_UI; +1/-1 for IN_VOLUME; IN_MEDIA_* for IN_MEDIA */
    int          hp, lo;    /* IN_JACKS: headphones / line-out device present */
} input_event_t;

/* Opens every /dev/input/event* device. Returns the number opened. */
int  input_open(void);
/* Current jack-detect switches (headphones, line out). */
void input_jacks(int *hp, int *lo);
/* Fills pollfds for the main loop. Returns the count. */
int  input_pollfds(struct pollfd *fds, int max);
/* Reads pending events and timers. Call after poll() and on every loop.
 * Returns 1 and fills *ev while there are events to deliver. */
int  input_next(input_event_t *ev, uint32_t now_ms);
/* Milliseconds until a pending long-press or key repeat fires (or -1). */
int  input_timeout(uint32_t now_ms);

#endif
