/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/*
 * Podcasts (Home > Podcasts): the parts that need no network, so the desktop
 * tests run them; net/podsync.h fetches and downloads with them.
 *
 *   - a show's feed (RSS, as every podcast publishes): its title, author and
 *     cover, and its episodes (title, date, length, the audio file's address,
 *     Podcasting 2.0 chapters), newest first, at most POD_MAX_EPISODES. A
 *     feed cut short (POD_FEED_MAX) keeps the episodes before the cut;
 *   - OPML (the subscription list any podcast app exports): shows to add;
 *   - Apple's podcast search (itunes.apple.com/search): shows found;
 *   - the files on the card, in DIR (fathom/podcasts):
 *       shows.txt          the subscriptions, one "Title = feed URL" a line
 *                          ('#' a note), in order; it can be edited by hand
 *       ID/show.txt        a show's details (key = value lines)
 *       ID/episodes.txt    its episodes, as last read from the feed
 *       ID/state.txt       where each episode was left, and which are played
 *       ID/KEY.mp3 (...)   downloaded episodes (KEY.mp3.part while downloading)
 *       ID/cover.565       its cover for Now Playing, ID/thumb.565 the same
 *                          small for the list of shows, ID/cover.url the
 *                          address of the image they were last made from
 *     ID is from the feed's address, KEY from the episode's guid (12 hex
 *     digits of their SHA-256), so a show or an episode keeps its files when
 *     its title changes. Files are written beside and moved over (a power cut
 *     leaves the old one or the new one).
 */
#ifndef FATHOM_PODCAST_H
#define FATHOM_PODCAST_H

#include <stddef.h>
#include <stdint.h>

#define POD_MAX_SHOWS 64
#define POD_MAX_EPISODES 60           /* kept per show, newest first */
#define POD_FEED_MAX (1536 * 1024)    /* a feed read up to here: some feeds are 10 MB of every episode ever */
#define POD_SEARCH_MAX 25

typedef struct {
    char id[13];            /* its folder: from the feed's address */
    char title[128];
    char url[512];          /* the feed */
    char author[96];
    char image[384];        /* its cover's address, "" = none */
    int64_t refreshed;      /* when its feed was last read (seconds), 0 = never */
    int64_t seen;           /* when its episodes were last looked at: newer ones are new */
    int64_t latest;         /* its newest episode's date */
    int episodes;           /* (search results: how many it has, if Apple says) */
} pod_show_t;

typedef struct {
    char key[13];           /* its files and state: from its guid */
    char title[160];
    char url[640];          /* the audio file (enclosure) */
    char type[32];          /* "audio/mpeg" */
    int64_t size;           /* bytes, if the feed says (0 = not said) */
    int64_t date;           /* published (seconds), 0 = not said */
    int dur_s;              /* length (seconds), 0 = not said */
    char chapters[256];     /* Podcasting 2.0 chapters (JSON), "" = none */
} pod_episode_t;

/* Where an episode was left. */
typedef struct {
    char key[13];
    uint32_t pos_ms;        /* 0 = from the start */
    uint32_t len_ms;        /* its length when it was played (0 = not known) */
    int played;
    int64_t played_at;      /* when it was finished or marked played (seconds) */
} pod_state_t;

/* ---- parsing (pure; tested on saved feeds) ---- */

/* A feed: the show's title, author and cover into show (its url and id are
 * left as they are), and up to max episodes (those with an audio file),
 * newest first. Returns how many, or -1 if it isn't a podcast feed. */
int pod_parse_feed(const char *xml, size_t len, pod_show_t *show, pod_episode_t *eps, int max);
/* Whether a feed arrived whole: it ends with its root's closing tag (</rss>,
 * </feed>, </rdf:RDF>). One cut off (the Wi-Fi gone mid-transfer) would read
 * as a feed with fewer episodes. */
int pod_feed_complete(const char *xml, size_t len);
/* OPML: each <outline> with a feed (xmlUrl), its title and url set. How many. */
int pod_parse_opml(const char *xml, size_t len, pod_show_t *out, int max);
/* Apple's search answer: the shows, with a feed each. How many, -1 if it isn't one. */
int pod_parse_search(const char *json, size_t len, pod_show_t *out, int max);
void pod_search_url(char *out, size_t cap, const char *term);
/* Podcasting 2.0 chapters (JSON): up to max start times (ms) and titles. How many. */
typedef struct { uint32_t start_ms; char title[96]; } pod_chapter_t;
int pod_parse_chapters(const char *json, size_t len, pod_chapter_t *out, int max);

