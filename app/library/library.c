/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE
#define _GNU_SOURCE   /* strcasestr (glibc, musl) */
#include "library.h"

#include <dirent.h>
#include <errno.h>
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

#include "library/cue.h"
#include "library/fatwalk.h"
#include "library/keys.h"
#include "library/playlist.h"
#include "library/tags.h"
#include "stb/stb_image.h"

#ifdef FATHOM_HAVE_LIBJPEG
#include <setjmp.h>
#include <jpeglib.h>
#endif

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
    char art_dir[1024];
    sqlite3_stmt *row_album, *row_artist, *row_credited, *row_track, *track_full, *album_name, *artist_name;
    sqlite3_stmt *row_playlist, *row_pl_track, *playlist_name;
    sqlite3_stmt *row_genre, *row_composer, *row_year, *row_folder, *genre_name, *composer_name, *folder_name;
    char pl_dir[1024];      /* your own playlists ("" = none) */
    int scan_changes;       /* tracks the last scan added, changed or dropped */
    volatile int cancel;    /* library_scan_cancel(): stop the scan in progress */
    char fw_dev[64], fw_mount[256];   /* library_set_fast_walk(): the music folder's FAT partition */
};

static const char *SCHEMA =
    "PRAGMA journal_mode=WAL;"
    /* WAL with NORMAL: no flush to the card at every commit (a play count, a
     * favourite); a power cut can lose the last commits, never the database */
    "PRAGMA synchronous=NORMAL;"
    "PRAGMA cache_size=" FATHOM_SQLITE_CACHE ";"
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
    if (rc != SQLITE_OK || sqlite3_exec(lib->db, SCHEMA, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "library: cannot open %s: %s\n", db_path, err ? err : sqlite3_errmsg(lib->db));
        sqlite3_free(err);
        library_close(lib);
        return NULL;
    }
    /* columns added after the first release of the schema (errors: already there) */
    sqlite3_exec(lib->db, "ALTER TABLE tracks ADD COLUMN favourite INTEGER DEFAULT 0", NULL, NULL, NULL);
    sqlite3_exec(lib->db, "ALTER TABLE tracks ADD COLUMN resume_ms INTEGER DEFAULT 0", NULL, NULL, NULL);
    sqlite3_exec(lib->db, "ALTER TABLE playlists ADD COLUMN writable INTEGER DEFAULT 0", NULL, NULL, NULL);
    sqlite3_exec(lib->db, "ALTER TABLE playlists ADD COLUMN mtime INTEGER", NULL, NULL, NULL);   /* (its file's, when read) */
    sqlite3_exec(lib->db, "ALTER TABLE playlists ADD COLUMN size INTEGER", NULL, NULL, NULL);
    sqlite3_exec(lib->db, "ALTER TABLE tracks ADD COLUMN search TEXT", NULL, NULL, NULL);
    sqlite3_exec(lib->db, "ALTER TABLE albums ADD COLUMN search TEXT", NULL, NULL, NULL);
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
    sqlite3_create_function(lib->db, "fold", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, NULL, sql_fold, NULL, NULL);
    /* how names sort: albums (without "the"), and songs (as titled) */
    static int strip_the = 1;
    sqlite3_create_function(lib->db, "sortkey", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, &strip_the, sql_sortkey, NULL, NULL);
    sqlite3_create_function(lib->db, "songkey", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, NULL, sql_sortkey, NULL, NULL);
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
     * in its own tags (kind 2). */
    sqlite3_exec(lib->db, "ALTER TABLE tracks ADD COLUMN cue INTEGER DEFAULT 0", NULL, NULL, NULL);
    sqlite3_exec(lib->db,
        "CREATE TABLE IF NOT EXISTS cues(path TEXT PRIMARY KEY, mtime INTEGER, size INTEGER, kind INTEGER,"
        " sheet TEXT, nfiles INTEGER DEFAULT 0, seen INTEGER DEFAULT 1);"
        "CREATE INDEX IF NOT EXISTS cues_sheet ON cues(sheet);",
        NULL, NULL, NULL);
    int had_composer = sqlite3_exec(lib->db, "ALTER TABLE tracks ADD COLUMN composer TEXT", NULL, NULL, NULL) != SQLITE_OK;
    if (!had_composer) {   /* a library from before Genres, Folders...: their groups now */
        sqlite3_exec(lib->db, "BEGIN", NULL, NULL, NULL);
        rebuild_groups(lib);
        sqlite3_exec(lib->db, "COMMIT", NULL, NULL, NULL);
    }
    if (!had_credits) {   /* a library from before credits: work them out once (no rescan) */
        sqlite3_exec(lib->db, "BEGIN", NULL, NULL, NULL);
        rebuild_groups(lib);
        sqlite3_exec(lib->db, "COMMIT", NULL, NULL, NULL);
    }
    index_search(lib, 1);
    lib->row_album = prep(lib->db, "SELECT title, year FROM albums WHERE id=?");
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
    lib->playlist_name = prep(lib->db, "SELECT name FROM playlists WHERE id=?");
    lib->row_genre = prep(lib->db, "SELECT name, nalbums FROM genres WHERE id=?");
    lib->row_composer = prep(lib->db, "SELECT name, nalbums FROM composers WHERE id=?");
    lib->row_year = prep(lib->db, "SELECT ?1, COUNT(*) FROM albums WHERE year=?1");
    lib->row_folder = prep(lib->db, "SELECT name, ntracks FROM folders WHERE id=?");
    lib->genre_name = prep(lib->db, "SELECT name FROM genres WHERE id=?");
    lib->composer_name = prep(lib->db, "SELECT name FROM composers WHERE id=?");
    lib->folder_name = prep(lib->db, "SELECT name FROM folders WHERE id=?");
    if (!lib->row_playlist || !lib->row_pl_track || !lib->playlist_name ||
        !lib->row_album || !lib->row_artist || !lib->row_credited || !lib->row_track || !lib->track_full ||
        !lib->album_name || !lib->artist_name || !lib->row_genre || !lib->row_composer || !lib->row_year ||
        !lib->row_folder || !lib->genre_name || !lib->composer_name || !lib->folder_name) {
        library_close(lib);
        return NULL;
    }
    return lib;
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
} pent_t;
typedef struct { pent_t *v; int n, cap; } pathvec_t;

