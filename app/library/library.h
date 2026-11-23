/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
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
#include <stdio.h>

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
    LIB_SMART,           /* parent = LIB_SMART_*: a smart playlist's songs, worked out now */
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

/* A scan's progress: done of total songs read (path: the one now); path
 * "albums": the albums and artists being sorted (no count); "indexes": a
 * first scan's indexes being built (no count); then, for covers,
 * total < 0: done albums looked at of -total - 1 (0: not known); path "done":
 * finished. */
typedef void (*lib_progress_cb)(int done, int total, const char *path, void *ctx);

library_t *library_open(const char *db_path, const char *art_dir);
/* A library from an older version, which library_open() brings up to date
 * (on a big one, a while: 50,000 songs, ~40 s on the H2): about how many songs
 * it has (its highest song id, read at once); 0 if it's up to date, new, or
 * not there. */
int64_t library_upgrade_needed(const char *db_path);
/* Called as an upgrade in library_open() goes (done of total steps; on the
 * thread that opens it). NULL: none. */
void library_set_upgrade_progress(void (*cb)(int done, int total, void *ctx), void *ctx);
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
/* The next scan (only) leaves the covers for the scan after it: a first scan
 * on its screen ends when the songs, albums and artists are in, and the
 * covers (its longest step) are found in the background while the library is
 * already in use (library_needs_art() says they're owed). */
void library_scan_defer_art(library_t *lib, int on);
/* Where what's being read is noted, so a file that kills the player isn't read
 * again at every start (a folder in RAM: /tmp on the H2; NULL or "": no notes).
 * A scan notes each song before its tags are read and each album before its
 * cover is made, and sets aside the one a note was left for. For work of the
 * player's own (levelling): library_trying(what, &fd, text) before each item
 * (fd -1 at first), library_trying_done after the run, and at the next run
 * library_trying_left(what, ...) gives the item it died on (1, and forgets it). */
void library_set_trying_dir(const char *dir);
void library_trying(const char *what, int *fd, const char *text);
void library_trying_done(const char *what, int *fd);
int  library_trying_left(const char *what, char *out, size_t cap);
/* Tracks the last scan added, changed or dropped (0: the library is as it was). */
int library_scan_changes(const library_t *lib);
/* How the last scan listed the music folder: 1 the quick FAT listing, 0 the
 * slow walk through the kernel. */
int library_scan_quick(const library_t *lib);
/* How many music files' names the last scan held in memory: all of them on a
 * first scan, only the new and changed ones afterwards (the rest are ticked
 * off against a table of the library's files as the folder is listed). */
int library_scan_held(const library_t *lib);
/* (tests) 0: every file held by name, as scans did before */
void library_test_known(int on);
/* How the last scan rebuilt albums, artists and folders: 0 not at all
 * (nothing changed), 1 only where songs changed, 2 all of them. */
int library_scan_groups(const library_t *lib);
/* tests: albums, artists and folders rebuilt from every song, as a scan's
 * full rebuild does */
void library_test_rebuild_groups(library_t *lib);
/* tests: the groups rebuilt a statement a track for everything, as before the
 * shortcuts that leave out what the track before has written (1), or with them (0) */
void library_test_groups_plain(int on);
/* (tests) 0: a first scan makes its groups afterwards, all at once, as it did before (1: as each song is added) */
void library_test_groups_direct(int on);
/* tests: what reads the FAT (NULL: fatwalk(), fatwalk.h) */
typedef int (*lib_fatwalk_fn)(const char *dev, const char *mount, const char *rel,
                              int (*cb)(const char *path, int64_t mtime, int64_t size, void *ctx), void *ctx);
void library_test_fatwalk(lib_fatwalk_fn fn);
/* tests: from how many files a scan of an empty library loads in bulk (0: the usual 1000) */
void library_test_bulk_min(int files);
/* Stop a scan running on this library (from another thread): it returns -1.
 * Nothing is dropped; the tracks it had read are kept, and the next scan
 * puts them in their albums, playlists and search. */
