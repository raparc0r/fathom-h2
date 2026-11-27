/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/*
 * The station directory (Home > Radio > Find stations): radio-browser.info's
 * public API, https://api.radio-browser.info. Asked only when someone opens a
 * list or searches: nothing at start-up, nothing in the background. The one
 * exception: a station of yours with no logo and no site of its own is looked
 * up once, by its stream, when Radio is opened (rdir_lookup_stream, for
 * net/radioicon.c, which never sends an address that looks private).
 *
 * One request at a time runs on its own worker thread (rdir_start); the UI
 * polls for the answer (rdir_poll) and drops one it no longer wants
 * (rdir_cancel). Answers are read into a bounded buffer (RDIR_MAX_ANSWER),
 * parsed in place (net/json.h) and only what's shown is kept: a station's
 * name, stream, a short detail ("MP3 128") and its id. Streams the H2 can't
 * play (WMA, video, an unknown codec that isn't HLS) are left out.
 */
#ifndef FATHOM_RADIODIR_H
#define FATHOM_RADIODIR_H

#include <stddef.h>

#include "net/http.h"

#include "player/radio.h"

#define RDIR_MAX_STATIONS RADIO_MAX     /* a list plays as the radio's stations: Next/Prev step through it */
#define RDIR_ASK_STATIONS 80            /* asked for: some are left out as unplayable */
#define RDIR_MAX_COUNTRIES 256
#define RDIR_MAX_ANSWER (128 * 1024)    /* 80 stations take ~100 KB; a longer answer is cut, whole stations kept */

/* What a station list is of. */
typedef enum { RDIR_POPULAR, RDIR_NAME, RDIR_TAG, RDIR_COUNTRY } rdir_by_t;

typedef struct {
    rdir_by_t by;
    char arg[64];        /* the name searched for, the tag, or the country's code */
    int random;          /* in random order (else most listened first) */
} rdir_query_t;

/* Shown beside a station, and what counting a click needs. */
typedef struct {
    char detail[16];     /* "MP3 128", "AAC+ 64", "FLAC", "HLS" */
    char uuid[40];       /* radio-browser's stationuuid */
} rdir_meta_t;

typedef struct {
    char name[64];       /* "United Kingdom" (the directory's long forms shortened) */
    char code[4];        /* ISO 3166-1: "GB" */
    int count;           /* working stations */
} rdir_country_t;

/* A finished request: stations or countries (the other NULL), owned by the
 * caller (rdir_result_free). n < 0: it failed (no answer from any server). */
typedef struct {
    int n;
    radio_station_t *st;
    rdir_meta_t *meta;
    rdir_country_t *countries;
} rdir_result_t;

/* Genres: a fixed list of the directory's well-used tags, so choosing one
 * asks for nothing until a genre is opened. */
extern const char *const RDIR_GENRES[];
extern const int RDIR_N_GENRES;
void rdir_genre_tag(int i, char *out, size_t cap);          /* genre i's tag */

/* ---- parsing (pure; tested on saved answers) ---- */

/* A codec the H2 decodes, as the directory names it ("MP3", "AAC+", "OGG"...);
 * hls: an HLS stream (its codec is often "UNKNOWN": the H2 picks a variant). */
int  rdir_playable(const char *codec, int hls);
/* The stations of an answer from /json/stations/...: up to max (playable ones,
 * each stream once), meta may be NULL. An answer cut short keeps the stations
 * before the cut. */
int  rdir_parse_stations(const char *text, size_t len, radio_station_t *st, rdir_meta_t *meta, int max);
/* The countries of an answer from /json/countries, by name. */
int  rdir_parse_countries(const char *text, size_t len, rdir_country_t *out, int max);
/* The request for q on server (e.g. "de1.api.radio-browser.info"). */
void rdir_stations_url(char *out, size_t cap, const char *server, const rdir_query_t *q);
void rdir_countries_url(char *out, size_t cap, const char *server);
/* The request for the station(s) with this stream, and what its answer says
 * of them: the station's own web page and its logo's address ("" for one it
 * doesn't give, or that isn't an address). 1, 0 if the directory has no
 * station with that stream, -1 if it isn't an answer at all. */
void rdir_stream_url(char *out, size_t cap, const char *server, const char *stream);
int  rdir_parse_stream(const char *text, size_t len, char *site, size_t sn, char *icon, size_t in);

/* ---- requests (a worker thread) ---- */

/* What fetches: http_get (net/http.h) unless replaced (the simulator and the
 * tests read saved answers). Same contract as http_get. */
void rdir_set_fetch(http_get_fn fn);

/* Start a request (cancelling any still running): stations for q, or (q NULL)
 * the countries. 0 started, -1 not (no memory, no thread). */
int  rdir_start(const rdir_query_t *q);
/* 1 and *out when the request has finished, 0 while it runs, -1 with none. */
int  rdir_poll(rdir_result_t *out);
int  rdir_busy(void);
void rdir_cancel(void);
void rdir_result_free(rdir_result_t *r);

/* A stream looked up there and then, on the caller's thread (the servers in
 * turn, as a request): as rdir_parse_stream, or -1 if none answered (or
 * *abort was set). */
int  rdir_lookup_stream(const char *stream, char *site, size_t sn, char *icon, size_t in, const volatile int *abort);

/* radio-browser counts a station's plays when a client asks for
 * /json/url/{uuid}: once when one is played from the directory (on its own
 * short-lived thread, the answer discarded). Asked a few seconds later, at
 * the lowest priority, so it doesn't compete with the station's connection;
 * of stations chosen within that time, the last is the one counted. */
void rdir_count_click(const char *uuid);
void rdir_test_click_wait(int ms);   /* tests: that wait (-1: the usual one) */

#endif