static int pv_push(pathvec_t *pv, const char *p, const struct stat *st)
{
    if (pv->n == pv->cap) {
        int nc = pv->cap ? pv->cap * 2 : 1024;
        pent_t *nv = realloc(pv->v, (size_t)nc * sizeof *nv);
        if (!nv) return -1;
        pv->v = nv;
        pv->cap = nc;
    }
    pv->v[pv->n].p = strdup(p);
    pv->v[pv->n].mtime = st ? (int64_t)st->st_mtime : 0;
    pv->v[pv->n].size = st ? (int64_t)st->st_size : 0;
    pv->v[pv->n].cue = -1;
    return pv->v[pv->n].p ? (pv->n++, 0) : -1;
}

static void pv_free(pathvec_t *pv)
{
    for (int i = 0; i < pv->n; i++) free(pv->v[i].p);
    free(pv->v);
    memset(pv, 0, sizeof *pv);
}

static void walk(const char *dir, pathvec_t *pv, pathvec_t *plv, pathvec_t *cv, int depth, const volatile int *cancel)
{
    if (depth > 16 || *cancel) return;
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    char path[2048];
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        /* the listing says what's a folder: only music, playlists and CUE sheets are looked up */
        int music = tags_supported(path), list = !music && m3u_supported(path), cue = !music && cue_supported(path);
        if (e->d_type != DT_UNKNOWN && e->d_type != DT_DIR && !music && !list && !cue) continue;
        struct stat st;
        if (stat(path, &st)) continue;
        if (S_ISDIR(st.st_mode)) walk(path, pv, plv, cv, depth + 1, cancel);
        else if (S_ISREG(st.st_mode) && music) pv_push(pv, path, &st);
        else if (S_ISREG(st.st_mode) && list) pv_push(plv, path, &st);
        else if (S_ISREG(st.st_mode) && cue) pv_push(cv, path, &st);
    }
    closedir(d);
}

static int cmp_pent(const void *a, const void *b) { return strcmp(((const pent_t *)a)->p, ((const pent_t *)b)->p); }

/* ---------- the fast walk ----------
 * Listing the music folder through the kernel costs ~20 s on the H2's card;
 * reading its FAT directly (fatwalk) a few. Its list is used only when it's
 * exactly what the library holds already (nothing changed: the usual case);
 * any difference and the folder is walked as usual. */
typedef struct { pathvec_t *pv, *plv, *cv; const volatile int *cancel; } fw_ctx_t;

static int fw_file(const char *path, int64_t mtime, int64_t size, void *ctx)
{
    fw_ctx_t *c = ctx;
    struct stat st = {0};
    st.st_mtime = (time_t)mtime;
    st.st_size = (off_t)size;
    if (tags_supported(path)) pv_push(c->pv, path, &st);
    else if (m3u_supported(path)) pv_push(c->plv, path, &st);
    else if (cue_supported(path)) pv_push(c->cv, path, &st);
    return *c->cancel;
}

/* The music files in `pv` and sheets in `cv` (both sorted) are exactly the
 * library's files (its tracks', and those in `cues`), dates and sizes. */
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
        if (!e || !p || strcmp(p, e->p) || sqlite3_column_int64(s, 1) != e->mtime || sqlite3_column_int64(s, 2) != e->size)
            same = 0;
    }
    sqlite3_finalize(s);
    return same && i == pv->n && j == cv->n;
}

void library_set_fast_walk(library_t *lib, const char *dev, const char *mount)
{
    snprintf(lib->fw_dev, sizeof lib->fw_dev, "%s", dev ? dev : "");
    snprintf(lib->fw_mount, sizeof lib->fw_mount, "%s", mount ? mount : "");
}

/* The fast walk of `root`, if it's set up and the result is the library as it
 * is: 1 (pv, plv filled, sorted), else 0 (and they're left empty). */
