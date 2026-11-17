/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * M3U / M3U8 playlists: read one into a list of paths, resolved against the
 * playlist's own folder, and the helpers used to match those paths to the
 * library's tracks.
 */
#ifndef FATHOM_PLAYLIST_H
#define FATHOM_PLAYLIST_H

#include <stddef.h>

typedef struct {
    char  name[256];    /* #PLAYLIST: if present, else the file name */
    char **paths;       /* entries, resolved and normalised */
    char **infos;       /* each entry's #EXTINF text ("Artist - Title"), or NULL */
    int   *secs;        /* each entry's #EXTINF duration in seconds, or -1 */
    int   n;
} m3u_t;

int  m3u_supported(const char *path);              /* .m3u or .m3u8 */
/* 0 on success. Relative entries are joined to the playlist's folder; URLs and
 * comments are skipped; Windows separators become '/'. */
int  m3u_read(const char *path, m3u_t *out);
/* The same from text in memory; `dir` is the folder relative entries are in. */
int  m3u_parse(const char *text, size_t len, const char *dir, const char *fallback_name, m3u_t *out);
void m3u_free(m3u_t *m);

/* In place: '\' -> '/', and "//", "/./", "dir/.." collapsed. */
void path_normalize(char *p);

/* The last `tail` components of `path`, folded for loose matching: ASCII
 * letters and digits only, lower case, Latin-1 accented letters as their base
 * letter (NFC and NFD spellings agree), '/' kept between components. */
void path_loose_key(const char *path, int tail, char *out, size_t cap);
/* The same folding over a whole string (tags, #EXTINF text). */
void text_loose_key(const char *s, char *out, size_t cap);

/* Words for search: lower-case ASCII letters and digits, Latin-1 accents as
 * their base letter, apostrophes dropped ("don't" -> "dont"), anything else a
 * single space; no leading or trailing space. "Beyoncé — Déjà Vu" -> "beyonce deja vu". */
void text_search_fold(const char *s, char *out, size_t cap);

#endif
