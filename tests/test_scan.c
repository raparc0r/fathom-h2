/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* The library's connections and scans (app/library/library.c): opening an
 * up-to-date database does no upgrade work, levelling's next track comes
 * from an index; a background scan lets the player's connection write
 * while it runs, writes nothing when nothing changed, keeps what it read
 * when stopped (and never drops music then), and notes a file it can't
 * read. */
#include <dirent.h>
#include <FLAC/metadata.h>
#include <FLAC/stream_encoder.h>
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

/* Every tag that groups songs (empty: left out) */
static int write_flac_tags(const char *path, const char *title, const char *artist, const char *album_artist,
                           const char *album, const char *genre, const char *composer, int year, int track)
{
    FLAC__StreamEncoder *e = FLAC__stream_encoder_new();
    FLAC__stream_encoder_set_channels(e, 2);
    FLAC__stream_encoder_set_bits_per_sample(e, 16);
    FLAC__stream_encoder_set_sample_rate(e, 44100);
    FLAC__StreamMetadata *vc = FLAC__metadata_object_new(FLAC__METADATA_TYPE_VORBIS_COMMENT);
    char tr[8], yr[8];
    snprintf(tr, sizeof tr, "%d", track);
    snprintf(yr, sizeof yr, "%d", year);
    const char *kv[][2] = {{"TITLE", title}, {"ARTIST", artist}, {"ALBUMARTIST", album_artist}, {"ALBUM", album},
                           {"GENRE", genre}, {"COMPOSER", composer}, {"DATE", year ? yr : ""}, {"TRACKNUMBER", tr}};
    for (size_t i = 0; i < sizeof kv / sizeof *kv; i++) {
        if (!kv[i][1] || !*kv[i][1]) continue;
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
    CHECK(db_int(db, "PRAGMA user_version") == 9);

    /* a column an upgrade would fill, emptied: an open of an up-to-date
     * library leaves it (it doesn't look) */
    CHECK(db_exec(db, "UPDATE tracks SET sort=NULL WHERE title='Song 3'") == SQLITE_OK);
    lib = library_open(db, art);
    CHECK(lib != NULL);
    library_close(lib);
    CHECK(db_int(db, "SELECT COUNT(*) FROM tracks WHERE sort IS NULL") == 1);
    /* ...and one from before (user_version 4) is upgraded once, saying how far
     * it has got, to the last step; library_upgrade_needed() tells it's
     * coming (about how many songs) and that it's done */
    char none[320];
    snprintf(none, sizeof none, "%s/no-such.db", dir);
    CHECK(library_upgrade_needed(db) == 0 && library_upgrade_needed(none) == 0);
    CHECK(access(none, F_OK) != 0);                  /* (not made by looking) */
    CHECK(db_exec(db, "PRAGMA user_version=4") == SQLITE_OK);
    CHECK(library_upgrade_needed(db) == 5);
    up_calls = up_last_done = up_last_total = 0;
    library_set_upgrade_progress(up_seen, NULL);
    lib = library_open(db, art);
    library_set_upgrade_progress(NULL, NULL);
    CHECK(lib != NULL);
    CHECK(up_calls > 10 && up_last_done == up_last_total && up_calls == up_last_total);
    CHECK(library_upgrade_needed(db) == 0);
    CHECK(db_int(db, "SELECT COUNT(*) FROM tracks WHERE sort IS NULL") == 0);
    CHECK(db_int(db, "PRAGMA user_version") == 9);
    library_close(lib);
    /* ...and one from 0.16.0 (8: the search text every word in a row, no index
     * of it) gets its search keys (name|artist|album) and their index, and
     * finds its songs */
    CHECK(db_exec(db, "DROP INDEX tracks_search; UPDATE tracks SET search='song 3 an artist an artist an album';"
                      "UPDATE albums SET search='an album an artist'; UPDATE artists SET search='an artist';"
                      "PRAGMA user_version=8") == SQLITE_OK);
    CHECK(library_upgrade_needed(db) == 5);
    lib = library_open(db, art);
    CHECK(lib != NULL && library_upgrade_needed(db) == 0);
    CHECK(db_int(db, "SELECT COUNT(*) FROM tracks WHERE search LIKE '%|%|%'") == 5);
    CHECK(db_int(db, "SELECT COUNT(*) FROM tracks WHERE search LIKE 'song 3|%'") == 1);
    CHECK(db_int(db, "SELECT COUNT(*) FROM albums WHERE search NOT LIKE '%|%|'") == 0);
    CHECK(db_int(db, "SELECT COUNT(*) FROM artists WHERE search NOT LIKE '%||'") == 0);
    CHECK(db_int(db, "SELECT COUNT(*) FROM sqlite_master WHERE name='tracks_search'") == 1);
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
    char where[160];                   /* the last progress call: a file, "covers" (for a slow write's report) */
} scan_job_t;

static void job_progress(int done, int total, const char *path, void *ctx)
{
    scan_job_t *j = ctx;
    (void)done;
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
#ifdef __linux__
    /* the player's writes come from its UI thread, at normal priority; the
     * scan (as the player's) at nice 19, as the H2 run is (started first:
     * a thread starts at its maker's) */
    id_t me = (id_t)syscall(SYS_gettid);
    int was = getpriority(PRIO_PROCESS, me);
    setpriority(PRIO_PROCESS, me, 0);
#endif
    double worst = 0, t0 = now();
    int writes = 0, kept = 1;
    while (!job.done) {
        double t = now();
        library_set_favourite(player, tid, writes & 1);
        double w = now() - t;
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
    double took = now() - t0;
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
    /* (every file held by name, as scans did: with the library's files ticked
     * off as the folder is listed there's no file here to stop at;
     * test_known_files stops one that has new files to read) */
    library_test_known(0);
    scan_job_t stop = {.lib = library_open(db, art), .music = music, .cancel_after = 3};
    CHECK(stop.lib && library_scan(stop.lib, music, job_progress, &stop) == -1);
    library_close(stop.lib);
    library_test_known(1);
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
    CHECK(library_search(player, "aa new 7", &hits) > 0 && hits.n_songs == 1);
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
    CHECK(db_exec(db, "PRAGMA user_version=3") == SQLITE_OK);   /* (opened: groups made again, all at once) */
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
    int64_t gone = db_int(db, "SELECT id FROM tracks WHERE path LIKE '%/Album 06/020.flac'");
    player = library_open(db, art);
    CHECK(player && gone > 0);
    if (player && gone > 0) {
        library_played(player, gone, 0, (int64_t)time(NULL) - 60);
        library_played(player, gone, 0, (int64_t)time(NULL));
        CHECK(db_int(db, "SELECT COUNT(*) FROM plays") >= 2);
        unlink(path);
        CHECK(library_scan(player, music, NULL, NULL) == 623);
        CHECK(db_int(db, "SELECT COUNT(*) FROM plays WHERE track_id NOT IN (SELECT id FROM tracks)") == 0);
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
        CHECK(db_int(db2, "SELECT COUNT(*) FROM tracks") > 0);
        CHECK(library_scan_unfinished(cut));
        CHECK(library_scan(cut, music, NULL, NULL) == 623);
        CHECK(!library_scan_unfinished(cut));
        CHECK(db_int(db2, "SELECT COUNT(*) FROM albums") > 0);
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
    CHECK(db_int(db, "SELECT COUNT(*) FROM tracks WHERE path LIKE '%Album A/003.flac'") == 1);

    /* a name it spells otherwise: the slow walk, and no song under that name */
    fw_set(NULL, "Album B/002", NULL);
    CHECK(library_scan(lib, music, NULL, NULL) == 15 && !library_scan_quick(lib) && library_scan_changes(lib) == 0);
    CHECK(db_int(db, "SELECT COUNT(*) FROM tracks WHERE path LIKE '%00Z.flac'") == 0);

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
    CHECK(db_int(db, "SELECT COUNT(*) FROM tracks WHERE title='Retitled'") == 1);
    CHECK(fw_calls == 7);

    /* many new files at once (a first copy: not looked up one by one), one of
     * them under a name the FAT reading spells otherwise: found when its tags
     * are read, the folder walked instead, every file in and none under the
     * wrong name */
    make_album(music, "Album D", 300);
    fw_set(NULL, "Album D/150", NULL);
    CHECK(library_scan(lib, music, NULL, NULL) == 314 && !library_scan_quick(lib) && library_scan_changes(lib) > 0);
    CHECK(db_int(db, "SELECT COUNT(*) FROM tracks WHERE path LIKE '%Album D/%'") == 300);
    CHECK(db_int(db, "SELECT COUNT(*) FROM tracks WHERE path LIKE '%15Z.flac'") == 0);
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
        CHECK(db_int(db, "SELECT (SELECT COUNT(*) FROM tracks WHERE search IS NULL) + (SELECT COUNT(*) FROM albums"
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
    return db_int(db, "SELECT COUNT(*) FROM sqlite_master WHERE type='index' AND tbl_name='tracks' AND sql IS NOT NULL");
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
    CHECK(db_int(db, "SELECT COUNT(*) FROM sqlite_master WHERE name='bulk_idx'") == 0);
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
    CHECK(track_indexes(db3) == 0 && db_int(db3, "SELECT COUNT(*) FROM bulk_idx") == want);
    library_close(job.lib);
    CHECK(library_upgrade_needed(db3) > 0);
    up_calls = 0;
    library_set_upgrade_progress(up_seen, NULL);
    lib = library_open(db3, art);
    library_set_upgrade_progress(NULL, NULL);
    CHECK(lib && track_indexes(db3) == want && up_calls == (int)want);
    CHECK(db_int(db3, "SELECT COUNT(*) FROM sqlite_master WHERE name='bulk_idx'") == 0);
    CHECK(library_upgrade_needed(db3) == 0 && library_scan_unfinished(lib));
    CHECK(library_scan(lib, music, NULL, NULL) == 90 && library_check_plans(lib, NULL) == 0);   /* finished, not in bulk */
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
    CHECK(db_int(db, "SELECT COUNT(*) FROM cues WHERE path LIKE '%/C/junk.flac'") == 1);
    CHECK(db_int(db, "SELECT (SELECT COUNT(*) FROM tracks WHERE search IS NULL) + (SELECT COUNT(*) FROM albums"
                     " WHERE search IS NULL) + (SELECT COUNT(*) FROM artists WHERE search IS NULL)") == 0);
    CHECK(db_int(db, "SELECT COUNT(*) FROM tracks WHERE album_id IS NULL OR artist_id IS NULL OR folder_id IS NULL") == 0);
    CHECK(db_int(db, "SELECT value FROM meta WHERE key='scan_pending'") == 0);
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
    CHECK(db_int(db, "SELECT COUNT(*) FROM cues WHERE path LIKE '%/C/junk.flac'") == 1);
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
    printf("groups as added: with a sheet, %d tracks (%lld from sheets)\n", n, (long long)db_int(db3, "SELECT COUNT(*) FROM tracks WHERE cue<>0"));
    CHECK(db_int(db3, "SELECT COUNT(*) FROM tracks WHERE cue<>0") >= 2);
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
        CHECK(db_int(db, "SELECT value FROM meta WHERE key='scan_sig'") == db_int(db2, "SELECT value FROM meta WHERE key='scan_sig'"));
        if (!same) break;
    }
    /* stopped while reading new files, others removed: nothing dropped; the next scan drops them */
    int before = gw_n;
    for (int k = 0; k < 6; k++) { gw_write(music, gw_n, 1); gw_n++; }
    remove(gw_path[0]);
    remove(gw_path[1]);
    scan_job_t stop = {.lib = lib, .music = music, .cancel_after = 2};
    CHECK(library_scan(lib, music, job_progress, &stop) == -1);
    CHECK(db_int(db, "SELECT COUNT(*) FROM tracks") >= before);
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
    CHECK(db_int(db, "SELECT COUNT(*) FROM tracks WHERE cue<>0") == 2);
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
    CHECK(db_int(db2, "SELECT COUNT(*) FROM tracks WHERE path LIKE '%Died/003.flac'") == 0);
    CHECK(db_int(db2, "SELECT COUNT(*) FROM cues WHERE path LIKE '%Died/003.flac' AND kind=3") == 1);
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
    library_close(lib);                                          /* (its log emptied into the file) */
    CHECK(db_exec(db, "UPDATE tracks SET play_count=7, last_played=1700000000, favourite=1 WHERE path LIKE '%Check 03/005.flac';"
                      "UPDATE tracks SET skip_count=2, resume_ms=61000 WHERE path LIKE '%Check 07/001.flac';"
                      "INSERT INTO plays(track_id, at) SELECT id, 1700000000 FROM tracks WHERE path LIKE '%Check 03/005.flac'") == SQLITE_OK);
    /* a new library of the same music: what was played comes over by path */
    lib = library_open(db2, art);
    CHECK(lib && library_scan(lib, music, NULL, NULL) == 240);
    CHECK(library_import_counts(lib, db) == 2);
    CHECK(db_int(db2, "SELECT play_count*1000 + favourite FROM tracks WHERE path LIKE '%Check 03/005.flac'") == 7001);
    CHECK(db_int(db2, "SELECT skip_count*100000 + resume_ms FROM tracks WHERE path LIKE '%Check 07/001.flac'") == 261000);
    CHECK(db_int(db2, "SELECT COUNT(*) FROM plays p JOIN tracks t ON t.id=p.track_id WHERE t.path LIKE '%Check 03/005.flac'") == 1);
    CHECK(db_int(db2, "SELECT COUNT(*) FROM tracks WHERE play_count>0 OR skip_count>0 OR favourite=1") == 2);
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
    CHECK(db_exec(db, "UPDATE tracks SET play_count=3") == SQLITE_OK);
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
    CHECK(db_int(db4, "SELECT COUNT(*) FROM tracks WHERE play_count=3") == got);
    library_close(lib);
}

int main(void)
{
    char dir[200];
    test_tmp(dir, sizeof dir, "fathom-test-scan");
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; }
    test_open_and_levelling(dir);
    test_scans(dir);
    test_playlist_during_scan(dir);
    test_quick_listing(dir);
    test_groups_where_changed(dir);
    test_bulk_load(dir);
    test_groups_as_added(dir);
    test_known_files(dir);
    test_died_on(dir);
    test_check_and_import(dir);
    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    printf("scan: all good\n");
    return 0;
}
