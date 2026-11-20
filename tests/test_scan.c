/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The library's connections and scans (app/library/library.c): opening an
 * up-to-date database does no upgrade work, levelling's next track comes
 * from an index; a background scan lets the player's connection write
 * while it runs, writes nothing when nothing changed, keeps what it read
 * when stopped (and never drops music then), and notes a file it can't
 * read. */
#include <FLAC/metadata.h>
#include <FLAC/stream_encoder.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <sqlite3.h>

#include "library/library.h"
#include "tmpdir.h"

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static int write_flac(const char *path, const char *title, const char *artist, const char *album, int track)
{
    FLAC__StreamEncoder *e = FLAC__stream_encoder_new();
    FLAC__stream_encoder_set_channels(e, 2);
    FLAC__stream_encoder_set_bits_per_sample(e, 16);
    FLAC__stream_encoder_set_sample_rate(e, 44100);
    FLAC__StreamMetadata *vc = FLAC__metadata_object_new(FLAC__METADATA_TYPE_VORBIS_COMMENT);
    char tr[8];
    snprintf(tr, sizeof tr, "%d", track);
    const char *kv[][2] = {{"TITLE", title}, {"ARTIST", artist}, {"ALBUM", album}, {"TRACKNUMBER", tr}};
    for (int i = 0; i < 4; i++) {
        FLAC__StreamMetadata_VorbisComment_Entry en;
        FLAC__metadata_object_vorbiscomment_entry_from_name_value_pair(&en, kv[i][0], kv[i][1]);
        FLAC__metadata_object_vorbiscomment_append_comment(vc, en, false);
    }
    FLAC__stream_encoder_set_metadata(e, &vc, 1);
    int rc = -1;
    if (FLAC__stream_encoder_init_file(e, path, NULL, NULL) == FLAC__STREAM_ENCODER_INIT_STATUS_OK) {
        static FLAC__int32 pcm[2 * 441];
        FLAC__stream_encoder_process_interleaved(e, pcm, 441);
        FLAC__stream_encoder_finish(e);
        rc = 0;
    }
    FLAC__stream_encoder_delete(e);
    FLAC__metadata_object_delete(vc);
    return rc;
}

static int64_t db_int(const char *db, const char *sql)
{
    sqlite3 *raw = NULL;
    sqlite3_stmt *q = NULL;
    int64_t v = -1;
    if (sqlite3_open(db, &raw) == SQLITE_OK && sqlite3_prepare_v2(raw, sql, -1, &q, NULL) == SQLITE_OK &&
        sqlite3_step(q) == SQLITE_ROW)
        v = sqlite3_column_int64(q, 0);
    sqlite3_finalize(q);
    sqlite3_close(raw);
    return v;
}

static int db_exec(const char *db, const char *sql)
{
    sqlite3 *raw = NULL;
    int rc = sqlite3_open(db, &raw) == SQLITE_OK ? sqlite3_exec(raw, sql, NULL, NULL, NULL) : -1;
    sqlite3_close(raw);
    return rc;
}

/* Opening: an up-to-date database isn't upgraded again (each connection,
 * the background scan's and levelling's too, re-read the whole tracks table
 * and took the write lock); levelling's next track by its index. */
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
    CHECK(db_int(db, "PRAGMA user_version") == 5);

    /* a column an upgrade would fill, emptied: an open of an up-to-date
     * library leaves it (it doesn't look) */
    CHECK(db_exec(db, "UPDATE tracks SET sort=NULL WHERE title='Song 3'") == SQLITE_OK);
    lib = library_open(db, art);
    CHECK(lib != NULL);
    library_close(lib);
    CHECK(db_int(db, "SELECT COUNT(*) FROM tracks WHERE sort IS NULL") == 1);
    /* ...and one from before (user_version 4) is upgraded once */
    CHECK(db_exec(db, "PRAGMA user_version=4") == SQLITE_OK);
    lib = library_open(db, art);
    CHECK(lib != NULL);
    CHECK(db_int(db, "SELECT COUNT(*) FROM tracks WHERE sort IS NULL") == 0);
    CHECK(db_int(db, "PRAGMA user_version") == 5);

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

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
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
} scan_job_t;

