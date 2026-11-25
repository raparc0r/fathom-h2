/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE
#define _GNU_SOURCE   /* strcasestr (glibc, musl) */
#include "library.h"
#include "core/fsafe.h"

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
#include <sys/statvfs.h>
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
#include "library/playlist.h"
#include "library/reader.h"
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
    void *search_run;       /* the search under way (search_keep) */
    int no_album_artists;   /* album rows show the year, not the artist (library_set_album_artists) */
    char art_dir[1024];
    sqlite3_stmt *row_album, *row_artist, *row_credited, *row_track, *track_full, *album_name, *artist_name;
    sqlite3_stmt *row_playlist, *row_pl_track, *row_plays, *track_album, *playlist_name;
    sqlite3_stmt *row_genre, *row_composer, *row_year, *row_folder, *genre_name, *composer_name, *folder_name;
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
    int groups_how;         /* library_scan_groups() */
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

    "CREATE INDEX IF NOT EXISTS albums_artist ON albums(artist_id, year);";

static void rebuild_groups(library_t *lib);
static int bulk_end(library_t *lib);
static void art_del(library_t *lib, int64_t album_id);
static void art_open(library_t *lib);
static int64_t get_int_col(library_t *lib, const char *sql, int64_t id);
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

/* ---- Search keys, and how close a name is to what was typed ----
 * Each artist, album and song has a key in its `search` column: its name, its
 * artist's and its album's, each folded (text_search_fold: lower case, no
 * accents, words a space apart), with '|' between them (no fold has one):
 *     exodus|bob marley the wailers|exodus
 * A search reads only these (the songs' from an index of their own,
 * tracks_search: 4 MB for 50,000 songs, where the table's rows are 30 MB and
 * reading them from the card took a second by itself), and what's typed is
 * matched against them as they are: nothing folded or copied per row. */
#define SKEY_SEP '|'

/* SQL skey(name, artist, album): the key. */
static void sql_skey(sqlite3_context *ctx, int argc, sqlite3_value **argv)
{
    char out[1600];
    size_t w = 0;
    for (int i = 0; i < 3; i++) {
        const unsigned char *in = i < argc ? sqlite3_value_text(argv[i]) : NULL;
        if (i) out[w++] = SKEY_SEP;
        text_search_fold(in ? (const char *)in : "", out + w, 500);
        w += strlen(out + w);
    }
    sqlite3_result_text(ctx, out, (int)w, SQLITE_TRANSIENT);
}

static void sql_search_keep(sqlite3_context *ctx, int argc, sqlite3_value **argv);

typedef struct { const char *p; int n; } tok_t;

/* [s, e) cut into its words (at most max: a longer name's last ones aren't
 * looked at). */
static int toks_of(const char *s, const char *e, tok_t *t, int max)
{
    int n = 0;
    while (s < e && n < max) {
        const char *sp = memchr(s, ' ', (size_t)(e - s));
        if (!sp) sp = e;
        if (sp > s) t[n++] = (tok_t){s, (int)(sp - s)};
        s = sp + 1;
    }
    return n;
}

static int tok_is(tok_t t, const char *w, int wl, int whole)
{
    return (whole ? t.n == wl : t.n >= wl) && !memcmp(t.p, w, (size_t)wl);
}

/* The words typed, in order, are the tokens from `at` on (the last one whole
 * only if asked). */
static int phrase_at(const tok_t *t, int nt, int at, const char *const *w, const int *wl, int n, int last_whole)
{
    if (at + n > nt) return 0;
    for (int k = 0; k < n; k++)
        if (!tok_is(t[at + k], w[k], wl[k], k < n - 1 || last_whole)) return 0;
    return 1;
}

/* A word of [s, e) starts with w. */
static int span_has(const char *s, const char *e, const char *w, int wl)
{
    while (s < e) {
        if (e - s >= wl && !memcmp(s, w, (size_t)wl)) return 1;
        const char *sp = memchr(s, ' ', (size_t)(e - s));
        if (!sp) break;
        s = sp + 1;
    }
    return 0;
}

/* library_search_tier, of a key. */
static int tier_of_key(const char *key, const char *const *words, int n, int last_whole)
{
    if (n <= 0 || n > 8 || !key) return 0;
    const char *own_e = strchr(key, SKEY_SEP);
    if (!own_e) own_e = key + strlen(key);
    const char *art = *own_e ? own_e + 1 : own_e, *art_e = strchr(art, SKEY_SEP);
    if (!art_e) art_e = art + strlen(art);
    const char *alb = *art_e ? art_e + 1 : art_e, *alb_e = alb + strlen(alb);
    int wl[8], in_own[8], begin = 1, any_own = 0;
    for (int k = 0; k < n; k++) {
        wl[k] = (int)strlen(words[k]);
        in_own[k] = span_has(key, own_e, words[k], wl[k]);
        /* (most rows end here: a word that's nowhere in them) */
        if (!in_own[k]) {
            begin = 0;
            if (!span_has(art, art_e, words[k], wl[k]) && !span_has(alb, alb_e, words[k], wl[k])) return 0;
        } else {
            any_own = 1;
        }
    }
    if (!any_own) return 0;                                  /* an artist's or an album's name alone */
    if (begin) {                                             /* every word begins a word of the name */
        tok_t own[48];
        int no = toks_of(key, own_e, own, 48);
        int the = no > 1 && own[0].n == 3 && !memcmp(own[0].p, "the", 3);   /* (a leading The passed over, as in the lists) */
        if (no - the == n && phrase_at(own, no, the, words, wl, n, 1)) return 1;
        if (no == n && phrase_at(own, no, 0, words, wl, n, 1)) return 1;
        if (phrase_at(own, no, the, words, wl, n, last_whole) || phrase_at(own, no, 0, words, wl, n, last_whole)) return 2;
        for (int at = 1; at + n <= no; at++)
            if (phrase_at(own, no, at, words, wl, n, last_whole)) return 3;
        int all = 1;
        for (int k = 0; k < n && all; k++) {
            int whole = k < n - 1 || last_whole, hit = 0;
            for (int i = 0; i < no && !hit; i++) hit = tok_is(own[i], words[k], wl[k], whole);
            all = hit;
        }
        return all ? 4 : 5;
    }
    /* split between the name and its artist's or album's: one of the words
     * at least in the name and not in the artist's */
    for (int k = 0; k < n; k++)
        if (in_own[k] && !span_has(art, art_e, words[k], wl[k])) return 6;
    return 0;
}

int library_search_tier(const char *name, const char *artist, const char *album, const char *const *words, int n,
                        int last_whole)
{
    char key[1600];
    size_t w = 0;
    const char *part[3] = {name, artist, album};
    for (int i = 0; i < 3; i++) {
        if (i) key[w++] = SKEY_SEP;
        text_search_fold(part[i] ? part[i] : "", key + w, 500);
        w += strlen(key + w);
    }
    return tier_of_key(key, words, n, last_whole);
}

/* A song's key and an artist's, as SQL (an album's: index_search). A song's
 * artist: its own and its album's, if that's another. */
#define SKEY_TRACK(title, artist, album_artist, album) \
    "skey(" title ", coalesce(" artist ",'')||CASE WHEN " album_artist " IS " artist " THEN '' ELSE ' '||coalesce(" \
    album_artist ",'') END, " album ")"
#define SKEY_ARTIST "skey(name, NULL, NULL)"

/* Fill the search columns (only the empty ones, or all after a scan; 2: the
 * empty ones of the songs a scan wrote, temp.g_c, and of albums and artists:
 * not a read of every song for a few new ones, the write lock held). */
static void index_search(library_t *lib, int only_missing)
{
    const char *w = only_missing == 2 ? " WHERE search IS NULL AND id IN (SELECT id FROM temp.g_c)"
                  : only_missing ? " WHERE search IS NULL" : "";
    char sql[600];
    snprintf(sql, sizeof sql, "UPDATE tracks SET search=" SKEY_TRACK("title", "artist", "album_artist", "album") "%s;", w);
    sqlite3_exec(lib->db, sql, NULL, NULL, NULL);
    if (only_missing) w = " WHERE search IS NULL";
    snprintf(sql, sizeof sql,
             "UPDATE albums SET search=skey(title, (SELECT name FROM artists a WHERE a.id=albums.artist_id), NULL)%s;", w);
    sqlite3_exec(lib->db, sql, NULL, NULL, NULL);
    snprintf(sql, sizeof sql, "UPDATE artists SET search=" SKEY_ARTIST "%s;", w);
    sqlite3_exec(lib->db, sql, NULL, NULL, NULL);
}

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
    if (!had_composer || !had_credits || version < 4) {   /* (4: names spelled as tagged now) */
        sqlite3_exec(lib->db, "BEGIN", NULL, NULL, NULL);
        rebuild_groups(lib);
        sqlite3_exec(lib->db, "PRAGMA user_version=4; COMMIT", NULL, NULL, NULL);
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
    sqlite3_create_function(lib->db, "fold", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, NULL, sql_fold, NULL, NULL);
    sqlite3_create_function(lib->db, "skey", 3, SQLITE_UTF8 | SQLITE_DETERMINISTIC, NULL, sql_skey, NULL, NULL);
    sqlite3_create_function(lib->db, "search_keep", 2, SQLITE_UTF8, &lib->search_run, sql_search_keep, NULL, NULL);
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
    sqlite3_close(lib->db);
    free(lib);
}

/* ---------- what the player died on ----------
 * A file whose tags or cover crash the reader, or take all the memory there is
 * (the player is killed), did so again at every start: the scan picks up where
 * it left off, at the same file. Before reading one, its name is noted in a
 * file in RAM (library_set_trying_dir: /tmp on the H2); done, the note goes.
 * A note still there when the next scan starts means the player died on it.
 * Once could be chance (the player killed for want of memory while reading a
 * blameless file): it's tried again. Twice running at the same one: that file
 * is set aside as unreadable (as one that can't be read is: tried again when
 * it changes), that album as having no cover, and the scan carries on.
 *
 * A file that stops the H2 itself (it hangs until the watchdog restarts it)
 * takes the RAM with it. So a run also leaves a mark on the card
 * (library_set_trying_keep: an empty file, written once, when the run reads
 * its first item, and removed at its end). A mark found with no note in RAM
 * means the H2 went down during the last run: this run is "careful", and
 * writes each item's name into the mark before reading it (a write to the
 * card per item: only ever after such a stop). A careful run's mark found at
 * the next start names the item the H2 went down on: once, twice, as above. */
static char g_trying_dir[256];
static char g_keep_dir[256];

#define RAM_PRE  "fathom-trying-"
#define KEEP_PRE "scan-reading-"

/* The runs that leave a mark on the card (each on a thread of its own) */
static struct keep { const char *what; int careful, marked, fd; } g_keep[] = {
    {"tags", 0, 0, -1}, {"cover", 0, 0, -1}, {"level", 0, 0, -1},
};

void library_set_trying_dir(const char *dir)
{
    snprintf(g_trying_dir, sizeof g_trying_dir, "%s", dir ? dir : "");
}

void library_set_trying_keep(const char *dir)
{
    snprintf(g_keep_dir, sizeof g_keep_dir, "%s", dir ? dir : "");
    for (size_t i = 0; i < sizeof g_keep / sizeof g_keep[0]; i++) {
        if (g_keep[i].fd >= 0) close(g_keep[i].fd);
        g_keep[i].careful = g_keep[i].marked = 0;
        g_keep[i].fd = -1;
    }
}

static struct keep *keep_slot(const char *what)
{
    if (!g_keep_dir[0]) return NULL;
    for (size_t i = 0; i < sizeof g_keep / sizeof g_keep[0]; i++)
        if (!strcmp(g_keep[i].what, what)) return &g_keep[i];
    return NULL;
}

static void note_path(const char *dir, const char *pre, const char *what, const char *sfx, char *out, size_t cap)
{
    snprintf(out, cap, "%s/%s%s%s", dir, pre, what, sfx);
}

