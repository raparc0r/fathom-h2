/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The network drive for the player (see share.c). All of it is safe from the UI thread. */
#ifndef FATHOM_SHARE_H
#define FATHOM_SHARE_H

#include <stddef.h>
#include "ui/ui.h"

int  share_available(void);                 /* fathom-share and its programs are there */
/* music: the folder served. on: wanted (it starts once Wi-Fi is connected). */
void share_init(const char *music, int on, const char *password);
void share_set_wifi(int connected);         /* follows Wi-Fi: called often */
void share_set(int on, const char *password);   /* wanted or not; a new password restarts it */

/* For ui_hooks_t (the platform's hook adds the settings and the password) */
int  share_ui_state(ui_share_t *st);        /* 1: wanted */

unsigned share_seq(void);                   /* changes when the state does */
/* Music was added, moved or deleted over the network and it's been quiet for
 * a while since: once, then 0 until the next change. */
int  share_take_changes(void);
/* Files went to or came from the H2 in the last 2 minutes (keeps it awake). */
int  share_busy(void);
/* The player is exiting: stop it now (it has the card open). */
void share_shutdown(void);

#endif