void library_scan_cancel(library_t *lib);

int  library_list(library_t *lib, lib_view_t view, int64_t parent, lib_list_t *out);
void library_list_free(lib_list_t *l);
/* Smart playlists (LIB_SMART's parent): lists Fathom keeps itself.
 *   Never played          never played nor skipped, by album
 *   On repeat             played 2 times or more in the last 30 days (the play log), the most first; 50
 *   Not heard in a while  played, but not in the last 90 days, longest ago first; 200
 *   Forgotten favourites  favourites not played in the last 60 days, longest ago first; 100
 *   Often skipped         skipped early 2 times or more, the most first; 100 */
enum { LIB_SMART_NEVER = 1, LIB_SMART_REPEAT, LIB_SMART_NOT_HEARD, LIB_SMART_FORGOTTEN, LIB_SMART_SKIPPED,
       LIB_SMART_N };
const char *library_smart_name(int which);

/* An artist's songs on one album (LIB_CREDIT_TRACKS), by disc and track. */
int  library_list_credit_tracks(library_t *lib, int64_t artist_id, int64_t album_id, lib_list_t *out);
/* An artist's songs, album by album: credited, every song they're credited on
 * (Artists); else every song on the albums they're the album artist of
 * (Album artists). Returns how many, or -1. */
int  library_artist_tracks(library_t *lib, int64_t artist_id, int credited, lib_list_t *out);
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
/* Albums whose cover no scan has looked at: since covers could come from more
 * places (FLAC pictures of any type, other tracks, a disc's album folder, big
 * images), or at all (a scan's cover pass cut short by a stop or a restart,
 * which goes through albums in the order they were added: the last ones were
 * left without, until something else made a scan run). One background scan
 * finds theirs. */
int  library_needs_art(library_t *lib);
/* Cover files from before the covers' store (0.15.1 and older: a file per
 * album and size) are still in the art folder: one background scan moves them
 * into it. They're read from where they are meanwhile. */
int  library_art_legacy(library_t *lib);
/* The last scan stopped before it was done (the H2 switched off, USB drive
 * mode, a restart): tracks it had read may have no album, artist or search
 * entry yet, and files it hadn't reached are missing. One background scan
 * finishes it. */
int  library_scan_unfinished(library_t *lib);
/* After a stop that wasn't clean (a power cut): library_check() reads the
 * whole database through (SQLite's quick_check: seconds; a background
 * connection's job): 0 sound, -1 damaged. library_scan_distrust() makes the
 * next scan compare every file again, not trust the last one's word that
 * nothing changed. library_import_counts() brings plays, skips, favourites,
 * resume places and the play log over from another library file (one set
 * aside as damaged), by path, as far as it can still be read: how many songs
 * got some, -1 if it can't be read at all. */
int  library_check(library_t *lib);
void library_scan_distrust(library_t *lib);
int  library_import_counts(library_t *lib, const char *old_db);
/* No songs at all (a fresh install, or nothing copied yet). */
int  library_empty(library_t *lib);
/* How many songs a smart playlist has (what library_list(LIB_SMART, which)
 * would list), counted without listing them: Playlists' counts. */
int  library_smart_count(library_t *lib, int which);
/* VACUUM: the database rewritten tight (a first scan leaves it fragmented,
 * which makes reading it about half as fast). Takes the write lock. It needs
 * about twice the database free on the card (SQLite's temporary copy, in
 * SQLITE_TMPDIR, and the write-ahead log, emptied afterwards): 0 done, -2 not
 * enough room (nothing done), -1 failed. */
int  library_compact(library_t *lib);
/* Each query a person waits on (the lists, smart playlists, Shuffle all, the
 * start's checks), its plan looked at: how many read the whole tracks table or
 * sort its rows, beyond the few allowed; each one found (allowed ones too) to
 * report, if not NULL. test_scan keeps it at 0. */
int  library_check_plans(library_t *lib, FILE *report);
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

