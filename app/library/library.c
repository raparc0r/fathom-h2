/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE
#define _GNU_SOURCE   /* strcasestr (glibc, musl) */
#include "library.h"
#include "library/library_int.h"
#include "core/fsafe.h"
#include "core/clock.h"
#include "core/hash.h"
#include "core/path.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/resource.h>
#include <sys/syscall.h>
#endif

#include <sqlite3.h>

#include "core/lang.h"
#include "library/artdec.h"
#include "library/cue.h"
#include "library/fatwalk.h"
#include "library/keys.h"
#include "library/scripts.h"
#include "library/playlist.h"
#include "library/reader.h"
#include "library/tags.h"

#ifndef FATHOM_SQLITE_CACHE
#define FATHOM_SQLITE_CACHE "-2000"   /* KiB; the H2 build uses less */
#endif

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

    "CREATE INDEX IF NOT EXISTS albums_artist ON albums(artist_id, year);";

static int rebuild_groups(library_t *lib);
static void meta_set(sqlite3 *db, const char *key, int v);
static int bulk_end(library_t *lib);
static void art_del(library_t *lib, int64_t album_id);
static void art_open(library_t *lib);

sqlite3_stmt *prep(sqlite3 *db, const char *sql)
{
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK) {
        fprintf(stderr, "library: %s\n  in: %s\n", sqlite3_errmsg(db), sql);
        return NULL;
    }
    return s;
}

/* A query's one number (its first row's first column), ?1 bound to `bind` if
 * it has one: `otherwise` when it gives no row or can't be run (said by prep). */
int64_t db_int(sqlite3 *db, const char *sql, int64_t bind, int64_t otherwise)
{
    sqlite3_stmt *s = prep(db, sql);
    if (!s) return otherwise;
    sqlite3_bind_int64(s, 1, bind);
    int64_t v = sqlite3_step(s) == SQLITE_ROW ? sqlite3_column_int64(s, 0) : otherwise;
    sqlite3_finalize(s);
    return v;
}

int64_t get_int_col(library_t *lib, const char *sql, int64_t id) { return db_int(lib->db, sql, id, 0); }

/* A statement run once with ?1 = v and ?2 = id (0, or -1: it couldn't be run or didn't end well) */
int set_int_col(library_t *lib, const char *sql, int64_t id, int64_t v)
{
    sqlite3_stmt *s = prep(lib->db, sql);
    if (!s) return -1;
    sqlite3_bind_int64(s, 1, v);
    sqlite3_bind_int64(s, 2, id);
    int rc = sqlite3_step(s) == SQLITE_DONE ? 0 : -1;
    sqlite3_finalize(s);
    return rc;
}

/* ...and one with ?1 = id alone */
int db_run_id(sqlite3 *db, const char *sql, int64_t id)
{
    sqlite3_stmt *s = prep(db, sql);
    if (!s) return -1;
    sqlite3_bind_int64(s, 1, id);
    int rc = sqlite3_step(s) == SQLITE_DONE ? 0 : -1;
    sqlite3_finalize(s);
    return rc;
}

void copy_col(sqlite3_stmt *s, int col, char *out, size_t cap)
{
    const unsigned char *t = sqlite3_column_text(s, col);
    snprintf(out, cap, "%s", t ? (const char *)t : "");
}

/* Room for one more item in an array of n (room for *cap, each `size` bytes;
 * doubled when full, `first` to begin with). The array, which may have moved:
 * NULL when there is no memory for it, and it is then as it was. */
void *vec_room(void *v, int n, int *cap, int first, size_t size)
{
    if (n < *cap) return v;
    int nc = *cap ? *cap * 2 : first;
    void *nv = realloc(v, (size_t)nc * size);
    if (nv) *cap = nc;
    return nv;
}

/* The folder a file is in, from its path (NULL: ""), into dir: 1, or 0 for a
 * path with no '/' in it (dir is then the path). */
int dir_of(const char *path, char *dir, size_t cap)
{
    snprintf(dir, cap, "%s", path ? path : "");
    char *slash = strrchr(dir, '/');
    if (slash) *slash = '\0';
    return slash != NULL;
}

static void sql_sortkey(sqlite3_context *ctx, int argc, sqlite3_value **argv);

/* An album as covers are looked for: library_album_info, library_albums_without_cover */
#define ALBUM_INFO_SQL "SELECT al.id, al.key, al.title, al.year, ar.name FROM albums al" \
                       " LEFT JOIN artists ar ON ar.id=al.artist_id"

/* Bring a database from `version` up to LIB_SCHEMA_V. */
/* How far an upgrade has got (library_set_upgrade_progress) */
static void (*g_up_cb)(int done, int total, void *ctx);
static void *g_up_ctx;
static int g_up_done, g_up_total;

void library_set_upgrade_progress(void (*cb)(int done, int total, void *ctx), void *ctx)
{
    g_up_cb = cb;
    g_up_ctx = ctx;
}

static void up_tick(void)
{
    if (g_up_cb) g_up_cb(++g_up_done, g_up_total, g_up_ctx);
}

/* 7: what each list, smart playlist and Shuffle all reads, found from an
 * index (most covering: the table's rows, ~640 bytes each, not read at all);
 * library_check_plans() and test_scan keep it so. (No ANALYZE: its statistics
 * would make the plans depend on the library's data.) 8: an artist's songs
 * (the groups rebuilt only where songs changed). One at a time: each a step
 * of the upgrade's progress. */
static const char *const INDEXES_7_8[] = {
    "CREATE INDEX IF NOT EXISTS tracks_order ON tracks(sort, title COLLATE NOCASE)",
    "CREATE INDEX IF NOT EXISTS tracks_fav_order ON tracks(sort, title COLLATE NOCASE) WHERE favourite=1",
    "CREATE INDEX IF NOT EXISTS tracks_fav_last ON tracks(last_played) WHERE favourite=1",
    "CREATE INDEX IF NOT EXISTS tracks_last ON tracks(last_played)",
    "CREATE INDEX IF NOT EXISTS tracks_played ON tracks(last_played) WHERE play_count>0",
    "CREATE INDEX IF NOT EXISTS tracks_most ON tracks(play_count DESC, last_played DESC) WHERE play_count>0",
    "CREATE INDEX IF NOT EXISTS tracks_skipped ON tracks(skip_count DESC, sort) WHERE skip_count>=2",
    "CREATE INDEX IF NOT EXISTS tracks_unplayed ON tracks(album_id, disc, track) WHERE play_count=0 AND skip_count=0",
    "CREATE INDEX IF NOT EXISTS tracks_added ON tracks(album_id, mtime)",
    "CREATE INDEX IF NOT EXISTS tracks_tags_v ON tracks(tags_v)",
    "CREATE INDEX IF NOT EXISTS tracks_shuffle ON tracks(album_id, disc, track, artist_id, duration_ms, last_played, play_count, skip_count, genre, title)",
    "DROP INDEX IF EXISTS tracks_genre",
    "CREATE INDEX IF NOT EXISTS tracks_genre_album ON tracks(genre_id, album_id)",
    "DROP INDEX IF EXISTS tracks_composer",
    "CREATE INDEX IF NOT EXISTS tracks_composer_album ON tracks(composer_id, album_id)",
    "DROP INDEX IF EXISTS albums_sort",
    "CREATE INDEX IF NOT EXISTS albums_order ON albums(sort, year)",
    "CREATE INDEX IF NOT EXISTS tracks_artist ON tracks(artist_id)",
    "CREATE INDEX IF NOT EXISTS tracks_search ON tracks(search)",       /* (9: what a search reads) */
};

static void migrate(library_t *lib, int version)
{
    g_up_done = 0;
    g_up_total = 4 + (int)(sizeof INDEXES_7_8 / sizeof *INDEXES_7_8);
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
    up_tick();
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
    int regroup = 0;                             /* (1: it stopped part way: none of it kept, the next scan's to do) */
    if (!had_composer || !had_credits || version < 4) {   /* (4: names spelled as tagged now) */
        sqlite3_exec(lib->db, "BEGIN", NULL, NULL, NULL);
        regroup = rebuild_groups(lib) != 0;
        sqlite3_exec(lib->db, regroup ? "ROLLBACK" : "PRAGMA user_version=4; COMMIT", NULL, NULL, NULL);
    }
    /* 3: covers' signatures cover found covers too, and images' dates: noted
     * afresh at the next scan (a found cover kept as it is) */
    if (version < 3) sqlite3_exec(lib->db, "UPDATE albums SET art_sig=0", NULL, NULL, NULL);
    up_tick();
    index_search(lib, version < 9 ? 0 : 1);      /* (9: every key written afresh, as it is now) */
    up_tick();
    /* 5: levelling's next track found by an index (a pass over the library
     * re-read every track measured so far, for each one), and a scan's note
     * of work it left (meta: library_scan) */
    sqlite3_exec(lib->db,
        "CREATE INDEX IF NOT EXISTS tracks_rg_todo ON tracks(id) WHERE rg_state=0;"
        "CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY, value INTEGER);",
        NULL, NULL, NULL);
    if (regroup) meta_set(lib->db, "scan_pending", 2);
    /* the play log: when each song was played (On repeat), a year of it */
    sqlite3_exec(lib->db,
        "CREATE TABLE IF NOT EXISTS plays(track_id INTEGER NOT NULL, at INTEGER NOT NULL);"
        "CREATE INDEX IF NOT EXISTS plays_at ON plays(at);",
        NULL, NULL, NULL);
    up_tick();
    for (size_t i = 0; i < sizeof INDEXES_7_8 / sizeof *INDEXES_7_8; i++) {
        sqlite3_exec(lib->db, INDEXES_7_8[i], NULL, NULL, NULL);
        up_tick();
    }
    sqlite3_exec(lib->db, "PRAGMA user_version=" LIB_STR(LIB_SCHEMA_V), NULL, NULL, NULL);
}

/* FATHOM_SQL_SLOW_MS=N: every statement slower than N ms, with its time, to
 * stderr (finding what makes something slow on a big library, on the H2 too) */
#ifndef FATHOM_SQLITE_NO_TRACE
static int sql_slow(unsigned type, void *ctx, void *p, void *x)
{
    (void)ctx;
    static int64_t min_ns = -1;
    if (min_ns < 0) min_ns = (int64_t)atoi(getenv("FATHOM_SQL_SLOW_MS")) * 1000000;
    if (type != SQLITE_TRACE_PROFILE) return 0;
    int64_t ns = *(sqlite3_int64 *)x;
    if (ns < min_ns) return 0;
    char *sql = sqlite3_expanded_sql((sqlite3_stmt *)p);
    fprintf(stderr, "sql: %6.1f ms  %.300s\n", ns / 1e6, sql ? sql : sqlite3_sql((sqlite3_stmt *)p));
    sqlite3_free(sql);
    return 0;
}
#endif

library_t *library_open(const char *db_path, const char *art_dir)
{
    library_t *lib = calloc(1, sizeof *lib);
    if (!lib) return NULL;
    snprintf(lib->art_dir, sizeof lib->art_dir, "%s", art_dir);
    snprintf(lib->db_path, sizeof lib->db_path, "%s", db_path);
    mkdir(art_dir, 0755);
    art_open(lib);
    char *err = NULL;
    int rc = sqlite3_open(db_path, &lib->db);
    /* another connection (a background scan) may be writing: wait for it */
    if (rc == SQLITE_OK) sqlite3_busy_timeout(lib->db, 10000);
#ifndef FATHOM_SQLITE_NO_TRACE   /* (the player's SQLite on the H2 has no tracing: external.mk) */
    if (rc == SQLITE_OK && getenv("FATHOM_SQL_SLOW_MS"))   /* (diagnostics: slow statements to stderr) */
        sqlite3_trace_v2(lib->db, SQLITE_TRACE_PROFILE, sql_slow, NULL);
#endif
    if (rc != SQLITE_OK || sqlite3_exec(lib->db, PRAGMAS, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "library: cannot open %s: %s\n", db_path, err ? err : sqlite3_errmsg(lib->db));
        sqlite3_free(err);
        library_close(lib);
        return NULL;
    }
    search_register(lib);
    /* how names sort: albums (without "the"), and songs (as titled) */
    static int strip_the = 1;
    sqlite3_create_function(lib->db, "sortkey", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, &strip_the, sql_sortkey, NULL, NULL);
    sqlite3_create_function(lib->db, "songkey", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, NULL, sql_sortkey, NULL, NULL);
    int version = (int)db_int(lib->db, "PRAGMA user_version", 0, 0);
    if (version < LIB_SCHEMA_V) {
        if (sqlite3_exec(lib->db, SCHEMA, NULL, NULL, &err) != SQLITE_OK) {
            fprintf(stderr, "library: cannot open %s: %s\n", db_path, err ? err : sqlite3_errmsg(lib->db));
            sqlite3_free(err);
            library_close(lib);
            return NULL;
        }
        migrate(lib, version);
    }
    bulk_end(lib);                                 /* (a first scan cut short left its indexes to build: before any list is asked for) */
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
    lib->artist_counts = prep(lib->db, "SELECT nalbums, ncredit_albums FROM artists WHERE id=?");
    lib->album_info = prep(lib->db, ALBUM_INFO_SQL " WHERE al.id=?");
    lib->unread_at = prep(lib->db, "SELECT path FROM cues WHERE kind=3 ORDER BY path LIMIT 1 OFFSET ?");
    if (!lib->artist_counts || !lib->album_info || !lib->unread_at || !lib->row_playlist || !lib->row_pl_track || !lib->row_plays || !lib->track_album || !lib->playlist_name ||
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
    if (!lib->plays_conn) plays_release(lib);       /* (the plays given for later are in; their connection to this file goes) */
    for (int i = 0; i < 2; i++)
        if (lib->art_fd[i] >= 0) close(lib->art_fd[i]);
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
    sqlite3_finalize(lib->artist_counts);
    sqlite3_finalize(lib->album_info);
    sqlite3_finalize(lib->unread_at);
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
    union {
        int want;      /* a .cue: how many files it splits (cues.nfiles) */
        uint32_t tid;  /* music, st == ST_SAME: its track */
    };
    unsigned char kept;   /* music: its row in `cues` stays; a playlist: its rows stay */
    unsigned char put;    /* a .cue: its row in `cues` is written at the end */
    unsigned char st;     /* against the library (against_library): ST_... */
    unsigned char skip;   /* music: the player died reading it last time (trying_left): not read */
} pent_t;
/* tags_v: 1 = the composer read; 2 = and the tags looked at for a CUE sheet */
#define TAGS_V 2
enum { ST_NEW, ST_SAME, ST_LOOK, ST_CHANGED };   /* not in it; as it is, nothing to do (tid); in it, look again; other date or size */
/* failed: a file or folder the walk couldn't note (no memory, a folder that
 * can't be listed): the list isn't the whole folder, so nothing is taken
 * for deleted */
struct known;
struct pa_chunk;
typedef struct { pent_t *v; int n, cap, failed; struct known *known; struct pa_chunk *names; } pathvec_t;

/* The names of a list's files, in blocks of 128 KB (each a mapping of its
 * own, handed back to the system when the last name in it is dropped). One
 * malloc a name cost 16 bytes more each, and freed, the C library kept them:
 * a first scan that let its names go as the songs came in still held all
 * 5 MB of them to the end (measured on the H2, 50,000 songs). Names are made
 * in the order the folder is listed and dropped in path order, nearly the
 * same, so a block empties soon after the scan has passed its folders. */
#define PA_CHUNK (128 * 1024)
typedef struct pa_chunk { struct pa_chunk *next, *prev; int live; size_t used; } pa_chunk_t;

static char *pa_dup(pathvec_t *pv, const char *p)
{
    size_t n = strlen(p) + 1;
    if (n > PA_CHUNK - sizeof(pa_chunk_t)) return NULL;
    pa_chunk_t *c = pv->names;
    if (!c || c->used + n > PA_CHUNK) {
        void *m = NULL;
        if (posix_memalign(&m, PA_CHUNK, PA_CHUNK)) return NULL;
        c = m;
        c->prev = NULL;
        c->next = pv->names;
        if (pv->names) pv->names->prev = c;
        c->live = 0;
        c->used = sizeof *c;
        pv->names = c;
    }
    char *out = (char *)c + c->used;
    memcpy(out, p, n);
    c->used += n;
    c->live++;
    return out;
}

/* A name no longer needed (its entry's p is cleared): its block goes when it was the last in it */
static void pa_drop(pathvec_t *pv, pent_t *e)
{
    if (!e->p) return;
    pa_chunk_t *c = (pa_chunk_t *)((uintptr_t)e->p & ~(uintptr_t)(PA_CHUNK - 1));
    e->p = NULL;
    if (--c->live > 0 || c == pv->names) return;     /* (the one being filled stays) */
    if (c->prev) c->prev->next = c->next;
    if (c->next) c->next->prev = c->prev;
    free(c);
}

static int known_same(struct known *k, const char *p, const struct stat *st);

static int pv_push(pathvec_t *pv, const char *p, const struct stat *st)
{
    /* (in the library as it is: noted there, its name not kept) */
    if (pv->known && st && known_same(pv->known, p, st)) return 0;
    pent_t *nv = vec_room(pv->v, pv->n, &pv->cap, 1024, sizeof *nv);
    if (!nv) { pv->failed = 1; return -1; }
    pv->v = nv;
    pent_t *e = &pv->v[pv->n];
    memset(e, 0, sizeof *e);
    e->p = pa_dup(pv, p);
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
    const char *slash = strrchr(path, '/'), *e = path_ext(path);
    if ((slash ? slash[1] : path[0]) == '.') return 0;   /* (a hidden file) */
    return !strcasecmp(e, "jpg") || !strcasecmp(e, "jpeg") || !strcasecmp(e, "png");
}

/* What the listing takes a file for, by its name: music first, as always
 * (the walk and the fast walk asked the same four questions, each its own way) */
enum { FILE_OTHER, FILE_MUSIC, FILE_LIST, FILE_SHEET, FILE_IMAGE };

static int file_kind(const char *path)
{
    if (tags_supported(path)) return FILE_MUSIC;
    if (m3u_supported(path)) return FILE_LIST;
    if (cue_supported(path)) return FILE_SHEET;
    if (art_image(path)) return FILE_IMAGE;
    return FILE_OTHER;
}

static void pv_free(pathvec_t *pv)
{
    for (pa_chunk_t *c = pv->names, *next; c; c = next) {
        next = c->next;
        free(c);
    }
    free(pv->v);
    memset(pv, 0, sizeof *pv);
}

/* A scan's four lists (music, playlists, sheets, images) freed */
static void lists_free(pathvec_t *pv, pathvec_t *plv, pathvec_t *cv, pathvec_t *iv)
{
    pv_free(pv);
    pv_free(plv);
    pv_free(cv);
    pv_free(iv);
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

/* ---------- the files the library has, in 20 bytes each ----------
 * A scan held every music file's path until its songs were read: 7 MB for
 * 50,000 songs, on a player with 25 MB in all (a rescan with the player up
 * left 1.5 MB). Nearly all of them are files the library has already, as
 * they are. So before the folder is listed, the library's tracks are read
 * once into a table of (the path's hash, date, size, id), 1 MB for 50,000;
 * a file the listing gives that's there with the same date and size is
 * ticked off by its id and its name is never kept. Only the others (new,
 * changed, or to be looked at again) go into the scan's lists, as before.
 *
 * Not when the library has CUE sheets or files split by one (a sheet names
 * music files, which are then looked for in the list), and given up, the
 * folder listed again the old way, if the listing turns up a sheet.
 * A file is taken for one the library has only if the 64-bit hash of its
 * whole path, its date and its size all match a track's: a new or changed
 * file can't be missed short of that. Two tracks with the same hash: neither
 * is in the table. */
typedef struct __attribute__((packed)) { uint64_t h; uint32_t id, mtime, size; } kent_t;

typedef struct known {
    kent_t *v;
    int n;
    int rows;               /* the library's tracks when it was made */
    idset_t same;           /* found as they are: their ids */
    int n_same;
    uint64_t sum, count;    /* ...and their part of the listing's signature (listing_sig) */
} known_t;

static int g_known = 1;

void library_test_known(int on) { g_known = on; }

static int cue_in_tags(const char *path);
#define SIG_SEED FNV64_START
#define SIG_MIX 0x9E3779B97F4A7C15ULL

static int cmp_kent(const void *a, const void *b)
{
    uint64_t x = ((const kent_t *)a)->h, y = ((const kent_t *)b)->h;
    return x < y ? -1 : x > y;
}

static void known_free(known_t *k)
{
    if (!k) return;
    free(k->v);
    free(k->same.bits);
    free(k);
}

/* (the listing is made again, another way: nothing is ticked off yet) */
static void known_reset(known_t *k)
{
    if (!k) return;
    if (k->same.bits) memset(k->same.bits, 0, k->same.cap);
    k->same.failed = 0;
    k->n_same = 0;
    k->sum = k->count = 0;
}

/* The table, or NULL: an empty library, one with sheets, no memory (the scan
 * then goes the old way) */
static known_t *known_build(sqlite3 *db)
{
    if (!g_known) return NULL;
    sqlite3_stmt *q = NULL;
    int64_t rows = -1;
    if (sqlite3_prepare_v2(db, "SELECT (SELECT COUNT(*) FROM tracks), EXISTS(SELECT 1 FROM cues),"
                               " EXISTS(SELECT 1 FROM tracks WHERE cue<>0)", -1, &q, NULL) == SQLITE_OK &&
        sqlite3_step(q) == SQLITE_ROW && !sqlite3_column_int(q, 1) && !sqlite3_column_int(q, 2))
        rows = sqlite3_column_int64(q, 0);
    sqlite3_finalize(q);
    if (rows <= 0 || rows > 4000000) return NULL;
    known_t *k = calloc(1, sizeof *k);
    if (!k || !(k->v = malloc((size_t)rows * sizeof *k->v))) { free(k); return NULL; }
    q = NULL;
    int got = 0;
    if (sqlite3_prepare_v2(db, "SELECT path, mtime, size, id, tags_v FROM tracks", -1, &q, NULL) != SQLITE_OK) {
        known_free(k);
        return NULL;
    }
    while (sqlite3_step(q) == SQLITE_ROW) {
        got++;
        const char *p = (const char *)sqlite3_column_text(q, 0);
        int64_t mt = sqlite3_column_int64(q, 1), sz = sqlite3_column_int64(q, 2), id = sqlite3_column_int64(q, 3);
        int v = sqlite3_column_int(q, 4);
        /* (as against_library takes a file for done; dates and sizes that
         * don't fit 32 bits go the old way) */
        if (!p || id <= 0 || id > UINT32_MAX || mt < 0 || mt > UINT32_MAX || sz < 0 || sz > UINT32_MAX) continue;
        if (!(v >= TAGS_V || (v >= 1 && !cue_in_tags(p))) || k->n >= rows) continue;
        k->v[k->n++] = (kent_t){fnv64(SIG_SEED, p, strlen(p)), (uint32_t)id, (uint32_t)mt, (uint32_t)sz};
    }
    sqlite3_finalize(q);
    k->rows = got;
    qsort(k->v, (size_t)k->n, sizeof *k->v, cmp_kent);
    int w = 0;                                  /* two tracks, one hash: neither */
    for (int i = 0; i < k->n;) {
        int j = i + 1;
        while (j < k->n && k->v[j].h == k->v[i].h) j++;
        if (j == i + 1) k->v[w++] = k->v[i];
        i = j;
    }
    k->n = w;
    return k;
}

/* The music file p as the listing gives it: 1 if the library has it so
 * (ticked off; its part of the listing's signature added) */
static int known_same(known_t *k, const char *p, const struct stat *st)
{
    int64_t mt = (int64_t)st->st_mtime, sz = (int64_t)st->st_size;
    if (!tags_supported(p)) return 0;           /* (only music: playlists and images are listed whole) */
    uint64_t h = fnv64(SIG_SEED, p, strlen(p));
    int lo = 0, hi = k->n;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (k->v[mid].h < h) lo = mid + 1;
        else hi = mid;
    }
    if (lo >= k->n || k->v[lo].h != h || (int64_t)k->v[lo].mtime != mt || (int64_t)k->v[lo].size != sz) return 0;
    uint32_t id = k->v[lo].id;
    if (idset_has(&k->same, id)) return 0;      /* (listed twice?: the second the old way) */
    idset_add(&k->same, id);
    if (!idset_has(&k->same, id)) return 0;     /* (no memory to note it: the old way) */
    h = fnv64(h, &mt, sizeof mt);
    h = fnv64(h, &sz, sizeof sz);
    k->sum += h * SIG_MIX;
    k->count++;
    k->n_same++;
    return 1;
}

/* A scan's writes, in short transactions: another connection's (the
 * player's play count when a track ends, a favourite, a playlist edit)
 * waits a fraction of a second at most, not the whole scan (minutes after
 * adding music: past its 10 s wait, it failed). Begun at a write; ended
 * before reading files again, or once BATCH_S old. */
#define BATCH_S 0.25

typedef struct { sqlite3 *db; double since; int open, nice; } batch_t;

/* While a write holds the lock, the thread runs at normal priority: a scan
 * or the levelling runs at nice 19, and with another such thread busy (the
 * levelling of a big library) a hold stretched to over half a second, which
 * the UI's and the player's writes waited out. Returns the nice to put back
 * (or NO_NICE: nothing changed). */
#define NO_NICE (-1000)
int lock_boost(void)
{
#ifdef __linux__
    id_t tid = (id_t)syscall(SYS_gettid);
    errno = 0;
    int n = getpriority(PRIO_PROCESS, tid);
    if (errno == 0 && n > 0 && setpriority(PRIO_PROCESS, tid, 0) == 0) return n;
#endif
    return NO_NICE;
}

void lock_unboost(int n)
{
#ifdef __linux__
    if (n != NO_NICE) setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), n);
