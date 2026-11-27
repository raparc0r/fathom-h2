/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* The library's connections and scans (app/library/library.c): opening an
 * up-to-date database does no upgrade work, levelling's next track comes
 * from an index; a background scan lets the player's connection write
 * while it runs, writes nothing when nothing changed, keeps what it read
 * when stopped (and never drops music then), and notes a file it can't
 * read. */
#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/resource.h>
#include <sys/syscall.h>
#endif

#include <sqlite3.h>

#include "core/clock.h"
#include "library/library.h"
#include "test_db.h"
#include "test_flac.h"
#include "test_util.h"

static int write_flac(const char *path, const char *title, const char *artist, const char *album, int track)
{
    char tr[8];
    snprintf(tr, sizeof tr, "%d", track);
    return t_write_flac(path, &(t_flac_t){.frames = 441, .tags = {{"TITLE", title}, {"ARTIST", artist}, {"ALBUM", album},
                                                                  {"TRACKNUMBER", tr}}});
}

/* Every tag that groups songs (empty: left out) */
static int write_flac_tags(const char *path, const char *title, const char *artist, const char *album_artist,
                           const char *album, const char *genre, const char *composer, int year, int track)
{
    char tr[8], yr[8];
    snprintf(tr, sizeof tr, "%d", track);
    snprintf(yr, sizeof yr, "%d", year);
    return t_write_flac(path, &(t_flac_t){.frames = 441, .tags = {
        {"TITLE", t_flac_some(title)}, {"ARTIST", t_flac_some(artist)}, {"ALBUMARTIST", t_flac_some(album_artist)},
        {"ALBUM", t_flac_some(album)}, {"GENRE", t_flac_some(genre)}, {"COMPOSER", t_flac_some(composer)},
        {"DATE", year ? yr : NULL}, {"TRACKNUMBER", tr}}});
}

/* The groups as they stand (albums, artists, credits, genres, composers,
 * folders, and each track's place in them), as text: covers and search
 * left out */
static char *groups_text(const char *db)
{
    static const char *const Q[] = {
        "SELECT id, album_id, artist_id, sort, genre_id, composer_id, folder_id FROM tracks ORDER BY id",
        "SELECT artist_id, track_id FROM credits ORDER BY 1, 2",
        "SELECT * FROM albums ORDER BY id", "SELECT * FROM artists ORDER BY id", "SELECT * FROM genres ORDER BY id",
        "SELECT * FROM composers ORDER BY id", "SELECT * FROM folders ORDER BY id",
    };
    sqlite3 *raw = NULL;
    sqlite3_str *out = NULL;
    if (sqlite3_open(db, &raw) == SQLITE_OK) out = sqlite3_str_new(raw);
    for (size_t i = 0; out && i < sizeof Q / sizeof *Q; i++) {
        sqlite3_stmt *q = NULL;
        if (sqlite3_prepare_v2(raw, Q[i], -1, &q, NULL) != SQLITE_OK) { sqlite3_str_appendf(out, "(%s?)", Q[i]); continue; }
        sqlite3_str_appendf(out, "%s\n", Q[i]);
        while (sqlite3_step(q) == SQLITE_ROW) {
            for (int c = 0; c < sqlite3_column_count(q); c++) {
                const char *n = sqlite3_column_name(q, c);
                if (!strcmp(n, "art") || !strcmp(n, "art_sig") || !strcmp(n, "search")) continue;
                const unsigned char *v = sqlite3_column_text(q, c);
                sqlite3_str_appendf(out, "%s=%s ", n, v ? (const char *)v : "NULL");
            }
            sqlite3_str_appendchar(out, 1, '\n');
        }
        sqlite3_finalize(q);
    }
    char *text = out ? sqlite3_str_finish(out) : NULL;
    sqlite3_close(raw);
    return text;
}

/* Opening: an up-to-date database isn't upgraded again (each connection,
 * the background scan's and levelling's too, re-read the whole tracks table
 * and took the write lock); levelling's next track by its index. */
static int up_calls, up_last_done, up_last_total;

static void up_seen(int done, int total, void *ctx)
{
    (void)ctx;
    up_calls++;
    up_last_done = done;
    up_last_total = total;
}

static void test_open_and_levelling(const char *dir)
{
    char music[300], path[700], db[300], art[300];
    snprintf(music, sizeof music, "%s/open", dir);
    mkdir(music, 0755);
    for (int i = 1; i <= 5; i++) {
        char t[16];
        snprintf(t, sizeof t, "Song %d", i);
        snprintf(path, sizeof path, "%s/%02d.flac", music, i);
        CHECK(write_flac(path, t, "Artist", "Album", i) == 0);
    }
    snprintf(db, sizeof db, "%s/open.db", dir);
    snprintf(art, sizeof art, "%s/open-art", dir);
    library_t *lib = library_open(db, art);
    CHECK(lib && library_scan(lib, music, NULL, NULL) == 5);
    library_close(lib);
    CHECK(t_db_int(db, "PRAGMA user_version") == 9);

    /* a column an upgrade would fill, emptied: an open of an up-to-date
     * library leaves it (it doesn't look) */
    CHECK(t_db_exec(db, "UPDATE tracks SET sort=NULL WHERE title='Song 3'") == SQLITE_OK);
    lib = library_open(db, art);
    CHECK(lib != NULL);
    library_close(lib);
    CHECK(t_db_int(db, "SELECT COUNT(*) FROM tracks WHERE sort IS NULL") == 1);
    /* ...and one from before (user_version 4) is upgraded once, saying how far
     * it has got, to the last step; library_upgrade_needed() tells it's
     * coming (about how many songs) and that it's done */
    char none[320];
    snprintf(none, sizeof none, "%s/no-such.db", dir);
    CHECK(library_upgrade_needed(db) == 0 && library_upgrade_needed(none) == 0);
    CHECK(access(none, F_OK) != 0);                  /* (not made by looking) */
    CHECK(t_db_exec(db, "PRAGMA user_version=4") == SQLITE_OK);
    CHECK(library_upgrade_needed(db) == 5);
    up_calls = up_last_done = up_last_total = 0;
    library_set_upgrade_progress(up_seen, NULL);
    lib = library_open(db, art);
    library_set_upgrade_progress(NULL, NULL);
    CHECK(lib != NULL);
    CHECK(up_calls > 10 && up_last_done == up_last_total && up_calls == up_last_total);
    CHECK(library_upgrade_needed(db) == 0);
    CHECK(t_db_int(db, "SELECT COUNT(*) FROM tracks WHERE sort IS NULL") == 0);
    CHECK(t_db_int(db, "PRAGMA user_version") == 9);
    library_close(lib);
    /* ...and one from 0.16.0 (8: the search text every word in a row, no index
     * of it) gets its search keys (name|artist|album) and their index, and
     * finds its songs */
    CHECK(t_db_exec(db, "DROP INDEX tracks_search; UPDATE tracks SET search='song 3 an artist an artist an album';"
                      "UPDATE albums SET search='an album an artist'; UPDATE artists SET search='an artist';"
                      "PRAGMA user_version=8") == SQLITE_OK);
    CHECK(library_upgrade_needed(db) == 5);
    lib = library_open(db, art);
    CHECK(lib != NULL && library_upgrade_needed(db) == 0);
    CHECK(t_db_int(db, "SELECT COUNT(*) FROM tracks WHERE search LIKE '%|%|%'") == 5);
    CHECK(t_db_int(db, "SELECT COUNT(*) FROM tracks WHERE search LIKE 'song 3|%'") == 1);
    CHECK(t_db_int(db, "SELECT COUNT(*) FROM albums WHERE search NOT LIKE '%|%|'") == 0);
    CHECK(t_db_int(db, "SELECT COUNT(*) FROM artists WHERE search NOT LIKE '%||'") == 0);
    CHECK(t_db_int(db, "SELECT COUNT(*) FROM sqlite_master WHERE name='tracks_search'") == 1);
    {
        lib_search_t found;
        CHECK(library_search(lib, "song 3", &found) >= 1 && found.n_songs >= 1 && found.hits[0].tier == 1);
        library_search_free(&found);
    }

    /* levelling: the tracks left come from tracks_rg_todo, in id order */
    sqlite3 *raw = NULL;
    sqlite3_stmt *q = NULL;
    CHECK(sqlite3_open(db, &raw) == SQLITE_OK &&
          sqlite3_prepare_v2(raw, "EXPLAIN QUERY PLAN SELECT id, path FROM tracks WHERE rg_state=0 ORDER BY id LIMIT 1",
                             -1, &q, NULL) == SQLITE_OK);
    int indexed = 0;
    while (q && sqlite3_step(q) == SQLITE_ROW)
        indexed |= strstr((const char *)sqlite3_column_text(q, 3), "tracks_rg_todo") != NULL;
    CHECK(indexed);
    sqlite3_finalize(q);
    sqlite3_close(raw);

    /* every query a person waits on (lists, smart playlists, Shuffle all, the
     * start's checks) found from an index, not by reading the whole tracks
     * table: about 2 s for 50,000 songs on the H2 (library_check_plans) */
    CHECK(library_check_plans(lib, stderr) == 0);
    int64_t id, last = 0;
    char p[1024];
    int n = 0, done, total;
    while (library_rg_next(lib, &id, p, sizeof p) == 0 && n < 10) {
        CHECK(id > last && strstr(p, ".flac"));
        last = id;
        lib_rg_t rg = {2, -4.0f, 0.5f, 0, 0, 0};
        library_rg_store(lib, id, &rg);
        library_rg_progress(lib, &done, &total);
        CHECK(done == ++n && total == 5);
    }
    CHECK(n == 5);
    CHECK(library_rg_next(lib, &id, p, sizeof p) != 0);
    library_close(lib);
}

/* count files in folder `name` (made), track numbers from 1 */
static void make_album(const char *music, const char *name, int count)
{
    char path[700], t[64];
    snprintf(path, sizeof path, "%s/%s", music, name);
    mkdir(path, 0755);
    for (int i = 1; i <= count; i++) {
        snprintf(path, sizeof path, "%s/%s/%03d.flac", music, name, i);
        snprintf(t, sizeof t, "%s %d", name, i);
        if (write_flac(path, t, "Some Artist", name, i)) { fprintf(stderr, "cannot write %s\n", path); exit(1); }
    }
}

typedef struct {
    library_t *lib;
    const char *music;
    int result, calls, cancel_after;   /* cancel_after: at that progress call (0: never) */
    const char *cancel_unless;         /* ...whose path doesn't have this in it */
    useconds_t nap;                    /* each progress call: slower files, as the H2's card */
    volatile int done;
    char where[160];                   /* the last progress call: a file, "covers" (for a slow write's report) */
} scan_job_t;

static void job_progress(int done, int total, const char *path, void *ctx)
{
    scan_job_t *j = ctx;
    (void)done;
    if (path && (!strcmp(path, "looking") || !strcmp(path, "checking") || !strcmp(path, "sorting") || !strcmp(path, "working"))) return;   /* (the folder being listed) */
    const char *base = path ? strrchr(path, '/') : NULL;
    snprintf(j->where, sizeof j->where, "%s", total < 0 ? "covers" : base ? base + 1 : path ? path : "?");
    if (total < 0) return;                         /* (covers) */
    j->calls++;
    if (j->nap) usleep(j->nap);
    if (j->cancel_after && j->calls >= j->cancel_after && (!j->cancel_unless || !strstr(path, j->cancel_unless)))
        library_scan_cancel(j->lib);
}

static void *job_run(void *arg)
{
    scan_job_t *j = arg;
    j->result = library_scan(j->lib, j->music, job_progress, j);
    j->done = 1;
    return NULL;
}

/* The music files under music, as the fast walk would see them, are all in
 * the library (tracks of their own, or in `cues`): one missing and the fast
 * walk's list never matched again. */
static int64_t files_in_library(const char *db)
{
    return t_db_int(db, "SELECT COUNT(*) FROM (SELECT path FROM tracks WHERE cue=0 UNION SELECT path FROM cues)");
}