/* Chapters in the audio file itself (MP3's ID3 CHAP, M4A's): how many (0
 * without FFmpeg, the desktop build). */
int pod_file_chapters(const char *path, pod_chapter_t *out, int max);
/* An episode's chapters: the feed's (KEY.json, fetched with the download),
 * else the file's own. How many. */
int pod_chapters(const char *dir, const char *id, const pod_episode_t *e, pod_chapter_t *out, int max);
void pod_chapters_path(const char *dir, const char *id, const char *key, char *out, size_t cap);
/* A show's cover, kept small (ID/cover.565: RGB565, LIB_ART_LARGE square, made from
 * its image when its feed is read) */
void pod_cover_path(const char *dir, const char *id, char *out, size_t cap);
/* The same for the list of shows (ID/thumb.565: RGB565, LIB_ART_SMALL square) */
void pod_thumb_path(const char *dir, const char *id, char *out, size_t cap);

/* "Tue, 19 Nov 2026 05:00:00 +0000" (RFC 822, as feeds give it): seconds, or 0. */
int64_t pod_parse_date(const char *s);
/* "3600", "59:12", "1:02:03": seconds, or 0. */
int pod_parse_duration(const char *s);
/* 12 hex digits of SHA-256(s): show ids and episode keys. */
void pod_hash(const char *s, char out[13]);
/* The extension for an episode's file: ".mp3", ".m4a", ".ogg", ".opus", ".aac". */
const char *pod_ext(const pod_episode_t *e);

/* ---- the card (dir: fathom/podcasts) ---- */

/* The subscriptions, in order, with each show's details from show.txt. How
 * many (0 too when shows.txt is there and can't be read). */
int pod_shows_load(const char *dir, pod_show_t *out, int max);
/* The list alone: each show's address, its folder (id) and the title it was
 * added with, nothing read of the shows' own files (two a show): for asking
 * "is it subscribed to" and "which folder is its". How many. */
int pod_shows_list(const char *dir, pod_show_t *out, int max);
/* One show by its address, with its details as pod_shows_load gives them: 1,
 * or 0 if it isn't subscribed to. Reads the list and that show's files only. */
int pod_show_find(const char *dir, const char *url, pod_show_t *out);
/* Subscribe: 1 added (at the end), 0 already there (the same feed), -2 full,
 * -1 couldn't write, or shows.txt is there and couldn't be read (nothing is
 * written over it). */
int pod_shows_add(const char *dir, const pod_show_t *s);
/* Unsubscribe: its line and its folder (downloads included). 0, -1 (shows.txt
 * not read whole: nothing written, nothing removed). */
int pod_shows_remove(const char *dir, const char *url);
/* A show's details (show.txt), and the time its feed was read (not `seen`). */
int pod_show_save(const char *dir, const pod_show_t *s);
/* When its episodes were last looked at (seen.txt: only the UI writes it, so
 * a feed read meanwhile can't put it back). */
int pod_show_seen(const char *dir, const char *id, int64_t seen);

int pod_episodes_save(const char *dir, const char *id, const pod_episode_t *eps, int n);
int pod_episodes_load(const char *dir, const char *id, pod_episode_t *eps, int max);

int pod_state_load(const char *dir, const char *id, pod_state_t *st, int max);
/* One episode's state, kept (the others as they were). 0, -1 (-1 too when
 * state.txt is there and couldn't be read: nothing is written over it). */
int pod_state_put(const char *dir, const char *id, const pod_state_t *s);
/* An episode's in st (or all zero, its key set, if it has none). */
void pod_state_find(const pod_state_t *st, int n, const char *key, pod_state_t *out);

/* An episode's file (whether or not it's there). */
void pod_episode_path(const char *dir, const char *id, const pod_episode_t *e, char *out, size_t cap);
/* 1 if it's downloaded (complete). */
int pod_downloaded(const char *dir, const char *id, const pod_episode_t *e);
/* Remove its download (and any part of one). 0, -1 (none). */
int pod_delete_download(const char *dir, const char *id, const pod_episode_t *e);

#endif