/* A note's text (1; 0: no note, or an empty one) */
static int note_read(const char *p, char *out, size_t cap)
{
    int fd = open(p, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    ssize_t n = read(fd, out, cap - 1);
    close(fd);
    if (n <= 0) return 0;
    out[n] = '\0';                                    /* (the text ends at its own NUL, if earlier) */
    return out[0] != '\0';
}

static void dir_sync(const char *dir)
{
    int fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return;
    fsync(fd);
    close(fd);
}

/* (durable: on the card, to be there after a reset) */
static void note_write(const char *dir, const char *p, const char *text, int durable)
{
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return;
    if (write(fd, text, strlen(text) + 1) < 0) { /* (no note: nothing set aside) */ }
    if (durable) fsync(fd);
    close(fd);
    if (durable) dir_sync(dir);
}

/* A run stopped on `text`: 2 when the stop before it was on the same one
 * (the note WHAT.1, forgotten now), else 1, and it's remembered */
static int note_again(const char *dir, const char *pre, const char *what, const char *text, int durable)
{
    char p[400], before[2048] = "";
    note_path(dir, pre, what, ".1", p, sizeof p);
    if (note_read(p, before, sizeof before) && !strcmp(before, text)) {
        unlink(p);
        if (durable) dir_sync(dir);
        return 2;
    }
    note_write(dir, p, text, durable);
    return 1;
}

/* What the last run of `what` stopped on, its text in out: 0 nothing, 1 for
 * the first time (to be tried once more), 2 twice running (to be set aside).
 * Called once as each run starts: it also says whether this one is careful. */
static int trying_left(const char *what, char *out, size_t cap)
{
    char p[400];
    struct keep *k = keep_slot(what);
    if (cap < 2) return 0;
    if (k) k->careful = 0;
    if (g_trying_dir[0]) {                            /* the player died (the H2 stayed up) */
        note_path(g_trying_dir, RAM_PRE, what, "", p, sizeof p);
        int got = note_read(p, out, cap);
        unlink(p);
        if (got) {
            int r = note_again(g_trying_dir, RAM_PRE, what, out, 0);
            if (r == 1) fprintf(stderr, "library: the player stopped while on %.200s (%s): once more\n", out, what);
            return r;
        }
    }
    if (!k) return 0;
    note_path(g_keep_dir, KEEP_PRE, what, "", p, sizeof p);
    if (access(p, F_OK) != 0) return 0;
    k->careful = 1;                                   /* the H2 itself went down during the last run */
    if (!note_read(p, out, cap)) {
        fprintf(stderr, "library: the H2 stopped during the last run (%s): each item is noted on the card this time\n", what);
        return 0;
    }
    int r = note_again(g_keep_dir, KEEP_PRE, what, out, 1);
    if (r == 1) fprintf(stderr, "library: the H2 stopped while on %.200s (%s): once more\n", out, what);
    return r;
}

/* What a run stopped on twice running (1, its text in out, and forgotten) */
int library_trying_left(const char *what, char *out, size_t cap)
{
    return trying_left(what, out, cap) == 2;
}

/* A note kept open while a run of files is read: *fd -1 before the first */
void library_trying(const char *what, int *fd, const char *text)
{
    if (g_trying_dir[0]) {
        if (*fd < 0) {
            char p[400];
            note_path(g_trying_dir, RAM_PRE, what, "", p, sizeof p);
            *fd = open(p, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        }
        if (*fd >= 0 && pwrite(*fd, text, strlen(text) + 1, 0) < 0) { /* (no note: nothing set aside) */ }
    }
    struct keep *k = keep_slot(what);
    if (!k) return;
    int made = 0;
    if (!k->marked) {                                 /* the run's mark on the card, once */
        char p[400];
        note_path(g_keep_dir, KEEP_PRE, what, "", p, sizeof p);
        k->marked = 1;                                /* (a card that can't be written: not tried per item) */
        k->fd = open(p, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (k->fd < 0) return;
        made = 1;
        if (!k->careful) {
            fsync(k->fd);
            close(k->fd);
            k->fd = -1;
            dir_sync(g_keep_dir);
            return;
        }
    }
    if (k->careful && k->fd >= 0) {
        if (pwrite(k->fd, text, strlen(text) + 1, 0) < 0) { /* (as above) */ }
        fsync(k->fd);
        if (made) dir_sync(g_keep_dir);
    }
}

void library_trying_done(const char *what, int *fd)
{
    char p[400];
    if (g_trying_dir[0]) {
        if (*fd >= 0) close(*fd);
        *fd = -1;
        note_path(g_trying_dir, RAM_PRE, what, "", p, sizeof p);
        unlink(p);
    }
    struct keep *k = keep_slot(what);
    if (!k) return;
    if (k->fd >= 0) close(k->fd);
    k->fd = -1;
    int gone = 0;                                     /* (a run that read nothing left no mark: nothing written) */
    note_path(g_keep_dir, KEEP_PRE, what, "", p, sizeof p);
    gone |= unlink(p) == 0;
    if (k->careful) {                                 /* (a careful run that ended: its "once" forgotten too) */
        note_path(g_keep_dir, KEEP_PRE, what, ".1", p, sizeof p);
        gone |= unlink(p) == 0;
    }
    if (gone) dir_sync(g_keep_dir);
    k->marked = k->careful = 0;
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
    if (pv->n == pv->cap) {
        int nc = pv->cap ? pv->cap * 2 : 1024;
        pent_t *nv = realloc(pv->v, (size_t)nc * sizeof *nv);
        if (!nv) { pv->failed = 1; return -1; }
        pv->v = nv;
        pv->cap = nc;
    }
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
    const char *dot = strrchr(path, '.'), *slash = strrchr(path, '/');
    if (!dot || (slash && dot < slash) || (slash ? slash[1] : path[0]) == '.') return 0;
    return !strcasecmp(dot, ".jpg") || !strcasecmp(dot, ".jpeg") || !strcasecmp(dot, ".png");
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

static uint64_t fnv(uint64_t hsh, const void *p, size_t n);
static int cue_in_tags(const char *path);
#define SIG_SEED 14695981039346656037ULL
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
        k->v[k->n++] = (kent_t){fnv(SIG_SEED, p, strlen(p)), (uint32_t)id, (uint32_t)mt, (uint32_t)sz};
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
    uint64_t h = fnv(SIG_SEED, p, strlen(p));
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
    h = fnv(h, &mt, sizeof mt);
    h = fnv(h, &sz, sizeof sz);
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
static int lock_boost(void)
{
#ifdef __linux__
    id_t tid = (id_t)syscall(SYS_gettid);
    errno = 0;
    int n = getpriority(PRIO_PROCESS, tid);
    if (errno == 0 && n > 0 && setpriority(PRIO_PROCESS, tid, 0) == 0) return n;
#endif
    return NO_NICE;
}

static void lock_unboost(int n)
{
#ifdef __linux__
    if (n != NO_NICE) setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), n);
#else
    (void)n;
#endif
}

/* Every statement reset (one left on a row keeps its read open, and a
 * write on that old snapshot fails): only where none is being stepped. */
static void reset_all(sqlite3 *db)
{
    for (sqlite3_stmt *s = sqlite3_next_stmt(db, NULL); s; s = sqlite3_next_stmt(db, s))
        if (sqlite3_stmt_busy(s)) sqlite3_reset(s);
}

/* 0, or -1: its commit failed (a full card), and it was rolled back */
static int batch_end(batch_t *b)
{
    if (!b->open) return 0;
    reset_all(b->db);
    /* (a commit that fails, a full card: rolled back, or every later batch
     * would sit in this one, never committed) */
    int rc = sqlite3_exec(b->db, "COMMIT", NULL, NULL, NULL) == SQLITE_OK ? 0 : -1;
    if (rc && !sqlite3_get_autocommit(b->db)) sqlite3_exec(b->db, "ROLLBACK", NULL, NULL, NULL);
    b->open = 0;
    lock_unboost(b->nice);
    return rc;
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
    b->since = now_s();
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
    if (tags_supported(path)) pv_push(c->pv, path, &st);
    else if (m3u_supported(path)) pv_push(c->plv, path, &st);
    else if (cue_supported(path)) pv_push(c->cv, path, &st);
    else if (art_image(path)) pv_push(c->iv, path, &st);
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
    if (stat(e->p, &st) || !S_ISREG(st.st_mode)) return fw_differs("not found by the kernel:", e->p);
    if ((int64_t)st.st_mtime == lib_mtime && (int64_t)st.st_size == lib_size)
        return fw_differs("date or size read otherwise:", e->p);   /* (it hasn't changed after all) */
    e->mtime = (int64_t)st.st_mtime;
    e->size = (int64_t)st.st_size;
    return 0;
}

static int pv_find(const pathvec_t *pv, const char *p);
static int cue_in_tags(const char *path);

static int against_library(library_t *lib, pathvec_t *pv, pathvec_t *cv, int verify)
{
    int changed = 0, gone = 0, added = 0, bad = 0;
    for (int i = 0; i < pv->n; i++) pv->v[i].st = ST_NEW;
    for (int i = 0; i < cv->n; i++) cv->v[i].st = ST_NEW;
    for (int pass = 0; pass < 2 && !bad; pass++) {
        sqlite3_stmt *s = prep(lib->db, pass == 0 ? "SELECT path, mtime, size, id, tags_v FROM tracks WHERE cue=0"
                                                  : "SELECT path, mtime, size, 0, 0, kind FROM cues");
        if (!s) return -1;
        while (!bad && sqlite3_step(s) == SQLITE_ROW) {
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
            uint64_t h = fnv(SIG_SEED + (uint64_t)k, e->p, strlen(e->p));
            h = fnv(h, &e->mtime, sizeof e->mtime);
            h = fnv(h, &e->size, sizeof e->size);
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
    pv_free(pv);
    known_reset(known);
    pv->known = known;
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
            double t0 = now_s();
            while (sqlite3_step(q) == SQLITE_ROW) { }
            sqlite3_finalize(q);
            if (now_s() - t0 >= 1.0) fprintf(stderr, "library: groups: %.1f s: %.90s\n", now_s() - t0, sql);
        }
        sql = tail;
    }
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

/* The folder all the tracks share (up to its last '/'): "" if none */
static void folders_root(sqlite3 *db, char *root, size_t cap)
{
    root[0] = '\0';
    sqlite3_stmt *ends = prep(db, "SELECT MIN(path), MAX(path) FROM tracks");
    if (ends && sqlite3_step(ends) == SQLITE_ROW && sqlite3_column_text(ends, 0)) {
        const char *a = (const char *)sqlite3_column_text(ends, 0), *b = (const char *)sqlite3_column_text(ends, 1);
        size_t n = 0, cut = 0;   /* the common part, up to its last '/' */
        while (a[n] && b && a[n] == b[n]) { if (a[n] == '/') cut = n; n++; }
        if (cut >= cap) cut = cap - 1;
        memcpy(root, a, cut);
        root[cut] = '\0';
    }
    sqlite3_finalize(ends);
}

/* Folders: the tree of the tracks' folders, from the folder they all share.
 * counts_only: every song has its folder already (a first scan that set it
 * as the song was added, from the same root); only the counts are left. */
static void rebuild_folders(sqlite3 *db, int counts_only)
{
    char root[1024];
    folders_root(db, root, sizeof root);
    sqlite3_stmt *q = prep(db, "SELECT id, path, folder_id FROM tracks ORDER BY path");
    sqlite3_stmt *up = prep(db, "INSERT INTO folders(path, parent, name, sort, seen) VALUES(?,?,?,?,1)"
                                " ON CONFLICT(path) DO UPDATE SET parent=excluded.parent, name=excluded.name,"
                                " sort=excluded.sort, seen=1");
    sqlite3_stmt *get = prep(db, "SELECT id FROM folders WHERE path=?");
    sqlite3_stmt *ut = prep(db, "UPDATE tracks SET folder_id=? WHERE id=?");
    if (q && up && get && ut) {
        double t0 = now_s();
        if (!counts_only) sqlite3_exec(db, "UPDATE folders SET seen=0", NULL, NULL, NULL);
        char dir[1024], last[1024] = "";
        int64_t fid = 0;
        while (!counts_only && sqlite3_step(q) == SQLITE_ROW) {
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
        if (now_s() - t0 >= 1.0) fprintf(stderr, "library: groups: %.1f s: each song's folder\n", now_s() - t0);
        /* Songs in each folder and those below it: every track counted once
         * for its folder and each folder above (one walk up the tree per
         * track, not a pass over all tracks per folder: that re-read the
         * table from the card for minutes on the H2) */
        exec_said(db,
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
    sqlite3_finalize(up);
    sqlite3_finalize(get);
    sqlite3_finalize(ut);
}

/* Composers: by name as tagged, ids kept from scan to scan. On its own when
 * only composers were read (an upgraded library's first scan). */
static void rebuild_composers(sqlite3 *db, int counts_only)
{
    if (!counts_only) exec_said(db,
        "INSERT OR IGNORE INTO composers(name) SELECT DISTINCT TRIM(composer) FROM tracks"
        " WHERE TRIM(COALESCE(composer,''))<>'';"
        "UPDATE tracks SET composer_id=(SELECT id FROM composers WHERE name=TRIM(tracks.composer))"
        " WHERE composer_id IS NOT (SELECT id FROM composers WHERE name=TRIM(tracks.composer));");
    exec_said(db,
        "UPDATE composers SET nalbums=(SELECT COUNT(DISTINCT album_id) FROM tracks WHERE composer_id=composers.id),"
        " sort=COALESCE(sort, fold(name));"
        "DELETE FROM composers WHERE nalbums=0;");
    if (!counts_only) exec_said(db,
        /* the spelling as tagged now (names match ignoring case: a retag from
         * "j. s. bach" to "J. S. Bach" was the same row, kept as first seen;
         * compared exactly, or the column's NOCASE took them as equal) */
        "UPDATE composers SET name=(SELECT TRIM(composer) FROM tracks WHERE composer_id=composers.id ORDER BY path LIMIT 1)"
        " WHERE name COLLATE BINARY IS NOT (SELECT TRIM(composer) FROM tracks WHERE composer_id=composers.id ORDER BY path LIMIT 1);");
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
        if (first_aa || !named || g_grp_plain) {
            sqlite3_stmt *un = first_aa ? g->ura : g->ur;
            sqlite3_reset(un);
            sqlite3_bind_text(un, 1, k.various ? "Various Artists" : album_artist_name, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(un, 2, k.aakey, -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(un, 3, album_artist);
            sqlite3_step(un);
        }
    }
    if (track_artist != album_artist && GRP_MINE(g->artists, track_artist) &&
        (!grp_mark(g, track_artist, GRP_NAMED) || g_grp_plain)) {
        sqlite3_reset(g->ur);
        sqlite3_bind_text(g->ur, 1, *artist ? artist : "Unknown artist", -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(g->ur, 2, k.akey, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(g->ur, 3, track_artist);
        sqlite3_step(g->ur);
    }

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
        snprintf(dir, sizeof dir, "%s", path ? path : "");
        char *slash = strrchr(dir, '/');
        if (slash) *slash = '\0';
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
        if (GRP_MINE(g->artists, cid) && (!grp_mark(g, cid, GRP_NAMED) || g_grp_plain)) {
            sqlite3_reset(g->ur);
            sqlite3_bind_text(g->ur, 1, names[i], -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(g->ur, 2, ckey, -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(g->ur, 3, cid);
            sqlite3_step(g->ur);
        }
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

static void groups_counts(library_t *lib, double *t_folders, int as_added);

static void rebuild_groups(library_t *lib)
{
    sqlite3 *db = lib->db;
    double t0 = now_s(), t1 = t0, t2 = t0;
    sqlite3_stmt *q = prep(db, "SELECT id, artist, album_artist, album, year, path, title, album_id, artist_id"
                               " FROM tracks ORDER BY path");
    grp_t g;
    if (!q || grp_open(&g, db)) { sqlite3_finalize(q); return; }

    sqlite3_exec(db, "UPDATE albums SET year=0, dir=NULL, first_track=NULL;"
                     "UPDATE tracks SET sort=songkey(COALESCE(title,'')) WHERE sort IS NOT songkey(COALESCE(title,''));",
                 NULL, NULL, NULL);
    sqlite3_exec(db, "DELETE FROM credits", NULL, NULL, NULL);
    /* names taken afresh from the tags (an album artist's first spelling in
     * path order, else the first track or credit to name them, as a new
     * library would): a retag from "the beatles" to "The Beatles" kept the
     * old spelling while any track matched */
    sqlite3_exec(db, "UPDATE artists SET name=NULL", NULL, NULL, NULL);
    while (sqlite3_step(q) == SQLITE_ROW) grp_track(&g, q);
    t1 = now_s();
    sqlite3_finalize(q);
    grp_close(&g);
    groups_counts(lib, &t2, 0);
    fprintf(stderr, "library: groups: tracks %.2f s, counts %.2f s, folders %.2f s\n", t1 - t0, t2 - t1, now_s() - t2);
}

/* What follows the tracks' pass: each album's and artist's counts, the ones
 * left empty removed, genres, composers and folders. Also on its own, after
 * a first scan that grouped each song as it was added (direct_track):
 * as_added, 1: each song has its genre and composer already, their names as
 * the first song in path order spells them (what the statements left out
 * here would find, reading every song again: 15 s on the H2 with 50,000);
 * 2: its folder as well. */
static void groups_counts(library_t *lib, double *t_folders, int as_added)
{
    sqlite3 *db = lib->db;
    exec_said(db, "UPDATE albums SET ntracks=(SELECT COUNT(*) FROM tracks WHERE album_id=albums.id)");
    /* Albums gone: their covers too. SQLite gives the next new album the
     * highest id again, and a coverless one showed the old album's cover. */
    sqlite3_stmt *gone = prep(db, "SELECT id FROM albums WHERE ntracks=0");
    while (gone && sqlite3_step(gone) == SQLITE_ROW) art_del(lib, sqlite3_column_int64(gone, 0));
    sqlite3_finalize(gone);
    exec_said(db,
        "DELETE FROM albums WHERE ntracks=0;"
        "UPDATE artists SET nalbums=(SELECT COUNT(*) FROM albums WHERE artist_id=artists.id);"
        "UPDATE artists SET ncredit_albums=(SELECT COUNT(DISTINCT t.album_id) FROM credits c"
        " JOIN tracks t ON t.id=c.track_id WHERE c.artist_id=artists.id);"
        "DELETE FROM artists WHERE nalbums=0 AND ncredit_albums=0 AND id NOT IN (SELECT DISTINCT artist_id FROM tracks);");
    /* Genres and Composers: by name as tagged, ids kept from scan to scan */
    if (!as_added) exec_said(db,
        "INSERT OR IGNORE INTO genres(name) SELECT DISTINCT TRIM(genre) FROM tracks WHERE TRIM(COALESCE(genre,''))<>'';"
        "UPDATE tracks SET genre_id=(SELECT id FROM genres WHERE name=TRIM(tracks.genre))"
        " WHERE genre_id IS NOT (SELECT id FROM genres WHERE name=TRIM(tracks.genre));");
    exec_said(db,
        "UPDATE genres SET nalbums=(SELECT COUNT(DISTINCT album_id) FROM tracks WHERE genre_id=genres.id),"
        " sort=COALESCE(sort, fold(name));"
        "DELETE FROM genres WHERE nalbums=0;");
    if (!as_added) exec_said(db,
        "UPDATE genres SET name=(SELECT TRIM(genre) FROM tracks WHERE genre_id=genres.id ORDER BY path LIMIT 1)"
        " WHERE name COLLATE BINARY IS NOT (SELECT TRIM(genre) FROM tracks WHERE genre_id=genres.id ORDER BY path LIMIT 1);");
    exec_said(db,
        /* search: what a changed name is found by (albums show their artist's) */
        "UPDATE artists SET search=NULL WHERE search IS NOT " SKEY_ARTIST ";"
        "UPDATE albums SET search=NULL WHERE artist_id IN (SELECT id FROM artists WHERE search IS NULL);");
    rebuild_composers(db, as_added > 0);
    if (t_folders) *t_folders = now_s();
    rebuild_folders(db, as_added > 1);
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
    double t0 = now_s();
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
    if (!rc) rebuild_groups(lib);              /* (on the copies: temp comes first) */
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
    double t1 = now_s();
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
        reset_all(db);
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        b->open = 0;
        lock_unboost(b->nice);
        shadow_drop(db);
        return -1;
    }
    batch_end(b);
    double t2 = now_s();
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
    while (sqlite3_step(q) == SQLITE_ROW) idset_add(set, sqlite3_column_int64(q, 0));
    sqlite3_finalize(q);
    return set->failed ? -1 : 0;
}

/* Folders of the changed songs: theirs found or made, and each folder's count
 * (its songs and those below) moved by what left it and what came in. 0, or
 * -1 (rebuild_folders() then) */
static int folders_where_changed(sqlite3 *db, const char *root)
{
    sqlite3_stmt *q = prep(db, "SELECT id, path, folder_id FROM main.tracks WHERE id IN (SELECT id FROM temp.g_c)"
                               " ORDER BY path");
    sqlite3_stmt *up = prep(db, "INSERT INTO folders(path, parent, name, sort, seen) VALUES(?,?,?,?,1)"
                                " ON CONFLICT(path) DO UPDATE SET parent=excluded.parent, name=excluded.name,"
                                " sort=excluded.sort, seen=1");
    sqlite3_stmt *get = prep(db, "SELECT id FROM folders WHERE path=?");
    sqlite3_stmt *ut = prep(db, "UPDATE tracks SET folder_id=? WHERE id=?");
    int rc = q && up && get && ut ? 0 : -1;
    while (!rc && sqlite3_step(q) == SQLITE_ROW) {
        const char *path = (const char *)sqlite3_column_text(q, 1);
        char dir[1024];
        snprintf(dir, sizeof dir, "%s", path ? path : "");
        char *slash = strrchr(dir, '/');
        if (slash) *slash = '\0';
        int64_t fid = folder_id(db, up, get, strlen(dir) >= strlen(root) ? dir : root, root);
        if (!fid) { rc = -1; break; }
        if (sqlite3_column_type(q, 2) == SQLITE_NULL || sqlite3_column_int64(q, 2) != fid) {
            sqlite3_reset(ut);
            sqlite3_bind_int64(ut, 1, fid);
            sqlite3_bind_int64(ut, 2, sqlite3_column_int64(q, 0));
            sqlite3_step(ut);
        }
    }
    sqlite3_finalize(q);
    sqlite3_finalize(up);
    sqlite3_finalize(get);
    sqlite3_finalize(ut);
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
 * nothing written, the full rebuild); -1: failed part way (the full rebuild
 * puts it right). Holds the write lock (b). temp.g_c and g_old filled. */
static int rebuild_groups_where_changed(library_t *lib, batch_t *b)
{
    sqlite3 *db = lib->db;
    double t0 = now_s();
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
    int rc = nq && fa && fb && ia && ib && nk ? 0 : 1;
    while (!rc && sqlite3_step(nq) == SQLITE_ROW) {
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
    double t_found = now_s();
    batch_write(b);
    if (!b->open) return 1;
    double t_locked = now_s();
    idset_t A = {0}, R = {0};
    grp_t g;
    sqlite3_stmt *mk = prep(db, "SELECT kind, key FROM temp.g_newkey");
    rc = mk && grp_open(&g, db) == 0 ? 0 : -1;
    if (rc) { sqlite3_finalize(mk); return 1; }
    while (sqlite3_step(mk) == SQLITE_ROW) {
        int album = sqlite3_column_int(mk, 0);
        int64_t id = upsert_key(album ? &g.u_alb : &g.u_art, (const char *)sqlite3_column_text(mk, 1));
        char *ins = sqlite3_mprintf("INSERT OR IGNORE INTO temp.%s VALUES(%lld)", album ? "g_alb" : "g_art", (long long)id);
        if (ins) sqlite3_exec(db, ins, NULL, NULL, NULL);
        sqlite3_free(ins);
    }
    sqlite3_finalize(mk);
    rc = load_idset(db, "SELECT id FROM temp.g_alb", &A) || load_idset(db, "SELECT id FROM temp.g_art", &R) ? -1 : 0;
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
    while (!rc && sqlite3_step(q) == SQLITE_ROW) grp_track(&g, q);
    sqlite3_finalize(q);
    grp_close(&g);
    double t1 = now_s();

    /* A's and R's counts; the empty ones gone (an album's covers with it) */
    rc = rc ? rc : shadow_exec(db,
        "UPDATE main.albums SET ntracks=(SELECT COUNT(*) FROM main.tracks WHERE album_id=albums.id)"
        " WHERE id IN (SELECT id FROM temp.g_alb);");
    sqlite3_stmt *gone = rc ? NULL : prep(db, "SELECT id FROM main.albums WHERE ntracks=0 AND id IN (SELECT id FROM temp.g_alb)");
    while (gone && sqlite3_step(gone) == SQLITE_ROW) art_del(lib, sqlite3_column_int64(gone, 0));
    sqlite3_finalize(gone);
    rc = rc ? rc : shadow_exec(db,
        "DELETE FROM main.albums WHERE ntracks=0 AND id IN (SELECT id FROM temp.g_alb);"
        "UPDATE main.artists SET nalbums=(SELECT COUNT(*) FROM main.albums WHERE artist_id=artists.id),"
        " ncredit_albums=(SELECT COUNT(DISTINCT t.album_id) FROM main.credits c JOIN main.tracks t ON t.id=c.track_id"
        " WHERE c.artist_id=artists.id) WHERE id IN (SELECT id FROM temp.g_art);"
        "DELETE FROM main.artists WHERE id IN (SELECT id FROM temp.g_art) AND nalbums=0 AND ncredit_albums=0"
        " AND NOT EXISTS (SELECT 1 FROM main.tracks WHERE artist_id=artists.id);"
        /* genres and composers of the changed songs, old and new */
        "INSERT OR IGNORE INTO main.genres(name) SELECT DISTINCT TRIM(genre) FROM main.tracks"
        " WHERE id IN (SELECT id FROM temp.g_c) AND TRIM(COALESCE(genre,''))<>'';"
        "UPDATE main.tracks SET genre_id=(SELECT id FROM main.genres WHERE name=TRIM(tracks.genre))"
        " WHERE id IN (SELECT id FROM temp.g_c) AND genre_id IS NOT (SELECT id FROM main.genres WHERE name=TRIM(tracks.genre));"
        "CREATE TEMP TABLE g_gen AS SELECT genre_id AS id FROM temp.g_old WHERE genre_id IS NOT NULL"
        " UNION SELECT genre_id FROM main.tracks WHERE id IN (SELECT id FROM temp.g_c) AND genre_id IS NOT NULL;"
        "UPDATE main.genres SET nalbums=(SELECT COUNT(DISTINCT album_id) FROM main.tracks WHERE genre_id=genres.id),"
        " sort=COALESCE(sort, fold(name)) WHERE id IN (SELECT id FROM temp.g_gen);"
        "DELETE FROM main.genres WHERE nalbums=0 AND id IN (SELECT id FROM temp.g_gen);"
        "UPDATE main.genres SET name=(SELECT TRIM(genre) FROM main.tracks WHERE genre_id=genres.id ORDER BY path LIMIT 1)"
        " WHERE id IN (SELECT id FROM temp.g_gen) AND name COLLATE BINARY IS NOT"
        " (SELECT TRIM(genre) FROM main.tracks WHERE genre_id=genres.id ORDER BY path LIMIT 1);"
        "INSERT OR IGNORE INTO main.composers(name) SELECT DISTINCT TRIM(composer) FROM main.tracks"
        " WHERE id IN (SELECT id FROM temp.g_c) AND TRIM(COALESCE(composer,''))<>'';"
        "UPDATE main.tracks SET composer_id=(SELECT id FROM main.composers WHERE name=TRIM(tracks.composer))"
        " WHERE id IN (SELECT id FROM temp.g_c)"
        " AND composer_id IS NOT (SELECT id FROM main.composers WHERE name=TRIM(tracks.composer));"
        "CREATE TEMP TABLE g_comp AS SELECT composer_id AS id FROM temp.g_old WHERE composer_id IS NOT NULL"
        " UNION SELECT composer_id FROM main.tracks WHERE id IN (SELECT id FROM temp.g_c) AND composer_id IS NOT NULL;"
        "UPDATE main.composers SET nalbums=(SELECT COUNT(DISTINCT album_id) FROM main.tracks WHERE composer_id=composers.id),"
        " sort=COALESCE(sort, fold(name)) WHERE id IN (SELECT id FROM temp.g_comp);"
        "DELETE FROM main.composers WHERE nalbums=0 AND id IN (SELECT id FROM temp.g_comp);"
        "UPDATE main.composers SET name=(SELECT TRIM(composer) FROM main.tracks WHERE composer_id=composers.id"
        " ORDER BY path LIMIT 1) WHERE id IN (SELECT id FROM temp.g_comp) AND name COLLATE BINARY IS NOT"
        " (SELECT TRIM(composer) FROM main.tracks WHERE composer_id=composers.id ORDER BY path LIMIT 1);"
        /* search: what a changed name is found by (albums show their artist's) */
        "UPDATE main.artists SET search=NULL WHERE search IS NOT " SKEY_ARTIST ";"
        "UPDATE main.albums SET search=NULL WHERE artist_id IN (SELECT id FROM main.artists WHERE search IS NULL);");
    double t2 = now_s();
    rc = rc ? rc : folders_where_changed(db, root);
    free(A.bits);
    free(R.bits);
    fprintf(stderr, "library: groups where songs changed: %lld songs, %s in %.2f s (found %.2f, lock waited %.2f,"
            " tracks %.2f, counts %.2f, folders %.2f: the lock held %.2f s so far)\n", (long long)around,
            rc ? "failed" : "done", now_s() - t0, t_found - t0, t_locked - t_found, t1 - t_locked, t2 - t1, now_s() - t2,
            now_s() - t_locked);
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

static uint32_t art_sum(const void *px, size_t n)
{
    const unsigned char *b = px;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 16777619u;
    return h;
}

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
    double t0 = now_s();
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
    fprintf(stderr, "library: covers: %d moved into the store in %.1f s%s\n", moved, now_s() - t0,
            lib->art_legacy ? " (more next time)" : "");
}

int library_art_legacy(library_t *lib)
{
    if (lib->art_legacy) lib->art_legacy = art_legacy_any(lib);   /* (another connection's scan may have moved them) */
    return lib->art_legacy;
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

static const art_limits_t ART_LIMITS = {FATHOM_JPEG_MAX_MEMORY, FATHOM_ART_MAX_PIXELS, FATHOM_ART_MAX_FILE, 0};

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

/* Whole inside a size x size square, the rest white (a station's logo: a wide
 * one isn't cut at its sides, and white is what its transparent parts are
 * laid over), as RGB565: 0, -1 */
static int art_fit(const unsigned char *img, int w, int h, int size, uint16_t *pixels)
{
    int big = w > h ? w : h;
    int dw = (int)((long long)w * size / big), dh = (int)((long long)h * size / big);
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    unsigned char *rs = malloc((size_t)size * (size_t)size * 3);
    if (!rs) return -1;
    memset(rs, 255, (size_t)size * (size_t)size * 3);
    int ox = (size - dw) / 2, oy = (size - dh) / 2;
    for (int y = 0; y < dh; y++) {                 /* (each pixel the average of those under it, as area_downscale) */
        int y0 = (int)((long long)y * h / dh), y1 = (int)((long long)(y + 1) * h / dh);
        if (y1 <= y0) y1 = y0 + 1;
        for (int x = 0; x < dw; x++) {
            int x0 = (int)((long long)x * w / dw), x1 = (int)((long long)(x + 1) * w / dw);
            if (x1 <= x0) x1 = x0 + 1;
            unsigned r = 0, g = 0, b = 0, n = (unsigned)((y1 - y0) * (x1 - x0));
            for (int sy = y0; sy < y1; sy++) {
                const unsigned char *p = img + 3 * ((size_t)sy * (size_t)w + (size_t)x0);
                for (int sx = x0; sx < x1; sx++, p += 3) { r += p[0]; g += p[1]; b += p[2]; }
            }
            unsigned char *o = rs + 3 * ((size_t)(oy + y) * size + (size_t)(ox + x));
            o[0] = (unsigned char)((r + n / 2) / n);
            o[1] = (unsigned char)((g + n / 2) / n);
            o[2] = (unsigned char)((b + n / 2) / n);
        }
    }
    to_rgb565(rs, size, pixels);
    free(rs);
    return 0;
}

/* A station's logo comes from anywhere: less of everything than a cover gets */
static const art_limits_t LOGO_LIMITS = {1024L * 1024, 400000LL, 512L * 1024, 1};

int library_logo_decode(const unsigned char *enc, size_t len, uint16_t *large, uint16_t *small, int *short_side)
{
    unsigned char *img;
    int w, h;
    if (art_decode_mem(enc, len, LIB_ART_LARGE * 3 / 2, &LOGO_LIMITS, &img, &w, &h)) return -1;
    if (short_side) *short_side = w < h ? w : h;
    int r = art_fit(img, w, h, LIB_ART_SMALL, small);
    if (r == 0 && large) r = art_fit(img, w, h, LIB_ART_LARGE, large);
    free(img);
    return r;
}

int library_logo_rgb(const unsigned char *rgb, int w, int h, uint16_t *large, uint16_t *small)
{
    if (!rgb || w < 1 || h < 1 || w > 1024 || h > 1024) return -1;
    int r = art_fit(rgb, w, h, LIB_ART_SMALL, small);
    if (r == 0 && large) r = art_fit(rgb, w, h, LIB_ART_LARGE, large);
    return r;
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

/* ---------- the scan's reads, done by the helper (reader.h) ----------
 * A file's tags, the CUE sheet in them, and a cover decoded small from an
 * image file or from a song's own picture: what a scan runs on files it has
 * never seen. library_reader_do is the helper's side. */
enum { RD_TAGS = 1, RD_SHEET, RD_ART_FILE, RD_ART_TRACK };

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
        unsigned char *img = op == RD_ART_FILE ? art_from_file(path, &w, &h) : art_from_track(path, &w, &h);
        if (!img || w <= 0 || h <= 0) { free(img); return -1; }
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

int library_reader_main(void) { return reader_serve(library_reader_do); }

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

void library_test_reader_budget(long kb) { g_test_budget = kb; }

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
 * g_art_later: one wasn't made while the helper was short of memory. */
static _Thread_local int g_art_later;

static unsigned char *rd_art(int op, const char *path, int *w, int *h)
{
    if (reader_on()) {
        int st, is_short;
        unsigned char *b;
        size_t n;
        int r = reader_call(op, path, rd_budget(&is_short), &st, &b, &n);
        if (r == READER_DIED || (r == 0 && st)) g_art_later |= is_short;
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

static int64_t get_int_col(library_t *lib, const char *sql, int64_t id);

static void build_art(library_t *lib, const pathvec_t *iv, batch_t *b, lib_progress_cb cb, void *ctx)
{
    sqlite3_stmt *q = prep(lib->db,
        "SELECT al.id, al.dir, t.path, al.art, al.art_sig, al.key FROM albums al LEFT JOIN tracks t ON t.id=al.first_track"
        " WHERE al.id>? ORDER BY al.id LIMIT " LIB_STR(ART_PAGE));
    sqlite3_stmt *u = prep(lib->db, "UPDATE albums SET art=?, art_sig=? WHERE id=?");
    sqlite3_stmt *others = prep(lib->db,
        "SELECT path FROM tracks WHERE album_id=? AND has_pic=1 AND path<>? ORDER BY disc, track LIMIT 4");
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
                if (r < 0 && g_art_later) {         /* (not made with little memory to spare: looked for again next scan) */
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
        same = kind == 3 ? 1 : sheet ? was && !strcmp(was, sheet) : was == NULL;   /* (kind 3: `sheet` is why) */
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
 * meant the slow walk at every scan). `why`: NULL, or UNREAD_STOPPED for one
 * the player stopped on (kept in the row's `sheet`). */
#define UNREAD_STOPPED "stopped"

static void cue_unreadable(cue_stmts_t *c, pent_t *f, const char *why)
{
    if (!why) fprintf(stderr, "library: can't read %s\n", f->p);
    sqlite3_reset(c->put_file);
    sqlite3_bind_text(c->put_file, 1, f->p, -1, SQLITE_STATIC);
    sqlite3_bind_int64(c->put_file, 2, f->mtime);
    sqlite3_bind_int64(c->put_file, 3, f->size);
    sqlite3_bind_int(c->put_file, 4, 3);
    if (why) sqlite3_bind_text(c->put_file, 5, why, -1, SQLITE_STATIC);
    else sqlite3_bind_null(c->put_file, 5);
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

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
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
        "INSERT INTO tracks(path, mtime, size, title, artist, album_artist, album, genre, year, disc, track,"
        " codec, rate, bits, channels, duration_ms, has_pic, seen,"
        " rg_state, rg_track, rg_track_peak, rg_album, rg_album_peak, composer, tags_v, sort,"
        " id, album_id, artist_id, genre_id, composer_id, folder_id, search)"
        " VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,1,?,?,?,?,?,?,2,songkey(?4),"
        " ?24, ?25, ?26, ?27, ?28, ?29,"
        " " SKEY_TRACK("?4", "?5", "?6", "?7") ")");
    sc->d_genre = prep(db, "INSERT OR IGNORE INTO genres(name) SELECT TRIM(?1) WHERE TRIM(COALESCE(?1,''))<>''");
    sc->d_gget = prep(db, "SELECT id FROM genres WHERE name=TRIM(?1)");
    sc->d_comp = prep(db, "INSERT OR IGNORE INTO composers(name) SELECT TRIM(?1) WHERE TRIM(COALESCE(?1,''))<>''");
    sc->d_cget = prep(db, "SELECT id FROM composers WHERE name=TRIM(?1)");
    sc->d_fup = prep(db, "INSERT INTO folders(path, parent, name, sort, seen) VALUES(?,?,?,?,1)"
                         " ON CONFLICT(path) DO UPDATE SET parent=excluded.parent, name=excluded.name,"
                         " sort=excluded.sort, seen=1");
    sc->d_fget = prep(db, "SELECT id FROM folders WHERE path=?");
    sqlite3_stmt *top = prep(db, "SELECT COALESCE(MAX(id), 0) + 1 FROM tracks");
    sc->d_next = top && sqlite3_step(top) == SQLITE_ROW ? sqlite3_column_int64(top, 0) : 0;
    sqlite3_finalize(top);
    if (!sc->d_row || !sc->d_ins || !sc->d_genre || !sc->d_gget || !sc->d_comp || !sc->d_cget || !sc->d_fup ||
        !sc->d_fget || sc->d_next != 1) {
        direct_close(sc);
        return;
    }
    /* the folder every file is under (as folders_root: the first and last
     * paths' common part; a file that turns out unreadable can move it, and
     * rebuild_folders, run afterwards, puts that right) */
    const char *a = pv->v[0].p, *b = pv->v[pv->n - 1].p;
    size_t n = 0, cut = 0;
    while (a[n] && a[n] == b[n]) { if (a[n] == '/') cut = n; n++; }
    if (cut >= sizeof sc->d_root) cut = sizeof sc->d_root - 1;
    memcpy(sc->d_root, a, cut);
    sc->d_root[cut] = '\0';
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
    if (strcmp(t->genre, sc->d_gname)) {
        sc->d_gid = direct_named(sc->d_genre, sc->d_gget, t->genre);
        snprintf(sc->d_gname, sizeof sc->d_gname, "%s", t->genre);
        if (strcmp(t->genre, sc->d_gname)) sc->d_gname[0] = '\1';   /* (cut: looked up each time) */
    }
    if (strcmp(t->composer, sc->d_cname)) {
        sc->d_cid = direct_named(sc->d_comp, sc->d_cget, t->composer);
        snprintf(sc->d_cname, sizeof sc->d_cname, "%s", t->composer);
        if (strcmp(t->composer, sc->d_cname)) sc->d_cname[0] = '\1';
    }
    char dir[1024];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) *slash = '\0';
    if (!sc->d_fid || strcmp(dir, sc->d_dir)) {
        sc->d_fid = folder_id(sc->db, sc->d_fup, sc->d_fget, strlen(dir) >= strlen(sc->d_root) ? dir : sc->d_root, sc->d_root);
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
            sqlite3_reset(sc->later);
            sqlite3_bind_int64(sc->later, 1, e->id);
            sqlite3_bind_text(sc->later, 2, e->t.composer, -1, SQLITE_TRANSIENT);
            sqlite3_step(sc->later);
            sqlite3_reset(sc->later);
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
    if (sc->direct == 1 && now_s() - sc->b.since < FIRST_BATCH_S) return;
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
    int n = 0, cap = 0;
    while (sqlite3_step(q) == SQLITE_ROW) {
        if (n == cap) {
            char **grown = realloc(sql, (size_t)(cap = cap ? cap * 2 : 32) * sizeof *sql);
            if (!grown) break;
            sql = grown;
        }
        const char *t = (const char *)sqlite3_column_text(q, 0);
        if (t && (sql[n] = strdup(t))) n++;
    }
    sqlite3_finalize(q);
    double t0 = now_s();
    g_up_done = 0;
    g_up_total = n;
    for (int i = 0; i < n; i++) {
        char *err = NULL;
        if (sqlite3_exec(db, sql[i], NULL, NULL, &err) != SQLITE_OK && !(err && strstr(err, "already exists")))
            fprintf(stderr, "library: %s\n  in: %s\n", err ? err : "?", sql[i]);
        sqlite3_free(err);
        free(sql[i]);
        up_tick();
    }
    free(sql);
    sqlite3_exec(db, "DROP TABLE IF EXISTS bulk_idx", NULL, NULL, NULL);
    lib->bulk = 0;
    if (n) fprintf(stderr, "library: %d indexes built in %.1f s\n", n, now_s() - t0);
    return n;
}

void library_scan_defer_art(library_t *lib, int on) { lib->defer_art = !!on; }

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
    /* (stopped before its indexes were built, and carrying on: now. On the
     * way out, library_open does it at the next start.) */
    if (lib->bulk && !lib->cancel) bulk_end(lib);
    lib->defer_art = 0;
    reader_close();                                /* (the helper's memory back until the next scan) */
    return n;
}

static int scan_once(library_t *lib, const char *root, lib_progress_cb cb, void *ctx, int allow_fast)
{
    double t0 = now_s(), t_walk, t_files, t_groups, t_pl, t_art, t_tags = 0, tt;
    pathvec_t pv = {0}, plv = {0}, cv = {0}, iv = {0}, none = {0}, nocue = {0};
    /* No music folder (renamed, or the card's away): every track would look
     * deleted, with its play counts. The library stays as it is instead. */
    struct stat rst;
    if (stat(root, &rst) != 0 || !S_ISDIR(rst.st_mode)) return -1;
    lib->cancel = 0;
    art_migrate(lib);                              /* (covers from before the store, once) */
    int64_t pl_before = INT64_MAX;                 /* playlists made from here on are kept */
    sqlite3_stmt *plmax = prep(lib->db, "SELECT COALESCE(MAX(id), 0) FROM playlists");
    if (plmax && sqlite3_step(plmax) == SQLITE_ROW) pl_before = sqlite3_column_int64(plmax, 0);
    sqlite3_finalize(plmax);
    lib->groups_how = 0;
    /* the library's files as they are aren't held by name (known_build) */
    known_t *known = known_build(lib->db);
    pv.known = known;
    int fast = allow_fast ? fast_walk(lib, root, &pv, &plv, &cv, &iv) : 0;
    if (!fast) walk(root, &pv, &plv, &cv, &iv, 0, &lib->cancel);
    if (known && cv.n && !lib->cancel) {           /* a sheet where there was none: every file by name, as before */
        pv_free(&pv);
        pv_free(&plv);
        pv_free(&cv);
        pv_free(&iv);
        known_free(known);
        known = NULL;
        fast = allow_fast ? fast_walk(lib, root, &pv, &plv, &cv, &iv) : 0;
        if (!fast) walk(root, &pv, &plv, &cv, &iv, 0, &lib->cancel);
    }
    lib->quick = fast;
    lib->scan_held = pv.n;
    if (lib->pl_dir[0]) walk(lib->pl_dir, &none, &plv, &nocue, NULL, 0, &lib->cancel);   /* your own playlists (no music in there) */
    plv.failed |= pv.failed | cv.failed | none.failed;   /* (playlists: none dropped then) */
    pv_free(&none);
    pv_free(&nocue);
    if (lib->cancel) {
        pv_free(&pv);
        pv_free(&plv);
        pv_free(&cv);
        pv_free(&iv);
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
    if (sig && (int64_t)sig == meta_get64(lib->db, "scan_sig") && !meta_get(lib->db, "scan_pending") &&
        !library_needs_tags(lib) && !library_needs_art(lib) &&
        !get_int_col(lib, "SELECT EXISTS(SELECT 1 FROM albums WHERE art_sig=0)", 0) &&   /* (every cover's source noted) */
        get_int_col(lib, "SELECT COUNT(*) FROM playlists", 0) == plv.n) {   /* (none of the player's left without its file) */
        fprintf(stderr, "library: scan %.1f s: nothing changed (%s%d files)\n", now_s() - t0, fast ? "fast walk, " : "",
                pv.n + (known ? known->n_same : 0));
        known_free(known);
        pv_free(&pv);
        pv_free(&plv);
        pv_free(&cv);
        pv_free(&iv);
        lib->scan_changes = 0;
        sqlite3_stmt *cn = prep(lib->db, "SELECT COUNT(*) FROM tracks");
        int count = (cn && sqlite3_step(cn) == SQLITE_ROW) ? sqlite3_column_int(cn, 0) : -1;
        sqlite3_finalize(cn);
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
    int first = 0;                                 /* a first scan, its indexes set aside just now */
    if (!lib->bulk && pv.n >= g_bulk_min && library_empty(lib)) { bulk_begin(lib); first = lib->bulk; }
    t_walk = now_s();

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
            tt = now_s();
            library_trying("tags", &trying, path);
            int got = !f->skip && rd_tags(path, &e->t, NULL) == 0;
            t_tags += now_s() - tt;
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
        tt = now_s();
        library_trying("tags", &trying, path);
        int later = 0;
        int rr = f->skip ? -1 : rd_tags(path, &e->t, &later);
        if (rr == READER_DIED && !later) {         /* (the helper, not the player: the scan carries on) */
            f->skip = 1;
            fprintf(stderr, "library: the reader stopped on %s: set aside as unreadable\n", path);
        }
        if (rr && later) fprintf(stderr, "library: %s not read with little memory to spare: again at the next scan\n", path);
        int unread = rr != 0;
        t_tags += now_s() - tt;
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
        sqlite3_finalize(sc.later);
        sqlite3_finalize(sc.up);
        cue_stmts_free(&sc.cs);
        free(sc.pend);
        free(sc.seen.bits);
        free(sc.wrote.bits);
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
    int groups_ok = 1;                         /* (0: they were needed and couldn't be written) */
    /* the groups only where songs changed: if no earlier scan left them to
     * do, this one removed none itself, there are groups to start from, and
     * not too many changed */
    int nwrote = 0;
    for (size_t k = 0; k < sc.wrote.cap; k++) nwrote += __builtin_popcount(sc.wrote.bits[k]);
    int where_changed = groups && sc.pending < 2 && !sc.dropped && !sc.wrote.failed && !sc.direct &&
                        nwrote + ngone <= GROUPS_INCR_CHANGED && get_int_col(lib, "SELECT EXISTS(SELECT 1 FROM albums)", 0);
    t_files = now_s();
    /* The files' names aren't needed from here (what's gone is listed, the
     * sheets have their own list): with 50,000 songs, 7 MB handed back before
     * the groups and the covers, the steps that most need the room. */
    int n_files = n_listed;
    pv_free(&pv);
    /* (the next step: albums and artists, no count; not after a first scan
     * that grouped each song as it was added: its indexes are next) */
    if (cb && groups && sc.direct != 1) cb(n_files, n_files, "albums", ctx);
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
            double tg = now_s(), ti;
            if (cb) cb(n_files, n_files, "indexes", ctx);
            bulk_end(lib);
            ti = now_s();
            batch_write(&sc.b);
            int had = sc.b.open;
            if (had) {
                char under[1024];              /* (the folder every song is under: as the songs' folders took it?) */
                folders_root(db, under, sizeof under);
                groups_counts(lib, NULL, strcmp(under, sc.d_root) ? 1 : 2);
                index_search(lib, 1);          /* (albums and artists: the songs' was set as they were added) */
            }
            if (batch_end(&sc.b) == 0 && had) {
                groups_ok = 1;
                done = 0;
                lib->groups_how = 3;
                fprintf(stderr, "library: groups as the songs were added (%d): indexes %.2f s, counts %.2f s\n",
                        sc.d_n, ti - tg, now_s() - ti);
            }
        } else if (groups && sc.direct) {
            fprintf(stderr, "library: groups: not as the songs were added (%d of them so): all made now\n", sc.d_n);
            if (cb && sc.direct == 1) cb(n_files, n_files, "albums", ctx);
        }
        if (groups && !lib->cancel && where_changed) {
            done = rebuild_groups_where_changed(lib, &sc.b);
            if (!done) lib->groups_how = 1;
            double ts = now_s();
            if (!done) index_search(lib, 2);   /* changed tracks (their search cleared), new albums and artists */
            int committed = batch_end(&sc.b) == 0;
            if (!done) fprintf(stderr, "library: groups: search and commit %.2f s\n", now_s() - ts);
            if (!done && committed) groups_ok = 1;
            else if (!done) done = 1;          /* (not kept: all of them, below) */
        }
        groups_incr_drop(db);
        if (groups && !lib->cancel && done) {
            if (rebuild_groups_aside(lib, &sc.b) == 0) groups_ok = 1;
            else {
                batch_write(&sc.b);
                int had = sc.b.open;
                if (had) {
                    rebuild_groups(lib);
                    index_search(lib, 1);      /* changed tracks (their search cleared), new albums and artists */
                }
                groups_ok = batch_end(&sc.b) == 0 && had;
            }
        }
        /* (a card too full for them: the songs are in without albums or
         * artists; scan_pending stays, and the next scan tries again) */
        if (groups && !groups_ok && !lib->cancel) fprintf(stderr, "library: the groups couldn't be written: again next scan\n");
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
    free(sc.wrote.bits);
    pv_free(&cv);
    if (lib->bulk && !lib->cancel) {               /* the songs and their groups are in: the indexes, once */
        if (cb) cb(n_files, n_files, "indexes", ctx);
        bulk_end(lib);
    }
    t_groups = now_s();
    scan_playlists(lib, &plv, groups, &sc.b, pl_before);
    pv_free(&plv);
    if ((sc.pending || sc.marked) && !lib->cancel && groups_ok) {   /* all done (stopped: playlists again next time) */
        batch_write(&sc.b);
        meta_set(db, "scan_pending", 0);
        batch_end(&sc.b);
    }
    t_pl = now_s();

    /* every scan: new albums, and any whose folder's images or tracks changed
     * (an image added or replaced doesn't change any track) */
    if (!lib->cancel && !lib->defer_art) build_art(lib, &iv, &sc.b, cb, ctx);
    pv_free(&iv);
    t_art = now_s();
    fprintf(stderr, "library: scan %.1f s, %d changes: %swalk %.1f, files %.1f (tags %.1f), groups %.1f, playlists %.1f, art %.1f\n",
            t_art - t0, changed, fast ? "fast " : "", t_walk - t0, t_files - t_walk, t_tags, t_groups - t_files,
            t_pl - t_groups, t_art - t_pl);
    /* the listing this scan made the library match (none: files still to be read) */
    if (!lib->cancel) {
        int64_t keep = sc.retry || lib->defer_art || !groups_ok ? 0 : (int64_t)sig;   /* (covers or groups still to make: not as it should be yet) */
        if (keep != meta_get64(db, "scan_sig")) {
            batch_write(&sc.b);
            if (sc.b.open) meta_set64(db, "scan_sig", keep);
            batch_end(&sc.b);
        }
    }

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

/* A view's SQL (?1 its parent, ?2 g_credit_album); *cut: a smart playlist's
 * "a while" (bound as ?1; 0 none). NULL: no such view. */
static const char *list_sql(lib_view_t view, int64_t parent, int64_t *cut)
{
    const char *sql = NULL;
    int64_t smart_cut = 0;
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
    case LIB_SMART: {   /* ?1: the time before which counts as "a while" */
        int64_t now = (int64_t)time(NULL);
        switch ((int)parent) {
        case LIB_SMART_NEVER:
            /* (the albums in their order, each one's unplayed songs from tracks_unplayed:
             * both read from indexes, in order, nothing sorted. CROSS JOIN keeps the
             * albums outside; the other way round sorted every song: 2.5 s for 50,000) */
            sql = "SELECT t.id FROM albums a CROSS JOIN tracks t ON t.album_id=a.id"
                  " WHERE t.play_count=0 AND t.skip_count=0 ORDER BY a.sort, a.year, a.id, t.disc, t.track";
            break;
        case LIB_SMART_REPEAT:
            smart_cut = now - 30 * 86400;
            sql = "SELECT track_id FROM plays WHERE at>?1 AND track_id IN (SELECT id FROM tracks)"
                  " GROUP BY track_id HAVING COUNT(*)>=2 ORDER BY COUNT(*) DESC, MAX(at) DESC LIMIT 50";
            break;
        case LIB_SMART_NOT_HEARD:
            smart_cut = now - 90 * 86400;
            sql = "SELECT id FROM tracks WHERE play_count>0 AND last_played<?1 ORDER BY last_played LIMIT 200";
            break;
        case LIB_SMART_FORGOTTEN:
            smart_cut = now - 60 * 86400;
            sql = "SELECT id FROM tracks WHERE favourite=1 AND last_played<?1 ORDER BY last_played LIMIT 100";
            break;
        case LIB_SMART_SKIPPED:
            sql = "SELECT id FROM tracks WHERE skip_count>=2 ORDER BY skip_count DESC, sort LIMIT 100";
            break;
        default:
            return NULL;
        }
        break;
    }
    case LIB_FOLDER:   /* the subfolders (as -id), then the songs; 0: the top folder's */
        sql = "SELECT x FROM (SELECT -id AS x, 0 AS k, sort AS o FROM folders WHERE parent=?1"
              " UNION ALL SELECT id, 1, path FROM tracks WHERE folder_id=?1) ORDER BY k, o";
        break;
    }
    (void)parent;
    *cut = smart_cut;
    return sql;
}

int library_list(library_t *lib, lib_view_t view, int64_t parent, lib_list_t *out)
{
    int64_t smart_cut = 0;
    const char *sql = list_sql(view, parent, &smart_cut);
    if (!sql) {
        memset(out, 0, sizeof *out);
        return -1;
    }
    if (view == LIB_FOLDER && parent == 0) {   /* (0: the top folder's) */
        sqlite3_stmt *top = prep(lib->db, "SELECT id FROM folders WHERE parent=0 ORDER BY length(path) LIMIT 1");
        if (top && sqlite3_step(top) == SQLITE_ROW) parent = sqlite3_column_int64(top, 0);
        sqlite3_finalize(top);
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
    if (view == LIB_SMART && smart_cut) sqlite3_bind_int64(s, 1, smart_cut);
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

const char *library_smart_name(int which)
{
    /* (N_: listed for translators by tools/lang/strings.py; the UI translates the name it's given) */
    static const char *const N[LIB_SMART_N] = {"", N_("Never played"), N_("On repeat"), N_("Not heard in a while"),
                                               N_("Forgotten favourites"), N_("Often skipped")};
    return which > 0 && which < LIB_SMART_N ? N[which] : "";
}

int library_list_credit_tracks(library_t *lib, int64_t artist_id, int64_t album_id, lib_list_t *out)
{
    g_credit_album = album_id;
    int n = library_list(lib, LIB_CREDIT_TRACKS, artist_id, out);
    g_credit_album = 0;
    return n;
}

int library_artist_tracks(library_t *lib, int64_t artist_id, int credited, lib_list_t *out)
{
    memset(out, 0, sizeof *out);
    lib_list_t albums;
    if (library_list(lib, credited ? LIB_CREDIT_ALBUMS : LIB_ARTIST_ALBUMS, artist_id, &albums) < 0) return -1;
    for (int i = 0; i < albums.n; i++) {
        lib_list_t t;
        int n = credited ? library_list_credit_tracks(lib, artist_id, albums.ids[i], &t)
                         : library_list(lib, LIB_ALBUM_TRACKS, albums.ids[i], &t);
        if (n > 0) {
            int64_t *ni = realloc(out->ids, (size_t)(out->n + t.n) * sizeof *ni);
            if (ni) {
                out->ids = ni;
                memcpy(out->ids + out->n, t.ids, (size_t)t.n * sizeof *ni);
                out->n += t.n;
            }
        }
        library_list_free(&t);
    }
    library_list_free(&albums);
    out->view = credited ? LIB_CREDIT_TRACKS : LIB_ALBUM_TRACKS;
    return out->n;
}

static int64_t get_int_col(library_t *lib, const char *sql, int64_t id);

int library_needs_tags(library_t *lib)
{
    /* (tags_v<2 first: its index finds the few, or none, at once) */
    return get_int_col(lib, "SELECT EXISTS(SELECT 1 FROM tracks WHERE tags_v<2 AND (tags_v<1 OR (cue=0 AND"
                            " (path LIKE '%.flac' OR path LIKE '%.wv' OR path LIKE '%.ape'))))", 0) > 0;
}

int library_smart_count(library_t *lib, int which)
{
    /* as many as library_list(LIB_SMART, which) gives, without listing them */
    if (which == LIB_SMART_NEVER)
        return (int)get_int_col(lib, "SELECT COUNT(*) FROM tracks WHERE play_count=0 AND skip_count=0", 0);
    int64_t cut = 0;
    const char *sql = list_sql(LIB_SMART, which, &cut);
    if (!sql) return -1;
    char *q = sqlite3_mprintf("SELECT COUNT(*) FROM (%s)", sql);
    sqlite3_stmt *s = q ? prep(lib->db, q) : NULL;
    sqlite3_free(q);
    if (!s) return -1;
    if (cut) sqlite3_bind_int64(s, 1, cut);
    int n = sqlite3_step(s) == SQLITE_ROW ? sqlite3_column_int(s, 0) : -1;
    sqlite3_finalize(s);
    return n;
}

/* Bytes free where path is (-1: can't tell) */
static int64_t free_at(const char *path)
{
    struct statvfs v;
    if (!path || !*path || statvfs(path, &v)) return -1;
    return (int64_t)v.f_bavail * (int64_t)v.f_frsize;
}

int library_compact(library_t *lib)
{
    int64_t size = (int64_t)get_int_col(lib, "PRAGMA page_count", 0) * get_int_col(lib, "PRAGMA page_size", 0);
    const char *file = sqlite3_db_filename(lib->db, "main");
    char dir[1024];
    snprintf(dir, sizeof dir, "%s", file ? file : "");
    char *slash = strrchr(dir, '/');
    if (slash) *slash = '\0';
    else snprintf(dir, sizeof dir, ".");
    const char *tmp = getenv("SQLITE_TMPDIR");
    int64_t here = free_at(dir), there = tmp ? free_at(tmp) : here;
    int same = !tmp || !strcmp(tmp, dir);
    /* the log beside the database, the temporary copy where SQLite puts it (8 MB to spare) */
    int64_t spare = 8LL << 20;
    if (size <= 0 || here < 0 || there < 0 ||
        (same ? here < 2 * size + spare : (here < size + spare || there < size + spare))) {
        fprintf(stderr, "library: not compacted: %lld MB, %lld MB free\n", (long long)(size >> 20),
                (long long)(here >> 20));
        return -2;
    }
    double t0 = now_s();
    int rc = sqlite3_exec(lib->db, "VACUUM", NULL, NULL, NULL);
    double t1 = now_s();
    /* (VACUUM went through the log: the whole database. Back to nothing, or
     * it stays that size on the card.) */
    sqlite3_exec(lib->db, "PRAGMA wal_checkpoint(TRUNCATE)", NULL, NULL, NULL);
    int64_t after = (int64_t)get_int_col(lib, "PRAGMA page_count", 0) * get_int_col(lib, "PRAGMA page_size", 0);
    fprintf(stderr, "library: compacted in %.1f s, the log emptied in %.1f s: %lld -> %lld KB (%s)\n", t1 - t0,
            now_s() - t1, (long long)(size >> 10), (long long)(after >> 10),
            rc == SQLITE_OK ? "ok" : sqlite3_errmsg(lib->db));
    return rc == SQLITE_OK ? 0 : -1;
}

/* One query's plan: whether it reads the whole tracks table (a SCAN of it,
 * not through a covering index) or sorts its rows (a temp B-tree for ORDER BY
 * on a query that reads tracks). */
static int plan_bad(library_t *lib, const char *label, const char *sql, int allowed, FILE *report)
{
    char *q = sqlite3_mprintf("EXPLAIN QUERY PLAN %s", sql);
    sqlite3_stmt *s = q ? prep(lib->db, q) : NULL;
    sqlite3_free(q);
    if (!s) return 0;
    int scan = 0, sort = 0, tracks = strstr(sql, "tracks") != NULL;
    while (sqlite3_step(s) == SQLITE_ROW) {
        const char *d = (const char *)sqlite3_column_text(s, 3);
        if (!d) continue;
        if ((!strncmp(d, "SCAN tracks", 11) || !strncmp(d, "SCAN t", 6)) && !strstr(d, "COVERING INDEX")) scan = 1;
        if (tracks && strstr(d, "TEMP B-TREE FOR ORDER BY")) sort = 1;
    }
    sqlite3_finalize(s);
    int bad = (scan || sort) && !allowed;
    if (report && (scan || sort))
        fprintf(report, "%s%s: %s%s\n", allowed ? "(allowed) " : "", label, scan ? "reads the whole tracks table" : "",
                sort ? (scan ? ", sorts" : "sorts its rows") : "");
    return bad;
}

static const char *search_sql(const char *table, const char *where, char *sql, size_t cap);

int library_check_plans(library_t *lib, FILE *report)
{
    static const struct { lib_view_t v; int which; const char *name; int allowed; } V[] = {
        /* allowed (1): sorting what one artist, genre, composer or folder holds, or
         * On repeat's 50 at most, found through an index: a few hundred rows */
        {LIB_ALBUMS, 0, "Albums", 0}, {LIB_ALBUM_ARTISTS, 0, "Album artists", 0}, {LIB_ARTISTS, 0, "Artists", 0},
        {LIB_CREDIT_ALBUMS, 0, "an artist's albums (credited)", 1}, {LIB_CREDIT_TRACKS, 0, "an artist's songs on an album", 0},
        {LIB_ARTIST_ALBUMS, 0, "an album artist's albums", 0}, {LIB_ALBUM_TRACKS, 0, "an album's songs", 0},
        {LIB_SONGS, 0, "Songs", 0}, {LIB_PLAYLISTS, 0, "Playlists", 0}, {LIB_PLAYLIST_TRACKS, 0, "a playlist's songs", 0},
        {LIB_FAVOURITES, 0, "Favourites", 0}, {LIB_RECENT_PLAYED, 0, "History", 0}, {LIB_MOST_PLAYED, 0, "Most played", 0},
        /* (every album's newest file: one pass over a small covering index, then 4,200 groups sorted) */
        {LIB_RECENT_ADDED, 0, "Recently added", 1},
        {LIB_GENRES, 0, "Genres", 0}, {LIB_GENRE_ALBUMS, 0, "a genre's albums", 1}, {LIB_COMPOSERS, 0, "Composers", 0},
        {LIB_COMPOSER_ALBUMS, 0, "a composer's albums", 1}, {LIB_COMPOSER_TRACKS, 0, "a composer's songs on an album", 0},
        {LIB_YEARS, 0, "Years", 0}, {LIB_YEAR_ALBUMS, 0, "a year's albums", 0},
        /* (the whole list, in album order across a join: still sorted; its count isn't) */
        {LIB_SMART, LIB_SMART_NEVER, "Never played", 0},
        {LIB_SMART, LIB_SMART_REPEAT, "On repeat", 1}, {LIB_SMART, LIB_SMART_NOT_HEARD, "Not heard in a while", 0},
        {LIB_SMART, LIB_SMART_FORGOTTEN, "Forgotten favourites", 0}, {LIB_SMART, LIB_SMART_SKIPPED, "Often skipped", 0},
        {LIB_FOLDER, 0, "a folder", 1},
    };
    int bad = 0;
    for (size_t i = 0; i < sizeof V / sizeof V[0]; i++) {
        int64_t cut;
        const char *sql = list_sql(V[i].v, V[i].which, &cut);
        if (sql) bad += plan_bad(lib, V[i].name, sql, V[i].allowed, report);
    }
    bad += plan_bad(lib, "Never played's count", "SELECT COUNT(*) FROM tracks WHERE play_count=0 AND skip_count=0", 0, report);
    bad += plan_bad(lib, "Shuffle all's songs",
                    "SELECT id, artist_id, album_id, disc, track, duration_ms, last_played, play_count, skip_count, genre, title"
                    " FROM tracks ORDER BY album_id, disc, track", 0, report);
    bad += plan_bad(lib, "tags to read again",
                    "SELECT EXISTS(SELECT 1 FROM tracks WHERE tags_v<2 AND (tags_v<1 OR (cue=0 AND"
                    " (path LIKE '%.flac' OR path LIKE '%.wv' OR path LIKE '%.ape'))))", 0, report);
    bad += plan_bad(lib, "any songs", "SELECT NOT EXISTS(SELECT 1 FROM tracks)", 0, report);
    char sql[160];                                   /* (every key read, from their index) */
    bad += plan_bad(lib, "a search's songs", search_sql("tracks", "1", sql, sizeof sql), 0, report);
    return bad;
}

int64_t library_upgrade_needed(const char *db_path)
{
    sqlite3 *d = NULL;
    int64_t n = 0;
    if (sqlite3_open_v2(db_path, &d, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK) {
        sqlite3_busy_timeout(d, 2000);
        sqlite3_stmt *s = NULL;
        int v = 0;
        if (sqlite3_prepare_v2(d, "PRAGMA user_version", -1, &s, NULL) == SQLITE_OK && sqlite3_step(s) == SQLITE_ROW)
            v = sqlite3_column_int(s, 0);
        sqlite3_finalize(s);
        s = NULL;
        int waiting = 0;                           /* (a first scan cut short: its indexes to build) */
        if (sqlite3_prepare_v2(d, "SELECT COUNT(*) FROM bulk_idx", -1, &s, NULL) == SQLITE_OK && sqlite3_step(s) == SQLITE_ROW)
            waiting = sqlite3_column_int(s, 0) > 0;
        sqlite3_finalize(s);
        s = NULL;
        if (v > 0 && (v < LIB_SCHEMA_V || waiting) &&
            sqlite3_prepare_v2(d, "SELECT MAX(rowid) FROM tracks", -1, &s, NULL) == SQLITE_OK && sqlite3_step(s) == SQLITE_ROW)
            n = sqlite3_column_int64(s, 0);
        sqlite3_finalize(s);
    }
    sqlite3_close(d);
    return n;
}

int library_empty(library_t *lib)
{
    return get_int_col(lib, "SELECT NOT EXISTS(SELECT 1 FROM tracks)", 1) != 0;
}

int library_scan_unfinished(library_t *lib)
{
    return meta_get(lib->db, "scan_pending") > 0;
}

int library_needs_art(library_t *lib)
{
    /* (art=0: never looked at, a scan's cover pass cut short by a stop or a restart) */
    return get_int_col(lib, "SELECT EXISTS(SELECT 1 FROM albums WHERE art=0 OR (art=2 AND COALESCE(art_sig, 0)=0))", 0) > 0;
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

/* The music files a scan couldn't read: damaged ones, and those set aside
 * because the player stopped while reading them */
int library_unread_count(library_t *lib)
{
    return lib ? (int)get_int_col(lib, "SELECT COUNT(*) FROM cues WHERE kind=3", 0) : 0;
}

int library_unread(library_t *lib, int i, char *path, size_t cap)
{
    sqlite3_stmt *q = lib && i >= 0 ? prep(lib->db, "SELECT path FROM cues WHERE kind=3 ORDER BY path LIMIT 1 OFFSET ?") : NULL;
    int r = -1;
    if (!q) return -1;
    sqlite3_bind_int(q, 1, i);
    if (sqlite3_step(q) == SQLITE_ROW && sqlite3_column_text(q, 0)) {
        snprintf(path, cap, "%s", (const char *)sqlite3_column_text(q, 0));
        r = 0;
    }
    sqlite3_finalize(q);
    return r;
}

int library_scan_quick(const library_t *lib) { return lib ? lib->quick : 0; }
int library_scan_held(const library_t *lib) { return lib ? lib->scan_held : 0; }

/* ---- after a stop that wasn't clean ---- */

int library_check(library_t *lib)
{
    sqlite3_stmt *q = NULL;
    int ok = 0;
    double t0 = now_s();
    if (sqlite3_prepare_v2(lib->db, "PRAGMA quick_check(1)", -1, &q, NULL) == SQLITE_OK && sqlite3_step(q) == SQLITE_ROW) {
        const char *r = (const char *)sqlite3_column_text(q, 0);
        ok = r && !strcmp(r, "ok");
        if (!ok) fprintf(stderr, "library: damaged: %.200s\n", r ? r : "?");
    } else fprintf(stderr, "library: can't be checked: %s\n", sqlite3_errmsg(lib->db));
    sqlite3_finalize(q);
    fprintf(stderr, "library: checked in %.1f s: %s\n", now_s() - t0, ok ? "sound" : "damaged");
    return ok ? 0 : -1;
}

void library_scan_distrust(library_t *lib)
{
    sqlite3_exec(lib->db, "UPDATE meta SET value=0 WHERE key='scan_sig'", NULL, NULL, NULL);
}

/* (the old file may be damaged: read a stretch of ids at a time, so a page
 * that can't be read loses the songs on it, not everything after it; and
 * through a connection of its own: attached to the library's, the first
 * unreadable page made SQLite refuse the rest of the transaction, the
 * commit included) */
#define IMPORT_STEP 128

static sqlite3 *import_open(const char *old_db)
{
    sqlite3 *o = NULL;
    /* (to write as well: its log, if it has one, can't be read otherwise) */
    if (sqlite3_open_v2(old_db, &o, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK) {
        sqlite3_close(o);
        o = NULL;
        if (sqlite3_open_v2(old_db, &o, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) { sqlite3_close(o); return NULL; }
    }
    sqlite3_busy_timeout(o, 2000);
    return o;
}

static int64_t import_top(sqlite3 *o, const char *table, int64_t otherwise)
{
    char *sql = sqlite3_mprintf("SELECT MAX(rowid) FROM %s", table);
    sqlite3_stmt *q = NULL;
    int64_t top = otherwise;
    if (sql && sqlite3_prepare_v2(o, sql, -1, &q, NULL) == SQLITE_OK && sqlite3_step(q) == SQLITE_ROW &&
        sqlite3_column_type(q, 0) != SQLITE_NULL)
        top = sqlite3_column_int64(q, 0);
    sqlite3_finalize(q);
    sqlite3_free(sql);
    return top;
}

/* read's rows for ids from..from+IMPORT_STEP-1, each bound to write and run.
 * The rows written; *lost counted up if the stretch couldn't all be read. */
static int import_stretch(sqlite3 *o, const char *read, sqlite3_stmt *write, int64_t from, int *lost, sqlite3 *db)
{
    sqlite3_stmt *rd = NULL;
    int n = 0, r = SQLITE_ERROR;
    if (sqlite3_prepare_v2(o, read, -1, &rd, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(rd, 1, from);
        sqlite3_bind_int64(rd, 2, from + IMPORT_STEP - 1);
        while ((r = sqlite3_step(rd)) == SQLITE_ROW) {
            sqlite3_reset(write);
            for (int c = 0; c < sqlite3_column_count(rd); c++) sqlite3_bind_value(write, c + 1, sqlite3_column_value(rd, c));
            if (sqlite3_step(write) == SQLITE_DONE) n += sqlite3_changes(db);
            sqlite3_reset(write);
        }
    }
    sqlite3_finalize(rd);
    if (r != SQLITE_DONE) (*lost)++;
    return n;
}

int library_import_counts(library_t *lib, const char *old_db)
{
    sqlite3 *db = lib->db, *o = access(old_db, F_OK) == 0 ? import_open(old_db) : NULL;
    if (!o) { fprintf(stderr, "library: %s can't be read: nothing brought over\n", old_db); return -1; }
    int n = 0, lost = 0;
    /* (the highest id can't be read, the end of the table damaged: as far as
     * a library ten times this one's size would go) */
    int64_t mine = get_int_col(lib, "SELECT COALESCE(MAX(id), 0) FROM tracks", 0);
    int64_t top = import_top(o, "tracks", mine * 10 + 1000);
    sqlite3_stmt *up = prep(db,
        "UPDATE tracks SET play_count=?2, skip_count=?3, last_played=?4, favourite=?5, resume_ms=?6 WHERE path=?1");
    sqlite3_stmt *ins = prep(db, "INSERT INTO plays(track_id, at) SELECT id, ?2 FROM tracks WHERE path=?1");
    sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL);
    for (int64_t from = 0; up && from <= top; from += IMPORT_STEP)
        n += import_stretch(o,
            "SELECT path, play_count, skip_count, last_played, favourite, resume_ms FROM tracks NOT INDEXED"
            " WHERE id BETWEEN ?1 AND ?2 AND (play_count>0 OR skip_count>0 OR favourite=1 OR resume_ms>0)",
            up, from, &lost, db);
    top = import_top(o, "plays", 0);
    for (int64_t from = 0; ins && from <= top; from += IMPORT_STEP)
        import_stretch(o,
            "SELECT t.path, p.at FROM plays p NOT INDEXED JOIN tracks t ON t.id=p.track_id WHERE p.rowid BETWEEN ?1 AND ?2",
            ins, from, &lost, db);
    sqlite3_finalize(up);
    sqlite3_finalize(ins);
    reset_all(db);
    if (sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) {
        fprintf(stderr, "library: what was played couldn't be saved: %s\n", sqlite3_errmsg(db));
        if (!sqlite3_get_autocommit(db)) sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        n = -1;
    }
    sqlite3_close(o);
    fprintf(stderr, "library: plays, skips and favourites of %d songs brought over from %s%s\n", n < 0 ? 0 : n, old_db,
            lost ? " (parts of it couldn't be read)" : "");
    return n;
}

int library_scan_groups(const library_t *lib) { return lib ? lib->groups_how : 0; }

void library_test_rebuild_groups(library_t *lib)
{
    sqlite3_exec(lib->db, "BEGIN IMMEDIATE", NULL, NULL, NULL);
    rebuild_groups(lib);
    sqlite3_exec(lib->db, "COMMIT", NULL, NULL, NULL);
}

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
                    : (view == LIB_PLAYLIST_TRACKS || view == LIB_FAVOURITES || view == LIB_RECENT_PLAYED ||
                       view == LIB_SMART) ? lib->row_pl_track
                    : lib->row_album;   /* albums, artist albums, recently added */
    sqlite3_reset(s);
    sqlite3_bind_int64(s, 1, view == LIB_FOLDER ? -id : id);
    main[0] = meta[0] = '\0';
    if (sqlite3_step(s) != SQLITE_ROW) { sqlite3_reset(s); return -1; }
    copy_col(s, 0, main, main_cap);
    int v = sqlite3_column_int(s, 1);
    int track_list = view == LIB_ALBUM_TRACKS || view == LIB_SONGS || view == LIB_PLAYLIST_TRACKS || view == LIB_CREDIT_TRACKS ||
                     view == LIB_FAVOURITES || view == LIB_RECENT_PLAYED || view == LIB_MOST_PLAYED ||
                     view == LIB_COMPOSER_TRACKS || view == LIB_SMART;
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
        view == LIB_MOST_PLAYED || view == LIB_SMART) {   /* a mix of artists: "Title · Artist" */
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
    uint64_t h = fnv(14695981039346656037ULL, key, strlen(key));
    snprintf(out, cap, "%s/%016llx.%s", g_found_dir, (unsigned long long)h, ext);
    return 0;
}

int library_album_cover(library_t *lib, int64_t album_id)
{
    sqlite3_stmt *s = prep(lib->db, "SELECT art FROM albums WHERE id=?");
    if (!s) return LIB_COVER_UNKNOWN;
    sqlite3_bind_int64(s, 1, album_id);
    int art = sqlite3_step(s) == SQLITE_ROW ? sqlite3_column_int(s, 0) : LIB_COVER_UNKNOWN;
    sqlite3_finalize(s);
    return art;
}

#define ALBUM_INFO_SQL "SELECT al.id, al.key, al.title, al.year, ar.name FROM albums al" \
                       " LEFT JOIN artists ar ON ar.id=al.artist_id"

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
    sqlite3_stmt *s = prep(lib->db, ALBUM_INFO_SQL " WHERE al.id=?");
    if (!s) return -1;
    sqlite3_bind_int64(s, 1, album_id);
    int ok = sqlite3_step(s) == SQLITE_ROW;
    if (ok) album_info_row(s, out);
    sqlite3_finalize(s);
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
        if (n == cap) {
            cap = cap ? cap * 2 : 64;
            lib_album_t *nv = realloc(v, (size_t)cap * sizeof *v);
            if (!nv) { free(v); sqlite3_finalize(s); return -1; }
            v = nv;
        }
        album_info_row(s, &v[n]);
        if (v[n].title[0]) n++;            /* (no album title: nothing to look for) */
    }
    sqlite3_finalize(s);
    *out = v;
    return n;
}

static int set_cover_state(library_t *lib, int64_t album_id, int art)
{
    sqlite3_stmt *s = prep(lib->db, "UPDATE albums SET art=? WHERE id=?");
    if (!s) return -1;
    sqlite3_bind_int(s, 1, art);
    sqlite3_bind_int64(s, 2, album_id);
    int rc = sqlite3_step(s) == SQLITE_DONE ? 0 : -1;
    sqlite3_finalize(s);
    return rc;
}

int library_store_found_cover(library_t *lib, int64_t album_id, const uint16_t *large, const uint16_t *small)
{
    lib_album_t a;
    char over[1200];
    if (library_album_info(lib, album_id, &a) || library_found_cover_path(a.key, "over", over, sizeof over)) return -1;
    int cur = library_album_cover(lib, album_id);
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
    if (library_album_info(lib, album_id, &a)) return -1;
    int picked = library_album_cover(lib, album_id) == LIB_COVER_PICKED;
    if (library_found_cover_path(a.key, "jpg", path, sizeof path) == 0) remove(path);
    if (library_found_cover_path(a.key, "over", path, sizeof path) == 0) remove(path);
    if (picked) {                                    /* back to its own cover, made again from where it is */
        sqlite3_stmt *q = prep(lib->db, "SELECT al.dir, t.path FROM albums al LEFT JOIN tracks t ON t.id=al.first_track"
                                        " WHERE al.id=?");
        sqlite3_stmt *others = prep(lib->db,
            "SELECT path FROM tracks WHERE album_id=? AND has_pic=1 AND path<>? ORDER BY disc, track LIMIT 4");
        char dir[1024] = "", first[1024] = "";
        int have = 0;
        if (q) {
            sqlite3_bind_int64(q, 1, album_id);
            if (sqlite3_step(q) == SQLITE_ROW) {
                copy_col(q, 0, dir, sizeof dir);
                have = sqlite3_column_type(q, 1) != SQLITE_NULL;
                copy_col(q, 1, first, sizeof first);
            }
            sqlite3_finalize(q);
        }
        int r = make_art(lib, others, album_id, dir, have ? first : NULL, a.key);
        reader_close();                            /* (one album's: no helper left waiting; a scan under way starts its own again) */
        sqlite3_finalize(others);
        if (r >= 0) return set_cover_state(lib, album_id, r == 0 ? LIB_COVER_OWN : LIB_COVER_FOUND);
        /* (its own is gone meanwhile: no cover, as below) */
    }
    if (library_found_cover_path(a.key, "none", path, sizeof path) == 0) {
        FILE *f = fopen(path, "we");
        if (f) fclose(f);
    }
    art_del(lib, album_id);
    return set_cover_state(lib, album_id, LIB_COVER_NONE);
}

#define SHUF_COLS "id, artist_id, album_id, disc, track, duration_ms, last_played, play_count, skip_count, genre, title"

/* s's row (SHUF_COLS) in shuffle form; a work's title prefix hashed into unit for shuf_works */
static void shuf_row(sqlite3_stmt *s, shuf_track_t *t)
{
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
     * title before ": ", for shuf_works (kept in unit for now) */
    const char *ti = (const char *)sqlite3_column_text(s, 10);
    const char *c = ti ? strstr(ti, ": ") : NULL;
    if (c && c - ti >= 4) {
        uint32_t h = 2166136261u;
        for (const char *q = ti; q < c; q++) h = (h ^ (unsigned char)*q) * 16777619u;
        t->unit = h ? h : 1;
    }
}

/* Works: consecutive tracks of an album with the same title prefix, two or
 * more of them, share a unit (shuffle keeps them together when asked to);
 * a prefix on its own is no work. t is in album, disc, track order. */
static void shuf_works(shuf_track_t *t, int n)
{
    for (int i = 0; i < n;) {
        int j = i + 1;
        while (j < n && t[i].unit && t[j].unit == t[i].unit && t[j].album == t[i].album) j++;
        uint32_t u = j - i >= 2 ? (t[i].id ? t[i].id : 1) : 0;   /* the first part's id: unique */
        for (int k = i; k < j; k++) t[k].unit = u;
        i = j;
    }
}

int library_shuffle_tracks(library_t *lib, shuf_track_t **out)
{
    sqlite3_stmt *s = prep(lib->db, "SELECT " SHUF_COLS " FROM tracks ORDER BY album_id, disc, track");
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
        shuf_row(s, &(*out)[n++]);
    }
    sqlite3_finalize(s);
    shuf_works(*out, n);
    return n;
}

int64_t library_top_track(library_t *lib) { return get_int_col(lib, "SELECT COALESCE(MAX(id), 0) FROM tracks", 0); }
int64_t library_track_from(library_t *lib, int64_t id)
{
    sqlite3_stmt *s = prep(lib->db, "SELECT id FROM tracks WHERE id>=? ORDER BY id LIMIT 1");
    int64_t at = 0;
    if (s) {
        sqlite3_bind_int64(s, 1, id);
        if (sqlite3_step(s) == SQLITE_ROW) at = sqlite3_column_int64(s, 0);
    }
    sqlite3_finalize(s);
    return at;
}

typedef struct { shuf_track_t t; int at; } shuf_at_t;

static int cmp_shuf_album(const void *a, const void *b)
{
    const shuf_track_t *x = &((const shuf_at_t *)a)->t, *y = &((const shuf_at_t *)b)->t;
    if (x->album != y->album) return x->album < y->album ? -1 : 1;
    if (x->disc != y->disc) return x->disc < y->disc ? -1 : 1;
    if (x->track != y->track) return x->track < y->track ? -1 : 1;
    return x->id < y->id ? -1 : x->id > y->id;
}

int library_shuffle_tracks_for(library_t *lib, const int64_t *ids, int n, shuf_track_t *out)
{
    sqlite3_stmt *s = prep(lib->db, "SELECT " SHUF_COLS " FROM tracks WHERE id=?");
    shuf_at_t *w = n > 0 ? malloc((size_t)n * sizeof *w) : NULL;
    if (!s || !w) { sqlite3_finalize(s); free(w); return -1; }
    for (int i = 0; i < n; i++) {
        sqlite3_reset(s);
        sqlite3_bind_int64(s, 1, ids[i]);
        if (sqlite3_step(s) == SQLITE_ROW) shuf_row(s, &w[i].t);
        else { memset(&w[i].t, 0, sizeof w[i].t); w[i].t.id = (uint32_t)ids[i]; }   /* (gone: as a song nothing is known of) */
        w[i].at = i;
    }
    sqlite3_finalize(s);
    /* works, as library_shuffle_tracks finds them: among these songs, in album order */
    qsort(w, (size_t)n, sizeof *w, cmp_shuf_album);
    for (int i = 0; i < n;) {
        int j = i + 1;
        while (j < n && w[i].t.unit && w[j].t.unit == w[i].t.unit && w[j].t.album == w[i].t.album) j++;
        uint32_t u = j - i >= 2 ? (w[i].t.id ? w[i].t.id : 1) : 0;
        for (int k = i; k < j; k++) w[k].t.unit = u;
        i = j;
    }
    for (int i = 0; i < n; i++) out[w[i].at] = w[i].t;
    free(w);
    return n;
}

void library_played(library_t *lib, int64_t track_id, int skipped_early, int64_t now)
{
    /* a play: counted, and logged (On repeat), the log kept to a year. One
     * commit for the lot (they were three, and the year-old plays were looked
     * for at every track's end: once since the library was opened is enough). */
    sqlite3 *db = lib->db;
    int tx = !skipped_early && sqlite3_get_autocommit(db) && sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) == SQLITE_OK;
    sqlite3_stmt *s = prep(db, skipped_early
        ? "UPDATE tracks SET skip_count=skip_count+1 WHERE id=?2"
        : "UPDATE tracks SET play_count=play_count+1, last_played=?1 WHERE id=?2");
    if (s) {
        sqlite3_bind_int64(s, 1, now);
        sqlite3_bind_int64(s, 2, track_id);
        sqlite3_step(s);
        sqlite3_finalize(s);
    }
    if (s && !skipped_early) {
        s = prep(db, "INSERT INTO plays(track_id, at) VALUES(?2, ?1)");
        if (s) {
            sqlite3_bind_int64(s, 1, now);
            sqlite3_bind_int64(s, 2, track_id);
            sqlite3_step(s);
            sqlite3_finalize(s);
        }
        if (!lib->plays_trimmed) {
            lib->plays_trimmed = 1;
            s = prep(db, "DELETE FROM plays WHERE at<?1");
            if (s) {
                sqlite3_bind_int64(s, 1, now - 366 * 86400);
                sqlite3_step(s);
                sqlite3_finalize(s);
            }
        }
    }
    if (tx && sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK && !sqlite3_get_autocommit(db))
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
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
    if (!ok || file_replace(tmp, path)) { unlink(tmp); return -1; }
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

/* One kind's names that what was typed finds, each with how close it is
 * (tier_of_key), the closest first and then by name (a leading The passed
 * over): up to LIB_SEARCH_MAX of them, added to hits. Every key is read (the
 * songs' from their index, tracks_search: library_check_plans) and given to
 * search_keep(), which keeps the closest so far: the whole pass in one
 * sqlite3_step (a step a row took the H2 twice as long: 0.55 s for 50,000
 * songs, where this takes a quarter of a second whatever is typed). */
static const char *search_sql(const char *table, const char *where, char *sql, size_t cap)
{
    snprintf(sql, cap, "SELECT count(search_keep(id, search)) FROM %s WHERE %s", table, where);
    return sql;
}

typedef struct { int64_t id; int tier, len; char ord[56]; } found_t;
typedef struct {
    const char *words[8];
    int nw, last_whole, n;
    found_t top[LIB_SEARCH_MAX];
} search_run_t;

/* SQL search_keep(id, key): the search under way (the library's), told of a
 * name. */
static void sql_search_keep(sqlite3_context *ctx, int argc, sqlite3_value **argv)
{
    search_run_t *r = *(search_run_t **)sqlite3_user_data(ctx);
    const char *key = argc == 2 ? (const char *)sqlite3_value_text(argv[1]) : NULL;
    int tier = r ? tier_of_key(key, r->words, r->nw, r->last_whole) : 0;
    if (!tier) return;
    const char *e = strchr(key, SKEY_SEP);
    if (!e) e = key + strlen(key);
    if (e - key > 4 && !memcmp(key, "the ", 4)) key += 4;
    int len = (int)(e - key) < (int)sizeof r->top->ord ? (int)(e - key) : (int)sizeof r->top->ord;
    /* where it goes among the closest so far (after the ones as close and
     * named the same: the first read stays first) */
    int at = r->n;
    while (at > 0) {
        const found_t *f = &r->top[at - 1];
        int c = f->tier - tier;
        if (!c) c = memcmp(f->ord, key, (size_t)(f->len < len ? f->len : len));
        if (!c) c = f->len - len;
        if (c <= 0) break;
        at--;
    }
    if (at == LIB_SEARCH_MAX) return;
    if (r->n < LIB_SEARCH_MAX) r->n++;
    memmove(&r->top[at + 1], &r->top[at], (size_t)(r->n - 1 - at) * sizeof *r->top);
    r->top[at].id = sqlite3_value_int64(argv[0]);
    r->top[at].tier = tier;
    r->top[at].len = len;
    memcpy(r->top[at].ord, key, (size_t)len);
}

static int search_kind_rows(library_t *lib, int kind, const char *table, const char *where, char words[][64], int nw,
                            int last_whole, lib_hit_t *hits, int n)
{
    char sql[160];
    search_run_t *r = calloc(1, sizeof *r);
    sqlite3_stmt *s = r ? prep(lib->db, search_sql(table, where, sql, sizeof sql)) : NULL;
    if (!s) { free(r); return n; }
    for (int i = 0; i < nw; i++) r->words[i] = words[i];
    r->nw = nw;
    r->last_whole = last_whole;
    lib->search_run = r;
    sqlite3_step(s);
    lib->search_run = NULL;
    sqlite3_finalize(s);
    for (int i = 0; i < r->n && n < 3 * LIB_SEARCH_MAX; i++) hits[n++] = (lib_hit_t){kind, r->top[i].id, r->top[i].tier};
    free(r);
    return n;
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
    size_t ql = strlen(query);
    int last_whole = ql > 0 && query[ql - 1] == ' ';         /* (a space after it: the word is finished) */
    lib_hit_t *all = malloc(3 * LIB_SEARCH_MAX * sizeof *all);
    if (!all) return 0;
    int n = 0;
    n = search_kind_rows(lib, LIB_HIT_ARTIST, "artists", "(nalbums>0 OR ncredit_albums>0)", words, nw, last_whole, all, n);
    n = search_kind_rows(lib, LIB_HIT_ALBUM, "albums", "1", words, nw, last_whole, all, n);
    n = search_kind_rows(lib, LIB_HIT_SONG, "tracks", "1", words, nw, last_whole, all, n);
    int close = 0;
    for (int i = 0; i < n; i++) close += all[i].tier <= 4;
    int top = close >= LIB_SEARCH_FEW ? 4 : 6;               /* the looser ones only when few are close */
    out->hits = malloc(LIB_SEARCH_MAX * sizeof *out->hits);
    out->songs = malloc(LIB_SEARCH_MAX * sizeof *out->songs);
    if (!out->hits || !out->songs) { free(all); library_search_free(out); return 0; }
    /* by tier, then artists, albums, songs, each in the order it came (its list's) */
    for (int tier = 1; tier <= top && out->n < LIB_SEARCH_MAX; tier++)
        for (int i = 0; i < n && out->n < LIB_SEARCH_MAX; i++) {
            if (all[i].tier != tier) continue;
            out->hits[out->n++] = all[i];
            if (all[i].kind == LIB_HIT_ARTIST) out->n_artists++;
            else if (all[i].kind == LIB_HIT_ALBUM) out->n_albums++;
            else out->songs[out->n_songs++] = all[i].id;
        }
    free(all);
    return out->n;
}

void library_search_free(lib_search_t *r)
{
    free(r->hits);
    free(r->songs);
    memset(r, 0, sizeof *r);
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
    int nice = lock_boost();
    sqlite3_step(s);
    lock_unboost(nice);
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