#else
    (void)n;
#endif
}

/* Every statement reset (one left on a row keeps its read open, and a
 * write on that old snapshot fails): only where none is being stepped. */
void reset_all(sqlite3 *db)
{
    for (sqlite3_stmt *s = sqlite3_next_stmt(db, NULL); s; s = sqlite3_next_stmt(db, s))
        if (sqlite3_stmt_busy(s)) sqlite3_reset(s);
}

/* COMMIT: 0, or -1 (a full card), the transaction then rolled back if it is
 * still open: every later write would sit in it, never committed. */
int db_commit(sqlite3 *db)
{
    if (sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) == SQLITE_OK) return 0;
    if (!sqlite3_get_autocommit(db)) sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
    return -1;
}

/* 0, or -1: its commit failed (a full card), and it was rolled back */
static int batch_end(batch_t *b)
{
    if (!b->open) return 0;
    reset_all(b->db);
    int rc = db_commit(b->db);
    b->open = 0;
    lock_unboost(b->nice);
    return rc;
}

/* The batch given up: nothing of it is kept. */
static void batch_undo(batch_t *b)
{
    if (!b->open) return;
    reset_all(b->db);
    sqlite3_exec(b->db, "ROLLBACK", NULL, NULL, NULL);
    b->open = 0;
    lock_unboost(b->nice);
}

/* Before a write. IMMEDIATE: the write lock at once (waiting for another
 * connection's commit), on the newest snapshot. */
static void batch_write(batch_t *b)
{
    if (b->open) return;
    reset_all(b->db);
    b->nice = lock_boost();   /* (before: once it has the lock, no moment at nice 19) */
    b->open = sqlite3_exec(b->db, "BEGIN IMMEDIATE", NULL, NULL, NULL) == SQLITE_OK;   /* (else: one at a time) */
    if (!b->open) lock_unboost(b->nice);
    b->since = clock_fsec();
}

/* What a scan left to do, kept across scans (meta.scan_pending): batches
 * are committed as it goes, so one stopped half way (USB drive mode, a
 * shutdown) leaves tracks that albums, search and playlists don't show
 * yet. 1: composers to count again; 2: groups, playlists and search. */
static int64_t meta_get64(sqlite3 *db, const char *key)
{
    sqlite3_stmt *s = prep(db, "SELECT value FROM meta WHERE key=?");
    int64_t v = 0;
    if (s) {
        sqlite3_bind_text(s, 1, key, -1, SQLITE_STATIC);
        if (sqlite3_step(s) == SQLITE_ROW) v = sqlite3_column_int64(s, 0);
    }
    sqlite3_finalize(s);
    return v;
}

static int meta_get(sqlite3 *db, const char *key) { return (int)meta_get64(db, key); }

static void meta_set64(sqlite3 *db, const char *key, int64_t v)
{
    sqlite3_stmt *s = prep(db, "INSERT INTO meta(key, value) VALUES(?, ?) ON CONFLICT(key) DO UPDATE SET value=excluded.value");
    if (s) {
        sqlite3_bind_text(s, 1, key, -1, SQLITE_STATIC);
        sqlite3_bind_int64(s, 2, v);
        sqlite3_step(s);
    }
    sqlite3_finalize(s);
}

static void meta_set(sqlite3 *db, const char *key, int v) { meta_set64(db, key, v); }

/* A folder or file that's there but couldn't be looked at (the card's
 * reads failing, out of memory or file handles), not one that's gone. */
/* Listing the music folder says how far it has got, a few times a second:
 * cb(songs found so far, 0, "looking"), and for the files it then looks up
 * one by one (those the quick listing has otherwise than the library),
 * cb(files looked up, 0, "checking"). Without it the screen said "Looking
 * for music" and nothing more for as long as that took: 16 s on the owner's
 * library with every file to look up, minutes on a big one. */
static double g_tell_every = 0.25;             /* (seconds between two reports; tests: 0) */
void library_test_progress_every(double s) { g_tell_every = s; }

static _Thread_local struct { lib_progress_cb cb; void *ctx; int found, checked; double at; } t_look;

static void look_tell(int checking)
{
    if (!t_look.cb) return;
    double now = clock_fsec();
    if (now - t_look.at < g_tell_every) return;
    t_look.at = now;
    if (checking) t_look.cb(t_look.checked, 0, "checking", t_look.ctx);
    else t_look.cb(t_look.found, 0, "looking", t_look.ctx);
}

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
        int kind = file_kind(path);
        if (kind == FILE_IMAGE && !iv) kind = FILE_OTHER;   /* (no images wanted from here) */
        if (e->d_type != DT_UNKNOWN && e->d_type != DT_DIR && kind == FILE_OTHER) continue;
        struct stat st;
        if (stat(path, &st)) {
            if (walk_error(errno)) pv->failed = 1;
            continue;
        }
        if (S_ISDIR(st.st_mode)) walk(path, pv, plv, cv, iv, depth + 1, cancel);
        else if (S_ISREG(st.st_mode) && kind == FILE_MUSIC) {
            pv_push(pv, path, &st);
            t_look.found++;
            look_tell(0);
        }
        else if (S_ISREG(st.st_mode) && kind == FILE_LIST) pv_push(plv, path, &st);
        else if (S_ISREG(st.st_mode) && kind == FILE_SHEET) pv_push(cv, path, &st);
        else if (S_ISREG(st.st_mode) && kind == FILE_IMAGE) pv_push(iv, path, &st);
    }
    closedir(d);
}

static int cmp_pent(const void *a, const void *b) { return strcmp(((const pent_t *)a)->p, ((const pent_t *)b)->p); }

/* ---------- the fast walk ----------
 * Listing the music folder through the kernel costs ~20 s on the H2's card;
 * reading its FAT directly (fatwalk) a few. Its list is used when it's what
 * the library holds already (nothing changed: the usual case), or when every
 * difference checks out through the kernel (music added or removed): each
 * file it has that the library hasn't, or has with another date or size, is
 * looked up (stat), and its date and size taken from there; each one the
 * library has that it hasn't must be gone. Any that doesn't (a name the FAT
 * reading spells otherwise, a folder it missed) and the folder is walked as
 * usual: a mistake in the FAT reading can't drop music. */
typedef struct { pathvec_t *pv, *plv, *cv, *iv; const volatile int *cancel; } fw_ctx_t;

static int fw_file(const char *path, int64_t mtime, int64_t size, void *ctx)
{
    fw_ctx_t *c = ctx;
    struct stat st = {0};
    st.st_mtime = (time_t)mtime;
    st.st_size = (off_t)size;
    int kind = file_kind(path);
    if (kind == FILE_MUSIC) {
        pv_push(c->pv, path, &st);
        t_look.found++;
        look_tell(0);
    }
    else if (kind == FILE_LIST) pv_push(c->plv, path, &st);
    else if (kind == FILE_SHEET) pv_push(c->cv, path, &st);
    else if (kind == FILE_IMAGE) pv_push(c->iv, path, &st);
    return *c->cancel;
}

/* The music files in `pv` and sheets in `cv` (both sorted) against the
 * library's files (its tracks', and those in `cues`), dates and sizes: each
 * entry marked (st: new, the same, changed; a track as it is with its id, so
 * the scan needn't look it up), and how many differ. The library's rows are
 * read as they lie (one pass over the table, ~1-2 s for 50,000 songs on the
 * H2) and each looked for in the lists (in memory); in path order they came
 * through the index, a seek a row: 10 s and more.
 *
 * verify (the lists are the FAT reading's, fatwalk): what differs is checked
 * through the kernel: a file with another date or size is looked up (stat)
 * and its date and size taken from there; one the library has that the lists
 * haven't must be gone. One that doesn't check out (logged: one that comes
 * back at every scan, a name the FAT reading spells otherwise than the
 * kernel, means the slow walk each time): -1. New files aren't looked up
 * when there are many (50,000 lookups on a first scan: two minutes): reading
 * their tags finds one that isn't there (library_scan then walks the folder
 * instead). */
#define FW_STAT_NEW 256
static int fw_differs(const char *what, const char *path)
{
    fprintf(stderr, "library: fast walk: %s %s: walking the folder\n", what, path);
    return -1;
}

/* A file the lists have with another date or size than the library: there,
 * as a file, and really changed; the kernel's date and size in e. 0, or -1 */
static int fw_check(pent_t *e, int64_t lib_mtime, int64_t lib_size)
{
    struct stat st;
    t_look.checked++;
    look_tell(1);
    if (stat(e->p, &st) || !S_ISREG(st.st_mode)) return fw_differs("not found by the kernel:", e->p);
    if ((int64_t)st.st_mtime == lib_mtime && (int64_t)st.st_size == lib_size)
        return fw_differs("date or size read otherwise:", e->p);   /* (it hasn't changed after all) */
    e->mtime = (int64_t)st.st_mtime;
    e->size = (int64_t)st.st_size;
    return 0;
}

static int pv_find(const pathvec_t *pv, const char *p);

static int g_test_against_fails;
void library_test_against_fails(int at_row) { g_test_against_fails = at_row; }

static int against_library(library_t *lib, pathvec_t *pv, pathvec_t *cv, int verify)
{
    int changed = 0, gone = 0, added = 0, bad = 0;
    for (int i = 0; i < pv->n; i++) pv->v[i].st = ST_NEW;
    for (int i = 0; i < cv->n; i++) cv->v[i].st = ST_NEW;
    for (int pass = 0; pass < 2 && !bad; pass++) {
        sqlite3_stmt *s = prep(lib->db, pass == 0 ? "SELECT path, mtime, size, id, tags_v FROM tracks WHERE cue=0"
                                                  : "SELECT path, mtime, size, 0, 0, kind FROM cues");
        if (!s) return -1;
        int rc = SQLITE_DONE, rows = 0;
        while (!bad && (rc = g_test_against_fails && ++rows == g_test_against_fails ? SQLITE_IOERR : sqlite3_step(s)) == SQLITE_ROW) {
            const char *p = (const char *)sqlite3_column_text(s, 0);
            if (!p) continue;
            pent_t *e = NULL;
            int j = pass == 1 && sqlite3_column_int(s, 5) == 0 ? -1 : pv_find(pv, p);   /* (kind 0: a .cue) */
            if (j >= 0) e = &pv->v[j];
            else if (pass == 1 && (j = pv_find(cv, p)) >= 0) e = &cv->v[j];
            if (!e && pass == 0 && pv->known && idset_has(&pv->known->same, sqlite3_column_int64(s, 3)))
                continue;                                /* (there, as it is: ticked off by the listing) */
            if (!e) {                                    /* only in the library: gone? */
                struct stat st;
                if (verify && (!stat(p, &st) || errno != ENOENT)) { bad = fw_differs("missed:", p); break; }
                gone++;
                continue;
            }
            int64_t mt = sqlite3_column_int64(s, 1), sz = sqlite3_column_int64(s, 2);
            if (mt != e->mtime || sz != e->size) {
                if (verify && fw_check(e, mt, sz)) { bad = 1; break; }
                if (e->st != ST_CHANGED) changed++;
                e->st = ST_CHANGED;
            } else if (e->st == ST_NEW) {
                int v = sqlite3_column_int(s, 4);
                uint64_t id = (uint64_t)sqlite3_column_int64(s, 3);
                int done = pass == 0 && id > 0 && id <= UINT32_MAX && (v >= TAGS_V || (v >= 1 && !cue_in_tags(p)));
                e->st = done ? ST_SAME : ST_LOOK;
                if (done) e->tid = (uint32_t)id;
            }
        }
        sqlite3_finalize(s);
        /* The fast walk's list is only as good as this check of it: one that
         * stopped at a failed step (the card not answering, no memory) has
         * checked nothing past that row. Ended there as at the table's end,
         * a list with a folder missing was taken, and that folder's songs
         * dropped as gone. */
        if (verify && !bad && rc != SQLITE_DONE) {
            fprintf(stderr, "library: fast walk: the check against the library stopped (%d): the folders walked\n", rc);
            bad = 1;
        }
    }
    if (bad) return -1;
    for (int i = 0; i < pv->n; i++) added += pv->v[i].st == ST_NEW;
    for (int i = 0; i < cv->n; i++) added += cv->v[i].st == ST_NEW;
    /* a few new files (an album or ten copied over): looked up as well, their
     * dates and sizes the kernel's, as the files' beside them */
    if (verify && added <= FW_STAT_NEW)
        for (int k = 0; k < 2; k++) {
            pathvec_t *v = k ? cv : pv;
            for (int i = 0; i < v->n; i++) {
                struct stat st;
                if (v->v[i].st != ST_NEW) continue;
                if (stat(v->v[i].p, &st) || !S_ISREG(st.st_mode)) return fw_differs("not found by the kernel:", v->v[i].p);
                v->v[i].mtime = (int64_t)st.st_mtime;
                v->v[i].size = (int64_t)st.st_size;
            }
        }
    if (verify && (added || changed || gone))
        fprintf(stderr, "library: fast walk: %d new, %d changed, %d gone (checked)\n", added, changed, gone);
    return added + changed + gone;
}

/* A walk's whole listing as one number (every music file, sheet, image and
 * playlist with its date and size, in any order): the same as the last
 * finished scan's (meta.scan_sig), the library is as that scan left it. */
static uint64_t listing_sig(const pathvec_t *const *v, int nv)
{
    /* (v[0]: the music; those ticked off as the library has them aren't in
     * it, and their part was added up as they were: known_same) */
    uint64_t sum = v[0]->known ? v[0]->known->sum : 0, n = v[0]->known ? v[0]->known->count : 0;
    for (int k = 0; k < nv; k++)
        for (int i = 0; i < v[k]->n; i++, n++) {
            const pent_t *e = &v[k]->v[i];
            uint64_t h = fnv64(SIG_SEED + (uint64_t)k, e->p, strlen(e->p));
            h = fnv64(h, &e->mtime, sizeof e->mtime);
            h = fnv64(h, &e->size, sizeof e->size);
            sum += h * SIG_MIX;
        }
    sum ^= n * 0xC2B2AE3D27D4EB4FULL;
    return (sum & 0x7fffffffffffffffULL) | 1;            /* (never 0: none kept) */
}

static lib_fatwalk_fn g_fatwalk;

void library_test_fatwalk(lib_fatwalk_fn fn) { g_fatwalk = fn; }

void library_set_fast_walk(library_t *lib, const char *dev, const char *mount)
{
    snprintf(lib->fw_dev, sizeof lib->fw_dev, "%s", dev ? dev : "");
    snprintf(lib->fw_mount, sizeof lib->fw_mount, "%s", mount ? mount : "");
}

/* The fast walk of `root`, if it's set up and the result is the library as it
 * is, or differs only where the kernel agrees: 1 (pv, plv filled, sorted),
 * else 0 (and they're left empty). */
static int fast_walk(library_t *lib, const char *root, pathvec_t *pv, pathvec_t *plv, pathvec_t *cv, pathvec_t *iv)
{
    size_t ml = strlen(lib->fw_mount);
    if (!lib->fw_dev[0] || !ml || strncmp(root, lib->fw_mount, ml) || (root[ml] != '/' && root[ml] != '\0'))
        return 0;
    const char *rel = root[ml] == '/' ? root + ml + 1 : "";
    sync();   /* fatwalk reads the partition itself: nothing may wait in the cache */
    fw_ctx_t c = {pv, plv, cv, iv, &lib->cancel};
    known_t *known = pv->known;
    if ((g_fatwalk ? g_fatwalk : fatwalk)(lib->fw_dev, lib->fw_mount, rel, fw_file, &c) == 0 && !lib->cancel) {
        qsort(pv->v, (size_t)pv->n, sizeof *pv->v, cmp_pent);
        qsort(cv->v, (size_t)cv->n, sizeof *cv->v, cmp_pent);
        /* every track ticked off and no other music file or sheet: the
         * library's files are the listing's (no second pass over the table) */
        if (known && !pv->n && !cv->n && !pv->failed && known->n_same == known->rows) return 1;
        if (known && cv->n) return 1;            /* (a sheet: listed again the old way, by the scan) */
        if (against_library(lib, pv, cv, 1) >= 0) return 1;
    }
    lists_free(pv, plv, cv, iv);
    known_reset(known);
    pv->known = known;
    return 0;
}

/* The music folder listed: the fast walk if it may be used and will do (1),
 * else the folder walked (0). */
