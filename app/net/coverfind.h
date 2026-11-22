/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/*
 * Covers for albums that have none, looked up online: MusicBrainz first (its
 * album, then that album's front cover from the Cover Art Archive), then
 * Apple's music search. Only when asked:
 *
 *   - one album (its context menu: Find cover): every likely match, best
 *     first, each one's picture fetched as it's shown; the one kept is saved;
 *   - every album without one (Settings > Library > Find missing covers),
 *     once: only sure matches are kept (the same artist and title, a year
 *     that fits, and no other album that would fit as well); the rest are
 *     counted, for Find cover.
 *
 * What's sent: each album's artist and title (and Fathom's name, as
 * MusicBrainz asks). A cover kept is saved as found, a JPEG, in the covers
 * folder on the card (library_found_cover_path), never beside the music; the
 * library makes its thumbnails from it, at once and at later scans. Albums
 * whose found cover was removed (a .none file there) are left out of the
 * batch.
 *
 * One worker thread does it all; a single find goes before the batch's next
 * album. MusicBrainz is asked at most once every 1.1 s, as it asks, and up
 * to four times (2, 3 and 5 s apart) when it says it's busy (HTTP 503).
 */
#ifndef FATHOM_COVERFIND_H
#define FATHOM_COVERFIND_H

#include <stddef.h>
#include <stdint.h>

#include "library/library.h"
#include "net/http.h"

typedef lib_album_t cf_album_t;   /* (its key: where its found cover goes) */

enum { CF_MUSICBRAINZ, CF_APPLE };

typedef struct {
    char title[256], artist[256];
    int year;
    int source;               /* CF_MUSICBRAINZ, CF_APPLE */
    char image[512];          /* its picture's URL */
    int relevance;            /* the source's own: MusicBrainz's score (0-100), Apple's order */
    int score;                /* how well it fits (cf_rate): higher is better */
    int sure;                 /* the same artist and title, and a year that fits */
} cf_match_t;

/* ---------- the parts that don't need the network (tests) ---------- */

/* For comparing: lower case, accents and punctuation dropped, "&" as "and",
 * a leading "the" dropped, spaces single. A title (title = 1) also loses an
 * edition's note: "(Remastered 2011)", "[Deluxe Edition]", " - EP"... */
void cf_norm(const char *in, int title, char *out, size_t cap);
void cf_mb_url(const cf_album_t *a, char *out, size_t cap);
void cf_apple_url(const cf_album_t *a, char *out, size_t cap);
/* An answer's albums (at most max), each rated for a (cf_rate), best first.
 * -1 if it isn't an answer at all. */
int  cf_parse_mb(const char *json, size_t len, const cf_album_t *a, cf_match_t *out, int max);
int  cf_parse_apple(const char *json, size_t len, const cf_album_t *a, cf_match_t *out, int max);
void cf_rate(const cf_album_t *a, cf_match_t *m);
/* The one sure match to keep without asking: its index, or -1 (none, or more
 * than one album would fit: Weezer has seven called "Weezer"; a self-titled
 * album without a year isn't taken either). */
int  cf_pick(const cf_album_t *a, const cf_match_t *m, int n);

/* ---------- the worker ---------- */

/* Fetching and downloading: http_get and http_download unless replaced (the
 * simulator and the tests read saved files). */
typedef long (*cf_fetch_fn)(const char *url, char *buf, size_t cap, const volatile int *abort);
typedef int64_t (*cf_download_fn)(const char *url, const char *path, http_progress_fn progress, void *ctx,
                                  const volatile int *abort);
void cf_set_fetch(cf_fetch_fn fn);
void cf_set_download(cf_download_fn fn);
void cf_set_mb_gap_ms(int ms);   /* (the tests: no wait between MusicBrainz's answers) */
void cf_set_retry_ms(int ms);    /* (the tests: between tries; otherwise 2 s, 3 s, 5 s) */

/* Start the worker; found covers go in dir (fathom/covers). 0, -1. */
int  cf_start(const char *dir);
void cf_stop(void);

/* One album: a new find (the last one's dropped). 0, -1 (not started). */
int  cf_find(const cf_album_t *a);
/* The find: 1 done (*n matches, maybe none), 0 looking, -1 none asked, -2 it
 * failed (no network, no answer from either). */
int  cf_find_state(int *n);
/* Match k: 1 and its picture as a large cover (LIB_ART_LARGE square, RGB565)
 * if it's here; 0 while it's fetched (asked now if it wasn't); -1 it has none
 * (or it couldn't be read); -2 it couldn't be fetched (three tries: no
 * network, a name that didn't resolve). m: the match (may be NULL). */
int  cf_find_show(int k, cf_match_t *m, uint16_t *large);
/* Match k's picture asked again (one that couldn't be fetched). 0, -1. */
int  cf_find_retry(int k);
/* Keep match k's picture (it must be here: cf_find_show 1) as the album's
 * found cover, its thumbnails into large and small (LIB_ART_SMALL) for
 * library_store_found_cover. 0, -1. */
int  cf_find_keep(int k, uint16_t *large, uint16_t *small);
/* Done with the find: its pictures dropped. */
void cf_find_close(void);

/* Every album in list (a copy is kept): those whose found cover was removed
 * are left out. 0, -1 (not started, or a batch is under way). */
int  cf_batch(const cf_album_t *list, int n);
void cf_batch_stop(void);
typedef struct {
    int running;
    int total, done;          /* albums to look at, looked at */
    int found, unsure, none;  /* kept; maybe (for Find cover); nothing like it */
    int failed;               /* couldn't ask (no network): stopped */
    unsigned seq;             /* up when a batch ends */
} cf_batch_t;
void cf_batch_status(cf_batch_t *st);
/* A cover the batch kept, to store (library_store_found_cover): 1 and the
 * album and its thumbnails, or 0. */
int  cf_batch_take(int64_t *album, uint16_t *large, uint16_t *small);

#endif