static void test_scans(const char *dir)
{
    char music[300], path[700], db[300], art[300];
    snprintf(music, sizeof music, "%s/music", dir);
    mkdir(music, 0755);
    snprintf(db, sizeof db, "%s/scan.db", dir);
    snprintf(art, sizeof art, "%s/scan-art", dir);
    char name[32];
    for (int a = 0; a < 2; a++) {
        snprintf(name, sizeof name, "Album %02d", a);
        make_album(music, name, 10);
    }
    library_t *player = library_open(db, art);    /* the player's connection, open throughout */
    CHECK(player && library_scan(player, music, NULL, NULL) == 20);
    lib_list_t songs;
    CHECK(library_list(player, LIB_SONGS, 0, &songs) == 20);
    int64_t tid = songs.n ? songs.ids[0] : 0;
    library_list_free(&songs);

    /* 1. Music added: a background scan of 600 new files (each slowed a
     * little, as on the card) while the player marks a favourite again and
     * again: each write waits a fraction of a second at most (the scan held
     * the write lock to the end: seconds, past 10 s on the H2 a failure) */
    for (int a = 2; a < 32; a++) {
        snprintf(name, sizeof name, "Album %02d", a);
        make_album(music, name, 20);
    }
    scan_job_t job = {.lib = library_open(db, art), .music = music, .nap = 80000};
    pthread_t th;
    CHECK(job.lib && pthread_create(&th, NULL, job_run, &job) == 0);
#ifdef __linux__
    /* the player's writes come from its UI thread, at normal priority; the
     * scan (as the player's) at nice 19, as the H2 run is (started first:
     * a thread starts at its maker's) */
    id_t me = (id_t)syscall(SYS_gettid);
    int was = getpriority(PRIO_PROCESS, me);
    setpriority(PRIO_PROCESS, me, 0);
#endif
    double worst = 0, t0 = clock_fsec();
    int writes = 0, kept = 1;
    while (!job.done) {
        double t = clock_fsec();
        library_set_favourite(player, tid, writes & 1);
        double w = clock_fsec() - t;
        if (w > worst) worst = w;
        if (w > 0.25)                             /* (where the scan was: what held the lock) */
            fprintf(stderr, "slow write: %.2f s, %.1f s in, the scan at %s (%d files)\n", w, t - t0, job.where, job.calls);
        kept &= library_favourite(player, tid) == (writes & 1);
        writes++;
        usleep(20000);
    }
    pthread_join(th, NULL);
#ifdef __linux__
    setpriority(PRIO_PROCESS, me, was);
#endif
    double took = clock_fsec() - t0;
    CHECK(job.result == 620);
    CHECK(took > 1.5 && writes > 20);             /* (long enough to tell) */
    /* On the H2 itself (tools/h2-tests.sh says so) the groups of the songs
     * that changed are written with the lock held, about 1 ms a song gone
     * over and 1,500 of them at most (GROUPS_INCR_AROUND): 1.4 s measured for
     * these 600, where the whole scan takes 7 s. */
    double most = getenv("FATHOM_TEST_ON_H2") ? 2.5 : 0.5;
    CHECK(worst < most);
    CHECK(kept);
    if (worst >= most) fprintf(stderr, "a write waited %.2f s (scan %.1f s)\n", worst, took);
    library_close(job.lib);
    CHECK(t_db_int(db, "SELECT COUNT(*) FROM tracks WHERE album_id IS NULL") == 0);
    CHECK(t_db_int(db, "SELECT COUNT(*) FROM albums") == 32);

    /* 2. Nothing changed: the scan writes nothing at all (it rewrote every
     * track's row, twice) */
    sqlite3 *raw = NULL;
    CHECK(sqlite3_open(db, &raw) == SQLITE_OK);
    int64_t v0 = -1, v1 = -2;
    sqlite3_stmt *q = NULL;
    if (sqlite3_prepare_v2(raw, "PRAGMA data_version", -1, &q, NULL) == SQLITE_OK && sqlite3_step(q) == SQLITE_ROW)
        v0 = sqlite3_column_int64(q, 0);
    sqlite3_reset(q);
    library_t *bg = library_open(db, art);
    CHECK(bg && library_scan(bg, music, NULL, NULL) == 620 && library_scan_changes(bg) == 0);
    library_close(bg);
    if (q && sqlite3_step(q) == SQLITE_ROW) v1 = sqlite3_column_int64(q, 0);
    sqlite3_finalize(q);
    CHECK(v0 == v1);                              /* no other connection committed */

    /* 3. Files removed, and the scan stopped half way: nothing is dropped;
     * a whole scan drops them */
    for (int i = 1; i <= 5; i++) {
        snprintf(path, sizeof path, "%s/Album 05/%03d.flac", music, i);
        unlink(path);
    }
    /* (every file held by name, as scans did: with the library's files ticked
     * off as the folder is listed there's no file here to stop at;
     * test_known_files stops one that has new files to read) */
    library_test_known(0);
    scan_job_t stop = {.lib = library_open(db, art), .music = music, .cancel_after = 3};
    CHECK(stop.lib && library_scan(stop.lib, music, job_progress, &stop) == -1);
    library_close(stop.lib);
    library_test_known(1);
    CHECK(t_db_int(db, "SELECT COUNT(*) FROM tracks") == 620);
    CHECK(library_scan(player, music, NULL, NULL) == 615);

    /* 4. New music, the scan stopped after reading it: what it read is kept
     * (it was all rolled back), and the next scan, with nothing new to
     * read, makes their album */
    make_album(music, "Aa New", 20);              /* (first in path order) */
    stop = (scan_job_t){.lib = library_open(db, art), .music = music, .cancel_after = 2, .cancel_unless = "Aa New"};
    CHECK(stop.lib && library_scan(stop.lib, music, job_progress, &stop) == -1);
    library_close(stop.lib);
    CHECK(t_db_int(db, "SELECT COUNT(*) FROM tracks") == 635);
    CHECK(t_db_int(db, "SELECT COUNT(*) FROM tracks WHERE album LIKE 'Aa New' AND album_id IS NULL") == 20);
    CHECK(t_db_int(db, "SELECT COUNT(*) FROM tracks WHERE album LIKE 'Aa New' AND sort IS NOT NULL") == 20);   /* (in Songs) */
    bg = library_open(db, art);
    CHECK(bg && library_scan(bg, music, NULL, NULL) == 635 && library_scan_changes(bg) > 0);
    library_close(bg);
    CHECK(t_db_int(db, "SELECT COUNT(*) FROM tracks WHERE album_id IS NULL") == 0);
    CHECK(t_db_int(db, "SELECT COUNT(*) FROM albums WHERE title='Aa New' AND ntracks=20") == 1);
    lib_search_t hits;
    CHECK(library_search(player, "aa new 7", &hits) > 0 && hits.n_songs == 1);
    library_search_free(&hits);
    CHECK(library_scan(player, music, NULL, NULL) == 635 && library_scan_changes(player) == 0);

    /* 5. A file that can't be read: noted in `cues`, so the library still
     * holds every file the walk lists, and it's not read again */
    snprintf(path, sizeof path, "%s/Album 00/bad.flac", music);
    FILE *f = fopen(path, "wb");
    if (f) { fputs("not a FLAC file at all", f); fclose(f); }
    CHECK(library_scan(player, music, NULL, NULL) == 635);
    CHECK(t_db_int(db, "SELECT kind FROM cues WHERE path LIKE '%/bad.flac'") == 3);
    CHECK(files_in_library(db) == 636);
    CHECK(library_scan(player, music, NULL, NULL) == 635 && library_scan_changes(player) == 0);
    /* ...until it changes: a track then, and the note goes */
    CHECK(write_flac(path, "Fixed", "Some Artist", "Album 00", 99) == 0);
    CHECK(library_scan(player, music, NULL, NULL) == 636);
    CHECK(t_db_int(db, "SELECT COUNT(*) FROM cues") == 0);
    CHECK(files_in_library(db) == 636);

    /* 6. Playlists made elsewhere: entries found by their last three path
     * components, or two, or the #EXTINF artist and title; more of them than
     * one pass looks for at once; and an .m3u in Windows' code page */
    snprintf(path, sizeof path, "%s/other.m3u8", music);
    f = fopen(path, "wb");
    if (f) {
        fputs("#EXTM3U\nAlbum 01/001.flac\nC:\\Users\\x\\music\\Album 01\\002.flac\nD:\\elsewhere\\Album 02\\003.flac\n"
              "#EXTINF:1,Some Artist - Album 03 4\nX:\\renamed\\zzz.flac\nmissing/nothing.flac\n", f);
        for (int a = 10; a < 25; a++)
            for (int i = 1; i <= 20; i++) fprintf(f, "E:\\pc\\music\\Album %02d\\%03d.flac\n", a, i);
        fclose(f);
    }
    make_album(music, "Caf\xC3\xA9", 3);
    snprintf(path, sizeof path, "%s/latin.m3u", music);
    f = fopen(path, "wb");
    if (f) { fputs("Caf\xE9/001.flac\r\nCaf\xE9/003.flac\r\n", f); fclose(f); }
    CHECK(library_scan(player, music, NULL, NULL) == 639);
    lib_list_t pls, items;
    CHECK(library_list(player, LIB_PLAYLISTS, 0, &pls) == 2);
    for (int p = 0; p < pls.n; p++) {
        char pname[128], meta[64];
        library_row(player, LIB_PLAYLISTS, pls.ids[p], pname, sizeof pname, meta, sizeof meta);
        int n = library_list(player, LIB_PLAYLIST_TRACKS, pls.ids[p], &items);
        if (!strcmp(pname, "other")) {
            CHECK(n == 304);
            CHECK(t_db_int(db, "SELECT nmissing FROM playlists WHERE name='other'") == 1);
            const char *want[5] = {"Album 01/001", "Album 01/002", "Album 02/003", "Album 03/004", "Album 10/001"};
            for (int k = 0; k < 5 && k < n; k++) {
                char sql[200];
                snprintf(sql, sizeof sql, "SELECT id FROM tracks WHERE path LIKE '%%/%s.flac'", want[k]);
                CHECK(items.ids[k] == t_db_int(db, sql));
            }
            if (n == 304) CHECK(items.ids[303] == t_db_int(db, "SELECT id FROM tracks WHERE path LIKE '%/Album 24/020.flac'"));
        } else {
            CHECK(!strcmp(pname, "latin") && n == 2);
        }
        library_list_free(&items);
    }
    library_list_free(&pls);

    /* 7. An unreadable file a .cue names: noted once, and later scans of
     * nothing new write nothing (it was claimed by the sheet each time,
     * and the groups made again) */
    snprintf(path, sizeof path, "%s/Album 01/whole.flac", music);
    f = fopen(path, "wb");
    if (f) { fputs("junk, not a FLAC", f); fclose(f); }
    snprintf(path, sizeof path, "%s/Album 01/whole.cue", music);
    f = fopen(path, "wb");
    if (f) { fputs("FILE \"whole.flac\" WAVE\nTRACK 01 AUDIO\nINDEX 01 00:00:00\nTRACK 02 AUDIO\nINDEX 01 00:01:00\n", f); fclose(f); }
    CHECK(library_scan(player, music, NULL, NULL) == 639);
    CHECK(library_scan(player, music, NULL, NULL) == 639);
    q = NULL;
    v0 = -1, v1 = -2;
    if (sqlite3_prepare_v2(raw, "PRAGMA data_version", -1, &q, NULL) == SQLITE_OK && sqlite3_step(q) == SQLITE_ROW)
        v0 = sqlite3_column_int64(q, 0);
    sqlite3_reset(q);
    CHECK(library_scan(player, music, NULL, NULL) == 639 && library_scan_changes(player) == 0);
    if (q && sqlite3_step(q) == SQLITE_ROW) v1 = sqlite3_column_int64(q, 0);
    sqlite3_finalize(q);
    sqlite3_close(raw);
    bg = library_open(db, art);                   /* (data_version: another connection's commits) */
    CHECK(bg && library_scan(bg, music, NULL, NULL) == 639 && library_scan_changes(bg) == 0);
    library_close(bg);
    CHECK(v0 == v1 && t_db_int(db, "SELECT value FROM meta WHERE key='scan_pending'") == 0);

    /* 8. A file that can't be opened just now (here: no permission; on the
     * H2 a read error): not noted as unreadable, read at the next scan */
    snprintf(path, sizeof path, "%s/Album 01/later.flac", music);
    CHECK(write_flac(path, "Later", "Some Artist", "Album 01", 50) == 0);
    chmod(path, 0);
    if (access(path, R_OK) != 0) {                /* (as root it can be read anyway) */
        CHECK(library_scan(player, music, NULL, NULL) == 639);
        CHECK(t_db_int(db, "SELECT COUNT(*) FROM cues WHERE path LIKE '%/later.flac'") == 0);
    }
    chmod(path, 0644);
    CHECK(library_scan(player, music, NULL, NULL) == 640);

    /* 9. The groups a scan writes (only what changed, see shadow_apply) are
     * the ones a whole rebuild makes: after an album gone (its id given
     * to the next new one), a new one, songs moved to another album and
     * artist, and an artist's name respelled */
    for (int i = 1; i <= 20; i++) {
        snprintf(path, sizeof path, "%s/Album 03/%03d.flac", music, i);
        unlink(path);
    }
    make_album(music, "Zz Later", 3);
    snprintf(path, sizeof path, "%s/Album 04/%03d.flac", music, 1);
    unlink(path);
    snprintf(path, sizeof path, "%s/Album 04/moved.flac", music);
    CHECK(write_flac(path, "Moved", "Other Artist", "Album 06", 1) == 0);
    snprintf(path, sizeof path, "%s/Album 07/respelled.flac", music);
    CHECK(write_flac(path, "Respelled", "SOME artist", "Album 07", 21) == 0);
    bg = library_open(db, art);
    CHECK(bg && library_scan(bg, music, NULL, NULL) == 624 && library_scan_changes(bg) > 0);
    library_close(bg);
    library_close(player);
    char *aside = groups_text(db);
    CHECK(t_db_exec(db, "PRAGMA user_version=3") == SQLITE_OK);   /* (opened: groups made again, all at once) */
    player = library_open(db, art);
    library_close(player);
    char *whole = groups_text(db);
    CHECK(aside && whole && strlen(aside) > 1000 && !strcmp(aside, whole));
    if (aside && whole && strcmp(aside, whole)) {
        size_t k = 0;
        while (aside[k] && aside[k] == whole[k]) k++;
        while (k && aside[k - 1] != '\n') k--;
        fprintf(stderr, "groups differ from:\n  aside: %.200s\n  whole: %.200s\n", aside + k, whole + k);
    }
    sqlite3_free(aside);
    sqlite3_free(whole);

    /* 10. A song removed: its play log goes with it (its id is given to the
     * next new song, which would show in On repeat, never played) */
    snprintf(path, sizeof path, "%s/Album 06/020.flac", music);
    int64_t gone = t_db_int(db, "SELECT id FROM tracks WHERE path LIKE '%/Album 06/020.flac'");
    player = library_open(db, art);
    CHECK(player && gone > 0);
    if (player && gone > 0) {
        library_played(player, gone, 0, (int64_t)time(NULL) - 60);
        library_played(player, gone, 0, (int64_t)time(NULL));
        CHECK(t_db_int(db, "SELECT COUNT(*) FROM plays") >= 2);
        unlink(path);
        CHECK(library_scan(player, music, NULL, NULL) == 623);
        CHECK(t_db_int(db, "SELECT COUNT(*) FROM plays WHERE track_id NOT IN (SELECT id FROM tracks)") == 0);
    }
    library_close(player);

    /* 11. A first scan cut short (the H2 switched off half way): the library
     * says so, so the next start finishes it in the background; once done,
     * it doesn't */
    char db2[400];
    snprintf(db2, sizeof db2, "%s/cut.db", dir);
    library_t *cut = library_open(db2, art);
    CHECK(cut != NULL);
    if (cut) {
        CHECK(!library_scan_unfinished(cut));
        scan_job_t j = {.lib = cut, .music = music, .cancel_after = 10};   /* (of about 26) */
        int r = library_scan(cut, music, job_progress, &j);
        if (r >= 0) fprintf(stderr, "cut scan: %d tracks, %d progress calls\n", r, j.calls);
        CHECK(r < 0);
        CHECK(t_db_int(db2, "SELECT COUNT(*) FROM tracks") > 0);
        CHECK(library_scan_unfinished(cut));
        CHECK(library_scan(cut, music, NULL, NULL) == 623);
        CHECK(!library_scan_unfinished(cut));
        CHECK(t_db_int(db2, "SELECT COUNT(*) FROM albums") > 0);
        library_close(cut);
    }
}