static int fast_walk(library_t *lib, const char *root, pathvec_t *pv, pathvec_t *plv, pathvec_t *cv)
{
    size_t ml = strlen(lib->fw_mount);
    if (!lib->fw_dev[0] || !ml || strncmp(root, lib->fw_mount, ml) || (root[ml] != '/' && root[ml] != '\0'))
        return 0;
    const char *rel = root[ml] == '/' ? root + ml + 1 : "";
    sync();   /* fatwalk reads the partition itself: nothing may wait in the cache */
    fw_ctx_t c = {pv, plv, cv, &lib->cancel};
    if (fatwalk(lib->fw_dev, lib->fw_mount, rel, fw_file, &c) == 0 && !lib->cancel) {
        qsort(pv->v, (size_t)pv->n, sizeof *pv->v, cmp_pent);
        qsort(cv->v, (size_t)cv->n, sizeof *cv->v, cmp_pent);
        if (same_as_library(lib, pv, cv)) return 1;
    }
    pv_free(pv);
    pv_free(plv);
    pv_free(cv);
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
        "DELETE FROM composers WHERE nalbums=0;",
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
    sqlite3_stmt *uc = prep(db, "INSERT OR IGNORE INTO credits(artist_id, track_id) VALUES(?, ?)");
    upsert_t u_art = {0}, u_alb = {0};
    if (!q || !ut || !ua || !ur || !uc || upsert_prep(db, "artists", &u_art) || upsert_prep(db, "albums", &u_alb)) goto out;

    sqlite3_exec(db, "UPDATE albums SET year=0, dir=NULL, first_track=NULL;"
                     "UPDATE tracks SET sort=songkey(COALESCE(title,'')) WHERE sort IS NOT songkey(COALESCE(title,''));",
                 NULL, NULL, NULL);
    sqlite3_exec(db, "DELETE FROM credits", NULL, NULL, NULL);
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

        sqlite3_reset(ur);
        sqlite3_bind_text(ur, 1, key_is_various(aa) ? "Various Artists" : album_artist_name, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(ur, 2, aakey, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(ur, 3, album_artist);
        sqlite3_step(ur);
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
    sqlite3_exec(db,
        "UPDATE albums SET ntracks=(SELECT COUNT(*) FROM tracks WHERE album_id=albums.id);"
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
        "DELETE FROM genres WHERE nalbums=0;",
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
    sqlite3_finalize(uc);
    upsert_free(&u_art);
    upsert_free(&u_alb);
}

/* ---------- cover art ---------- */

static unsigned char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *b = (n > 0 && n < 32 * 1024 * 1024) ? malloc((size_t)n) : NULL;
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    fclose(f);
    *len = b ? (size_t)n : 0;
    return b;
}

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

#ifdef FATHOM_HAVE_LIBJPEG
/*
 * JPEG covers through libjpeg(-turbo), which scales by 1/2, 1/4 or 1/8 while
 * decoding: a 3000x3000 cover decodes straight to 375x375 (0.4 MB instead of
 * 27 MB), and much faster. Picks the smallest output that still has at least
 * `min_side` pixels on its short side. Returns an RGB888 buffer (free()).
 */
typedef struct { struct jpeg_error_mgr pub; jmp_buf jb; } jerr_t;
static void jpeg_fail(j_common_ptr c) { longjmp(((jerr_t *)c->err)->jb, 1); }

static unsigned char *decode_jpeg_scaled(const unsigned char *enc, size_t len, FILE *fp, int min_side, int *w, int *h)
{
    struct jpeg_decompress_struct c;
    jerr_t err;
    unsigned char *volatile img = NULL;
    c.err = jpeg_std_error(&err.pub);
    err.pub.error_exit = jpeg_fail;
    if (setjmp(err.jb)) {
        jpeg_destroy_decompress(&c);
        free(img);
        return NULL;
    }
    jpeg_create_decompress(&c);
    /* Progressive JPEGs keep every DCT coefficient of the full image in memory
     * whatever the output scale (about 12 MB for a 2000x2000 cover). Cap it, so
     * libjpeg fails cleanly instead of the kernel killing the player. */
    c.mem->max_memory_to_use = FATHOM_JPEG_MAX_MEMORY;
    if (fp) jpeg_stdio_src(&c, fp);   /* streamed: the encoded file is never held in RAM */
    else jpeg_mem_src(&c, enc, (unsigned long)len);
    if (jpeg_read_header(&c, TRUE) != JPEG_HEADER_OK) { jpeg_destroy_decompress(&c); return NULL; }
    unsigned short_side = c.image_width < c.image_height ? c.image_width : c.image_height;
    unsigned denom = 1;
    while (denom < 8 && short_side / (denom * 2) >= (unsigned)min_side) denom *= 2;
    c.scale_num = 1;
    c.scale_denom = denom;
    c.out_color_space = JCS_RGB;
    c.dct_method = JDCT_IFAST;
    jpeg_start_decompress(&c);
    *w = (int)c.output_width;
    *h = (int)c.output_height;
    if ((long long)*w * *h > FATHOM_ART_MAX_PIXELS) { jpeg_destroy_decompress(&c); return NULL; }
    img = malloc((size_t)*w * *h * 3);
    if (!img) { jpeg_destroy_decompress(&c); return NULL; }
    while (c.output_scanline < c.output_height) {
        JSAMPROW row = img + (size_t)c.output_scanline * *w * 3;
        jpeg_read_scanlines(&c, &row, 1);
    }
    jpeg_finish_decompress(&c);
    jpeg_destroy_decompress(&c);
    return img;
}
#endif

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

int library_art_decode(const unsigned char *enc, size_t len, int size, uint16_t *pixels)
{
    int w = 0, h = 0, ch;
    unsigned char *img = NULL;
    if (!enc || len < 4) return -1;
#ifdef FATHOM_HAVE_LIBJPEG
    if (enc[0] == 0xFF && enc[1] == 0xD8) img = decode_jpeg_scaled(enc, len, NULL, size * 3 / 2, &w, &h);
#endif
    if (!img) {
        if (!stbi_info_from_memory(enc, (int)len, &w, &h, &ch) || (long long)w * h > FATHOM_ART_MAX_PIXELS) return -1;
        img = stbi_load_from_memory(enc, (int)len, &w, &h, &ch, 3);
    }
    if (!img) return -1;
    int side = w < h ? w : h;
    const unsigned char *crop = img + 3 * ((size_t)((h - side) / 2) * w + (w - side) / 2);
    unsigned char *rs = malloc((size_t)size * size * 3);
    if (rs) {
        area_downscale(crop, side, w * 3, rs, size);
        to_rgb565(rs, size, pixels);
    }
    free(rs);
    free(img);
    return rs ? 0 : -1;
}

static int make_art(library_t *lib, int64_t album_id, const char *dir, const char *first_track)
{
    char file[2048];
    unsigned char *enc = NULL;
    size_t enc_len = 0;
    int w = 0, h = 0, ch;
    unsigned char *img = NULL;
    int rank = find_cover_file(dir, file, sizeof file);
    int have_file = rank >= 0;
    /* A file that isn't named like a cover (a scan, a booklet page, a photo)
     * loses to a front cover embedded in the music file. */
    if (rank > 4 && first_track && tags_picture(first_track, &enc, &enc_len) == 0) have_file = 0;
#ifdef FATHOM_HAVE_LIBJPEG
    const char *dot = have_file ? strrchr(file, '.') : NULL;
    if (dot && (!strcasecmp(dot, ".jpg") || !strcasecmp(dot, ".jpeg"))) {
        FILE *fp = fopen(file, "rb");
        if (fp) {
            img = decode_jpeg_scaled(NULL, 0, fp, LIB_ART_LARGE * 3 / 2, &w, &h);
            fclose(fp);
        }
    }
#endif
    if (!img) {
        struct stat st;
        /* Anything else is decoded from memory: skip huge files (scans, photos). */
        if (!enc && have_file && stat(file, &st) == 0 && st.st_size <= FATHOM_ART_MAX_FILE) enc = slurp(file, &enc_len);
        if (!enc && first_track) tags_picture(first_track, &enc, &enc_len);
        if (!enc) return -1;
    }
#ifdef FATHOM_HAVE_LIBJPEG
    if (!img && enc_len > 3 && enc[0] == 0xFF && enc[1] == 0xD8)
        img = decode_jpeg_scaled(enc, enc_len, NULL, LIB_ART_LARGE * 3 / 2, &w, &h);
#endif
    if (!img && enc) {
        /* stb_image decodes at full size: cap the pixel count so a huge cover
         * can't exhaust the H2's RAM (a 3000x3000 image is 27 MB decoded). */
        if (!stbi_info_from_memory(enc, (int)enc_len, &w, &h, &ch) ||
            (long long)w * h > FATHOM_ART_MAX_PIXELS) {
            free(enc);
            return -1;
        }
        img = stbi_load_from_memory(enc, (int)enc_len, &w, &h, &ch, 3);
    }
    free(enc);
    if (!img) return -1;
    /* centre-crop to a square */
    int side = w < h ? w : h;
    const unsigned char *crop = img + 3 * ((size_t)((h - side) / 2) * w + (w - side) / 2);

    const int sizes[2] = {LIB_ART_LARGE, LIB_ART_SMALL};
    int ok = 0;
    for (int i = 0; i < 2; i++) {
        int s = sizes[i];
        unsigned char *rs = malloc((size_t)s * s * 3);
        uint16_t *px = malloc((size_t)s * s * 2);
        if (rs && px) {
            area_downscale(crop, side, w * 3, rs, s);
            to_rgb565(rs, s, px);
            char path[1200];
            art_path(lib, album_id, s, path, sizeof path);
            FILE *f = fopen(path, "wb");
            if (f) { ok += fwrite(px, 2, (size_t)s * s, f) == (size_t)s * s; fclose(f); }
        }
        free(rs);
        free(px);
    }
    free(img);   /* stb_image and the libjpeg path both use malloc */
    return ok == 2 ? 0 : -1;
}

static void build_art(library_t *lib, lib_progress_cb cb, void *ctx)
{
    sqlite3_stmt *q = prep(lib->db,
        "SELECT al.id, al.dir, t.path FROM albums al LEFT JOIN tracks t ON t.id=al.first_track WHERE al.art=0");
    sqlite3_stmt *u = prep(lib->db, "UPDATE albums SET art=? WHERE id=?");
    if (!q || !u) { sqlite3_finalize(q); sqlite3_finalize(u); return; }
    int n = 0;
    while (sqlite3_step(q) == SQLITE_ROW) {
        int64_t id = sqlite3_column_int64(q, 0);
        const char *dir = (const char *)sqlite3_column_text(q, 1);
        const char *first = (const char *)sqlite3_column_text(q, 2);
        int r = make_art(lib, id, dir ? dir : "", first);
        sqlite3_reset(u);
        sqlite3_bind_int(u, 1, r == 0 ? 1 : 2);   /* 2 = looked, none found */
        sqlite3_bind_int64(u, 2, id);
        sqlite3_step(u);
        if (cb && (++n % 20) == 0) cb(n, -1, "cover art", ctx);
    }
    sqlite3_finalize(q);
    sqlite3_finalize(u);
}

/* ---------- playlists ---------- */

typedef struct { char *key; int64_t id; int secs; } keyed_t;

static int cmp_keyed(const void *a, const void *b) { return strcmp(((const keyed_t *)a)->key, ((const keyed_t *)b)->key); }

static int64_t keyed_find(const keyed_t *v, int n, const char *key)
{
    keyed_t k = {(char *)key, 0, 0};
    const keyed_t *hit = bsearch(&k, v, (size_t)n, sizeof *v, cmp_keyed);
    return hit ? hit->id : 0;
}

/* Among the tracks with this key, the one closest to `secs` long (-1: any). */
static int64_t keyed_find_closest(const keyed_t *v, int n, const char *key, int secs)
{
    keyed_t k = {(char *)key, 0, 0};
    const keyed_t *hit = bsearch(&k, v, (size_t)n, sizeof *v, cmp_keyed);
    if (!hit) return 0;
    while (hit > v && !strcmp(hit[-1].key, key)) hit--;
    const keyed_t *best = hit;
    for (const keyed_t *c = hit; c < v + n && !strcmp(c->key, key); c++)
        if (secs >= 0 && abs(c->secs - secs) < abs(best->secs - secs)) best = c;
    return best->id;
}

/* Every track's folded "artist title" from its tags, sorted. */
static keyed_t *tag_index(sqlite3 *db, int *n_out)
{
    *n_out = 0;
    sqlite3_stmt *q = prep(db, "SELECT id, artist, title, duration_ms FROM tracks");
    if (!q) return NULL;
    int cap = 1024, n = 0;
    keyed_t *v = malloc((size_t)cap * sizeof *v);
    char text[1024], key[1024];
    while (v && sqlite3_step(q) == SQLITE_ROW) {
        if (n == cap) {
            keyed_t *nv = realloc(v, (size_t)(cap *= 2) * sizeof *v);
            if (!nv) break;
            v = nv;
        }
        const unsigned char *a = sqlite3_column_text(q, 1), *t = sqlite3_column_text(q, 2);
        snprintf(text, sizeof text, "%s %s", a ? (const char *)a : "", t ? (const char *)t : "");
        text_loose_key(text, key, sizeof key);
        if (!*key) continue;
        v[n].key = strdup(key);
        v[n].id = sqlite3_column_int64(q, 0);
        v[n].secs = (int)(sqlite3_column_int64(q, 3) / 1000);
        if (v[n].key) n++;
    }
    sqlite3_finalize(q);
    if (v) qsort(v, (size_t)n, sizeof *v, cmp_keyed);
    *n_out = n;
    return v;
}

/* Every track's loose key over its last `tail` path components, sorted. */
static keyed_t *loose_index(sqlite3 *db, int tail, int *n_out)
{
    *n_out = 0;
    sqlite3_stmt *q = prep(db, "SELECT id, path FROM tracks");
    if (!q) return NULL;
    int cap = 1024, n = 0;
    keyed_t *v = malloc((size_t)cap * sizeof *v);
    char key[1024];
    while (v && sqlite3_step(q) == SQLITE_ROW) {
        if (n == cap) {
            keyed_t *nv = realloc(v, (size_t)(cap *= 2) * sizeof *v);
            if (!nv) break;
            v = nv;
        }
        path_loose_key((const char *)sqlite3_column_text(q, 1), tail, key, sizeof key);
        v[n].key = strdup(key);
        v[n].id = sqlite3_column_int64(q, 0);
        if (v[n].key) n++;
    }
    sqlite3_finalize(q);
    if (v) qsort(v, (size_t)n, sizeof *v, cmp_keyed);
    *n_out = n;
    return v;
}

static void loose_free(keyed_t *v, int n)
{
    for (int i = 0; i < n; i++) free(v[i].key);
    free(v);
}

/* Re-read every playlist (they're small) and match its entries to tracks:
 * the exact path first, then the folded last three components, then two (a
 * playlist made on another computer, or names spelt in another Unicode form),
 * then the #EXTINF "Artist - Title" against the tags, nearest duration first
 * (the same music copied with other file names). */
/* Playlists: read again when their file changed, or when tracks did (an
 * entry may now be found, or gone); otherwise kept as they are. */
static void scan_playlists(library_t *lib, pathvec_t *plv, int tracks_changed)
{
    sqlite3 *db = lib->db;
    sqlite3_exec(db, "UPDATE playlists SET seen=0;", NULL, NULL, NULL);
    sqlite3_stmt *exact = prep(db, "SELECT id FROM tracks WHERE path=?");
    /* an entry naming a file a CUE sheet splits (a playlist made before, or
     * elsewhere): its tracks, in order */
    sqlite3_stmt *parts = prep(db, "SELECT id FROM tracks WHERE path>=?1||'::' AND path<?1||':;' AND cue>0 ORDER BY path");
    sqlite3_stmt *known = prep(db, "UPDATE playlists SET seen=1 WHERE path=?1 AND mtime=?2 AND size=?3");
    sqlite3_stmt *upl = prep(db,
        "INSERT INTO playlists(path, name, sort, seen, writable, mtime, size) VALUES(?,?,?,1,?,?,?)"
        " ON CONFLICT(path) DO UPDATE SET name=excluded.name, sort=excluded.sort, seen=1, writable=excluded.writable,"
        " mtime=excluded.mtime, size=excluded.size");
    sqlite3_stmt *pid = prep(db, "SELECT id FROM playlists WHERE path=?");
    sqlite3_stmt *clear = prep(db, "DELETE FROM playlist_items WHERE playlist_id=?");
    sqlite3_stmt *add = prep(db, "INSERT INTO playlist_items(playlist_id, pos, track_id) VALUES(?,?,?)");
    sqlite3_stmt *count = prep(db, "UPDATE playlists SET ntracks=?, nmissing=? WHERE id=?");
    /* the looser matches' indexes: made the first time an entry needs them */
    int n3 = 0, n2 = 0, nt = 0, have_loose = 0;
    keyed_t *k3 = NULL, *k2 = NULL, *kt = NULL;

    for (int i = 0; exact && parts && known && upl && pid && clear && add && count && i < plv->n; i++) {
        if (!tracks_changed) {
            sqlite3_reset(known);
            sqlite3_bind_text(known, 1, plv->v[i].p, -1, SQLITE_STATIC);
            sqlite3_bind_int64(known, 2, plv->v[i].mtime);
            sqlite3_bind_int64(known, 3, plv->v[i].size);
            if (sqlite3_step(known) == SQLITE_DONE && sqlite3_changes(db) > 0) continue;   /* as it was */
        }
        m3u_t m;
        if (m3u_read(plv->v[i].p, &m)) continue;
        char sort[256];
        const char *base = strrchr(plv->v[i].p, '/');
        sort_key(base ? base + 1 : plv->v[i].p, sort, sizeof sort);
        sqlite3_reset(upl);
        sqlite3_bind_text(upl, 1, plv->v[i].p, -1, SQLITE_STATIC);
        sqlite3_bind_text(upl, 2, m.name, -1, SQLITE_STATIC);
        sqlite3_bind_text(upl, 3, sort, -1, SQLITE_STATIC);
        size_t dl = strlen(lib->pl_dir);
        sqlite3_bind_int(upl, 4, dl && !strncmp(plv->v[i].p, lib->pl_dir, dl) && plv->v[i].p[dl] == '/');
        sqlite3_bind_int64(upl, 5, plv->v[i].mtime);
        sqlite3_bind_int64(upl, 6, plv->v[i].size);
        sqlite3_step(upl);
        sqlite3_reset(pid);
        sqlite3_bind_text(pid, 1, plv->v[i].p, -1, SQLITE_STATIC);
        int64_t id = sqlite3_step(pid) == SQLITE_ROW ? sqlite3_column_int64(pid, 0) : 0;
        if (!id) { m3u_free(&m); continue; }
        sqlite3_reset(clear);
        sqlite3_bind_int64(clear, 1, id);
        sqlite3_step(clear);

        int found = 0, missing = 0;
        char key[1024];
        for (int j = 0; j < m.n; j++) {
            sqlite3_reset(exact);
            sqlite3_bind_text(exact, 1, m.paths[j], -1, SQLITE_STATIC);
            int64_t t = sqlite3_step(exact) == SQLITE_ROW ? sqlite3_column_int64(exact, 0) : 0;
            if (!t) {
                sqlite3_reset(parts);
                sqlite3_bind_text(parts, 1, m.paths[j], -1, SQLITE_STATIC);
                int np = 0;
                for (; sqlite3_step(parts) == SQLITE_ROW; np++) {
                    sqlite3_reset(add);
                    sqlite3_bind_int64(add, 1, id);
                    sqlite3_bind_int(add, 2, found++);
                    sqlite3_bind_int64(add, 3, sqlite3_column_int64(parts, 0));
                    sqlite3_step(add);
                }
                if (np) continue;
            }
            if (!t && !have_loose) {
                k3 = loose_index(db, 3, &n3);
                k2 = loose_index(db, 2, &n2);
                kt = tag_index(db, &nt);
                have_loose = 1;
            }
            if (!t) { path_loose_key(m.paths[j], 3, key, sizeof key); t = keyed_find(k3, n3, key); }
            if (!t) { path_loose_key(m.paths[j], 2, key, sizeof key); t = keyed_find(k2, n2, key); }
            if (!t && m.infos[j]) { text_loose_key(m.infos[j], key, sizeof key); t = keyed_find_closest(kt, nt, key, m.secs[j]); }
            if (!t) {
                if (missing++ < 3) fprintf(stderr, "playlist %s: not in the library: %s\n", m.name, m.paths[j]);
                continue;
            }
            sqlite3_reset(add);
            sqlite3_bind_int64(add, 1, id);
            sqlite3_bind_int(add, 2, found++);
            sqlite3_bind_int64(add, 3, t);
            sqlite3_step(add);
        }
        if (missing) fprintf(stderr, "playlist %s: %d of %d entries not found\n", m.name, missing, m.n);
        sqlite3_reset(count);
        sqlite3_bind_int(count, 1, found);
        sqlite3_bind_int(count, 2, missing);
        sqlite3_bind_int64(count, 3, id);
        sqlite3_step(count);
        m3u_free(&m);
    }
    loose_free(k3, n3);
    loose_free(k2, n2);
    loose_free(kt, nt);
    sqlite3_finalize(exact); sqlite3_finalize(parts); sqlite3_finalize(known); sqlite3_finalize(upl); sqlite3_finalize(pid);
    sqlite3_finalize(clear); sqlite3_finalize(add); sqlite3_finalize(count);
    sqlite3_exec(db, "DELETE FROM playlists WHERE seen=0;"
                     "DELETE FROM playlist_items WHERE playlist_id NOT IN (SELECT id FROM playlists);",
                 NULL, NULL, NULL);
}

/* ---------- CUE sheets ---------- */

/* tags_v: 1 = the composer read; 2 = and the tags looked at for a CUE sheet */
#define TAGS_V 2

/* Formats whose tags can hold a CUE sheet: files read before tags_v 2 are
 * read again for it, once. */
static int cue_in_tags(const char *path)
{
    const char *dot = strrchr(path, '.');
    return dot && (!strcasecmp(dot, ".flac") || !strcasecmp(dot, ".wv") || !strcasecmp(dot, ".ape"));
}

static int pv_find(const pathvec_t *pv, const char *p)
{
    pent_t k = {(char *)p, 0, 0, -1};
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
    sqlite3_stmt *look, *mark, *mark_parts;    /* a split music file, unchanged */
    sqlite3_stmt *put_file, *up;               /* ...or written again */
} cue_stmts_t;

static int cue_stmts_prep(sqlite3 *db, cue_stmts_t *c)
{
    c->known = prep(db, "SELECT mtime, size, nfiles FROM cues WHERE path=? AND kind=0");
    c->split = prep(db, "SELECT path FROM cues WHERE kind=1 AND sheet=?");
    c->plain = prep(db, "SELECT 1 FROM tracks WHERE path=? AND cue=0 AND mtime=? AND size=?");
    c->put_sheet = prep(db,
        "INSERT INTO cues(path, mtime, size, kind, sheet, nfiles, seen) VALUES(?,?,?,0,NULL,?,1)"
        " ON CONFLICT(path) DO UPDATE SET mtime=excluded.mtime, size=excluded.size, kind=0, sheet=NULL,"
        " nfiles=excluded.nfiles, seen=1");
    c->look = prep(db, "SELECT mtime, size, kind, sheet FROM cues WHERE path=?");
    c->mark = prep(db, "UPDATE cues SET seen=1 WHERE path=?");
    /* a file's tracks: their paths are the file's and then "::" (cue.h) */
    c->mark_parts = prep(db, "UPDATE tracks SET seen=1 WHERE path>=?1||'::' AND path<?1||':;' AND cue>0");
    c->put_file = prep(db,
        "INSERT INTO cues(path, mtime, size, kind, sheet, seen) VALUES(?,?,?,?,?,1)"
        " ON CONFLICT(path) DO UPDATE SET mtime=excluded.mtime, size=excluded.size, kind=excluded.kind,"
        " sheet=excluded.sheet, seen=1");
    /* As a file's upsert, but a measured gain stays while it's the same part
     * of the same file (the path says the part; date and size are the file's)
     * and the sheet doesn't give one now: an edited title costs nothing. */
#define CUE_KEEP_RG "(tracks.mtime IS excluded.mtime AND tracks.size IS excluded.size AND tracks.rg_state IN (2,3)" \
                    " AND excluded.rg_state=0)"
    c->up = prep(db,
        "INSERT INTO tracks(path, mtime, size, title, artist, album_artist, album, genre, year, disc, track,"
        " codec, rate, bits, channels, duration_ms, has_pic, seen,"
        " rg_state, rg_track, rg_track_peak, rg_album, rg_album_peak, composer, tags_v, cue)"
        " VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,1,?,?,?,?,?,?,2,?)"
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
        " composer=excluded.composer, tags_v=2, cue=excluded.cue, search=NULL");
#undef CUE_KEEP_RG
    return c->known && c->split && c->put_sheet && c->plain && c->look && c->mark && c->mark_parts && c->put_file && c->up ? 0 : -1;
}

static void cue_stmts_free(cue_stmts_t *c)
{
    sqlite3_finalize(c->known); sqlite3_finalize(c->split); sqlite3_finalize(c->put_sheet); sqlite3_finalize(c->plain);
    sqlite3_finalize(c->look); sqlite3_finalize(c->mark); sqlite3_finalize(c->mark_parts);
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
 * one there before, as it was, that stayed a track of its own, stays so). */
static void cue_claim(cue_stmts_t *c, pathvec_t *pv, pathvec_t *cv, cue_sheet_t **sheet)
{
    for (int k = 0; k < cv->n; k++) {
        pent_t *e = &cv->v[k];
        int same = 0, want = 0, got = 0;
        sqlite3_reset(c->known);
        sqlite3_bind_text(c->known, 1, e->p, -1, SQLITE_STATIC);
        if (sqlite3_step(c->known) == SQLITE_ROW && sqlite3_column_int64(c->known, 0) == e->mtime &&
            sqlite3_column_int64(c->known, 1) == e->size) {
            same = 1;
            want = sqlite3_column_int(c->known, 2);
            sqlite3_reset(c->split);
            sqlite3_bind_text(c->split, 1, e->p, -1, SQLITE_STATIC);
            while (sqlite3_step(c->split) == SQLITE_ROW) {
                int j = pv_find(pv, (const char *)sqlite3_column_text(c->split, 0));
                if (j >= 0 && pv->v[j].cue < 0) { pv->v[j].cue = k; got++; }
            }
        }
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
                    if (sqlite3_step(c->plain) == SQLITE_ROW) continue;
                }
                pv->v[j].cue = k;
            }
        }
        e->cue = !same;
        sqlite3_reset(c->put_sheet);
        sqlite3_bind_text(c->put_sheet, 1, e->p, -1, SQLITE_STATIC);
        sqlite3_bind_int64(c->put_sheet, 2, e->mtime);
        sqlite3_bind_int64(c->put_sheet, 3, e->size);
        sqlite3_bind_int(c->put_sheet, 4, want);
        sqlite3_step(c->put_sheet);
    }
}

