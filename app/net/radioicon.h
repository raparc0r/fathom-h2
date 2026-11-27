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
 *               isn't a PNG or a JPEG), so it isn't asked for again: for a
 *               week (what answered may have been a network's sign-in page,
 *               or a server saying "too many requests").
 *
 * A station whose line names no icon, or one that's no use, but names its own
 * site ("| site: URL") gets the icon that site's page names: the one phones
 * put on their home screens (apple-touch-icon, big enough for Now Playing)
 * or its favicon (.ico files are read too; small ones show in the list only).
 * Kept the same way, under the site's own HASH.
 *
 * A station that names neither (one added before sites were kept, say) is
 * looked up in the directory by its stream, once: the logo and the site the
 * directory has for it are tried the same way, under the stream's HASH. A
 * stream's address that looks private is never sent (ricon_stream_askable).
 *
 * HASH is 12 hex digits of the icon address's SHA-256 (pod_hash; a site's:
 * of "site:" and its address; a stream's: of "stream:" and its address), so
 * stations that share a logo share its file. Only the stations in your list get one:
 * ricon_sync is given that list, and removes the files of stations no longer
 * in it. A logo that couldn't be reached leaves nothing, and is tried again
 * the next time.
 */
#ifndef FATHOM_RADIOICON_H
#define FATHOM_RADIOICON_H

#include <stddef.h>
#include <stdint.h>

#include "net/http.h"

#include "player/radio.h"

#define RICON_MAX_FILE (512 * 1024)   /* a logo's encoded bytes (a PNG, a JPEG or an .ico) */
#define RICON_LARGE_MIN LIB_LOGO_SHARP_MIN   /* its short side, to fill Now Playing's square (a smaller one sits in its middle) */
#define RICON_SMALL_MIN 16            /* ...and to be shown at all */

/* What fetches: http_get (net/http.h) unless replaced (the tests and the
 * simulator read saved pictures). Same contract as http_get. */
void ricon_set_fetch(http_get_fn fn);

/* The icons are kept in dir (made when the first is written). 0. */
int  ricon_start(const char *dir);
/* A run under way gives up (its fetch dropped) and is waited for, a few seconds at most. */
void ricon_stop(void);
/* The same in two steps (as podsync_stop_ask and podsync_stop_wait): _ask
 * only tells the run, _wait waits for it, ms at most. 1: no run is left; 0:
 * one is, and ends with the player. */
void ricon_stop_ask(void);
int  ricon_stop_wait(int ms);

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
/* ...and a site's */
void ricon_site_id(const char *site_url, char id[13]);
/* ...and a stream's: what the directory led to, for a station that names
 * neither ("" for a stream that isn't asked about: ricon_stream_askable) */
void ricon_stream_id(const char *stream_url, char id[13]);
/* A stream's address that may be given to the directory (radio-browser.info)
 * to ask what it has for it: not one with a login or a query in it, nor one
 * on a home's own network (a name with no dot, .local and the like, the
 * private number ranges). */
int  ricon_stream_askable(const char *url);
/* The picture a station shows: its own logo's id, else its site's icon's,
 * else its stream's; a later one when that's the one on the card ("" if it
 * has none of them). */
void ricon_station_id(const radio_station_t *st, char id[13]);
/* Its picture file, there or not: 0, or -1 (no icon address, or not started) */
int  ricon_path(const char *icon_url, char *out, size_t cap);
/* An icon's pixels (size: LIB_ART_SMALL or LIB_ART_LARGE, size * size RGB565)
 * by its id: 1, or 0 if it has none of that size */
int  ricon_read(const char *id, int size, uint16_t *px);

/* An http(s) address whose host is out on the internet by its name or number
 * (not one of a home network's own: no dot, .local, fritz.box, the private
 * number ranges however written). What a station's page or the directory
 * names is fetched only from such a host: the H2 isn't sent to an address on
 * the home's network by a page. */
int  ricon_host_public(const char *url);

/* tests: the readers of what a server sends, each on its own (the fuzz test,
 * tests/fuzz_radioicon.c, gives them damaged ones) */
/* an .ico file's n bytes, decoded and thrown away: 0 and *side (its shorter
 * side, may be NULL), or -1 */
int  ricon_test_ico(const unsigned char *buf, size_t n, int *side);
/* how many icons a page (n bytes, fetched from base) names, their addresses
 * the biggest first, one a line, into urls (may be NULL) */
int  ricon_test_site(const char *html, size_t n, const char *base, char *urls, size_t cap);
/* href as the page at base names it: 0, or -1 */
int  ricon_test_join(const char *base, const char *href, char *out, size_t cap);
/* the servers not reached since the start: forgotten (a test's next part starts as a new session) */
void ricon_test_forget_unreached(void);

#endif
