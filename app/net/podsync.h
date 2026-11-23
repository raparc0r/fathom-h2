/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/*
 * Podcasts over the network (net/podcast.h for the files): one worker thread
 * does one job at a time, in the order asked:
 *
 *   - reading a show's feed (or every show's), keeping its newest episodes
 *     and its picture, small (ID/cover.565, ID/thumb.565);
 *   - subscribing to a show (shows.txt), then reading its feed;
 *   - searching Apple's directory (a new search replaces one still running);
 *   - downloading an episode to the card (ID/KEY.ext.part, renamed when whole).
 *
 * The UI polls podsync_status(): what it's doing, a download's progress, and
 * `changed`, which goes up whenever something on the card changed (a feed
 * read, a download done or failed) so lists can be read again. Nothing runs
 * unless asked; podsync_auto() is the automatic part (stage 2): feeds read
 * every few hours and new episodes downloaded while charging on Wi-Fi.
 */
#ifndef FATHOM_PODSYNC_H
#define FATHOM_PODSYNC_H

#include <stddef.h>
#include <stdint.h>

#include "net/http.h"
#include "net/podcast.h"

/* Fetching and downloading: http_get and http_download unless replaced (the
 * simulator and the tests read saved files). Same contracts as those. */
typedef long (*podsync_fetch_fn)(const char *url, char *buf, size_t cap, const volatile int *abort);
typedef int64_t (*podsync_download_fn)(const char *url, const char *path, http_progress_fn progress, void *ctx,
                                       const volatile int *abort);
void podsync_set_fetch(podsync_fetch_fn fn);
void podsync_set_download(podsync_download_fn fn);

/* Start the worker for the podcasts in dir (fathom/podcasts). 0, -1. */
int  podsync_start(const char *dir);
void podsync_stop(void);                      /* (stops a download, waits for the worker) */
const char *podsync_dir(void);

/* Jobs, queued (0) or not (-1: the queue is full, or not started). */
int podsync_refresh(const char *url);         /* "" or NULL: every show */
int podsync_subscribe(const pod_show_t *s);   /* then its feed is read */
int podsync_unsubscribe(const char *url);     /* its downloads go too */
int podsync_import_opml(const char *path);    /* every show in it (those not there yet) */
int podsync_search(const char *term);
int podsync_download(const char *id, const pod_episode_t *e);
void podsync_cancel_download(const char *id, const char *key);   /* queued or under way */

/* An episode's download: -1 none, 0 queued, 1..100 under way (percent; 1 while
 * the size isn't known). */
int podsync_download_state(const char *id, const char *key);

/* The last search: 1 and up to max shows (*n) when done, 0 while it runs, -1
 * none asked, -2 it failed (no network, no answer). */
int podsync_search_result(pod_show_t *out, int max, int *n);

typedef struct {
    int busy;                 /* a job under way */
    int queued;               /* jobs waiting */
    unsigned changed;         /* up when something on the card changed */
    int refreshing;           /* reading feeds */
    int last_failed;          /* the last feed or download failed (no network...) */
    unsigned failed_seq;      /* up each time one fails */
    char failed_what[64];     /* ...which: a show's or an episode's title */
    int imported;             /* shows the last OPML import added (-1: it couldn't be read) */
    unsigned import_seq;      /* up when an import finished */
} podsync_status_t;
void podsync_status(podsync_status_t *st);

/* Stage 2, called now and then (once a minute): with Wi-Fi up, feeds read if
 * the last was over `every_s` ago; while charging (and on Wi-Fi), each show's
 * newest `keep` unplayed episodes downloaded; played downloads older than
 * `clean_days` days removed (any time). 1 if it queued something. */
int podsync_auto(int64_t now, int wifi, int charging, int every_s, int keep, int clean_days);

#endif
