/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Bluetooth for the player (see bt.c). All of it is safe from the UI thread. */
#ifndef FATHOM_BT_H
#define FATHOM_BT_H

#include <stddef.h>
#include "ui/ui.h"

int  bt_available(void);                      /* the stack is on the card */
void bt_init(int on, const char *last);       /* on: start now; last: headphones to reconnect */
void bt_request(int cmd, const char *addr);

/* For ui_hooks_t */
int  bt_ui_state(char *busy, size_t cap);
int  bt_ui_devices(ui_bt_dev_t *out, int max);
void bt_ui_do(int what, const char *addr);

unsigned bt_seq(void);                        /* changes when anything below does */
int  bt_on(void);
void bt_set(int on);                          /* start or stop the stack (the setting stays as it is) */
/* The connected device's playback PCM (and its name); 0 if none. */
int  bt_pcm(char *pcm, size_t cap, char *name, size_t ncap);
int  bt_last(char *addr, size_t cap);         /* the last device that connected */
int  bt_take_message(char *out, size_t cap);  /* a message for a toast, once */
int  bt_volume_step(int dir);                 /* the link's volume, 0..127; returns the new one */
void bt_volume_limit(int max);                /* no higher than max (0..127); lowers it now if above */

#endif