/* A playlist made on the player while a scan runs: its row goes in before
 * its file is written, and a scan checking in between took it for deleted
 * (and the songs being added to it with it) */
static void test_playlist_during_scan(const char *dir)
{
    char music[300], db[300], art[300], pl[300], sql[900];
    snprintf(music, sizeof music, "%s/plmusic", dir);
    mkdir(music, 0755);
    snprintf(db, sizeof db, "%s/pl.db", dir);
    snprintf(art, sizeof art, "%s/pl-art", dir);
    snprintf(pl, sizeof pl, "%s/plists", dir);
    mkdir(pl, 0755);
    char name[32];
    for (int a = 0; a < 5; a++) {
        snprintf(name, sizeof name, "Album %02d", a);
        make_album(music, name, 20);
    }
    library_t *player = library_open(db, art);
    library_set_playlist_dir(player, pl);
    CHECK(player && library_scan(player, music, NULL, NULL) == 100);
    for (int a = 5; a < 10; a++) {                 /* (something for the scan to read, slowly) */
        snprintf(name, sizeof name, "Album %02d", a);
        make_album(music, name, 20);
    }
    scan_job_t job = {.lib = library_open(db, art), .music = music, .nap = 150000};
    library_set_playlist_dir(job.lib, pl);
    pthread_t th;
    CHECK(job.lib && pthread_create(&th, NULL, job_run, &job) == 0);
    while (!job.done && job.calls < 1) usleep(2000);   /* walked, reading files */
    CHECK(!job.done);
    /* the player's row, its file not there yet */
    snprintf(sql, sizeof sql, "PRAGMA busy_timeout=5000; INSERT INTO playlists(path, name, sort, seen, writable)"
             " VALUES('%s/New one.m3u', 'New one', 'new one', 1, 1)", pl);
    CHECK(t_db_exec(db, sql) == SQLITE_OK);
    pthread_join(th, NULL);
    CHECK(job.result == 200);
    CHECK(t_db_int(db, "SELECT COUNT(*) FROM playlists WHERE name='New one'") == 1);
    library_close(job.lib);
    /* written by then; and if never, the next scan drops it */
    CHECK(library_scan(player, music, NULL, NULL) == 200);
    CHECK(t_db_int(db, "SELECT COUNT(*) FROM playlists WHERE name='New one'") == 0);
    library_close(player);
}

/* ---- the quick listing (fast walk): a FAT listing of the test's own ----
 * The real files, as the kernel has them, with what a wrong FAT reading would
 * do to them: miss a file, spell a name otherwise, read a date otherwise. */

static const char *fw_skip, *fw_rename, *fw_bump;   /* (substrings of a path) */
static int fw_calls;

static int fake_list(const char *dir, int (*cb)(const char *, int64_t, int64_t, void *), void *ctx)
{
    DIR *d = opendir(dir);
    if (!d) return -1;
    struct dirent *e;
    char path[1024];
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(path, &st)) continue;
        if (S_ISDIR(st.st_mode)) { fake_list(path, cb, ctx); continue; }
        if (fw_skip && strstr(path, fw_skip)) continue;
        int64_t mtime = (int64_t)st.st_mtime;
        if (fw_bump && strstr(path, fw_bump)) mtime += 2;
        char *r = fw_rename ? strstr(path, fw_rename) : NULL;
        if (r) r[strlen(fw_rename) - 1] = 'Z';      /* ("002.flac" listed as "00Z.flac") */
        if (cb(path, mtime, (int64_t)st.st_size, ctx)) break;
    }
    closedir(d);
    return 0;
}

static int fake_fatwalk(const char *dev, const char *mount, const char *rel,
                        int (*cb)(const char *, int64_t, int64_t, void *), void *ctx)
{
    (void)dev;
    char root[700];
    snprintf(root, sizeof root, "%s/%s", mount, rel);
    fw_calls++;
    return fake_list(root, cb, ctx);
}

static void fw_set(const char *skip, const char *rename, const char *bump)
{
    fw_skip = skip;
    fw_rename = rename;
    fw_bump = bump;
}

static void test_quick_listing(const char *dir)
{
    char music[300], db[300], art[300], path[700];
    snprintf(music, sizeof music, "%s/qmusic", dir);
    mkdir(music, 0755);
    snprintf(db, sizeof db, "%s/quick.db", dir);
    snprintf(art, sizeof art, "%s/quick-art", dir);
    make_album(music, "Album A", 5);
    make_album(music, "Album B", 5);
    library_t *lib = library_open(db, art);
    CHECK(lib && library_scan(lib, music, NULL, NULL) == 10 && !library_scan_quick(lib));   /* (no FAT set up) */
    library_test_fatwalk(fake_fatwalk);
    library_set_fast_walk(lib, "fake", dir);

    /* nothing changed: the listing alone */
    fw_set(NULL, NULL, NULL);
    CHECK(library_scan(lib, music, NULL, NULL) == 10 && library_scan_quick(lib) && library_scan_changes(lib) == 0);

    /* an album added (USB drive mode): the listing, the new files checked */
    make_album(music, "Album C", 5);
    fw_set(NULL, NULL, "Album C");                   /* (its dates misread: the kernel's are kept) */
    CHECK(library_scan(lib, music, NULL, NULL) == 15 && library_scan_quick(lib) && library_scan_changes(lib) == 5);
    fw_set(NULL, NULL, NULL);
    CHECK(library_scan(lib, music, NULL, NULL) == 15 && library_scan_quick(lib) && library_scan_changes(lib) == 0);

    /* a file the FAT reading misses: the slow walk, and it stays */
    fw_set("Album A/003", NULL, NULL);
    CHECK(library_scan(lib, music, NULL, NULL) == 15 && !library_scan_quick(lib) && library_scan_changes(lib) == 0);
    CHECK(t_db_int(db, "SELECT COUNT(*) FROM tracks WHERE path LIKE '%Album A/003.flac'") == 1);

    /* a name it spells otherwise: the slow walk, and no song under that name */
    fw_set(NULL, "Album B/002", NULL);
    CHECK(library_scan(lib, music, NULL, NULL) == 15 && !library_scan_quick(lib) && library_scan_changes(lib) == 0);
    CHECK(t_db_int(db, "SELECT COUNT(*) FROM tracks WHERE path LIKE '%00Z.flac'") == 0);

    /* a date it reads otherwise, the file unchanged: the slow walk */
    fw_set(NULL, NULL, "Album A/001");
    CHECK(library_scan(lib, music, NULL, NULL) == 15 && !library_scan_quick(lib) && library_scan_changes(lib) == 0);

    /* a file deleted, another changed: the listing, both checked */
    fw_set(NULL, NULL, NULL);
    snprintf(path, sizeof path, "%s/Album C/005.flac", music);
    CHECK(remove(path) == 0);
    snprintf(path, sizeof path, "%s/Album B/001.flac", music);
    CHECK(write_flac(path, "Retitled", "Some Artist", "Album B", 1) == 0);
    struct timeval tv[2] = {{time(NULL) + 60, 0}, {time(NULL) + 60, 0}};
    CHECK(utimes(path, tv) == 0);
    CHECK(library_scan(lib, music, NULL, NULL) == 14 && library_scan_quick(lib) && library_scan_changes(lib) == 2);
    CHECK(t_db_int(db, "SELECT COUNT(*) FROM tracks WHERE title='Retitled'") == 1);
    CHECK(fw_calls == 7);

    /* many new files at once (a first copy: not looked up one by one), one of
     * them under a name the FAT reading spells otherwise: found when its tags
     * are read, the folder walked instead, every file in and none under the
     * wrong name */
    make_album(music, "Album D", 300);
    fw_set(NULL, "Album D/150", NULL);
    CHECK(library_scan(lib, music, NULL, NULL) == 314 && !library_scan_quick(lib) && library_scan_changes(lib) > 0);
    CHECK(t_db_int(db, "SELECT COUNT(*) FROM tracks WHERE path LIKE '%Album D/%'") == 300);
    CHECK(t_db_int(db, "SELECT COUNT(*) FROM tracks WHERE path LIKE '%15Z.flac'") == 0);
    /* ...and all read right: the listing alone from then on (the same as the
     * last finished scan's: nothing looked up, nothing written) */
    fw_set(NULL, NULL, NULL);
    CHECK(library_scan(lib, music, NULL, NULL) == 314 && library_scan_quick(lib) && library_scan_changes(lib) == 0);
    char wal[320];
    struct stat w0, w1;
    snprintf(wal, sizeof wal, "%s-wal", db);
    CHECK(stat(wal, &w0) == 0);
    CHECK(library_scan(lib, music, NULL, NULL) == 314 && library_scan_changes(lib) == 0 && library_scan_groups(lib) == 0);
    CHECK(stat(wal, &w1) == 0 && w0.st_size == w1.st_size && w0.st_mtime == w1.st_mtime);   /* (its log untouched) */
    /* many new files, all read right: the listing stands, none looked up twice */
    make_album(music, "Album E", 300);
    CHECK(library_scan(lib, music, NULL, NULL) == 614 && library_scan_quick(lib) && library_scan_changes(lib) == 300);
    CHECK(library_scan(lib, music, NULL, NULL) == 614 && library_scan_quick(lib) && library_scan_changes(lib) == 0);

    /* the FAT reading misses a folder's file, and the check of its list against
     * the library stops at a failed step: not "checked" (the folders are
     * walked, and the file stays; taken as checked, it was dropped as gone) */
    fw_set("Album A/003", NULL, NULL);
    library_test_against_fails(2);
    CHECK(library_scan(lib, music, NULL, NULL) == 614 && !library_scan_quick(lib));
    library_test_against_fails(0);
    CHECK(t_db_int(db, "SELECT COUNT(*) FROM tracks WHERE path LIKE '%Album A/003.flac'") == 1);
    fw_set(NULL, NULL, NULL);

    library_test_fatwalk(NULL);
    library_close(lib);
}

/* ---- the groups rebuilt only where songs changed ----
 * A library of songs whose tags overlap every way grouping cares about
 * (artists spelt differently, "The", guests, album artists and Various
 * Artists, genres and composers in two cases, years, folders in folders),
 * changed at random over many rescans: after each, a full rebuild on top
 * changes nothing (the same albums, artists, credits, genres, composers and
 * folders, ids and all). */

static unsigned rnd_state = 12345;
static unsigned rnd(unsigned n) { rnd_state = rnd_state * 1103515245u + 12345u; return (rnd_state >> 16) % n; }

#define GW_MAX 400
static char gw_path[GW_MAX][400];
static int gw_n;

static void gw_write(const char *music, int i, int fresh)
{
    static const char *const artists[] = {"Alpha", "The Beta", "beta", "Gamma feat. Delta", "Delta", "", "Ëpsilon", "Zeta & Eta"};
    static const char *const aas[] = {"", "", "Alpha", "Various Artists", "VA", "Beta", "Delta"};
    static const char *const albums[] = {"One", "Two", "two", "Three", "", "Ünder"};
    static const char *const genres[] = {"Rock", "rock", "Jazz", "", "Pop"};
    static const char *const composers[] = {"Bach", "bach", "", "", "Ravel"};
    static const int years[] = {0, 1999, 2001, 1987, 0};
    static const char *const dirs[] = {"A", "A/B", "C", "C/D/E", "F", "A/B/G"};
    if (fresh) {
        snprintf(gw_path[i], sizeof gw_path[i], "%s/%s/%03d %u.flac", music, dirs[rnd(6)], i, rnd(1000));
        char d[400];
        snprintf(d, sizeof d, "%s", gw_path[i]);
        for (char *p = d + strlen(music) + 1; (p = strchr(p, '/')); p++) { *p = '\0'; mkdir(d, 0755); *p = '/'; }
    }
    char title[64];
    snprintf(title, sizeof title, "Song %d%s", i, rnd(5) == 0 ? " (feat. Theta)" : "");
    if (write_flac_tags(gw_path[i], title, artists[rnd(8)], aas[rnd(7)], albums[rnd(6)], genres[rnd(5)],
                        composers[rnd(5)], years[rnd(5)], 1 + (int)rnd(12))) {
        fprintf(stderr, "cannot write %s\n", gw_path[i]);
        exit(1);
    }
    struct timeval tv[2] = {{time(NULL) + 100 + i + (long)rnd(100000), 0}, {time(NULL) + 100 + i + (long)rnd(100000), 0}};
    utimes(gw_path[i], tv);                          /* (a change shows even within the second) */
}

static void test_groups_where_changed(const char *dir)
{
    char music[300], db[300], art[300];
    snprintf(music, sizeof music, "%s/gmusic", dir);
    mkdir(music, 0755);
    snprintf(db, sizeof db, "%s/groups.db", dir);
    snprintf(art, sizeof art, "%s/groups-art", dir);
    if (getenv("GW_SEED")) rnd_state = (unsigned)atoi(getenv("GW_SEED"));   /* (more libraries: GW_SEED=n) */
    for (gw_n = 0; gw_n < 80; gw_n++) gw_write(music, gw_n, 1);
    library_t *lib = library_open(db, art);
    CHECK(lib && library_scan(lib, music, NULL, NULL) == gw_n && library_scan_groups(lib) == 2);   /* (the first: all) */
    int incremental = 0, rounds = 40;
    for (int r = 0; r < rounds; r++) {
        int ops = 1 + (int)rnd(r % 7 == 6 ? 25 : 5);
        for (int k = 0; k < ops; k++) {
            int what = (int)rnd(10), i = (int)rnd((unsigned)gw_n);
            if (what < 3 && gw_n < GW_MAX) { gw_write(music, gw_n, 1); gw_n++; }        /* added */
            else if (what < 5 && gw_n > 10) {                                          /* removed */
                remove(gw_path[i]);
                memcpy(gw_path[i], gw_path[gw_n - 1], sizeof gw_path[i]);
                gw_n--;
            } else if (what < 9) gw_write(music, i, 0);                                /* retagged */
            else {                                                                     /* moved */
                char old[400];
                snprintf(old, sizeof old, "%s", gw_path[i]);
                gw_write(music, i, 1);
                remove(gw_path[i]);
                CHECK(rename(old, gw_path[i]) == 0);
            }
        }
        CHECK(library_scan(lib, music, NULL, NULL) == gw_n);
        incremental += library_scan_groups(lib) == 1;
        CHECK(t_db_int(db, "SELECT (SELECT COUNT(*) FROM tracks WHERE search IS NULL) + (SELECT COUNT(*) FROM albums"
                         " WHERE search IS NULL) + (SELECT COUNT(*) FROM artists WHERE search IS NULL)") == 0);   /* (all findable) */
        char *before = groups_text(db);
        library_test_rebuild_groups(lib);
        char *after = groups_text(db);
        int same = before && after && !strcmp(before, after);
        CHECK(same);
        /* ...and the rebuild's shortcuts (what the track before has written
         * isn't written again) change nothing: the same with every statement run */
        library_test_groups_plain(1);
        library_test_rebuild_groups(lib);
        library_test_groups_plain(0);
        char *plain = groups_text(db);
        int same_plain = plain && after && !strcmp(plain, after);
        CHECK(same_plain);
        if (!same_plain && plain && after) {
            const char *a = after, *b = plain;
            while (*a && *a == *b) { a++; b++; }
            while (a > after && a[-1] != '\n') { a--; b--; }
            fprintf(stderr, "round %d: shortcuts: %.200s\n  plain: %.200s\n", r, a, b);
        }
        sqlite3_free(plain);
        same = same && same_plain;
        if (!same && before && after) {                /* the first line that differs */
            const char *a = before, *b = after;
            while (*a && *a == *b) { a++; b++; }
            while (a > before && a[-1] != '\n') { a--; b--; }
            fprintf(stderr, "round %d: where changed: %.200s\n  all: %.200s\n", r, a, b);
        }
        sqlite3_free(before);
        sqlite3_free(after);
        if (!same) break;
    }
    CHECK(incremental >= rounds / 2);                  /* (most rounds took the quick way) */
    printf("groups where changed: %d of %d rounds\n", incremental, rounds);
    library_close(lib);
}