/* A split music file as it was at the last scan (`kind` and `sheet` as
 * then): its tracks and its row are kept. 1 if so. */
static int cue_unchanged(cue_stmts_t *c, const pent_t *f, int kind, const char *sheet)
{
    sqlite3_reset(c->look);
    sqlite3_bind_text(c->look, 1, f->p, -1, SQLITE_STATIC);
    if (sqlite3_step(c->look) != SQLITE_ROW || sqlite3_column_int64(c->look, 0) != f->mtime ||
        sqlite3_column_int64(c->look, 1) != f->size || sqlite3_column_int(c->look, 2) != kind)
        return 0;
    const char *was = (const char *)sqlite3_column_text(c->look, 3);
    if (sheet ? !was || strcmp(was, sheet) : was != NULL) return 0;
    sqlite3_reset(c->mark);
    sqlite3_bind_text(c->mark, 1, f->p, -1, SQLITE_STATIC);
    sqlite3_step(c->mark);
    sqlite3_reset(c->mark_parts);
    sqlite3_bind_text(c->mark_parts, 1, f->p, -1, SQLITE_STATIC);
    sqlite3_step(c->mark_parts);
    return 1;
}

static void bind_tags(sqlite3_stmt *up, const char *path, const pent_t *f, const tags_t *t);

