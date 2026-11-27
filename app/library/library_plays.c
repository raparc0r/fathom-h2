/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* A play written down: counted on its song and noted in the play log, at
 * once or, for the player, by a thread and a connection of its own so the
 * screen never waits for the card. Out of library.c, as it was there. */
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE
#define _GNU_SOURCE
#include "library/library_int.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 0, or -1 when the database was busy and nothing of it was written (another
 * connection writing for longer than this one waits). */
static int played_write(library_t *lib, int64_t track_id, int skipped_early, int64_t now)
{
    /* a play: counted, and logged (On repeat), the log kept to a year. One
     * commit for the lot (they were three, and the year-old plays were looked
     * for at every track's end: once since the library was opened is enough). */
    sqlite3 *db = lib->db;
    int tx = 0, busy = 0;
    if (!skipped_early && sqlite3_get_autocommit(db)) {
        /* Busy here is busy: nothing written, tried again by whoever asked.
         * (Going on without the transaction waited the ten seconds a second
         * time at the UPDATE, and if the other connection let go in between,
         * the count went in alone and the log's line could be refused.) */
        int rc = sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL);
        if (rc == SQLITE_BUSY || rc == SQLITE_LOCKED) return -1;
        tx = rc == SQLITE_OK;
    }
    sqlite3_stmt *s = prep(db, skipped_early
        ? "UPDATE tracks SET skip_count=skip_count+1 WHERE id=?2"
        : "UPDATE tracks SET play_count=play_count+1, last_played=?1 WHERE id=?2");
    if (s) {
        sqlite3_bind_int64(s, 1, now);
        sqlite3_bind_int64(s, 2, track_id);
        int rc = sqlite3_step(s);
        busy = rc == SQLITE_BUSY || rc == SQLITE_LOCKED;
        sqlite3_finalize(s);
    }
    if (busy) {                                       /* (nothing written: whoever asked may try again) */
        if (tx) sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        return -1;
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
    if (tx) db_commit(db);
    return 0;
}

void library_played(library_t *lib, int64_t track_id, int skipped_early, int64_t now)
{
    if (played_write(lib, track_id, skipped_early, now)) played_write(lib, track_id, skipped_early, now);   /* (busy: once more) */
}

/* ---- a play written off the caller's thread ----
 * The player takes up a song's end on the UI's thread, and writing the play
 * there held the screen: 51 ms measured on the H2 (an UPDATE, an INSERT and a
 * commit on the card), up to this connection's ten seconds of patience when a
 * scan's connection was writing, and any animation under way stood still for
 * it (2026-11-25: the cover flow's slide, 0.6 s once). library_played_later
 * hands the play to one thread for the whole program, which writes it
 * through a connection of its own to the same file (WAL: the caller's reads
 * go on meanwhile, and see it once it is in). The plays are written in the
 * order given; library_plays_wait waits until all are in; closing a library
 * does that for its file. A database with no file of its own (":memory:"),
 * no thread to be had, or more waiting than there is room for: written at
 * once, by the caller, as before. */
typedef struct { char db[1024]; int64_t id, now; int early; } play_t;
static struct {
    pthread_mutex_t mu;
    pthread_cond_t more, idle;
    play_t q[32];
    int n, busy, started;
    library_t *conn;        /* the thread's own connection, to the file last written to */
} PW = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, PTHREAD_COND_INITIALIZER, {{{0}, 0, 0, 0}}, 0, 0, 0, NULL};

static _Thread_local int t_plays_own;      /* this thread is the plays' own */

/* The plays' own connection: the file, and nothing else of a library. Not
 * library_open, which does what a library's first user wants done: it builds
 * the indexes a first scan set aside when it finds them waiting (so a song
 * that ended during a first scan in the background had them built over the
 * half-filled table, and the rest of the scan paid for every one), opens the
 * cover store, and readies nineteen statements this connection never runs.
 * Its page cache is small and given back after each play: an UPDATE touches
 * a few pages of the songs and their indexes, different ones each song, and
 * the usual half megabyte filled within a few songs and stayed for the
 * session. A file that isn't there is not made (the card may be out). */
