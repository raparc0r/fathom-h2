/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/*
 * Radio stations' icons: the logo a station's line names (radio.txt's
 * "| icon: URL", player/radio.h), fetched once and kept small on the card, in
 * DIR (fathom/radio-icons):
 *
 *   HASH.565    the logo as RGB565: LIB_ART_SMALL square (the list's, 2,312
 *               bytes), then LIB_ART_LARGE square (Now Playing's, 36,992
 *               more). One under RICON_LARGE_MIN pixels on its short side
 *               has only the small part.
 *   HASH.none   empty: that address has no picture that can be used (the
 *               server says it isn't there, it's over RICON_MAX_FILE, or it
 *               isn't a PNG or a JPEG), so it isn't asked for again.
 *
 * HASH is 12 hex digits of the icon address's SHA-256 (pod_hash), so stations
 * that share a logo share its file. Only the stations in your list get one:
 * ricon_sync is given that list, and removes the files of stations no longer
 * in it. A logo that couldn't be reached leaves nothing, and is tried again
 * the next time.
 */
#ifndef FATHOM_RADIOICON_H
#define FATHOM_RADIOICON_H

#include <stddef.h>
#include <stdint.h>

#include "player/radio.h"

#define RICON_MAX_FILE (512 * 1024)   /* a logo's encoded bytes */
#define RICON_LARGE_MIN 68            /* its short side, to be shown at LIB_ART_LARGE */
#define RICON_SMALL_MIN 16            /* ...and to be shown at all */

/* What fetches: http_get (net/http.h) unless replaced (the tests and the
 * simulator read saved pictures). Same contract as http_get. */
typedef long (*ricon_fetch_fn)(const char *url, char *buf, size_t cap, const volatile int *abort);
void ricon_set_fetch(ricon_fetch_fn fn);

/* The icons are kept in dir (made when the first is written). 0. */
int  ricon_start(const char *dir);
/* A run under way gives up (its fetch dropped) and is waited for, a few seconds at most. */
void ricon_stop(void);

/* Your stations as they are now (st may be freed after): on a short-lived
 * thread of its own, the files of icons no station names any more are
 * removed, then, if net (the network is up), each station's icon that has no
 * file yet is fetched, one at a time. While a run is under way the newest list
 * given waits for it to end, then gets its own. n < 0 (no list): nothing. */
void ricon_sync(const radio_station_t *st, int n, int net);
int  ricon_busy(void);
/* Up each time an icon's file is written: the lists read theirs again. */
unsigned ricon_seq(void);

/* The 12 hex digits an icon address's files are named by ("" for none) */
void ricon_id(const char *icon_url, char id[13]);
/* Its picture file, there or not: 0, or -1 (no icon address, or not started) */
int  ricon_path(const char *icon_url, char *out, size_t cap);
/* An icon's pixels (size: LIB_ART_SMALL or LIB_ART_LARGE, size * size RGB565)
 * by its id: 1, or 0 if it has none of that size */
int  ricon_read(const char *id, int size, uint16_t *px);

#endif