/* The tracks of file `fi` of the sheet, as parts of the music file f (its
 * tags in t: the format, the length, and what the sheet doesn't say). The
 * number written, or -1 if fewer than two can be played: then the file stays
 * one track. */
static int cue_write(cue_stmts_t *c, const pent_t *f, const cue_sheet_t *s, int fi, const tags_t *t,
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
    }
    sqlite3_reset(c->put_file);
    sqlite3_bind_text(c->put_file, 1, f->p, -1, SQLITE_STATIC);
    sqlite3_bind_int64(c->put_file, 2, f->mtime);
    sqlite3_bind_int64(c->put_file, 3, f->size);
    sqlite3_bind_int(c->put_file, 4, kind);
    if (sheet) sqlite3_bind_text(c->put_file, 5, sheet, -1, SQLITE_STATIC);
    else sqlite3_bind_null(c->put_file, 5);
    sqlite3_step(c->put_file);
    return n;
}

/* A music file split by the sheet in its own tags: its tracks (> 0), or -1. */
static int cue_embedded(cue_stmts_t *c, const pent_t *f, const tags_t *t, cue_sheet_t **sheet)
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
static int cue_external(cue_stmts_t *c, const pathvec_t *pv, int i, const pent_t *e, cue_sheet_t **sheet)
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

