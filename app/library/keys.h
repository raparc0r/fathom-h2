/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* Normalized grouping keys for artists and albums (used by the library and shuffle). */
#ifndef FATHOM_KEYS_H
#define FATHOM_KEYS_H

#include <stddef.h>

/*
 * Artist key: the album artist unless it is empty or a "Various Artists" marker,
 * otherwise the track artist. ASCII-lowercased, leading "the " removed,
 * featured guests ("feat.", "ft.", "featuring") cut off, whitespace collapsed.
 */
void key_artist(const char *artist, const char *album_artist, char *out, size_t cap);

/* Album key: the album artist's key (as key_artist makes it for the album artist, or
 * the artist without one; "various artists" for compilations) + U+001F + the
 * normalized album title. */
void key_album(const char *artist, const char *album_artist, const char *album,
               char *out, size_t cap);

/*
 * Everyone credited on a track, as display names (trimmed, in order, no
 * duplicates ignoring case): the artist tag split at "feat." / "ft." /
 * "featuring" / "with" and at ";" and " / ", plus guests named in the title
 * ("Song (feat. X)"). Guest parts are also split at ", ", " & " and " and ",
 * which main names are not ("Simon & Garfunkel" stays one). Returns the count.
 */
#define KEY_CREDIT_MAX 8
#define KEY_CREDIT_LEN 128
int key_credits(const char *artist, const char *title, char names[KEY_CREDIT_MAX][KEY_CREDIT_LEN]);

/* True if the album artist marks a compilation ("Various Artists", "VA", ...). */
int key_is_various(const char *album_artist);

#endif