/* ---- a first scan loads in bulk: the tracks' indexes set aside, built at the end ----
 * The same library as one scanned the slow way; and cut short mid-scan, the
 * next open builds the indexes before anything else. */
static int64_t track_indexes(const char *db)
{
    return t_db_int(db, "SELECT COUNT(*) FROM sqlite_master WHERE type='index' AND tbl_name='tracks' AND sql IS NOT NULL");
}

static void test_bulk_load(const char *dir)
{
    char music[300], db[300], db2[300], art[300], name[32];
    snprintf(music, sizeof music, "%s/bmusic", dir);
    mkdir(music, 0755);
    snprintf(db, sizeof db, "%s/bulk.db", dir);
    snprintf(db2, sizeof db2, "%s/plain.db", dir);
    snprintf(art, sizeof art, "%s/bulk-art", dir);
    for (int a = 0; a < 6; a++) {
        snprintf(name, sizeof name, "Bulk %02d", a);
        make_album(music, name, 10);
    }
    /* the usual way (too few files for bulk) */
    library_t *lib = library_open(db2, art);
    CHECK(lib && library_scan(lib, music, NULL, NULL) == 60);
    library_close(lib);
    int64_t want = track_indexes(db2);
    CHECK(want >= 15);
    char *plain = groups_text(db2);
    /* in bulk */
    library_test_bulk_min(20);
    lib = library_open(db, art);
    CHECK(lib && track_indexes(db) == want);
    CHECK(library_scan(lib, music, NULL, NULL) == 60);
    CHECK(track_indexes(db) == want);                                /* all back */
    CHECK(t_db_int(db, "SELECT COUNT(*) FROM sqlite_master WHERE name='bulk_idx'") == 0);
    CHECK(library_check_plans(lib, NULL) == 0 && library_upgrade_needed(db) == 0);
    library_close(lib);
    char *bulk = groups_text(db);
    CHECK(plain && bulk && !strcmp(plain, bulk));                    /* the same library */
    sqlite3_free(plain);
    sqlite3_free(bulk);
    /* a second scan isn't a first one */
    lib = library_open(db, art);
    snprintf(name, sizeof name, "Bulk %02d", 7);
    make_album(music, name, 30);
    CHECK(lib && library_scan(lib, music, NULL, NULL) == 90 && track_indexes(db) == want);
    library_close(lib);
    /* cut short while the indexes are aside (switched off mid-scan): the next
     * open builds them, and says beforehand that it has work to do */
    char db3[300];
    snprintf(db3, sizeof db3, "%s/bulk-cut.db", dir);
    scan_job_t job = {.lib = library_open(db3, art), .music = music, .cancel_after = 2};
    CHECK(job.lib != NULL);
    job_run(&job);
    CHECK(job.result == -1);
    CHECK(track_indexes(db3) == 0 && t_db_int(db3, "SELECT COUNT(*) FROM bulk_idx") == want);
    /* a song ends while that first scan is still going: its play is written by
     * the library's own thread, whose connection leaves the indexes aside (as
     * a whole library opened there, it built them over the half-filled table,
     * and the rest of the scan paid for every one) */
    library_played_later(job.lib, t_db_int(db3, "SELECT COALESCE(MIN(id), 1) FROM tracks"), 0, 1795000000);
    library_plays_wait();
    CHECK(track_indexes(db3) == 0 && t_db_int(db3, "SELECT COUNT(*) FROM bulk_idx") == want);
    library_close(job.lib);
    CHECK(library_upgrade_needed(db3) > 0);
    up_calls = 0;
    library_set_upgrade_progress(up_seen, NULL);
    lib = library_open(db3, art);
    library_set_upgrade_progress(NULL, NULL);
    CHECK(lib && track_indexes(db3) == want && up_calls == (int)want);
    CHECK(t_db_int(db3, "SELECT COUNT(*) FROM sqlite_master WHERE name='bulk_idx'") == 0);
    CHECK(library_upgrade_needed(db3) == 0 && library_scan_unfinished(lib));
    CHECK(library_scan(lib, music, NULL, NULL) == 90 && library_check_plans(lib, NULL) == 0);   /* finished, not in bulk */
    library_close(lib);
    /* an index that can't be made at that open (the card full; here, one that
     * names no table): the list of what is still to build is kept, the others
     * are built, and the next open, with nothing in the way, finishes and drops it */
    char db5[300];
    snprintf(db5, sizeof db5, "%s/bulk-cut2.db", dir);
    scan_job_t job2 = {.lib = library_open(db5, art), .music = music, .cancel_after = 2};
    CHECK(job2.lib != NULL);
    job_run(&job2);
    library_close(job2.lib);
    CHECK(t_db_int(db5, "SELECT COUNT(*) FROM bulk_idx") == want);
    CHECK(t_db_exec(db5, "INSERT INTO bulk_idx(name, sql) VALUES('nope', 'CREATE INDEX nope ON no_such_table(x)')") == SQLITE_OK);
    lib = library_open(db5, art);
    CHECK(lib && track_indexes(db5) == want);
    CHECK(t_db_int(db5, "SELECT COUNT(*) FROM sqlite_master WHERE name='bulk_idx'") == 1 && library_upgrade_needed(db5) > 0);
    library_close(lib);
    CHECK(t_db_exec(db5, "DELETE FROM bulk_idx WHERE name='nope'") == SQLITE_OK);
    lib = library_open(db5, art);
    CHECK(lib && track_indexes(db5) == want);
    CHECK(t_db_int(db5, "SELECT COUNT(*) FROM sqlite_master WHERE name='bulk_idx'") == 0 && library_upgrade_needed(db5) == 0);
    library_close(lib);
    library_test_bulk_min(0);
}

/* ---- a first scan groups each song as it's added ----
 * The same library as the full pass over every track makes (a rebuild after
 * it changes nothing; names and counts as a library scanned the old way);
 * and with a file split by a CUE sheet, the old way is what runs. */
static char *names_text(const char *db)
{
    static const char *const Q[] = {
        "SELECT t.path, al.title, al.year, al.ntracks, ar.name, aa.name, g.name, c.name, f.path, t.sort, t.search"
        " FROM tracks t LEFT JOIN albums al ON al.id=t.album_id LEFT JOIN artists ar ON ar.id=t.artist_id"
        " LEFT JOIN artists aa ON aa.id=al.artist_id LEFT JOIN genres g ON g.id=t.genre_id"
        " LEFT JOIN composers c ON c.id=t.composer_id LEFT JOIN folders f ON f.id=t.folder_id ORDER BY t.path",
        "SELECT name, sort, nalbums, ncredit_albums, search FROM artists ORDER BY sort, name COLLATE BINARY",
        ("SELECT a.title, a.sort, a.year, a.ntracks, a.dir, a.search, t.path FROM albums a LEFT JOIN tracks t"
         " ON t.id=a.first_track ORDER BY a.dir, a.sort, a.title COLLATE BINARY"),
        "SELECT name, sort, nalbums FROM genres ORDER BY name COLLATE BINARY",
        "SELECT name, sort, nalbums FROM composers ORDER BY name COLLATE BINARY",
        "SELECT f.path, f.name, f.sort, f.ntracks, p.path FROM folders f LEFT JOIN folders p ON p.id=f.parent ORDER BY f.path",
        "SELECT a.name, t.path FROM credits c JOIN artists a ON a.id=c.artist_id JOIN tracks t ON t.id=c.track_id ORDER BY 1, 2",
    };
    sqlite3 *raw = NULL;
    sqlite3_str *out = NULL;
    if (sqlite3_open(db, &raw) == SQLITE_OK) out = sqlite3_str_new(raw);
    for (size_t i = 0; out && i < sizeof Q / sizeof *Q; i++) {
        sqlite3_stmt *q = NULL;
        if (sqlite3_prepare_v2(raw, Q[i], -1, &q, NULL) != SQLITE_OK) { sqlite3_str_appendf(out, "(%s: %s)", Q[i], sqlite3_errmsg(raw)); continue; }
        sqlite3_str_appendf(out, "-- %d\n", (int)i);
        while (sqlite3_step(q) == SQLITE_ROW) {
            for (int c = 0; c < sqlite3_column_count(q); c++) {
                const unsigned char *v = sqlite3_column_text(q, c);
                sqlite3_str_appendf(out, "%s | ", v ? (const char *)v : "NULL");
            }
            sqlite3_str_appendchar(out, 1, '\n');
        }
        sqlite3_finalize(q);
    }
    char *text = out ? sqlite3_str_finish(out) : NULL;
    sqlite3_close(raw);
    return text;
}

static void first_diff(const char *what, const char *a0, const char *b0)
{
    const char *a = a0, *b = b0;
    while (*a && *a == *b) { a++; b++; }
    while (a > a0 && a[-1] != '\n') { a--; b--; }
    fprintf(stderr, "%s: %.300s\n  against: %.300s\n", what, a, b);
}

static void test_groups_as_added(const char *dir)
{
    char music[300], db[300], db2[300], db3[300], art[300], path[420];
    snprintf(music, sizeof music, "%s/amusic", dir);
    mkdir(music, 0755);
    snprintf(db, sizeof db, "%s/direct.db", dir);
    snprintf(db2, sizeof db2, "%s/direct-old.db", dir);
    snprintf(db3, sizeof db3, "%s/direct-cue.db", dir);
    snprintf(art, sizeof art, "%s/direct-art", dir);
    for (gw_n = 0; gw_n < 160; gw_n++) gw_write(music, gw_n, 1);
    /* (a file that can't be read among them: noted, by name, to the end of
     * the scan, while the others' names are freed as their songs come in) */
    snprintf(path, sizeof path, "%s/C/junk.flac", music);
    FILE *junk = fopen(path, "wb");
    if (junk) { fputs("junk, not a FLAC", junk); fclose(junk); }
    library_test_bulk_min(20);
    library_t *lib = library_open(db, art);
    CHECK(lib && library_scan(lib, music, NULL, NULL) == gw_n && library_scan_groups(lib) == 3);   /* (3: as added) */
    CHECK(t_db_int(db, "SELECT COUNT(*) FROM cues WHERE path LIKE '%/C/junk.flac'") == 1);
    CHECK(t_db_int(db, "SELECT (SELECT COUNT(*) FROM tracks WHERE search IS NULL) + (SELECT COUNT(*) FROM albums"
                     " WHERE search IS NULL) + (SELECT COUNT(*) FROM artists WHERE search IS NULL)") == 0);
    CHECK(t_db_int(db, "SELECT COUNT(*) FROM tracks WHERE album_id IS NULL OR artist_id IS NULL OR folder_id IS NULL") == 0);
    CHECK(t_db_int(db, "SELECT value FROM meta WHERE key='scan_pending'") == 0);
    CHECK(library_check_plans(lib, NULL) == 0 && track_indexes(db) >= 15);
    char *added = groups_text(db), *added_names = names_text(db);
    library_test_groups_plain(1);
    library_test_rebuild_groups(lib);              /* every track again, every statement run */
    library_test_groups_plain(0);
    char *all = groups_text(db);
    int same = added && all && !strcmp(added, all);
    CHECK(same);
    if (!same && added && all) first_diff("as added", added, all);
    /* a second scan: nothing to do (the unreadable file still noted, not read again) */
    CHECK(library_scan(lib, music, NULL, NULL) == gw_n && library_scan_changes(lib) == 0);
    CHECK(t_db_int(db, "SELECT COUNT(*) FROM cues WHERE path LIKE '%/C/junk.flac'") == 1);
    library_close(lib);
    /* the same music scanned the way it was before */
    library_test_groups_direct(0);
    lib = library_open(db2, art);
    CHECK(lib && library_scan(lib, music, NULL, NULL) == gw_n && library_scan_groups(lib) == 2);
    library_close(lib);
    library_test_groups_direct(1);
    char *old_names = names_text(db2);
    same = added_names && old_names && !strcmp(added_names, old_names);
    CHECK(same);
    if (!same && added_names && old_names) first_diff("as added", added_names, old_names);
    sqlite3_free(added);
    sqlite3_free(all);
    sqlite3_free(added_names);
    sqlite3_free(old_names);
    /* a file split by a CUE sheet: its tracks don't come one by one, so the
     * groups are all made afterwards, and come out as a rebuild makes them */
    snprintf(path, sizeof path, "%s/A/whole.flac", music);
    CHECK(write_flac(path, "Whole", "Alpha", "Sheet", 1) == 0);
    snprintf(path, sizeof path, "%s/A/whole.cue", music);
    FILE *f = fopen(path, "wb");
    if (f) {
        fputs("PERFORMER \"Alpha\"\nTITLE \"Sheet\"\nFILE \"whole.flac\" WAVE\n  TRACK 01 AUDIO\n    TITLE \"First\"\n"
              "    INDEX 01 00:00:00\n  TRACK 02 AUDIO\n    TITLE \"Second\"\n    INDEX 01 00:00:01\n", f);
        fclose(f);
    }
    lib = library_open(db3, art);
    int n = lib ? library_scan(lib, music, NULL, NULL) : -1;
    CHECK(n >= gw_n + 1 && library_scan_groups(lib) == 2);
    printf("groups as added: with a sheet, %d tracks (%lld from sheets)\n", n, (long long)t_db_int(db3, "SELECT COUNT(*) FROM tracks WHERE cue<>0"));
    CHECK(t_db_int(db3, "SELECT COUNT(*) FROM tracks WHERE cue<>0") >= 2);
    char *cue = groups_text(db3);
    library_test_rebuild_groups(lib);
    char *cue_all = groups_text(db3);
    same = cue && cue_all && !strcmp(cue, cue_all);
    CHECK(same);
    if (!same && cue && cue_all) first_diff("with a sheet", cue, cue_all);
    sqlite3_free(cue);
    sqlite3_free(cue_all);
    library_close(lib);
    library_test_bulk_min(0);
}