static library_t *plays_open(const char *db_path)
{
    library_t *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->art_fd[0] = c->art_fd[1] = -1;
    c->plays_conn = 1;
    snprintf(c->db_path, sizeof c->db_path, "%s", db_path);
    if (sqlite3_open_v2(db_path, &c->db, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK ||
        sqlite3_busy_timeout(c->db, 10000) != SQLITE_OK ||
        sqlite3_exec(c->db, "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA cache_size=-64;", NULL, NULL, NULL) != SQLITE_OK) {
        fprintf(stderr, "library: the plays' connection to %s: %s\n", db_path, c->db ? sqlite3_errmsg(c->db) : "no memory");
        library_close(c);
        return NULL;
    }
    return c;
}

static void *plays_thread(void *arg)
{
    (void)arg;
    t_plays_own = 1;
    pthread_mutex_lock(&PW.mu);
    for (;;) {
        while (!PW.n) {
            PW.busy = 0;
            pthread_cond_broadcast(&PW.idle);
            pthread_cond_wait(&PW.more, &PW.mu);
        }
        play_t it = PW.q[0];
        memmove(&PW.q[0], &PW.q[1], (size_t)--PW.n * sizeof PW.q[0]);
        PW.busy = 1;
        library_t *c = PW.conn;
        if (c && strcmp(c->db_path, it.db)) {       /* another library's play: its own file */
            PW.conn = NULL;
            pthread_mutex_unlock(&PW.mu);
            library_close(c);
            c = NULL;
            pthread_mutex_lock(&PW.mu);
        }
        pthread_mutex_unlock(&PW.mu);
        if (!c) c = plays_open(it.db);
        int rc = -1;
        /* (another connection writing: this one waits its ten seconds each time; a scan commits far more often) */
        for (int tries = 0; c && rc && tries < 6; tries++) rc = played_write(c, it.id, it.early, it.now);
        if (c) sqlite3_db_release_memory(c->db);
        if (rc) fprintf(stderr, "library: a play of track %lld couldn't be written (%s)\n", (long long)it.id, c ? "the database stayed busy" : "no connection");
        pthread_mutex_lock(&PW.mu);
        PW.conn = c;
    }
    return NULL;
}

void library_played_later(library_t *lib, int64_t track_id, int skipped_early, int64_t now)
{
    int later = lib->db_path[0] && lib->db_path[0] != ':' && !lib->plays_conn;
    pthread_mutex_lock(&PW.mu);
    if (later && !PW.started) {
        pthread_t th;
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        PW.started = pthread_create(&th, &at, plays_thread, NULL) == 0;
        pthread_attr_destroy(&at);
    }
    later = later && PW.started && PW.n < (int)(sizeof PW.q / sizeof PW.q[0]);
    if (later) {
        play_t *it = &PW.q[PW.n++];
        snprintf(it->db, sizeof it->db, "%s", lib->db_path);
        it->id = track_id;
        it->now = now;
        it->early = skipped_early;
        PW.busy = 1;                                 /* (until the thread has said it is idle again) */
        pthread_cond_signal(&PW.more);
    }
    pthread_mutex_unlock(&PW.mu);
    if (!later) library_played(lib, track_id, skipped_early, now);
}

void library_plays_wait(void)
{
    pthread_mutex_lock(&PW.mu);
    while (PW.started && (PW.n || PW.busy)) pthread_cond_wait(&PW.idle, &PW.mu);
    pthread_mutex_unlock(&PW.mu);
}

/* A library is being closed: the plays given for later are written first, and
 * the thread's connection to its file is closed with it (the card may be
 * about to be unmounted: nothing of ours may hold a file on it open). */
void plays_release(const library_t *lib)
{
    if (t_plays_own) return;                         /* (its own connection that didn't open: nothing to wait for) */
    pthread_mutex_lock(&PW.mu);
    while (PW.started && (PW.n || PW.busy)) pthread_cond_wait(&PW.idle, &PW.mu);
    library_t *c = PW.conn && !strcmp(PW.conn->db_path, lib->db_path) ? PW.conn : NULL;
    if (c) PW.conn = NULL;
    pthread_mutex_unlock(&PW.mu);
    if (c) library_close(c);
}
