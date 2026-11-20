/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE
#define _GNU_SOURCE   /* strcasestr (glibc, musl) */
#include "library.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <sqlite3.h>

#include "core/lang.h"
#include "library/artdec.h"
#include "library/cue.h"
#include "library/fatwalk.h"
#include "library/keys.h"
#include "library/playlist.h"
#include "library/tags.h"

#ifndef FATHOM_SQLITE_CACHE
#define FATHOM_SQLITE_CACHE "-2000"   /* KiB; the H2 build uses less */
#endif

#ifndef FATHOM_JPEG_MAX_MEMORY
#define FATHOM_JPEG_MAX_MEMORY (64L * 1024 * 1024)   /* desktop; the H2 build uses 4 MB */
#endif

#ifndef FATHOM_ART_MAX_FILE
#define FATHOM_ART_MAX_FILE (64L * 1024 * 1024)   /* encoded bytes held in RAM; the H2 uses 4 MB */
#endif

#ifndef FATHOM_ART_MAX_PIXELS
#define FATHOM_ART_MAX_PIXELS (16LL * 1024 * 1024)   /* desktop; the H2 build sets ~2.4 MP */
#endif

struct library {
    sqlite3 *db;
    int no_album_artists;   /* album rows show the year, not the artist (library_set_album_artists) */
    char art_dir[1024];
    sqlite3_stmt *row_album, *row_artist, *row_credited, *row_track, *track_full, *album_name, *artist_name;
    sqlite3_stmt *row_playlist, *row_pl_track, *row_plays, *track_album, *playlist_name;
    sqlite3_stmt *row_genre, *row_composer, *row_year, *row_folder, *genre_name, *composer_name, *folder_name;
    char pl_dir[1024];      /* your own playlists ("" = none) */
    int scan_changes;       /* tracks the last scan added, changed or dropped */
    volatile int cancel;    /* library_scan_cancel(): stop the scan in progress */
    char fw_dev[64], fw_mount[256];   /* library_set_fast_walk(): the music folder's FAT partition */
};

/* Every connection's own settings. */
static const char *PRAGMAS =
    "PRAGMA journal_mode=WAL;"
    /* WAL with NORMAL: no flush to the card at every commit (a play count, a
     * favourite); a power cut can lose the last commits, never the database */
    "PRAGMA synchronous=NORMAL;"
    "PRAGMA cache_size=" FATHOM_SQLITE_CACHE ";";

/* The first schema; what came later is added by migrate(). */
static const char *SCHEMA =
    "CREATE TABLE IF NOT EXISTS tracks("
    " id INTEGER PRIMARY KEY, path TEXT UNIQUE NOT NULL, mtime INTEGER, size INTEGER,"
    " title TEXT, artist TEXT, album_artist TEXT, album TEXT, genre TEXT,"
    " year INTEGER, disc INTEGER, track INTEGER, codec TEXT, rate INTEGER, bits INTEGER,"
    " channels INTEGER, duration_ms INTEGER, has_pic INTEGER,"
    " album_id INTEGER, artist_id INTEGER,"
    " last_played INTEGER DEFAULT 0, play_count INTEGER DEFAULT 0, skip_count INTEGER DEFAULT 0,"
    " seen INTEGER DEFAULT 1, favourite INTEGER DEFAULT 0, resume_ms INTEGER DEFAULT 0);"
    "CREATE TABLE IF NOT EXISTS albums("
    " id INTEGER PRIMARY KEY, key TEXT UNIQUE NOT NULL, title TEXT, sort TEXT,"
    " artist_id INTEGER, year INTEGER, dir TEXT, first_track INTEGER, ntracks INTEGER,"
    " art INTEGER DEFAULT 0);"
    "CREATE TABLE IF NOT EXISTS artists("
    " id INTEGER PRIMARY KEY, key TEXT UNIQUE NOT NULL, name TEXT, sort TEXT, nalbums INTEGER DEFAULT 0);"
    "CREATE TABLE IF NOT EXISTS credits("
    " artist_id INTEGER NOT NULL, track_id INTEGER NOT NULL, PRIMARY KEY(artist_id, track_id));"
    "CREATE INDEX IF NOT EXISTS credits_track ON credits(track_id);"
    "CREATE TABLE IF NOT EXISTS playlists("
    " id INTEGER PRIMARY KEY, path TEXT UNIQUE NOT NULL, name TEXT, sort TEXT,"
    " ntracks INTEGER DEFAULT 0, nmissing INTEGER DEFAULT 0, seen INTEGER DEFAULT 1);"
    "CREATE TABLE IF NOT EXISTS playlist_items("
    " playlist_id INTEGER NOT NULL, pos INTEGER NOT NULL, track_id INTEGER NOT NULL,"
    " PRIMARY KEY(playlist_id, pos));"
    "CREATE INDEX IF NOT EXISTS tracks_album ON tracks(album_id, disc, track);"
    "CREATE INDEX IF NOT EXISTS tracks_title ON tracks(title COLLATE NOCASE);"
    "CREATE INDEX IF NOT EXISTS albums_sort ON albums(sort);"
    "CREATE INDEX IF NOT EXISTS albums_artist ON albums(artist_id, year);";

static void rebuild_groups(library_t *lib);
static void art_path(const library_t *lib, int64_t album_id, int size, char *out, size_t cap);
static double now_s(void);

static sqlite3_stmt *prep(sqlite3 *db, const char *sql)
{
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK) {
        fprintf(stderr, "library: %s\n  in: %s\n", sqlite3_errmsg(db), sql);
        return NULL;
    }
    return s;
}

static void copy_col(sqlite3_stmt *s, int col, char *out, size_t cap)
{
    const unsigned char *t = sqlite3_column_text(s, col);
    snprintf(out, cap, "%s", t ? (const char *)t : "");
}

/* SQL fold(text): words for search (text_search_fold). */
static void sql_sortkey(sqlite3_context *ctx, int argc, sqlite3_value **argv);

static void sql_fold(sqlite3_context *ctx, int argc, sqlite3_value **argv)
{
    (void)argc;
    const unsigned char *in = sqlite3_value_text(argv[0]);
    char out[1024];
    text_search_fold(in ? (const char *)in : "", out, sizeof out);
    sqlite3_result_text(ctx, out, -1, SQLITE_TRANSIENT);
}

/* Fill the search columns (only the empty ones, or all after a scan). */
static void index_search(library_t *lib, int only_missing)
{
    const char *w = only_missing ? " WHERE search IS NULL" : "";
    char sql[600];
    snprintf(sql, sizeof sql,
             "UPDATE tracks SET search=fold(coalesce(title,'')||' '||coalesce(artist,'')||' '||"
             "coalesce(album_artist,'')||' '||coalesce(album,''))%s;", w);
    sqlite3_exec(lib->db, sql, NULL, NULL, NULL);
    snprintf(sql, sizeof sql,
             "UPDATE albums SET search=fold(coalesce(title,'')||' '||"
             "coalesce((SELECT name FROM artists a WHERE a.id=albums.artist_id),''))%s;", w);
    sqlite3_exec(lib->db, sql, NULL, NULL, NULL);
    snprintf(sql, sizeof sql, "UPDATE artists SET search=fold(coalesce(name,''))%s;", w);
    sqlite3_exec(lib->db, sql, NULL, NULL, NULL);
}

/* user_version: what the database has been brought up to. 5: every column,
 * table and index below is there and the one-off upgrades are done, so
 * later opens (a background scan's, levelling's: each a connection of its
 * own) skip them; they read the tracks table three times over and took the
 * write lock, waiting behind a scan's. */
#define LIB_SCHEMA_V 5
#define LIB_STR_(x) #x
#define LIB_STR(x) LIB_STR_(x)

/* Bring a database from `version` up to LIB_SCHEMA_V. */
static void migrate(library_t *lib, int version)
{
    /* columns added after the first release of the schema (errors: already there) */
    sqlite3_exec(lib->db, "ALTER TABLE tracks ADD COLUMN favourite INTEGER DEFAULT 0", NULL, NULL, NULL);
    sqlite3_exec(lib->db, "ALTER TABLE tracks ADD COLUMN resume_ms INTEGER DEFAULT 0", NULL, NULL, NULL);
    sqlite3_exec(lib->db, "ALTER TABLE playlists ADD COLUMN writable INTEGER DEFAULT 0", NULL, NULL, NULL);
    sqlite3_exec(lib->db, "ALTER TABLE playlists ADD COLUMN mtime INTEGER", NULL, NULL, NULL);   /* (its file's, when read) */
    sqlite3_exec(lib->db, "ALTER TABLE playlists ADD COLUMN size INTEGER", NULL, NULL, NULL);
    sqlite3_exec(lib->db, "ALTER TABLE tracks ADD COLUMN search TEXT", NULL, NULL, NULL);
    sqlite3_exec(lib->db, "ALTER TABLE albums ADD COLUMN search TEXT", NULL, NULL, NULL);
    sqlite3_exec(lib->db, "ALTER TABLE albums ADD COLUMN art_sig INTEGER DEFAULT 0", NULL, NULL, NULL);
    sqlite3_exec(lib->db, "ALTER TABLE artists ADD COLUMN search TEXT", NULL, NULL, NULL);
    /* volume levelling: rg_state 0 = not done yet, 1 = from tags, 2 = measured, 3 = couldn't */
    sqlite3_exec(lib->db, "ALTER TABLE tracks ADD COLUMN rg_state INTEGER DEFAULT 0", NULL, NULL, NULL);
    sqlite3_exec(lib->db, "ALTER TABLE tracks ADD COLUMN rg_track REAL", NULL, NULL, NULL);
    sqlite3_exec(lib->db, "ALTER TABLE tracks ADD COLUMN rg_track_peak REAL", NULL, NULL, NULL);
    sqlite3_exec(lib->db, "ALTER TABLE tracks ADD COLUMN rg_album REAL", NULL, NULL, NULL);
    sqlite3_exec(lib->db, "ALTER TABLE tracks ADD COLUMN rg_album_peak REAL", NULL, NULL, NULL);
    /* albums each artist is credited on (Artists); 0 = an album artist only */
    int had_credits = sqlite3_exec(lib->db, "ALTER TABLE artists ADD COLUMN ncredit_albums INTEGER DEFAULT 0",
                                   NULL, NULL, NULL) != SQLITE_OK;
    /* Songs sort by songkey(title) (leading punctuation skipped): a column, filled
     * here for libraries from before it and after each scan that changes tracks.
     * Albums from before punctuation was skipped get their new sort too. */
    sqlite3_exec(lib->db, "ALTER TABLE tracks ADD COLUMN sort TEXT", NULL, NULL, NULL);
    sqlite3_exec(lib->db,
        "UPDATE tracks SET sort=songkey(COALESCE(title,'')) WHERE sort IS NULL;"
        "UPDATE albums SET sort=sortkey(COALESCE(title,'')) WHERE sort IS NOT sortkey(COALESCE(title,''));",
        NULL, NULL, NULL);
    /* Genres, Composers, Years, Folders: their groups, rebuilt with the others */
    sqlite3_exec(lib->db,
        "CREATE TABLE IF NOT EXISTS genres(id INTEGER PRIMARY KEY, name TEXT UNIQUE COLLATE NOCASE, sort TEXT,"
        " nalbums INTEGER DEFAULT 0);"
        "CREATE TABLE IF NOT EXISTS composers(id INTEGER PRIMARY KEY, name TEXT UNIQUE COLLATE NOCASE, sort TEXT,"
        " nalbums INTEGER DEFAULT 0);"
        "CREATE TABLE IF NOT EXISTS folders(id INTEGER PRIMARY KEY, path TEXT UNIQUE NOT NULL, parent INTEGER,"
        " name TEXT, sort TEXT, ntracks INTEGER DEFAULT 0, seen INTEGER DEFAULT 1);"
        "CREATE INDEX IF NOT EXISTS folders_parent ON folders(parent, sort);",
        NULL, NULL, NULL);
    sqlite3_exec(lib->db, "ALTER TABLE tracks ADD COLUMN genre_id INTEGER", NULL, NULL, NULL);
    sqlite3_exec(lib->db, "ALTER TABLE tracks ADD COLUMN composer_id INTEGER", NULL, NULL, NULL);
    sqlite3_exec(lib->db, "ALTER TABLE tracks ADD COLUMN folder_id INTEGER", NULL, NULL, NULL);
    sqlite3_exec(lib->db, "CREATE INDEX IF NOT EXISTS tracks_folder ON tracks(folder_id)", NULL, NULL, NULL);
    sqlite3_exec(lib->db, "CREATE INDEX IF NOT EXISTS tracks_genre ON tracks(genre_id)", NULL, NULL, NULL);
    sqlite3_exec(lib->db, "CREATE INDEX IF NOT EXISTS tracks_composer ON tracks(composer_id)", NULL, NULL, NULL);
    /* tags_v: which of the tags read later than the first schema a track has
     * (1: the composer; 2: and a CUE sheet looked for in them, which only
     * FLAC, WavPack and APE files are read again for); the next scan reads
     * them from the files that lack them, changing nothing else (not the
     * measured volume levelling) */
    sqlite3_exec(lib->db, "ALTER TABLE tracks ADD COLUMN tags_v INTEGER DEFAULT 0", NULL, NULL, NULL);
    /* CUE sheets (cue.h): a track's number in its file's sheet (0: a file of
     * its own); `cues` holds the files that aren't tracks themselves, so a
     * scan can tell nothing changed without reading them: every .cue found
     * (kind 0; nfiles: how many of its files it splits), and each music file
     * split into tracks by a .cue (kind 1, `sheet` its path) or by the sheet
     * in its own tags (kind 2); and a music file whose tags can't be read
     * (kind 3: not read again while it's the same, and the fast walk still
     * finds everything it lists in the library). */
    sqlite3_exec(lib->db, "ALTER TABLE tracks ADD COLUMN cue INTEGER DEFAULT 0", NULL, NULL, NULL);
    sqlite3_exec(lib->db,
        "CREATE TABLE IF NOT EXISTS cues(path TEXT PRIMARY KEY, mtime INTEGER, size INTEGER, kind INTEGER,"
        " sheet TEXT, nfiles INTEGER DEFAULT 0, seen INTEGER DEFAULT 1);"
        "CREATE INDEX IF NOT EXISTS cues_sheet ON cues(sheet);",
        NULL, NULL, NULL);
    int had_composer = sqlite3_exec(lib->db, "ALTER TABLE tracks ADD COLUMN composer TEXT", NULL, NULL, NULL) != SQLITE_OK;
    /* Groups worked out again once (no rescan) for a library from before
     * Genres, Folders... (no composer column), before credits, or before
     * album keys took the album artist's key (user_version 2: "X feat. Y" and
     * "The X" no longer split an album), or before names followed their tags'
     * spelling (4) */
    if (!had_composer || !had_credits || version < 4) {   /* (4: names spelled as tagged now) */
        sqlite3_exec(lib->db, "BEGIN", NULL, NULL, NULL);
        rebuild_groups(lib);
        sqlite3_exec(lib->db, "PRAGMA user_version=4; COMMIT", NULL, NULL, NULL);
    }
    /* 3: covers' signatures cover found covers too, and images' dates: noted
     * afresh at the next scan (a found cover kept as it is) */
    if (version < 3) sqlite3_exec(lib->db, "UPDATE albums SET art_sig=0", NULL, NULL, NULL);
    index_search(lib, 1);
    /* 5: levelling's next track found by an index (a pass over the library
     * re-read every track measured so far, for each one), and a scan's note
     * of work it left (meta: library_scan) */
    sqlite3_exec(lib->db,
        "CREATE INDEX IF NOT EXISTS tracks_rg_todo ON tracks(id) WHERE rg_state=0;"
        "CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY, value INTEGER);",
        NULL, NULL, NULL);
    sqlite3_exec(lib->db, "PRAGMA user_version=" LIB_STR(LIB_SCHEMA_V), NULL, NULL, NULL);
}

library_t *library_open(const char *db_path, const char *art_dir)
{
    library_t *lib = calloc(1, sizeof *lib);
    if (!lib) return NULL;
    snprintf(lib->art_dir, sizeof lib->art_dir, "%s", art_dir);
    mkdir(art_dir, 0755);
    char *err = NULL;
    int rc = sqlite3_open(db_path, &lib->db);
    /* another connection (a background scan) may be writing: wait for it */
    if (rc == SQLITE_OK) sqlite3_busy_timeout(lib->db, 10000);
    if (rc != SQLITE_OK || sqlite3_exec(lib->db, PRAGMAS, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "library: cannot open %s: %s\n", db_path, err ? err : sqlite3_errmsg(lib->db));
        sqlite3_free(err);
        library_close(lib);
        return NULL;
    }
    sqlite3_create_function(lib->db, "fold", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, NULL, sql_fold, NULL, NULL);
    /* how names sort: albums (without "the"), and songs (as titled) */
    static int strip_the = 1;
    sqlite3_create_function(lib->db, "sortkey", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, &strip_the, sql_sortkey, NULL, NULL);
    sqlite3_create_function(lib->db, "songkey", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, NULL, sql_sortkey, NULL, NULL);
    int version = 0;
    sqlite3_stmt *v = prep(lib->db, "PRAGMA user_version");
    if (v && sqlite3_step(v) == SQLITE_ROW) version = sqlite3_column_int(v, 0);
    sqlite3_finalize(v);
    if (version < LIB_SCHEMA_V) {
        if (sqlite3_exec(lib->db, SCHEMA, NULL, NULL, &err) != SQLITE_OK) {
            fprintf(stderr, "library: cannot open %s: %s\n", db_path, err ? err : sqlite3_errmsg(lib->db));
            sqlite3_free(err);
            library_close(lib);
            return NULL;
        }
        migrate(lib, version);
    }
    lib->row_album = prep(lib->db, "SELECT al.title, al.year, ar.name FROM albums al"
                                    " LEFT JOIN artists ar ON ar.id=al.artist_id WHERE al.id=?");
    lib->row_artist = prep(lib->db, "SELECT name, nalbums FROM artists WHERE id=?");
    lib->row_credited = prep(lib->db, "SELECT name, ncredit_albums FROM artists WHERE id=?");
    lib->row_track = prep(lib->db, "SELECT title, duration_ms FROM tracks WHERE id=?");
    lib->track_full = prep(lib->db,
        "SELECT t.path, t.title, t.artist, al.title, t.album_id, t.artist_id, t.disc, t.track, t.year,"
        " t.codec, t.rate, t.bits, t.channels, t.duration_ms"
        " FROM tracks t LEFT JOIN albums al ON al.id=t.album_id WHERE t.id=?");
    lib->album_name = prep(lib->db, "SELECT title FROM albums WHERE id=?");
    lib->artist_name = prep(lib->db, "SELECT name FROM artists WHERE id=?");
    lib->row_playlist = prep(lib->db, "SELECT name, ntracks FROM playlists WHERE id=?");
    lib->row_pl_track = prep(lib->db, "SELECT title, duration_ms, artist FROM tracks WHERE id=?");
    lib->row_plays = prep(lib->db, "SELECT title, play_count, artist FROM tracks WHERE id=?");   /* (Most played) */
    lib->track_album = prep(lib->db, "SELECT album_id FROM tracks WHERE id=?");
    lib->playlist_name = prep(lib->db, "SELECT name FROM playlists WHERE id=?");
    lib->row_genre = prep(lib->db, "SELECT name, nalbums FROM genres WHERE id=?");
    lib->row_composer = prep(lib->db, "SELECT name, nalbums FROM composers WHERE id=?");
    lib->row_year = prep(lib->db, "SELECT ?1, COUNT(*) FROM albums WHERE year=?1");
    lib->row_folder = prep(lib->db, "SELECT name, ntracks FROM folders WHERE id=?");
    lib->genre_name = prep(lib->db, "SELECT name FROM genres WHERE id=?");
    lib->composer_name = prep(lib->db, "SELECT name FROM composers WHERE id=?");
    lib->folder_name = prep(lib->db, "SELECT name FROM folders WHERE id=?");
    if (!lib->row_playlist || !lib->row_pl_track || !lib->row_plays || !lib->track_album || !lib->playlist_name ||
        !lib->row_album || !lib->row_artist || !lib->row_credited || !lib->row_track || !lib->track_full ||
        !lib->album_name || !lib->artist_name || !lib->row_genre || !lib->row_composer || !lib->row_year ||
        !lib->row_folder || !lib->genre_name || !lib->composer_name || !lib->folder_name) {
        library_close(lib);
        return NULL;
    }
    return lib;
}

void library_release_memory(library_t *lib)
{
    if (lib) sqlite3_db_release_memory(lib->db);
}

void library_close(library_t *lib)
{
    if (!lib) return;
    sqlite3_finalize(lib->row_album);
    sqlite3_finalize(lib->row_artist);
    sqlite3_finalize(lib->row_genre);
    sqlite3_finalize(lib->row_composer);
    sqlite3_finalize(lib->row_year);
    sqlite3_finalize(lib->row_folder);
    sqlite3_finalize(lib->genre_name);
    sqlite3_finalize(lib->composer_name);
    sqlite3_finalize(lib->folder_name);
    sqlite3_finalize(lib->row_credited);
    sqlite3_finalize(lib->row_track);
    sqlite3_finalize(lib->track_full);
    sqlite3_finalize(lib->album_name);
    sqlite3_finalize(lib->artist_name);
    sqlite3_finalize(lib->row_playlist);
    sqlite3_finalize(lib->row_pl_track);
    sqlite3_finalize(lib->row_plays);
    sqlite3_finalize(lib->track_album);
    sqlite3_finalize(lib->playlist_name);
    sqlite3_close(lib->db);
    free(lib);
}