/* ---- a rescan holds by name only the files that are new or changed ----
 * The rest are ticked off against a table of the library's files as the
 * folder is listed. The same result as holding every name (the two ways run
 * side by side over the same changes), through both listings. */
static void test_known_files(const char *dir)
{
    char music[300], db[300], db2[300], art[300], path[420];
    snprintf(music, sizeof music, "%s/kmusic", dir);
    mkdir(music, 0755);
    snprintf(db, sizeof db, "%s/known.db", dir);
    snprintf(db2, sizeof db2, "%s/known-old.db", dir);
    snprintf(art, sizeof art, "%s/known-art", dir);
    for (gw_n = 0; gw_n < 150; gw_n++) gw_write(music, gw_n, 1);
    library_t *lib = library_open(db, art), *old = library_open(db2, art);
    CHECK(lib && old);
    CHECK(library_scan(lib, music, NULL, NULL) == gw_n && library_scan_held(lib) == gw_n);   /* (a first scan: all) */
    library_test_known(0);
    CHECK(library_scan(old, music, NULL, NULL) == gw_n);
    library_test_known(1);
    /* nothing changed: no name held, nothing written */
    CHECK(library_scan(lib, music, NULL, NULL) == gw_n && library_scan_held(lib) == 0 && library_scan_changes(lib) == 0);
    for (int r = 0; r < 12; r++) {
        int added = 0, touched = 0;
        int ops = 1 + (int)rnd(6);
        for (int k = 0; k < ops; k++) {
            int what = (int)rnd(10), i = (int)rnd((unsigned)gw_n);
            if (what < 3 && gw_n < GW_MAX) { gw_write(music, gw_n, 1); gw_n++; added++; }
            else if (what < 5 && gw_n > 10) {
                remove(gw_path[i]);
                memcpy(gw_path[i], gw_path[gw_n - 1], sizeof gw_path[i]);
                gw_n--;
            } else if (what < 9) { gw_write(music, i, 0); touched++; }
            else {
                char was[400];
                snprintf(was, sizeof was, "%s", gw_path[i]);
                gw_write(music, i, 1);
                remove(gw_path[i]);
                CHECK(rename(was, gw_path[i]) == 0);
                added++;
            }
        }
        /* (odd rounds: the fast listing, here the test's own reading of the folder) */
        if (r & 1) {
            fw_set(NULL, NULL, NULL);
            library_test_fatwalk(fake_fatwalk);
            library_set_fast_walk(lib, "fake", dir);
            library_set_fast_walk(old, "fake", dir);
        }
        CHECK(library_scan(lib, music, NULL, NULL) == gw_n);
        int held = library_scan_held(lib), changes = library_scan_changes(lib);
        CHECK(held <= added + touched);                           /* (only what's new or changed, by name) */
        CHECK(!(r & 1) || library_scan_quick(lib));
        library_test_known(0);
        CHECK(library_scan(old, music, NULL, NULL) == gw_n && library_scan_held(old) == gw_n);
        library_test_known(1);
        CHECK(library_scan_changes(old) == changes);
        if (r & 1) { library_set_fast_walk(lib, NULL, NULL); library_set_fast_walk(old, NULL, NULL); library_test_fatwalk(NULL); }
        char *a = names_text(db), *b = names_text(db2);
        int same = a && b && !strcmp(a, b);
        CHECK(same);
        if (!same && a && b) first_diff("names ticked off", a, b);
        sqlite3_free(a);
        sqlite3_free(b);
        CHECK(t_db_int(db, "SELECT value FROM meta WHERE key='scan_sig'") == t_db_int(db2, "SELECT value FROM meta WHERE key='scan_sig'"));
        if (!same) break;
    }
    /* stopped while reading new files, others removed: nothing dropped; the next scan drops them */
    int before = gw_n;
    for (int k = 0; k < 6; k++) { gw_write(music, gw_n, 1); gw_n++; }
    remove(gw_path[0]);
    remove(gw_path[1]);
    scan_job_t stop = {.lib = lib, .music = music, .cancel_after = 2};
    CHECK(library_scan(lib, music, job_progress, &stop) == -1);
    CHECK(t_db_int(db, "SELECT COUNT(*) FROM tracks") >= before);
    CHECK(library_scan(lib, music, NULL, NULL) == gw_n - 2);
    /* a CUE sheet turns up: every file by name again (the sheet's file is looked for among them) */
    snprintf(path, sizeof path, "%s/A/whole.flac", music);
    CHECK(write_flac(path, "Whole", "Alpha", "Sheet", 1) == 0);
    snprintf(path, sizeof path, "%s/A/whole.cue", music);
    FILE *f = fopen(path, "wb");
    if (f) {
        fputs("PERFORMER \"Alpha\"\nTITLE \"Sheet\"\nFILE \"whole.flac\" WAVE\n  TRACK 01 AUDIO\n    TITLE \"First\"\n"
              "    INDEX 01 00:00:00\n  TRACK 02 AUDIO\n    TITLE \"Second\"\n    INDEX 01 00:00:01\n", f);
        fclose(f);
    }
    int n = library_scan(lib, music, NULL, NULL);
    CHECK(n == gw_n - 2 + 2 && library_scan_held(lib) == gw_n - 2 + 1);
    CHECK(t_db_int(db, "SELECT COUNT(*) FROM tracks WHERE cue<>0") == 2);
    CHECK(library_scan(lib, music, NULL, NULL) == n && library_scan_changes(lib) == 0);
    library_close(lib);
    library_close(old);
}

/* ---- a file the player dies on ----
 * The note of what was being read, left in RAM by a run that died: once, the
 * file is tried again; twice running, it's set aside as unreadable and the
 * scan carries on with the rest. */
static void trying_write(const char *dir, const char *what, const char *text)
{
    char p[400];
    snprintf(p, sizeof p, "%s/fathom-trying-%s", dir, what);
    FILE *f = fopen(p, "wb");
    if (f) { fwrite(text, 1, strlen(text) + 1, f); fclose(f); }
}

static void test_died_on(const char *dir)
{
    char music[300], db[300], art[300], notes[300], bad[400], p[400];
    snprintf(music, sizeof music, "%s/dmusic", dir);
    mkdir(music, 0755);
    snprintf(db, sizeof db, "%s/died.db", dir);
    snprintf(art, sizeof art, "%s/died-art", dir);
    snprintf(notes, sizeof notes, "%s/notes", dir);
    mkdir(notes, 0755);
    make_album(music, "Died", 6);
    snprintf(bad, sizeof bad, "%s/Died/003.flac", music);
    library_set_trying_dir(notes);
    library_t *lib = library_open(db, art);
    /* once: tried again, and (it reads fine) it's in */
    trying_write(notes, "tags", bad);
    CHECK(lib && library_scan(lib, music, NULL, NULL) == 6);
    snprintf(p, sizeof p, "%s/fathom-trying-tags", notes);
    CHECK(access(p, F_OK) != 0);                                 /* (a finished scan leaves no note) */
    snprintf(p, sizeof p, "%s/fathom-trying-tags.1", notes);
    CHECK(access(p, F_OK) == 0);                                 /* (remembered) */
    library_close(lib);
    /* twice running, on a new library: set aside, the others in */
    char db2[300];
    snprintf(db2, sizeof db2, "%s/died2.db", dir);
    trying_write(notes, "tags", bad);
    lib = library_open(db2, art);
    CHECK(lib && library_scan(lib, music, NULL, NULL) == 5);
    CHECK(t_db_int(db2, "SELECT COUNT(*) FROM tracks WHERE path LIKE '%Died/003.flac'") == 0);
    CHECK(t_db_int(db2, "SELECT COUNT(*) FROM cues WHERE path LIKE '%Died/003.flac' AND kind=3") == 1);
    CHECK(access(p, F_OK) != 0);
    /* ...and stays so while it's the same file, without being read again */
    CHECK(library_scan(lib, music, NULL, NULL) == 5 && library_scan_changes(lib) == 0);
    /* another file's death in between starts the count again */
    char other[400];
    snprintf(other, sizeof other, "%s/Died/005.flac", music);
    make_album(music, "Died More", 2);
    trying_write(notes, "tags", other);
    CHECK(library_scan(lib, music, NULL, NULL) == 7);            /* (once: nothing set aside) */
    library_close(lib);
    library_set_trying_dir(NULL);
}

/* ---- two files the player dies on, side by side ----
 * The first, set aside, is written down before anything else is read (the
 * player dying on the second would otherwise have it forgotten, and the two
 * tried in turn for ever: a user's MP3 album, 0.17.0); and the second, in the
 * folder of one already set aside, goes at its first death. */
typedef struct { const char *db; const char *sql; int seen, calls; } at_start_t;

static void at_start(int done, int total, const char *path, void *ctx)
{
    (void)total;
    at_start_t *a = ctx;
    if (path && (!strcmp(path, "looking") || !strcmp(path, "checking") || !strcmp(path, "sorting") || !strcmp(path, "working"))) return;
    if (done == 1 && !a->calls++) a->seen = t_db_int(a->db, a->sql);   /* (before the first file is read) */
}

static void test_died_side_by_side(const char *dir)
{
    char music[300], db[300], art[300], notes[300], a[400], b[400], c[400], p[400];
    snprintf(music, sizeof music, "%s/smusic", dir);
    mkdir(music, 0755);
    snprintf(db, sizeof db, "%s/side.db", dir);
    snprintf(art, sizeof art, "%s/side-art", dir);
    snprintf(notes, sizeof notes, "%s/side-notes", dir);
    mkdir(notes, 0755);
    make_album(music, "Bad Album", 4);
    make_album(music, "Fine Album", 3);
    snprintf(a, sizeof a, "%s/Bad Album/002.flac", music);
    snprintf(b, sizeof b, "%s/Bad Album/005.flac", music);
    snprintf(c, sizeof c, "%s/Fine Album/004.flac", music);
    library_set_trying_dir(notes);
    library_t *lib = library_open(db, art);
    /* A, twice running: set aside, and in the library before the scan reads a file */
    trying_write(notes, "tags", a);
    trying_write(notes, "tags.1", a);
    at_start_t st = {db, "SELECT COUNT(*) FROM cues WHERE kind=3 AND sheet='stopped' AND path LIKE '%Bad Album/002.flac'", 0, 0};
    CHECK(lib && library_scan(lib, music, at_start, &st) == 6);
    CHECK(st.calls == 1 && st.seen == 1);
    /* B, new beside it, once: no second chance */
    if (write_flac(b, "Bad 5", "Some Artist", "Bad Album", 5)) exit(1);
    trying_write(notes, "tags", b);
    CHECK(library_scan(lib, music, NULL, NULL) == 6);
    CHECK(t_db_int(db, "SELECT COUNT(*) FROM cues WHERE kind=3 AND sheet='stopped'") == 2);
    CHECK(t_db_int(db, "SELECT COUNT(*) FROM tracks WHERE path LIKE '%Bad Album/00_.flac'") == 3);
    /* a new file in another folder, once: tried again as ever (and it reads) */
    if (write_flac(c, "Fine 4", "Some Artist", "Fine Album", 4)) exit(1);
    trying_write(notes, "tags", c);
    CHECK(library_scan(lib, music, NULL, NULL) == 7);
    CHECK(t_db_int(db, "SELECT COUNT(*) FROM cues WHERE kind=3") == 2);
    /* one named by a note but in the library as it is (the player stopped on
     * something else, after it): never set aside, here beside two that are */
    snprintf(p, sizeof p, "%s/Bad Album/001.flac", music);
    trying_write(notes, "tags", p);
    snprintf(p, sizeof p, "%s/Fine Album/005.flac", music);
    if (write_flac(p, "Fine 5", "Some Artist", "Fine Album", 5)) exit(1);
    CHECK(library_scan(lib, music, NULL, NULL) == 8);
    CHECK(t_db_int(db, "SELECT COUNT(*) FROM cues WHERE kind=3") == 2);
    /* and both stay aside at later scans, unread, with nothing written */
    CHECK(library_scan(lib, music, NULL, NULL) == 8 && library_scan_changes(lib) == 0);
    /* a file that simply can't be read isn't one the player stopped on */
    snprintf(p, sizeof p, "%s/Fine Album/junk.flac", music);
    FILE *f = fopen(p, "wb");
    if (f) { fputs("not a flac at all", f); fclose(f); }
    CHECK(library_scan(lib, music, NULL, NULL) == 8);
    CHECK(t_db_int(db, "SELECT COUNT(*) FROM cues WHERE kind=3 AND sheet IS NULL") == 1);
    CHECK(library_unread_count(lib) == 3);
    char got[400] = "";
    CHECK(library_unread(lib, 0, got, sizeof got) == 0 && !strcmp(got, a));
    CHECK(library_unread(lib, 1, got, sizeof got) == 0 && !strcmp(got, b));
    CHECK(library_unread(lib, 2, got, sizeof got) == 0 && !strcmp(got, p));
    CHECK(library_unread(lib, 3, got, sizeof got) != 0);
    library_close(lib);
    library_set_trying_dir(NULL);
}

/* ---- files that couldn't be read, tried again when a scan is asked to ----
 * A file left out stays out while it's the same file (its date and size):
 * an ordinary scan doesn't read it again. A scan asked to (Rescan) does: one
 * that reads now comes in. One a reader stopped on is tried again only when
 * a helper does the reading (not here: tests/test_reader.c). */
