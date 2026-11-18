/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Updates over Wi-Fi for the player (see updater.c and app/net/update.h).
 * Only ever when asked (Settings > About > Check for updates): nothing here
 * runs on its own, at start or later. All of it is safe from the UI thread.
 */
#ifndef FATHOM_UPDATER_H
#define FATHOM_UPDATER_H

#include "ui/ui.h"

/* card: the card's top folder; version: the running one (FATHOM_VERSION);
 * api: where to ask (UPDATE_API, or a test server's). */
void updater_init(const char *card, const char *version, const char *api);

/* For ui_hooks_t */
void updater_ui_state(ui_update_t *st);
void updater_do(int what);              /* UI_UPD_CHECK, _START, _CANCEL, _INSTALL */
void updater_refuse(const char *why);   /* a check that can't happen now (no Wi-Fi): why */

unsigned updater_seq(void);             /* changes when the state or progress does */
int  updater_busy(void);                /* downloading, unpacking or installing: stay awake */
int  updater_installed(void);           /* installed: restart the H2 now */

#endif
