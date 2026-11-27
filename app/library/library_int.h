/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* What the library's own files share (library.c and the library_*.c cut out
 * of it): the library itself, and the small helpers they all use. Nothing
 * outside app/library includes this: library.h is the library's face. */
#ifndef FATHOM_LIBRARY_INT_H
#define FATHOM_LIBRARY_INT_H

#include <sqlite3.h>

#include "library/artdec.h"
#include "library/library.h"

struct library {
    sqlite3 *db;
    void *search_run;       /* the search under way (search_keep) */
    int no_album_artists;   /* album rows show the year, not the artist (library_set_album_artists) */
    char art_dir[1024];
    sqlite3_stmt *row_album, *row_artist, *row_credited, *row_track, *track_full, *album_name, *artist_name;
    sqlite3_stmt *row_playlist, *row_pl_track, *row_plays, *track_album, *playlist_name;
    sqlite3_stmt *row_genre, *row_composer, *row_year, *row_folder, *genre_name, *composer_name, *folder_name;
    sqlite3_stmt *artist_counts, *album_info, *unread_at;   /* (asked per row drawn: not compiled each time) */
    char pl_dir[1024];      /* your own playlists ("" = none) */
    int scan_changes;       /* tracks the last scan added, changed or dropped */
    volatile int cancel;    /* library_scan_cancel(): stop the scan in progress */
    char fw_dev[64], fw_mount[256];   /* library_set_fast_walk(): the music folder's FAT partition */
    int art_fd[2];          /* the covers' store: large.pack, small.pack (-1: not open) */
    int art_legacy;         /* a cover file per album and size (before the store) may still be in art_dir */
    int quick;              /* the last scan's listing was the fast walk's */
    int scan_held;          /* files whose names the last scan held in memory (library_scan_held) */
    int plays_trimmed;      /* the play log's year-old entries removed since it was opened (library_played) */
    int fw_bad;             /* ...and named a new file that isn't there: the scan stops, and walks the folder instead */
    int bulk;               /* a first scan in progress with the tracks' indexes set aside (bulk_begin) */
    int defer_art;          /* library_scan_defer_art: the next scan leaves the covers to the one after */
    int art_later;          /* this scan left a cover unmade for want of memory: the next looks again */
    int retry_unread;       /* library_scan_retry_unread: the next scan reads again the files none could read */
    int groups_how;         /* library_scan_groups() */
    char db_path[1024];     /* the database's file, as opened (library_played_later opens it again) */
    int plays_conn;         /* this connection is the plays' own (PW.conn) */
};

/* user_version: what the database has been brought up to. 5: every column,
 * table and index below is there and the one-off upgrades are done, so
 * later opens (a background scan's, levelling's: each a connection of its
 * own) skip them; they read the tracks table three times over and took the
 * write lock, waiting behind a scan's. 6: the play log (plays). 7: an index for
 * each query a person waits on (the lists, smart playlists, Shuffle all), so
 * none reads the whole tracks table: about 2 s for 50,000 songs on the H2.
 * 8: an artist's songs found from an index (the groups rebuilt only where
 * songs changed). 9: the search keys (name|artist|album, where it was all
 * their words in a row) and the index a search reads the songs' from. */
#define LIB_SCHEMA_V 9
#define LIB_STR_(x) #x
#define LIB_STR(x) LIB_STR_(x)

/* Queries a person waits on, kept here once: what runs is what
 * library_check_plans checks the plan of. */
#define SHUF_COLS "id, artist_id, album_id, disc, track, duration_ms, last_played, play_count, skip_count, genre, title"
#define SHUFFLE_ALL_SQL "SELECT " SHUF_COLS " FROM tracks ORDER BY album_id, disc, track"
#define NEVER_PLAYED_COUNT_SQL "SELECT COUNT(*) FROM tracks WHERE play_count=0 AND skip_count=0"
/* (tags_v<2 first: its index finds the few, or none, at once) */
#define NEEDS_TAGS_SQL "SELECT EXISTS(SELECT 1 FROM tracks WHERE tags_v<2 AND (tags_v<1 OR (cue=0 AND" \
                       " (path LIKE '%.flac' OR path LIKE '%.wv' OR path LIKE '%.ape'))))"
#define NO_SONGS_SQL "SELECT NOT EXISTS(SELECT 1 FROM tracks)"

/* ---- library.c: statements and small things ---- */
sqlite3_stmt *prep(sqlite3 *db, const char *sql);
int64_t db_int(sqlite3 *db, const char *sql, int64_t bind, int64_t otherwise);
int64_t get_int_col(library_t *lib, const char *sql, int64_t id);
int set_int_col(library_t *lib, const char *sql, int64_t id, int64_t v);
int db_run_id(sqlite3 *db, const char *sql, int64_t id);
void copy_col(sqlite3_stmt *s, int col, char *out, size_t cap);
void *vec_room(void *v, int n, int *cap, int first, size_t size);
int dir_of(const char *path, char *dir, size_t cap);
int db_commit(sqlite3 *db);
void reset_all(sqlite3 *db);                   /* (every statement reset: only where none is being stepped) */
void sort_key(const char *in, char *out, size_t cap);              /* (how names sort) */
void playlist_sort_key(const char *path, char *sort, size_t cap);
int lock_boost(void);                          /* (while a write holds the lock: normal priority) */
void lock_unboost(int n);

/* ---- library_browse.c ---- */
const char *list_sql(lib_view_t view, int64_t parent, int64_t *cut);   /* (a view's query) */

/* ---- library_pixels.c ---- */
extern const art_limits_t ART_LIMITS;          /* what a cover is decoded within */
int art_square(const unsigned char *img, int w, int h, int size, uint16_t *pixels);

/* ---- library_plays.c ---- */
void plays_release(const library_t *lib);      /* (a library being closed: its plays are in first) */

/* ---- library_search.c ---- */
/* A song's key and an artist's, as SQL (an album's: index_search). A song's
 * artist: its own and its album's, if that's another. */
#define SKEY_TRACK(title, artist, album_artist, album) \
    "skey(" title ", coalesce(" artist ",'')||CASE WHEN " album_artist " IS " artist " THEN '' ELSE ' '||coalesce(" \
    album_artist ",'') END, " album ")"
#define SKEY_ARTIST "skey(name, NULL, NULL)"
void search_register(library_t *lib);
void index_search(library_t *lib, int only_missing);
const char *search_sql(const char *table, const char *where, char *sql, size_t cap);

/* ---- library_trying.c ---- */
int trying_left(const char *what, char *out, size_t cap);

#endif