/* `from`'s bytes as `path`, dated `when` (a file replaced by one of the same size and date) */
static void same_size_file(const char *path, const char *from, time_t when)
{
    FILE *in = fopen(from, "rb"), *out = fopen(path, "wb");
    char b[65536];
    size_t n = in ? fread(b, 1, sizeof b, in) : 0;
    if (in) fclose(in);
    if (out) { fwrite(b, 1, n, out); fclose(out); }
    struct timeval tv[2] = {{when, 0}, {when, 0}};
    utimes(path, tv);
}

static void test_retry_unread(const char *dir)
{
    char music[300], db[300], art[300], good[400], later[400], stopped[400];
    snprintf(music, sizeof music, "%s/tmusic", dir);
    mkdir(music, 0755);
    snprintf(db, sizeof db, "%s/retry.db", dir);
    snprintf(art, sizeof art, "%s/retry-art", dir);
    make_album(music, "Retry", 3);
    snprintf(good, sizeof good, "%s/good.flac.tmp", dir);
    if (write_flac(good, "Late One", "Some Artist", "Retry", 9)) exit(1);
    snprintf(later, sizeof later, "%s/Retry/later.flac", music);
    snprintf(stopped, sizeof stopped, "%s/Retry/stopped.flac", music);
    const time_t when = 1700000000;
    struct stat sb;
    stat(good, &sb);
    char junk[65536];
    memset(junk, 'j', sizeof junk);
    for (int k = 0; k < 2; k++) {                                /* two files of junk, the good one's size */
        FILE *f = fopen(k ? stopped : later, "wb");
        if (f) { fwrite(junk, 1, (size_t)sb.st_size < sizeof junk ? (size_t)sb.st_size : sizeof junk, f); fclose(f); }
        struct timeval tv[2] = {{when, 0}, {when, 0}};
        utimes(k ? stopped : later, tv);
    }
    library_t *lib = library_open(db, art);
    CHECK(lib && library_scan(lib, music, NULL, NULL) == 3);
    CHECK(library_unread_count(lib) == 2);
    /* one of them as a file a reader stopped on */
    CHECK(t_db_exec(db, "UPDATE cues SET sheet='stopped' WHERE path LIKE '%stopped.flac'") == SQLITE_OK);
    /* both now good files, of the same date and size: an ordinary scan leaves them out still */
    same_size_file(later, good, when);
    same_size_file(stopped, good, when);
    CHECK(library_scan(lib, music, NULL, NULL) == 3 && library_scan_changes(lib) == 0);
    CHECK(library_unread_count(lib) == 2);
    /* asked to try again: the damaged one is read and comes in; the one a reader
     * stopped on stays out (no helper here to take its place) */
    library_scan_retry_unread(lib, 1);
    CHECK(library_scan(lib, music, NULL, NULL) == 4);
    CHECK(t_db_int(db, "SELECT COUNT(*) FROM tracks WHERE path LIKE '%Retry/later.flac' AND title='Late One'") == 1);
    CHECK(library_unread_count(lib) == 1);
    CHECK(t_db_int(db, "SELECT COUNT(*) FROM cues WHERE kind=3 AND sheet='stopped' AND path LIKE '%stopped.flac'") == 1);
    /* asked of that one scan only: the next is an ordinary one, and writes nothing */
    CHECK(library_scan(lib, music, NULL, NULL) == 4 && library_scan_changes(lib) == 0);
    /* and with nothing to try again, a scan asked to is as quick as any (nothing changed) */
    CHECK(t_db_exec(db, "DELETE FROM cues WHERE kind=3") == SQLITE_OK);
    unlink(stopped);
    CHECK(library_scan(lib, music, NULL, NULL) == 4);
    library_scan_retry_unread(lib, 1);
    CHECK(library_scan(lib, music, NULL, NULL) == 4 && library_scan_changes(lib) == 0);
    library_close(lib);
    unlink(good);
}

/* ---- a scan says how far it has got at every step ----
 * While it lists the folder: the songs found, going up to all of them. While
 * it sorts albums and artists: the songs gone through, of all of them, and
 * for the rest of that step a number that only goes up. (Sent a few times a
 * second; at every chance here.) */
typedef struct { int looking, look_last, look_down, sorting, sort_last, sort_total, sort_bad, working, work_last, work_down, order_bad, phase, mixed; } told_t;

static void told(int done, int total, const char *path, void *ctx)
{
    told_t *t = ctx;
    if (!path) return;
    if (!strcmp(path, "looking")) {
        t->looking++;
        if (done < t->look_last) t->look_down = 1;
        t->look_last = done;
        if (t->phase > 0) t->order_bad = 1;                      /* (before any song is read) */
    } else if (!strcmp(path, "sorting")) {
        t->sorting++;
        if (done < t->sort_last || done > total) t->sort_bad = 1;
        t->sort_last = done;
        t->sort_total = total;
        if (t->phase < 1) t->order_bad = 1;                      /* (after the songs) */
    } else if (!strcmp(path, "working")) {
        t->working++;
        if (t->sorting && t->sort_last < t->sort_total) t->mixed = 1;   /* (in the middle of the songs' count: the two took turns) */
        if (done <= t->work_last) t->work_down = 1;
        t->work_last = done;
    } else if (strcmp(path, "checking") && strcmp(path, "albums") && strcmp(path, "indexes") && strcmp(path, "done") && total > 0) {
        t->phase = 1;                                            /* a song being read */
    }
}

static void test_progress_told(const char *dir)
{
    char music[300], db[300], art[300], name[32];
    snprintf(music, sizeof music, "%s/pmusic", dir);
    mkdir(music, 0755);
    snprintf(db, sizeof db, "%s/told.db", dir);
    snprintf(art, sizeof art, "%s/told-art", dir);
    for (int a = 0; a < 4; a++) {
        snprintf(name, sizeof name, "Told %d", a);
        make_album(music, name, 10);
    }
    library_test_progress_every(0);
    library_t *lib = library_open(db, art);
    told_t t = {0};
    CHECK(lib && library_scan(lib, music, told, &t) == 40);
    CHECK(t.looking >= 40 && t.look_last == 40 && !t.look_down);
    CHECK(t.sorting >= 1 && t.sort_total == 40 && t.sort_last == 40 && !t.sort_bad);
    CHECK(!t.work_down && !t.order_bad && !t.mixed);
    /* nothing new: the folder is listed again (and says so), no step after it */
    told_t u = {0};
    CHECK(library_scan(lib, music, told, &u) == 40 && library_scan_changes(lib) == 0);
    CHECK(u.looking >= 40 && u.sorting == 0 && u.working == 0);
    library_close(lib);
    library_test_progress_every(0.25);
}

/* ---- a file the H2 itself goes down on (a reset: the RAM's notes lost) ----
 * A run leaves an empty mark on the card while it reads; one found at the
 * next start makes that run note each item in it; and what such a run's mark
 * names is tried once more, then set aside. */
typedef struct { const char *mark; long size_at_25; int exists_at_25; } mark_look_t;

static void mark_look(int done, int total, const char *path, void *ctx)
{
    (void)total;
    mark_look_t *m = ctx;
    struct stat sb;
    if (path && (!strcmp(path, "looking") || !strcmp(path, "checking") || !strcmp(path, "sorting") || !strcmp(path, "working"))) return;
    if (done != 26) return;                                      /* (25 files read) */
    m->exists_at_25 = stat(m->mark, &sb) == 0;
    m->size_at_25 = m->exists_at_25 ? (long)sb.st_size : -1;
}

static void test_reset_on(const char *dir)
{
    char music[300], db[300], art[300], keep[300], mark[400], once[400], bad[400];
    snprintf(music, sizeof music, "%s/rmusic", dir);
    mkdir(music, 0755);
    snprintf(art, sizeof art, "%s/reset-art", dir);
    snprintf(keep, sizeof keep, "%s/reset-keep", dir);
    mkdir(keep, 0755);
    snprintf(mark, sizeof mark, "%s/scan-reading-tags", keep);
    snprintf(once, sizeof once, "%s/scan-reading-tags.1", keep);
    make_album(music, "Reset", 30);
    snprintf(bad, sizeof bad, "%s/Reset/007.flac", music);
    library_set_trying_keep(keep);
    /* an ordinary run: an empty mark while it reads, none after */
    snprintf(db, sizeof db, "%s/reset1.db", dir);
    library_t *lib = library_open(db, art);
    mark_look_t m = {mark, -1, 0};
    CHECK(lib && library_scan(lib, music, mark_look, &m) == 30);
    CHECK(m.exists_at_25 && m.size_at_25 == 0);
    CHECK(access(mark, F_OK) != 0);
    /* a scan that reads nothing leaves no mark at all */
    m = (mark_look_t){mark, -1, 0};
    CHECK(library_scan(lib, music, mark_look, &m) == 30 && !m.exists_at_25);
    library_close(lib);
    /* a mark left by a run the H2 went down in: this run names each file in it */
    snprintf(db, sizeof db, "%s/reset2.db", dir);
    trying_write(keep, "nothing", "");                           /* (some other file there: no matter) */
    FILE *f = fopen(mark, "wb");
    if (f) fclose(f);
    lib = library_open(db, art);
    m = (mark_look_t){mark, -1, 0};
    CHECK(lib && library_scan(lib, music, mark_look, &m) == 30);
    CHECK(m.exists_at_25 && m.size_at_25 > 10);
    CHECK(access(mark, F_OK) != 0 && access(once, F_OK) != 0);
    library_close(lib);
    /* such a run's mark names a file: once more (it reads: in), and the "once" goes with the run */
    snprintf(db, sizeof db, "%s/reset3.db", dir);
    f = fopen(mark, "wb");
    if (f) { fwrite(bad, 1, strlen(bad) + 1, f); fclose(f); }
    lib = library_open(db, art);
    CHECK(lib && library_scan(lib, music, NULL, NULL) == 30);
    CHECK(access(mark, F_OK) != 0 && access(once, F_OK) != 0);
    library_close(lib);
    /* twice running: set aside */
    snprintf(db, sizeof db, "%s/reset4.db", dir);
    f = fopen(mark, "wb");
    if (f) { fwrite(bad, 1, strlen(bad) + 1, f); fclose(f); }
    f = fopen(once, "wb");
    if (f) { fwrite(bad, 1, strlen(bad) + 1, f); fclose(f); }
    lib = library_open(db, art);
    CHECK(lib && library_scan(lib, music, NULL, NULL) == 29);
    CHECK(t_db_int(db, "SELECT COUNT(*) FROM cues WHERE kind=3 AND sheet='stopped' AND path LIKE '%Reset/007.flac'") == 1);
    CHECK(access(mark, F_OK) != 0 && access(once, F_OK) != 0);
    library_close(lib);
    /* the player died (its note in RAM) with a careful run's mark on the card:
     * the RAM's note is the one believed, and the run isn't careful */
    char notes[300];
    snprintf(notes, sizeof notes, "%s/reset-notes", dir);
    mkdir(notes, 0755);
    library_set_trying_dir(notes);
    snprintf(db, sizeof db, "%s/reset5.db", dir);
    trying_write(notes, "tags", bad);
    f = fopen(mark, "wb");
    if (f) { fputs("/somewhere/else.flac", f); fclose(f); }
    lib = library_open(db, art);
    m = (mark_look_t){mark, -1, 0};
    CHECK(lib && library_scan(lib, music, mark_look, &m) == 30);
    CHECK(m.exists_at_25 && m.size_at_25 == 0);
    CHECK(access(mark, F_OK) != 0);
    library_close(lib);
    library_set_trying_dir(NULL);
    library_set_trying_keep(NULL);
}

/* ---- after a stop that wasn't clean: the library checked; a damaged one's
 * play counts brought over to the one rebuilt ---- */