static void job_progress(int done, int total, const char *path, void *ctx)
{
    scan_job_t *j = ctx;
    (void)done;
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
    return db_int(db, "SELECT COUNT(*) FROM (SELECT path FROM tracks WHERE cue=0 UNION SELECT path FROM cues)");
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
    double worst = 0, t0 = now();
    int writes = 0, kept = 1;
    while (!job.done) {
        double t = now();
        library_set_favourite(player, tid, writes & 1);
        if (now() - t > worst) worst = now() - t;
        kept &= library_favourite(player, tid) == (writes & 1);
        writes++;
        usleep(20000);
    }
    pthread_join(th, NULL);
    double took = now() - t0;
    CHECK(job.result == 620);
    CHECK(took > 1.5 && writes > 20);             /* (long enough to tell) */
    CHECK(worst < 0.5);
    CHECK(kept);
    if (worst >= 0.5) fprintf(stderr, "a write waited %.2f s (scan %.1f s)\n", worst, took);
    library_close(job.lib);
    CHECK(db_int(db, "SELECT COUNT(*) FROM tracks WHERE album_id IS NULL") == 0);
    CHECK(db_int(db, "SELECT COUNT(*) FROM albums") == 32);

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
    scan_job_t stop = {.lib = library_open(db, art), .music = music, .cancel_after = 3};
    CHECK(stop.lib && library_scan(stop.lib, music, job_progress, &stop) == -1);
    library_close(stop.lib);
    CHECK(db_int(db, "SELECT COUNT(*) FROM tracks") == 620);
    CHECK(library_scan(player, music, NULL, NULL) == 615);

    /* 4. New music, the scan stopped after reading it: what it read is kept
     * (it was all rolled back), and the next scan, with nothing new to
     * read, makes their album */
    make_album(music, "Aa New", 20);              /* (first in path order) */
    stop = (scan_job_t){.lib = library_open(db, art), .music = music, .cancel_after = 2, .cancel_unless = "Aa New"};
    CHECK(stop.lib && library_scan(stop.lib, music, job_progress, &stop) == -1);
    library_close(stop.lib);
    CHECK(db_int(db, "SELECT COUNT(*) FROM tracks") == 635);
    CHECK(db_int(db, "SELECT COUNT(*) FROM tracks WHERE album LIKE 'Aa New' AND album_id IS NULL") == 20);
    CHECK(db_int(db, "SELECT COUNT(*) FROM tracks WHERE album LIKE 'Aa New' AND sort IS NOT NULL") == 20);   /* (in Songs) */
    bg = library_open(db, art);
    CHECK(bg && library_scan(bg, music, NULL, NULL) == 635 && library_scan_changes(bg) > 0);
    library_close(bg);
    CHECK(db_int(db, "SELECT COUNT(*) FROM tracks WHERE album_id IS NULL") == 0);
    CHECK(db_int(db, "SELECT COUNT(*) FROM albums WHERE title='Aa New' AND ntracks=20") == 1);
    lib_search_t hits;
    CHECK(library_search(player, "aa new 7", &hits) > 0 && hits.songs.n == 1);
    library_search_free(&hits);
    CHECK(library_scan(player, music, NULL, NULL) == 635 && library_scan_changes(player) == 0);

    /* 5. A file that can't be read: noted in `cues`, so the library still
     * holds every file the walk lists, and it's not read again */
    snprintf(path, sizeof path, "%s/Album 00/bad.flac", music);
    FILE *f = fopen(path, "wb");
    if (f) { fputs("not a FLAC file at all", f); fclose(f); }
    CHECK(library_scan(player, music, NULL, NULL) == 635);
    CHECK(db_int(db, "SELECT kind FROM cues WHERE path LIKE '%/bad.flac'") == 3);
    CHECK(files_in_library(db) == 636);
    CHECK(library_scan(player, music, NULL, NULL) == 635 && library_scan_changes(player) == 0);
    /* ...until it changes: a track then, and the note goes */
    CHECK(write_flac(path, "Fixed", "Some Artist", "Album 00", 99) == 0);
    CHECK(library_scan(player, music, NULL, NULL) == 636);
    CHECK(db_int(db, "SELECT COUNT(*) FROM cues") == 0);
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
            CHECK(db_int(db, "SELECT nmissing FROM playlists WHERE name='other'") == 1);
            const char *want[5] = {"Album 01/001", "Album 01/002", "Album 02/003", "Album 03/004", "Album 10/001"};
            for (int k = 0; k < 5 && k < n; k++) {
                char sql[200];
                snprintf(sql, sizeof sql, "SELECT id FROM tracks WHERE path LIKE '%%/%s.flac'", want[k]);
                CHECK(items.ids[k] == db_int(db, sql));
            }
            if (n == 304) CHECK(items.ids[303] == db_int(db, "SELECT id FROM tracks WHERE path LIKE '%/Album 24/020.flac'"));
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
    CHECK(v0 == v1 && db_int(db, "SELECT value FROM meta WHERE key='scan_pending'") == 0);

    /* 8. A file that can't be opened just now (here: no permission; on the
     * H2 a read error): not noted as unreadable, read at the next scan */
    snprintf(path, sizeof path, "%s/Album 01/later.flac", music);
    CHECK(write_flac(path, "Later", "Some Artist", "Album 01", 50) == 0);
    chmod(path, 0);
    if (access(path, R_OK) != 0) {                /* (as root it can be read anyway) */
        CHECK(library_scan(player, music, NULL, NULL) == 639);
        CHECK(db_int(db, "SELECT COUNT(*) FROM cues WHERE path LIKE '%/later.flac'") == 0);
    }
    chmod(path, 0644);
    CHECK(library_scan(player, music, NULL, NULL) == 640);
    library_close(player);
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
    CHECK(db_exec(db, sql) == SQLITE_OK);
    pthread_join(th, NULL);
    CHECK(job.result == 200);
    CHECK(db_int(db, "SELECT COUNT(*) FROM playlists WHERE name='New one'") == 1);
    library_close(job.lib);
    /* written by then; and if never, the next scan drops it */
    CHECK(library_scan(player, music, NULL, NULL) == 200);
    CHECK(db_int(db, "SELECT COUNT(*) FROM playlists WHERE name='New one'") == 0);
    library_close(player);
}

int main(void)
{
    char dir[200];
    test_tmp(dir, sizeof dir, "fathom-test-scan");
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; }
    test_open_and_levelling(dir);
    test_scans(dir);
    test_playlist_during_scan(dir);
    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    printf("scan: all good\n");
    return 0;
}
