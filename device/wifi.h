/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* Wi-Fi for the player (see wifi.c). All of it is safe from the UI thread. */
#ifndef FATHOM_WIFI_H
#define FATHOM_WIFI_H

#include <stddef.h>
#include "ui/ui.h"

int  wifi_available(void);                    /* fathom-wifi, its programs and the firmware are there */
void wifi_init(int on);                       /* on: start now */

/* For ui_hooks_t */
int  wifi_ui_state(char *busy, size_t cap, ui_wifi_status_t *st);
int  wifi_ui_networks(ui_wifi_net_t *out, int max);
void wifi_ui_do(int what, const char *ssid, const char *password);

unsigned wifi_seq(void);                      /* changes when anything above does */
int  wifi_on(void);
int  wifi_connected(void);                    /* joined, with an address */
int  wifi_take_message(char *out, size_t cap);
void wifi_wake(void);                         /* the screen turned on: nothing joined, look for networks now */
/* The screen went on or off (on until told): off, the signal's strength isn't
 * asked for while connected (nobody sees it; each asking wakes the chip) */
void wifi_screen(int on);
/* fathom/wifi.txt (core/wifitxt.h): its networks joined and saved as if typed,
 * once Wi-Fi is on (the caller turns it on), and the file deleted */
void wifi_import(const char *path);

#endif