static void test_check_and_import(const char *dir)
{
    char music[300], db[300], db2[300], art[300], hurt[300];
    snprintf(music, sizeof music, "%s/cmusic", dir);
    mkdir(music, 0755);
    snprintf(db, sizeof db, "%s/check.db", dir);
    snprintf(db2, sizeof db2, "%s/check-new.db", dir);
    snprintf(hurt, sizeof hurt, "%s/check-hurt.db", dir);
    snprintf(art, sizeof art, "%s/check-art", dir);
    for (int a = 0; a < 12; a++) {
        char name[32];
        snprintf(name, sizeof name, "Check %02d", a);
        make_album(music, name, 20);
    }
    library_t *lib = library_open(db, art);
    CHECK(lib && library_scan(lib, music, NULL, NULL) == 240 && library_check(lib) == 0);
    {   /* a check that couldn't be made (no memory for it at that moment) says
         * nothing about the file: not "damaged", which sets the library aside
         * and reads the whole card again at the next start */
        library_test_check_fails(SQLITE_NOMEM);
        CHECK(lib && library_check(lib) == 0);
        library_test_check_fails(SQLITE_CORRUPT);                /* (the file's own word that it is damaged: that is) */
        CHECK(lib && library_check(lib) == -1);
        CHECK(lib && library_check(lib) == 0);                   /* (and the real check again) */
    }
    library_close(lib);                                          /* (its log emptied into the file) */
    CHECK(t_db_exec(db, "UPDATE tracks SET play_count=7, last_played=1700000000, favourite=1 WHERE path LIKE '%Check 03/005.flac';"
                      "UPDATE tracks SET skip_count=2, resume_ms=61000 WHERE path LIKE '%Check 07/001.flac';"
                      "INSERT INTO plays(track_id, at) SELECT id, 1700000000 FROM tracks WHERE path LIKE '%Check 03/005.flac'") == SQLITE_OK);
    /* a new library of the same music: what was played comes over by path */
    lib = library_open(db2, art);
    CHECK(lib && library_scan(lib, music, NULL, NULL) == 240);
    CHECK(library_import_counts(lib, db) == 2);
    CHECK(t_db_int(db2, "SELECT play_count*1000 + favourite FROM tracks WHERE path LIKE '%Check 03/005.flac'") == 7001);
    CHECK(t_db_int(db2, "SELECT skip_count*100000 + resume_ms FROM tracks WHERE path LIKE '%Check 07/001.flac'") == 261000);
    CHECK(t_db_int(db2, "SELECT COUNT(*) FROM plays p JOIN tracks t ON t.id=p.track_id WHERE t.path LIKE '%Check 03/005.flac'") == 1);
    CHECK(t_db_int(db2, "SELECT COUNT(*) FROM tracks WHERE play_count>0 OR skip_count>0 OR favourite=1") == 2);
    CHECK(library_import_counts(lib, "/no/such/file.db") <= 0);  /* (nothing to bring: no harm) */
    library_close(lib);
    /* a file with a stretch of it overwritten (a card's bad write): found damaged */
    FILE *in = fopen(db, "rb"), *out = fopen(hurt, "wb");
    char blk[4096];
    size_t k;
    long at = 0;
    CHECK(in && out);
    while (in && out && (k = fread(blk, 1, sizeof blk, in)) > 0) {
        if (at >= 8 * 4096 && at < 20 * 4096) memset(blk, 0x5a, k);
        fwrite(blk, 1, k, out);
        at += (long)k;
    }
    if (in) fclose(in);
    if (out) fclose(out);
    CHECK(at > 24 * 4096);
    lib = library_open(hurt, art);
    CHECK(!lib || library_check(lib) != 0);
    if (lib) library_close(lib);
    /* one page of its songs lost (the page holding a song from the middle):
     * the plays of the songs on the other pages still come over */
    char one[300], db4[300];
    snprintf(one, sizeof one, "%s/check-one.db", dir);
    snprintf(db4, sizeof db4, "%s/check-new2.db", dir);
    CHECK(t_db_exec(db, "UPDATE tracks SET play_count=3") == SQLITE_OK);
    in = fopen(db, "rb");
    out = fopen(one, "wb");
    int hit = 0;
    CHECK(in && out);
    while (in && out && (k = fread(blk, 1, sizeof blk, in)) > 0) {
        int has = 0;
        for (size_t i = 0; k == sizeof blk && i + 17 <= k; i++) has |= !memcmp(blk + i, "Check 05/010.flac", 17);
        if (has && blk[0] == 0x0d && !hit) { memset(blk, 0x5a, k); hit = 1; }   /* (a leaf of a table) */
        fwrite(blk, 1, k, out);
    }
    if (in) fclose(in);
    if (out) fclose(out);
    CHECK(hit);
    lib = library_open(db4, art);
    CHECK(lib && library_scan(lib, music, NULL, NULL) == 240);
    int got = library_import_counts(lib, one);
    printf("check: plays brought over from a file with a page lost: %d of 240\n", got);
    CHECK(got >= 100 && got < 240);
    CHECK(t_db_int(db4, "SELECT COUNT(*) FROM tracks WHERE play_count=3") == got);
    library_close(lib);
    /* A library set aside: its plays are owed until a scan of the new one has
     * finished, across restarts. The first scan cut short, the player started
     * again: still owed, brought over once the music is all read, once. */
    char db5[300], bad5[320], mark5[320], bad2[320];
    snprintf(db5, sizeof db5, "%s/check-owed.db", dir);
    snprintf(bad5, sizeof bad5, "%s.bad", db5);
    snprintf(mark5, sizeof mark5, "%s.bad.owed", db5);
    snprintf(bad2, sizeof bad2, "%s.bad2", db5);
    lib = library_open(db5, art);
    CHECK(lib && library_scan(lib, music, NULL, NULL) == 240);
    library_close(lib);
    CHECK(t_db_exec(db5, "UPDATE tracks SET play_count=4, favourite=1 WHERE path LIKE '%Check 03/%'") == SQLITE_OK);
    {   /* (a file that is no database at all: damaged; one that is, or isn't there: not known to be) */
        char junk[300];
        snprintf(junk, sizeof junk, "%s/check-junk.db", dir);
        FILE *j = fopen(junk, "wb");
        if (j) { for (int i = 0; i < 4096; i++) fputc(0x5a, j); fclose(j); }
        CHECK(!library_file_damaged(db5) && library_file_damaged(junk) && !library_file_damaged("/no/such/file.db"));
        CHECK(library_file_damaged(hurt));                       /* (a database still, a stretch of it overwritten) */
    }
    library_set_aside(db5);
    CHECK(access(db5, F_OK) != 0 && access(bad5, F_OK) == 0 && access(mark5, F_OK) == 0);
    scan_job_t cut = {.lib = library_open(db5, art), .music = music, .cancel_after = 2};
    CHECK(cut.lib != NULL);
    job_run(&cut);
    CHECK(cut.result == -1 && library_import_owed(cut.lib) == 0 && access(mark5, F_OK) == 0);   /* (not all read: not yet) */
    library_close(cut.lib);
    lib = library_open(db5, art);                                /* (the next start) */
    CHECK(lib && library_import_owed(lib) == 0 && access(mark5, F_OK) == 0);
    CHECK(lib && library_scan(lib, music, NULL, NULL) == 240);
    CHECK(t_db_exec(db5, "UPDATE tracks SET play_count=1 WHERE path LIKE '%Check 03/001.flac'") == SQLITE_OK);   /* (played meanwhile) */
    CHECK(lib && library_import_owed(lib) == 20 && access(mark5, F_OK) != 0);
    CHECK(t_db_int(db5, "SELECT COUNT(*) FROM tracks WHERE play_count=4 AND favourite=1") == 19);
    CHECK(t_db_int(db5, "SELECT play_count FROM tracks WHERE path LIKE '%Check 03/001.flac'") == 5);
    CHECK(lib && library_import_owed(lib) == 0);                 /* (once) */
    CHECK(t_db_int(db5, "SELECT SUM(play_count) FROM tracks") == 19 * 4 + 5);
    library_close(lib);
    /* set aside again while the first is still owed: the first is kept */
    library_set_aside(db5);                                      /* (not owed any more: this one becomes .bad) */
    CHECK(access(mark5, F_OK) == 0 && access(bad2, F_OK) != 0);
    lib = library_open(db5, art);
    library_close(lib);
    int64_t first = t_db_int(bad5, "SELECT SUM(play_count) FROM tracks");
    library_set_aside(db5);
    CHECK(access(bad2, F_OK) == 0 && t_db_int(bad5, "SELECT SUM(play_count) FROM tracks") == first && first == 19 * 4 + 5);
}

/* ---- a playlist whose file can't be read just now stays ----
 * As it was, under its number, until it reads again. (It went with the
 * playlists that are gone, and came back at the next scan as a new one.) */
static void test_playlist_unread(const char *dir)
{
    char music[300], db[300], art[300], list[400];
    snprintf(music, sizeof music, "%s/pumusic", dir);
    mkdir(music, 0755);
    snprintf(db, sizeof db, "%s/pu.db", dir);
    snprintf(art, sizeof art, "%s/pu-art", dir);
    make_album(music, "One", 3);
    snprintf(list, sizeof list, "%s/mine.m3u8", music);
    FILE *f = fopen(list, "wb");
    if (f) { fputs("One/001.flac\nOne/003.flac\n", f); fclose(f); }
    library_t *lib = library_open(db, art);
    CHECK(lib && library_scan(lib, music, NULL, NULL) == 3);
    int64_t id = t_db_int(db, "SELECT id FROM playlists WHERE name='mine'");
    CHECK(id > 0 && t_db_int(db, "SELECT COUNT(*) FROM playlist_items") == 2);
    if (geteuid() != 0) {                                        /* (root reads a file whatever its mode) */
        struct timeval tv[2] = {{1700000300, 0}, {1700000300, 0}};
        utimes(list, tv);
        chmod(list, 0);
        CHECK(library_scan(lib, music, NULL, NULL) == 3);
        CHECK(t_db_int(db, "SELECT id FROM playlists WHERE name='mine'") == id);
        CHECK(t_db_int(db, "SELECT COUNT(*) FROM playlist_items") == 2);
        chmod(list, 0644);
        CHECK(library_scan(lib, music, NULL, NULL) == 3);
        CHECK(t_db_int(db, "SELECT id FROM playlists WHERE name='mine'") == id);
        CHECK(t_db_int(db, "SELECT COUNT(*) FROM playlist_items") == 2);
    }
    library_close(lib);
}

/* ---- the folders' counts at a connection's second full rebuild ----
 * Written again each time: the first rebuild left its work table behind (a
 * statement of its own still open when it was dropped), the second couldn't
 * make it and stopped before the counts. */
static void test_folder_counts_again(const char *dir)
{
    char music[300], db[300], art[300];
    snprintf(music, sizeof music, "%s/fcmusic", dir);
    mkdir(music, 0755);
    snprintf(db, sizeof db, "%s/fc.db", dir);
    snprintf(art, sizeof art, "%s/fc-art", dir);
    make_album(music, "One", 4);
    make_album(music, "Two", 2);                     /* (two folders: with one, it is the tree's top itself) */
    library_t *lib = library_open(db, art);
    CHECK(lib && library_scan(lib, music, NULL, NULL) == 6);
    library_test_rebuild_groups(lib);
    CHECK(t_db_int(db, "SELECT ntracks FROM folders WHERE name='One'") == 4);
    make_album(music, "Three", 3);
    CHECK(library_scan(lib, music, NULL, NULL) == 9);
    CHECK(t_db_exec(db, "UPDATE folders SET ntracks=99") == SQLITE_OK);   /* (whatever they were: to be written) */
    library_test_rebuild_groups(lib);
    CHECK(t_db_int(db, "SELECT ntracks FROM folders WHERE name='One'") == 4);
    CHECK(t_db_int(db, "SELECT ntracks FROM folders WHERE name='Three'") == 3);
    CHECK(t_db_int(db, "SELECT COUNT(*) FROM folders WHERE ntracks=99") == 0);
    library_close(lib);
}

/* ---- what can't be read or looked up for a moment is not gone ----
 * An album in one file, split by its sheet, with plays. Its file changed and
 * can't be read just now; its sheet changed and can't be read just now; a
 * lookup in the library failing during a scan: each time the tracks stay, with
 * their plays, and are read again when they can be. (Each of the three dropped
 * them: the next scan made them again as new songs, or left the file as one
 * long track.) */
static void test_unread_is_not_gone(const char *dir)
{
    char music[300], db[300], art[300], whole[400], sheet[400], other[400];
    snprintf(music, sizeof music, "%s/umusic", dir);
    mkdir(music, 0755);
    snprintf(db, sizeof db, "%s/unread.db", dir);
    snprintf(art, sizeof art, "%s/unread-art", dir);
    make_album(music, "Plain", 3);
    snprintf(other, sizeof other, "%s/Plain/001.flac", music);
    snprintf(whole, sizeof whole, "%s/Plain/whole.flac", music);
    CHECK(write_flac(whole, "Whole", "Alpha", "Sheet", 1) == 0);
    snprintf(sheet, sizeof sheet, "%s/Plain/whole.cue", music);
    FILE *f = fopen(sheet, "wb");
    if (f) {
        fputs("PERFORMER \"Alpha\"\nTITLE \"Sheet\"\nFILE \"whole.flac\" WAVE\n  TRACK 01 AUDIO\n    TITLE \"First\"\n"
              "    INDEX 01 00:00:00\n  TRACK 02 AUDIO\n    TITLE \"Second\"\n    INDEX 01 00:00:01\n", f);
        fclose(f);
    }
    library_t *lib = library_open(db, art);
    CHECK(lib && library_scan(lib, music, NULL, NULL) == 5);
    CHECK(t_db_int(db, "SELECT COUNT(*) FROM tracks WHERE cue<>0") == 2);
    CHECK(t_db_exec(db, "UPDATE tracks SET play_count=5, favourite=1 WHERE cue<>0") == SQLITE_OK);
    const char *kept = "SELECT COUNT(*) FROM tracks WHERE cue<>0 AND play_count=5 AND favourite=1";
    int64_t ids = t_db_int(db, "SELECT SUM(id) FROM tracks WHERE cue<>0");
    if (geteuid() != 0) {                                        /* (root reads a file whatever its mode) */
        struct timeval tv[2] = {{1700000100, 0}, {1700000100, 0}};
        /* the music file: changed, and not to be read */
        utimes(whole, tv);
        chmod(whole, 0);
        CHECK(library_scan(lib, music, NULL, NULL) == 5);
        CHECK(t_db_int(db, kept) == 2 && t_db_int(db, "SELECT SUM(id) FROM tracks WHERE cue<>0") == ids);
        chmod(whole, 0644);
        CHECK(library_scan(lib, music, NULL, NULL) == 5);
        CHECK(t_db_int(db, kept) == 2 && t_db_int(db, "SELECT SUM(id) FROM tracks WHERE cue<>0") == ids);
        /* the sheet: changed, and not to be read */
        utimes(sheet, tv);
        chmod(sheet, 0);
        CHECK(library_scan(lib, music, NULL, NULL) == 5);
        CHECK(t_db_int(db, kept) == 2 && t_db_int(db, "SELECT SUM(id) FROM tracks WHERE cue<>0") == ids);
        CHECK(t_db_int(db, "SELECT mtime FROM cues WHERE kind=0") != 1700000100);   /* (not written down as read) */
        chmod(sheet, 0644);
        CHECK(library_scan(lib, music, NULL, NULL) == 5);
        CHECK(t_db_int(db, kept) == 2 && t_db_int(db, "SELECT SUM(id) FROM tracks WHERE cue<>0") == ids);
        CHECK(t_db_int(db, "SELECT mtime FROM cues WHERE kind=0") == 1700000100);
    }
    {   /* every lookup of a scan failing (a song changed, so there is one to run) */
        struct timeval tv[2] = {{1700000200, 0}, {1700000200, 0}};
        utimes(other, tv);
        library_test_scan_lookups_fail(1);
        library_scan(lib, music, NULL, NULL);
        library_test_scan_lookups_fail(0);
        CHECK(t_db_int(db, "SELECT COUNT(*) FROM tracks") == 5 && t_db_int(db, kept) == 2);
        CHECK(t_db_int(db, "SELECT SUM(id) FROM tracks WHERE cue<>0") == ids);
        CHECK(library_scan(lib, music, NULL, NULL) == 5 && t_db_int(db, kept) == 2);   /* (and the scan after it, whole) */
    }
    library_close(lib);
}

/* ---- a groups' pass that stops before its end makes nothing ----
 * The passes over the songs that make albums, artists and folders, each
 * stopped at every step in turn (the card not answering, no memory): the
 * groups stay as they were, the scan is left unfinished, and the next one
 * makes them, as a full rebuild would. (A pass that stopped was taken for one
 * that ended: albums past that point without year or folder, their artists
 * without a name, folders deleted, and the scan noted as done.) */
