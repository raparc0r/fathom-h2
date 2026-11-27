/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/*
 * A song's lyrics: its lines, each with the time it is sung at when the text
 * says so (an LRC file's "[01:23.45]"), else plain text to read. Where they
 * come from, looked at in this order:
 *
 *   - a file beside the song with its name and ".lrc" (yours: it comes first);
 *   - the song's own tags (FLAC's LYRICS and UNSYNCEDLYRICS comments, an
 *     MP3's USLT frame, an M4A's lyrics: tags_lyrics, tags.h);
 *   - what was found online for it earlier, kept in the card's lyrics folder
 *     (net/lyricfind.h), never beside the music.
 *
 * A track of an album in one file (a CUE sheet's) has only the last: the
 * file's tags and its .lrc are the whole album's.
 */
#ifndef FATHOM_LYRICS_H
#define FATHOM_LYRICS_H

#include <stddef.h>
#include <stdint.h>

#define LYRICS_MAX_BYTES (128 * 1024)   /* a text longer than this isn't a song's lyrics */
#define LYRICS_MAX_LINES 2000

typedef struct {
    int32_t ms;          /* when it is sung, from the song's start; -1: the text doesn't say */
    uint32_t at;         /* its text, in lyrics_t.text (several lines may share one: a chorus written once) */
} lyrics_line_t;

typedef struct {
    char *text;          /* the lines' texts, each ended with a NUL */
    lyrics_line_t *line;
    int n;
    int synced;          /* every line has its time, and they are in order */
} lyrics_t;

/* The text's lines (UTF-8; UTF-16 with its mark and Latin-1 are read too): 0,
 * or -1 if there is nothing to show in it (or no memory). An LRC's own lines
 * about itself ([ar:], [ti:], [offset:]...) are left out, its offset taken;
 * a line with several times is one line at each; times inside a line (word by
 * word) are dropped. Empty lines stay, one at a time: a pause in the song. */
int  lyrics_parse(const char *in, size_t len, lyrics_t *out);
void lyrics_free(lyrics_t *l);
static inline const char *lyrics_line(const lyrics_t *l, int i) { return l->text + l->line[i].at; }
/* The line being sung at ms: -1 before the first (and for plain text). */
int  lyrics_at(const lyrics_t *l, int32_t ms);
/* 1 if the text has a line that starts with a time: timed lyrics. */
int  lyrics_looks_synced(const char *text, size_t len);

typedef struct {
    const char *path;                    /* the song's file (a CUE track's path as the library has it) */
    const char *artist, *title, *album;  /* as tagged: what found lyrics are kept under */
} lyrics_song_t;

enum { LYRICS_FROM_FILE = 1, LYRICS_FROM_TAG, LYRICS_FROM_STORE };

/* What the song has without asking anyone (store: the lyrics folder, may be
 * NULL): 0 with *out and where from, or -1. */
int  lyrics_local(const char *store, const lyrics_song_t *s, lyrics_t *out, int *from);

/* The lyrics folder: found lyrics are STORE/KEY.lrc, the key made of the
 * song's artist, title and album, whatever its file is called or where it is;
 * STORE/KEY.none says it was looked for and not found (lyrics_store_none: when). */
void lyrics_store_path(const char *store, const lyrics_song_t *s, const char *ext, char *out, size_t cap);
int  lyrics_store_save(const char *store, const lyrics_song_t *s, const char *text, size_t len);   /* 0, -1 */
int  lyrics_store_none(const char *store, const lyrics_song_t *s);                                 /* 0, -1 */
/* 1 if it was looked for and not found less than `days` ago. */
int  lyrics_store_tried(const char *store, const lyrics_song_t *s, int days);

#endif