/* ---------- scanning ---------- */

/* Files found by the walk, with the date and size it saw (one lookup each:
 * on the card's FAT, looking a file up again by path costs as much as the
 * whole walk). */
typedef struct {
    char *p;
    int64_t mtime, size;
    int cue;   /* music: the .cue (its index in the sheets' list) splitting it, or -1;
                * a .cue: 1 if it changed since the last scan */
    int want;  /* a .cue: how many files it splits (cues.nfiles) */
    unsigned char kept;   /* music: its row in `cues` stays; a playlist: its rows stay */
    unsigned char put;    /* a .cue: its row in `cues` is written at the end */
} pent_t;
/* failed: a file or folder the walk couldn't note (no memory, a folder that
 * can't be listed): the list isn't the whole folder, so nothing is taken
 * for deleted */
typedef struct { pent_t *v; int n, cap, failed; } pathvec_t;

static int pv_push(pathvec_t *pv, const char *p, const struct stat *st)
{
    if (pv->n == pv->cap) {
        int nc = pv->cap ? pv->cap * 2 : 1024;
        pent_t *nv = realloc(pv->v, (size_t)nc * sizeof *nv);
        if (!nv) { pv->failed = 1; return -1; }
        pv->v = nv;
        pv->cap = nc;
    }
    pent_t *e = &pv->v[pv->n];
    memset(e, 0, sizeof *e);
    e->p = strdup(p);
    e->mtime = st ? (int64_t)st->st_mtime : 0;
    e->size = st ? (int64_t)st->st_size : 0;
    e->cue = -1;
    if (!e->p) { pv->failed = 1; return -1; }
    pv->n++;
    return 0;
}

/* A cover image beside the music (the walk notes them: build_art) */
static int art_image(const char *path)
{
    const char *dot = strrchr(path, '.'), *slash = strrchr(path, '/');
    if (!dot || (slash && dot < slash) || (slash ? slash[1] : path[0]) == '.') return 0;
    return !strcasecmp(dot, ".jpg") || !strcasecmp(dot, ".jpeg") || !strcasecmp(dot, ".png");
}

static void pv_free(pathvec_t *pv)
{
    for (int i = 0; i < pv->n; i++) free(pv->v[i].p);
    free(pv->v);
    memset(pv, 0, sizeof *pv);
}

/* The tracks a scan found, by id (a bit each): those left out at the end
 * are gone. Kept here, not in a column: marking every row seen, then
 * unseen, rewrote the whole table at every scan. */
typedef struct { unsigned char *bits; size_t cap; int failed; } idset_t;

static void idset_add(idset_t *s, int64_t id)
{
    if (id <= 0) return;
    size_t byte = (size_t)(id >> 3);
    if (byte >= s->cap) {
        size_t cap = byte + byte / 2 + 256;
        unsigned char *nb = realloc(s->bits, cap);
        if (!nb) { s->failed = 1; return; }   /* (then nothing is taken for gone) */
        memset(nb + s->cap, 0, cap - s->cap);
        s->bits = nb;
        s->cap = cap;
    }
    s->bits[byte] |= (unsigned char)(1u << (id & 7));
}

static int idset_has(const idset_t *s, int64_t id)
{
    size_t byte = (size_t)(id >> 3);
    return id > 0 && byte < s->cap && (s->bits[byte] >> (id & 7)) & 1;
}

/* A scan's writes, in short transactions: another connection's (the
 * player's play count when a track ends, a favourite, a playlist edit)
 * waits a fraction of a second at most, not the whole scan (minutes after
 * adding music: past its 10 s wait, it failed). Begun at a write; ended
 * before reading files again, or once BATCH_S old. */
#define BATCH_S 0.25

typedef struct { sqlite3 *db; double since; int open; } batch_t;

/* Every statement reset (one left on a row keeps its read open, and a
 * write on that old snapshot fails): only where none is being stepped. */
static void reset_all(sqlite3 *db)
{
    for (sqlite3_stmt *s = sqlite3_next_stmt(db, NULL); s; s = sqlite3_next_stmt(db, s))
        if (sqlite3_stmt_busy(s)) sqlite3_reset(s);
}