static int gs_n;

static void gs_retag(const char *song, int n)
{
    char album[32], artist[32];
    snprintf(album, sizeof album, "Other %d", n);
    snprintf(artist, sizeof artist, "New %d feat. Guest %d", n, n % 3);
    CHECK(write_flac_tags(song, "Song", artist, n % 2 ? "Main" : "", album, n % 2 ? "Rock" : "Jazz", "Ravel", 1970 + n, 1) == 0);
    struct timeval tv[2] = {{1700002000 + gs_n, 0}, {1700002000 + gs_n, 0}};
    gs_n++;
    utimes(song, tv);
}

/* a full rebuild on top changes nothing */
static int gs_right(library_t *lib, const char *db)
{
    char *before = groups_text(db);
    library_test_rebuild_groups(lib);
    char *after = groups_text(db);
    int same = before && after && !strcmp(before, after);
    if (!same && before && after) first_diff("groups stopped", before, after);
    sqlite3_free(before);
    sqlite3_free(after);
    return same;
}

static void test_groups_stopped(const char *dir)
{
    static const char *const folders[] = {"K", "K/L", "M", "N", "N/O"};
    enum { N = 15 };
    char music[300], db[300], art[300], song[N][400];
    snprintf(music, sizeof music, "%s/gsmusic", dir);
    mkdir(music, 0755);
    snprintf(db, sizeof db, "%s/stopped.db", dir);
    snprintf(art, sizeof art, "%s/stopped-art", dir);
    for (int i = 0; i < N; i++) {
        char d[400], album[32];
        snprintf(d, sizeof d, "%s/%s", music, folders[i / 3]);
        mkdir(d, 0755);
        snprintf(song[i], sizeof song[i], "%s/%02d.flac", d, i);
        snprintf(album, sizeof album, "Album %d", i / 3);
        CHECK(write_flac_tags(song[i], "Song", i % 3 ? "Main" : "Main feat. Guest", "Main", album, "Rock", "Bach",
                              1990 + i / 3, i % 3 + 1) == 0);
    }
    const char *sig = "SELECT value FROM meta WHERE key='scan_sig'";
    library_t *lib = library_open(db, art);
    CHECK(lib && library_scan(lib, music, NULL, NULL) == N && !library_scan_unfinished(lib) && t_db_int(db, sig) != 0);
    int at, bit = 0;
    /* where songs changed: every step from one on failing, so the full rebuild after it too */
    for (at = 1; at < 200; at++) {
        gs_retag(song[at % N], at);
        char *before = groups_text(db);
        library_test_groups_fail(at);
        CHECK(library_scan(lib, music, NULL, NULL) == N);
        library_test_groups_fail(0);
        if (!library_scan_unfinished(lib)) { sqlite3_free(before); break; }   /* (past the last step: a scan as any) */
        char *after = groups_text(db);
        int same = before && after && !strcmp(before, after);
        CHECK(same);
        if (!same && before && after) first_diff("stopped where changed", before, after);
        sqlite3_free(before);
        sqlite3_free(after);
        CHECK(t_db_int(db, sig) == 0);
        CHECK(library_scan(lib, music, NULL, NULL) == N && !library_scan_unfinished(lib) && t_db_int(db, sig) != 0);
        CHECK(gs_right(lib, db));
        if (!same) break;
        bit++;
    }
    CHECK(bit >= 5 && gs_right(lib, db));
    /* where songs changed: one step failing, and the full rebuild of the same scan puts it right */
    for (at = 1, bit = 0; at < 200; at++) {
        gs_retag(song[at % N], 300 + at);
        library_test_groups_fail(-at);
        CHECK(library_scan(lib, music, NULL, NULL) == N && !library_scan_unfinished(lib));
        library_test_groups_fail(0);
        int right = gs_right(lib, db);
        CHECK(right);
        if (library_scan_groups(lib) == 1 || !right) break;                   /* (1: past the last step) */
        bit++;
    }
    CHECK(bit >= 5);
    /* all of them (a scan before left them to do): made aside, then in place */
    for (at = 1, bit = 0; at < 200; at++) {
        CHECK(t_db_exec(db, "INSERT INTO meta(key, value) VALUES('scan_pending', 2)"
                          " ON CONFLICT(key) DO UPDATE SET value=2") == SQLITE_OK);
        char *before = groups_text(db);
        library_test_groups_fail(at);
        CHECK(library_scan(lib, music, NULL, NULL) == N);
        library_test_groups_fail(0);
        int unfinished = library_scan_unfinished(lib);
        char *after = groups_text(db);
        int same = before && after && !strcmp(before, after);
        CHECK(same);
        if (!same && before && after) first_diff("stopped, all of them", before, after);
        sqlite3_free(before);
        sqlite3_free(after);
        if (!unfinished || !same) break;
        CHECK(t_db_int(db, sig) == 0);
        CHECK(library_scan(lib, music, NULL, NULL) == N && !library_scan_unfinished(lib) && t_db_int(db, sig) != 0);
        bit++;
    }
    CHECK(bit >= 2 * N);                               /* (the songs' pass and the folders' pass, each step) */
    /* ...and in place, each step (the rebuild a scan falls back to) */
    for (at = 1; at <= 2 * N + 4; at++) {
        char *before = groups_text(db);
        library_test_groups_fail(at);
        library_test_rebuild_groups(lib);
        library_test_groups_fail(0);
        char *after = groups_text(db);
        int same = before && after && !strcmp(before, after);
        CHECK(same);
        if (!same && before && after) first_diff("stopped in place", before, after);
        sqlite3_free(before);
        sqlite3_free(after);
        if (!same) break;
    }
    library_close(lib);
    /* a library from before names were spelled as tagged, opened: its groups are made
     * again then. Stopped: as they were, and the next scan's to do */
    CHECK(t_db_exec(db, "PRAGMA user_version=3") == SQLITE_OK);
    char *before = groups_text(db);
    library_test_groups_fail(4);
    lib = library_open(db, art);
    library_test_groups_fail(0);
    char *after = groups_text(db);
    CHECK(lib && before && after && !strcmp(before, after));
    if (before && after && strcmp(before, after)) first_diff("stopped at an upgrade", before, after);
    CHECK(library_scan_unfinished(lib));
    CHECK(library_scan(lib, music, NULL, NULL) == N && !library_scan_unfinished(lib) && gs_right(lib, db));
    sqlite3_free(before);
    sqlite3_free(after);
    library_close(lib);
}

/* ---- a playlist whose matching or writing stops at a failed step is left as it was ----
 * One playlist, its entries found each way (by path, a split file's tracks,
 * the looser match, one not there), read again because its file changed, then
 * because a song did: each step of that failing in turn (the card not
 * answering, no memory), the playlist stays as it was, songs, date and all,
 * the scan is not finished, and the next one reads it. (A failed lookup was
 * "not in the library": the playlist written without the song, or cleared and
 * not all put back, under the file's new date, so it stayed that way.) */
static char *pl_text(const char *db, int dated)
{
    sqlite3 *raw = NULL;
    sqlite3_stmt *q = NULL;
    char *text = NULL;
    if (sqlite3_open(db, &raw) == SQLITE_OK && sqlite3_prepare_v2(raw,
            "SELECT CASE WHEN ?1 THEN p.mtime||' '||p.size||' ' ELSE '' END || p.id||' '||p.ntracks||' '||p.nmissing||':'"
            " || COALESCE((SELECT group_concat(track_id, ',') FROM (SELECT track_id FROM playlist_items"
            " WHERE playlist_id=p.id ORDER BY pos)), '') FROM playlists p WHERE p.name='mix'", -1, &q, NULL) == SQLITE_OK) {
        sqlite3_bind_int(q, 1, dated);
        if (sqlite3_step(q) == SQLITE_ROW) text = sqlite3_mprintf("%s", (const char *)sqlite3_column_text(q, 0));
    }
    sqlite3_finalize(q);
    sqlite3_close(raw);
    return text;
}

static void pl_write(const char *path, int variant, int n)
{
    FILE *f = fopen(path, "wb");
    if (f) {
        fputs(variant ? "#EXTM3U\nPA/004.flac\nPA/whole.flac\nD:\\away\\PA\\001.flac\n#EXTINF:1,Some Artist - PA 2\nZ:\\renamed\\zzz.flac\n"
                      : "#EXTM3U\nPA/001.flac\nPA/002.flac\nPA/whole.flac\nX:\\elsewhere\\PA\\003.flac\nmissing/none.flac\n", f);
        fclose(f);
    }
    struct timeval tv[2] = {{1700003000 + n, 0}, {1700003000 + n, 0}};
    utimes(path, tv);
}

static void test_playlists_stopped(const char *dir)
{
    char music[300], db[300], art[300], whole[400], sheet[400], mix[400], song[400];
    snprintf(music, sizeof music, "%s/psmusic", dir);
    mkdir(music, 0755);
    snprintf(db, sizeof db, "%s/plstopped.db", dir);
    snprintf(art, sizeof art, "%s/plstopped-art", dir);
    make_album(music, "PA", 4);
    snprintf(song, sizeof song, "%s/PA/003.flac", music);
    snprintf(whole, sizeof whole, "%s/PA/whole.flac", music);
    CHECK(write_flac(whole, "Whole", "Some Artist", "Sheet", 1) == 0);
    snprintf(sheet, sizeof sheet, "%s/PA/whole.cue", music);
    FILE *f = fopen(sheet, "wb");
    if (f) {
        fputs("PERFORMER \"Some Artist\"\nTITLE \"Sheet\"\nFILE \"whole.flac\" WAVE\n  TRACK 01 AUDIO\n    TITLE \"First\"\n"
              "    INDEX 01 00:00:00\n  TRACK 02 AUDIO\n    TITLE \"Second\"\n    INDEX 01 00:00:01\n", f);
        fclose(f);
    }
    snprintf(mix, sizeof mix, "%s/mix.m3u8", music);
    const char *sig = "SELECT value FROM meta WHERE key='scan_sig'";
    library_t *lib = library_open(db, art);
    char *want[2] = {NULL, NULL};                      /* the playlist as each of its two files makes it */
    int n = 0;
    for (int v = 1; v >= 0; v--) {
        pl_write(mix, v, n++);
        CHECK(lib && library_scan(lib, music, NULL, NULL) == 6 && t_db_int(db, sig) != 0);
        want[v] = pl_text(db, 0);
    }
    CHECK(want[0] && want[1] && strcmp(want[0], want[1]));
    CHECK(t_db_int(db, "SELECT ntracks*10 + nmissing FROM playlists WHERE name='mix'") == 51);
    int at, bit = 0, v = 0;
    /* its file changed (more steps than there are: past the last, a scan as any) */
    for (at = 1; at <= 60; at++) {
        char *before = pl_text(db, 1);
        pl_write(mix, v = !v, n++);
        library_test_playlists_fail(at);
        CHECK(library_scan(lib, music, NULL, NULL) == 6);
        library_test_playlists_fail(0);
        char *after = pl_text(db, 1), *now = pl_text(db, 0);
        int left = before && after && !strcmp(before, after) && t_db_int(db, sig) == 0;
        int read = now && !strcmp(now, want[v]) && t_db_int(db, sig) != 0;
        CHECK(left || read);                           /* as it was and to be read again, or read whole */
        if (!left && !read) fprintf(stderr, "step %d: %s\n  was: %s\n  to be: %s\n", at, after, before, want[v]);
        CHECK(t_db_int(db, "SELECT COUNT(*) FROM playlists") == 1);
        CHECK(library_scan(lib, music, NULL, NULL) == 6 && t_db_int(db, sig) != 0);
        sqlite3_free(now);
        now = pl_text(db, 0);
        CHECK(now && !strcmp(now, want[v]));
        bit += left;
        sqlite3_free(before);
        sqlite3_free(after);
        sqlite3_free(now);
    }
    CHECK(bit >= 20 && bit < 60);                      /* (the lookups, the pass of the looser match, the writes) */
    /* a song changed: every playlist is matched again */
    for (at = 1, bit = 0; at <= 60; at++) {
        char *before = pl_text(db, 1);
        CHECK(write_flac(song, "PA 3", "Some Artist", "PA", 3 + at) == 0);
        /* (two seconds on each time: the H2's card is FAT, whose times go in twos. A
         * second on was no change there every other round, and nothing was matched) */
        struct timeval tv[2] = {{1700004000 + 2 * at, 0}, {1700004000 + 2 * at, 0}};
        utimes(song, tv);
        library_test_playlists_fail(at);
        CHECK(library_scan(lib, music, NULL, NULL) == 6);
        library_test_playlists_fail(0);
        char *after = pl_text(db, 1);
        CHECK(before && after && !strcmp(before, after));   /* (left as it was, or matched to the same) */
        if (before && after && strcmp(before, after)) fprintf(stderr, "step %d: %s\n  was: %s\n", at, after, before);
        sqlite3_free(before);
        sqlite3_free(after);
        int left = library_scan_unfinished(lib);
        CHECK(left ? t_db_int(db, sig) == 0 : t_db_int(db, sig) != 0);
        CHECK(library_scan(lib, music, NULL, NULL) == 6 && !library_scan_unfinished(lib) && t_db_int(db, sig) != 0);
        char *now = pl_text(db, 0);
        CHECK(now && !strcmp(now, want[v]));
        sqlite3_free(now);
        bit += left;
    }
    CHECK(bit >= 20 && bit < 60);
    sqlite3_free(want[0]);
    sqlite3_free(want[1]);
    library_close(lib);
}

int main(void)
{
    char dir[200];
    t_mkdtemp(dir, sizeof dir, "fathom-test-scan");
    test_open_and_levelling(dir);
    test_scans(dir);
    test_playlist_during_scan(dir);
    test_quick_listing(dir);
    test_groups_where_changed(dir);
    test_bulk_load(dir);
    test_groups_as_added(dir);
    test_known_files(dir);
    test_died_on(dir);
    test_died_side_by_side(dir);
    test_retry_unread(dir);
    test_progress_told(dir);
    test_reset_on(dir);
    test_playlist_unread(dir);
    test_folder_counts_again(dir);
    test_unread_is_not_gone(dir);
    test_groups_stopped(dir);
    test_playlists_stopped(dir);
    test_check_and_import(dir);
    return t_result();
}
