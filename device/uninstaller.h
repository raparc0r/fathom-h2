/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/*
 * Uninstall Fathom for the player (see uninstaller.c and app/sys/uninstall.h).
 * All of it is safe from the UI thread.
 */
#ifndef FATHOM_UNINSTALLER_H
#define FATHOM_UNINSTALLER_H

#include "ui/ui.h"

/* card: the card's top folder. */
void uninstaller_init(const char *card);

/* For ui_hooks_t (UI_UNI_RESTART is the player's own: not here) */
void uninstaller_ui_state(ui_uninstall_t *st);
void uninstaller_do(int what);          /* UI_UNI_CHECK, _APPLY, _CANCEL */

unsigned uninstaller_seq(void);         /* changes when the state or progress does */
int  uninstaller_busy(void);            /* reading the file: stay awake */
int  uninstaller_pending(void);         /* made ready and waiting for the restart */

/* The next start goes to HiBy's recovery: 0, or -1 if the signal couldn't be
 * set (the H2 then has to be taken there by hand: Vol+ at power-on, Boot
 * select > HibyOS Recovery). */
int  uninstaller_ask_recovery(void);

#endif
