/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Music library: SQLite database of tracks, albums and artists, an incremental
 * scanner, and a cover-art cache (pre-scaled RGB565, the panel's native format).
 *
 * Built for 32 MiB of RAM: lists hold only row ids; text is fetched per visible
 * row. The music folder is only ever read.
 */
#ifndef FATHOM_LIBRARY_H
#define FATHOM_LIBRARY_H

#include <stddef.h>
#include <stdint.h>

#include "shuffle/shuffle.h"

#define LIB_ART_LARGE 136   /* Now Playing */
#define LIB_ART_SMALL 34    /* list thumbnails */

typedef struct library library_t;

typedef enum {
    LIB_ALBUMS,          /* all albums, by title */
    LIB_ALBUM_ARTISTS,   /* album artists (compilations: "Various Artists"), by name */
    LIB_ARTISTS,         /* everyone credited on a track (guests too), by name */
    LIB_ARTIST_ALBUMS,   /* parent = album artist id; their albums, by year */
    LIB_CREDIT_ALBUMS,   /* parent = artist id; albums they're credited on, by year */
    LIB_CREDIT_TRACKS,   /* library_list_credit_tracks(): their songs on one album */
    LIB_ALBUM_TRACKS,    /* parent = album id; by disc, track */
    LIB_SONGS,           /* all tracks, by title */
    LIB_PLAYLISTS,       /* M3U/M3U8 files found in the music folder, by file name */
    LIB_PLAYLIST_TRACKS, /* parent = playlist id; in playlist order (entries found in the library) */
    LIB_FAVOURITES,      /* tracks marked as favourites, by title */
    LIB_RECENT_PLAYED,   /* the last 200 tracks played, newest first */
    LIB_MOST_PLAYED,     /* the 200 tracks played most */
    LIB_RECENT_ADDED,    /* albums, newest files first (100) */
    LIB_GENRES,          /* genres (as tagged), by name */
    LIB_GENRE_ALBUMS,    /* parent = genre id; albums with a track in it, by title */
    LIB_COMPOSERS,       /* composers (COMPOSER tag), by name */
    LIB_COMPOSER_ALBUMS, /* parent = composer id; albums with their works, by year */
    LIB_COMPOSER_TRACKS, /* library_list_composer_tracks(): their tracks on one album */
    LIB_YEARS,           /* the years albums came out, newest first; ids are the years */
    LIB_YEAR_ALBUMS,     /* parent = a year; its albums, by title */
    LIB_FOLDER,          /* parent = folder id (0: the top); subfolders (as -id), then songs (ids) */
} lib_view_t;

typedef struct {
    lib_view_t view;
    int64_t   *ids;
    int        n;
} lib_list_t;

typedef struct {
    int64_t  id, album_id, artist_id;
    char     path[1024];   /* a CUE track's: its file's, and which part (library/cue.h) */
    char     title[256], artist[256], album[256];
    int      disc, track, year;
    char     codec[8];
    uint32_t sample_rate, duration_ms;
    uint8_t  bits, channels;
} lib_track_t;

typedef void (*lib_progress_cb)(int done, int total, const char *path, void *ctx);

library_t *library_open(const char *db_path, const char *art_dir);
void       library_close(library_t *lib);
/* Hands back the database cache's memory (read again from the card as needed):
 * with the screen off, nothing browses for a while. */
void       library_release_memory(library_t *lib);

/* Scan `root` (read-only): add new/changed files, drop missing ones, rebuild albums,
 * artists, playlists and the art cache. Returns the number of tracks, or -1 on error.
 * Its writes go in short transactions: another connection's wait a fraction of
 * a second at most, and a scan that finds nothing new writes nothing. */
int library_scan(library_t *lib, const char *root, lib_progress_cb cb, void *ctx);
/* The music folder is on FAT32 partition `dev` mounted at `mount`: scans first
 * read its FAT directly to tell quickly that nothing changed (fatwalk.h). */
void library_set_fast_walk(library_t *lib, const char *dev, const char *mount);
/* Tracks the last scan added, changed or dropped (0: the library is as it was). */
int library_scan_changes(const library_t *lib);
/* Stop a scan running on this library (from another thread): it returns -1.
 * Nothing is dropped; the tracks it had read are kept, and the next scan
 * puts them in their albums, playlists and search. */
void library_scan_cancel(library_t *lib);

int  library_list(library_t *lib, lib_view_t view, int64_t parent, lib_list_t *out);
void library_list_free(lib_list_t *l);
/* An artist's songs on one album (LIB_CREDIT_TRACKS), by disc and track. */
int  library_list_credit_tracks(library_t *lib, int64_t artist_id, int64_t album_id, lib_list_t *out);
/* How many albums an artist has as album artist, and is credited on. */
void library_artist_counts(library_t *lib, int64_t artist_id, int *nalbums, int *ncredit_albums);
/* Album lists whose rows show the album artist on the right, not the year:
 * where the list isn't one artist's already, and the year isn't its order
 * (an artist's albums, the ones they're credited on, keep the year). */
static inline int library_row_shows_artist(lib_view_t v)
{
    return v == LIB_ALBUMS || v == LIB_RECENT_ADDED || v == LIB_GENRE_ALBUMS || v == LIB_YEAR_ALBUMS ||
           v == LIB_COMPOSER_ALBUMS;
}
/* Whether those rows show the artist (on, by default) or the year. */
void library_set_album_artists(library_t *lib, int on);
/* Text for one row: main label and right-hand metadata (year, album artist,
 * duration, count). */
int  library_row(library_t *lib, lib_view_t view, int64_t id,
                 char *main, size_t main_cap, char *meta, size_t meta_cap);
int  library_track(library_t *lib, int64_t id, lib_track_t *t);
int  library_album_name(library_t *lib, int64_t album_id, char *out, size_t cap);
int  library_artist_name(library_t *lib, int64_t artist_id, char *out, size_t cap);
/* Tracks read before tags added later (the composer, a CUE sheet in the
 * tags): a scan reads just those from them, once. 1 if there are any. */
int  library_needs_tags(library_t *lib);
/* Albums without a cover that no scan has looked at since covers could come
 * from more places (FLAC pictures of any type, other tracks, a disc's album
 * folder, big images): one background scan finds theirs. */
int  library_needs_art(library_t *lib);
/* A composer's tracks on one album (LIB_COMPOSER_TRACKS). */
int  library_list_composer_tracks(library_t *lib, int64_t composer_id, int64_t album_id, lib_list_t *out);
/* Names for screens' titles: 0, or -1 if there's no such one. A folder id of 0
 * is the top folder. */
int  library_genre_name(library_t *lib, int64_t id, char *out, size_t cap);
int  library_composer_name(library_t *lib, int64_t id, char *out, size_t cap);
int  library_folder_name(library_t *lib, int64_t id, char *out, size_t cap);
int  library_playlist_name(library_t *lib, int64_t playlist_id, char *out, size_t cap);
int64_t library_album_artist(library_t *lib, int64_t album_id);   /* 0 if unknown */

/* Cached cover art for an album as RGB565 (size*size*2 bytes). 0 if present. */
int library_art(library_t *lib, int64_t album_id, int size, uint16_t *pixels);
/* A track's album (its cover in Songs): the id, 0 if unknown. */
int64_t library_track_album(library_t *lib, int64_t track_id);
/* An image file's contents (JPEG, PNG) as a square cover, centre-cropped, as
 * RGB565 (size*size*2 bytes): a radio station's. 0 if decoded. */
int library_art_decode(const unsigned char *enc, size_t len, int size, uint16_t *pixels);

/* All tracks in shuffle form (ids in shuf_track_t.id). Caller frees *out. */
int library_shuffle_tracks(library_t *lib, shuf_track_t **out);
/* Playback history for freshness and "most played". */
void library_played(library_t *lib, int64_t track_id, int skipped_early, int64_t now);

/* Your own playlists: M3U8 files in a folder of their own (fathom/playlists on
 * the card, never the music folder), scanned with the music and editable on the
 * player. Entries are written relative to the playlist, with #EXTINF lines. */
void    library_set_playlist_dir(library_t *lib, const char *dir);
int     library_playlist_writable(library_t *lib, int64_t playlist_id);
int64_t library_playlist_create(library_t *lib, const char *name);   /* 0 on failure */
int     library_playlist_add(library_t *lib, int64_t playlist_id, const int64_t *tracks, int n);
int     library_playlist_remove(library_t *lib, int64_t playlist_id, int pos);
int     library_playlist_move(library_t *lib, int64_t playlist_id, int pos, int dir);
int     library_playlist_delete(library_t *lib, int64_t playlist_id);
int     library_playlist_rename(library_t *lib, int64_t playlist_id, const char *name);   /* and its file */
int     library_playlist_count_writable(library_t *lib);

/* Search: every word of `query` must start a word of the title, artist or
 * album (case and accents ignored). Up to 5 artists, 10 albums, 40 songs. */
typedef struct { lib_list_t artists, albums, songs; } lib_search_t;
int  library_search(library_t *lib, const char *query, lib_search_t *out);   /* total found */
void library_search_free(lib_search_t *r);

/* Favourites. */
void library_set_favourite(library_t *lib, int64_t track_id, int on);
int  library_favourite(library_t *lib, int64_t track_id);

/* Volume levelling (ReplayGain 2.0, -18 LUFS). Each track's gain comes from
 * its tags (at scan time) or is measured later (fathom-player, in the
 * background while charging): */
typedef struct {
    int   state;                  /* 1 = from tags, 2 = measured, 3 = couldn't */
    float track_gain, track_peak; /* dB; 0..1 of full scale */
    int   has_album;              /* album gain tagged */
    float album_gain, album_peak;
} lib_rg_t;
int   library_rg_next(library_t *lib, int64_t *id, char *path, size_t cap);   /* 0 if one is left */
void  library_rg_store(library_t *lib, int64_t id, const lib_rg_t *rg);
void  library_rg_progress(library_t *lib, int *done, int *total);
/* The linear factor to play a track at: its own gain, or with `album` its
 * album's; held below what would clip; 1.0 if not known yet. */
float library_rg_gain(library_t *lib, int64_t id, int album);

/* Where a long file was left (audiobooks, mixes): 0 = start. */
void     library_set_resume(library_t *lib, int64_t track_id, uint32_t ms);
uint32_t library_resume(library_t *lib, int64_t track_id);

#endif
