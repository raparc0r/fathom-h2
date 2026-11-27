/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* The library's upkeep: compacting its file, checking that the queries a
 * person waits on still read through their indexes, whether a file needs an
 * upgrade, the check after a stop that wasn't clean, and a damaged library
 * set aside with its plays brought over to the new one. Out of library.c, as
 * it was there. */
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE
#define _GNU_SOURCE
#include "library/library_int.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "core/clock.h"

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
    if (!dir_of(file, dir, sizeof dir)) snprintf(dir, sizeof dir, ".");
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
    double t0 = clock_fsec();
    int rc = sqlite3_exec(lib->db, "VACUUM", NULL, NULL, NULL);
    double t1 = clock_fsec();
    /* (VACUUM went through the log: the whole database. Back to nothing, or
     * it stays that size on the card.) */
    sqlite3_exec(lib->db, "PRAGMA wal_checkpoint(TRUNCATE)", NULL, NULL, NULL);
    int64_t after = (int64_t)get_int_col(lib, "PRAGMA page_count", 0) * get_int_col(lib, "PRAGMA page_size", 0);
    fprintf(stderr, "library: compacted in %.1f s, the log emptied in %.1f s: %lld -> %lld KB (%s)\n", t1 - t0,
            clock_fsec() - t1, (long long)(size >> 10), (long long)(after >> 10),
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
    bad += plan_bad(lib, "Never played's count", NEVER_PLAYED_COUNT_SQL, 0, report);
    bad += plan_bad(lib, "Shuffle all's songs", SHUFFLE_ALL_SQL, 0, report);
    bad += plan_bad(lib, "tags to read again", NEEDS_TAGS_SQL, 0, report);
    bad += plan_bad(lib, "any songs", NO_SONGS_SQL, 0, report);
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

/* ---- after a stop that wasn't clean ---- */

static int g_test_check_rc;
void library_test_check_fails(int sqlite_rc) { g_test_check_rc = sqlite_rc; }

int library_check(library_t *lib)
{
    sqlite3_stmt *q = NULL;
    int ok = 0, told = 1;
    double t0 = clock_fsec();
    int rc = g_test_check_rc ? g_test_check_rc : sqlite3_prepare_v2(lib->db, "PRAGMA quick_check(1)", -1, &q, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_step(q);
    g_test_check_rc = 0;
    if (rc == SQLITE_ROW) {
        const char *r = (const char *)sqlite3_column_text(q, 0);
        ok = r && !strcmp(r, "ok");
        if (!ok) fprintf(stderr, "library: damaged: %.200s\n", r ? r : "?");
    } else if (rc == SQLITE_CORRUPT || rc == SQLITE_NOTADB) {
        fprintf(stderr, "library: damaged: %s\n", sqlite3_errmsg(lib->db));
    } else {
        /* The check itself didn't run (no memory just then, a read the card
         * didn't answer, busy): that says nothing about the file. Counted as
         * damaged, the library was set aside and the whole card read again
         * at the next start, 20 minutes for a big one. */
        fprintf(stderr, "library: can't be checked (%d): %s\n", rc, sqlite3_errmsg(lib->db));
        ok = 1;
        told = 0;
    }
    sqlite3_finalize(q);
    fprintf(stderr, "library: checked in %.1f s: %s\n", clock_fsec() - t0, !told ? "not known" : ok ? "sound" : "damaged");
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
        /* (added to what the new library has by now: songs played while the
         * music was being read again keep those plays) */
        "UPDATE tracks SET play_count=play_count+?2, skip_count=skip_count+?3, last_played=MAX(COALESCE(last_played, 0), ?4),"
        " favourite=MAX(COALESCE(favourite, 0), ?5), resume_ms=CASE WHEN COALESCE(resume_ms, 0)>0 THEN resume_ms ELSE ?6 END"
        " WHERE path=?1");
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

/* ---- a library set aside, and what is owed from it ----
 * The file that can't be used moves to PATH.bad with its log (what was
 * written last, play counts among it, is in the log until it's folded in), a
 * new library is made, and the music read again. What was played is brought
 * over from the old file once, when a scan of the new one has finished:
 * songs are matched by their paths, and a song not read yet has no row to
 * take its plays. Until then a mark beside the old file (PATH.bad.owed) says
 * it's owed: across restarts. (It was brought over only if the first scan
 * finished in the run that set the file aside. A scan left with Power held,
 * by the battery or by an error, and nothing ever read the old file again:
 * every play, favourite and resume place gone.) An old file still owed is
 * not written over by a second one set aside: the newer goes to PATH.bad2. */
static void aside_paths(const char *db, char *bad, char *mark, size_t cap)
{
    snprintf(bad, cap, "%s.bad", db);
    snprintf(mark, cap, "%s.bad.owed", db);
}

static void move_with_log(const char *db, const char *to)
{
    char a[1100], b[1100];
    snprintf(b, sizeof b, "%s-wal", to);
    unlink(b);                                       /* (an earlier one's) */
    snprintf(b, sizeof b, "%s-shm", to);
    unlink(b);
    rename(db, to);
    snprintf(a, sizeof a, "%s-wal", db);
    snprintf(b, sizeof b, "%s-wal", to);
    rename(a, b);
    snprintf(a, sizeof a, "%s-shm", db);
    unlink(a);
}

void library_set_aside(const char *db_path)
{
    char bad[1060], mark[1060], second[1060];
    aside_paths(db_path, bad, mark, sizeof bad);
    if (access(mark, F_OK) == 0 && access(bad, F_OK) == 0) {
        snprintf(second, sizeof second, "%s.bad2", db_path);
        fprintf(stderr, "library: %s is still owed its plays: this one set aside as %s\n", bad, second);
        move_with_log(db_path, second);
        return;
    }
    move_with_log(db_path, bad);
    int fd = open(mark, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
    if (fd >= 0) { fsync(fd); close(fd); }
}

int library_import_owed(library_t *lib)
{
    char bad[1060], mark[1060];
    aside_paths(lib->db_path, bad, mark, sizeof bad);
    if (access(mark, F_OK) != 0) return 0;
    if (library_empty(lib) || library_scan_unfinished(lib)) return 0;   /* (not all read yet: after the scan that finishes) */
    int n = library_import_counts(lib, bad);
    /* (an old file that can't be read at all has nothing to give: not asked at every start) */
    if (n >= 0 || access(bad, F_OK) != 0) unlink(mark);
    return n < 0 ? 0 : n;
}

/* 1: the file is damaged (not a database any more, or read through and found
 * wrong: SQLite's own word either way); 0: it is sound, or that can't be told
 * just now (no memory, the card not answering). A library that didn't open is
 * set aside only for the first. (Read through: seconds for a big one, on the
 * way no one takes unless the library didn't open.) */
int library_file_damaged(const char *db_path)
{
    sqlite3 *o = NULL;
    sqlite3_stmt *q = NULL;
    int damaged = 0;
    int rc = sqlite3_open_v2(db_path, &o, SQLITE_OPEN_READONLY, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_prepare_v2(o, "PRAGMA quick_check(1)", -1, &q, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_step(q);
    if (rc == SQLITE_ROW) {
        const char *r = (const char *)sqlite3_column_text(q, 0);
        damaged = r && strcmp(r, "ok") != 0;
    } else damaged = rc == SQLITE_NOTADB || rc == SQLITE_CORRUPT;
    sqlite3_finalize(q);
    sqlite3_close(o);
    return damaged;
}