static void batch_end(batch_t *b)
{
    if (!b->open) return;
    reset_all(b->db);
    /* (a commit that fails, a full card: rolled back, or every later batch
     * would sit in this one, never committed) */
    if (sqlite3_exec(b->db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK && !sqlite3_get_autocommit(b->db))
        sqlite3_exec(b->db, "ROLLBACK", NULL, NULL, NULL);
    b->open = 0;
}

/* Before a write. IMMEDIATE: the write lock at once (waiting for another
 * connection's commit), on the newest snapshot. */
static void batch_write(batch_t *b)
{
    if (b->open) return;
    reset_all(b->db);
    b->open = sqlite3_exec(b->db, "BEGIN IMMEDIATE", NULL, NULL, NULL) == SQLITE_OK;   /* (else: one at a time) */
    b->since = now_s();
}

/* Between files: the batch ends once it's BATCH_S old. */
static void batch_tick(batch_t *b)
{
    if (b->open && now_s() - b->since >= BATCH_S) batch_end(b);
}

/* What a scan left to do, kept across scans (meta.scan_pending): batches
 * are committed as it goes, so one stopped half way (USB drive mode, a
 * shutdown) leaves tracks that albums, search and playlists don't show
 * yet. 1: composers to count again; 2: groups, playlists and search. */
static int meta_get(sqlite3 *db, const char *key)
{
    sqlite3_stmt *s = prep(db, "SELECT value FROM meta WHERE key=?");
    int v = 0;
    if (s) {
        sqlite3_bind_text(s, 1, key, -1, SQLITE_STATIC);
        if (sqlite3_step(s) == SQLITE_ROW) v = sqlite3_column_int(s, 0);
    }
    sqlite3_finalize(s);
    return v;
}

static void meta_set(sqlite3 *db, const char *key, int v)
{
    sqlite3_stmt *s = prep(db, "INSERT INTO meta(key, value) VALUES(?, ?) ON CONFLICT(key) DO UPDATE SET value=excluded.value");
    if (s) {
        sqlite3_bind_text(s, 1, key, -1, SQLITE_STATIC);
        sqlite3_bind_int(s, 2, v);
        sqlite3_step(s);
    }
    sqlite3_finalize(s);
}

/* A folder or file that's there but couldn't be looked at (the card's
 * reads failing, out of memory or file handles), not one that's gone. */
static int walk_error(int e)
{
    return e == EIO || e == ENOMEM || e == EMFILE || e == ENFILE;
}

static void walk(const char *dir, pathvec_t *pv, pathvec_t *plv, pathvec_t *cv, pathvec_t *iv, int depth,
                 const volatile int *cancel)
{
    if (depth > 16 || *cancel) return;
    DIR *d = opendir(dir);
    if (!d) {
        if (walk_error(errno)) pv->failed = 1;
        return;
    }
    struct dirent *e;
    char path[2048];
    for (;;) {
        errno = 0;
        if (!(e = readdir(d))) {
            if (errno) pv->failed = 1;         /* (the listing broke off: the rest isn't gone) */
            break;
        }
        if (e->d_name[0] == '.') continue;
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        /* the listing says what's a folder: only music, playlists and CUE sheets are looked up */
        int music = tags_supported(path), list = !music && m3u_supported(path), cue = !music && cue_supported(path);
        int image = !music && !list && !cue && iv && art_image(path);
        if (e->d_type != DT_UNKNOWN && e->d_type != DT_DIR && !music && !list && !cue && !image) continue;
        struct stat st;
        if (stat(path, &st)) {
            if (walk_error(errno)) pv->failed = 1;
            continue;
        }
        if (S_ISDIR(st.st_mode)) walk(path, pv, plv, cv, iv, depth + 1, cancel);
        else if (S_ISREG(st.st_mode) && music) pv_push(pv, path, &st);
        else if (S_ISREG(st.st_mode) && list) pv_push(plv, path, &st);
        else if (S_ISREG(st.st_mode) && cue) pv_push(cv, path, &st);
        else if (S_ISREG(st.st_mode) && image) pv_push(iv, path, &st);
    }
    closedir(d);
}

static int cmp_pent(const void *a, const void *b) { return strcmp(((const pent_t *)a)->p, ((const pent_t *)b)->p); }

/* ---------- the fast walk ----------
 * Listing the music folder through the kernel costs ~20 s on the H2's card;
 * reading its FAT directly (fatwalk) a few. Its list is used only when it's
 * exactly what the library holds already (nothing changed: the usual case);
 * any difference and the folder is walked as usual. */
typedef struct { pathvec_t *pv, *plv, *cv, *iv; const volatile int *cancel; } fw_ctx_t;

static int fw_file(const char *path, int64_t mtime, int64_t size, void *ctx)
{
    fw_ctx_t *c = ctx;
    struct stat st = {0};
    st.st_mtime = (time_t)mtime;
    st.st_size = (off_t)size;
    if (tags_supported(path)) pv_push(c->pv, path, &st);
    else if (m3u_supported(path)) pv_push(c->plv, path, &st);
    else if (cue_supported(path)) pv_push(c->cv, path, &st);
    else if (art_image(path)) pv_push(c->iv, path, &st);
    return *c->cancel;
}

/* The music files in `pv` and sheets in `cv` (both sorted) are exactly the
 * library's files (its tracks', and those in `cues`), dates and sizes. The
 * first difference is logged: one that comes back at every scan (a name the
 * FAT reading spells otherwise than the kernel) means the slow walk each time. */
static int same_as_library(library_t *lib, const pathvec_t *pv, const pathvec_t *cv)
{
    sqlite3_stmt *s = prep(lib->db, "SELECT path, mtime, size FROM tracks WHERE cue=0"
                                    " UNION ALL SELECT path, mtime, size FROM cues ORDER BY 1");
    if (!s) return 0;
    int i = 0, j = 0, same = 1;
    while (same && sqlite3_step(s) == SQLITE_ROW) {
        const char *p = (const char *)sqlite3_column_text(s, 0);
        const pent_t *e = i < pv->n && (j >= cv->n || strcmp(pv->v[i].p, cv->v[j].p) < 0) ? &pv->v[i++]
                        : j < cv->n ? &cv->v[j++] : NULL;
        if (!e || !p || strcmp(p, e->p) || sqlite3_column_int64(s, 1) != e->mtime || sqlite3_column_int64(s, 2) != e->size) {
            same = 0;
            fprintf(stderr, "library: fast walk: %s, library: %s\n", e ? e->p : "(none)", p ? p : "(none)");
        }
    }
    sqlite3_finalize(s);
    if (same && (i < pv->n || j < cv->n))
        fprintf(stderr, "library: fast walk: %s, library: (none)\n", i < pv->n ? pv->v[i].p : cv->v[j].p);
    return same && i == pv->n && j == cv->n;
}

void library_set_fast_walk(library_t *lib, const char *dev, const char *mount)
{
    snprintf(lib->fw_dev, sizeof lib->fw_dev, "%s", dev ? dev : "");
    snprintf(lib->fw_mount, sizeof lib->fw_mount, "%s", mount ? mount : "");
}

/* The fast walk of `root`, if it's set up and the result is the library as it
 * is: 1 (pv, plv filled, sorted), else 0 (and they're left empty). */
static int fast_walk(library_t *lib, const char *root, pathvec_t *pv, pathvec_t *plv, pathvec_t *cv, pathvec_t *iv)
{
    size_t ml = strlen(lib->fw_mount);
    if (!lib->fw_dev[0] || !ml || strncmp(root, lib->fw_mount, ml) || (root[ml] != '/' && root[ml] != '\0'))
        return 0;
    const char *rel = root[ml] == '/' ? root + ml + 1 : "";
    sync();   /* fatwalk reads the partition itself: nothing may wait in the cache */
    fw_ctx_t c = {pv, plv, cv, iv, &lib->cancel};
    if (fatwalk(lib->fw_dev, lib->fw_mount, rel, fw_file, &c) == 0 && !lib->cancel) {
        qsort(pv->v, (size_t)pv->n, sizeof *pv->v, cmp_pent);
        qsort(cv->v, (size_t)cv->n, sizeof *cv->v, cmp_pent);
        if (same_as_library(lib, pv, cv)) return 1;
    }
    pv_free(pv);
    pv_free(plv);
    pv_free(cv);
    pv_free(iv);
    return 0;
}

/* A table's row for key (made if new): its id. The two statements are kept
 * for the whole rebuild (compiling them for every track was most of the
 * rebuild's work on the H2). */
typedef struct { sqlite3_stmt *ins, *get; } upsert_t;

static int upsert_prep(sqlite3 *db, const char *table, upsert_t *u)
{
    char sql[160];
    snprintf(sql, sizeof sql, "INSERT INTO %s(key) VALUES(?) ON CONFLICT(key) DO NOTHING", table);
    u->ins = prep(db, sql);
    snprintf(sql, sizeof sql, "SELECT id FROM %s WHERE key=?", table);
    u->get = prep(db, sql);
    return u->ins && u->get ? 0 : -1;
}

static void upsert_free(upsert_t *u)
{
    sqlite3_finalize(u->ins);
    sqlite3_finalize(u->get);
}

static int64_t upsert_key(upsert_t *u, const char *key)
{
    sqlite3_reset(u->get);
    sqlite3_bind_text(u->get, 1, key, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(u->get) == SQLITE_ROW) return sqlite3_column_int64(u->get, 0);   /* (most often) */
    sqlite3_reset(u->ins);
    sqlite3_bind_text(u->ins, 1, key, -1, SQLITE_TRANSIENT);
    sqlite3_step(u->ins);
    sqlite3_reset(u->get);
    sqlite3_bind_text(u->get, 1, key, -1, SQLITE_TRANSIENT);
    return sqlite3_step(u->get) == SQLITE_ROW ? sqlite3_column_int64(u->get, 0) : 0;
}

/* Punctuation a name may start with that sorting skips ("(What's the Story)..."
 * under W, as the fast-scroll letter says): its length in bytes, or 0. */
static size_t lead_punct(const unsigned char *p)
{
    if (*p < 0x80) return *p && *p != ' ' && !isalnum(*p) ? 1 : 0;
    static const char *const u[] = {"\xC2\xA1", "\xC2\xBF", "\xC2\xAB", "\xE2\x80\x9C", "\xE2\x80\x98",
                                    "\xE2\x80\x9E", "\xE2\x80\xA6"};   /* ¡ ¿ « “ ‘ „ … */
    for (size_t i = 0; i < sizeof u / sizeof u[0]; i++)
        if (!strncmp((const char *)p, u[i], strlen(u[i]))) return strlen(u[i]);
    return 0;
}

/* How names sort: lower case, leading punctuation skipped (unless that's all
 * there is), and for albums, artists and folders a leading "the " too. */
static void sort_key_ex(const char *in, char *out, size_t cap, int strip_the)
{
    const unsigned char *p = (const unsigned char *)in;
    for (size_t k; (k = lead_punct(p)); ) {
        p += k;
        while (*p == ' ') p++;
    }
    if (!*p) p = (const unsigned char *)in;
    size_t w = 0;
    for (; *p && w + 1 < cap; p++) out[w++] = (char)((*p >= 'A' && *p <= 'Z') ? *p + 32 : *p);
    out[w] = '\0';
    if (strip_the && !strncmp(out, "the ", 4)) memmove(out, out + 4, strlen(out + 4) + 1);
}

static void sort_key(const char *in, char *out, size_t cap) { sort_key_ex(in, out, cap, 1); }

static void sql_sortkey(sqlite3_context *ctx, int argc, sqlite3_value **argv)
{
    (void)argc;
    const unsigned char *in = sqlite3_value_text(argv[0]);
    char out[600];
    sort_key_ex(in ? (const char *)in : "", out, sizeof out, sqlite3_user_data(ctx) != NULL);
    sqlite3_result_text(ctx, out, -1, SQLITE_TRANSIENT);
}

/* Rebuild album/artist links from track tags (keys from library/keys.c). */
/* The folder at dir (and its parents, up to root): its id. */
static int64_t folder_id(sqlite3 *db, sqlite3_stmt *up, sqlite3_stmt *get, const char *dir, const char *root)
{
    size_t rl = strlen(root);
    int64_t parent = 0;
    if (strlen(dir) > rl) {
        char up_dir[1024];
        size_t dl = strlen(dir);
        if (dl >= sizeof up_dir) dl = sizeof up_dir - 1;
        memcpy(up_dir, dir, dl);
        up_dir[dl] = '\0';
        char *slash = strrchr(up_dir, '/');
        if (slash && (size_t)(slash - up_dir) >= rl) { *slash = '\0'; parent = folder_id(db, up, get, up_dir, root); }
        else parent = folder_id(db, up, get, root, root);
    }
    const char *name = strlen(dir) > rl ? strrchr(dir, '/') + 1 : "";
    char sort[300];
    sort_key(name, sort, sizeof sort);
    sqlite3_reset(up);
    sqlite3_bind_text(up, 1, dir, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(up, 2, parent);
    sqlite3_bind_text(up, 3, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(up, 4, sort, -1, SQLITE_TRANSIENT);
    sqlite3_step(up);
    sqlite3_reset(get);
    sqlite3_bind_text(get, 1, dir, -1, SQLITE_TRANSIENT);
    return sqlite3_step(get) == SQLITE_ROW ? sqlite3_column_int64(get, 0) : 0;
}

/* Folders: the tree of the tracks' folders, from the folder they all share. */
static void rebuild_folders(sqlite3 *db)
{
    char root[1024] = "";
    sqlite3_stmt *ends = prep(db, "SELECT MIN(path), MAX(path) FROM tracks");
    if (ends && sqlite3_step(ends) == SQLITE_ROW && sqlite3_column_text(ends, 0)) {
        const char *a = (const char *)sqlite3_column_text(ends, 0), *b = (const char *)sqlite3_column_text(ends, 1);
        size_t n = 0, cut = 0;   /* the common part, up to its last '/' */
        while (a[n] && b && a[n] == b[n]) { if (a[n] == '/') cut = n; n++; }
        if (cut >= sizeof root) cut = sizeof root - 1;
        memcpy(root, a, cut);
        root[cut] = '\0';
    }
    sqlite3_finalize(ends);
    sqlite3_stmt *q = prep(db, "SELECT id, path, folder_id FROM tracks ORDER BY path");
    sqlite3_stmt *up = prep(db, "INSERT INTO folders(path, parent, name, sort, seen) VALUES(?,?,?,?,1)"
                                " ON CONFLICT(path) DO UPDATE SET parent=excluded.parent, name=excluded.name,"
                                " sort=excluded.sort, seen=1");
    sqlite3_stmt *get = prep(db, "SELECT id FROM folders WHERE path=?");
    sqlite3_stmt *ut = prep(db, "UPDATE tracks SET folder_id=? WHERE id=?");
    if (q && up && get && ut) {
        sqlite3_exec(db, "UPDATE folders SET seen=0", NULL, NULL, NULL);
        char dir[1024], last[1024] = "";
        int64_t fid = 0;
        while (sqlite3_step(q) == SQLITE_ROW) {
            const char *path = (const char *)sqlite3_column_text(q, 1);
            snprintf(dir, sizeof dir, "%s", path ? path : "");
            char *slash = strrchr(dir, '/');
            if (slash) *slash = '\0';
            if (strcmp(dir, last)) {
                fid = folder_id(db, up, get, strlen(dir) >= strlen(root) ? dir : root, root);
                snprintf(last, sizeof last, "%s", dir);
            }
            if (sqlite3_column_type(q, 2) == SQLITE_NULL || sqlite3_column_int64(q, 2) != fid) {
                sqlite3_reset(ut);
                sqlite3_bind_int64(ut, 1, fid);
                sqlite3_bind_int64(ut, 2, sqlite3_column_int64(q, 0));
                sqlite3_step(ut);
            }
        }
        /* Songs in each folder and those below it: every track counted once
         * for its folder and each folder above (one walk up the tree per
         * track, not a pass over all tracks per folder: that re-read the
         * table from the card for minutes on the H2) */
        sqlite3_exec(db,
            "DELETE FROM folders WHERE seen=0;"
            "DROP TABLE IF EXISTS temp.fcount;"
            "CREATE TEMP TABLE fcount AS WITH RECURSIVE up(id) AS ("
            " SELECT folder_id FROM tracks WHERE folder_id IS NOT NULL"
            " UNION ALL SELECT f.parent FROM up JOIN folders f ON f.id=up.id WHERE f.parent<>0)"
            " SELECT id, COUNT(*) AS n FROM up GROUP BY id;"
            "UPDATE folders SET ntracks=COALESCE((SELECT n FROM temp.fcount c WHERE c.id=folders.id), 0);"
            "DROP TABLE temp.fcount;",
            NULL, NULL, NULL);
    }
    sqlite3_finalize(q);
    sqlite3_finalize(up);
    sqlite3_finalize(get);
    sqlite3_finalize(ut);
}

/* Composers: by name as tagged, ids kept from scan to scan. On its own when
 * only composers were read (an upgraded library's first scan). */
static void rebuild_composers(sqlite3 *db)
{
    sqlite3_exec(db,
        "INSERT OR IGNORE INTO composers(name) SELECT DISTINCT TRIM(composer) FROM tracks"
        " WHERE TRIM(COALESCE(composer,''))<>'';"
        "UPDATE tracks SET composer_id=(SELECT id FROM composers WHERE name=TRIM(tracks.composer))"
        " WHERE composer_id IS NOT (SELECT id FROM composers WHERE name=TRIM(tracks.composer));"
        "UPDATE composers SET nalbums=(SELECT COUNT(DISTINCT album_id) FROM tracks WHERE composer_id=composers.id),"
        " sort=COALESCE(sort, fold(name));"
        "DELETE FROM composers WHERE nalbums=0;"
        /* the spelling as tagged now (names match ignoring case: a retag from
         * "j. s. bach" to "J. S. Bach" was the same row, kept as first seen;
         * compared exactly, or the column's NOCASE took them as equal) */
        "UPDATE composers SET name=(SELECT TRIM(composer) FROM tracks WHERE composer_id=composers.id ORDER BY path LIMIT 1)"
        " WHERE name COLLATE BINARY IS NOT (SELECT TRIM(composer) FROM tracks WHERE composer_id=composers.id ORDER BY path LIMIT 1);",
        NULL, NULL, NULL);
}

static void rebuild_groups(library_t *lib)
{
    sqlite3 *db = lib->db;
    double t0 = now_s(), t1 = t0, t2 = t0;
    sqlite3_stmt *q = prep(db, "SELECT id, artist, album_artist, album, year, path, title, album_id, artist_id"
                               " FROM tracks ORDER BY path");
    sqlite3_stmt *ut = prep(db, "UPDATE tracks SET album_id=?, artist_id=? WHERE id=?");
    sqlite3_stmt *ua = prep(db,
        "UPDATE albums SET search=CASE WHEN title IS ?1 AND artist_id IS ?3 THEN search END, title=?1, sort=?2, artist_id=?3,"
        " year=CASE WHEN year IS NULL OR year=0 OR (?4>0 AND ?5<year)"
        " THEN ?6 ELSE year END, dir=COALESCE(dir, ?7), first_track=COALESCE(first_track, ?8) WHERE id=?9");
    sqlite3_stmt *ur = prep(db, "UPDATE artists SET name=COALESCE(name, ?), sort=? WHERE id=?");
    sqlite3_stmt *ura = prep(db, "UPDATE artists SET name=?, sort=? WHERE id=?");
    sqlite3_stmt *uc = prep(db, "INSERT OR IGNORE INTO credits(artist_id, track_id) VALUES(?, ?)");
    upsert_t u_art = {0}, u_alb = {0};
    /* artists already named as an album artist: that spelling wins over a
     * track artist's or a guest's ("The Prodigy" over a soundtrack's
     * "Prodigy" earlier in path order) */
    unsigned char *aa_named = NULL;
    size_t aa_cap = 0;
    if (!q || !ut || !ua || !ur || !ura || !uc || upsert_prep(db, "artists", &u_art) || upsert_prep(db, "albums", &u_alb)) goto out;

    sqlite3_exec(db, "UPDATE albums SET year=0, dir=NULL, first_track=NULL;"
                     "UPDATE tracks SET sort=songkey(COALESCE(title,'')) WHERE sort IS NOT songkey(COALESCE(title,''));",
                 NULL, NULL, NULL);
    sqlite3_exec(db, "DELETE FROM credits", NULL, NULL, NULL);
    /* names taken afresh from the tags (an album artist's first spelling in
     * path order, else the first track or credit to name them, as a new
     * library would): a retag from "the beatles" to "The Beatles" kept the
     * old spelling while any track matched */
    sqlite3_exec(db, "UPDATE artists SET name=NULL", NULL, NULL, NULL);
    char akey[600], aakey[600], albkey[1200], sort[300];
    while (sqlite3_step(q) == SQLITE_ROW) {
        int64_t tid = sqlite3_column_int64(q, 0);
        const char *artist = (const char *)sqlite3_column_text(q, 1);
        const char *aa = (const char *)sqlite3_column_text(q, 2);
        const char *album = (const char *)sqlite3_column_text(q, 3);
        int year = sqlite3_column_int(q, 4);
        const char *path = (const char *)sqlite3_column_text(q, 5);
        const char *title = (const char *)sqlite3_column_text(q, 6);
        if (!artist) artist = "";
        if (!aa) aa = "";
        if (!album || !*album) album = "Unknown album";

        /* track artist key (shuffle spread) and album-artist key (browsing) */
        key_artist(artist, aa, akey, sizeof akey);
        const char *album_artist_name = *aa ? aa : (*artist ? artist : "Unknown artist");
        key_artist(album_artist_name, NULL, aakey, sizeof aakey);
        if (key_is_various(aa)) snprintf(aakey, sizeof aakey, "various artists");
        if (!*akey) snprintf(akey, sizeof akey, "unknown artist");
        if (!*aakey) snprintf(aakey, sizeof aakey, "unknown artist");
        key_album(artist, aa, album, albkey, sizeof albkey);

        int64_t track_artist = upsert_key(&u_art, akey);
        int64_t album_artist = upsert_key(&u_art, aakey);
        int64_t album_id = upsert_key(&u_alb, albkey);

        int first_aa = 1;
        if (album_artist > 0) {
            if ((size_t)album_artist >= aa_cap) {
                size_t cap = (size_t)album_artist * 2 + 64;
                unsigned char *grown = realloc(aa_named, cap);
                if (grown) {
                    memset(grown + aa_cap, 0, cap - aa_cap);
                    aa_named = grown;
                    aa_cap = cap;
                }
            }
            if ((size_t)album_artist < aa_cap) {
                first_aa = !aa_named[album_artist];
                aa_named[album_artist] = 1;
            }
        }
        sqlite3_stmt *un = first_aa ? ura : ur;
        sqlite3_reset(un);
        sqlite3_bind_text(un, 1, key_is_various(aa) ? "Various Artists" : album_artist_name, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(un, 2, aakey, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(un, 3, album_artist);
        sqlite3_step(un);
        if (track_artist != album_artist) {
            sqlite3_reset(ur);
            sqlite3_bind_text(ur, 1, *artist ? artist : "Unknown artist", -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(ur, 2, akey, -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(ur, 3, track_artist);
            sqlite3_step(ur);
        }

        char dir[1024];
        snprintf(dir, sizeof dir, "%s", path ? path : "");
        char *slash = strrchr(dir, '/');
        if (slash) *slash = '\0';
        sort_key(album, sort, sizeof sort);
        sqlite3_reset(ua);
        sqlite3_bind_text(ua, 1, album, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(ua, 2, sort, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(ua, 3, album_artist);
        sqlite3_bind_int(ua, 4, year);
        sqlite3_bind_int(ua, 5, year);
        sqlite3_bind_int(ua, 6, year);
        sqlite3_bind_text(ua, 7, dir, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(ua, 8, tid);
        sqlite3_bind_int64(ua, 9, album_id);
        sqlite3_step(ua);

        /* everyone credited on it (Artists): the main artists and guests */
        char names[KEY_CREDIT_MAX][KEY_CREDIT_LEN], ckey[600];
        int nn = key_credits(artist, title, names);
        for (int i = 0; i < nn; i++) {
            key_artist(names[i], NULL, ckey, sizeof ckey);
            if (!*ckey) continue;
            int64_t cid = upsert_key(&u_art, ckey);
            sqlite3_reset(ur);
            sqlite3_bind_text(ur, 1, names[i], -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(ur, 2, ckey, -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(ur, 3, cid);
            sqlite3_step(ur);
            sqlite3_reset(uc);
            sqlite3_bind_int64(uc, 1, cid);
            sqlite3_bind_int64(uc, 2, tid);
            sqlite3_step(uc);
        }

        /* written only when it changed: a rebuild used to rewrite every track */
        if (sqlite3_column_int64(q, 7) != album_id || sqlite3_column_int64(q, 8) != track_artist ||
            sqlite3_column_type(q, 7) == SQLITE_NULL) {
            sqlite3_reset(ut);
            sqlite3_bind_int64(ut, 1, album_id);
            sqlite3_bind_int64(ut, 2, track_artist);
            sqlite3_bind_int64(ut, 3, tid);
            sqlite3_step(ut);
        }
    }
    t1 = now_s();
    sqlite3_exec(db, "UPDATE albums SET ntracks=(SELECT COUNT(*) FROM tracks WHERE album_id=albums.id)", NULL, NULL, NULL);
    /* Albums gone: their covers too. SQLite gives the next new album the
     * highest id again, and a coverless one showed the old album's cover. */
    sqlite3_stmt *gone = prep(db, "SELECT id FROM albums WHERE ntracks=0");
    while (gone && sqlite3_step(gone) == SQLITE_ROW) {
        static const int sizes[2] = {LIB_ART_LARGE, LIB_ART_SMALL};
        char path[1200];
        for (int i = 0; i < 2; i++) {
            art_path(lib, sqlite3_column_int64(gone, 0), sizes[i], path, sizeof path);
            unlink(path);
        }
    }
    sqlite3_finalize(gone);
    sqlite3_exec(db,
        "DELETE FROM albums WHERE ntracks=0;"
        "UPDATE artists SET nalbums=(SELECT COUNT(*) FROM albums WHERE artist_id=artists.id);"
        "UPDATE artists SET ncredit_albums=(SELECT COUNT(DISTINCT t.album_id) FROM credits c"
        " JOIN tracks t ON t.id=c.track_id WHERE c.artist_id=artists.id);"
        "DELETE FROM artists WHERE nalbums=0 AND ncredit_albums=0 AND id NOT IN (SELECT DISTINCT artist_id FROM tracks);"
        /* Genres and Composers: by name as tagged, ids kept from scan to scan */
        "INSERT OR IGNORE INTO genres(name) SELECT DISTINCT TRIM(genre) FROM tracks WHERE TRIM(COALESCE(genre,''))<>'';"
        "UPDATE tracks SET genre_id=(SELECT id FROM genres WHERE name=TRIM(tracks.genre))"
        " WHERE genre_id IS NOT (SELECT id FROM genres WHERE name=TRIM(tracks.genre));"
        "UPDATE genres SET nalbums=(SELECT COUNT(DISTINCT album_id) FROM tracks WHERE genre_id=genres.id),"
        " sort=COALESCE(sort, fold(name));"
        "DELETE FROM genres WHERE nalbums=0;"
        "UPDATE genres SET name=(SELECT TRIM(genre) FROM tracks WHERE genre_id=genres.id ORDER BY path LIMIT 1)"
        " WHERE name COLLATE BINARY IS NOT (SELECT TRIM(genre) FROM tracks WHERE genre_id=genres.id ORDER BY path LIMIT 1);"
        /* search: what a changed name is found by (albums show their artist's) */
        "UPDATE artists SET search=NULL WHERE search IS NOT fold(coalesce(name,''));"
        "UPDATE albums SET search=NULL WHERE artist_id IN (SELECT id FROM artists WHERE search IS NULL);",
        NULL, NULL, NULL);
    rebuild_composers(db);
    t2 = now_s();
    rebuild_folders(db);
    fprintf(stderr, "library: groups: tracks %.2f s, counts %.2f s, folders %.2f s\n", t1 - t0, t2 - t1, now_s() - t2);
out:
    sqlite3_finalize(q);
    sqlite3_finalize(ut);
    sqlite3_finalize(ua);
    sqlite3_finalize(ur);
    sqlite3_finalize(ura);
    sqlite3_finalize(uc);
    free(aa_named);
    upsert_free(&u_art);
    upsert_free(&u_alb);
}

/* ---------- the groups, without holding the write lock ----------
 * rebuild_groups() goes over every track: on the H2 about a millisecond a
 * track, all under the write lock (2,400 tracks: a favourite or a play count
 * waited 2-3 s). Done instead on private copies of the tables it uses (TEMP
 * tables of the same names: SQLite looks in temp first, so the same code runs
 * on them, and only reads the library), then what differs is written back in
 * one short batch. Only scans write these tables and columns; the player's own
 * writes (plays, favourites, covers: albums.art) are left as they are. */

static const char *const SHADOWED[] = {"albums", "artists", "credits", "genres", "composers", "folders"};

static void shadow_drop(sqlite3 *db)
{
    sqlite3_exec(db, "DROP TABLE IF EXISTS temp.tracks", NULL, NULL, NULL);
    for (size_t i = 0; i < sizeof SHADOWED / sizeof *SHADOWED; i++) {
        char sql[80];
        snprintf(sql, sizeof sql, "DROP TABLE IF EXISTS temp.%s", SHADOWED[i]);
        sqlite3_exec(db, sql, NULL, NULL, NULL);
    }
}

/* run sql; 0, or -1 (said) */
static int shadow_exec(sqlite3 *db, const char *sql)
{
    char *err = NULL;
    if (sqlite3_exec(db, sql, NULL, NULL, &err) == SQLITE_OK) return 0;
    fprintf(stderr, "library: groups: %s\n  in: %.200s\n", err ? err : "?", sql);
    sqlite3_free(err);
    return -1;
}

/* A table's copy in temp, its indexes too (the rebuild's lookups need them) */
static int shadow_copy(sqlite3 *db, const char *table)
{
    sqlite3_stmt *q = prep(db, "SELECT type, sql FROM main.sqlite_master WHERE tbl_name=?1 AND sql IS NOT NULL"
                               " ORDER BY type='index'");
    if (!q) return -1;
    sqlite3_bind_text(q, 1, table, -1, SQLITE_STATIC);
    int rc = 0, tables = 0;
    while (!rc && sqlite3_step(q) == SQLITE_ROW) {
        const char *type = (const char *)sqlite3_column_text(q, 0), *sql = (const char *)sqlite3_column_text(q, 1);
        const char *at = strstr(sql, !strcmp(type, "table") ? "TABLE " : "INDEX ");
        if (!at) { rc = -1; break; }
        at += 6;
        char *s = sqlite3_mprintf(!strcmp(type, "table") ? "CREATE TEMP TABLE %s" : "CREATE INDEX temp.%s", at);
        rc = s ? shadow_exec(db, s) : -1;
        sqlite3_free(s);
        tables += !strcmp(type, "table");
    }
    sqlite3_finalize(q);
    if (rc || tables != 1) return -1;
    char *s = sqlite3_mprintf("INSERT INTO temp.%s SELECT * FROM main.%s", table, table);
    rc = s ? shadow_exec(db, s) : -1;
    sqlite3_free(s);
    return rc;
}

/* main.table made as temp.table: rows gone deleted, new ones added, changed
 * ones updated (compared exactly: a NOCASE name respelled is a change).
 * albums keep their cover state (written by the cover step and the player),
 * unless the id now holds another album (SQLite gives a removed album's id
 * to the next new one). */
static int shadow_apply(sqlite3 *db, const char *table)
{
    char *pragma = sqlite3_mprintf("PRAGMA main.table_info(%s)", table);
    sqlite3_stmt *ti = pragma ? prep(db, pragma) : NULL;
    sqlite3_free(pragma);
    if (!ti) return -1;
    int albums = !strcmp(table, "albums");
    sqlite3_str *set = sqlite3_str_new(db), *cmp = sqlite3_str_new(db), *cols = sqlite3_str_new(db);
    while (sqlite3_step(ti) == SQLITE_ROW) {
        const char *c = (const char *)sqlite3_column_text(ti, 1);
        if (!strcmp(c, "id") || (albums && (!strcmp(c, "art") || !strcmp(c, "art_sig")))) continue;
        sqlite3_str_appendf(set, "%s\"%w\"=t.\"%w\"", sqlite3_str_length(set) ? ", " : "", c, c);
        sqlite3_str_appendf(cmp, "%sm.\"%w\" IS NOT t.\"%w\" COLLATE BINARY", sqlite3_str_length(cmp) ? " OR " : "", c, c);
        sqlite3_str_appendf(cols, ", \"%w\"", c);
    }
    sqlite3_finalize(ti);
    if (albums)
        sqlite3_str_appendall(set, ", art=CASE WHEN m.key IS t.key THEN m.art ELSE 0 END,"
                                   " art_sig=CASE WHEN m.key IS t.key THEN m.art_sig ELSE 0 END");
    char *s_set = sqlite3_str_finish(set), *s_cmp = sqlite3_str_finish(cmp), *s_cols = sqlite3_str_finish(cols);
    char *sql = s_set && s_cmp && s_cols && *s_cmp ? sqlite3_mprintf(
        "DELETE FROM main.%s WHERE id NOT IN (SELECT id FROM temp.%s);"
        "UPDATE main.%s AS m SET %s FROM temp.%s AS t WHERE t.id=m.id AND (%s);"
        "INSERT INTO main.%s(id%s) SELECT id%s FROM temp.%s AS t"
        " WHERE NOT EXISTS (SELECT 1 FROM main.%s AS m WHERE m.id=t.id);",
        table, table, table, s_set, table, s_cmp, table, s_cols, s_cols, table, table) : NULL;
    int rc = sql ? shadow_exec(db, sql) : -1;
    sqlite3_free(sql);
    sqlite3_free(s_set);
    sqlite3_free(s_cmp);
    sqlite3_free(s_cols);
    return rc;
}

/* The groups as rebuild_groups() makes them, the write lock held only to put
 * in what changed (and the search columns that then need filling). 0, or -1:
 * nothing written (do it the old way). Called outside any batch. */
static int rebuild_groups_aside(library_t *lib, batch_t *b)
{
    sqlite3 *db = lib->db;
    double t0 = now_s();
    shadow_drop(db);
    /* (on the H2 the temp store spills to the card, not RAM: SQLITE_TMPDIR) */
    int rc = shadow_exec(db, "PRAGMA temp.cache_size=-256; BEGIN;");
    rc = rc ? rc : shadow_exec(db,
        "CREATE TEMP TABLE tracks(id INTEGER PRIMARY KEY, path TEXT UNIQUE NOT NULL, title TEXT, artist TEXT,"
        " album_artist TEXT, album TEXT, genre TEXT, year INTEGER, composer TEXT, album_id INTEGER,"
        " artist_id INTEGER, sort TEXT, genre_id INTEGER, composer_id INTEGER, folder_id INTEGER);"
        "INSERT INTO temp.tracks SELECT id, path, title, artist, album_artist, album, genre, year, composer,"
        " album_id, artist_id, sort, genre_id, composer_id, folder_id FROM main.tracks;"
        "CREATE INDEX temp.tracks_album ON tracks(album_id);"
        "CREATE INDEX temp.tracks_genre ON tracks(genre_id);"
        "CREATE INDEX temp.tracks_composer ON tracks(composer_id);"
        "CREATE INDEX temp.tracks_folder ON tracks(folder_id);");
    for (size_t i = 0; !rc && i < sizeof SHADOWED / sizeof *SHADOWED; i++) rc = shadow_copy(db, SHADOWED[i]);
    if (!rc) rebuild_groups(lib);              /* (on the copies: temp comes first) */
    if (rc || shadow_exec(db, "COMMIT") != 0) {
        if (!sqlite3_get_autocommit(db)) sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        shadow_drop(db);
        return -1;
    }
    double t1 = now_s();
    batch_write(b);
    if (!b->open) { shadow_drop(db); return -1; }
    rc = shadow_exec(db,
        "UPDATE main.tracks AS m SET album_id=t.album_id, artist_id=t.artist_id, sort=t.sort, genre_id=t.genre_id,"
        " composer_id=t.composer_id, folder_id=t.folder_id FROM temp.tracks AS t WHERE t.id=m.id AND"
        " (m.album_id IS NOT t.album_id OR m.artist_id IS NOT t.artist_id OR m.sort IS NOT t.sort COLLATE BINARY"
        " OR m.genre_id IS NOT t.genre_id OR m.composer_id IS NOT t.composer_id OR m.folder_id IS NOT t.folder_id);"
        "DELETE FROM main.credits WHERE rowid IN (SELECT m.rowid FROM main.credits AS m WHERE NOT EXISTS"
        " (SELECT 1 FROM temp.credits AS t WHERE t.artist_id=m.artist_id AND t.track_id=m.track_id));"
        "INSERT OR IGNORE INTO main.credits(artist_id, track_id) SELECT artist_id, track_id FROM temp.credits;");
    for (size_t i = 0; !rc && i < sizeof SHADOWED / sizeof *SHADOWED; i++)
        if (strcmp(SHADOWED[i], "credits")) rc = shadow_apply(db, SHADOWED[i]);
    if (rc) {                                  /* (nothing of it kept) */
        reset_all(db);
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        b->open = 0;
        shadow_drop(db);
        return -1;
    }
    shadow_drop(db);
    index_search(lib, 1);                      /* changed tracks (their search cleared), new albums and artists */
    batch_end(b);
    sqlite3_db_release_memory(db);             /* (the copies' pages) */
    fprintf(stderr, "library: groups aside %.2f s, written in %.2f s\n", t1 - t0, now_s() - t1);
    return 0;
}

/* ---------- cover art ---------- */

/* Find a cover image file in an album folder: preferred names first, then any
 * image. Returns the match's rank (lower is better) or -1. */
static int find_cover_file(const char *dir, char *out, size_t cap)
{
    static const char *const names[] = {"cover", "folder", "front", "album", "albumart"};
    DIR *d = opendir(dir);
    if (!d) return -1;
    struct dirent *e;
    int best = 99;
    out[0] = '\0';
    while ((e = readdir(d))) {
        const char *dot = strrchr(e->d_name, '.');
        if (e->d_name[0] == '.' || !dot) continue;
        if (strcasecmp(dot, ".jpg") && strcasecmp(dot, ".jpeg") && strcasecmp(dot, ".png")) continue;
        int rank = 50;
        for (int i = 0; i < 5; i++)
            if (!strncasecmp(e->d_name, names[i], strlen(names[i])) && e->d_name + strlen(names[i]) == dot) rank = i;
        if (rank < best) {
            best = rank;
            snprintf(out, cap, "%s/%s", dir, e->d_name);
        }
    }
    closedir(d);
    return out[0] ? best : -1;   /* 0-4: named like a cover; 50: just some image */
}

/* RGB888 -> RGB565 with a 4x4 ordered dither, so gradients don't band on the panel. */
static void to_rgb565(const unsigned char *rgb, int size, uint16_t *out)
{
    static const int bayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++) {
            const unsigned char *p = rgb + 3 * (y * size + x);
            int d = bayer[y & 3][x & 3];
            int r = p[0] + (d >> 1) - 4, g = p[1] + (d >> 2) - 2, b = p[2] + (d >> 1) - 4;
            r = r < 0 ? 0 : r > 255 ? 255 : r;
            g = g < 0 ? 0 : g > 255 ? 255 : g;
            b = b < 0 ? 0 : b > 255 ? 255 : b;
            out[y * size + x] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        }
}

static void art_path(const library_t *lib, int64_t album_id, int size, char *out, size_t cap)
{
    snprintf(out, cap, "%s/%lld-%d.rgb565", lib->art_dir, (long long)album_id, size);
}

/*
 * Shrink a square RGB888 region (side `src`, row stride `stride` bytes) to
 * `dst` x `dst` by averaging the source pixels under each output pixel.
 * Integer only: fast on the H2's FPU-light core, and for downscaling album
 * art it looks the same as a windowed filter.
 */
static void area_downscale(const unsigned char *in, int src, int stride, unsigned char *out, int dst)
{
    for (int oy = 0; oy < dst; oy++) {
        int y0 = oy * src / dst, y1 = (oy + 1) * src / dst;
        if (y1 <= y0) y1 = y0 + 1;
        for (int ox = 0; ox < dst; ox++) {
            int x0 = ox * src / dst, x1 = (ox + 1) * src / dst;
            if (x1 <= x0) x1 = x0 + 1;
            unsigned r = 0, g = 0, b = 0, n = (unsigned)((y1 - y0) * (x1 - x0));
            for (int y = y0; y < y1; y++) {
                const unsigned char *p = in + (size_t)y * stride + (size_t)x0 * 3;
                for (int x = x0; x < x1; x++, p += 3) { r += p[0]; g += p[1]; b += p[2]; }
            }
            unsigned char *o = out + 3 * ((size_t)oy * dst + ox);
            o[0] = (unsigned char)((r + n / 2) / n);
            o[1] = (unsigned char)((g + n / 2) / n);
            o[2] = (unsigned char)((b + n / 2) / n);
        }
    }
}

static const art_limits_t ART_LIMITS = {FATHOM_JPEG_MAX_MEMORY, FATHOM_ART_MAX_PIXELS, FATHOM_ART_MAX_FILE};

/* Crop to a square and shrink to size x size RGB565: 0, -1 */
static int art_square(const unsigned char *img, int w, int h, int size, uint16_t *pixels)
{
    int side = w < h ? w : h;
    const unsigned char *crop = img + 3 * ((size_t)((h - side) / 2) * (size_t)w + (size_t)((w - side) / 2));
    unsigned char *rs = malloc((size_t)size * (size_t)size * 3);
    if (!rs) return -1;
    area_downscale(crop, side, w * 3, rs, size);
    to_rgb565(rs, size, pixels);
    free(rs);
    return 0;
}

int library_art_decode(const unsigned char *enc, size_t len, int size, uint16_t *pixels)
{
    unsigned char *img;
    int w, h;
    if (art_decode_mem(enc, len, size * 3 / 2, &ART_LIMITS, &img, &w, &h)) return -1;
    int r = art_square(img, w, h, size, pixels);
    free(img);
    return r;
}

/* Done with a file read for its cover: out of the page cache, where a 20 MB
 * PNG would push out the player's own code (run from the card). */
static void art_done(FILE *f)
{
#ifdef POSIX_FADV_DONTNEED
    posix_fadvise(fileno(f), 0, 0, POSIX_FADV_DONTNEED);
#endif
    fclose(f);
}

/* An image file, decoded small (artdec.h). NULL if it isn't one we can read. */
static unsigned char *art_from_file(const char *file, int *w, int *h)
{
    FILE *f = fopen(file, "rbe");
    unsigned char *img = NULL;
    if (f && art_decode(f, 0, -1, LIB_ART_LARGE * 3 / 2, &ART_LIMITS, &img, w, h)) img = NULL;
    if (f) art_done(f);
    return img;
}

/* A track's embedded cover: a FLAC's read from where it is in the file
 * (any size), others' through memory (up to FATHOM_ART_MAX_FILE). */
static unsigned char *art_from_track(const char *track, int *w, int *h)
{
    long off, len;
    unsigned char *img = NULL, *enc;
    size_t n;
    if (tags_picture_at(track, &off, &len) == 0) {
        FILE *f = fopen(track, "rbe");
        if (f && art_decode(f, off, len, LIB_ART_LARGE * 3 / 2, &ART_LIMITS, &img, w, h)) img = NULL;
        if (f) art_done(f);
    } else if (tags_picture(track, &enc, &n) == 0) {
        if (art_decode_mem(enc, n, LIB_ART_LARGE * 3 / 2, &ART_LIMITS, &img, w, h)) img = NULL;
        free(enc);
    }
    return img;
}

/* "CD1", "Disc 2", "disk 01 - Bonus": one disc of an album, whose cover is
 * often in the folder above. */
static int disc_folder(const char *dir)
{
    const char *b = strrchr(dir, '/');
    b = b ? b + 1 : dir;
    static const char *const pre[] = {"cd", "disc", "disk"};
    for (int i = 0; i < 3; i++) {
        size_t n = strlen(pre[i]);
        if (strncasecmp(b, pre[i], n)) continue;
        const char *p = b + n;
        while (*p == ' ' || *p == '_' || *p == '-' || *p == '.') p++;
        if (isdigit((unsigned char)*p)) return 1;
    }
    return 0;
}

/* The folder above dir, into out: 0, or -1 if there's none. */
static int parent_dir(const char *dir, char *out, size_t cap)
{
    const char *slash = strrchr(dir, '/');
    if (!slash || slash == dir || (size_t)(slash - dir) >= cap) return -1;
    memcpy(out, dir, (size_t)(slash - dir));
    out[slash - dir] = '\0';
    return 0;
}

/* The album's cover, best first: an image named like a cover in its folder
 * (or, for a disc's folder, in the album's above it); the first track's
 * embedded one, then up to four other tracks' that have one; any other image
 * in the folder (a scan or a booklet page loses to an embedded cover). Each
 * is tried until one decodes. */
static int make_art(library_t *lib, sqlite3_stmt *others, int64_t album_id, const char *dir, const char *first_track)
{
    char file[2048], up[2048], pfile[2048];
    int w = 0, h = 0;
    unsigned char *img = NULL;
    int rank = find_cover_file(dir, file, sizeof file);
    int prank = -1;
    if ((rank < 0 || rank > 4) && disc_folder(dir) && parent_dir(dir, up, sizeof up) == 0) {
        prank = find_cover_file(up, pfile, sizeof pfile);
        if (prank > 4) prank = -1;                   /* (only one named like a cover up there) */
    }
    if (rank >= 0 && rank <= 4) img = art_from_file(file, &w, &h);
    if (!img && prank >= 0) img = art_from_file(pfile, &w, &h);
    if (!img && first_track) img = art_from_track(first_track, &w, &h);
    if (!img && others) {
        sqlite3_reset(others);
        sqlite3_bind_int64(others, 1, album_id);
        sqlite3_bind_text(others, 2, first_track ? first_track : "", -1, SQLITE_STATIC);
        while (!img && sqlite3_step(others) == SQLITE_ROW)
            img = art_from_track((const char *)sqlite3_column_text(others, 0), &w, &h);
        sqlite3_reset(others);
    }
    if (!img && rank > 4) img = art_from_file(file, &w, &h);
    if (!img) return -1;
    const int sizes[2] = {LIB_ART_LARGE, LIB_ART_SMALL};
    int ok = 0;
    for (int i = 0; i < 2; i++) {
        int s = sizes[i];
        uint16_t *px = malloc((size_t)s * (size_t)s * 2);
        if (px && art_square(img, w, h, s, px) == 0) {
            char path[1200];
            art_path(lib, album_id, s, path, sizeof path);
            FILE *f = fopen(path, "wbe");
            if (f) { ok += fwrite(px, 2, (size_t)s * s, f) == (size_t)s * s; fclose(f); }
        }
        free(px);
    }
    free(img);
    return ok == 2 ? 0 : -1;
}

/* What an album's cover comes from (or could), as one number: the images in
 * its folder (and a disc's album folder) by name, size and date, as the walk
 * saw them, and its tracks (how many, their times, how many have a picture).
 * The same next scan: nothing to look at again. */
static uint64_t fnv(uint64_t hsh, const void *p, size_t n)
{
    const unsigned char *b = p;
    for (size_t i = 0; i < n; i++) hsh = (hsh ^ b[i]) * 1099511628211ULL;
    return hsh;
}

static uint64_t images_sig(const pathvec_t *iv, const char *dir)
{
    uint64_t sig = 0;
    size_t dl = strlen(dir);
    int lo = 0, hi = iv->n;                    /* the first path at or after "dir/" (iv is sorted) */
    while (lo < hi) {
        int m = (lo + hi) / 2;
        int c = strncmp(iv->v[m].p, dir, dl);
        if (c < 0 || (c == 0 && iv->v[m].p[dl] < '/')) lo = m + 1;
        else hi = m;
    }
    for (int i = lo; i < iv->n && !strncmp(iv->v[i].p, dir, dl) && iv->v[i].p[dl] == '/'; i++) {
        const char *name = iv->v[i].p + dl + 1;
        if (strchr(name, '/')) continue;       /* (in a folder below) */
        sig = fnv(sig ^ 14695981039346656037ULL, name, strlen(name));
        sig = fnv(sig, &iv->v[i].size, sizeof iv->v[i].size);
        sig = fnv(sig, &iv->v[i].mtime, sizeof iv->v[i].mtime);
    }
    return sig;
}

static int64_t art_sig(const pathvec_t *iv, sqlite3_stmt *tracks, int64_t album_id, const char *dir)
{
    char up[2048];
    uint64_t sig = images_sig(iv, dir);
    if (disc_folder(dir) && parent_dir(dir, up, sizeof up) == 0) sig = fnv(sig, "^", 1) ^ images_sig(iv, up);
    sqlite3_reset(tracks);
    sqlite3_bind_int64(tracks, 1, album_id);
    if (sqlite3_step(tracks) == SQLITE_ROW) {
        int64_t v[3] = {sqlite3_column_int64(tracks, 0), sqlite3_column_int64(tracks, 1), sqlite3_column_int64(tracks, 2)};
        sig = fnv(sig, v, sizeof v);
    }
    sqlite3_reset(tracks);
    return (int64_t)(sig & 0x7fffffffffffffffULL) | 1;   /* (never 0: not looked yet) */
}

/* New albums' covers, and any album's again when what it comes from (or
 * could) has changed: an image added, replaced or removed, a track retagged.
 * A cover from before signatures covered found covers (art_sig 0) is kept as
 * it is, its signature noted. Albums are read a page at a time (none being
 * read while writing), and what's written is committed before the next
 * cover is made (decoding one can take a second on the H2: never with the
 * write lock held). */
#define ART_PAGE 32

static void build_art(library_t *lib, const pathvec_t *iv, batch_t *b, lib_progress_cb cb, void *ctx)
{
    sqlite3_stmt *q = prep(lib->db,
        "SELECT al.id, al.dir, t.path, al.art, al.art_sig FROM albums al LEFT JOIN tracks t ON t.id=al.first_track"
        " WHERE al.id>? ORDER BY al.id LIMIT " LIB_STR(ART_PAGE));
    sqlite3_stmt *u = prep(lib->db, "UPDATE albums SET art=?, art_sig=? WHERE id=?");
    sqlite3_stmt *others = prep(lib->db,
        "SELECT path FROM tracks WHERE album_id=? AND has_pic=1 AND path<>? ORDER BY disc, track LIMIT 4");
    sqlite3_stmt *tracks = prep(lib->db,
        "SELECT COUNT(*), TOTAL(mtime), TOTAL(has_pic) FROM tracks WHERE album_id=?");
    struct { int64_t id, was; int art; char *dir, *first; } page[ART_PAGE];
    int n = 0, retried = 0, found = 0, got;
    int64_t after = 0;
    if (!q || !u || !others || !tracks) goto out;
    do {
        got = 0;
        sqlite3_reset(q);
        sqlite3_bind_int64(q, 1, after);
        while (got < ART_PAGE && sqlite3_step(q) == SQLITE_ROW) {
            const unsigned char *d = sqlite3_column_text(q, 1), *f = sqlite3_column_text(q, 2);
            page[got].id = sqlite3_column_int64(q, 0);
            page[got].dir = strdup(d ? (const char *)d : "");
            page[got].first = f ? strdup((const char *)f) : NULL;
            page[got].art = sqlite3_column_int(q, 3);
            page[got].was = sqlite3_column_int64(q, 4);
            after = page[got].id;
            got++;
        }
        sqlite3_reset(q);
        for (int k = 0; k < got && !lib->cancel; k++) {   /* (stopped: the rest at the next scan) */
            int64_t id = page[k].id, was = page[k].was;
            int art = page[k].art;
            const char *dir = page[k].dir ? page[k].dir : "";
            int64_t sig = art_sig(iv, tracks, id, dir);
            if (art != 0 && sig == was) continue;   /* nothing new to look at */
            int r = 0;
            if (!(art == 1 && was == 0)) {          /* (found before signatures: kept, its signature noted) */
                batch_end(b);
                r = make_art(lib, others, id, dir, page[k].first);
                if (art != 0) retried++;
                if (r == 0) found++;
                else if (art == 1) {                /* its cover is gone: so are the thumbnails */
                    static const int sizes[2] = {LIB_ART_LARGE, LIB_ART_SMALL};
                    char path[1200];
                    for (int i = 0; i < 2; i++) {
                        art_path(lib, id, sizes[i], path, sizeof path);
                        unlink(path);
                    }
                }
                if (cb && (++n % 20) == 0) cb(n, -1, "cover art", ctx);
            }
            batch_write(b);
            sqlite3_reset(u);
            sqlite3_bind_int(u, 1, r == 0 ? 1 : 2);   /* 2 = looked, none found */
            sqlite3_bind_int64(u, 2, sig);
            sqlite3_bind_int64(u, 3, id);
            sqlite3_step(u);
            sqlite3_reset(u);
            batch_tick(b);
        }
        for (int k = 0; k < got; k++) {
            free(page[k].dir);
            free(page[k].first);
        }
    } while (got == ART_PAGE && !lib->cancel);
    batch_end(b);
    if (n) fprintf(stderr, "library: covers: %d looked for (%d again), %d found\n", n, retried, found);
out:
    sqlite3_finalize(q);
    sqlite3_finalize(u);
    sqlite3_finalize(others);
    sqlite3_finalize(tracks);
}

/* ---------- playlists ---------- */

/* Entries no exact path found, looked for by looser keys in one pass over
 * the tracks: their last three path components folded, then two (a
 * playlist made on another computer, or names spelt in another Unicode
 * form), then the #EXTINF "Artist - Title" against the tags, nearest
 * duration first (the same music copied with other file names). The keys
 * held are the entries' (LOOSE_CHUNK at a time), not the library's: an
 * index of every track, three times over, took several MB on a big one. */
#define LOOSE_CHUNK 256

typedef struct { char *key; int k; } probe_t;

static int cmp_probe(const void *a, const void *b) { return strcmp(((const probe_t *)a)->key, ((const probe_t *)b)->key); }

/* The probes with this key: the first's index, *n of them (0: none). */
static int probe_range(const probe_t *v, int n, const char *key, int *count)
{
    int lo = 0, hi = n;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (strcmp(v[mid].key, key) < 0) lo = mid + 1;
        else hi = mid;
    }
    int e = lo;
    while (e < n && !strcmp(v[e].key, key)) e++;
    *count = e - lo;
    return lo;
}

/* Entries ent[0..n) of m (n <= LOOSE_CHUNK): their tracks into match[]. */
static void loose_match(sqlite3 *db, const m3u_t *m, const int *ent, int n, int64_t *match)
{
    probe_t *p3 = calloc((size_t)n, sizeof *p3), *p2 = calloc((size_t)n, sizeof *p2), *pt = calloc((size_t)n, sizeof *pt);
    int64_t *best = calloc((size_t)n * 3, sizeof *best);   /* by entry: by 3 components, 2, tags */
    int *diff = calloc((size_t)n, sizeof *diff);
    sqlite3_stmt *q = prep(db, "SELECT id, path, artist, title, duration_ms FROM tracks");
    char key[1024], text[1024];
    int n3 = 0, n2 = 0, nt = 0;
    if (!p3 || !p2 || !pt || !best || !diff || !q) goto out;
    for (int k = 0; k < n; k++) {
        const int j = ent[k];
        path_loose_key(m->paths[j], 3, key, sizeof key);
        if (*key && (p3[n3].key = strdup(key))) p3[n3++].k = k;
        path_loose_key(m->paths[j], 2, key, sizeof key);
        if (*key && (p2[n2].key = strdup(key))) p2[n2++].k = k;
        if (m->infos[j]) {
            text_loose_key(m->infos[j], key, sizeof key);
            if (*key && (pt[nt].key = strdup(key))) pt[nt++].k = k;
        }
    }
    qsort(p3, (size_t)n3, sizeof *p3, cmp_probe);
    qsort(p2, (size_t)n2, sizeof *p2, cmp_probe);
    qsort(pt, (size_t)nt, sizeof *pt, cmp_probe);
    while (sqlite3_step(q) == SQLITE_ROW) {
        int64_t id = sqlite3_column_int64(q, 0);
        const char *path = (const char *)sqlite3_column_text(q, 1);
        int c, at;
        path_loose_key(path ? path : "", 3, key, sizeof key);
        for (at = probe_range(p3, n3, key, &c); c-- > 0; at++)
            if (!best[p3[at].k * 3]) best[p3[at].k * 3] = id;
        path_loose_key(path ? path : "", 2, key, sizeof key);
        for (at = probe_range(p2, n2, key, &c); c-- > 0; at++)
            if (!best[p2[at].k * 3 + 1]) best[p2[at].k * 3 + 1] = id;
        if (!nt) continue;
        const unsigned char *a = sqlite3_column_text(q, 2), *t = sqlite3_column_text(q, 3);
        snprintf(text, sizeof text, "%s %s", a ? (const char *)a : "", t ? (const char *)t : "");
        text_loose_key(text, key, sizeof key);
        if (!*key) continue;
        int secs = (int)(sqlite3_column_int64(q, 4) / 1000);
        for (at = probe_range(pt, nt, key, &c); c-- > 0; at++) {
            int k = pt[at].k, want = m->secs[ent[k]];
            int d = want >= 0 ? abs(secs - want) : 0;
            if (!best[k * 3 + 2] || d < diff[k]) { best[k * 3 + 2] = id; diff[k] = d; }
        }
    }
    for (int k = 0; k < n; k++)
        match[ent[k]] = best[k * 3] ? best[k * 3] : best[k * 3 + 1] ? best[k * 3 + 1] : best[k * 3 + 2];
out:
    sqlite3_finalize(q);
    for (int i = 0; i < n3; i++) free(p3[i].key);
    for (int i = 0; i < n2; i++) free(p2[i].key);
    for (int i = 0; i < nt; i++) free(pt[i].key);
    free(p3);
    free(p2);
    free(pt);
    free(best);
    free(diff);
}

static int pv_find(const pathvec_t *pv, const char *p);

/* Re-read playlists and match their entries to tracks: the exact path,
 * else the tracks of a file a CUE sheet splits, else loose_match's. Read
 * again when their file changed, or when tracks did (an entry may now be
 * found, or gone); otherwise kept as they are, with nothing written. Each
 * one read is written in its batch. */
static void scan_playlists(library_t *lib, pathvec_t *plv, int tracks_changed, batch_t *b, int64_t pl_before)
{
    sqlite3 *db = lib->db;
    qsort(plv->v, (size_t)plv->n, sizeof *plv->v, cmp_pent);
    sqlite3_stmt *exact = prep(db, "SELECT id FROM tracks WHERE path=?");
    /* an entry naming a file a CUE sheet splits (a playlist made before, or
     * elsewhere): its tracks, in order */
    sqlite3_stmt *parts = prep(db, "SELECT id FROM tracks WHERE path>=?1||'::' AND path<?1||':;' AND cue>0 ORDER BY path");
    sqlite3_stmt *known = prep(db, "SELECT 1 FROM playlists WHERE path=?1 AND mtime=?2 AND size=?3");
    sqlite3_stmt *upl = prep(db,
        "INSERT INTO playlists(path, name, sort, seen, writable, mtime, size) VALUES(?,?,?,1,?,?,?)"
        " ON CONFLICT(path) DO UPDATE SET name=excluded.name, sort=excluded.sort, seen=1, writable=excluded.writable,"
        " mtime=excluded.mtime, size=excluded.size");
    sqlite3_stmt *pid = prep(db, "SELECT id FROM playlists WHERE path=?");
    sqlite3_stmt *clear = prep(db, "DELETE FROM playlist_items WHERE playlist_id=?");
    sqlite3_stmt *add = prep(db, "INSERT INTO playlist_items(playlist_id, pos, track_id) VALUES(?,?,?)");
    sqlite3_stmt *count = prep(db, "UPDATE playlists SET ntracks=?, nmissing=? WHERE id=?");
    int ok = exact && parts && known && upl && pid && clear && add && count;

    for (int i = 0; ok && i < plv->n && !lib->cancel; i++) {
        pent_t *pe = &plv->v[i];
        if (!tracks_changed) {
            sqlite3_reset(known);
            sqlite3_bind_text(known, 1, pe->p, -1, SQLITE_STATIC);
            sqlite3_bind_int64(known, 2, pe->mtime);
            sqlite3_bind_int64(known, 3, pe->size);
            int same = sqlite3_step(known) == SQLITE_ROW;
            sqlite3_reset(known);
            if (same) { pe->kept = 1; continue; }   /* as it was */
        }
        m3u_t m;
        if (m3u_read(pe->p, &m)) continue;
        /* each entry's track (> 0), -1 for a split file's tracks, 0 not found */
        int64_t *match = calloc((size_t)m.n + 1, sizeof *match);
        int *ent = malloc(LOOSE_CHUNK * sizeof *ent);
        if (!match || !ent) { free(match); free(ent); m3u_free(&m); continue; }
        int nl = 0;
        for (int j = 0; j < m.n; j++) {
            sqlite3_reset(exact);
            sqlite3_bind_text(exact, 1, m.paths[j], -1, SQLITE_STATIC);
            match[j] = sqlite3_step(exact) == SQLITE_ROW ? sqlite3_column_int64(exact, 0) : 0;
            sqlite3_reset(exact);
            if (!match[j]) {
                sqlite3_reset(parts);
                sqlite3_bind_text(parts, 1, m.paths[j], -1, SQLITE_STATIC);
                if (sqlite3_step(parts) == SQLITE_ROW) match[j] = -1;
                sqlite3_reset(parts);
            }
            if (!match[j]) ent[nl++] = j;
            if (nl == LOOSE_CHUNK || (nl && j == m.n - 1)) {
                loose_match(db, &m, ent, nl, match);
                nl = 0;
            }
        }
        char sort[256];
        const char *base = strrchr(pe->p, '/');
        sort_key(base ? base + 1 : pe->p, sort, sizeof sort);
        batch_write(b);
        sqlite3_reset(upl);
        sqlite3_bind_text(upl, 1, pe->p, -1, SQLITE_STATIC);
        sqlite3_bind_text(upl, 2, m.name, -1, SQLITE_STATIC);
        sqlite3_bind_text(upl, 3, sort, -1, SQLITE_STATIC);
        size_t dl = strlen(lib->pl_dir);
        sqlite3_bind_int(upl, 4, dl && !strncmp(pe->p, lib->pl_dir, dl) && pe->p[dl] == '/');
        sqlite3_bind_int64(upl, 5, pe->mtime);
        sqlite3_bind_int64(upl, 6, pe->size);
        sqlite3_step(upl);
        sqlite3_reset(upl);
        sqlite3_reset(pid);
        sqlite3_bind_text(pid, 1, pe->p, -1, SQLITE_STATIC);
        int64_t id = sqlite3_step(pid) == SQLITE_ROW ? sqlite3_column_int64(pid, 0) : 0;
        sqlite3_reset(pid);
        if (id) {
            pe->kept = 1;
            sqlite3_reset(clear);
            sqlite3_bind_int64(clear, 1, id);
            sqlite3_step(clear);
            int found = 0, missing = 0;
            for (int j = 0; j < m.n; j++) {
                if (match[j] < 0) {             /* a split file: its tracks */
                    sqlite3_reset(parts);
                    sqlite3_bind_text(parts, 1, m.paths[j], -1, SQLITE_STATIC);
                    while (sqlite3_step(parts) == SQLITE_ROW) {
                        sqlite3_reset(add);
                        sqlite3_bind_int64(add, 1, id);
                        sqlite3_bind_int(add, 2, found++);
                        sqlite3_bind_int64(add, 3, sqlite3_column_int64(parts, 0));
                        sqlite3_step(add);
                    }
                    sqlite3_reset(parts);
                    continue;
                }
                if (!match[j]) {
                    if (missing++ < 3) fprintf(stderr, "playlist %s: not in the library: %s\n", m.name, m.paths[j]);
                    continue;
                }
                sqlite3_reset(add);
                sqlite3_bind_int64(add, 1, id);
                sqlite3_bind_int(add, 2, found++);
                sqlite3_bind_int64(add, 3, match[j]);
                sqlite3_step(add);
            }
            sqlite3_reset(add);
            if (missing) fprintf(stderr, "playlist %s: %d of %d entries not found\n", m.name, missing, m.n);
            sqlite3_reset(count);
            sqlite3_bind_int(count, 1, found);
            sqlite3_bind_int(count, 2, missing);
            sqlite3_bind_int64(count, 3, id);
            sqlite3_step(count);
            sqlite3_reset(count);
        }
        batch_end(b);                           /* (before the next is read) */
        free(match);
        free(ent);
        m3u_free(&m);
    }
    sqlite3_finalize(exact); sqlite3_finalize(parts); sqlite3_finalize(known); sqlite3_finalize(upl); sqlite3_finalize(pid);
    sqlite3_finalize(clear); sqlite3_finalize(add); sqlite3_finalize(count);
    if (!ok || plv->failed || lib->cancel) return;   /* (stopped: not all of them looked at) */
    /* Playlists gone: not found by the walk, or found and no longer
     * readable. One the walk didn't see whose file is there was made on the
     * player meanwhile (after the walk): it stays; and so does one made
     * since the scan began, its file perhaps not written yet (the player
     * adds its row first): the next scan sees it. */
    sqlite3_stmt *all = prep(db, "SELECT id, path FROM playlists");
    int64_t *gone = NULL;
    int ng = 0, cap = 0;
    while (all && sqlite3_step(all) == SQLITE_ROW) {
        const char *path = (const char *)sqlite3_column_text(all, 1);
        int j = path ? pv_find(plv, path) : -1;
        struct stat st;
        if (j >= 0 ? plv->v[j].kept : (path && stat(path, &st) == 0)) continue;
        if (j < 0 && sqlite3_column_int64(all, 0) > pl_before) continue;
        if (ng == cap) {
            int64_t *ngone = realloc(gone, (size_t)(cap = cap ? cap * 2 : 16) * sizeof *gone);
            if (!ngone) break;
            gone = ngone;
        }
        gone[ng++] = sqlite3_column_int64(all, 0);
    }
    sqlite3_finalize(all);
    if (ng) {
        sqlite3_stmt *del = prep(db, "DELETE FROM playlists WHERE id=?");
        sqlite3_stmt *deli = prep(db, "DELETE FROM playlist_items WHERE playlist_id=?");
        batch_write(b);
        for (int k = 0; del && deli && k < ng; k++) {
            sqlite3_reset(del);
            sqlite3_bind_int64(del, 1, gone[k]);
            sqlite3_step(del);
            sqlite3_reset(deli);
            sqlite3_bind_int64(deli, 1, gone[k]);
            sqlite3_step(deli);
        }
        sqlite3_finalize(del);
        sqlite3_finalize(deli);
        batch_end(b);
    }
    free(gone);
}

/* ---------- CUE sheets ---------- */

/* tags_v: 1 = the composer read; 2 = and the tags looked at for a CUE sheet */
#define TAGS_V 2
/* The tag reader's version, noted with a file it couldn't read (cues kind 3,
 * nfiles): raised when it reads more (2: ID3v2.2, extended headers, ID3v1),
 * those files are tried again. */
#define TAGS_READER 2

/* A file tags_read() failed on that is there and can be read: its format
 * then (noted, not read again), not the card or the memory (tried again
 * at the next scan). */
static int readable(const char *path)
{
    FILE *f = fopen(path, "rbe");
    char b[16];
    int ok = f && fread(b, 1, sizeof b, f) > 0;
    if (f) fclose(f);
    return ok;
}

/* Formats whose tags can hold a CUE sheet: files read before tags_v 2 are
 * read again for it, once. */
static int cue_in_tags(const char *path)
{
    const char *dot = strrchr(path, '.');
    return dot && (!strcasecmp(dot, ".flac") || !strcasecmp(dot, ".wv") || !strcasecmp(dot, ".ape"));
}

static int pv_find(const pathvec_t *pv, const char *p)
{
    pent_t k = {.p = (char *)p, .cue = -1};
    const pent_t *hit = pv->n ? bsearch(&k, pv->v, (size_t)pv->n, sizeof *pv->v, cmp_pent) : NULL;
    return hit ? (int)(hit - pv->v) : -1;
}

/* The first entry not before `p` (sorted). */
static int pv_lower(const pathvec_t *pv, const char *p)
{
    int lo = 0, hi = pv->n;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (strcmp(pv->v[mid].p, p) < 0) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

/* The music file a sheet's FILE line names, among the walk's (sorted): its
 * index, or -1. The name as written; else the same name with another
 * extension (a sheet made for the WAV rip, kept with the FLAC); else, for a
 * sheet of one file, the file named like the sheet. Case ignored, as FAT does;
 * a Windows path's folders (C:\Rips\...) are not ours: just its name. */
static int cue_resolve(const pathvec_t *pv, const char *sheet, const char *name, int single)
{
    char want[300], dir[1400];
    snprintf(want, sizeof want, "%s", name);
    for (char *c = want; *c; c++)
        if (*c == '\\') *c = '/';
    const char *sl = strrchr(sheet, '/');
    int dl = sl ? (int)(sl - sheet) : 0;
    char *slash = strrchr(want, '/');
    const char *base = slash ? slash + 1 : want;
    if (want[0] == '/' || (want[0] && want[1] == ':')) {
        if (!slash && want[1] == ':') base = want + 2;
        snprintf(dir, sizeof dir, "%.*s", dl, sheet);
    } else if (slash) {
        *slash = '\0';                               /* a folder below the sheet's */
        snprintf(dir, sizeof dir, "%.*s/%s", dl, sheet, want);
        path_normalize(dir);
    } else {
        snprintf(dir, sizeof dir, "%.*s", dl, sheet);
    }
    size_t pl = strlen(dir);
    if (pl + 2 >= sizeof dir || !*base) return -1;
    dir[pl++] = '/';
    dir[pl] = '\0';
    const char *bdot = strrchr(base, '.');
    size_t stem = bdot ? (size_t)(bdot - base) : strlen(base);
    const char *cname = sl ? sl + 1 : sheet, *cdot = strrchr(cname, '.');
    size_t cstem = cdot ? (size_t)(cdot - cname) : strlen(cname);
    int best = -1, rank = 9;
    for (int j = pv_lower(pv, dir); j < pv->n && !strncmp(pv->v[j].p, dir, pl); j++) {
        const char *f = pv->v[j].p + pl;
        if (strchr(f, '/')) continue;                /* in a folder further down */
        if (!strcasecmp(f, base)) return j;
        const char *dot = strrchr(f, '.');
        size_t fl = dot ? (size_t)(dot - f) : strlen(f);
        if (rank > 1 && fl == stem && !strncasecmp(f, base, fl)) { best = j; rank = 1; }
        else if (single && rank > 2 && fl == cstem && !strncasecmp(f, cname, fl)) { best = j; rank = 2; }
    }
    return best;
}

/* The scan's statements for CUE sheets. */
typedef struct {
    sqlite3_stmt *known, *split, *put_sheet, *plain;   /* the .cue files */
    sqlite3_stmt *look, *parts;                /* a split music file, unchanged */
    sqlite3_stmt *put_file, *up, *id;          /* ...or written again */
    idset_t *seen;                             /* the tracks found (library_scan) */
} cue_stmts_t;

static int cue_stmts_prep(sqlite3 *db, cue_stmts_t *c)
{
    c->known = prep(db, "SELECT mtime, size, nfiles FROM cues WHERE path=? AND kind=0");
    c->split = prep(db, "SELECT path FROM cues WHERE kind=1 AND sheet=?");
    c->plain = prep(db, "SELECT 1 FROM tracks WHERE path=?1 AND cue=0 AND mtime=?2 AND size=?3"
                        " UNION ALL SELECT 1 FROM cues WHERE path=?1 AND kind=3 AND mtime=?2 AND size=?3");
    c->put_sheet = prep(db,
        "INSERT INTO cues(path, mtime, size, kind, sheet, nfiles, seen) VALUES(?,?,?,0,NULL,?,1)"
        " ON CONFLICT(path) DO UPDATE SET mtime=excluded.mtime, size=excluded.size, kind=0, sheet=NULL,"
        " nfiles=excluded.nfiles, seen=1");
    c->look = prep(db, "SELECT mtime, size, kind, sheet, nfiles FROM cues WHERE path=?");
    /* a file's tracks: their paths are the file's and then "::" (cue.h) */
    c->parts = prep(db, "SELECT id FROM tracks WHERE path>=?1||'::' AND path<?1||':;' AND cue>0");
    c->id = prep(db, "SELECT id FROM tracks WHERE path=?");
    c->put_file = prep(db,
        "INSERT INTO cues(path, mtime, size, kind, sheet, nfiles, seen) VALUES(?,?,?,?,?,?,1)"
        " ON CONFLICT(path) DO UPDATE SET mtime=excluded.mtime, size=excluded.size, kind=excluded.kind,"
        " sheet=excluded.sheet, nfiles=excluded.nfiles, seen=1");
    /* As a file's upsert, but a measured gain stays while it's the same part
     * of the same file (the path says the part; date and size are the file's)
     * and the sheet doesn't give one now: an edited title costs nothing. */
#define CUE_KEEP_RG "(tracks.mtime IS excluded.mtime AND tracks.size IS excluded.size AND tracks.rg_state IN (2,3)" \
                    " AND excluded.rg_state=0)"
    c->up = prep(db,
        "INSERT INTO tracks(path, mtime, size, title, artist, album_artist, album, genre, year, disc, track,"
        " codec, rate, bits, channels, duration_ms, has_pic, seen,"
        " rg_state, rg_track, rg_track_peak, rg_album, rg_album_peak, composer, tags_v, cue, sort)"
        " VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,1,?,?,?,?,?,?,2,?,songkey(?4))"
        " ON CONFLICT(path) DO UPDATE SET mtime=excluded.mtime, size=excluded.size, title=excluded.title,"
        " artist=excluded.artist, album_artist=excluded.album_artist, album=excluded.album,"
        " genre=excluded.genre, year=excluded.year, disc=excluded.disc, track=excluded.track,"
        " codec=excluded.codec, rate=excluded.rate, bits=excluded.bits, channels=excluded.channels,"
        " duration_ms=excluded.duration_ms, has_pic=excluded.has_pic, seen=1,"
        " rg_state=CASE WHEN " CUE_KEEP_RG " THEN rg_state ELSE excluded.rg_state END,"
        " rg_track=CASE WHEN " CUE_KEEP_RG " THEN rg_track ELSE excluded.rg_track END,"
        " rg_track_peak=CASE WHEN " CUE_KEEP_RG " THEN rg_track_peak ELSE excluded.rg_track_peak END,"
        " rg_album=CASE WHEN " CUE_KEEP_RG " THEN rg_album ELSE excluded.rg_album END,"
        " rg_album_peak=CASE WHEN " CUE_KEEP_RG " THEN rg_album_peak ELSE excluded.rg_album_peak END,"
        " composer=excluded.composer, tags_v=2, cue=excluded.cue, sort=excluded.sort, search=NULL");
#undef CUE_KEEP_RG
    return c->known && c->split && c->put_sheet && c->plain && c->look && c->parts && c->put_file && c->up && c->id ? 0 : -1;
}

static void cue_stmts_free(cue_stmts_t *c)
{
    sqlite3_finalize(c->known); sqlite3_finalize(c->split); sqlite3_finalize(c->put_sheet); sqlite3_finalize(c->plain);
    sqlite3_finalize(c->look); sqlite3_finalize(c->parts); sqlite3_finalize(c->id);
    sqlite3_finalize(c->put_file); sqlite3_finalize(c->up);
}

/* The sheet's buffer, made the first time one is read (65 KB: not on a thread's stack). */
static cue_sheet_t *sheet_buf(cue_sheet_t **s)
{
    if (!*s) *s = malloc(sizeof **s);
    return *s;
}

/* Which music files the .cue files split into tracks: each one's `cue` (in
 * pv) is the sheet's index in cv; a sheet's own `cue` says whether it changed.
 * A sheet as it was isn't read again: what it split is in `cues` (read again
 * only if fewer files than it names are there: one may have arrived since;
 * one there before, as it was, that stayed a track of its own, stays so).
 * Nothing is written here: a sheet's row is, at the end (cue_put_sheets),
 * once the files it splits are, or a scan stopped half way would leave a
 * changed sheet noted as it is, its tracks as they were. */
static void cue_claim(cue_stmts_t *c, pathvec_t *pv, pathvec_t *cv, cue_sheet_t **sheet)
{
    for (int k = 0; k < cv->n; k++) {
        pent_t *e = &cv->v[k];
        int same = 0, want = 0, got = 0, had = -1;
        sqlite3_reset(c->known);
        sqlite3_bind_text(c->known, 1, e->p, -1, SQLITE_STATIC);
        if (sqlite3_step(c->known) == SQLITE_ROW && sqlite3_column_int64(c->known, 0) == e->mtime &&
            sqlite3_column_int64(c->known, 1) == e->size) {
            same = 1;
            want = had = sqlite3_column_int(c->known, 2);
            sqlite3_reset(c->split);
            sqlite3_bind_text(c->split, 1, e->p, -1, SQLITE_STATIC);
            while (sqlite3_step(c->split) == SQLITE_ROW) {
                int j = pv_find(pv, (const char *)sqlite3_column_text(c->split, 0));
                if (j >= 0 && pv->v[j].cue < 0) { pv->v[j].cue = k; got++; }
            }
            sqlite3_reset(c->split);
        }
        sqlite3_reset(c->known);
        if ((!same || got < want) && sheet_buf(sheet) && cue_read(e->p, *sheet) > 0) {
            cue_sheet_t *s = *sheet;
            want = 0;
            for (int f = 0; f < s->nfiles; f++) {
                if (cue_file_tracks(s, f) < 2) continue;   /* a sheet for one track per file: they're tracks already */
                want++;
                int j = cue_resolve(pv, e->p, s->files[f], s->nfiles == 1);
                if (j < 0 || pv->v[j].cue >= 0) continue;
                if (same) {
                    sqlite3_reset(c->plain);
                    sqlite3_bind_text(c->plain, 1, pv->v[j].p, -1, SQLITE_STATIC);
                    sqlite3_bind_int64(c->plain, 2, pv->v[j].mtime);
                    sqlite3_bind_int64(c->plain, 3, pv->v[j].size);
                    int plain = sqlite3_step(c->plain) == SQLITE_ROW;
                    sqlite3_reset(c->plain);
                    if (plain) continue;
                }
                pv->v[j].cue = k;
            }
        }
        e->cue = !same;
        e->want = want;
        e->put = !same || want != had;
    }
}

/* The sheets' rows that changed (cue_claim), in the scan's last batch. */
static void cue_put_sheets(cue_stmts_t *c, const pathvec_t *cv)
{
    for (int k = 0; k < cv->n; k++) {
        const pent_t *e = &cv->v[k];
        if (!e->put) continue;
        sqlite3_reset(c->put_sheet);
        sqlite3_bind_text(c->put_sheet, 1, e->p, -1, SQLITE_STATIC);
        sqlite3_bind_int64(c->put_sheet, 2, e->mtime);
        sqlite3_bind_int64(c->put_sheet, 3, e->size);
        sqlite3_bind_int(c->put_sheet, 4, e->want);
        sqlite3_step(c->put_sheet);
        sqlite3_reset(c->put_sheet);
    }
}

/* Track `path`, just written: found. */
static void cue_found(cue_stmts_t *c, const char *path)
{
    sqlite3_reset(c->id);
    sqlite3_bind_text(c->id, 1, path, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(c->id) == SQLITE_ROW) idset_add(c->seen, sqlite3_column_int64(c->id, 0));
    sqlite3_reset(c->id);
}

/* A split music file (or one that can't be read: kind 3) as it was at the
 * last scan (`kind` and `sheet` as then): its tracks and its row are kept,
 * nothing written. 1 if so. */
static int cue_unchanged(cue_stmts_t *c, pent_t *f, int kind, const char *sheet)
{
    sqlite3_reset(c->look);
    sqlite3_bind_text(c->look, 1, f->p, -1, SQLITE_STATIC);
    int same = sqlite3_step(c->look) == SQLITE_ROW && sqlite3_column_int64(c->look, 0) == f->mtime &&
               sqlite3_column_int64(c->look, 1) == f->size && sqlite3_column_int(c->look, 2) == kind;
    if (same) {
        const char *was = (const char *)sqlite3_column_text(c->look, 3);
        same = sheet ? was && !strcmp(was, sheet) : was == NULL;
        if (kind == 3 && sqlite3_column_int(c->look, 4) < TAGS_READER) same = 0;   /* (a newer reader may manage) */
    }
    sqlite3_reset(c->look);
    if (!same) return 0;
    f->kept = 1;
    sqlite3_reset(c->parts);
    sqlite3_bind_text(c->parts, 1, f->p, -1, SQLITE_STATIC);
    while (sqlite3_step(c->parts) == SQLITE_ROW) idset_add(c->seen, sqlite3_column_int64(c->parts, 0));
    sqlite3_reset(c->parts);
    return 1;
}

/* A music file whose tags can't be read (a damaged or half-copied file):
 * noted, so it's not read again while it's the same, and the fast walk's
 * list still matches the library (that never matched again: one such file
 * meant the slow walk at every scan). */
static void cue_unreadable(cue_stmts_t *c, pent_t *f)
{
    fprintf(stderr, "library: can't read %s\n", f->p);
    sqlite3_reset(c->put_file);
    sqlite3_bind_text(c->put_file, 1, f->p, -1, SQLITE_STATIC);
    sqlite3_bind_int64(c->put_file, 2, f->mtime);
    sqlite3_bind_int64(c->put_file, 3, f->size);
    sqlite3_bind_int(c->put_file, 4, 3);
    sqlite3_bind_null(c->put_file, 5);
    sqlite3_bind_int(c->put_file, 6, TAGS_READER);
    sqlite3_step(c->put_file);
    sqlite3_reset(c->put_file);
    f->kept = 1;
}

static void bind_tags(sqlite3_stmt *up, const char *path, const pent_t *f, const tags_t *t);

/* The tracks of file `fi` of the sheet, as parts of the music file f (its
 * tags in t: the format, the length, and what the sheet doesn't say). The
 * number written, or -1 if fewer than two can be played: then the file stays
 * one track. */
static int cue_write(cue_stmts_t *c, pent_t *f, const cue_sheet_t *s, int fi, const tags_t *t,
                     int kind, const char *sheet)
{
    int idx[CUE_MAX_TRACKS], n = 0;
    uint64_t len = (uint64_t)t->duration_ms * CUE_FPS / 1000;   /* the file's, in CD frames (0: unknown) */
    for (int i = 0; i < s->n; i++) {
        const cue_track_t *ct = &s->tracks[i];
        if (ct->file != fi || (len && ct->start >= len)) continue;   /* past the end: a sheet for another rip */
        if (n && ct->start <= s->tracks[idx[n - 1]].start) continue;
        idx[n++] = i;
    }
    if (n < 2) return -1;
    /* One album: the sheet's performer, else the file's, else (tracks by
     * different performers) a compilation. */
    const char *aa = s->performer[0] ? s->performer : t->album_artist[0] ? t->album_artist : t->artist;
    int mixed = 0;
    for (int k = 1; k < n && !*aa; k++) mixed |= strcmp(s->tracks[idx[k]].performer, s->tracks[idx[0]].performer) != 0;
    if (!*aa && mixed) aa = "Various Artists";
    tags_t tt;
    char path[1400];
    for (int k = 0; k < n; k++) {
        const cue_track_t *ct = &s->tracks[idx[k]];
        uint32_t end = k + 1 < n ? s->tracks[idx[k + 1]].start : 0;
        uint64_t from_ms = (uint64_t)ct->start * 1000 / CUE_FPS;
        tt = *t;
        if (ct->title[0]) snprintf(tt.title, sizeof tt.title, "%s", ct->title);
        else snprintf(tt.title, sizeof tt.title, "Track %d", ct->number);
        snprintf(tt.artist, sizeof tt.artist, "%s", ct->performer[0] ? ct->performer : s->performer[0] ? s->performer : t->artist);
        snprintf(tt.album_artist, sizeof tt.album_artist, "%s", aa);
        if (s->title[0]) snprintf(tt.album, sizeof tt.album, "%s", s->title);
        if (s->genre[0]) snprintf(tt.genre, sizeof tt.genre, "%s", s->genre);
        if (ct->songwriter[0] || s->songwriter[0])
            snprintf(tt.composer, sizeof tt.composer, "%s", ct->songwriter[0] ? ct->songwriter : s->songwriter);
        if (s->year) tt.year = s->year;
        if (s->disc) tt.disc = s->disc;
        tt.track = ct->number;
        tt.duration_ms = end ? (uint32_t)(((uint64_t)end - ct->start) * 1000 / CUE_FPS)
                             : t->duration_ms > from_ms ? (uint32_t)(t->duration_ms - from_ms) : 0;
        /* the track's gain from the sheet (or measured later); the album's
         * from the sheet, else the file's own: the whole file is the album */
        tt.has_rg_track = ct->has_rg;
        tt.rg_track_gain = ct->rg_gain;
        tt.rg_track_peak = ct->rg_peak;
        if (s->has_rg_album) {
            tt.has_rg_album = 1;
            tt.rg_album_gain = s->rg_album_gain;
            tt.rg_album_peak = s->rg_album_peak;
        } else if (!t->has_rg_album && t->has_rg_track) {
            tt.has_rg_album = 1;
            tt.rg_album_gain = t->rg_track_gain;
            tt.rg_album_peak = t->rg_track_peak;
        }
        cue_part_path(path, sizeof path, f->p, ct->number, ct->start, end);
        sqlite3_reset(c->up);
        bind_tags(c->up, path, f, &tt);
        sqlite3_bind_int(c->up, 24, ct->number);
        sqlite3_step(c->up);
        sqlite3_reset(c->up);
        cue_found(c, path);
    }
    sqlite3_reset(c->put_file);
    sqlite3_bind_text(c->put_file, 1, f->p, -1, SQLITE_STATIC);
    sqlite3_bind_int64(c->put_file, 2, f->mtime);
    sqlite3_bind_int64(c->put_file, 3, f->size);
    sqlite3_bind_int(c->put_file, 4, kind);
    if (sheet) sqlite3_bind_text(c->put_file, 5, sheet, -1, SQLITE_STATIC);
    else sqlite3_bind_null(c->put_file, 5);
    sqlite3_bind_int(c->put_file, 6, 0);
    sqlite3_step(c->put_file);
    sqlite3_reset(c->put_file);
    f->kept = 1;
    return n;
}

/* A music file split by the sheet in its own tags: its tracks (> 0), or -1. */
static int cue_embedded(cue_stmts_t *c, pent_t *f, const tags_t *t, cue_sheet_t **sheet)
{
    char *text;
    size_t len;
    if (!sheet_buf(sheet) || tags_cuesheet(f->p, &text, &len)) return -1;
    int r = cue_parse(text, len, *sheet);
    free(text);
    if (r < 2) return -1;
    for (int i = 0; i < (*sheet)->n; i++) (*sheet)->tracks[i].file = 0;   /* whatever its FILE says: this one */
    return cue_write(c, f, *sheet, 0, t, 2, NULL);
}

/* A music file split by a .cue next to it (sheet `e`): its tracks (> 0), or -1. */
static int cue_external(cue_stmts_t *c, pathvec_t *pv, int i, const pent_t *e, cue_sheet_t **sheet)
{
    cue_sheet_t *s = sheet_buf(sheet);
    if (!s || cue_read(e->p, s) <= 0) return -1;
    for (int fi = 0; fi < s->nfiles; fi++) {
        tags_t t;
        if (cue_file_tracks(s, fi) >= 2 && cue_resolve(pv, e->p, s->files[fi], s->nfiles == 1) == i)
            return tags_read(pv->v[i].p, &t) ? -1 : cue_write(c, &pv->v[i], s, fi, &t, 1, e->p);
    }
    return -1;
}

/* A track's columns 1-23 of the tracks upserts, from its tags. */
static void bind_tags(sqlite3_stmt *up, const char *path, const pent_t *f, const tags_t *t)
{
    sqlite3_bind_text(up, 1, path, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(up, 2, f->mtime);
    sqlite3_bind_int64(up, 3, f->size);
    sqlite3_bind_text(up, 4, t->title, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(up, 5, t->artist, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(up, 6, t->album_artist, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(up, 7, t->album, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(up, 8, t->genre, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(up, 9, t->year);
    sqlite3_bind_int(up, 10, t->disc);
    sqlite3_bind_int(up, 11, t->track);
    sqlite3_bind_text(up, 12, t->codec, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(up, 13, (int)t->sample_rate);
    sqlite3_bind_int(up, 14, t->bits);
    sqlite3_bind_int(up, 15, t->channels);
    sqlite3_bind_int64(up, 16, t->duration_ms);
    sqlite3_bind_int(up, 17, t->has_picture);
    /* tagged gains now; the rest are measured later (rg_state 0) */
    sqlite3_bind_int(up, 18, t->has_rg_track ? 1 : 0);
    if (t->has_rg_track) sqlite3_bind_double(up, 19, t->rg_track_gain); else sqlite3_bind_null(up, 19);
    if (t->has_rg_track && t->rg_track_peak > 0) sqlite3_bind_double(up, 20, t->rg_track_peak); else sqlite3_bind_null(up, 20);
    if (t->has_rg_album) sqlite3_bind_double(up, 21, t->rg_album_gain); else sqlite3_bind_null(up, 21);
    if (t->has_rg_album && t->rg_album_peak > 0) sqlite3_bind_double(up, 22, t->rg_album_peak); else sqlite3_bind_null(up, 22);
    sqlite3_bind_text(up, 23, t->composer, -1, SQLITE_TRANSIENT);
}

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

/* The files a scan reads tags from, read before their batch begins (no lock
 * held while reading the card) and written together. */
#define PEND_MAX 32

enum { PEND_NEW, PEND_LATER, PEND_BAD };

typedef struct {
    int i, kind;   /* the file (in pv); a new or changed track, an old one's later tags, or unreadable */
    int64_t id;    /* PEND_LATER: its track */
    tags_t t;
} pend_t;

typedef struct {
    sqlite3 *db;
    batch_t b;
    idset_t seen;
    cue_stmts_t cs;
    sqlite3_stmt *up, *later;
    pend_t *pend;
    int npend;
    double pend_since;
    int pending, marked;   /* meta.scan_pending at the start; the most this scan noted */
} scan_t;

/* About to change tracks (level 2) or only composers (1): noted first, in
 * the same batch, for a scan that doesn't get to the end. */
static void scan_dirty(scan_t *sc, int level)
{
    batch_write(&sc->b);
    if (level > sc->marked && level > sc->pending) meta_set(sc->db, "scan_pending", level);
    if (level > sc->marked) sc->marked = level;
}

static void scan_flush(scan_t *sc, pathvec_t *pv)
{
    if (!sc->npend) return;
    int level = 0;   /* (unreadable files change no group) */
    for (int k = 0; k < sc->npend; k++) {
        if (sc->pend[k].kind == PEND_NEW) level = 2;
        else if (sc->pend[k].kind == PEND_LATER && level < 1) level = 1;
    }
    batch_write(&sc->b);
    if (level) scan_dirty(sc, level);
    for (int k = 0; k < sc->npend; k++) {
        pend_t *e = &sc->pend[k];
        pent_t *f = &pv->v[e->i];
        if (e->kind == PEND_BAD) {
            cue_unreadable(&sc->cs, f);
        } else if (e->kind == PEND_LATER) {
            sqlite3_reset(sc->later);
            sqlite3_bind_int64(sc->later, 1, e->id);
            sqlite3_bind_text(sc->later, 2, e->t.composer, -1, SQLITE_TRANSIENT);
            sqlite3_step(sc->later);
            sqlite3_reset(sc->later);
        } else {
            sqlite3_reset(sc->up);
            bind_tags(sc->up, f->p, f, &e->t);
            sqlite3_step(sc->up);
            sqlite3_reset(sc->up);
            cue_found(&sc->cs, f->p);
        }
    }
    sc->npend = 0;
    batch_end(&sc->b);
}

/* A slot for file i's tags (the queue written first when full, or once it
 * has waited BATCH_S). */
static pend_t *scan_queue(scan_t *sc, pathvec_t *pv, int i, int kind)
{
    if (sc->npend == PEND_MAX || (sc->npend && now_s() - sc->pend_since >= BATCH_S)) scan_flush(sc, pv);
    if (!sc->npend) sc->pend_since = now_s();
    pend_t *e = &sc->pend[sc->npend++];
    e->i = i;
    e->kind = kind;
    e->id = 0;
    return e;
}

/* What can't wait in the queue (a file split by a CUE sheet): written now,
 * in a batch of its own. */
static void scan_now(scan_t *sc, pathvec_t *pv)
{
    scan_flush(sc, pv);
    scan_dirty(sc, 2);
}

int library_scan(library_t *lib, const char *root, lib_progress_cb cb, void *ctx)
{
    double t0 = now_s(), t_walk, t_files, t_groups, t_pl, t_art;
    pathvec_t pv = {0}, plv = {0}, cv = {0}, iv = {0}, none = {0}, nocue = {0};
    /* No music folder (renamed, or the card's away): every track would look
     * deleted, with its play counts. The library stays as it is instead. */
    struct stat rst;
    if (stat(root, &rst) != 0 || !S_ISDIR(rst.st_mode)) return -1;
    lib->cancel = 0;
    int64_t pl_before = INT64_MAX;                 /* playlists made from here on are kept */
    sqlite3_stmt *plmax = prep(lib->db, "SELECT COALESCE(MAX(id), 0) FROM playlists");
    if (plmax && sqlite3_step(plmax) == SQLITE_ROW) pl_before = sqlite3_column_int64(plmax, 0);
    sqlite3_finalize(plmax);
    int fast = fast_walk(lib, root, &pv, &plv, &cv, &iv);
    if (!fast) walk(root, &pv, &plv, &cv, &iv, 0, &lib->cancel);
    if (lib->pl_dir[0]) walk(lib->pl_dir, &none, &plv, &nocue, NULL, 0, &lib->cancel);   /* your own playlists (no music in there) */
    plv.failed |= pv.failed | cv.failed | none.failed;   /* (playlists: none dropped then) */
    pv_free(&none);
    pv_free(&nocue);
    if (lib->cancel) {
        pv_free(&pv);
        pv_free(&plv);
        pv_free(&cv);
        pv_free(&iv);
        return -1;
    }
    qsort(iv.v, (size_t)iv.n, sizeof *iv.v, cmp_pent);
    qsort(pv.v, (size_t)pv.n, sizeof *pv.v, cmp_pent);
    qsort(cv.v, (size_t)cv.n, sizeof *cv.v, cmp_pent);
    t_walk = now_s();

    /* Nothing is written for a file as it was (the usual case: a scan that
     * finds nothing new writes nothing at all); tracks found are noted in
     * memory, and those not found are dropped only at the end, after a
     * whole walk: a scan stopped half way keeps what it has written, and
     * never drops music. */
    sqlite3 *db = lib->db;
    scan_t sc = {.db = db, .b = {.db = db}};
    sc.pending = meta_get(db, "scan_pending");
    sc.cs.seen = &sc.seen;
    sc.pend = malloc(PEND_MAX * sizeof *sc.pend);
    sqlite3_stmt *look = prep(db, "SELECT id, mtime, size, tags_v FROM tracks WHERE path=? AND cue=0");
    sc.later = prep(db, "UPDATE tracks SET composer=?2, tags_v=2 WHERE id=?1");
    sc.up = prep(db,
        "INSERT INTO tracks(path, mtime, size, title, artist, album_artist, album, genre, year, disc, track,"
        " codec, rate, bits, channels, duration_ms, has_pic, seen,"
        " rg_state, rg_track, rg_track_peak, rg_album, rg_album_peak, composer, tags_v, sort)"
        " VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,1,?,?,?,?,?,?,2,songkey(?4))"
        " ON CONFLICT(path) DO UPDATE SET mtime=excluded.mtime, size=excluded.size, title=excluded.title,"
        " artist=excluded.artist, album_artist=excluded.album_artist, album=excluded.album,"
        " genre=excluded.genre, year=excluded.year, disc=excluded.disc, track=excluded.track,"
        " codec=excluded.codec, rate=excluded.rate, bits=excluded.bits, channels=excluded.channels,"
        " duration_ms=excluded.duration_ms, has_pic=excluded.has_pic,"
        " rg_state=excluded.rg_state, rg_track=excluded.rg_track, rg_track_peak=excluded.rg_track_peak,"
        " rg_album=excluded.rg_album, rg_album_peak=excluded.rg_album_peak,"
        " composer=excluded.composer, tags_v=2, sort=excluded.sort, search=NULL");
    cue_sheet_t *sheet = NULL;
    int ok = look && sc.later && sc.up && sc.pend && cue_stmts_prep(db, &sc.cs) == 0;
    if (ok) cue_claim(&sc.cs, &pv, &cv, &sheet);

    int changed = 0;   /* tracks added, changed or gone */
    int extra = 0;     /* unchanged tracks that got a tag read later (a composer) */
    for (int i = 0; ok && i < pv.n; i++) {
        if (lib->cancel) { ok = 0; break; }
        pent_t *f = &pv.v[i];
        const char *path = f->p;
        int n;
        if (cb && (i % 25) == 0) cb(i + 1, pv.n, path, ctx);
        if (f->cue >= 0) {                     /* split by a .cue */
            const pent_t *e = &cv.v[f->cue];
            if (!e->cue && cue_unchanged(&sc.cs, f, 1, e->p)) continue;
            scan_now(&sc, &pv);
            n = cue_external(&sc.cs, &pv, i, e, &sheet);
            batch_end(&sc.b);
            if (n > 0) { changed += n; continue; }
            /* (not after all: one track, as any file) */
        }
        sqlite3_reset(look);
        sqlite3_bind_text(look, 1, path, -1, SQLITE_STATIC);
        int row = sqlite3_step(look) == SQLITE_ROW;
        int64_t id = row ? sqlite3_column_int64(look, 0) : 0;
        int same = row && sqlite3_column_int64(look, 1) == f->mtime && sqlite3_column_int64(look, 2) == f->size;
        int v = row ? sqlite3_column_int(look, 3) : 0;
        sqlite3_reset(look);
        if (same && (v >= TAGS_V || (v >= 1 && !cue_in_tags(path)))) {
            idset_add(&sc.seen, id);           /* as it was: nothing to write */
            continue;
        }
        if (same) {
            /* unchanged, but read before composers or CUE sheets in tags: those, once */
            pend_t *e = scan_queue(&sc, &pv, i, PEND_LATER);
            e->id = id;
            int got = tags_read(path, &e->t) == 0;
            if (!got) memset(&e->t, 0, sizeof e->t);
            if (got && e->t.has_cuesheet) {
                tags_t t = e->t;
                sc.npend--;                    /* (written now instead) */
                scan_now(&sc, &pv);
                n = cue_embedded(&sc.cs, f, &t, &sheet);
                if (n > 0) {
                    batch_end(&sc.b);
                    changed += n;              /* (and its old row goes) */
                    continue;
                }
                sqlite3_reset(sc.later);
                sqlite3_bind_int64(sc.later, 1, id);
                sqlite3_bind_text(sc.later, 2, t.composer, -1, SQLITE_TRANSIENT);
                sqlite3_step(sc.later);
                sqlite3_reset(sc.later);
                batch_end(&sc.b);
                idset_add(&sc.seen, id);
                if (t.composer[0] && v < 1) extra++;
                continue;
            }
            idset_add(&sc.seen, id);
            if (got && e->t.composer[0] && v < 1) extra++;
            continue;
        }
        if (!row && (cue_unchanged(&sc.cs, f, 2, NULL) || cue_unchanged(&sc.cs, f, 3, NULL)))
            continue;                          /* split by its own sheet, or unreadable, as it was */
        pend_t *e = scan_queue(&sc, &pv, i, PEND_NEW);
        if (tags_read(path, &e->t)) {
            if (readable(path)) { e->kind = PEND_BAD; continue; }
            sc.npend--;                        /* can't be read now (EIO, no memory): next time */
            idset_add(&sc.seen, id);           /* (a track's row stays as it was) */
            continue;
        }
        if (e->t.has_cuesheet) {
            tags_t t = e->t;
            sc.npend--;
            scan_now(&sc, &pv);
            n = cue_embedded(&sc.cs, f, &t, &sheet);
            batch_end(&sc.b);
            if (n > 0) { changed += n; continue; }
            e = scan_queue(&sc, &pv, i, PEND_NEW);
            e->t = t;
        }
        changed++;
    }
    if (sc.pend) scan_flush(&sc, &pv);         /* (stopped too: what's read is kept) */
    batch_end(&sc.b);
    free(sheet);
    sqlite3_finalize(look);
    if (!ok) {   /* stopped (or the database can't be read): nothing dropped */
        if (sc.marked) fprintf(stderr, "library: scan stopped, %d tracks written, the rest next time\n", changed);
        sqlite3_finalize(sc.later);
        sqlite3_finalize(sc.up);
        cue_stmts_free(&sc.cs);
        free(sc.pend);
        free(sc.seen.bits);
        pv_free(&cv);
        pv_free(&pv);
        pv_free(&plv);
        pv_free(&iv);
        return -1;
    }

    /* Tracks not found, and `cues` rows no longer true (a whole walk only) */
    int64_t *gone = NULL;
    int ngone = 0, gcap = 0;
    char **cgone = NULL;
    int ncgone = 0, ccap = 0;
    if (!pv.failed && !cv.failed && !sc.seen.failed) {
        sqlite3_stmt *all = prep(db, "SELECT id FROM tracks");
        while (all && sqlite3_step(all) == SQLITE_ROW) {
            int64_t id = sqlite3_column_int64(all, 0);
            if (idset_has(&sc.seen, id)) continue;
            if (ngone == gcap) {
                int64_t *ng = realloc(gone, (size_t)(gcap = gcap ? gcap * 2 : 64) * sizeof *gone);
                if (!ng) { ngone = -1; break; }
                gone = ng;
            }
            gone[ngone++] = id;
        }
        sqlite3_finalize(all);
        all = prep(db, "SELECT path, kind FROM cues");
        while (ngone >= 0 && all && sqlite3_step(all) == SQLITE_ROW) {
            const char *p = (const char *)sqlite3_column_text(all, 0);
            int j = p ? pv_find(sqlite3_column_int(all, 1) == 0 ? &cv : &pv, p) : -1;
            if (j >= 0 && (sqlite3_column_int(all, 1) == 0 || pv.v[j].kept)) continue;
            if (ncgone == ccap) {
                char **ng = realloc(cgone, (size_t)(ccap = ccap ? ccap * 2 : 16) * sizeof *cgone);
                if (!ng) break;
                cgone = ng;
            }
            if ((cgone[ncgone] = strdup(p ? p : ""))) ncgone++;
        }
        sqlite3_finalize(all);
        if (ngone < 0) ngone = 0;              /* (out of memory: none dropped this time) */
    }
    int sheets = 0;
    for (int k = 0; k < cv.n; k++) sheets |= cv.v[k].put;
    changed += ngone;
    /* albums, artists, credits, search and covers only follow the tracks:
     * nothing to do if no track changed (the usual case), or a scan before
     * this one wrote some and stopped */
    int groups = changed || sc.pending >= 2 || sc.marked >= 2;
    int composers = !groups && (extra || sc.pending == 1);
    t_files = now_s();
    if (ngone || ncgone || sheets || groups || composers) {
        sqlite3_stmt *del = prep(db, "DELETE FROM tracks WHERE id=?");
        sqlite3_stmt *delc = prep(db, "DELETE FROM cues WHERE path=?");
        batch_write(&sc.b);                    /* one batch: the groups end as the tracks are */
        if (!sc.b.open) {                      /* (the lock not had in 10 s: all of it next time) */
            fprintf(stderr, "library: scan: can't write, the rest next time\n");
            lib->cancel = 1;                   /* (returns -1, as stopped) */
            ngone = ncgone = groups = composers = 0;
            for (int k = 0; k < cv.n; k++) cv.v[k].put = 0;
        }
        if (groups) scan_dirty(&sc, 2);        /* (noted first: playlists, after this batch, follow) */
        for (int k = 0; del && k < ngone; k++) {
            sqlite3_reset(del);
            sqlite3_bind_int64(del, 1, gone[k]);
            sqlite3_step(del);
        }
        for (int k = 0; delc && k < ncgone; k++) {
            sqlite3_reset(delc);
            sqlite3_bind_text(delc, 1, cgone[k], -1, SQLITE_STATIC);
            sqlite3_step(delc);
        }
        sqlite3_finalize(del);
        sqlite3_finalize(delc);
        cue_put_sheets(&sc.cs, &cv);
        if (!groups && composers) rebuild_composers(db);
        batch_end(&sc.b);
        /* the groups follow (scan_pending says so until then): made aside,
         * the lock held only to put them in; else in one batch, as before.
         * Meanwhile a new track can show in Songs before its album, and a
         * removed album for a moment with no songs. */
        if (groups && !lib->cancel && rebuild_groups_aside(lib, &sc.b) != 0) {
            batch_write(&sc.b);
            if (sc.b.open) {
                rebuild_groups(lib);
                index_search(lib, 1);          /* changed tracks (their search cleared), new albums and artists */
            }
            batch_end(&sc.b);
        }
    }
    free(gone);
    for (int k = 0; k < ncgone; k++) free(cgone[k]);
    free(cgone);
    lib->scan_changes = changed + extra + (sc.pending > 0);
    sqlite3_finalize(sc.later);
    sqlite3_finalize(sc.up);
    cue_stmts_free(&sc.cs);
    free(sc.pend);
    free(sc.seen.bits);
    pv_free(&cv);
    t_groups = now_s();
    scan_playlists(lib, &plv, groups, &sc.b, pl_before);
    pv_free(&plv);
    if ((sc.pending || sc.marked) && !lib->cancel) {   /* all done (stopped: playlists again next time) */
        batch_write(&sc.b);
        meta_set(db, "scan_pending", 0);
        batch_end(&sc.b);
    }
    t_pl = now_s();

    /* every scan: new albums, and any whose folder's images or tracks changed
     * (an image added or replaced doesn't change any track) */
    if (!lib->cancel) build_art(lib, &iv, &sc.b, cb, ctx);
    pv_free(&iv);
    t_art = now_s();
    fprintf(stderr, "library: scan %.1f s, %d changes: %swalk %.1f, files %.1f, groups %.1f, playlists %.1f, art %.1f\n",
            t_art - t0, changed, fast ? "fast " : "", t_walk - t0, t_files - t_walk, t_groups - t_files, t_pl - t_groups,
            t_art - t_pl);

    pv_free(&pv);

    if (lib->cancel) return -1;                /* (stopped while making playlists or covers) */
    sqlite3_stmt *c = prep(db, "SELECT COUNT(*) FROM tracks");
    int n = (c && sqlite3_step(c) == SQLITE_ROW) ? sqlite3_column_int(c, 0) : -1;
    sqlite3_finalize(c);
    if (cb) cb(n, n, "done", ctx);
    return n;
}

/* ---------- queries ---------- */

static int64_t g_credit_album;   /* LIB_CREDIT_TRACKS: the album (library_list takes one parent) */

int library_list(library_t *lib, lib_view_t view, int64_t parent, lib_list_t *out)
{
    const char *sql = NULL;
    switch (view) {
    case LIB_ALBUMS: sql = "SELECT id FROM albums ORDER BY sort, year"; break;
    case LIB_ALBUM_ARTISTS: sql = "SELECT id FROM artists WHERE nalbums>0 ORDER BY sort"; break;
    case LIB_ARTISTS: sql = "SELECT id FROM artists WHERE ncredit_albums>0 ORDER BY sort"; break;
    case LIB_CREDIT_ALBUMS:
        sql = "SELECT id FROM albums WHERE id IN (SELECT t.album_id FROM credits c JOIN tracks t ON t.id=c.track_id"
              " WHERE c.artist_id=?1) ORDER BY year, sort";
        break;
    case LIB_CREDIT_TRACKS:   /* library_list_credit_tracks() */
        sql = "SELECT t.id FROM tracks t JOIN credits c ON c.track_id=t.id WHERE c.artist_id=?1 AND t.album_id=?2"
              " ORDER BY t.disc, t.track, t.path";
        break;
    case LIB_ARTIST_ALBUMS: sql = "SELECT id FROM albums WHERE artist_id=?1 ORDER BY year, sort"; break;
    case LIB_ALBUM_TRACKS: sql = "SELECT id FROM tracks WHERE album_id=?1 ORDER BY disc, track, path"; break;
    case LIB_SONGS: sql = "SELECT id FROM tracks ORDER BY sort, title COLLATE NOCASE"; break;
    case LIB_PLAYLISTS: sql = "SELECT id FROM playlists ORDER BY sort"; break;
    case LIB_PLAYLIST_TRACKS: sql = "SELECT track_id FROM playlist_items WHERE playlist_id=?1 ORDER BY pos"; break;
    case LIB_FAVOURITES: sql = "SELECT id FROM tracks WHERE favourite=1 ORDER BY sort, title COLLATE NOCASE"; break;
    case LIB_RECENT_PLAYED: sql = "SELECT id FROM tracks WHERE last_played>0 ORDER BY last_played DESC LIMIT 200"; break;
    case LIB_MOST_PLAYED:
        sql = "SELECT id FROM tracks WHERE play_count>0 ORDER BY play_count DESC, last_played DESC LIMIT 200";
        break;
    case LIB_RECENT_ADDED:
        sql = "SELECT album_id FROM tracks WHERE album_id IS NOT NULL GROUP BY album_id ORDER BY MAX(mtime) DESC LIMIT 100";
        break;
    case LIB_GENRES: sql = "SELECT id FROM genres ORDER BY sort"; break;
    case LIB_GENRE_ALBUMS:
        sql = "SELECT id FROM albums WHERE id IN (SELECT album_id FROM tracks WHERE genre_id=?1) ORDER BY sort, year";
        break;
    case LIB_COMPOSERS: sql = "SELECT id FROM composers ORDER BY sort"; break;
    case LIB_COMPOSER_ALBUMS:
        sql = "SELECT id FROM albums WHERE id IN (SELECT album_id FROM tracks WHERE composer_id=?1) ORDER BY year, sort";
        break;
    case LIB_COMPOSER_TRACKS:   /* library_list_composer_tracks() */
        sql = "SELECT id FROM tracks WHERE composer_id=?1 AND album_id=?2 ORDER BY disc, track, path";
        break;
    case LIB_YEARS: sql = "SELECT DISTINCT year FROM albums WHERE year>0 ORDER BY year DESC"; break;
    case LIB_YEAR_ALBUMS: sql = "SELECT id FROM albums WHERE year=?1 ORDER BY sort"; break;
    case LIB_FOLDER:   /* the subfolders (as -id), then the songs; 0: the top folder's */
        sql = "SELECT x FROM (SELECT -id AS x, 0 AS k, sort AS o FROM folders WHERE parent=?1"
              " UNION ALL SELECT id, 1, path FROM tracks WHERE folder_id=?1) ORDER BY k, o";
        if (parent == 0) {
            sqlite3_stmt *top = prep(lib->db, "SELECT id FROM folders WHERE parent=0 ORDER BY length(path) LIMIT 1");
            if (top && sqlite3_step(top) == SQLITE_ROW) parent = sqlite3_column_int64(top, 0);
            sqlite3_finalize(top);
        }
        break;
    }
    memset(out, 0, sizeof *out);
    out->view = view;
    sqlite3_stmt *s = prep(lib->db, sql);
    if (!s) return -1;
    if (view == LIB_ARTIST_ALBUMS || view == LIB_ALBUM_TRACKS || view == LIB_PLAYLIST_TRACKS ||
        view == LIB_CREDIT_ALBUMS || view == LIB_CREDIT_TRACKS || view == LIB_GENRE_ALBUMS ||
        view == LIB_COMPOSER_ALBUMS || view == LIB_COMPOSER_TRACKS || view == LIB_YEAR_ALBUMS || view == LIB_FOLDER)
        sqlite3_bind_int64(s, 1, parent);
    if (view == LIB_CREDIT_TRACKS || view == LIB_COMPOSER_TRACKS) sqlite3_bind_int64(s, 2, g_credit_album);
    int cap = 0;
    while (sqlite3_step(s) == SQLITE_ROW) {
        if (out->n == cap) {
            cap = cap ? cap * 2 : 256;
            int64_t *ni = realloc(out->ids, (size_t)cap * sizeof *ni);
            if (!ni) break;
            out->ids = ni;
        }
        out->ids[out->n++] = sqlite3_column_int64(s, 0);
    }
    sqlite3_finalize(s);
    return out->n;
}

int library_list_credit_tracks(library_t *lib, int64_t artist_id, int64_t album_id, lib_list_t *out)
{
    g_credit_album = album_id;
    int n = library_list(lib, LIB_CREDIT_TRACKS, artist_id, out);
    g_credit_album = 0;
    return n;
}

static int64_t get_int_col(library_t *lib, const char *sql, int64_t id);

int library_needs_tags(library_t *lib)
{
    return get_int_col(lib, "SELECT EXISTS(SELECT 1 FROM tracks WHERE tags_v<1 OR (tags_v<2 AND cue=0 AND"
                            " (path LIKE '%.flac' OR path LIKE '%.wv' OR path LIKE '%.ape')))", 0) > 0;
}

int library_needs_art(library_t *lib)
{
    return get_int_col(lib, "SELECT EXISTS(SELECT 1 FROM albums WHERE art=2 AND COALESCE(art_sig, 0)=0)", 0) > 0;
}

int library_list_composer_tracks(library_t *lib, int64_t composer_id, int64_t album_id, lib_list_t *out)
{
    g_credit_album = album_id;
    int n = library_list(lib, LIB_COMPOSER_TRACKS, composer_id, out);
    g_credit_album = 0;
    return n;
}

void library_artist_counts(library_t *lib, int64_t artist_id, int *nalbums, int *ncredit_albums)
{
    *nalbums = *ncredit_albums = 0;
    sqlite3_stmt *s = prep(lib->db, "SELECT nalbums, ncredit_albums FROM artists WHERE id=?");
    if (!s) return;
    sqlite3_bind_int64(s, 1, artist_id);
    if (sqlite3_step(s) == SQLITE_ROW) {
        *nalbums = sqlite3_column_int(s, 0);
        *ncredit_albums = sqlite3_column_int(s, 1);
    }
    sqlite3_finalize(s);
}

int library_scan_changes(const library_t *lib) { return lib->scan_changes; }
void library_scan_cancel(library_t *lib) { lib->cancel = 1; }

void library_list_free(lib_list_t *l)
{
    free(l->ids);
    memset(l, 0, sizeof *l);
}

int library_row(library_t *lib, lib_view_t view, int64_t id,
                char *main, size_t main_cap, char *meta, size_t meta_cap)
{
    if (view == LIB_FOLDER && id >= 0) view = LIB_SONGS;      /* a song in a folder */
    sqlite3_stmt *s = view == LIB_ALBUM_ARTISTS ? lib->row_artist
                    : view == LIB_ARTISTS ? lib->row_credited
                    : view == LIB_GENRES ? lib->row_genre
                    : view == LIB_COMPOSERS ? lib->row_composer
                    : view == LIB_YEARS ? lib->row_year
                    : view == LIB_FOLDER ? lib->row_folder
                    : (view == LIB_ALBUM_TRACKS || view == LIB_SONGS || view == LIB_CREDIT_TRACKS ||
                       view == LIB_COMPOSER_TRACKS) ? lib->row_track
                    : view == LIB_PLAYLISTS ? lib->row_playlist
                    : view == LIB_MOST_PLAYED ? lib->row_plays
                    : (view == LIB_PLAYLIST_TRACKS || view == LIB_FAVOURITES || view == LIB_RECENT_PLAYED)
                        ? lib->row_pl_track
                    : lib->row_album;   /* albums, artist albums, recently added */
    sqlite3_reset(s);
    sqlite3_bind_int64(s, 1, view == LIB_FOLDER ? -id : id);
    main[0] = meta[0] = '\0';
    if (sqlite3_step(s) != SQLITE_ROW) { sqlite3_reset(s); return -1; }
    copy_col(s, 0, main, main_cap);
    int v = sqlite3_column_int(s, 1);
    int track_list = view == LIB_ALBUM_TRACKS || view == LIB_SONGS || view == LIB_PLAYLIST_TRACKS || view == LIB_CREDIT_TRACKS ||
                     view == LIB_FAVOURITES || view == LIB_RECENT_PLAYED || view == LIB_MOST_PLAYED ||
                     view == LIB_COMPOSER_TRACKS;
    if (view == LIB_ARTISTS || view == LIB_ALBUM_ARTISTS || view == LIB_PLAYLISTS || view == LIB_GENRES ||
        view == LIB_COMPOSERS || view == LIB_YEARS)
        snprintf(meta, meta_cap, "%d", v);                                                   /* a count */
    else if (view == LIB_FOLDER || view == LIB_MOST_PLAYED) {                               /* (the UI's words) */
        char n[16];
        snprintf(n, sizeof n, "%d", v);
        if (view == LIB_FOLDER) tr_fill(meta, meta_cap, v == 1 ? "1 song" : "%s songs", n, NULL);
        else tr_fill(meta, meta_cap, v == 1 ? "1 play" : "%s plays", n, NULL);
    }
    else if (track_list) snprintf(meta, meta_cap, "%d:%02d", v / 60000, (v / 1000) % 60);   /* length */
    else if (library_row_shows_artist(view) && !lib->no_album_artists && sqlite3_column_text(s, 2) &&
             *sqlite3_column_text(s, 2))
        copy_col(s, 2, meta, meta_cap);                                                      /* album artist */
    else if (v > 0) snprintf(meta, meta_cap, "%d", v);                                       /* album year */
    if (view == LIB_PLAYLIST_TRACKS || view == LIB_FAVOURITES || view == LIB_RECENT_PLAYED ||
        view == LIB_MOST_PLAYED) {   /* a mix of artists: "Title · Artist" */
        const unsigned char *a = sqlite3_column_text(s, 2);
        size_t l = strlen(main);
        if (a && *a && l + 4 < main_cap) snprintf(main + l, main_cap - l, " \xC2\xB7 %s", (const char *)a);
    }
    /* Done with the row: reset now. A kept statement left on its row keeps
     * its read transaction open, which in WAL pins this connection to that
     * snapshot: another connection's commits (a scan, levelling) went unseen,
     * and this one's own writes failed with SQLITE_BUSY_SNAPSHOT (favourites,
     * play counts, playlists lost without a word). */
    sqlite3_reset(s);
    return 0;
}

void library_set_album_artists(library_t *lib, int on)
{
    if (lib) lib->no_album_artists = !on;
}

int64_t library_track_album(library_t *lib, int64_t track_id)
{
    sqlite3_stmt *s = lib->track_album;
    sqlite3_reset(s);
    sqlite3_bind_int64(s, 1, track_id);
    int64_t a = sqlite3_step(s) == SQLITE_ROW ? sqlite3_column_int64(s, 0) : 0;
    sqlite3_reset(s);
    return a;
}

int library_track(library_t *lib, int64_t id, lib_track_t *t)
{
    sqlite3_stmt *s = lib->track_full;
    sqlite3_reset(s);
    sqlite3_bind_int64(s, 1, id);
    memset(t, 0, sizeof *t);
    if (sqlite3_step(s) != SQLITE_ROW) return -1;
    t->id = id;
    copy_col(s, 0, t->path, sizeof t->path);
    copy_col(s, 1, t->title, sizeof t->title);
    copy_col(s, 2, t->artist, sizeof t->artist);
    copy_col(s, 3, t->album, sizeof t->album);
    t->album_id = sqlite3_column_int64(s, 4);
    t->artist_id = sqlite3_column_int64(s, 5);
    t->disc = sqlite3_column_int(s, 6);
    t->track = sqlite3_column_int(s, 7);
    t->year = sqlite3_column_int(s, 8);
    copy_col(s, 9, t->codec, sizeof t->codec);
    t->sample_rate = (uint32_t)sqlite3_column_int(s, 10);
    t->bits = (uint8_t)sqlite3_column_int(s, 11);
    t->channels = (uint8_t)sqlite3_column_int(s, 12);
    t->duration_ms = (uint32_t)sqlite3_column_int64(s, 13);
    sqlite3_reset(s);   /* (see library_row) */
    return 0;
}

static int name_query(sqlite3_stmt *s, int64_t id, char *out, size_t cap);

int library_genre_name(library_t *lib, int64_t id, char *out, size_t cap) { return name_query(lib->genre_name, id, out, cap); }
int library_composer_name(library_t *lib, int64_t id, char *out, size_t cap) { return name_query(lib->composer_name, id, out, cap); }

int library_folder_name(library_t *lib, int64_t id, char *out, size_t cap)
{
    if (id == 0) { snprintf(out, cap, "Folders"); return 0; }
    return name_query(lib->folder_name, id, out, cap);
}

static int name_query(sqlite3_stmt *s, int64_t id, char *out, size_t cap)
{
    sqlite3_reset(s);
    sqlite3_bind_int64(s, 1, id);
    out[0] = '\0';
    int ok = sqlite3_step(s) == SQLITE_ROW;
    if (ok) copy_col(s, 0, out, cap);
    sqlite3_reset(s);   /* (a statement left on its row keeps a read open: see library_row) */
    return ok ? 0 : -1;
}

int library_album_name(library_t *lib, int64_t id, char *out, size_t cap) { return name_query(lib->album_name, id, out, cap); }
int library_artist_name(library_t *lib, int64_t id, char *out, size_t cap) { return name_query(lib->artist_name, id, out, cap); }
int library_playlist_name(library_t *lib, int64_t id, char *out, size_t cap) { return name_query(lib->playlist_name, id, out, cap); }

int64_t library_album_artist(library_t *lib, int64_t album_id)
{
    sqlite3_stmt *s = prep(lib->db, "SELECT artist_id FROM albums WHERE id=?");
    if (!s) return 0;
    sqlite3_bind_int64(s, 1, album_id);
    int64_t id = sqlite3_step(s) == SQLITE_ROW ? sqlite3_column_int64(s, 0) : 0;
    sqlite3_finalize(s);
    return id;
}

int library_art(library_t *lib, int64_t album_id, int size, uint16_t *pixels)
{
    char path[1200];
    art_path(lib, album_id, size, path, sizeof path);
    FILE *f = fopen(path, "rbe");
    if (!f) return -1;
    size_t n = fread(pixels, 2, (size_t)size * size, f);
    fclose(f);
    return n == (size_t)size * size ? 0 : -1;
}

int library_shuffle_tracks(library_t *lib, shuf_track_t **out)
{
    sqlite3_stmt *s = prep(lib->db,
        "SELECT id, artist_id, album_id, disc, track, duration_ms, last_played, play_count, skip_count, genre, title"
        " FROM tracks ORDER BY album_id, disc, track");
    *out = NULL;
    if (!s) return -1;
    int n = 0, cap = 0;
    while (sqlite3_step(s) == SQLITE_ROW) {
        if (n == cap) {
            cap = cap ? cap * 2 : 1024;
            shuf_track_t *nt = realloc(*out, (size_t)cap * sizeof *nt);
            if (!nt) break;
            *out = nt;
        }
        shuf_track_t *t = &(*out)[n++];
        memset(t, 0, sizeof *t);
        t->id = (uint32_t)sqlite3_column_int64(s, 0);
        t->artist = (uint32_t)sqlite3_column_int64(s, 1);
        t->album = (uint32_t)sqlite3_column_int64(s, 2);
        t->disc = (uint16_t)sqlite3_column_int(s, 3);
        t->track = (uint16_t)sqlite3_column_int(s, 4);
        t->duration_ms = (uint32_t)sqlite3_column_int64(s, 5);
        t->last_played = sqlite3_column_int64(s, 6);
        t->play_count = (uint16_t)sqlite3_column_int(s, 7);
        t->skip_count = (uint16_t)sqlite3_column_int(s, 8);
        const char *g = (const char *)sqlite3_column_text(s, 9);
        if (g && (strcasestr(g, "audiobook") || strcasestr(g, "podcast") || strcasestr(g, "spoken")))
            t->flags |= SHUF_F_SPOKEN;
        /* A work in several parts ("Symphony No. 5 in C minor: I. Allegro"): the
         * title before ": ", for the pass below (kept in unit for now) */
        const char *ti = (const char *)sqlite3_column_text(s, 10);
        const char *c = ti ? strstr(ti, ": ") : NULL;
        if (c && c - ti >= 4) {
            uint32_t h = 2166136261u;
            for (const char *q = ti; q < c; q++) h = (h ^ (unsigned char)*q) * 16777619u;
            t->unit = h ? h : 1;
        }
    }
    sqlite3_finalize(s);
    /* Works: consecutive tracks of an album with the same title prefix, two or
     * more of them, share a unit (shuffle keeps them together when asked to);
     * a prefix on its own is no work. Rows come in album, disc, track order. */
    for (int i = 0; i < n;) {
        int j = i + 1;
        while (j < n && (*out)[i].unit && (*out)[j].unit == (*out)[i].unit && (*out)[j].album == (*out)[i].album) j++;
        uint32_t u = j - i >= 2 ? ((*out)[i].id ? (*out)[i].id : 1) : 0;   /* the first part's id: unique */
        for (int k = i; k < j; k++) (*out)[k].unit = u;
        i = j;
    }
    return n;
}

void library_played(library_t *lib, int64_t track_id, int skipped_early, int64_t now)
{
    sqlite3_stmt *s = prep(lib->db, skipped_early
        ? "UPDATE tracks SET skip_count=skip_count+1 WHERE id=?2"
        : "UPDATE tracks SET play_count=play_count+1, last_played=?1 WHERE id=?2");
    if (!s) return;
    sqlite3_bind_int64(s, 1, now);
    sqlite3_bind_int64(s, 2, track_id);
    sqlite3_step(s);
    sqlite3_finalize(s);
}

static void set_int_col(library_t *lib, const char *sql, int64_t id, int64_t v)
{
    sqlite3_stmt *s = prep(lib->db, sql);
    if (!s) return;
    sqlite3_bind_int64(s, 1, v);
    sqlite3_bind_int64(s, 2, id);
    sqlite3_step(s);
    sqlite3_finalize(s);
}

static int64_t get_int_col(library_t *lib, const char *sql, int64_t id)
{
    sqlite3_stmt *s = prep(lib->db, sql);
    if (!s) return 0;
    sqlite3_bind_int64(s, 1, id);
    int64_t v = sqlite3_step(s) == SQLITE_ROW ? sqlite3_column_int64(s, 0) : 0;
    sqlite3_finalize(s);
    return v;
}

void library_set_favourite(library_t *lib, int64_t id, int on)
{
    set_int_col(lib, "UPDATE tracks SET favourite=?1 WHERE id=?2", id, on ? 1 : 0);
}

int library_favourite(library_t *lib, int64_t id)
{
    return (int)get_int_col(lib, "SELECT favourite FROM tracks WHERE id=?1", id);
}

void library_set_resume(library_t *lib, int64_t id, uint32_t ms)
{
    set_int_col(lib, "UPDATE tracks SET resume_ms=?1 WHERE id=?2", id, ms);
}

uint32_t library_resume(library_t *lib, int64_t id)
{
    return (uint32_t)get_int_col(lib, "SELECT resume_ms FROM tracks WHERE id=?1", id);
}

/* ---------- your own playlists ---------- */

void library_set_playlist_dir(library_t *lib, const char *dir)
{
    snprintf(lib->pl_dir, sizeof lib->pl_dir, "%s", dir ? dir : "");
    if (lib->pl_dir[0]) mkdir(lib->pl_dir, 0755);
}

int library_playlist_writable(library_t *lib, int64_t id)
{
    return (int)get_int_col(lib, "SELECT writable FROM playlists WHERE id=?1", id);
}

int library_playlist_count_writable(library_t *lib)
{
    sqlite3_stmt *s = prep(lib->db, "SELECT COUNT(*) FROM playlists WHERE writable=1");
    int n = s && sqlite3_step(s) == SQLITE_ROW ? sqlite3_column_int(s, 0) : 0;
    sqlite3_finalize(s);
    return n;
}

/* `to` relative to the folder `from_dir` (both absolute): "../../Music/A/B/c.flac". */
static void rel_path(const char *from_dir, const char *to, char *out, size_t cap)
{
    size_t i = 0, common = 0;             /* longest common prefix, ending at a '/' */
    while (from_dir[i] && to[i] && from_dir[i] == to[i]) {
        i++;
        if (to[i - 1] == '/') common = i;
    }
    if (!from_dir[i] && to[i] == '/') common = i + 1;   /* `to` is inside from_dir */
    int ups = 0;                          /* folders of from_dir below the common part */
    const char *rest = from_dir + common;
    if (*rest) {
        ups = 1;
        for (const char *c = rest; *c; c++) ups += *c == '/';
    }
    size_t w = 0;
    for (int k = 0; k < ups && w + 4 < cap; k++, w += 3) memcpy(out + w, "../", 3);
    snprintf(out + w, cap - w, "%s", to + common);
}

/* Write a playlist's file from its rows (atomically). */
static int write_playlist(library_t *lib, int64_t id)
{
    sqlite3_stmt *q = prep(lib->db, "SELECT path, name FROM playlists WHERE id=? AND writable=1");
    if (!q) return -1;
    sqlite3_bind_int64(q, 1, id);
    char path[1100], name[256], dir[1100];
    if (sqlite3_step(q) != SQLITE_ROW) { sqlite3_finalize(q); return -1; }
    copy_col(q, 0, path, sizeof path);
    copy_col(q, 1, name, sizeof name);
    sqlite3_finalize(q);
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) *slash = '\0';

    char tmp[1200];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "we");
    if (!f) return -1;
    fprintf(f, "#EXTM3U\n#PLAYLIST:%s\n", name);
    sqlite3_stmt *it = prep(lib->db,
        "SELECT t.path, t.artist, t.title, t.duration_ms FROM playlist_items i JOIN tracks t ON t.id=i.track_id"
        " WHERE i.playlist_id=? ORDER BY i.pos");
    if (it) {
        sqlite3_bind_int64(it, 1, id);
        while (sqlite3_step(it) == SQLITE_ROW) {
            char rel[1200];
            rel_path(dir, (const char *)sqlite3_column_text(it, 0), rel, sizeof rel);
            const unsigned char *a = sqlite3_column_text(it, 1), *ti = sqlite3_column_text(it, 2);
            fprintf(f, "#EXTINF:%lld,%s - %s\n%s\n", (long long)(sqlite3_column_int64(it, 3) / 1000),
                    a ? (const char *)a : "", ti ? (const char *)ti : "", rel);
        }
        sqlite3_finalize(it);
    }
    int ok = fflush(f) == 0 && fsync(fileno(f)) == 0;
    ok = fclose(f) == 0 && ok;
    if (!ok || rename(tmp, path)) { unlink(tmp); return -1; }
    return 0;
}

static void renumber(library_t *lib, int64_t id)
{
    /* positions 0..n-1 again after a removal */
    sqlite3_stmt *q = prep(lib->db, "SELECT rowid FROM playlist_items WHERE playlist_id=? ORDER BY pos");
    sqlite3_stmt *u = prep(lib->db, "UPDATE playlist_items SET pos=? WHERE rowid=?");
    if (q && u) {
        sqlite3_bind_int64(q, 1, id);
        int pos = 0;
        while (sqlite3_step(q) == SQLITE_ROW) {
            sqlite3_reset(u);
            sqlite3_bind_int(u, 1, pos++);
            sqlite3_bind_int64(u, 2, sqlite3_column_int64(q, 0));
            sqlite3_step(u);
        }
    }
    sqlite3_finalize(q);
    sqlite3_finalize(u);
    set_int_col(lib, "UPDATE playlists SET ntracks=(SELECT COUNT(*) FROM playlist_items WHERE playlist_id=?2)"
                     " + 0 * ?1 WHERE id=?2", id, 0);
}

static void playlist_path(library_t *lib, const char *name, const char *keep, char *out, size_t cap);

int64_t library_playlist_create(library_t *lib, const char *name)
{
    if (!lib->pl_dir[0]) return 0;
    char path[1400], fname[256];
    playlist_path(lib, name, NULL, path, sizeof path);   /* never over another playlist's file */
    const char *slash = strrchr(path, '/');
    snprintf(fname, sizeof fname, "%.255s", slash ? slash + 1 : path);
    sqlite3_stmt *s = prep(lib->db, "INSERT INTO playlists(path, name, sort, ntracks, seen, writable) VALUES(?,?,?,0,1,1)");
    if (!s) return 0;
    char sort[256];
    sort_key(fname, sort, sizeof sort);
    sqlite3_bind_text(s, 1, path, -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 2, name, -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 3, sort, -1, SQLITE_STATIC);
    int ok = sqlite3_step(s) == SQLITE_DONE;
    sqlite3_finalize(s);
    if (!ok) return 0;
    int64_t id = sqlite3_last_insert_rowid(lib->db);
    if (write_playlist(lib, id)) { library_playlist_delete(lib, id); return 0; }
    return id;
}

int library_playlist_add(library_t *lib, int64_t id, const int64_t *tracks, int n)
{
    if (!library_playlist_writable(lib, id)) return -1;
    int pos = (int)get_int_col(lib, "SELECT COUNT(*) FROM playlist_items WHERE playlist_id=?1", id);
    sqlite3_stmt *s = prep(lib->db, "INSERT INTO playlist_items(playlist_id, pos, track_id) VALUES(?,?,?)");
    if (!s) return -1;
    sqlite3_exec(lib->db, "BEGIN", NULL, NULL, NULL);
    for (int i = 0; i < n; i++) {
        sqlite3_reset(s);
        sqlite3_bind_int64(s, 1, id);
        sqlite3_bind_int(s, 2, pos++);
        sqlite3_bind_int64(s, 3, tracks[i]);
        sqlite3_step(s);
    }
    sqlite3_finalize(s);
    renumber(lib, id);
    sqlite3_exec(lib->db, "COMMIT", NULL, NULL, NULL);
    return write_playlist(lib, id);
}

int library_playlist_remove(library_t *lib, int64_t id, int pos)
{
    if (!library_playlist_writable(lib, id)) return -1;
    sqlite3_stmt *s = prep(lib->db, "DELETE FROM playlist_items WHERE playlist_id=? AND pos=?");
    if (!s) return -1;
    sqlite3_bind_int64(s, 1, id);
    sqlite3_bind_int(s, 2, pos);
    sqlite3_step(s);
    sqlite3_finalize(s);
    renumber(lib, id);
    return write_playlist(lib, id);
}

int library_playlist_move(library_t *lib, int64_t id, int pos, int dir)
{
    if (!library_playlist_writable(lib, id)) return -1;
    int other = pos + dir, n = (int)get_int_col(lib, "SELECT COUNT(*) FROM playlist_items WHERE playlist_id=?1", id);
    if (pos < 0 || other < 0 || pos >= n || other >= n) return -1;
    /* swap the two positions through a temporary one */
    sqlite3_stmt *s = prep(lib->db, "UPDATE playlist_items SET pos=?3 WHERE playlist_id=?1 AND pos=?2");
    if (!s) return -1;
    int steps[3][2] = {{pos, -1}, {other, pos}, {-1, other}};
    for (int i = 0; i < 3; i++) {
        sqlite3_reset(s);
        sqlite3_bind_int64(s, 1, id);
        sqlite3_bind_int(s, 2, steps[i][0]);
        sqlite3_bind_int(s, 3, steps[i][1]);
        sqlite3_step(s);
    }
    sqlite3_finalize(s);
    return write_playlist(lib, id);
}

/* A file name for a playlist name, in pl_dir; " 2", " 3"... if taken by another file. */
static void playlist_path(library_t *lib, const char *name, const char *keep, char *out, size_t cap)
{
    char fname[256];
    size_t w = 0;
    for (const char *c = name; *c && w + 1 < sizeof fname; c++)
        fname[w++] = (*c == '/' || *c == '\\' || *c == ':') ? '-' : *c;
    fname[w] = '\0';
    for (int n = 1; n < 100; n++) {
        if (n == 1) snprintf(out, cap, "%s/%s.m3u8", lib->pl_dir, fname);
        else snprintf(out, cap, "%s/%s %d.m3u8", lib->pl_dir, fname, n);
        if ((keep && !strcmp(out, keep)) || access(out, F_OK) != 0) return;
    }
}

int library_playlist_rename(library_t *lib, int64_t id, const char *name)
{
    if (!library_playlist_writable(lib, id) || !name[0]) return -1;
    sqlite3_stmt *q = prep(lib->db, "SELECT path FROM playlists WHERE id=?");
    if (!q) return -1;
    sqlite3_bind_int64(q, 1, id);
    char old[1100] = "", path[1400];
    if (sqlite3_step(q) == SQLITE_ROW) copy_col(q, 0, old, sizeof old);
    sqlite3_finalize(q);
    if (!old[0]) return -1;
    playlist_path(lib, name, old, path, sizeof path);
    if (strcmp(path, old) && rename(old, path)) return -1;
    char sort[256];
    const char *base = strrchr(path, '/');
    sort_key(base ? base + 1 : path, sort, sizeof sort);
    sqlite3_stmt *u = prep(lib->db, "UPDATE playlists SET path=?, name=?, sort=? WHERE id=?");
    if (!u) return -1;
    sqlite3_bind_text(u, 1, path, -1, SQLITE_STATIC);
    sqlite3_bind_text(u, 2, name, -1, SQLITE_STATIC);
    sqlite3_bind_text(u, 3, sort, -1, SQLITE_STATIC);
    sqlite3_bind_int64(u, 4, id);
    sqlite3_step(u);
    sqlite3_finalize(u);
    return write_playlist(lib, id);   /* #PLAYLIST: the new name */
}

int library_playlist_delete(library_t *lib, int64_t id)
{
    sqlite3_stmt *q = prep(lib->db, "SELECT path FROM playlists WHERE id=? AND writable=1");
    if (!q) return -1;
    sqlite3_bind_int64(q, 1, id);
    char path[1100] = "";
    if (sqlite3_step(q) == SQLITE_ROW) copy_col(q, 0, path, sizeof path);
    sqlite3_finalize(q);
    if (!path[0]) return -1;
    unlink(path);
    set_int_col(lib, "DELETE FROM playlist_items WHERE playlist_id=?2 AND ?1 = ?1", id, 0);
    set_int_col(lib, "DELETE FROM playlists WHERE id=?2 AND ?1 = ?1", id, 0);
    return 0;
}

/* ---------- search ---------- */

static int search_one(library_t *lib, const char *table, const char *where_extra, const char *order,
                      int limit, char words[][64], int nw, lib_list_t *out)
{
    char sql[1024];
    int w = snprintf(sql, sizeof sql, "SELECT id FROM %s WHERE %s", table, where_extra);
    for (int i = 0; i < nw && w < (int)sizeof sql - 64; i++)
        w += snprintf(sql + w, sizeof sql - (size_t)w, " AND (' '||search) LIKE ?%d", i + 1);
    snprintf(sql + w, sizeof sql - (size_t)w, " ORDER BY %s LIMIT %d", order, limit);
    memset(out, 0, sizeof *out);
    sqlite3_stmt *s = prep(lib->db, sql);
    if (!s) return 0;
    for (int i = 0; i < nw; i++) {
        char pat[80];
        snprintf(pat, sizeof pat, "%% %s%%", words[i]);   /* a word starting with it */
        sqlite3_bind_text(s, i + 1, pat, -1, SQLITE_TRANSIENT);
    }
    out->ids = malloc((size_t)limit * sizeof *out->ids);
    while (out->ids && out->n < limit && sqlite3_step(s) == SQLITE_ROW) out->ids[out->n++] = sqlite3_column_int64(s, 0);
    sqlite3_finalize(s);
    return out->n;
}

int library_search(library_t *lib, const char *query, lib_search_t *out)
{
    memset(out, 0, sizeof *out);
    char folded[256], words[8][64];
    text_search_fold(query, folded, sizeof folded);
    int nw = 0;
    for (char *save = NULL, *t = strtok_r(folded, " ", &save); t && nw < 8; t = strtok_r(NULL, " ", &save))
        snprintf(words[nw++], sizeof words[0], "%s", t);
    if (!nw) return 0;
    search_one(lib, "artists", "(nalbums>0 OR ncredit_albums>0)", "sort", 5, words, nw, &out->artists);
    search_one(lib, "albums", "1", "sort, year", 10, words, nw, &out->albums);
    search_one(lib, "tracks", "1", "title COLLATE NOCASE", 40, words, nw, &out->songs);
    return out->artists.n + out->albums.n + out->songs.n;
}

void library_search_free(lib_search_t *r)
{
    library_list_free(&r->artists);
    library_list_free(&r->albums);
    library_list_free(&r->songs);
}

/* ---------- volume levelling (ReplayGain) ---------- */

/* (By tracks_rg_todo, which holds only the tracks left: the first of them
 * at once, where a walk in id order went past every one done.) */
int library_rg_next(library_t *lib, int64_t *id, char *path, size_t cap)
{
    sqlite3_stmt *s = prep(lib->db, "SELECT id, path FROM tracks WHERE rg_state=0 ORDER BY id LIMIT 1");
    int rc = -1;
    if (s && sqlite3_step(s) == SQLITE_ROW) {
        *id = sqlite3_column_int64(s, 0);
        copy_col(s, 1, path, cap);
        rc = 0;
    }
    sqlite3_finalize(s);
    return rc;
}

void library_rg_store(library_t *lib, int64_t id, const lib_rg_t *rg)
{
    sqlite3_stmt *s = prep(lib->db,
        "UPDATE tracks SET rg_state=?, rg_track=?, rg_track_peak=?, rg_album=?, rg_album_peak=? WHERE id=?");
    if (!s) return;
    sqlite3_bind_int(s, 1, rg->state);
    if (rg->state == 1 || rg->state == 2) sqlite3_bind_double(s, 2, rg->track_gain); else sqlite3_bind_null(s, 2);
    if (rg->track_peak > 0) sqlite3_bind_double(s, 3, rg->track_peak); else sqlite3_bind_null(s, 3);
    if (rg->has_album) sqlite3_bind_double(s, 4, rg->album_gain); else sqlite3_bind_null(s, 4);
    if (rg->has_album && rg->album_peak > 0) sqlite3_bind_double(s, 5, rg->album_peak); else sqlite3_bind_null(s, 5);
    sqlite3_bind_int64(s, 6, id);
    sqlite3_step(s);
    sqlite3_finalize(s);
}

void library_rg_progress(library_t *lib, int *done, int *total)
{
    *done = *total = 0;
    sqlite3_stmt *s = prep(lib->db, "SELECT COUNT(*) - (SELECT COUNT(*) FROM tracks WHERE rg_state=0), COUNT(*) FROM tracks");
    if (s && sqlite3_step(s) == SQLITE_ROW) {
        *done = sqlite3_column_int(s, 0);
        *total = sqlite3_column_int(s, 1);
    }
    sqlite3_finalize(s);
}

/* The gain (dB) and peak for a track: its own, or its album's (from the album
 * tag, or combined from the album's tracks, each weighted by its length, once
 * all of them are known). -1 if unknown. */
static int rg_lookup(library_t *lib, int64_t id, int album, double *gain, double *peak)
{
    sqlite3_stmt *s = prep(lib->db,
        "SELECT rg_state, rg_track, rg_track_peak, rg_album, rg_album_peak, album_id FROM tracks WHERE id=?");
    if (!s) return -1;
    sqlite3_bind_int64(s, 1, id);
    int rc = -1;
    int64_t album_id = 0;
    if (sqlite3_step(s) == SQLITE_ROW) {
        int st = sqlite3_column_int(s, 0);
        album_id = sqlite3_column_int64(s, 5);
        if (album && sqlite3_column_type(s, 3) != SQLITE_NULL) {
            *gain = sqlite3_column_double(s, 3);
            *peak = sqlite3_column_type(s, 4) != SQLITE_NULL ? sqlite3_column_double(s, 4) : 0;
            rc = 0;
        } else if (st == 1 || st == 2) {
            *gain = sqlite3_column_double(s, 1);
            *peak = sqlite3_column_type(s, 2) != SQLITE_NULL ? sqlite3_column_double(s, 2) : 0;
            rc = album ? 1 : 0;      /* album mode: try the album's tracks below */
        }
    }
    sqlite3_finalize(s);
    if (rc != 1) return rc;

    /* the album's loudness: its tracks' mean energy, weighted by length */
    s = prep(lib->db, "SELECT rg_state, rg_track, duration_ms, rg_track_peak FROM tracks WHERE album_id=?");
    if (!s) return 0;
    sqlite3_bind_int64(s, 1, album_id);
    double energy = 0, ms = 0, apeak = 0;
    int all = 1;
    while (sqlite3_step(s) == SQLITE_ROW) {
        int st = sqlite3_column_int(s, 0);
        if (st != 1 && st != 2) { all = 0; break; }
        double d = sqlite3_column_double(s, 2);
        energy += d * pow(10.0, (-18.0 - sqlite3_column_double(s, 1)) / 10.0);
        ms += d;
        if (sqlite3_column_double(s, 3) > apeak) apeak = sqlite3_column_double(s, 3);
    }
    sqlite3_finalize(s);
    if (all && ms > 0 && energy > 0) {
        *gain = -18.0 - 10.0 * log10(energy / ms);
        *peak = apeak;
    }
    return 0;
}

float library_rg_gain(library_t *lib, int64_t id, int album)
{
    double gain, peak;
    if (rg_lookup(lib, id, album, &gain, &peak) < 0) return 1.0f;
    if (peak > 0 && gain > -20.0 * log10(peak)) gain = -20.0 * log10(peak);   /* never clip */
    return (float)pow(10.0, gain / 20.0);
}
