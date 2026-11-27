/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/*
 * A song's lyrics, read or looked up off the UI thread: what the song has by
 * itself (library/lyrics.h: a .lrc beside it, its tags, what was found
 * before) and, only when asked, LRCLIB (lrclib.net: an open collection, no
 * account), which is asked for the song by its artist, title, album and
 * length, then without the album if it has none under that one.
 *
 * What's sent: those four (and Fathom's name, as every request has). Lyrics
 * found are kept in the lyrics folder on the card (lyrics_store_save), never
 * beside the music; timed ones are only taken for a recording of the same
 * length, give or take three seconds.
 *
 * One song at a time: a new ask drops the one before (its fetch given up).
 */
#ifndef FATHOM_LYRICFIND_H
#define FATHOM_LYRICFIND_H

#include <stddef.h>
#include <stdint.h>

#include "library/lyrics.h"
#include "net/http.h"

typedef struct {
    int64_t track;             /* the library's id (the caller's: which song the answer is for) */
    char path[1024];
    char artist[256], title[256], album[256];
    int duration_s;            /* 0: not known */
} lf_song_t;

/* ---------- the parts that don't need the network (tests) ---------- */

/* LRCLIB's address for the song, with its album or without. */
void lf_url(const lf_song_t *s, int with_album, char *out, size_t cap);
/* Its answer: 1 and the lyrics as text (malloc'd: an LRC's when it has timed
 * ones for a recording of this length, else plain), 2 it says the song has no
 * words, 0 it has neither, -1 it isn't an answer. */
int  lf_parse(const char *json, size_t len, int duration_s, char **text, size_t *text_len);

/* ---------- the worker ---------- */

void lf_set_fetch(http_get_fn fn);     /* (the simulator and the tests: saved answers) */
/* Where found lyrics are kept (fathom/lyrics); "" or NULL: nowhere, and
 * nothing is looked up online. */
void lf_start(const char *store);
/* As podsync's: _ask tells the worker to give up, _wait waits for it, ms at
 * most (1: it's back). */
void lf_stop_ask(void);
int  lf_stop_wait(int ms);

enum {
    LF_IDLE = -1,        /* nothing asked */
    LF_LOOKING = 0,
    LF_FOUND,            /* lf_take has them */
    LF_NONE,             /* the song has none of its own (online wasn't asked) */
    LF_NOT_FOUND,        /* asked online: not there */
    LF_INSTRUMENTAL,     /* asked online: a song without words */
    LF_FAILED            /* asked online: couldn't ask (no network, no answer) */
};

/* The song's lyrics asked for (the last ask is dropped): what it has by
 * itself, and if it has none and online is set, LRCLIB's. 0, -1. */
int  lf_ask(const lf_song_t *s, int online);
/* How the ask stands, and for which song (*track); *online: it is, or was,
 * looked up online; *tried: it was looked for online before and not found. */
int  lf_state(int64_t *track, int *online, int *tried);
/* The lyrics found, handed over once (the caller frees them: lyrics_free):
 * 0 with where they're from (LYRICS_FROM_*; from online: LYRICS_FROM_STORE), -1. */
int  lf_take(lyrics_t *out, int *from);

#endif