/* Covers found online (net/coverfind.h), kept on the card in their own folder
 * (fathom/covers), never beside the music: an album's as KEYHASH.jpg, with
 * KEYHASH.over when it was picked over the album's own cover (it stays ahead
 * of it); KEYHASH.none when one that filled a gap was removed (not looked for
 * again unless asked). Set once, for every library handle. */
void library_set_found_covers(const char *dir);
/* Where album `key`'s is: ext "jpg", "over" or "none". 0, or -1 (no folder set). */
int  library_found_cover_path(const char *key, const char *ext, char *out, size_t cap);
/* Where an album's cover came from */
enum { LIB_COVER_UNKNOWN, LIB_COVER_OWN, LIB_COVER_NONE, LIB_COVER_FOUND, LIB_COVER_PICKED };
int  library_album_cover(library_t *lib, int64_t album_id);
/* An album as a cover search needs it */
typedef struct {
    int64_t id;
    char key[512];
    char title[256], artist[256];   /* its album artist ("Various Artists": a compilation) */
    int year;                       /* 0: unknown */
} lib_album_t;
int  library_album_info(library_t *lib, int64_t album_id, lib_album_t *out);
/* Every album no cover was found for (malloc'd into *out): how many, or -1. */
int  library_albums_without_cover(library_t *lib, lib_album_t **out);
/* A found cover (its file already in the folder) as the album's: its
 * thumbnails (LIB_ART_LARGE and LIB_ART_SMALL squares, RGB565) written. Over
 * a cover of its own (or one picked before): picked, ahead of its own from
 * now on; otherwise found, filling the gap. 0, -1. */
int  library_store_found_cover(library_t *lib, int64_t album_id, const uint16_t *large, const uint16_t *small);
/* The album's found cover removed: one picked over its own goes back to its
 * own; one that filled a gap leaves no cover (and is marked: KEYHASH.none). 0, -1. */
int  library_remove_found_cover(library_t *lib, int64_t album_id);
/* A track's album (its cover in Songs): the id, 0 if unknown. */
int64_t library_track_album(library_t *lib, int64_t track_id);
/* An image file's contents (JPEG, PNG) as a square cover, centre-cropped, as
 * RGB565 (size*size*2 bytes): a radio station's. 0 if decoded. */
int library_art_decode(const unsigned char *enc, size_t len, int size, uint16_t *pixels);
/* A radio station's logo (JPEG, PNG) at both sizes, whole inside each square
 * with white around it and under its transparent parts, not cropped: small
 * (LIB_ART_SMALL) always, large (LIB_ART_LARGE) too unless it's NULL.
 * *short_side (may be NULL): the picture's shorter side as decoded, which is
 * its own when that's under LIB_ART_LARGE * 3 / 2 (a bigger one is shrunk no
 * further than that), so the caller can tell one too small to show large.
 * Stricter limits than a cover's: 512 KB, 400,000 pixels decoded. 0 if decoded. */
int library_logo_decode(const unsigned char *enc, size_t len, uint16_t *large, uint16_t *small, int *short_side);
/* The same from an image file, at both sizes (LIB_ART_LARGE and LIB_ART_SMALL
 * squares): read from where it is, a JPEG or a PNG of any size without its
 * encoded bytes held in memory (a podcast's cover). 0 if decoded. */
int library_art_decode_file(const char *path, uint16_t *large, uint16_t *small);

/* All tracks in shuffle form (ids in shuf_track_t.id). Caller frees *out. */
int library_shuffle_tracks(library_t *lib, shuf_track_t **out);
/* The n tracks ids[] in shuffle form, out[i] for ids[i] (one that's gone: its
 * id and nothing else): for shuffling an album or a playlist without reading
 * every song of the library. Works are found among these songs. n, or -1. */
int library_shuffle_tracks_for(library_t *lib, const int64_t *ids, int n, shuf_track_t *out);
/* The highest track id (about how many songs there are, at no cost: no count). 0: none. */
int64_t library_top_track(library_t *lib);
/* The track with this id, or the next one there is after it (0: none): a song taken at random. */
int64_t library_track_from(library_t *lib, int64_t id);
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