static int list_music(library_t *lib, const char *root, int allow_fast, pathvec_t *pv, pathvec_t *plv, pathvec_t *cv,
                      pathvec_t *iv)
{
    int fast = allow_fast ? fast_walk(lib, root, pv, plv, cv, iv) : 0;
    if (!fast) walk(root, pv, plv, cv, iv, 0, &lib->cancel);
    return fast;
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

void sort_key(const char *in, char *out, size_t cap) { sort_key_ex(in, out, cap, 1); }

static void sql_sortkey(sqlite3_context *ctx, int argc, sqlite3_value **argv)
{
    (void)argc;
    const unsigned char *in = sqlite3_value_text(argv[0]);
    char out[600];
    sort_key_ex(in ? (const char *)in : "", out, sizeof out, sqlite3_user_data(ctx) != NULL);
    sqlite3_result_text(ctx, out, -1, SQLITE_TRANSIENT);
}

/* sql's statements run one by one, as sqlite3_exec does (each one's errors
 * passed over); any that takes over a second said, with how long: on the H2,
 * with tens of thousands of songs, that's where the groups' time goes. */
static void exec_said(sqlite3 *db, const char *sql)
{
    while (sql && *sql) {
        sqlite3_stmt *q = NULL;
        const char *tail = NULL;
        if (sqlite3_prepare_v2(db, sql, -1, &q, &tail) != SQLITE_OK) {
            fprintf(stderr, "library: groups: %s\n  in: %.120s\n", sqlite3_errmsg(db), sql);
            return;
        }
        if (q) {
            double t0 = clock_fsec();
            while (sqlite3_step(q) == SQLITE_ROW) { }
            sqlite3_finalize(q);
            if (clock_fsec() - t0 >= 1.0) fprintf(stderr, "library: groups: %.1f s: %.90s\n", clock_fsec() - t0, sql);
        }
        sql = tail;
    }
}

/* A step of one of the groups' passes over the songs. A pass has its end at
 * SQLITE_DONE only: one that stopped at a failed step (a read the card didn't
 * answer, no memory, busy) has grouped the songs up to there, and nothing of
 * what it made is kept. (Taken for the end, it left the albums past that point
 * without year or folder, their artists without a name, their folders deleted,
 * and the scan noted the groups as done: so they stayed until those songs
 * changed.) */
static int g_test_groups_fail, g_test_groups_steps;
void library_test_groups_fail(int at_step) { g_test_groups_fail = at_step; g_test_groups_steps = 0; }

static int grp_step(sqlite3_stmt *q)
{
    if (g_test_groups_fail) {
        int n = ++g_test_groups_steps;
        if (g_test_groups_fail > 0 ? n >= g_test_groups_fail : n == -g_test_groups_fail) return SQLITE_IOERR;
    }
    return sqlite3_step(q);
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
    int64_t id = sqlite3_step(get) == SQLITE_ROW ? sqlite3_column_int64(get, 0) : 0;
    /* (not left on its row: a statement still reading made the counts'
     * "DROP TABLE temp.fcount" fail, unsaid; the table stayed, and at this
     * connection's next full rebuild its making failed and the folders'
     * counts weren't written: songs added since showed as 0 in Folders) */
    sqlite3_reset(get);
    return id;
}

/* The folder at dir, which is root or below it (a dir shorter than root: root's own): its id */
static int64_t folder_under(sqlite3 *db, sqlite3_stmt *up, sqlite3_stmt *get, const char *dir, const char *root)
{
    return folder_id(db, up, get, strlen(dir) >= strlen(root) ? dir : root, root);
}

/* What folder_id runs (a folder's row found or made), and a song given its folder */
#define FOLDER_PUT_SQL "INSERT INTO folders(path, parent, name, sort, seen) VALUES(?,?,?,?,1)" \
                       " ON CONFLICT(path) DO UPDATE SET parent=excluded.parent, name=excluded.name," \
                       " sort=excluded.sort, seen=1"
#define FOLDER_GET_SQL "SELECT id FROM folders WHERE path=?"

typedef struct { sqlite3_stmt *up, *get, *ut; } folder_stmts_t;

static int folder_stmts_prep(sqlite3 *db, folder_stmts_t *f)
{
    f->up = prep(db, FOLDER_PUT_SQL);
    f->get = prep(db, FOLDER_GET_SQL);
    f->ut = prep(db, "UPDATE tracks SET folder_id=? WHERE id=?");
    return f->up && f->get && f->ut ? 0 : -1;
}

static void folder_stmts_free(folder_stmts_t *f)
{
    sqlite3_finalize(f->up);
    sqlite3_finalize(f->get);
    sqlite3_finalize(f->ut);
}

/* The song of q's row (its id, path, folder_id) put in folder fid, if it isn't there */
static void folder_put_track(folder_stmts_t *f, sqlite3_stmt *q, int64_t fid)
{
    if (sqlite3_column_type(q, 2) == SQLITE_NULL || sqlite3_column_int64(q, 2) != fid) {
        sqlite3_reset(f->ut);
        sqlite3_bind_int64(f->ut, 1, fid);
        sqlite3_bind_int64(f->ut, 2, sqlite3_column_int64(q, 0));
        sqlite3_step(f->ut);
    }
}

/* What the paths a and b share, up to its last '/', into out ("" if nothing; b may be NULL) */
static void common_dir(const char *a, const char *b, char *out, size_t cap)
{
    size_t n = 0, cut = 0;
    while (a[n] && b && a[n] == b[n]) { if (a[n] == '/') cut = n; n++; }
    if (cut >= cap) cut = cap - 1;
    memcpy(out, a, cut);
    out[cut] = '\0';
}

/* The folder all the tracks share (up to its last '/'): "" if none */
static void folders_root(sqlite3 *db, char *root, size_t cap)
{
    root[0] = '\0';
    sqlite3_stmt *ends = prep(db, "SELECT MIN(path), MAX(path) FROM tracks");
    if (ends && sqlite3_step(ends) == SQLITE_ROW && sqlite3_column_text(ends, 0))
        common_dir((const char *)sqlite3_column_text(ends, 0), (const char *)sqlite3_column_text(ends, 1), root, cap);
    sqlite3_finalize(ends);
}

/* Folders: the tree of the tracks' folders, from the folder they all share.
 * counts_only: every song has its folder already (a first scan that set it
 * as the song was added, from the same root); only the counts are left.
 * 0, or -1: the pass over the songs didn't reach its end (the caller keeps
 * nothing of it). */
static int rebuild_folders(sqlite3 *db, int counts_only)
{
    char root[1024];
    folders_root(db, root, sizeof root);
    sqlite3_stmt *q = prep(db, "SELECT id, path, folder_id FROM tracks ORDER BY path");
    folder_stmts_t f;
    int rc = folder_stmts_prep(db, &f) == 0 && q ? SQLITE_DONE : SQLITE_NOMEM;
    if (rc == SQLITE_DONE) {
        double t0 = clock_fsec();
        if (!counts_only) sqlite3_exec(db, "UPDATE folders SET seen=0", NULL, NULL, NULL);
        char dir[1024], last[1024] = "";
        int64_t fid = 0;
        while (!counts_only && (rc = grp_step(q)) == SQLITE_ROW) {
            dir_of((const char *)sqlite3_column_text(q, 1), dir, sizeof dir);
            if (strcmp(dir, last)) {
                fid = folder_under(db, f.up, f.get, dir, root);
                snprintf(last, sizeof last, "%s", dir);
            }
            folder_put_track(&f, q, fid);
        }
        if (clock_fsec() - t0 >= 1.0) fprintf(stderr, "library: groups: %.1f s: each song's folder\n", clock_fsec() - t0);
        /* Songs in each folder and those below it: every track counted once
         * for its folder and each folder above (one walk up the tree per
         * track, not a pass over all tracks per folder: that re-read the
         * table from the card for minutes on the H2). (Not after a pass that
         * stopped: the folders it didn't come to would go as unseen.) */
        if (rc == SQLITE_DONE) exec_said(db,
            "DELETE FROM folders WHERE seen=0;"
            "DROP TABLE IF EXISTS temp.fcount;"
            /* (keyed: the UPDATE below read the whole of it for every folder,
             * 17 s on the H2 with 5,600 folders) */
            "CREATE TEMP TABLE fcount(id INTEGER PRIMARY KEY, n INTEGER);"
            /* (each folder's own songs counted from the index, then carried
             * up: a row a folder and level, not a row a song and level) */
            "INSERT INTO temp.fcount WITH RECURSIVE up(id, n) AS ("
            " SELECT folder_id, COUNT(*) FROM tracks WHERE folder_id IS NOT NULL GROUP BY folder_id"
            " UNION ALL SELECT f.parent, up.n FROM up JOIN folders f ON f.id=up.id WHERE f.parent<>0)"
            " SELECT id, SUM(n) FROM up WHERE id IS NOT NULL GROUP BY id;"
            "UPDATE folders SET ntracks=COALESCE((SELECT n FROM temp.fcount c WHERE c.id=folders.id), 0);"
            "DROP TABLE temp.fcount;");
    }
    sqlite3_finalize(q);
    folder_stmts_free(&f);
    return rc == SQLITE_DONE ? 0 : -1;
}

/* Genres and Composers are made the same way: by name as tagged, ids kept
 * from scan to scan. Their SQL, for table T ("genres") of the tracks' column
 * C ("genre", whose id is C_id):
 * NAMED_NEW_SQL: the names not there yet made, and each song given its own;
 * NAMED_COUNT_SQL: their albums counted, the empty ones removed;
 * NAMED_SPELL_SQL: the spelling as tagged now (names match ignoring case: a
 *   retag from "j. s. bach" to "J. S. Bach" was the same row, kept as first
 *   seen; compared exactly, or the column's NOCASE took them as equal);
 * NAMED_CHANGED_SQL: all three for the changed songs only (temp.g_c, and what
 *   they belonged to, temp.g_old), their names old and new noted in temp.G;
 * NAMED_PUT_SQL, NAMED_GET_SQL: one name (?1) made if new, and its id. */
#define NAMED_NEW_SQL(T, C) \
    "INSERT OR IGNORE INTO " T "(name) SELECT DISTINCT TRIM(" C ") FROM tracks WHERE TRIM(COALESCE(" C ",''))<>'';" \
    "UPDATE tracks SET " C "_id=(SELECT id FROM " T " WHERE name=TRIM(tracks." C "))" \
    " WHERE " C "_id IS NOT (SELECT id FROM " T " WHERE name=TRIM(tracks." C "));"
#define NAMED_COUNT_SQL(T, C) \
    "UPDATE " T " SET nalbums=(SELECT COUNT(DISTINCT album_id) FROM tracks WHERE " C "_id=" T ".id)," \
    " sort=COALESCE(sort, fold(name));" \
    "DELETE FROM " T " WHERE nalbums=0;"
#define NAMED_SPELL_SQL(T, C) \
    "UPDATE " T " SET name=(SELECT TRIM(" C ") FROM tracks WHERE " C "_id=" T ".id ORDER BY path LIMIT 1)" \
    " WHERE name COLLATE BINARY IS NOT (SELECT TRIM(" C ") FROM tracks WHERE " C "_id=" T ".id ORDER BY path LIMIT 1);"
#define NAMED_CHANGED_SQL(T, C, G) \
    "INSERT OR IGNORE INTO main." T "(name) SELECT DISTINCT TRIM(" C ") FROM main.tracks" \
    " WHERE id IN (SELECT id FROM temp.g_c) AND TRIM(COALESCE(" C ",''))<>'';" \
    "UPDATE main.tracks SET " C "_id=(SELECT id FROM main." T " WHERE name=TRIM(tracks." C "))" \
    " WHERE id IN (SELECT id FROM temp.g_c) AND " C "_id IS NOT (SELECT id FROM main." T " WHERE name=TRIM(tracks." C "));" \
    "CREATE TEMP TABLE " G " AS SELECT " C "_id AS id FROM temp.g_old WHERE " C "_id IS NOT NULL" \
    " UNION SELECT " C "_id FROM main.tracks WHERE id IN (SELECT id FROM temp.g_c) AND " C "_id IS NOT NULL;" \
    "UPDATE main." T " SET nalbums=(SELECT COUNT(DISTINCT album_id) FROM main.tracks WHERE " C "_id=" T ".id)," \
    " sort=COALESCE(sort, fold(name)) WHERE id IN (SELECT id FROM temp." G ");" \
    "DELETE FROM main." T " WHERE nalbums=0 AND id IN (SELECT id FROM temp." G ");" \
    "UPDATE main." T " SET name=(SELECT TRIM(" C ") FROM main.tracks WHERE " C "_id=" T ".id ORDER BY path LIMIT 1)" \
    " WHERE id IN (SELECT id FROM temp." G ") AND name COLLATE BINARY IS NOT" \
    " (SELECT TRIM(" C ") FROM main.tracks WHERE " C "_id=" T ".id ORDER BY path LIMIT 1);"
#define NAMED_PUT_SQL(T) "INSERT OR IGNORE INTO " T "(name) SELECT TRIM(?1) WHERE TRIM(COALESCE(?1,''))<>''"
#define NAMED_GET_SQL(T) "SELECT id FROM " T " WHERE name=TRIM(?1)"

/* A whole table of them (the three statements above, in order); counts_only:
 * every song has its name's id already, spelled as its first song spells it. */
static void named_groups(sqlite3 *db, const char *fresh, const char *count, const char *spell, int counts_only)
{
    if (!counts_only) exec_said(db, fresh);
    exec_said(db, count);
    if (!counts_only) exec_said(db, spell);
}

static void rebuild_genres(sqlite3 *db, int counts_only)
{
    named_groups(db, NAMED_NEW_SQL("genres", "genre"), NAMED_COUNT_SQL("genres", "genre"), NAMED_SPELL_SQL("genres", "genre"),
                 counts_only);
}

/* (On its own when only composers were read: an upgraded library's first scan.) */
static void rebuild_composers(sqlite3 *db, int counts_only)
{
    named_groups(db, NAMED_NEW_SQL("composers", "composer"), NAMED_COUNT_SQL("composers", "composer"),
                 NAMED_SPELL_SQL("composers", "composer"), counts_only);
}

/* A track's keys, as browsing groups it: its artist (the shuffle's spread),
 * its album artist (browsing) and its album (whose key starts with the album
 * artist's: every track of an album has the album's artist). */
typedef struct { char akey[600], aakey[600], albkey[1200]; const char *aa_name; int various; } tkeys_t;

static void track_keys(const char *artist, const char *aa, const char *album, tkeys_t *k)
{
    if (!artist) artist = "";
    if (!aa) aa = "";
    if (!album || !*album) album = "Unknown album";
    key_artist(artist, aa, k->akey, sizeof k->akey);
    k->aa_name = *aa ? aa : (*artist ? artist : "Unknown artist");
    key_artist(k->aa_name, NULL, k->aakey, sizeof k->aakey);
    k->various = key_is_various(aa);
    if (k->various) snprintf(k->aakey, sizeof k->aakey, "various artists");
    if (!*k->akey) snprintf(k->akey, sizeof k->akey, "unknown artist");
    if (!*k->aakey) snprintf(k->aakey, sizeof k->aakey, "unknown artist");
    key_album(artist, aa, album, k->albkey, sizeof k->albkey);
}

/* What groups tracks into albums, artists and credits, one track at a time
 * (grp_track), in path order: the whole library (rebuild_groups), or only the
 * tracks around those that changed (rebuild_groups_where_changed), then only
 * the albums and artists in `albums` and `artists` written (NULL: all). */
typedef struct {
    sqlite3_stmt *ut, *ua, *ur, *ura, *uc;
    upsert_t u_art, u_alb;
    /* artists already named as an album artist: that spelling wins over a
     * track artist's or a guest's ("The Prodigy" over a soundtrack's
     * "Prodigy" earlier in path order) */
    unsigned char *aa_named;
    size_t aa_cap;
    const idset_t *albums, *artists;
    /* What a track would write that the tracks before it in this pass have
     * written already (they come in path order: an album's, one after another):
     * - an artist once named keeps the name (bit 2 of aa_named: named at all);
     * - an album's row takes the same title and artist from each of its tracks,
     *   and the earliest year: written again only when one of them differs
     *   (alb_year: the year its row has; al_*: the album last written);
     * - the same artist and album keys give the same ids (k_*: the last
     *   looked up).
     * Each was a statement a track: with 50,000 songs, most of the minute and
     * more this step took on the H2. g_grp_plain (tests): none of it. */
    int *alb_year;
    size_t alb_cap;
    int64_t al_id, al_artist;
    char al_title[256];
    char k_key[3][600];
    int64_t k_id[3];
    int no_ut;                     /* the track's row isn't written (it isn't there yet: the caller adds it) */
    int64_t out_album, out_artist; /* the last track's album and artist */
} grp_t;

static int g_grp_plain;

void library_test_groups_plain(int on) { g_grp_plain = on; }

#define GRP_MINE(set, id) (!(set) || idset_has((set), (id)))

static void grp_close(grp_t *g)
{
    sqlite3_finalize(g->ut);
    sqlite3_finalize(g->ua);
    sqlite3_finalize(g->ur);
    sqlite3_finalize(g->ura);
    sqlite3_finalize(g->uc);
    free(g->aa_named);
    free(g->alb_year);
    upsert_free(&g->u_art);
    upsert_free(&g->u_alb);
    memset(g, 0, sizeof *g);
}

static int grp_open(grp_t *g, sqlite3 *db)
{
    memset(g, 0, sizeof *g);
    g->ut = prep(db, "UPDATE tracks SET album_id=?, artist_id=? WHERE id=?");
    g->ua = prep(db,
        "UPDATE albums SET search=CASE WHEN title IS ?1 AND artist_id IS ?3 THEN search END, title=?1, sort=?2, artist_id=?3,"
        " year=CASE WHEN year IS NULL OR year=0 OR (?4>0 AND ?5<year)"
        " THEN ?6 ELSE year END, dir=COALESCE(dir, ?7), first_track=COALESCE(first_track, ?8) WHERE id=?9");
    g->ur = prep(db, "UPDATE artists SET name=COALESCE(name, ?), sort=? WHERE id=?");
    g->ura = prep(db, "UPDATE artists SET name=?, sort=? WHERE id=?");
    g->uc = prep(db, "INSERT OR IGNORE INTO credits(artist_id, track_id) VALUES(?, ?)");
    if (!g->ut || !g->ua || !g->ur || !g->ura || !g->uc || upsert_prep(db, "artists", &g->u_art) ||
        upsert_prep(db, "albums", &g->u_alb)) {
        grp_close(g);
        return -1;
    }
    return 0;
}

/* upsert_key, the last one asked in `slot` remembered */
static int64_t grp_key(grp_t *g, int slot, upsert_t *u, const char *key)
{
    if (!g_grp_plain && g->k_id[slot] > 0 && !strcmp(g->k_key[slot], key)) return g->k_id[slot];
    int64_t id = upsert_key(u, key);
    if (strlen(key) < sizeof g->k_key[slot]) {
        strcpy(g->k_key[slot], key);
        g->k_id[slot] = id;
    } else g->k_id[slot] = 0;
    return id;
}

/* Marks for an artist (aa_named, grown as needed): `bit` set now; whether it was before */
static int grp_mark(grp_t *g, int64_t artist, unsigned char bit)
{
    if (artist <= 0) return 0;
    if ((size_t)artist >= g->aa_cap) {
        size_t cap = (size_t)artist * 2 + 64;
        unsigned char *grown = realloc(g->aa_named, cap);
        if (!grown) return 0;                       /* (no memory: as if never seen, which only costs a statement) */
        memset(grown + g->aa_cap, 0, cap - g->aa_cap);
        g->aa_named = grown;
        g->aa_cap = cap;
    }
    int was = (g->aa_named[artist] & bit) != 0;
    g->aa_named[artist] |= bit;
    return was;
}
#define GRP_AA 1      /* seen as an album artist (its first spelling there wins) */
#define GRP_NAMED 2   /* has a name from this pass */

/* An artist's row given its name and sort key (un: g->ur, which keeps a name
 * it has, or g->ura, which takes this one) */
static void grp_name(sqlite3_stmt *un, const char *name, const char *key, int64_t artist)
{
    sqlite3_reset(un);
    sqlite3_bind_text(un, 1, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(un, 2, key, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(un, 3, artist);
    sqlite3_step(un);
}

/* The row of q: id, artist, album_artist, album, year, path, title, album_id, artist_id */
static void grp_track(grp_t *g, sqlite3_stmt *q)
{
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
    tkeys_t k;
    track_keys(artist, aa, album, &k);
    const char *album_artist_name = k.aa_name;

    int64_t track_artist = grp_key(g, 0, &g->u_art, k.akey);
    int64_t album_artist = grp_key(g, 1, &g->u_art, k.aakey);
    int64_t album_id = grp_key(g, 2, &g->u_alb, k.albkey);

    int first_aa = !grp_mark(g, album_artist, GRP_AA);
    if (GRP_MINE(g->artists, album_artist)) {
        /* (named already, and not for the first time as an album artist: the
         * statement would keep the name it has) */
        int named = grp_mark(g, album_artist, GRP_NAMED);
        if (first_aa || !named || g_grp_plain)
            grp_name(first_aa ? g->ura : g->ur, k.various ? "Various Artists" : album_artist_name, k.aakey, album_artist);
    }
    if (track_artist != album_artist && GRP_MINE(g->artists, track_artist) &&
        (!grp_mark(g, track_artist, GRP_NAMED) || g_grp_plain))
        grp_name(g->ur, *artist ? artist : "Unknown artist", k.akey, track_artist);

    /* The album's row: as the track before left it (the same album, title and
     * artist, and no earlier year): nothing to write */
    int album_same = 0;
    if (album_id > 0 && !g_grp_plain) {
        if ((size_t)album_id >= g->alb_cap) {
            size_t cap = (size_t)album_id * 2 + 64;
            int *grown = realloc(g->alb_year, cap * sizeof *grown);
            if (grown) {
                for (size_t i = g->alb_cap; i < cap; i++) grown[i] = -1;   /* (-1: not written in this pass) */
                g->alb_year = grown;
                g->alb_cap = cap;
            }
        }
        if ((size_t)album_id < g->alb_cap) {
            int had = g->alb_year[album_id];
            album_same = had >= 0 && g->al_id == album_id && g->al_artist == album_artist && !strcmp(g->al_title, album) &&
                         !(had == 0 ? year != 0 : (year > 0 && year < had));
            if (!album_same) {
                int before = had < 0 ? 0 : had;             /* (its year was set to 0 before the pass) */
                g->alb_year[album_id] = before == 0 || (year > 0 && year < before) ? (year < 0 ? 0 : year) : before;
                g->al_id = strlen(album) < sizeof g->al_title ? album_id : 0;
                g->al_artist = album_artist;
                snprintf(g->al_title, sizeof g->al_title, "%s", album);
            }
        }
    }
    if (GRP_MINE(g->albums, album_id) && !album_same) {
        char dir[1024], sort[300];
        dir_of(path, dir, sizeof dir);
        sort_key(album, sort, sizeof sort);
        sqlite3_reset(g->ua);
        sqlite3_bind_text(g->ua, 1, album, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(g->ua, 2, sort, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(g->ua, 3, album_artist);
        sqlite3_bind_int(g->ua, 4, year);
        sqlite3_bind_int(g->ua, 5, year);
        sqlite3_bind_int(g->ua, 6, year);
        sqlite3_bind_text(g->ua, 7, dir, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(g->ua, 8, tid);
        sqlite3_bind_int64(g->ua, 9, album_id);
        sqlite3_step(g->ua);
    }

    /* everyone credited on it (Artists): the main artists and guests */
    char names[KEY_CREDIT_MAX][KEY_CREDIT_LEN], ckey[600];
    int nn = key_credits(artist, title, names);
    for (int i = 0; i < nn; i++) {
        key_artist(names[i], NULL, ckey, sizeof ckey);
        if (!*ckey) continue;
        int64_t cid = !g_grp_plain && g->k_id[0] > 0 && !strcmp(g->k_key[0], ckey) ? g->k_id[0]
                    : !g_grp_plain && g->k_id[1] > 0 && !strcmp(g->k_key[1], ckey) ? g->k_id[1]
                    : upsert_key(&g->u_art, ckey);
        if (GRP_MINE(g->artists, cid) && (!grp_mark(g, cid, GRP_NAMED) || g_grp_plain)) grp_name(g->ur, names[i], ckey, cid);
        sqlite3_reset(g->uc);
        sqlite3_bind_int64(g->uc, 1, cid);
        sqlite3_bind_int64(g->uc, 2, tid);
        sqlite3_step(g->uc);
    }

    g->out_album = album_id;
    g->out_artist = track_artist;
    if (g->no_ut) return;
    /* written only when it changed: a rebuild used to rewrite every track */
    if (sqlite3_column_int64(q, 7) != album_id || sqlite3_column_int64(q, 8) != track_artist ||
        sqlite3_column_type(q, 7) == SQLITE_NULL) {
        sqlite3_reset(g->ut);
        sqlite3_bind_int64(g->ut, 1, album_id);
        sqlite3_bind_int64(g->ut, 2, track_artist);
        sqlite3_bind_int64(g->ut, 3, tid);
        sqlite3_step(g->ut);
    }
}

static int groups_counts(library_t *lib, double *t_folders, int as_added);

/* The albums-and-artists step says it is at work, a few times a second:
 * cb(done, total, "sorting") while it goes through the songs one by one
 * (rebuild_groups), and for the rest, which is the database's own work on
 * whole tables, cb(a number that goes up, 0, "working"), from SQLite's
 * progress handler: sent only while statements are really running. The
 * step's screen said "Sorting albums and artists" and nothing more: 3 s on
 * 2,400 songs, four minutes on 50,000. */
static _Thread_local struct { lib_progress_cb cb; void *ctx; int tick; double at; int counting; } t_grp;

static void grp_tell(int done, int total)
{
    if (!t_grp.cb) return;
    double now = clock_fsec();
    if (now - t_grp.at < g_tell_every) return;
    t_grp.at = now;
    if (total > 0) t_grp.cb(done, total, "sorting", t_grp.ctx);
    else t_grp.cb(++t_grp.tick, 0, "working", t_grp.ctx);
}

static int grp_busy(void *arg)
{
    (void)arg;
    /* (not while the songs are counted off: the two took turns on the screen,
     * "1,200 of 2,400 songs" then "Working.." and back) */
    if (!t_grp.counting) grp_tell(0, 0);
    return 0;
}

static void grp_watch(sqlite3 *db, lib_progress_cb cb, void *ctx)
{
    if (!cb || t_grp.cb) return;
    t_grp.cb = cb;
    t_grp.ctx = ctx;
    t_grp.tick = 0;
    t_grp.at = clock_fsec();
    sqlite3_progress_handler(db, 20000, grp_busy, NULL);
}

static void grp_unwatch(sqlite3 *db)
{
    if (!t_grp.cb) return;
    sqlite3_progress_handler(db, 0, NULL, NULL);
    t_grp.cb = NULL;
}

/* 0, or -1: one of its passes didn't reach its end, and the groups are half
 * made: the caller rolls back what it runs in. */
static int rebuild_groups(library_t *lib)
{
    sqlite3 *db = lib->db;
    double t0 = clock_fsec(), t1 = t0, t2 = t0;
    sqlite3_stmt *q = prep(db, "SELECT id, artist, album_artist, album, year, path, title, album_id, artist_id"
                               " FROM tracks ORDER BY path");
    grp_t g;
    if (!q || grp_open(&g, db)) { sqlite3_finalize(q); return -1; }

    sqlite3_exec(db, "UPDATE albums SET year=0, dir=NULL, first_track=NULL;"
                     "UPDATE tracks SET sort=songkey(COALESCE(title,'')) WHERE sort IS NOT songkey(COALESCE(title,''));",
                 NULL, NULL, NULL);
    sqlite3_exec(db, "DELETE FROM credits", NULL, NULL, NULL);
    /* names taken afresh from the tags (an album artist's first spelling in
     * path order, else the first track or credit to name them, as a new
     * library would): a retag from "the beatles" to "The Beatles" kept the
     * old spelling while any track matched */
    sqlite3_exec(db, "UPDATE artists SET name=NULL", NULL, NULL, NULL);
    int total = t_grp.cb ? (int)get_int_col(lib, "SELECT COUNT(*) FROM tracks", 0) : 0, at = 0;
    t_grp.counting = total > 0;
    int rc;
    while ((rc = grp_step(q)) == SQLITE_ROW) {
        grp_track(&g, q);
        if (total > 0) grp_tell(++at, total);
    }
    t_grp.counting = 0;
    t1 = clock_fsec();
    sqlite3_finalize(q);
    grp_close(&g);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "library: groups: the pass over the songs stopped (%d): none of it kept\n", rc);
        return -1;
    }
    if (groups_counts(lib, &t2, 0)) return -1;
    fprintf(stderr, "library: groups: tracks %.2f s, counts %.2f s, folders %.2f s\n", t1 - t0, t2 - t1, clock_fsec() - t2);
    return 0;
}

/* What follows the tracks' pass: each album's and artist's counts, the ones
 * left empty removed, genres, composers and folders. Also on its own, after
 * a first scan that grouped each song as it was added (direct_track):
 * as_added, 1: each song has its genre and composer already, their names as
 * the first song in path order spells them (what the statements left out
 * here would find, reading every song again: 15 s on the H2 with 50,000);
 * 2: its folder as well. 0, or -1: a pass didn't reach its end (the caller
 * rolls back). */
static int groups_counts(library_t *lib, double *t_folders, int as_added)
{
    sqlite3 *db = lib->db;
    exec_said(db, "UPDATE albums SET ntracks=(SELECT COUNT(*) FROM tracks WHERE album_id=albums.id)");
    /* Albums gone: their covers too. SQLite gives the next new album the
     * highest id again, and a coverless one showed the old album's cover. */
    sqlite3_stmt *gone = prep(db, "SELECT id FROM albums WHERE ntracks=0");
    int rc = SQLITE_NOMEM;
    while (gone && (rc = grp_step(gone)) == SQLITE_ROW) art_del(lib, sqlite3_column_int64(gone, 0));
    sqlite3_finalize(gone);
    if (rc != SQLITE_DONE) return -1;          /* (an album gone with its covers left: not removed without them) */
    exec_said(db,
        "DELETE FROM albums WHERE ntracks=0;"
        "UPDATE artists SET nalbums=(SELECT COUNT(*) FROM albums WHERE artist_id=artists.id);"
        "UPDATE artists SET ncredit_albums=(SELECT COUNT(DISTINCT t.album_id) FROM credits c"
        " JOIN tracks t ON t.id=c.track_id WHERE c.artist_id=artists.id);"
        "DELETE FROM artists WHERE nalbums=0 AND ncredit_albums=0 AND id NOT IN (SELECT DISTINCT artist_id FROM tracks);");
    /* Genres and Composers: by name as tagged, ids kept from scan to scan */
    rebuild_genres(db, as_added > 0);
    exec_said(db,
        /* search: what a changed name is found by (albums show their artist's) */
        "UPDATE artists SET search=NULL WHERE search IS NOT " SKEY_ARTIST ";"
        "UPDATE albums SET search=NULL WHERE artist_id IN (SELECT id FROM artists WHERE search IS NULL);");
    rebuild_composers(db, as_added > 0);
    if (t_folders) *t_folders = clock_fsec();
    return rebuild_folders(db, as_added > 1);
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
    sqlite3_exec(db, "DROP TABLE IF EXISTS temp.tracks; DROP TABLE IF EXISTS temp.d_tracks;", NULL, NULL, NULL);
    for (size_t i = 0; i < sizeof SHADOWED / sizeof *SHADOWED; i++) {
        char *sql = sqlite3_mprintf("DROP TABLE IF EXISTS temp.%s; DROP TABLE IF EXISTS temp.dg_%s;"
                                    " DROP TABLE IF EXISTS temp.dc_%s; DROP TABLE IF EXISTS temp.dn_%s;",
                                    SHADOWED[i], SHADOWED[i], SHADOWED[i], SHADOWED[i]);
        if (sql) sqlite3_exec(db, sql, NULL, NULL, NULL);
        sqlite3_free(sql);
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
 * to the next new one). In two steps: which rows (dg_, dc_, dn_: gone,
 * changed, new), found before the write lock, reading only (only scans
 * write these tables: they stay right till the write); then, holding it,
 * those rows written. */
static int shadow_apply(sqlite3 *db, const char *table, int write)
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
    char *sql = !(s_set && s_cmp && s_cols && *s_cmp) ? NULL : !write ? sqlite3_mprintf(
        "CREATE TEMP TABLE dg_%s AS SELECT id FROM main.%s WHERE id NOT IN (SELECT id FROM temp.%s);"
        "CREATE TEMP TABLE dc_%s AS SELECT t.id FROM temp.%s AS t JOIN main.%s AS m ON m.id=t.id WHERE %s;"
        "CREATE TEMP TABLE dn_%s AS SELECT id FROM temp.%s AS t"
        " WHERE NOT EXISTS (SELECT 1 FROM main.%s AS m WHERE m.id=t.id);",
        table, table, table, table, table, table, s_cmp, table, table, table) : sqlite3_mprintf(
        "DELETE FROM main.%s WHERE id IN (SELECT id FROM temp.dg_%s);"
        "UPDATE main.%s AS m SET %s FROM temp.%s AS t WHERE t.id=m.id AND m.id IN (SELECT id FROM temp.dc_%s);"
        "INSERT INTO main.%s(id%s) SELECT id%s FROM temp.%s WHERE id IN (SELECT id FROM temp.dn_%s);",
        table, table, table, s_set, table, table, table, s_cols, s_cols, table, table);
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
    double t0 = clock_fsec();
    shadow_drop(db);
    /* (on the H2 the temp store spills to the card, not RAM: SQLITE_TMPDIR,
     * and its cache stays small. 3 MB for a first scan was tried: with 50,000
     * songs the H2 was left under 400 KB of free memory and the writing back
     * took three times as long.) */
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
    if (!rc) rc = rebuild_groups(lib);         /* (on the copies: temp comes first; half made: not put in) */
    if (rc || shadow_exec(db, "COMMIT") != 0) {
        if (!sqlite3_get_autocommit(db)) sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        shadow_drop(db);
        return -1;
    }
    /* what differs, found reading only (see shadow_apply) */
    rc = shadow_exec(db,
        "CREATE TEMP TABLE d_tracks AS SELECT t.id FROM temp.tracks AS t JOIN main.tracks AS m ON m.id=t.id WHERE"
        " m.album_id IS NOT t.album_id OR m.artist_id IS NOT t.artist_id OR m.sort IS NOT t.sort COLLATE BINARY"
        " OR m.genre_id IS NOT t.genre_id OR m.composer_id IS NOT t.composer_id OR m.folder_id IS NOT t.folder_id;"
        "CREATE TEMP TABLE dg_credits AS SELECT m.rowid AS r FROM main.credits AS m WHERE NOT EXISTS"
        " (SELECT 1 FROM temp.credits AS t WHERE t.artist_id=m.artist_id AND t.track_id=m.track_id);"
        "CREATE TEMP TABLE dn_credits AS SELECT artist_id, track_id FROM temp.credits"
        " EXCEPT SELECT artist_id, track_id FROM main.credits;");
    for (size_t i = 0; !rc && i < sizeof SHADOWED / sizeof *SHADOWED; i++)
        if (strcmp(SHADOWED[i], "credits")) rc = shadow_apply(db, SHADOWED[i], 0);
    if (rc) { shadow_drop(db); return -1; }
    double t1 = clock_fsec();
    batch_write(b);
    if (!b->open) { shadow_drop(db); return -1; }
    rc = shadow_exec(db,
        "UPDATE main.tracks AS m SET album_id=t.album_id, artist_id=t.artist_id, sort=t.sort, genre_id=t.genre_id,"
        " composer_id=t.composer_id, folder_id=t.folder_id FROM temp.tracks AS t WHERE t.id=m.id"
        " AND m.id IN (SELECT id FROM temp.d_tracks);"
        "DELETE FROM main.credits WHERE rowid IN (SELECT r FROM temp.dg_credits);"
        "INSERT OR IGNORE INTO main.credits(artist_id, track_id) SELECT artist_id, track_id FROM temp.dn_credits;");
    for (size_t i = 0; !rc && i < sizeof SHADOWED / sizeof *SHADOWED; i++)
        if (strcmp(SHADOWED[i], "credits")) rc = shadow_apply(db, SHADOWED[i], 1);
    if (rc) {                                  /* (nothing of it kept) */
        batch_undo(b);
        shadow_drop(db);
        return -1;
    }
    batch_end(b);
    double t2 = clock_fsec();
    shadow_drop(db);                           /* (temp only: no need of the lock) */
    sqlite3_exec(db, "PRAGMA temp.cache_size=-256", NULL, NULL, NULL);
    sqlite3_db_release_memory(db);             /* (the copies' pages) */
    /* changed tracks (their search cleared), new albums and artists: a hold
     * of its own (they turn up in Search a moment after the lists) */
    batch_write(b);
    index_search(lib, 1);
    batch_end(b);
    fprintf(stderr, "library: groups aside %.2f s, written in %.2f s\n", t1 - t0, t2 - t1);
    return 0;
}

/* ---------- the groups, only where songs changed ----------
 * After a scan that added, changed or removed a few songs (USB drive mode,
 * the network drive), rebuild_groups() went over all of them: on the H2,
 * 50,000 songs, about 2 minutes for one album. Instead: the albums and
 * artists those songs belonged to and now belong to (A, R), every song of
 * those albums and every song naming those artists (S: as artist, album
 * artist or guest), in path order, through the same grp_track(), writing
 * only A's and R's rows; then A's and R's counts, the genres, composers and
 * folders of the changed songs. As rebuild_groups() would leave it (test_scan
 * checks: a full rebuild after it changes nothing). Too many songs around
 * them (Various Artists, say) and the full rebuild, as before.
 *
 * The scan notes which songs it wrote (temp.g_c: their ids, removed ones too)
 * and, before removing any, what they belonged to (groups_note_old). */

#define GROUPS_INCR_CHANGED 2000   /* songs changed, at most */
#define GROUPS_INCR_AROUND 1500    /* songs gone over, at most (~1 ms each on the H2, the write lock held) */

static void groups_incr_drop(sqlite3 *db)
{
    sqlite3_exec(db, "DROP TABLE IF EXISTS temp.g_c; DROP TABLE IF EXISTS temp.g_old; DROP TABLE IF EXISTS temp.g_cred;"
                     "DROP TABLE IF EXISTS temp.g_alb; DROP TABLE IF EXISTS temp.g_art; DROP TABLE IF EXISTS temp.g_s;"
                     "DROP TABLE IF EXISTS temp.g_newkey; DROP TABLE IF EXISTS temp.g_gen; DROP TABLE IF EXISTS temp.g_comp;"
                     "DROP TABLE IF EXISTS temp.g_fd; DROP TABLE IF EXISTS temp.g_fsum;", NULL, NULL, NULL);
}

/* The songs in temp.g_c (filled by the scan): what they belong to now,
 * before they're changed in the groups or removed. 0, or -1 */
static int groups_note_old(sqlite3 *db)
{
    return shadow_exec(db,
        "CREATE TEMP TABLE g_old AS SELECT id AS track_id, album_id, artist_id, genre_id, composer_id, folder_id"
        " FROM main.tracks WHERE id IN (SELECT id FROM temp.g_c);"
        "CREATE TEMP TABLE g_cred AS SELECT DISTINCT artist_id FROM main.credits"
        " WHERE track_id IN (SELECT id FROM temp.g_c);");
}

static int load_idset(sqlite3 *db, const char *sql, idset_t *set)
{
    sqlite3_stmt *q = prep(db, sql);
    if (!q) return -1;
    int rc;
    while ((rc = grp_step(q)) == SQLITE_ROW) idset_add(set, sqlite3_column_int64(q, 0));
    sqlite3_finalize(q);
    return set->failed || rc != SQLITE_DONE ? -1 : 0;
}

/* Folders of the changed songs: theirs found or made, and each folder's count
 * (its songs and those below) moved by what left it and what came in. 0, or
 * -1 (rebuild_folders() then) */
static int folders_where_changed(sqlite3 *db, const char *root)
{
    sqlite3_stmt *q = prep(db, "SELECT id, path, folder_id FROM main.tracks WHERE id IN (SELECT id FROM temp.g_c)"
                               " ORDER BY path");
    folder_stmts_t f;
    int rc = folder_stmts_prep(db, &f) == 0 && q ? 0 : -1, st = SQLITE_DONE;
    while (!rc && (st = grp_step(q)) == SQLITE_ROW) {
        char dir[1024];
        dir_of((const char *)sqlite3_column_text(q, 1), dir, sizeof dir);
        int64_t fid = folder_under(db, f.up, f.get, dir, root);
        if (!fid) { rc = -1; break; }
        folder_put_track(&f, q, fid);
    }
    if (!rc && st != SQLITE_DONE) rc = -1;     /* (stopped: the songs past there keep the folder they had) */
    sqlite3_finalize(q);
    folder_stmts_free(&f);
    /* -1 for each song's old folder and those above, +1 for its new one's */
    return rc ? rc : shadow_exec(db,
        "CREATE TEMP TABLE g_fd(folder INTEGER, d INTEGER);"
        "INSERT INTO g_fd SELECT folder_id, -1 FROM temp.g_old WHERE folder_id IS NOT NULL;"
        "INSERT INTO g_fd SELECT folder_id, 1 FROM main.tracks WHERE id IN (SELECT id FROM temp.g_c)"
        " AND folder_id IS NOT NULL;"
        "CREATE TEMP TABLE g_fsum AS WITH RECURSIVE up(id, d) AS (SELECT folder, d FROM temp.g_fd"
        " UNION ALL SELECT f.parent, up.d FROM up JOIN main.folders f ON f.id=up.id WHERE f.parent<>0)"
        " SELECT id, SUM(d) AS d FROM up GROUP BY id;"
        "UPDATE main.folders SET ntracks=ntracks+(SELECT d FROM temp.g_fsum s WHERE s.id=folders.id)"
        " WHERE id IN (SELECT id FROM temp.g_fsum);"
        "DELETE FROM main.folders WHERE id IN (SELECT id FROM temp.g_fsum) AND ntracks<=0;");
}

/* 0: done; 1: not here (too many songs around them, folders' root moved:
 * nothing written, the full rebuild); -1: failed part way, a pass that
 * stopped before its end too (the caller rolls the batch back, and the full
 * rebuild follows). Holds the write lock (b). temp.g_c and g_old filled. */
static int rebuild_groups_where_changed(library_t *lib, batch_t *b)
{
    sqlite3 *db = lib->db;
    double t0 = clock_fsec();
    char root[1024], had[1024] = "";
    folders_root(db, root, sizeof root);
    sqlite3_stmt *rq = prep(db, "SELECT path, (SELECT COUNT(*) FROM folders WHERE parent=0) FROM folders WHERE parent=0");
    int roots = 0;
    if (rq && sqlite3_step(rq) == SQLITE_ROW) {
        snprintf(had, sizeof had, "%s", (const char *)sqlite3_column_text(rq, 0) ? (const char *)sqlite3_column_text(rq, 0) : "");
        roots = sqlite3_column_int(rq, 1);
    }
    sqlite3_finalize(rq);
    if (roots != 1 || strcmp(root, had)) {
        fprintf(stderr, "library: groups: the music's top folder moved: all of them\n");
        return 1;
    }

    /* A and R: what the changed songs belonged to; then what they belong to
     * now (new names noted, made only once it's known this goes ahead) */
    if (shadow_exec(db,
            "CREATE TEMP TABLE g_alb(id INTEGER PRIMARY KEY); CREATE TEMP TABLE g_art(id INTEGER PRIMARY KEY);"
            "CREATE TEMP TABLE g_s(id INTEGER PRIMARY KEY); CREATE TEMP TABLE g_newkey(kind INTEGER, key TEXT, PRIMARY KEY(kind, key));"
            "INSERT OR IGNORE INTO g_alb SELECT album_id FROM temp.g_old WHERE album_id IS NOT NULL;"
            "INSERT OR IGNORE INTO g_art SELECT artist_id FROM temp.g_old WHERE artist_id IS NOT NULL;"
            "INSERT OR IGNORE INTO g_art SELECT artist_id FROM temp.g_cred;"
            "INSERT OR IGNORE INTO g_art SELECT artist_id FROM main.albums WHERE id IN (SELECT id FROM temp.g_alb)"
            " AND artist_id IS NOT NULL;"))
        return 1;
    sqlite3_stmt *nq = prep(db, "SELECT artist, album_artist, album, title FROM main.tracks WHERE id IN (SELECT id FROM temp.g_c)");
    sqlite3_stmt *fa = prep(db, "SELECT id FROM main.artists WHERE key=?");
    sqlite3_stmt *fb = prep(db, "SELECT id FROM main.albums WHERE key=?");
    sqlite3_stmt *ia = prep(db, "INSERT OR IGNORE INTO temp.g_art VALUES(?)");
    sqlite3_stmt *ib = prep(db, "INSERT OR IGNORE INTO temp.g_alb VALUES(?)");
    sqlite3_stmt *nk = prep(db, "INSERT OR IGNORE INTO temp.g_newkey VALUES(?, ?)");
    int rc = nq && fa && fb && ia && ib && nk ? 0 : 1, st = SQLITE_DONE;
    while (!rc && (st = grp_step(nq)) == SQLITE_ROW) {
        const char *artist = (const char *)sqlite3_column_text(nq, 0), *title = (const char *)sqlite3_column_text(nq, 3);
        tkeys_t k;
        track_keys(artist, (const char *)sqlite3_column_text(nq, 1), (const char *)sqlite3_column_text(nq, 2), &k);
        char names[KEY_CREDIT_MAX][KEY_CREDIT_LEN], ckey[KEY_CREDIT_MAX][600];
        int nn = key_credits(artist ? artist : "", title, names);
        const char *akeys[2 + KEY_CREDIT_MAX] = {k.akey, k.aakey};
        int na = 2;
        for (int i = 0; i < nn; i++) {
            key_artist(names[i], NULL, ckey[i], sizeof ckey[i]);
            if (*ckey[i]) akeys[na++] = ckey[i];
        }
        for (int i = 0; i <= na; i++) {
            int album = i == na;
            const char *key = album ? k.albkey : akeys[i];
            sqlite3_stmt *f = album ? fb : fa;
            sqlite3_reset(f);
            sqlite3_bind_text(f, 1, key, -1, SQLITE_TRANSIENT);
            sqlite3_stmt *ins = f == fb ? ib : ia;
            if (sqlite3_step(f) == SQLITE_ROW) {
                sqlite3_reset(ins);
                sqlite3_bind_int64(ins, 1, sqlite3_column_int64(f, 0));
                sqlite3_step(ins);
            } else {
                sqlite3_reset(nk);
                sqlite3_bind_int(nk, 1, album);
                sqlite3_bind_text(nk, 2, key, -1, SQLITE_TRANSIENT);
                sqlite3_step(nk);
            }
            sqlite3_reset(f);
        }
    }
    if (!rc && st != SQLITE_DONE) rc = 1;      /* (not every changed song's album and artists known) */
    sqlite3_finalize(nq);
    sqlite3_finalize(fa);
    sqlite3_finalize(fb);
    sqlite3_finalize(ia);
    sqlite3_finalize(ib);
    sqlite3_finalize(nk);
    /* S: the changed songs, every song of A's albums, every song naming one of R */
    rc = rc ? rc : shadow_exec(db,
        "INSERT OR IGNORE INTO g_s SELECT id FROM main.tracks WHERE id IN (SELECT id FROM temp.g_c);"
        "INSERT OR IGNORE INTO g_s SELECT id FROM main.tracks WHERE album_id IN (SELECT id FROM temp.g_alb);"
        "INSERT OR IGNORE INTO g_s SELECT id FROM main.tracks WHERE artist_id IN (SELECT id FROM temp.g_art);"
        "INSERT OR IGNORE INTO g_s SELECT t.id FROM main.albums a JOIN main.tracks t ON t.album_id=a.id"
        " WHERE a.artist_id IN (SELECT id FROM temp.g_art);"
        "INSERT OR IGNORE INTO g_s SELECT track_id FROM main.credits WHERE artist_id IN (SELECT id FROM temp.g_art);") ? 1 : 0;
    int64_t around = rc ? 0 : get_int_col(lib, "SELECT COUNT(*) FROM temp.g_s", 0);
    if (!rc && around > GROUPS_INCR_AROUND) {
        fprintf(stderr, "library: groups: %lld songs around those changed: all of them\n", (long long)around);
        rc = 1;
    }
    if (rc) return 1;

    /* from here on it writes */
    double t_found = clock_fsec();
    batch_write(b);
    if (!b->open) return 1;
    double t_locked = clock_fsec();
    idset_t A = {0}, R = {0};
    grp_t g;
    sqlite3_stmt *mk = prep(db, "SELECT kind, key FROM temp.g_newkey");
    rc = mk && grp_open(&g, db) == 0 ? 0 : -1;
    if (rc) { sqlite3_finalize(mk); return 1; }
    /* (compiled once: with a statement made and run for each new key, an
     * album of new artists compiled it a dozen times, the write lock held) */
    sqlite3_stmt *note[2] = {prep(db, "INSERT OR IGNORE INTO temp.g_art VALUES(?)"),
                             prep(db, "INSERT OR IGNORE INTO temp.g_alb VALUES(?)")};
    while ((st = grp_step(mk)) == SQLITE_ROW) {
        int album = sqlite3_column_int(mk, 0);
        int64_t id = upsert_key(album ? &g.u_alb : &g.u_art, (const char *)sqlite3_column_text(mk, 1));
        sqlite3_stmt *ins = note[album != 0];
        if (!ins) continue;
        sqlite3_reset(ins);
        sqlite3_bind_int64(ins, 1, id);
        sqlite3_step(ins);
        sqlite3_reset(ins);
    }
    sqlite3_finalize(note[0]);
    sqlite3_finalize(note[1]);
    sqlite3_finalize(mk);
    rc = st != SQLITE_DONE || load_idset(db, "SELECT id FROM temp.g_alb", &A) || load_idset(db, "SELECT id FROM temp.g_art", &R) ? -1 : 0;
    rc = rc ? rc : shadow_exec(db,
        "UPDATE main.albums SET year=0, dir=NULL, first_track=NULL WHERE id IN (SELECT id FROM temp.g_alb);"
        "UPDATE main.tracks SET sort=songkey(COALESCE(title,'')) WHERE id IN (SELECT id FROM temp.g_c)"
        " AND sort IS NOT songkey(COALESCE(title,''));"
        "DELETE FROM main.credits WHERE track_id IN (SELECT id FROM temp.g_c);"
        "UPDATE main.artists SET name=NULL WHERE id IN (SELECT id FROM temp.g_art);");
    sqlite3_stmt *q = rc ? NULL : prep(db, "SELECT id, artist, album_artist, album, year, path, title, album_id, artist_id"
                                           " FROM main.tracks WHERE id IN (SELECT id FROM temp.g_s) ORDER BY path");
    if (!rc && !q) rc = -1;
    g.albums = &A;
    g.artists = &R;
    st = SQLITE_DONE;
    while (!rc && (st = grp_step(q)) == SQLITE_ROW) grp_track(&g, q);
    if (!rc && st != SQLITE_DONE) rc = -1;
    sqlite3_finalize(q);
    grp_close(&g);
    double t1 = clock_fsec();

    /* A's and R's counts; the empty ones gone (an album's covers with it) */
    rc = rc ? rc : shadow_exec(db,
        "UPDATE main.albums SET ntracks=(SELECT COUNT(*) FROM main.tracks WHERE album_id=albums.id)"
        " WHERE id IN (SELECT id FROM temp.g_alb);");
    sqlite3_stmt *gone = rc ? NULL : prep(db, "SELECT id FROM main.albums WHERE ntracks=0 AND id IN (SELECT id FROM temp.g_alb)");
    if (!rc && !gone) rc = -1;
    while (gone && (st = grp_step(gone)) == SQLITE_ROW) art_del(lib, sqlite3_column_int64(gone, 0));
    if (!rc && st != SQLITE_DONE) rc = -1;
    sqlite3_finalize(gone);
    rc = rc ? rc : shadow_exec(db,
        "DELETE FROM main.albums WHERE ntracks=0 AND id IN (SELECT id FROM temp.g_alb);"
        "UPDATE main.artists SET nalbums=(SELECT COUNT(*) FROM main.albums WHERE artist_id=artists.id),"
        " ncredit_albums=(SELECT COUNT(DISTINCT t.album_id) FROM main.credits c JOIN main.tracks t ON t.id=c.track_id"
        " WHERE c.artist_id=artists.id) WHERE id IN (SELECT id FROM temp.g_art);"
        "DELETE FROM main.artists WHERE id IN (SELECT id FROM temp.g_art) AND nalbums=0 AND ncredit_albums=0"
        " AND NOT EXISTS (SELECT 1 FROM main.tracks WHERE artist_id=artists.id);"
        /* genres and composers of the changed songs, old and new */
        NAMED_CHANGED_SQL("genres", "genre", "g_gen")
        NAMED_CHANGED_SQL("composers", "composer", "g_comp")
        /* search: what a changed name is found by (albums show their artist's) */
        "UPDATE main.artists SET search=NULL WHERE search IS NOT " SKEY_ARTIST ";"
        "UPDATE main.albums SET search=NULL WHERE artist_id IN (SELECT id FROM main.artists WHERE search IS NULL);");
    double t2 = clock_fsec();
    rc = rc ? rc : folders_where_changed(db, root);
    free(A.bits);
    free(R.bits);
    fprintf(stderr, "library: groups where songs changed: %lld songs, %s in %.2f s (found %.2f, lock waited %.2f,"
            " tracks %.2f, counts %.2f, folders %.2f: the lock held %.2f s so far)\n", (long long)around,
            rc ? "failed" : "done", clock_fsec() - t0, t_found - t0, t_locked - t_found, t1 - t_locked, t2 - t1, clock_fsec() - t2,
            clock_fsec() - t_locked);
    return rc ? -1 : 0;
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

/* ---- the covers' store ----
 * Two files in the art folder, large.pack and small.pack: each album's cover
 * at its id's place (a slot: 8 bytes, then the pixels), read with one pread.
 * Before, a file per album and size ("12-136.rgb565") in one folder: on the
 * card's FAT a folder is searched from its start for every open and every new
 * file, so 4,200 albums' 8,400 files cost ~15 ms a cover to open (a list of
 * them, the whole first scan's cover step half as long again) and 32 KB of
 * card each. A slot says it's whole (a sum of its pixels): one a power cut
 * left half written reads as no cover, and the album is looked at again.
 * Those older files are still read where the store has none, and moved into
 * it by the next scan (art_migrate). */
#define ART_MAGIC 0x31634146u                      /* "FAc1" */
#define ART_MAX_BYTES 0xFFF00000ull                /* (a file on FAT32: under 4 GB) */

static int art_idx(int size) { return size == LIB_ART_LARGE ? 0 : size == LIB_ART_SMALL ? 1 : -1; }
static size_t art_pixels(int size) { return (size_t)size * (size_t)size * 2; }
static size_t art_slot(int size) { return 8 + art_pixels(size); }

static uint32_t art_sum(const void *px, size_t n) { return fnv32(FNV32_START, px, n); }

static void art_legacy_path(const library_t *lib, int64_t album_id, int size, char *out, size_t cap)
{
    snprintf(out, cap, "%s/%lld-%d.rgb565", lib->art_dir, (long long)album_id, size);
}

/* Whether any of the older cover files is left in the art folder */
static int art_legacy_any(const library_t *lib)
{
    DIR *d = opendir(lib->art_dir);
    if (!d) return 0;
    struct dirent *e;
    int any = 0;
    while (!any && (e = readdir(d))) {
        const char *dot = strrchr(e->d_name, '.');
        any = dot && !strcmp(dot, ".rgb565");
    }
    closedir(d);
    return any;
}

static void art_open(library_t *lib)
{
    static const char *const NAME[2] = {"large.pack", "small.pack"};
    for (int i = 0; i < 2; i++) {
        char path[1200];
        snprintf(path, sizeof path, "%s/%s", lib->art_dir, NAME[i]);
        lib->art_fd[i] = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    }
    lib->art_legacy = art_legacy_any(lib);
}

/* Where an album's slot is, or -1 (no such size, or past what a file can hold) */
static int64_t art_at(int64_t album_id, int size)
{
    if (album_id <= 0 || art_idx(size) < 0) return -1;
    uint64_t off = (uint64_t)album_id * art_slot(size);
    return off + art_slot(size) <= ART_MAX_BYTES ? (int64_t)off : -1;
}

static int art_put(library_t *lib, int64_t album_id, int size, const uint16_t *px)
{
    int64_t off = art_at(album_id, size);
    if (off < 0 || lib->art_fd[art_idx(size)] < 0) return -1;
    size_t n = art_pixels(size);
    unsigned char *slot = malloc(8 + n);
    if (!slot) return -1;
    uint32_t head[2] = {ART_MAGIC, art_sum(px, n)};
    memcpy(slot, head, 8);
    memcpy(slot + 8, px, n);
    ssize_t w = pwrite(lib->art_fd[art_idx(size)], slot, 8 + n, (off_t)off);   /* (one write: a reader sees all or none, a cut aside) */
    free(slot);
    return w == (ssize_t)(8 + n) ? 0 : -1;
}

/* What's been put, on the card (before the library says it's there: a row
 * that outlived its cover in a power cut would never be looked at again) */
static void art_sync(library_t *lib)
{
    for (int i = 0; i < 2; i++)
        if (lib->art_fd[i] >= 0) fdatasync(lib->art_fd[i]);
}

/* The store's own (0), or -1 */
static int art_get_stored(library_t *lib, int64_t album_id, int size, uint16_t *px)
{
    int64_t off = art_at(album_id, size);
    if (off < 0 || lib->art_fd[art_idx(size)] < 0) return -1;
    uint32_t head[2];
    size_t n = art_pixels(size);
    int fd = lib->art_fd[art_idx(size)];
    if (pread(fd, head, 8, (off_t)off) != 8 || head[0] != ART_MAGIC) return -1;
    if (pread(fd, px, n, (off_t)off + 8) != (ssize_t)n) return -1;
    return art_sum(px, n) == head[1] ? 0 : -1;
}

static int art_get(library_t *lib, int64_t album_id, int size, uint16_t *px)
{
    if (art_get_stored(lib, album_id, size, px) == 0) return 0;
    if (!lib->art_legacy) return -1;
    char path[1200];
    art_legacy_path(lib, album_id, size, path, sizeof path);
    FILE *f = fopen(path, "rbe");
    if (!f) return -1;
    size_t n = fread(px, 2, (size_t)size * size, f);
    fclose(f);
    return n == (size_t)size * size ? 0 : -1;
}

/* An album's covers gone (the album is, or its cover): its slots emptied
 * (never growing the file for it), its older files removed */
static void art_del(library_t *lib, int64_t album_id)
{
    static const int sizes[2] = {LIB_ART_LARGE, LIB_ART_SMALL};
    static const unsigned char none[8];
    for (int i = 0; i < 2; i++) {
        int64_t off = art_at(album_id, sizes[i]);
        struct stat st;
        if (off >= 0 && lib->art_fd[i] >= 0 && fstat(lib->art_fd[i], &st) == 0 && (int64_t)st.st_size >= off + 8)
            if (pwrite(lib->art_fd[i], none, 8, (off_t)off) != 8) { /* (stays: looked at again when it shows) */ }
        if (lib->art_legacy) {
            char path[1200];
            art_legacy_path(lib, album_id, sizes[i], path, sizeof path);
            unlink(path);
        }
    }
}

/* The older cover files moved into the store (a scan's first step, while any
 * is left): each read, put in its slot unless the store has that cover
 * already, and removed. Stopped (lib->cancel): the rest next time. */
static void art_migrate(library_t *lib)
{
    if (!lib->art_legacy) return;
    double t0 = clock_fsec();
    int moved = 0, left = 0;
    uint16_t *px = malloc(art_pixels(LIB_ART_LARGE));
    char (*names)[40] = malloc(256 * sizeof *names);
    while (px && names && !lib->cancel) {
        DIR *d = opendir(lib->art_dir);                   /* (a batch of names, then the folder closed: files go from it) */
        if (!d) break;
        struct dirent *e;
        int n = 0;
        while (n < 256 && (e = readdir(d))) {
            const char *dot = strrchr(e->d_name, '.');
            if (dot && !strcmp(dot, ".rgb565") && strlen(e->d_name) < sizeof *names) strcpy(names[n++], e->d_name);
        }
        closedir(d);
        if (!n) break;
        int done = 0;
        for (; done < n && !lib->cancel; done++) {
            char path[1200];
            long long id = 0;
            int size = 0;
            snprintf(path, sizeof path, "%s/%s", lib->art_dir, names[done]);
            if (sscanf(names[done], "%lld-%d.rgb565", &id, &size) == 2 && art_at(id, size) >= 0 &&
                art_get_stored(lib, id, size, px) != 0) {
                FILE *f = fopen(path, "rbe");
                size_t got = f ? fread(px, 2, (size_t)size * size, f) : 0;
                if (f) fclose(f);
                if (got == (size_t)size * size && art_put(lib, id, size, px) == 0) moved++;
            }
        }
        art_sync(lib);                                    /* in the store, on the card: then their files go */
        for (int i = 0; i < done; i++) {
            char path[1200];
            snprintf(path, sizeof path, "%s/%s", lib->art_dir, names[i]);
            if (unlink(path) != 0) left++;                /* (a card that can't be written: not for ever) */
        }
        if (left) break;
    }
    free(px);
    free(names);
    if (!lib->cancel && !left) lib->art_legacy = art_legacy_any(lib);
    fprintf(stderr, "library: covers: %d moved into the store in %.1f s%s\n", moved, clock_fsec() - t0,
            lib->art_legacy ? " (more next time)" : "");
}

int library_art_legacy(library_t *lib)
{
    if (lib->art_legacy) lib->art_legacy = art_legacy_any(lib);   /* (another connection's scan may have moved them) */
    return lib->art_legacy;
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

/* g_art_later: a cover wasn't made, and not for anything the picture is: the
 * helper was short of memory for it (rd_art), or its file is there and
 * couldn't be read just now (art_unread). Nothing is concluded about the
 * album's cover then, and the next scan looks again. */
static _Thread_local int g_art_later;

/* A picture's file gave no picture: f as it was opened and read (NULL: it
 * couldn't be opened, and errno says why). A file that is there and couldn't
 * be opened, or one whose read failed (the card not answering, no memory or
 * file handles), is not "this isn't a picture": g_art_later. (It was: the
 * album marked as having no cover, its thumbnails emptied, and not looked at
 * again until its files changed.) */
static void art_unread(FILE *f)
{
    struct stat st;
    if (f ? ferror(f) && fstat(fileno(f), &st) == 0 && S_ISREG(st.st_mode) : errno != ENOENT && errno != ENOTDIR)
        g_art_later = 1;
}

/* An image file, decoded small (artdec.h). NULL if it isn't one we can read,
 * or couldn't be read just now (art_unread). */
static unsigned char *art_from_file(const char *file, int *w, int *h)
{
    FILE *f = fopen(file, "rbe");
    unsigned char *img = NULL;
    if (f && art_decode(f, 0, -1, LIB_ART_LARGE * 3 / 2, &ART_LIMITS, &img, w, h)) img = NULL;
    if (!img) art_unread(f);
    if (f) art_done(f);
    return img;
}

int library_art_decode_file(const char *path, uint16_t *large, uint16_t *small)
{
    int w, h;
    unsigned char *img = art_from_file(path, &w, &h);
    if (!img) return -1;
    int r = art_square(img, w, h, LIB_ART_LARGE, large);
    if (r == 0) r = art_square(img, w, h, LIB_ART_SMALL, small);
    free(img);
    return r;
}

/* A track's embedded cover: a FLAC's read from where it is in the file
 * (any size), others' through memory (up to FATHOM_ART_MAX_FILE). */
static unsigned char *art_from_track(const char *track, int *w, int *h)
{
    long off, len;
    unsigned char *img = NULL, *enc;
    size_t n;
    /* A track of an album in one file (its path the file's and then "::01@...",
     * cue.h): the picture is the file's. The tags' reader takes such a path;
     * the file was then opened by it here, which is no file's name, and an
     * album split by a sheet had no cover unless a picture lay beside it. */
    char file[1100];
    const char *of = cue_part_file(track, file, sizeof file);
    if (of) track = of;
    if (tags_picture_at(track, &off, &len) == 0) {
        FILE *f = fopen(track, "rbe");
        if (f && art_decode(f, off, len, LIB_ART_LARGE * 3 / 2, &ART_LIMITS, &img, w, h)) img = NULL;
        if (!img) art_unread(f);
        if (f) art_done(f);
    } else if (strcasecmp(path_ext(track), "flac") && tags_picture(track, &enc, &n) == 0) {
        /* (not asked of a FLAC: its picture is the one tags_picture_at has just looked for, and not found) */
        if (art_decode_mem(enc, n, LIB_ART_LARGE * 3 / 2, &ART_LIMITS, &img, w, h)) img = NULL;
        free(enc);
    } else {
        /* (no picture, the tags' reader says, and says the same of a song it
         * couldn't open or read: which, asked of the file's first bytes) */
        char b[16];
        FILE *f = fopen(track, "rbe");
        if (!f || fread(b, 1, sizeof b, f) < sizeof b) art_unread(f);
        if (f) fclose(f);
    }
    return img;
}

/* ---------- the scan's reads, done by the helper (reader.h) ----------
 * A file's tags, the CUE sheet in them, and a cover decoded small from an
 * image file or from a song's own picture: what a scan runs on files it has
 * never seen. library_reader_do is the helper's side. */
enum { RD_TAGS = 1, RD_SHEET, RD_ART_FILE, RD_ART_TRACK };
#define RD_UNREAD (-5)   /* a cover's answer: none, the file being there and not read just now (art_unread) */

int library_reader_do(int op, const char *path, unsigned char **out, size_t *len)
{
    switch (op) {
    case RD_TAGS: {
        tags_t *t = malloc(sizeof *t);
        if (!t) return -1;
        int r = tags_read(path, t);
        *out = (unsigned char *)t;
        *len = sizeof *t;
        return r ? -1 : 0;
    }
    case RD_SHEET: {
        char *text = NULL;
        size_t n = 0;
        if (tags_cuesheet(path, &text, &n)) return -1;
        *out = (unsigned char *)text;
        *len = n;
        return 0;
    }
    case RD_ART_FILE:
    case RD_ART_TRACK: {
        int w = 0, h = 0;
        g_art_later = 0;
        unsigned char *img = op == RD_ART_FILE ? art_from_file(path, &w, &h) : art_from_track(path, &w, &h);
        if (!img || w <= 0 || h <= 0) { free(img); return g_art_later ? RD_UNREAD : -1; }
        size_t n = (size_t)w * (size_t)h * 3;
        unsigned char *b = malloc(8 + n);          /* its width and height, then the pixels */
        if (b) {
            int32_t wh[2] = {w, h};
            memcpy(b, wh, 8);
            memcpy(b + 8, img, n);
            *out = b;
            *len = 8 + n;
        }
        free(img);
        return b ? 0 : -1;
    }
    default: return -1;
    }
}

/* What the helper may take for one file: the memory available now less a
 * reserve for everything else (the player, the music it is playing), and at
 * least RD_LEAST_KB. Under RD_FULL_KB it is "short": a file that fails then
 * may only have wanted more than there was at that moment (a song playing,
 * Wi-Fi and Bluetooth on), so nothing is concluded about it and a later scan
 * reads it again. With RD_FULL_KB or more a failure is the file's own. 0
 * where the system doesn't say what is available (not the H2): no limit. */
#define RD_RESERVE_KB 2048
#define RD_LEAST_KB   1024
#define RD_FULL_KB    4096
static long g_test_budget = -1;
static long rd_budget(int *is_short);

void library_test_reader_budget(long kb) { g_test_budget = kb; }

long library_reader_budget(int *is_short) { return rd_budget(is_short); }

static long rd_budget(int *is_short)
{
    long avail = 0;
    if (g_test_budget >= 0) avail = g_test_budget + RD_RESERVE_KB;
    else {
        FILE *f = fopen("/proc/meminfo", "re");
        char line[128];
        while (f && fgets(line, sizeof line, f))
            if (sscanf(line, "MemAvailable: %ld", &avail) == 1) break;
        if (f) fclose(f);
    }
    *is_short = 0;
    if (avail <= 0) return 0;
    long b = avail - RD_RESERVE_KB;
    *is_short = b < RD_FULL_KB;
    return b < RD_LEAST_KB ? RD_LEAST_KB : b;
}

/* A file's tags: 0, -1 (not read: t emptied), or READER_DIED (the helper
 * died on it twice running, or hung). *later (if given): it wasn't read, and
 * the helper was short of memory for it (rd_budget): again at a later scan. */
static int rd_tags(const char *path, tags_t *t, int *later)
{
    if (later) *later = 0;
    if (reader_on()) {
        int st, is_short;
        unsigned char *b;
        size_t n;
        long budget = rd_budget(&is_short);
        int r = reader_call(RD_TAGS, path, budget, &st, &b, &n);
        if (r == 0) {
            if (st == 0 && n == sizeof *t) memcpy(t, b, n);
            else { memset(t, 0, sizeof *t); st = -1; }
            free(b);
            if (st && later) *later = is_short;
            return st;
        }
        if (r == READER_DIED) {
            memset(t, 0, sizeof *t);
            if (later) *later = is_short;
            return r;
        }
    }
    return tags_read(path, t) ? -1 : 0;
}

/* The sheet in a file's tags (as tags_cuesheet) */
static int rd_sheet(const char *path, char **text, size_t *len)
{
    if (reader_on()) {
        int st, is_short;
        unsigned char *b;
        size_t n;
        int r = reader_call(RD_SHEET, path, rd_budget(&is_short), &st, &b, &n);
        if (r == READER_DIED) return -1;
        if (r == 0) {
            char *t = st == 0 && b ? realloc(b, n + 1) : NULL;
            if (!t) { free(b); return -1; }
            t[n] = '\0';
            *text = t;
            *len = n;
            return 0;
        }
    }
    return tags_cuesheet(path, text, len);
}

/* A cover decoded small (as art_from_file, art_from_track): NULL too when the
 * helper died on it, and the album's cover is looked for elsewhere.
 * g_art_later: one wasn't made while the helper was short of memory, or the
 * helper couldn't read its file just now. */
/* g_rd_nowait: this thread doesn't wait for a helper that's busy (the
 * screen's: library_remove_found_cover) */
static _Thread_local int g_rd_nowait;

static unsigned char *rd_art(int op, const char *path, int *w, int *h)
{
    if (reader_on()) {
        int st, is_short;
        unsigned char *b;
        size_t n;
        long budget = rd_budget(&is_short);
        int r = g_rd_nowait ? reader_call_nowait(op, path, budget, &st, &b, &n) : reader_call(op, path, budget, &st, &b, &n);
        if (r == READER_DIED || (r == 0 && st)) g_art_later |= is_short || (r == 0 && st == RD_UNREAD);
        if (r == READER_DIED) {
            fprintf(stderr, "library: the reader stopped on %s: no cover from it\n", path);
            return NULL;
        }
        if (r == 0) {
            int32_t wh[2] = {0, 0};
            if (st == 0 && n > 8) memcpy(wh, b, 8);
            if (wh[0] > 0 && wh[1] > 0 && wh[0] <= 8192 && wh[1] <= 8192 && (size_t)wh[0] * (size_t)wh[1] * 3 == n - 8) {
                *w = wh[0];
                *h = wh[1];
                memmove(b, b + 8, n - 8);
                return b;
            }
            free(b);
            return NULL;
        }
    }
    return op == RD_ART_FILE ? art_from_file(path, w, h) : art_from_track(path, w, h);
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

/* The album's cover, best first: one found online that you picked over its
 * own (Find another cover: KEYHASH.over beside it); an image named like a
 * cover in its folder (or, for a disc's folder, in the album's above it);
 * the first track's embedded one, then up to four other tracks' that have
 * one; one found online and kept (coverfind.h); any other image in the
 * folder (a scan or a booklet page loses to an embedded cover, and to a
 * found one). Each is tried until one decodes. 0: its own; 1: the found one;
 * 2: the one picked over its own; -1: none. */
static int make_art(library_t *lib, sqlite3_stmt *others, int64_t album_id, const char *dir, const char *first_track,
                    const char *key)
{
    char file[2048], up[2048], pfile[2048], found[1200], over[1200];
    int was_found = 0;
    int w = 0, h = 0;
    unsigned char *img = NULL;
    struct stat ost;
    if (key && library_found_cover_path(key, "over", over, sizeof over) == 0 && stat(over, &ost) == 0 &&
        library_found_cover_path(key, "jpg", found, sizeof found) == 0 && (img = rd_art(RD_ART_FILE, found, &w, &h)) != NULL)
        was_found = 2;
    int rank = img ? -1 : find_cover_file(dir, file, sizeof file);
    int prank = -1;
    if ((rank < 0 || rank > 4) && disc_folder(dir) && parent_dir(dir, up, sizeof up) == 0) {
        prank = find_cover_file(up, pfile, sizeof pfile);
        if (prank > 4) prank = -1;                   /* (only one named like a cover up there) */
    }
    if (rank >= 0 && rank <= 4) img = rd_art(RD_ART_FILE, file, &w, &h);
    if (!img && prank >= 0) img = rd_art(RD_ART_FILE, pfile, &w, &h);
    if (!img && first_track) img = rd_art(RD_ART_TRACK, first_track, &w, &h);
    if (!img && others) {
        sqlite3_reset(others);
        sqlite3_bind_int64(others, 1, album_id);
        sqlite3_bind_text(others, 2, first_track ? first_track : "", -1, SQLITE_STATIC);
        while (!img && sqlite3_step(others) == SQLITE_ROW)
            img = rd_art(RD_ART_TRACK, (const char *)sqlite3_column_text(others, 0), &w, &h);
        sqlite3_reset(others);
    }
    if (!img && key && library_found_cover_path(key, "jpg", found, sizeof found) == 0 &&
        (img = rd_art(RD_ART_FILE, found, &w, &h)) != NULL) was_found = 1;
    if (!img && rank > 4) img = rd_art(RD_ART_FILE, file, &w, &h);
    if (!img) return -1;
    const int sizes[2] = {LIB_ART_LARGE, LIB_ART_SMALL};
    int ok = 0;
    for (int i = 0; i < 2; i++) {
        int s = sizes[i];
        uint16_t *px = malloc((size_t)s * (size_t)s * 2);
        if (px && art_square(img, w, h, s, px) == 0) ok += art_put(lib, album_id, s, px) == 0;
        free(px);
    }
    free(img);
    return ok == 2 ? was_found : -1;
}

/* What an album's cover comes from (or could), as one number: the images in
 * its folder (and a disc's album folder) by name, size and date, as the walk
 * saw them, and its tracks (how many, their times, how many have a picture).
 * The same next scan: nothing to look at again. */
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
        sig = fnv64(sig ^ FNV64_START, name, strlen(name));
        sig = fnv64(sig, &iv->v[i].size, sizeof iv->v[i].size);
        sig = fnv64(sig, &iv->v[i].mtime, sizeof iv->v[i].mtime);
    }
    return sig;
}

static int64_t art_sig(const pathvec_t *iv, sqlite3_stmt *tracks, int64_t album_id, const char *dir)
{
    char up[2048];
    uint64_t sig = images_sig(iv, dir);
    if (disc_folder(dir) && parent_dir(dir, up, sizeof up) == 0) sig = fnv64(sig, "^", 1) ^ images_sig(iv, up);
    sqlite3_reset(tracks);
    sqlite3_bind_int64(tracks, 1, album_id);
    if (sqlite3_step(tracks) == SQLITE_ROW) {
        int64_t v[3] = {sqlite3_column_int64(tracks, 0), sqlite3_column_int64(tracks, 1), sqlite3_column_int64(tracks, 2)};
        sig = fnv64(sig, v, sizeof v);
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
/* (an album's other songs with a picture, up to four: ?1 the album, ?2 its first song's path) */
#define ART_OTHERS_SQL "SELECT path FROM tracks WHERE album_id=? AND has_pic=1 AND path<>? ORDER BY disc, track LIMIT 4"

static void build_art(library_t *lib, const pathvec_t *iv, batch_t *b, lib_progress_cb cb, void *ctx)
{
    sqlite3_stmt *q = prep(lib->db,
        "SELECT al.id, al.dir, t.path, al.art, al.art_sig, al.key FROM albums al LEFT JOIN tracks t ON t.id=al.first_track"
        " WHERE al.id>? ORDER BY al.id LIMIT " LIB_STR(ART_PAGE));
    sqlite3_stmt *u = prep(lib->db, "UPDATE albums SET art=?, art_sig=? WHERE id=?");
    sqlite3_stmt *others = prep(lib->db, ART_OTHERS_SQL);
    sqlite3_stmt *tracks = prep(lib->db,
        "SELECT COUNT(*), TOTAL(mtime), TOTAL(has_pic) FROM tracks WHERE album_id=?");
    struct { int64_t id, was, sig; int art, now; char *dir, *first, *key; } page[ART_PAGE];   /* now: the state to write (-1: as it is) */
    int n = 0, retried = 0, found = 0, got;
    int64_t after = 0;
    if (!q || !u || !others || !tracks) goto out;
    /* (for the progress: the albums no scan has looked at, all of them the
     * first time; later ones look at a few more, whose files changed) */
    int todo = cb ? (int)get_int_col(lib, "SELECT COUNT(*) FROM albums WHERE art=0", 0) : 0;
    char died[32];
    int64_t poison = library_trying_left("cover", died, sizeof died) ? atoll(died) : 0;   /* the album the player died on */
    if (poison) fprintf(stderr, "library: the player stopped while making album %lld's cover: left without one\n", (long long)poison);
    int trying = -1;
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
            page[got].now = -1;
            const unsigned char *ky = sqlite3_column_text(q, 5);
            page[got].key = ky ? strdup((const char *)ky) : NULL;
            after = page[got].id;
            got++;
        }
        sqlite3_reset(q);
        batch_end(b);
        /* The page's covers made (decoding one can take a second: never with
         * the write lock held), put on the card, and only then its rows
         * written: a power cut never leaves a row saying "has a cover" for
         * one that didn't reach the card. */
        int made = 0;
        for (int k = 0; k < got && !lib->cancel; k++) {   /* (stopped: the rest at the next scan) */
            int64_t id = page[k].id, was = page[k].was;
            int art = page[k].art;
            const char *dir = page[k].dir ? page[k].dir : "";
            int64_t sig = art_sig(iv, tracks, id, dir);
            if (art != 0 && sig == was) continue;   /* nothing new to look at */
            int r = 0;
            if (!(art == 1 && was == 0)) {          /* (found before signatures: kept, its signature noted) */
                char note[32];
                snprintf(note, sizeof note, "%lld", (long long)id);
                library_trying("cover", &trying, note);
                g_art_later = 0;
                r = id == poison ? -1 : make_art(lib, others, id, dir, page[k].first, page[k].key);
                if (r < 0 && g_art_later) {         /* (not made with little memory to spare, or its file not read just now: as it was, looked for again next scan) */
                    lib->art_later = 1;             /* (...which mustn't settle for the listing being as it was) */
                    made++;
                    continue;
                }
                if (art != 0) retried++;
                if (r >= 0) found++;
                else art_del(lib, id);              /* none (its cover gone, or an album's id used again): no thumbnails left */
                made++;
                if (cb && (++n % 20) == 0) cb(n, -1 - todo, "cover art", ctx);
            }
            page[k].sig = sig;
            page[k].now = r == 0 ? LIB_COVER_OWN : r == 1 ? LIB_COVER_FOUND : r == 2 ? LIB_COVER_PICKED : LIB_COVER_NONE;
            if (id == poison && r < 0) {
                /* written now, not with its page: the player dying on the
                 * next album's cover would have this one forgotten, and
                 * the two of them tried in turn for ever (as the songs
                 * were: scan_once) */
                batch_write(b);
                if (b->open) {
                    sqlite3_reset(u);
                    sqlite3_bind_int(u, 1, LIB_COVER_NONE);
                    sqlite3_bind_int64(u, 2, sig);
                    sqlite3_bind_int64(u, 3, id);
                    sqlite3_step(u);
                    sqlite3_reset(u);
                    page[k].now = -1;
                }
                batch_end(b);
            }
        }
        if (made) art_sync(lib);
        batch_write(b);
        for (int k = 0; k < got && b->open; k++) {
            if (page[k].now < 0) continue;
            sqlite3_reset(u);
            sqlite3_bind_int(u, 1, page[k].now);
            sqlite3_bind_int64(u, 2, page[k].sig);
            sqlite3_bind_int64(u, 3, page[k].id);
            sqlite3_step(u);
            sqlite3_reset(u);
        }
        batch_end(b);
        for (int k = 0; k < got; k++) {
            free(page[k].dir);
            free(page[k].first);
            free(page[k].key);
        }
    } while (got == ART_PAGE && !lib->cancel);
    batch_end(b);
    library_trying_done("cover", &trying);
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

/* A step while a playlist is matched or written: what it returned, and
 * *bad set if it failed (a read the card didn't answer, no memory, busy).
 * That is not "no such song": the playlist is then left as it was, and read
 * at the next scan. (An entry whose lookup failed was "not in the library",
 * and the playlist was written without it, or with its songs cleared and not
 * all put back, under the file's new date and size: known, so it stayed.) */
static int g_test_pl_fail, g_test_pl_steps;
void library_test_playlists_fail(int at_step) { g_test_pl_fail = at_step; g_test_pl_steps = 0; }

static int pl_step(sqlite3_stmt *s, int *bad)
{
    int rc = g_test_pl_fail && ++g_test_pl_steps == g_test_pl_fail ? SQLITE_IOERR : sqlite3_step(s);
    if (rc != SQLITE_ROW && rc != SQLITE_DONE) *bad = 1;
    return rc;
}

/* Entries ent[0..n) of m (n <= LOOSE_CHUNK): their tracks into match[].
 * 0, or -1: the library couldn't be gone through to its end (no memory, a
 * step that failed), and nothing is said of them. */
static int loose_match(sqlite3 *db, const m3u_t *m, const int *ent, int n, int64_t *match)
{
    probe_t *p3 = calloc((size_t)n, sizeof *p3), *p2 = calloc((size_t)n, sizeof *p2), *pt = calloc((size_t)n, sizeof *pt);
    int64_t *best = calloc((size_t)n * 3, sizeof *best);   /* by entry: by 3 components, 2, tags */
    int *diff = calloc((size_t)n, sizeof *diff);
    sqlite3_stmt *q = prep(db, "SELECT id, path, artist, title, duration_ms FROM tracks");
    char key[1024], text[1024];
    int n3 = 0, n2 = 0, nt = 0, bad = 1;
    if (!p3 || !p2 || !pt || !best || !diff || !q) goto out;
    bad = 0;
    for (int k = 0; k < n; k++) {
        const int j = ent[k];
        path_loose_key(m->paths[j], 3, key, sizeof key);
        if (*key && (p3[n3].key = strdup(key))) p3[n3++].k = k;
        else if (*key) bad = 1;
        path_loose_key(m->paths[j], 2, key, sizeof key);
        if (*key && (p2[n2].key = strdup(key))) p2[n2++].k = k;
        else if (*key) bad = 1;
        if (m->infos[j]) {
            text_loose_key(m->infos[j], key, sizeof key);
            if (*key && (pt[nt].key = strdup(key))) pt[nt++].k = k;
            else if (*key) bad = 1;
        }
    }
    if (bad) goto out;
    qsort(p3, (size_t)n3, sizeof *p3, cmp_probe);
    qsort(p2, (size_t)n2, sizeof *p2, cmp_probe);
    qsort(pt, (size_t)nt, sizeof *pt, cmp_probe);
    while (pl_step(q, &bad) == SQLITE_ROW) {
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
    for (int k = 0; !bad && k < n; k++)
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
    return bad ? -1 : 0;
}

/* How a playlist sorts: by its file's name. */
void playlist_sort_key(const char *path, char *sort, size_t cap)
{
    const char *base = strrchr(path, '/');
    sort_key(base ? base + 1 : path, sort, cap);
}

/* Re-read playlists and match their entries to tracks: the exact path,
 * else the tracks of a file a CUE sheet splits, else loose_match's. Read
 * again when their file changed, or when tracks did (an entry may now be
 * found, or gone); otherwise kept as they are, with nothing written. Each
 * one read is written in its batch, whole or not at all: a step that failed
 * while it was matched or written (pl_step) leaves it as it was. Returns how
 * many were left so: the scan is then not finished, and the next one reads
 * them again. */
static int scan_playlists(library_t *lib, pathvec_t *plv, int tracks_changed, batch_t *b, int64_t pl_before)
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
    int ok = exact && parts && known && upl && pid && clear && add && count, left = !ok;

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
        if (m3u_read(pe->p, &m)) {
            /* There, and not read just now (the card): as it was, and read
             * at the next scan. (Dropped with the lists that are gone, it
             * came back at the next scan as another playlist, under a new
             * number.) One that is read and refused (too long, not a list)
             * does go. */
            if (file_unreadable(pe->p)) {
                pe->kept = 1;
                left++;
            }
            continue;
        }
        /* each entry's track (> 0), -1 for a split file's tracks, 0 not found */
        int64_t *match = calloc((size_t)m.n + 1, sizeof *match);
        int *ent = malloc(LOOSE_CHUNK * sizeof *ent);
        int nl = 0, bad = !match || !ent;       /* (bad: not matched or not written, and left as it was) */
        for (int j = 0; !bad && j < m.n; j++) {
            sqlite3_reset(exact);
            sqlite3_bind_text(exact, 1, m.paths[j], -1, SQLITE_STATIC);
            match[j] = pl_step(exact, &bad) == SQLITE_ROW ? sqlite3_column_int64(exact, 0) : 0;
            sqlite3_reset(exact);
            if (!match[j]) {
                sqlite3_reset(parts);
                sqlite3_bind_text(parts, 1, m.paths[j], -1, SQLITE_STATIC);
                if (pl_step(parts, &bad) == SQLITE_ROW) match[j] = -1;
                sqlite3_reset(parts);
            }
            if (!match[j]) ent[nl++] = j;
            if (nl == LOOSE_CHUNK || (nl && j == m.n - 1)) {
                if (loose_match(db, &m, ent, nl, match)) bad = 1;
                nl = 0;
            }
        }
        char sort[256];
        playlist_sort_key(pe->p, sort, sizeof sort);
        /* (from a step that failed on, none: SQLite may have ended the batch
         * itself by then, and each further write would stay) */
        if (!bad) {
            batch_write(b);
            bad = !b->open;                     /* (no batch: nothing to take back) */
        }
        if (!bad) {
            sqlite3_reset(upl);
            sqlite3_bind_text(upl, 1, pe->p, -1, SQLITE_STATIC);
            sqlite3_bind_text(upl, 2, m.name, -1, SQLITE_STATIC);
            sqlite3_bind_text(upl, 3, sort, -1, SQLITE_STATIC);
            size_t dl = strlen(lib->pl_dir);
            sqlite3_bind_int(upl, 4, dl && !strncmp(pe->p, lib->pl_dir, dl) && pe->p[dl] == '/');
            sqlite3_bind_int64(upl, 5, pe->mtime);
            sqlite3_bind_int64(upl, 6, pe->size);
            pl_step(upl, &bad);
            sqlite3_reset(upl);
        }
        int64_t id = 0;
        if (!bad) {
            sqlite3_reset(pid);
            sqlite3_bind_text(pid, 1, pe->p, -1, SQLITE_STATIC);
            if (pl_step(pid, &bad) == SQLITE_ROW) id = sqlite3_column_int64(pid, 0);
            sqlite3_reset(pid);
        }
        if (id && !bad) {
            pe->kept = 1;
            sqlite3_reset(clear);
            sqlite3_bind_int64(clear, 1, id);
            pl_step(clear, &bad);
            int found = 0, missing = 0;
            for (int j = 0; !bad && j < m.n; j++) {
                if (match[j] < 0) {             /* a split file: its tracks */
                    sqlite3_reset(parts);
                    sqlite3_bind_text(parts, 1, m.paths[j], -1, SQLITE_STATIC);
                    while (!bad && pl_step(parts, &bad) == SQLITE_ROW) {
                        sqlite3_reset(add);
                        sqlite3_bind_int64(add, 1, id);
                        sqlite3_bind_int(add, 2, found++);
                        sqlite3_bind_int64(add, 3, sqlite3_column_int64(parts, 0));
                        pl_step(add, &bad);
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
                pl_step(add, &bad);
            }
            sqlite3_reset(add);
            if (missing && !bad) fprintf(stderr, "playlist %s: %d of %d entries not found\n", m.name, missing, m.n);
            if (!bad) {
                sqlite3_reset(count);
                sqlite3_bind_int(count, 1, found);
                sqlite3_bind_int(count, 2, missing);
                sqlite3_bind_int64(count, 3, id);
                pl_step(count, &bad);
                sqlite3_reset(count);
            }
        }
        if (bad) batch_undo(b);
        if (batch_end(b)) bad = 1;              /* (before the next is read) */
        if (bad) {                              /* its row as it was, and not gone (below) */
            fprintf(stderr, "playlist %s: couldn't be matched or written: as it was, again next scan\n", m.name);
            pe->kept = 1;
            left++;
        }
        free(match);
        free(ent);
        m3u_free(&m);
    }
    sqlite3_finalize(exact); sqlite3_finalize(parts); sqlite3_finalize(known); sqlite3_finalize(upl); sqlite3_finalize(pid);
    sqlite3_finalize(clear); sqlite3_finalize(add); sqlite3_finalize(count);
    if (!ok || plv->failed || lib->cancel) return left;   /* (stopped: not all of them looked at) */
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
        int64_t *ngone = vec_room(gone, ng, &cap, 16, sizeof *gone);
        if (!ngone) break;
        gone = ngone;
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
    return left;
}

/* ---------- CUE sheets ---------- */

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
    const char *e = path_ext(path);
    return !strcasecmp(e, "flac") || !strcasecmp(e, "wv") || !strcasecmp(e, "ape");
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

/* The tracks table's upserts (a file's: scan_once; a sheet's track: cue_stmts_prep;
 * a first scan's: direct_open) start alike: the columns bind_tags fills (?1 to
 * ?23) with seen and tags_v, their values, and what a row there already takes
 * from the file again. */
#define TRACK_COLS "path, mtime, size, title, artist, album_artist, album, genre, year, disc, track," \
                   " codec, rate, bits, channels, duration_ms, has_pic, seen," \
                   " rg_state, rg_track, rg_track_peak, rg_album, rg_album_peak, composer, tags_v"
#define TRACK_VALS "?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,1,?,?,?,?,?,?,2"
#define TRACK_SET_TAGS "mtime=excluded.mtime, size=excluded.size, title=excluded.title," \
                       " artist=excluded.artist, album_artist=excluded.album_artist, album=excluded.album," \
                       " genre=excluded.genre, year=excluded.year, disc=excluded.disc, track=excluded.track," \
                       " codec=excluded.codec, rate=excluded.rate, bits=excluded.bits, channels=excluded.channels," \
                       " duration_ms=excluded.duration_ms, has_pic=excluded.has_pic"

/* The scan's statements for CUE sheets. */
typedef struct {
    sqlite3_stmt *known, *split, *put_sheet, *plain;   /* the .cue files */
    sqlite3_stmt *look, *parts;                /* a split music file, unchanged */
    sqlite3_stmt *put_file, *up, *id;          /* ...or written again */
    idset_t *seen;                             /* the tracks found (library_scan) */
    int retry;                                 /* files that couldn't be read are read again (library_scan_retry_unread):
                                                * 1 the damaged ones, 2 those the reader stopped on as well */
    int db_failed;                             /* a lookup in the library failed (scan_row): nothing is dropped as gone */
    int unread;                                /* sheets that changed and couldn't be read just now: as they were, tried again */
} cue_stmts_t;

/* A step of one of the scan's lookups: 1 a row, 0 none. A step that failed
 * (a read the card didn't answer, no memory, busy) is neither, and says so:
 * taken for "not in the library", the song wasn't ticked off as found, and
 * the scan's end dropped its row as gone, with its plays, its favourite and
 * its place in playlists. A scan with any such failure drops nothing. */
static int g_test_lookups_fail;
void library_test_scan_lookups_fail(int on) { g_test_lookups_fail = on; }

static int scan_row(sqlite3_stmt *s, int *failed)
{
    int rc = g_test_lookups_fail ? SQLITE_IOERR : sqlite3_step(s);
    if (rc != SQLITE_ROW && rc != SQLITE_DONE) *failed = 1;
    return rc == SQLITE_ROW;
}

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
        "INSERT INTO tracks(" TRACK_COLS ", cue, sort)"
        " VALUES(" TRACK_VALS ",?,songkey(?4))"
        " ON CONFLICT(path) DO UPDATE SET " TRACK_SET_TAGS ", seen=1,"
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
        int there = scan_row(c->known, &c->db_failed);
        if (there) had = sqlite3_column_int(c->known, 2);
        same = there && sqlite3_column_int64(c->known, 0) == e->mtime && sqlite3_column_int64(c->known, 1) == e->size;
        sqlite3_reset(c->known);
        /* A sheet that changed and can't be read just now (the card not
         * answering, no memory for it): as it was at the last scan, and read
         * at the next. (It claimed nothing and was written down as read, with
         * no files: its album's songs were dropped, the file became one long
         * track, and the sheet wasn't read again until something touched it.) */
        int read_ok = !same && sheet_buf(sheet) && cue_read(e->p, *sheet) > 0;
        /* (an empty file is read, and says nothing: not this) */
        int unread = !same && !read_ok && e->size > 0 && (!*sheet || !readable(e->p));
        if (unread) c->unread++;
        if (unread && there) same = 1;
        if (same) {
            want = had;
            sqlite3_reset(c->split);
            sqlite3_bind_text(c->split, 1, e->p, -1, SQLITE_STATIC);
            while (scan_row(c->split, &c->db_failed)) {
                int j = pv_find(pv, (const char *)sqlite3_column_text(c->split, 0));
                if (j >= 0 && pv->v[j].cue < 0) { pv->v[j].cue = k; got++; }
            }
            sqlite3_reset(c->split);
            read_ok = got < want && sheet_buf(sheet) && cue_read(e->p, *sheet) > 0;
        }
        if (read_ok) {
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
                    int plain = scan_row(c->plain, &c->db_failed);
                    sqlite3_reset(c->plain);
                    if (plain) continue;
                }
                pv->v[j].cue = k;
            }
        }
        e->cue = !same;
        e->want = want;
        e->put = (!same || want != had) && !(unread && !there);   /* (a new one not read: not noted as read) */
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
    if (scan_row(c->id, &c->db_failed)) idset_add(c->seen, sqlite3_column_int64(c->id, 0));
    sqlite3_reset(c->id);
}

/* File f's row in `cues` stays, and the tracks it is split into are found. */
static void cue_keep(cue_stmts_t *c, pent_t *f)
{
    f->kept = 1;
    sqlite3_reset(c->parts);
    sqlite3_bind_text(c->parts, 1, f->p, -1, SQLITE_STATIC);
    while (scan_row(c->parts, &c->db_failed)) idset_add(c->seen, sqlite3_column_int64(c->parts, 0));
    sqlite3_reset(c->parts);
}

/* A file with a row of its own in `cues` that couldn't be read just now for
 * want of memory or of the card: it stays as it was. One set aside as
 * unreadable (kind 3) stays set aside. (Tried again because a Rescan asked,
 * it had its row dropped as "no longer true": gone from Files not read, and
 * read again at every scan.) One split into its sheet's tracks keeps them:
 * they are found. (None was, and a changed album that couldn't be read at
 * that moment lost every track, with their plays; the next scan made them
 * again as new songs.) */
static void cue_keep_unread(cue_stmts_t *c, pent_t *f)
{
    sqlite3_reset(c->look);
    sqlite3_bind_text(c->look, 1, f->p, -1, SQLITE_STATIC);
    int there = scan_row(c->look, &c->db_failed);
    sqlite3_reset(c->look);
    if (!there) return;
    cue_keep(c, f);
}

/* A split music file (or one that can't be read: kind 3) as it was at the
 * last scan (`kind` and `sheet` as then): its tracks and its row are kept,
 * nothing written. 1 if so. */
static int cue_unchanged(cue_stmts_t *c, pent_t *f, int kind, const char *sheet)
{
    sqlite3_reset(c->look);
    sqlite3_bind_text(c->look, 1, f->p, -1, SQLITE_STATIC);
    int same = scan_row(c->look, &c->db_failed) && sqlite3_column_int64(c->look, 0) == f->mtime &&
               sqlite3_column_int64(c->look, 1) == f->size && sqlite3_column_int(c->look, 2) == kind;
    if (same) {
        const char *was = (const char *)sqlite3_column_text(c->look, 3);
        same = kind == 3 ? 1 : sheet ? was && !strcmp(was, sheet) : was == NULL;   /* (kind 3: `sheet` is why) */
        if (kind == 3 && sqlite3_column_int(c->look, 4) < TAGS_READER) same = 0;   /* (a newer reader may manage) */
        if (kind == 3 && c->retry && (c->retry == 2 || !was)) same = 0;           /* (asked to try again) */
    }
    sqlite3_reset(c->look);
    if (!same) return 0;
    cue_keep(c, f);
    return 1;
}

/* File f's row in `cues` written (kind 1 or 2: split into tracks, `sheet` the
 * .cue or NULL; kind 3: not readable, `sheet` why): it stays. */
static void cue_put_file(cue_stmts_t *c, pent_t *f, int kind, const char *sheet, int nfiles)
{
    sqlite3_reset(c->put_file);
    sqlite3_bind_text(c->put_file, 1, f->p, -1, SQLITE_STATIC);
    sqlite3_bind_int64(c->put_file, 2, f->mtime);
    sqlite3_bind_int64(c->put_file, 3, f->size);
    sqlite3_bind_int(c->put_file, 4, kind);
    if (sheet) sqlite3_bind_text(c->put_file, 5, sheet, -1, SQLITE_STATIC);
    else sqlite3_bind_null(c->put_file, 5);
    sqlite3_bind_int(c->put_file, 6, nfiles);
    sqlite3_step(c->put_file);
    sqlite3_reset(c->put_file);
    f->kept = 1;
}

/* A music file whose tags can't be read (a damaged or half-copied file):
 * noted, so it's not read again while it's the same, and the fast walk's
 * list still matches the library (that never matched again: one such file
 * meant the slow walk at every scan). `why`: NULL, or UNREAD_STOPPED for one
 * the player stopped on (kept in the row's `sheet`). */
#define UNREAD_STOPPED "stopped"

static void cue_unreadable(cue_stmts_t *c, pent_t *f, const char *why)
{
    if (!why) fprintf(stderr, "library: can't read %s\n", f->p);
    cue_put_file(c, f, 3, why, TAGS_READER);
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
    cue_put_file(c, f, kind, sheet, 0);
    return n;
}

/* A music file split by the sheet in its own tags: its tracks (> 0), or -1. */
static int cue_embedded(cue_stmts_t *c, pent_t *f, const tags_t *t, cue_sheet_t **sheet)
{
    char *text;
    size_t len;
    if (!sheet_buf(sheet) || rd_sheet(f->p, &text, &len)) return -1;
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
            return rd_tags(pv->v[i].p, &t, NULL) ? -1 : cue_write(c, &pv->v[i], s, fi, &t, 1, e->p);
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

/* The files a scan reads tags from, read before their batch begins (no lock
 * held while reading the card) and written together. */
#define PEND_MAX 32
#define FIRST_BATCH_S 2.0

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
    idset_t wrote;         /* tracks this scan wrote (the update hook): the groups follow only them */
    int dropped;           /* tracks it removed itself (a CUE sheet's): then all the groups */
    int retry;             /* files it couldn't read for now (the card's reads failing, no memory): again next scan */
    /* a first scan: each song's groups as it's added (direct_track) */
    int direct;            /* 1: so far; -1: given up (the groups all made afterwards, as before) */
    int d_n;               /* songs grouped so */
    grp_t d_g;
    sqlite3_stmt *d_row, *d_ins, *d_genre, *d_gget, *d_comp, *d_cget, *d_fup, *d_fget;
    const char *d_last;    /* the last one's path (they must come in path order) */
    char d_lastbuf[2048];
    int d_free;            /* each file's name freed once its song is in (no sheet will look for it) */
    char d_root[1024], d_dir[1024], d_gname[256], d_cname[256];
    int64_t d_fid, d_gid, d_cid, d_next;   /* the last folder, genre and composer; the next song's id */
} scan_t;

/* ---- a first scan groups each song as it's added ----
 * The groups used to follow the songs: every track read again in path order,
 * on copies of the tables (so nothing else waited), then each one's album,
 * artist, genre, composer, folder and search text written back: every row of
 * the tracks table rewritten several times. Measured on the H2, 50,000 songs:
 * 290 s, a third of the scan, and under 1 MB of memory left. A first scan
 * starts from nothing, with nobody reading, and adds the songs in path order
 * (the order the groups depend on: an album artist's first spelling, an
 * album's first track). So each song goes through the same grp_track() just
 * before its row is added, and the row goes in once, with its album, artist,
 * genre, composer, folder and search text; afterwards only the indexes and
 * the counts are left (groups_counts).
 * As rebuild_groups() would leave it (test_scan checks: a full rebuild after
 * it changes nothing). Given up, and the groups made the old way, if a song
 * comes out of order or the tracks table ends with rows that didn't come
 * through here (a file split by a CUE sheet). */
static int g_grp_direct = 1;

void library_test_groups_direct(int on) { g_grp_direct = on; }

static void direct_close(scan_t *sc)
{
    if (!sc->d_row && !sc->d_ins) return;
    grp_close(&sc->d_g);
    sqlite3_stmt **all[] = {&sc->d_row, &sc->d_ins, &sc->d_genre, &sc->d_gget, &sc->d_comp, &sc->d_cget, &sc->d_fup, &sc->d_fget};
    for (size_t i = 0; i < sizeof all / sizeof *all; i++) {
        sqlite3_finalize(*all[i]);
        *all[i] = NULL;
    }
}

static void direct_open(scan_t *sc, const pathvec_t *pv)
{
    sqlite3 *db = sc->db;
    if (!g_grp_direct || pv->n < 1 || grp_open(&sc->d_g, db)) return;
    sc->d_g.no_ut = 1;
    /* (bound as the row's INSERT is, by bind_tags: grp_track's columns of a
     * song that isn't in the table yet; ?24 the id it will have) */
    sc->d_row = prep(db, "SELECT ?24, ?5, ?6, ?7, ?9, ?1, ?4, NULL, NULL");
    /* (as scan_once's INSERT, with what rebuild_groups, rebuild_composers,
     * rebuild_folders and index_search would write afterwards) */
    sc->d_ins = prep(db,
        "INSERT INTO tracks(" TRACK_COLS ", sort,"
        " id, album_id, artist_id, genre_id, composer_id, folder_id, search)"
        " VALUES(" TRACK_VALS ",songkey(?4),"
        " ?24, ?25, ?26, ?27, ?28, ?29,"
        " " SKEY_TRACK("?4", "?5", "?6", "?7") ")");
    sc->d_genre = prep(db, NAMED_PUT_SQL("genres"));
    sc->d_gget = prep(db, NAMED_GET_SQL("genres"));
    sc->d_comp = prep(db, NAMED_PUT_SQL("composers"));
    sc->d_cget = prep(db, NAMED_GET_SQL("composers"));
    sc->d_fup = prep(db, FOLDER_PUT_SQL);
    sc->d_fget = prep(db, FOLDER_GET_SQL);
    sc->d_next = db_int(db, "SELECT COALESCE(MAX(id), 0) + 1 FROM tracks", 0, 0);
    if (!sc->d_row || !sc->d_ins || !sc->d_genre || !sc->d_gget || !sc->d_comp || !sc->d_cget || !sc->d_fup ||
        !sc->d_fget || sc->d_next != 1) {
        direct_close(sc);
        return;
    }
    /* the folder every file is under (as folders_root: the first and last
     * paths' common part; a file that turns out unreadable can move it, and
     * rebuild_folders, run afterwards, puts that right) */
    common_dir(pv->v[0].p, pv->v[pv->n - 1].p, sc->d_root, sizeof sc->d_root);
    sc->d_dir[0] = '\0';
    sc->d_gname[0] = sc->d_cname[0] = '\1';       /* (no name yet: the first one is looked up) */
    sc->d_gname[1] = sc->d_cname[1] = '\0';
    sc->d_fid = sc->d_gid = sc->d_cid = 0;
    sc->d_last = NULL;
    sc->d_n = 0;
    sc->direct = 1;
}

/* name's row in genres or composers (made if new): its id, 0 for none */
static int64_t direct_named(sqlite3_stmt *put, sqlite3_stmt *get, const char *name)
{
    sqlite3_reset(put);
    sqlite3_bind_text(put, 1, name, -1, SQLITE_STATIC);
    sqlite3_step(put);
    sqlite3_reset(put);
    sqlite3_reset(get);
    sqlite3_bind_text(get, 1, name, -1, SQLITE_STATIC);
    int64_t id = sqlite3_step(get) == SQLITE_ROW ? sqlite3_column_int64(get, 0) : 0;
    sqlite3_reset(get);
    return id;
}

/* direct_named, the last name asked remembered (in `last`, cap bytes: one too
 * long for it is looked up each time) */
static void direct_cached(sqlite3_stmt *put, sqlite3_stmt *get, const char *name, char *last, size_t cap, int64_t *id)
{
    if (!strcmp(name, last)) return;
    *id = direct_named(put, get, name);
    snprintf(last, cap, "%s", name);
    if (strcmp(name, last)) last[0] = '\1';
}

/* The song of file f, tags t: added with its groups (in the batch that's
 * open). 0, or -1: not added, and no more this way (sc->direct is -1). */
static int direct_track(scan_t *sc, const pent_t *f, const tags_t *t)
{
    const char *path = f->p;
    if (sc->d_last && strcmp(path, sc->d_last) <= 0) { sc->direct = -1; return -1; }
    int64_t id = sc->d_next;
    /* its album and artists (their rows made or named; who's credited on it) */
    sqlite3_reset(sc->d_row);
    bind_tags(sc->d_row, path, f, t);
    sqlite3_bind_int64(sc->d_row, 24, id);
    if (sqlite3_step(sc->d_row) != SQLITE_ROW) { sqlite3_reset(sc->d_row); sc->direct = -1; return -1; }
    grp_track(&sc->d_g, sc->d_row);
    sqlite3_reset(sc->d_row);
    direct_cached(sc->d_genre, sc->d_gget, t->genre, sc->d_gname, sizeof sc->d_gname, &sc->d_gid);
    direct_cached(sc->d_comp, sc->d_cget, t->composer, sc->d_cname, sizeof sc->d_cname, &sc->d_cid);
    char dir[1024];
    dir_of(path, dir, sizeof dir);
    if (!sc->d_fid || strcmp(dir, sc->d_dir)) {
        sc->d_fid = folder_under(sc->db, sc->d_fup, sc->d_fget, dir, sc->d_root);
        snprintf(sc->d_dir, sizeof sc->d_dir, "%s", dir);
    }
    sqlite3_stmt *in = sc->d_ins;
    sqlite3_reset(in);
    bind_tags(in, path, f, t);
    sqlite3_bind_int64(in, 24, id);
    sqlite3_bind_int64(in, 25, sc->d_g.out_album);
    sqlite3_bind_int64(in, 26, sc->d_g.out_artist);
    if (sc->d_gid) sqlite3_bind_int64(in, 27, sc->d_gid); else sqlite3_bind_null(in, 27);
    if (sc->d_cid) sqlite3_bind_int64(in, 28, sc->d_cid); else sqlite3_bind_null(in, 28);
    if (sc->d_fid) sqlite3_bind_int64(in, 29, sc->d_fid); else sqlite3_bind_null(in, 29);
    int rc = sqlite3_step(in);
    sqlite3_reset(in);
    if (rc != SQLITE_DONE) { sc->direct = -1; return -1; }       /* (its path or id taken: the usual way, and the groups afterwards) */
    snprintf(sc->d_lastbuf, sizeof sc->d_lastbuf, "%s", path);   /* (a copy: the name itself may be freed) */
    sc->d_last = sc->d_lastbuf;
    sc->d_next = id + 1;
    sc->d_n++;
    return 0;
}

/* (sqlite3_update_hook, while the scan writes tracks) */
static void scan_wrote(void *ctx, int op, const char *dbn, const char *table, sqlite3_int64 rowid)
{
    scan_t *sc = ctx;
    if (strcmp(dbn, "main") || strcmp(table, "tracks")) return;
    if (op == SQLITE_DELETE) sc->dropped++;
    idset_add(&sc->wrote, rowid);
}

/* About to change tracks (level 2) or only composers (1): noted first, in
 * the same batch, for a scan that doesn't get to the end. */
static void scan_dirty(scan_t *sc, int level)
{
    batch_write(&sc->b);
    if (level > sc->marked && level > sc->pending) meta_set(sc->db, "scan_pending", level);
    if (level > sc->marked) sc->marked = level;
}

/* A file the player stopped on is already set aside in `path`'s folder: 1 */
static int stopped_beside(sqlite3 *db, const char *path)
{
    const char *sl = strrchr(path, '/');
    if (!sl) return 0;
    size_t n = (size_t)(sl - path) + 1;
    sqlite3_stmt *q = prep(db, "SELECT path FROM cues WHERE kind=3 AND sheet='" UNREAD_STOPPED "'");
    int r = 0;
    while (q && !r && sqlite3_step(q) == SQLITE_ROW) {
        const char *p = (const char *)sqlite3_column_text(q, 0);
        r = p && !strncmp(p, path, n) && !strchr(p + n, '/');
    }
    sqlite3_finalize(q);
    return r;
}

/* Track `id`, as it was: the tags read later than its row (its composer), noted. */
static void scan_later(scan_t *sc, int64_t id, const char *composer)
{
    sqlite3_reset(sc->later);
    sqlite3_bind_int64(sc->later, 1, id);
    sqlite3_bind_text(sc->later, 2, composer, -1, SQLITE_TRANSIENT);
    sqlite3_step(sc->later);
    sqlite3_reset(sc->later);
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
            cue_unreadable(&sc->cs, f, f->skip ? UNREAD_STOPPED : NULL);
        } else if (e->kind == PEND_LATER) {
            scan_later(sc, e->id, e->t.composer);
        } else {
            int added = sc->direct == 1 && direct_track(sc, f, &e->t) == 0;
            if (!added) {
                sqlite3_reset(sc->up);
                bind_tags(sc->up, f->p, f, &e->t);
                sqlite3_step(sc->up);
                sqlite3_reset(sc->up);
            }
            cue_found(&sc->cs, f->p);
            /* A first scan's names go as its songs come in: 50,000 of them
             * are 5 MB, and the scan was at its tightest (under 2 MB
             * available on the H2) two thirds of the way through, with all
             * of them still held. (scan_once drops the emptied entries;
             * pa_drop hands the memory back a block at a time.) */
            if (added && sc->d_free) pa_drop(pv, f);
        }
    }
    sc->npend = 0;
    /* a first scan (nobody else is waiting to write): a commit every couple
     * of seconds, not every 32 songs. Each one writes again the pages every
     * song touches (albums, artists, the names' index): measured on the H2,
     * 50,000 songs, over a minute of the scan. */
    if (sc->direct == 1 && clock_fsec() - sc->b.since < FIRST_BATCH_S) return;
    batch_end(&sc->b);
}

/* A slot for file i's tags (the queue written first when full, or once it
 * has waited BATCH_S). */
static pend_t *scan_queue(scan_t *sc, pathvec_t *pv, int i, int kind)
{
    if (sc->npend == PEND_MAX || (sc->npend && clock_fsec() - sc->pend_since >= BATCH_S)) scan_flush(sc, pv);
    if (!sc->npend) sc->pend_since = clock_fsec();
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

/* A scan's statements and what it noted, freed (its batch is ended by then) */
static void scan_state_free(scan_t *sc)
{
    sqlite3_finalize(sc->later);
    sqlite3_finalize(sc->up);
    cue_stmts_free(&sc->cs);
    free(sc->pend);
    free(sc->seen.bits);
    free(sc->wrote.bits);
}

/* ---- a first scan loads in bulk ----
 * Every song inserted moves an entry in each of the tracks table's indexes
 * (17 by schema 8, for the lists to open at once); with a cache of 512 KB and
 * tens of thousands of songs each is a page read back from the card and
 * written out again. Measured on the H2, 50,000 songs: the scan's reading
 * step took over twice as long as before those indexes. So a scan that
 * starts from an empty library sets them aside (their definitions kept in
 * the table bulk_idx, in the same transaction as dropping them), inserts and
 * groups the songs, and builds them once at the end, each from one sorted
 * pass. Cut short (the H2 switched off mid-scan): library_open finds
 * bulk_idx and builds them before anything else (bulk_end). */
#define BULK_MIN_FILES 1000
static int g_bulk_min = BULK_MIN_FILES;

void library_test_bulk_min(int files) { g_bulk_min = files > 0 ? files : BULK_MIN_FILES; }

static void bulk_begin(library_t *lib)
{
    sqlite3 *db = lib->db;
    if (sqlite3_exec(db, "BEGIN IMMEDIATE;"
                         "CREATE TABLE IF NOT EXISTS bulk_idx(name TEXT, sql TEXT);"
                         "INSERT INTO bulk_idx SELECT name, sql FROM sqlite_master"
                         " WHERE type='index' AND tbl_name='tracks' AND sql IS NOT NULL;", NULL, NULL, NULL) != SQLITE_OK) {
        fprintf(stderr, "library: indexes not set aside: %s\n", sqlite3_errmsg(db));
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        return;
    }
    sqlite3_stmt *q = prep(db, "SELECT name FROM bulk_idx");
    char *drops = NULL;
    while (q && sqlite3_step(q) == SQLITE_ROW) {
        char *more = sqlite3_mprintf("%z DROP INDEX IF EXISTS \"%w\";", drops, (const char *)sqlite3_column_text(q, 0));
        drops = more;
    }
    sqlite3_finalize(q);
    int ok = drops && sqlite3_exec(db, drops, NULL, NULL, NULL) == SQLITE_OK &&
             sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) == SQLITE_OK;
    sqlite3_free(drops);
    if (!ok) {
        fprintf(stderr, "library: indexes not set aside: %s\n", sqlite3_errmsg(db));
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        return;
    }
    lib->bulk = 1;
    fprintf(stderr, "library: a first scan: the tracks' indexes set aside until the songs are in\n");
}

/* The indexes bulk_begin set aside, built (each said to the upgrade's
 * progress, if one is watching). 0 if none were waiting. */
static int bulk_end(library_t *lib)
{
    sqlite3 *db = lib->db;
    sqlite3_stmt *q = NULL;
    if (sqlite3_prepare_v2(db, "SELECT sql FROM bulk_idx", -1, &q, NULL) != SQLITE_OK) { lib->bulk = 0; return 0; }   /* (no such table: none) */
    char **sql = NULL;
    int n = 0, cap = 0, rc, short_of = 0;
    while ((rc = sqlite3_step(q)) == SQLITE_ROW) {
        char **grown = vec_room(sql, n, &cap, 32, sizeof *sql);
        if (!grown) { short_of = 1; break; }
        sql = grown;
        const char *t = (const char *)sqlite3_column_text(q, 0);
        if (t && (sql[n] = strdup(t))) n++;
        else short_of = 1;
    }
    if (rc != SQLITE_ROW && rc != SQLITE_DONE) short_of = 1;
    sqlite3_finalize(q);
    double t0 = clock_fsec();
    g_up_done = 0;
    g_up_total = n;
    for (int i = 0; i < n; i++) {
        char *err = NULL;
        if (sqlite3_exec(db, sql[i], NULL, NULL, &err) != SQLITE_OK && !(err && strstr(err, "already exists"))) {
            fprintf(stderr, "library: %s\n  in: %s\n", err ? err : "?", sql[i]);
            short_of = 1;
        }
        sqlite3_free(err);
        free(sql[i]);
        up_tick();
    }
    free(sql);
    /* The list of what is still to build goes only once all of it is built.
     * (It went whatever had happened: a list read short, or an index that
     * couldn't be made on a card that filled during the first scan, and
     * the songs stayed without those indexes, every list reading the whole
     * table, with nothing left to say which were missing.) Kept, the next
     * open builds what's missing: those made already say so and are passed. */
    if (short_of) fprintf(stderr, "library: not every index could be built: the rest at the next start\n");
    else sqlite3_exec(db, "DROP TABLE IF EXISTS bulk_idx", NULL, NULL, NULL);
    lib->bulk = 0;
    if (n) fprintf(stderr, "library: %d indexes built in %.1f s\n", n, clock_fsec() - t0);
    return n;
}

void library_scan_defer_art(library_t *lib, int on) { lib->defer_art = !!on; }
void library_scan_retry_unread(library_t *lib, int on) { lib->retry_unread = !!on; }

static int scan_once(library_t *lib, const char *root, lib_progress_cb cb, void *ctx, int allow_fast);

int library_scan(library_t *lib, const char *root, lib_progress_cb cb, void *ctx)
{
    lib->fw_bad = 0;
    int n = scan_once(lib, root, cb, ctx, 1);
    if (lib->fw_bad && !lib->cancel) {             /* the FAT reading named a file that isn't there */
        fprintf(stderr, "library: fast walk: a new file it listed isn't there: walking the folder\n");
        lib->fw_bad = 0;
        n = scan_once(lib, root, cb, ctx, 0);
    }
    t_look.cb = NULL;                              /* (however the listing ended: nothing of this scan's is called later) */
    grp_unwatch(lib->db);
    lib->retry_unread = 0;                         /* (asked of one scan) */
    /* (stopped before its indexes were built, and carrying on: now. On the
     * way out, library_open does it at the next start.) */
    if (lib->bulk && !lib->cancel) bulk_end(lib);
    lib->defer_art = 0;
    reader_close();                                /* (the helper's memory back until the next scan) */
    return n;
}

static int scan_once(library_t *lib, const char *root, lib_progress_cb cb, void *ctx, int allow_fast)
{
    double t0 = clock_fsec(), t_walk, t_files, t_groups, t_pl, t_art, t_tags = 0, tt;
    pathvec_t pv = {0}, plv = {0}, cv = {0}, iv = {0}, none = {0}, nocue = {0};
    /* No music folder (renamed, or the card's away): every track would look
     * deleted, with its play counts. The library stays as it is instead. */
    struct stat rst;
    if (stat(root, &rst) != 0 || !S_ISDIR(rst.st_mode)) return -1;
    lib->cancel = 0;
    art_migrate(lib);                              /* (covers from before the store, once) */
    /* playlists made from here on are kept */
    int64_t pl_before = db_int(lib->db, "SELECT COALESCE(MAX(id), 0) FROM playlists", 0, INT64_MAX);
    lib->groups_how = 0;
    t_look.cb = cb;                                /* (the listing's own count: look_tell) */
    t_look.ctx = ctx;
    t_look.found = t_look.checked = 0;
    t_look.at = clock_fsec();
    /* the library's files as they are aren't held by name (known_build) */
    known_t *known = known_build(lib->db);
    pv.known = known;
    int fast = list_music(lib, root, allow_fast, &pv, &plv, &cv, &iv);
    if (known && cv.n && !lib->cancel) {           /* a sheet where there was none: every file by name, as before */
        lists_free(&pv, &plv, &cv, &iv);
        known_free(known);
        known = NULL;
        fast = list_music(lib, root, allow_fast, &pv, &plv, &cv, &iv);
    }
    lib->quick = fast;
    lib->scan_held = pv.n;
    if (lib->pl_dir[0]) walk(lib->pl_dir, &none, &plv, &nocue, NULL, 0, &lib->cancel);   /* your own playlists (no music in there) */
    plv.failed |= pv.failed | cv.failed | none.failed;   /* (playlists: none dropped then) */
    pv_free(&none);
    pv_free(&nocue);
    if (lib->cancel) {
        lists_free(&pv, &plv, &cv, &iv);
        known_free(known);
        return -1;
    }
    pv.known = known;                              /* (pv_free forgets it) */
    qsort(iv.v, (size_t)iv.n, sizeof *iv.v, cmp_pent);
    qsort(pv.v, (size_t)pv.n, sizeof *pv.v, cmp_pent);
    qsort(cv.v, (size_t)cv.n, sizeof *cv.v, cmp_pent);
    /* The whole listing as the last finished scan saw it, and nothing left to
     * do from before (a scan cut short, tags or covers a newer version reads):
     * the library is as it should be. Nothing more is read (with 50,000 songs,
     * 15 s of looking every file up and every album's cover over). */
    const pathvec_t *const lists[4] = {&pv, &cv, &iv, &plv};
    uint64_t sig = pv.failed || cv.failed || iv.failed || plv.failed ? 0 : listing_sig(lists, 4);
    /* A scan asked to try the unread files again (library_scan_retry_unread:
     * Rescan): those that are simply damaged, and, when a helper does the
     * reading (reader.h), those a reader stopped on as well: one that dies
     * again costs a helper, where it used to cost the player. With any to
     * try, the listing being as it was settles nothing. */
    int retry_unread = !lib->retry_unread ? 0 : reader_on() ? 2 : 1;
    if (retry_unread && !get_int_col(lib, retry_unread == 2 ? "SELECT EXISTS(SELECT 1 FROM cues WHERE kind=3)"
                                                            : "SELECT EXISTS(SELECT 1 FROM cues WHERE kind=3 AND sheet IS NULL)", 0))
        retry_unread = 0;
    if (retry_unread) fprintf(stderr, "library: reading again the files that couldn't be read\n");
    if (sig && !retry_unread && (int64_t)sig == meta_get64(lib->db, "scan_sig") && !meta_get(lib->db, "scan_pending") &&
        !library_needs_tags(lib) && !library_needs_art(lib) &&
        !get_int_col(lib, "SELECT EXISTS(SELECT 1 FROM albums WHERE art_sig=0)", 0) &&   /* (every cover's source noted) */
        get_int_col(lib, "SELECT COUNT(*) FROM playlists", 0) == plv.n) {   /* (none of the player's left without its file) */
        fprintf(stderr, "library: scan %.1f s: nothing changed (%s%d files)\n", clock_fsec() - t0, fast ? "fast walk, " : "",
                pv.n + (known ? known->n_same : 0));
        known_free(known);
        lists_free(&pv, &plv, &cv, &iv);
        lib->scan_changes = 0;
        int count = (int)db_int(lib->db, "SELECT COUNT(*) FROM tracks", 0, -1);
        if (cb) cb(count, count, "done", ctx);
        return count;
    }
    /* (the fast walk's lists are marked already; with every file ticked off
     * there's none to mark: what's gone is what wasn't found, at the end) */
    if (!fast && (pv.n || cv.n)) against_library(lib, &pv, &cv, 0);
    idset_t seen0 = {0};                           /* the tracks ticked off by the listing: found */
    if (known) {
        seen0 = known->same;
        memset(&known->same, 0, sizeof known->same);
        known_free(known);                         /* (the table's megabyte back before the songs are read) */
        known = NULL;
        pv.known = NULL;
    }
    t_look.cb = NULL;                              /* (listed: the songs' own count from here) */
    int first = 0;                                 /* a first scan, its indexes set aside just now */
    if (!lib->bulk && pv.n >= g_bulk_min && library_empty(lib)) { bulk_begin(lib); first = lib->bulk; }
    t_walk = clock_fsec();

    /* Nothing is written for a file as it was (the usual case: a scan that
     * finds nothing new writes nothing at all); tracks found are noted in
     * memory, and those not found are dropped only at the end, after a
     * whole walk: a scan stopped half way keeps what it has written, and
     * never drops music. */
    sqlite3 *db = lib->db;
    scan_t sc = {.db = db, .b = {.db = db}, .seen = seen0};
    sc.pending = meta_get(db, "scan_pending");
    sc.cs.seen = &sc.seen;
    sc.pend = malloc(PEND_MAX * sizeof *sc.pend);
    sqlite3_stmt *look = prep(db, "SELECT id, mtime, size, tags_v FROM tracks WHERE path=? AND cue=0");
    sc.later = prep(db, "UPDATE tracks SET composer=?2, tags_v=2 WHERE id=?1");
    sc.up = prep(db,
        "INSERT INTO tracks(" TRACK_COLS ", sort)"
        " VALUES(" TRACK_VALS ",songkey(?4))"
        " ON CONFLICT(path) DO UPDATE SET " TRACK_SET_TAGS ","
        " rg_state=excluded.rg_state, rg_track=excluded.rg_track, rg_track_peak=excluded.rg_track_peak,"
        " rg_album=excluded.rg_album, rg_album_peak=excluded.rg_album_peak,"
        " composer=excluded.composer, tags_v=2, sort=excluded.sort, search=NULL");
    cue_sheet_t *sheet = NULL;
    int ok = look && sc.later && sc.up && sc.pend && cue_stmts_prep(db, &sc.cs) == 0;
    sc.cs.retry = retry_unread;
    if (ok && first) direct_open(&sc, &pv);        /* (each song's groups as it's added) */
    sc.d_free = sc.direct == 1 && !cv.n;           /* (with sheets, files are looked for by name to the end) */
    int n_listed = pv.n;
    sqlite3_update_hook(db, scan_wrote, &sc);
    if (ok) cue_claim(&sc.cs, &pv, &cv, &sheet);

    int changed = 0;   /* tracks added, changed or gone */
    int extra = 0;     /* unchanged tracks that got a tag read later (a composer) */
    {   /* The file the player died reading, last time: not read again. It's
         * written down now, in a batch of its own: left to go with the next
         * songs', it was lost when the player died on the very next file (the
         * rest of an album made the same way), which was then the one set
         * aside, and forgotten in its turn: two such files side by side, and
         * the scan never got past them. And after one in a folder, the others
         * there get no second chance: an album of twelve is twelve starts,
         * not twenty-four. */
        char died[2048];
        int times = trying_left("tags", died, sizeof died);
        int j = ok && times ? pv_find(&pv, died) : -1;
        if (j >= 0) {                              /* (in the library as it is: it was read, and something else stopped the player) */
            sqlite3_bind_text(look, 1, died, -1, SQLITE_STATIC);
            if (sqlite3_step(look) == SQLITE_ROW && sqlite3_column_int64(look, 1) == pv.v[j].mtime &&
                sqlite3_column_int64(look, 2) == pv.v[j].size) j = -1;
            sqlite3_reset(look);
        }
        if (j >= 0 && times == 1 && stopped_beside(db, died)) times = 2;
        if (j >= 0 && times == 2) {
            pv.v[j].skip = 1;
            fprintf(stderr, "library: the player stopped while reading %s: set aside as unreadable\n", died);
            batch_write(&sc.b);
            cue_unreadable(&sc.cs, &pv.v[j], UNREAD_STOPPED);
            batch_end(&sc.b);
        }
    }
    int trying = -1;
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
        if (f->st == ST_SAME && f->cue < 0) {  /* as it was (against_library): nothing to look up or write */
            idset_add(&sc.seen, (int64_t)f->tid);
            continue;
        }
        sqlite3_reset(look);
        sqlite3_bind_text(look, 1, path, -1, SQLITE_STATIC);
        int row = scan_row(look, &sc.cs.db_failed);
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
            tt = clock_fsec();
            library_trying("tags", &trying, path);
            int got = !f->skip && rd_tags(path, &e->t, NULL) == 0;
            t_tags += clock_fsec() - tt;
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
                scan_later(&sc, id, t.composer);
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
        tt = clock_fsec();
        library_trying("tags", &trying, path);
        int later = 0;
        int rr = f->skip ? -1 : rd_tags(path, &e->t, &later);
        if (rr == READER_DIED && !later) {         /* (the helper, not the player: the scan carries on) */
            f->skip = 1;
            fprintf(stderr, "library: the reader stopped on %s: set aside as unreadable\n", path);
        }
        if (rr && later) fprintf(stderr, "library: %s not read with little memory to spare: again at the next scan\n", path);
        int unread = rr != 0;
        t_tags += clock_fsec() - tt;
        if (unread) {
            if (!later && (f->skip || readable(path))) { e->kind = PEND_BAD; continue; }
            sc.npend--;                        /* can't be read now (EIO, no memory): next time */
            if (fast && !row && access(path, F_OK) != 0 && errno == ENOENT) {
                lib->fw_bad = 1;               /* (never there: the FAT reading's mistake) */
                ok = 0;
                break;
            }
            sc.retry++;
            idset_add(&sc.seen, id);           /* (a track's row stays as it was) */
            if (!row) cue_keep_unread(&sc.cs, f);
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
    library_trying_done("tags", &trying);
    if (sc.pend) scan_flush(&sc, &pv);         /* (stopped too: what's read is kept) */
    batch_end(&sc.b);
    direct_close(&sc);
    if (sc.d_free) {                           /* the names freed as their songs came in: those entries out of the list */
        int w = 0;
        for (int i = 0; i < pv.n; i++)
            if (pv.v[i].p) pv.v[w++] = pv.v[i];
        pv.n = w;
    }
    sqlite3_update_hook(db, NULL, NULL);
    free(sheet);
    sqlite3_finalize(look);
    if (!ok) {   /* stopped (or the database can't be read): nothing dropped */
        if (sc.marked) fprintf(stderr, "library: scan stopped, %d tracks written, the rest next time\n", changed);
        scan_state_free(&sc);
        lists_free(&pv, &plv, &cv, &iv);
        return -1;
    }

    /* Tracks not found, and `cues` rows no longer true (a whole walk only) */
    int64_t *gone = NULL;
    int ngone = 0, gcap = 0;
    char **cgone = NULL;
    int ncgone = 0, ccap = 0;
    if (sc.cs.db_failed) fprintf(stderr, "library: a lookup failed during the scan: nothing dropped as gone this time\n");
    if (!pv.failed && !cv.failed && !sc.seen.failed && !sc.cs.db_failed) {
        sqlite3_stmt *all = prep(db, "SELECT id FROM tracks");
        while (all && sqlite3_step(all) == SQLITE_ROW) {
            int64_t id = sqlite3_column_int64(all, 0);
            if (idset_has(&sc.seen, id)) continue;
            int64_t *ng = vec_room(gone, ngone, &gcap, 64, sizeof *gone);
            if (!ng) { ngone = -1; break; }
            gone = ng;
            gone[ngone++] = id;
        }
        sqlite3_finalize(all);
        all = prep(db, "SELECT path, kind FROM cues");
        while (ngone >= 0 && all && sqlite3_step(all) == SQLITE_ROW) {
            const char *p = (const char *)sqlite3_column_text(all, 0);
            int j = p ? pv_find(sqlite3_column_int(all, 1) == 0 ? &cv : &pv, p) : -1;
            if (j >= 0 && (sqlite3_column_int(all, 1) == 0 || pv.v[j].kept)) continue;
            char **ng = vec_room(cgone, ncgone, &ccap, 16, sizeof *cgone);
            if (!ng) break;
            cgone = ng;
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
    int groups_ok = 1;                         /* (0: they were needed and couldn't be written) */
    /* the groups only where songs changed: if no earlier scan left them to
     * do, this one removed none itself, there are groups to start from, and
     * not too many changed */
    int nwrote = 0;
    for (size_t k = 0; k < sc.wrote.cap; k++) nwrote += __builtin_popcount(sc.wrote.bits[k]);
    int where_changed = groups && sc.pending < 2 && !sc.dropped && !sc.wrote.failed && !sc.direct &&
                        nwrote + ngone <= GROUPS_INCR_CHANGED && get_int_col(lib, "SELECT EXISTS(SELECT 1 FROM albums)", 0);
    t_files = clock_fsec();
    /* The files' names aren't needed from here (what's gone is listed, the
     * sheets have their own list): with 50,000 songs, 7 MB handed back before
     * the groups and the covers, the steps that most need the room. */
    int n_files = n_listed;
    pv_free(&pv);
    /* (the next step: albums and artists, no count; not after a first scan
     * that grouped each song as it was added: its indexes are next) */
    if (cb && groups && sc.direct != 1) cb(n_files, n_files, "albums", ctx);
    if (ngone || ncgone || sheets || groups || composers) {
        grp_watch(db, cb, ctx);                /* (the step says it is at work: grp_tell) */
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
        if (where_changed) {                   /* which songs, and what they belonged to, before any goes */
            groups_incr_drop(db);
            sqlite3_stmt *ins = shadow_exec(db, "CREATE TEMP TABLE g_c(id INTEGER PRIMARY KEY)") ? NULL
                              : prep(db, "INSERT OR IGNORE INTO temp.g_c VALUES(?)");
            for (size_t k = 0; ins && k < sc.wrote.cap * 8; k++) {
                if (!idset_has(&sc.wrote, (int64_t)k)) continue;
                sqlite3_reset(ins);
                sqlite3_bind_int64(ins, 1, (int64_t)k);
                sqlite3_step(ins);
            }
            for (int k = 0; ins && k < ngone; k++) {
                sqlite3_reset(ins);
                sqlite3_bind_int64(ins, 1, gone[k]);
                sqlite3_step(ins);
            }
            where_changed = ins && groups_note_old(db) == 0;
            sqlite3_finalize(ins);
        }
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
        /* the play log of songs gone: their ids are given to new ones (the
         * highest first), which would show in On repeat, never played */
        if (ngone || sheets)
            sqlite3_exec(db, "DELETE FROM plays WHERE track_id NOT IN (SELECT id FROM tracks)", NULL, NULL, NULL);
        if (!groups && composers) rebuild_composers(db, 0);
        batch_end(&sc.b);
        /* the groups follow (scan_pending says so until then): made aside,
         * the lock held only to put them in; else in one batch, as before.
         * Meanwhile a new track can show in Songs before its album, and a
         * removed album for a moment with no songs. */
        int done = 1;                          /* (1: not done yet) */
        groups_ok = !groups;
        lib->groups_how = groups ? 2 : 0;
        pv_free(&cv);                          /* (the card's list: not needed from here) */
        /* a first scan that grouped every song as it was added: the indexes
         * (the counts read through them), then the counts */
        if (groups && !lib->cancel && sc.direct == 1 && lib->bulk &&
            get_int_col(lib, "SELECT COUNT(*) FROM tracks", 0) == sc.d_n) {
            double tg = clock_fsec(), ti;
            if (cb) cb(n_files, n_files, "indexes", ctx);
            bulk_end(lib);
            ti = clock_fsec();
            batch_write(&sc.b);
            int had = sc.b.open;
            if (had) {
                char under[1024];              /* (the folder every song is under: as the songs' folders took it?) */
                folders_root(db, under, sizeof under);
                if (groups_counts(lib, NULL, strcmp(under, sc.d_root) ? 1 : 2)) {
                    batch_undo(&sc.b);         /* (a pass that stopped: none of it kept; all made now, below) */
                    had = 0;
                } else index_search(lib, 1);   /* (albums and artists: the songs' was set as they were added) */
            }
            if (batch_end(&sc.b) == 0 && had) {
                groups_ok = 1;
                done = 0;
                lib->groups_how = 3;
                fprintf(stderr, "library: groups as the songs were added (%d): indexes %.2f s, counts %.2f s\n",
                        sc.d_n, ti - tg, clock_fsec() - ti);
            }
        } else if (groups && sc.direct) {
            fprintf(stderr, "library: groups: not as the songs were added (%d of them so): all made now\n", sc.d_n);
            if (cb && sc.direct == 1) cb(n_files, n_files, "albums", ctx);
        }
        if (groups && !lib->cancel && where_changed) {
            done = rebuild_groups_where_changed(lib, &sc.b);
            if (done < 0) batch_undo(&sc.b);   /* (stopped part way: none of it kept; all of them, below) */
            if (!done) lib->groups_how = 1;
            double ts = clock_fsec();
            if (!done) index_search(lib, 2);   /* changed tracks (their search cleared), new albums and artists */
            int committed = batch_end(&sc.b) == 0;
            if (!done) fprintf(stderr, "library: groups: search and commit %.2f s\n", clock_fsec() - ts);
            if (!done && committed) groups_ok = 1;
            else if (!done) done = 1;          /* (not kept: all of them, below) */
        }
        groups_incr_drop(db);
        if (groups && !lib->cancel && done) {
            if (rebuild_groups_aside(lib, &sc.b) == 0) groups_ok = 1;
            else {
                batch_write(&sc.b);
                int had = sc.b.open;
                if (had && rebuild_groups(lib)) {
                    batch_undo(&sc.b);         /* (half made: none of it kept, and the groups stay to do) */
                    had = 0;
                }
                if (had) index_search(lib, 1); /* changed tracks (their search cleared), new albums and artists */
                groups_ok = batch_end(&sc.b) == 0 && had;
            }
        }
        /* (a card too full for them, or a pass over the songs that stopped
         * at a failed read: the songs are in without albums or artists;
         * scan_pending stays, and the next scan tries again) */
        if (groups && !groups_ok && !lib->cancel) fprintf(stderr, "library: the groups couldn't be written: again next scan\n");
    }
    free(gone);
    for (int k = 0; k < ncgone; k++) free(cgone[k]);
    free(cgone);
    lib->scan_changes = changed + extra + (sc.pending > 0);
    scan_state_free(&sc);
    pv_free(&cv);
    if (lib->bulk && !lib->cancel) {               /* the songs and their groups are in: the indexes, once */
        if (cb) cb(n_files, n_files, "indexes", ctx);
        grp_watch(db, cb, ctx);
        bulk_end(lib);
    }
    grp_unwatch(db);
    t_groups = clock_fsec();
    int pl_left = scan_playlists(lib, &plv, groups, &sc.b, pl_before);   /* (playlists left as they were: again next time) */
    pv_free(&plv);
    if ((sc.pending || sc.marked) && !lib->cancel && groups_ok && !pl_left) {   /* all done (stopped: playlists again next time) */
        batch_write(&sc.b);
        meta_set(db, "scan_pending", 0);
        batch_end(&sc.b);
    }
    t_pl = clock_fsec();

    /* every scan: new albums, and any whose folder's images or tracks changed
     * (an image added or replaced doesn't change any track) */
    lib->art_later = 0;
    if (!lib->cancel && !lib->defer_art) build_art(lib, &iv, &sc.b, cb, ctx);
    pv_free(&iv);
    t_art = clock_fsec();
    fprintf(stderr, "library: scan %.1f s, %d changes: %swalk %.1f, files %.1f (tags %.1f), groups %.1f, playlists %.1f, art %.1f\n",
            t_art - t0, changed, fast ? "fast " : "", t_walk - t0, t_files - t_walk, t_tags, t_groups - t_files,
            t_pl - t_groups, t_art - t_pl);
    /* the listing this scan made the library match (none: files still to be read) */
    if (!lib->cancel) {
        int64_t keep = sc.retry || sc.cs.db_failed || sc.cs.unread || lib->defer_art || lib->art_later || !groups_ok || pl_left ? 0 : (int64_t)sig;   /* (covers, groups or playlists still to make: not as it should be yet) */
        if (keep != meta_get64(db, "scan_sig")) {
            batch_write(&sc.b);
            if (sc.b.open) meta_set64(db, "scan_sig", keep);
            batch_end(&sc.b);
        }
    }

    pv_free(&pv);

    if (lib->cancel) return -1;                /* (stopped while making playlists or covers) */
    int n = (int)db_int(db, "SELECT COUNT(*) FROM tracks", 0, -1);
    if (cb) cb(n, n, "done", ctx);
    return n;
}

int library_scan_unfinished(library_t *lib)
{
    return meta_get(lib->db, "scan_pending") > 0;
}

int library_scan_changes(const library_t *lib) { return lib->scan_changes; }

int library_scan_quick(const library_t *lib) { return lib ? lib->quick : 0; }
int library_scan_held(const library_t *lib) { return lib ? lib->scan_held : 0; }

int library_scan_groups(const library_t *lib) { return lib ? lib->groups_how : 0; }

void library_test_rebuild_groups(library_t *lib)
{
    sqlite3_exec(lib->db, "BEGIN IMMEDIATE", NULL, NULL, NULL);
    sqlite3_exec(lib->db, rebuild_groups(lib) ? "ROLLBACK" : "COMMIT", NULL, NULL, NULL);
}

void library_scan_cancel(library_t *lib) { lib->cancel = 1; }

int library_art(library_t *lib, int64_t album_id, int size, uint16_t *pixels)
{
    return art_get(lib, album_id, size, pixels);
}

/* ---------- covers found online ---------- */

static char g_found_dir[512];

void library_set_found_covers(const char *dir)
{
    snprintf(g_found_dir, sizeof g_found_dir, "%s", dir ? dir : "");
    if (g_found_dir[0]) mkdir(g_found_dir, 0755);
}

int library_found_cover_path(const char *key, const char *ext, char *out, size_t cap)
{
    if (!g_found_dir[0] || !key) return -1;
    uint64_t h = fnv64(FNV64_START, key, strlen(key));
    snprintf(out, cap, "%s/%016llx.%s", g_found_dir, (unsigned long long)h, ext);
    return 0;
}

/* An album's cover state into *art: 0, or -1 if it couldn't be read (which
 * library_album_cover gives as LIB_COVER_UNKNOWN, an album not looked at yet). */
static int cover_state(library_t *lib, int64_t album_id, int *art)
{
    sqlite3_stmt *s = prep(lib->db, "SELECT art FROM albums WHERE id=?");
    if (!s) return -1;
    sqlite3_bind_int64(s, 1, album_id);
    int ok = sqlite3_step(s) == SQLITE_ROW;
    if (ok) *art = sqlite3_column_int(s, 0);
    sqlite3_finalize(s);
    return ok ? 0 : -1;
}

int library_album_cover(library_t *lib, int64_t album_id)
{
    int art;
    return cover_state(lib, album_id, &art) ? LIB_COVER_UNKNOWN : art;
}

static void album_info_row(sqlite3_stmt *s, lib_album_t *a)
{
    memset(a, 0, sizeof *a);
    a->id = sqlite3_column_int64(s, 0);
    copy_col(s, 1, a->key, sizeof a->key);
    copy_col(s, 2, a->title, sizeof a->title);
    a->year = sqlite3_column_int(s, 3);
    copy_col(s, 4, a->artist, sizeof a->artist);
}

int library_album_info(library_t *lib, int64_t album_id, lib_album_t *out)
{
    sqlite3_stmt *s = lib->album_info;
    sqlite3_reset(s);
    sqlite3_bind_int64(s, 1, album_id);
    int ok = sqlite3_step(s) == SQLITE_ROW;
    if (ok) album_info_row(s, out);
    sqlite3_reset(s);   /* (not left on its row: see library_row) */
    return ok ? 0 : -1;
}

int library_albums_without_cover(library_t *lib, lib_album_t **out)
{
    *out = NULL;
    sqlite3_stmt *s = prep(lib->db, ALBUM_INFO_SQL " WHERE al.art=2 ORDER BY al.sort"   /* (2: LIB_COVER_NONE) */);
    if (!s) return -1;
    int n = 0, cap = 0;
    lib_album_t *v = NULL;
    while (sqlite3_step(s) == SQLITE_ROW) {
        lib_album_t *nv = vec_room(v, n, &cap, 64, sizeof *v);
        if (!nv) { free(v); sqlite3_finalize(s); return -1; }
        v = nv;
        album_info_row(s, &v[n]);
        if (v[n].title[0]) n++;            /* (no album title: nothing to look for) */
    }
    sqlite3_finalize(s);
    *out = v;
    return n;
}

static int set_cover_state(library_t *lib, int64_t album_id, int art)
{
    return set_int_col(lib, "UPDATE albums SET art=? WHERE id=?", album_id, art);
}

int library_store_found_cover(library_t *lib, int64_t album_id, const uint16_t *large, const uint16_t *small)
{
    lib_album_t a;
    char over[1200];
    if (library_album_info(lib, album_id, &a) || library_found_cover_path(a.key, "over", over, sizeof over)) return -1;
    /* (its state not read: nothing done. Taken for "no cover of its own", the
     * found one went over its own without the mark that keeps it ahead, and
     * Remove cover then left the album with none.) */
    int cur;
    if (cover_state(lib, album_id, &cur)) return -1;
    int picked = cur == LIB_COVER_OWN || cur == LIB_COVER_PICKED;   /* over a cover of its own: it stays ahead of it */
    if (picked) {
        FILE *f = fopen(over, "we");
        if (!f) return -1;
        fclose(f);
    } else {
        remove(over);
    }
    if (art_put(lib, album_id, LIB_ART_LARGE, large) || art_put(lib, album_id, LIB_ART_SMALL, small)) return -1;
    art_sync(lib);
    return set_cover_state(lib, album_id, picked ? LIB_COVER_PICKED : LIB_COVER_FOUND);
}

int library_remove_found_cover(library_t *lib, int64_t album_id)
{
    lib_album_t a;
    char path[1200];
    int cur;
    /* (its state not read: nothing removed. Taken for "not picked over its
     * own", the album was marked as having no cover, its own never made again.) */
    if (library_album_info(lib, album_id, &a) || cover_state(lib, album_id, &cur)) return -1;
    int picked = cur == LIB_COVER_PICKED;
    if (library_found_cover_path(a.key, "jpg", path, sizeof path) == 0) remove(path);
    if (library_found_cover_path(a.key, "over", path, sizeof path) == 0) remove(path);
    if (picked) {                                    /* back to its own cover, made again from where it is */
        sqlite3_stmt *q = prep(lib->db, "SELECT al.dir, t.path FROM albums al LEFT JOIN tracks t ON t.id=al.first_track"
                                        " WHERE al.id=?");
        sqlite3_stmt *others = prep(lib->db, ART_OTHERS_SQL);
        char dir[1024] = "", first[1024] = "";
        int have = 0, asked = 0;                     /* (asked: where its own cover is, read) */
        if (q) {
            sqlite3_bind_int64(q, 1, album_id);
            if (sqlite3_step(q) == SQLITE_ROW) {
                asked = 1;
                copy_col(q, 0, dir, sizeof dir);
                have = sqlite3_column_type(q, 1) != SQLITE_NULL;
                copy_col(q, 1, first, sizeof first);
            }
            sqlite3_finalize(q);
        }
        /* (on the screen's thread: not behind a helper that is measuring a
         * song for the levelling, which is minutes, and which this thread's
         * own timer would have to stop) */
        g_rd_nowait = 1;
        g_art_later = 0;
        int r = make_art(lib, others, album_id, dir, have ? first : NULL, a.key);
        int later = r < 0 && (g_art_later || !asked);
        g_rd_nowait = 0;
        reader_close_nowait();                     /* (one album's: no helper left waiting; a scan under way starts its own again) */
        sqlite3_finalize(others);
        if (r >= 0) return set_cover_state(lib, album_id, r == 0 ? LIB_COVER_OWN : LIB_COVER_FOUND);
        if (later) {
            /* Its own couldn't be made just now, with little memory to
             * spare, or its file (or where it is) not to be read at that
             * moment: not "it has none" (marked so, no scan looked again).
             * No cover for now, and the next scan makes it. */
            art_del(lib, album_id);
            return db_run_id(lib->db, "UPDATE albums SET art=0, art_sig=0 WHERE id=?", album_id);
        }
        /* (its own is gone meanwhile: no cover, as below) */
    }
    if (library_found_cover_path(a.key, "none", path, sizeof path) == 0) {
        FILE *f = fopen(path, "we");
        if (f) fclose(f);
    }
    art_del(lib, album_id);
    return set_cover_state(lib, album_id, LIB_COVER_NONE);
}