int library_scan(library_t *lib, const char *root, lib_progress_cb cb, void *ctx)
{
    double t0 = now_s(), t_walk, t_files, t_groups, t_pl, t_search, t_art;
    pathvec_t pv = {0}, plv = {0}, cv = {0}, none = {0}, nocue = {0};
    /* No music folder (renamed, or the card's away): every track would look
     * deleted, with its play counts. The library stays as it is instead. */
    struct stat rst;
    if (stat(root, &rst) != 0 || !S_ISDIR(rst.st_mode)) return -1;
    lib->cancel = 0;
    int fast = fast_walk(lib, root, &pv, &plv, &cv);
    if (!fast) walk(root, &pv, &plv, &cv, 0, &lib->cancel);
    if (lib->pl_dir[0]) walk(lib->pl_dir, &none, &plv, &nocue, 0, &lib->cancel);   /* your own playlists (no music in there) */
    pv_free(&none);
    pv_free(&nocue);
    if (lib->cancel) {
        pv_free(&pv);
        pv_free(&plv);
        pv_free(&cv);
        return -1;
    }
    qsort(pv.v, (size_t)pv.n, sizeof *pv.v, cmp_pent);
    qsort(cv.v, (size_t)cv.n, sizeof *cv.v, cmp_pent);
    t_walk = now_s();

    sqlite3 *db = lib->db;
    sqlite3_exec(db, "BEGIN; UPDATE tracks SET seen=0; UPDATE cues SET seen=0;", NULL, NULL, NULL);
    sqlite3_stmt *look = prep(db, "SELECT id, mtime, size, tags_v FROM tracks WHERE path=? AND cue=0");
    sqlite3_stmt *mark = prep(db, "UPDATE tracks SET seen=1 WHERE id=?");
    sqlite3_stmt *later = prep(db, "UPDATE tracks SET seen=1, composer=?2, tags_v=2 WHERE id=?1");
    sqlite3_stmt *up = prep(db,
        "INSERT INTO tracks(path, mtime, size, title, artist, album_artist, album, genre, year, disc, track,"
        " codec, rate, bits, channels, duration_ms, has_pic, seen,"
        " rg_state, rg_track, rg_track_peak, rg_album, rg_album_peak, composer, tags_v)"
        " VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,1,?,?,?,?,?,?,2)"
        " ON CONFLICT(path) DO UPDATE SET mtime=excluded.mtime, size=excluded.size, title=excluded.title,"
        " artist=excluded.artist, album_artist=excluded.album_artist, album=excluded.album,"
        " genre=excluded.genre, year=excluded.year, disc=excluded.disc, track=excluded.track,"
        " codec=excluded.codec, rate=excluded.rate, bits=excluded.bits, channels=excluded.channels,"
        " duration_ms=excluded.duration_ms, has_pic=excluded.has_pic, seen=1,"
        " rg_state=excluded.rg_state, rg_track=excluded.rg_track, rg_track_peak=excluded.rg_track_peak,"
        " rg_album=excluded.rg_album, rg_album_peak=excluded.rg_album_peak,"
        " composer=excluded.composer, tags_v=2, search=NULL");
    cue_stmts_t cs = {0};
    cue_sheet_t *sheet = NULL;
    int ok = look && mark && later && up && cue_stmts_prep(db, &cs) == 0;
    if (ok) cue_claim(&cs, &pv, &cv, &sheet);

    int changed = 0;   /* tracks added, changed or gone */
    int extra = 0;     /* unchanged tracks that got a tag read later (a composer) */
    for (int i = 0; ok && i < pv.n; i++) {
        if (lib->cancel) { ok = 0; break; }
        const pent_t *f = &pv.v[i];
        const char *path = f->p;
        int n;
        if (cb && (i % 25) == 0) cb(i + 1, pv.n, path, ctx);
        if (f->cue >= 0) {                     /* split by a .cue */
            const pent_t *e = &cv.v[f->cue];
            if (!e->cue && cue_unchanged(&cs, f, 1, e->p)) continue;
            if ((n = cue_external(&cs, &pv, i, e, &sheet)) > 0) { changed += n; continue; }
            /* (not after all: one track, as any file) */
        }
        sqlite3_reset(look);
        sqlite3_bind_text(look, 1, path, -1, SQLITE_STATIC);
        int row = sqlite3_step(look) == SQLITE_ROW;
        if (row && sqlite3_column_int64(look, 1) == f->mtime && sqlite3_column_int64(look, 2) == f->size) {
            int v = sqlite3_column_int(look, 3);
            int64_t id = sqlite3_column_int64(look, 0);
            if (v >= TAGS_V || (v >= 1 && !cue_in_tags(path))) {
                sqlite3_reset(mark);
                sqlite3_bind_int64(mark, 1, id);
                sqlite3_step(mark);
                continue;
            }
            /* unchanged, but read before composers or CUE sheets in tags: those, once */
            tags_t t;
            int got = tags_read(path, &t) == 0;
            if (got && t.has_cuesheet && (n = cue_embedded(&cs, f, &t, &sheet)) > 0) {
                changed += n;                  /* (and its old row goes) */
                continue;
            }
            sqlite3_reset(later);
            sqlite3_bind_int64(later, 1, id);
            sqlite3_bind_text(later, 2, got ? t.composer : "", -1, SQLITE_TRANSIENT);
            sqlite3_step(later);
            if (got && t.composer[0] && v < 1) extra++;
            continue;
        }
        if (!row && cue_unchanged(&cs, f, 2, NULL)) continue;   /* split by its own sheet, as it was */
        tags_t t;
        if (tags_read(path, &t)) continue;
        if (t.has_cuesheet && (n = cue_embedded(&cs, f, &t, &sheet)) > 0) { changed += n; continue; }
        changed++;
        sqlite3_reset(up);
        bind_tags(up, path, f, &t);
        sqlite3_step(up);
    }
    free(sheet);
    sqlite3_finalize(look);
    sqlite3_finalize(mark);
    sqlite3_finalize(later);
    sqlite3_finalize(up);
    cue_stmts_free(&cs);
    pv_free(&cv);
    if (!ok) {   /* cancelled (or the database can't be read): the library as it was */
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        pv_free(&pv);
        pv_free(&plv);
        return -1;
    }
    sqlite3_exec(db, "DELETE FROM tracks WHERE seen=0;", NULL, NULL, NULL);
    changed += sqlite3_changes(db);
    sqlite3_exec(db, "DELETE FROM cues WHERE seen=0;", NULL, NULL, NULL);
    lib->scan_changes = changed + extra;
    t_files = now_s();
    /* albums, artists, credits, search and covers only follow the tracks:
     * nothing to do if no track changed (the usual case) */
    if (changed) rebuild_groups(lib);
    else if (extra) rebuild_composers(db);
    t_groups = now_s();
    scan_playlists(lib, &plv, changed);
    t_pl = now_s();
    index_search(lib, 1);   /* changed tracks (search cleared below), new albums and artists */
    sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
    t_search = now_s();
    pv_free(&plv);

    if (changed) {
        sqlite3_exec(db, "BEGIN", NULL, NULL, NULL);
        build_art(lib, cb, ctx);
        sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
    }
    t_art = now_s();
    fprintf(stderr, "library: scan %.1f s, %d changes: %swalk %.1f, files %.1f, groups %.1f, playlists %.1f, search %.1f, art %.1f\n",
            t_art - t0, changed, fast ? "fast " : "", t_walk - t0, t_files - t_walk, t_groups - t_files, t_pl - t_groups, t_search - t_pl,
            t_art - t_search);

    pv_free(&pv);

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
                    : (view == LIB_PLAYLIST_TRACKS || view == LIB_FAVOURITES || view == LIB_RECENT_PLAYED ||
                       view == LIB_MOST_PLAYED) ? lib->row_pl_track
                    : lib->row_album;   /* albums, artist albums, recently added */
    sqlite3_reset(s);
    sqlite3_bind_int64(s, 1, view == LIB_FOLDER ? -id : id);
    main[0] = meta[0] = '\0';
    if (sqlite3_step(s) != SQLITE_ROW) return -1;
    copy_col(s, 0, main, main_cap);
    int v = sqlite3_column_int(s, 1);
    int track_list = view == LIB_ALBUM_TRACKS || view == LIB_SONGS || view == LIB_PLAYLIST_TRACKS || view == LIB_CREDIT_TRACKS ||
                     view == LIB_FAVOURITES || view == LIB_RECENT_PLAYED || view == LIB_MOST_PLAYED ||
                     view == LIB_COMPOSER_TRACKS;
    if (view == LIB_ARTISTS || view == LIB_ALBUM_ARTISTS || view == LIB_PLAYLISTS || view == LIB_GENRES ||
        view == LIB_COMPOSERS || view == LIB_YEARS)
        snprintf(meta, meta_cap, "%d", v);                                                   /* a count */
    else if (view == LIB_FOLDER) snprintf(meta, meta_cap, "%d song%s", v, v == 1 ? "" : "s");
    else if (track_list) snprintf(meta, meta_cap, "%d:%02d", v / 60000, (v / 1000) % 60);   /* length */
    else if (v > 0) snprintf(meta, meta_cap, "%d", v);                                       /* album year */
    if (view == LIB_PLAYLIST_TRACKS || view == LIB_FAVOURITES || view == LIB_RECENT_PLAYED ||
        view == LIB_MOST_PLAYED) {   /* a mix of artists: "Title · Artist" */
        const unsigned char *a = sqlite3_column_text(s, 2);
        size_t l = strlen(main);
        if (a && *a && l + 4 < main_cap) snprintf(main + l, main_cap - l, " \xC2\xB7 %s", (const char *)a);
    }
    return 0;
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
    if (sqlite3_step(s) != SQLITE_ROW) return -1;
    copy_col(s, 0, out, cap);
    return 0;
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
    FILE *f = fopen(path, "rb");
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
    FILE *f = fopen(tmp, "w");
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
    sqlite3_stmt *s = prep(lib->db, "SELECT SUM(rg_state<>0), COUNT(*) FROM tracks");
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
