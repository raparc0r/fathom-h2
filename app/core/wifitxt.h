/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
#ifndef FATHOM_WIFITXT_H
#define FATHOM_WIFITXT_H

#include <stddef.h>

/* fathom/wifi.txt: Wi-Fi networks typed on a computer rather than with the
 * wheel, read (and then deleted) at the next start. A "network =" line, then
 * its "password =" (none: an open network); as many as wanted; # comments.
 * Spaces around the = and the ends are dropped; Windows line ends, a BOM and
 * any case of the words are fine. */
#define WIFITXT_MAX 8
typedef struct {
    char ssid[33];       /* (up to 32 bytes, as Wi-Fi allows) */
    char pass[64];       /* "" open; else 8 to 63 characters */
} wifitxt_net_t;

/* The networks in text[0..n): how many (up to max); *bad: lines it couldn't
 * use (a name too long, a password too short or long, a password with no
 * network before it). */
int wifitxt_parse(const char *text, size_t n, wifitxt_net_t *out, int max, int *bad);

#endif
