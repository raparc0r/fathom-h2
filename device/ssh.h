/* SPDX-License-Identifier: GPL-3.0-or-later */
/* SSH for the player (see ssh.c). All of it is safe from the UI thread. */
#ifndef FATHOM_SSH_H
#define FATHOM_SSH_H

#include "ui/ui.h"

int  ssh_available(void);                      /* fathom-ssh and dropbear are there */
/* At start: root's password is set, and SSH started if on. */
void ssh_init(int on, const char *password);
void ssh_set(int on, const char *password);    /* on or off; a new password applies at once */
int  ssh_ui_state(ui_ssh_t *st);               /* for ui_hooks_t: 1 when on */
unsigned ssh_seq(void);                        /* changes when the state does */

#endif
