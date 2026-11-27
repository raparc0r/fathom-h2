/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* The helper that reads files for a scan (app/library/reader.c): this test
 * runs itself as the helper (--reader), one that is killed on any file with
 * KILL in its path, as if the kernel had taken it for want of memory, and
 * never answers for one with HANG in it. A scan through it reads what a scan
 * in this process reads; the files it dies on are set aside after two tries,
 * the one it hangs on after one, with the scan carrying on each time; covers
 * come through it; and with no helper to be had the scan does the work
 * itself. Run from the repo root (tests/data/covers/cover.jpg). */
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <sqlite3.h>

#include "core/clock.h"
#include "library/library.h"
#include "library/reader.h"
#include "test_db.h"
#include "test_flac.h"
#include "test_util.h"

static int helper_do(int op, const char *path, unsigned char **out, size_t *len)
{
    if (strstr(path, "KILL")) raise(SIGKILL);
    if (strstr(path, "HANG")) for (;;) sleep(100);
    return library_reader_do(op, path, out, len);
}

static int write_flac(const char *path, const char *title, const char *album, int track)
{
    char tr[8];
    snprintf(tr, sizeof tr, "%d", track);
    return t_write_flac(path, &(t_flac_t){.frames = 441, .tags = {{"TITLE", title}, {"ARTIST", "Some Artist"}, {"ALBUM", album},
                                                                  {"TRACKNUMBER", tr}}});
}

static void song(const char *music, const char *album, const char *file, int track)
{
    char path[700], title[200];
    snprintf(path, sizeof path, "%s/%s", music, album);
    mkdir(path, 0755);
    snprintf(path, sizeof path, "%s/%s/%s", music, album, file);
    snprintf(title, sizeof title, "%s %d", album, track);
    if (write_flac(path, title, album, track)) { fprintf(stderr, "cannot write %s\n", path); exit(1); }
}

static void copy_file(const char *from, const char *to)
{
    FILE *in = fopen(from, "rb"), *out = fopen(to, "wb");
    if (!in || !out) { perror(from); exit(1); }
    char b[4096];
    size_t n;
    while ((n = fread(b, 1, sizeof b, in)) > 0) fwrite(b, 1, n, out);
    fclose(in);
    fclose(out);
}

/* A long request given up when its caller's stop is set: the helper, in the
 * middle of the work, is killed, nobody is blamed, and the next request
 * finds a new one. (Levelling, stopped when music starts.) */
static volatile int g_stop;

static void *stop_soon(void *arg)
{
    (void)arg;
    usleep(400 * 1000);
    g_stop = 1;
    return NULL;
}

static void test_stopped(const char *music)
{
    char hang[700], good[700];
    snprintf(hang, sizeof hang, "%s/Slow/01 HANG.flac", music);
    snprintf(good, sizeof good, "%s/Good One/01.flac", music);
    int st = 0, deaths = reader_deaths(), starts = reader_starts();
    unsigned char *b = NULL;
    size_t n = 0;
    pthread_t th;
    g_stop = 0;
    double t0 = clock_fsec();
    pthread_create(&th, NULL, stop_soon, NULL);
    int r = reader_call_long(1, hang, 0, 60000, &g_stop, &st, &b, &n);
    double took = clock_fsec() - t0;
    pthread_join(th, NULL);
    CHECK(r == READER_STOPPED && b == NULL);
    CHECK(took > 0.3 && took < 2.0);                             /* (not the minute it was given) */
    CHECK(reader_deaths() == deaths);                            /* (nobody's fault) */
    /* a stop already set: given up at once, the file not even judged */
    r = reader_call_long(1, good, 0, 60000, &g_stop, &st, &b, &n);
    CHECK(r == READER_STOPPED);
    /* and the next request is answered, by a new helper */
    g_stop = 0;
    r = reader_call_long(1, good, 0, 60000, &g_stop, &st, &b, &n);
    CHECK(r == 0 && st == 0 && b != NULL && reader_starts() > starts);
    free(b);
    reader_close();
}

/* "Remove cover" on a picked cover (the album goes back to its own, made
 * again on the thread the screen is drawn on): not behind a helper that is
 * at work for someone else (a song being measured for the levelling:
 * minutes), and with little memory to spare a cover that couldn't be made
 * isn't "this album has none" (marked so, no scan looked again). */
static volatile int g_busy_stop;
static char g_busy_path[700];
static void *keep_busy(void *arg)
{
    (void)arg;
    int st;
    unsigned char *b;
    size_t n;
    reader_call_long(1, g_busy_path, 0, 60000, &g_busy_stop, &st, &b, &n);
    return NULL;
}

static void test_remove_cover(const char *dir, const char *music)
{
    char db[700], art[700], found[700], p[700], q[700];
    snprintf(db, sizeof db, "%s/remove.db", dir);
    snprintf(art, sizeof art, "%s/art-remove", dir);
    snprintf(found, sizeof found, "%s/found", dir);
    mkdir(found, 0755);
    library_set_found_covers(found);
    reader_set_timeout_ms(1500);
    library_t *lib = library_open(db, art);
    CHECK(lib && library_scan(lib, music, NULL, NULL) > 0);
    int64_t id = t_db_int_ro(db, "SELECT id FROM albums WHERE title='Good Two'");
    CHECK(library_album_cover(lib, id) == LIB_COVER_OWN);
    uint16_t *lg = calloc((size_t)LIB_ART_LARGE * LIB_ART_LARGE, 2), *sm = calloc((size_t)LIB_ART_SMALL * LIB_ART_SMALL, 2);
    CHECK(lg && sm && library_store_found_cover(lib, id, lg, sm) == 0 && library_album_cover(lib, id) == LIB_COVER_PICKED);
    /* the helper busy with a file it never answers for */
    snprintf(g_busy_path, sizeof g_busy_path, "%s/Slow/01 HANG.flac", music);
    g_busy_stop = 0;
    pthread_t th;
    pthread_create(&th, NULL, keep_busy, NULL);
    usleep(300 * 1000);
    double t0 = clock_fsec();
    CHECK(library_remove_found_cover(lib, id) == 0);
    double took = clock_fsec() - t0;
    CHECK(took < 1.0);                                           /* (not when the helper is free again) */
    if (took >= 1.0) fprintf(stderr, "Remove cover waited %.1f s for the helper\n", took);
    CHECK(library_album_cover(lib, id) == LIB_COVER_OWN);        /* its own, made here */
    g_busy_stop = 1;
    pthread_join(th, NULL);
    /* its own can't be made with little memory to spare (the helper dies on
     * it, and wasn't given much): no cover for now, and not "it has none" */
    CHECK(library_store_found_cover(lib, id, lg, sm) == 0 && library_album_cover(lib, id) == LIB_COVER_PICKED);
    snprintf(p, sizeof p, "%s/Good Two/cover.jpg", music);
    snprintf(q, sizeof q, "%s/Good Two/KILL.jpg", music);
    CHECK(rename(p, q) == 0);
    library_test_reader_budget(1000);
    CHECK(library_remove_found_cover(lib, id) == 0);
    CHECK(library_album_cover(lib, id) == LIB_COVER_UNKNOWN);
    CHECK(t_db_int_ro(db, "SELECT art_sig FROM albums WHERE title='Good Two'") == 0);
    library_test_reader_budget(-1);
    CHECK(rename(q, p) == 0);
    CHECK(library_scan(lib, music, NULL, NULL) > 0 && library_album_cover(lib, id) == LIB_COVER_OWN);   /* the next scan makes it */
    free(lg);
    free(sm);
    library_close(lib);
    library_set_found_covers(NULL);
    reader_close();
}

/* ---- a cover that couldn't be read just now is not "this album has none" ----
 * An album's image there and not to be opened (the card not answering, no
 * memory or file handles: here, its permissions): the album's cover stays as
 * it was, thumbnails and all, and the next scan looks again; so for a new
 * album (not looked at yet), and for Remove cover on one picked over its own
 * (no cover for now). Each was "it has none": the thumbnails emptied, the
 * album marked so and not looked at again until its files changed. A file
 * that is read and is no picture is still none. And a cover's state that
 * can't be read is not "no cover of its own": nothing is stored or removed.
 * self: through the helper (NULL: read in this process). */
static int db_run(const char *db, const char *sql)
{
    sqlite3 *d = NULL;
    int rc = sqlite3_open(db, &d) == SQLITE_OK ? sqlite3_exec(d, sql, NULL, NULL, NULL) : -1;
    sqlite3_close(d);
    return rc;
}

static void touch_at(const char *path, long when)
{
    struct timeval tv[2] = {{when, 0}, {when, 0}};
    utimes(path, tv);
}

static void test_cover_unread(const char *dir, const char *self)
{
    char music[700], db[700], art[700], found[700], cover[800], p[900], over[1200], jpg[1200], none[1200];
    snprintf(music, sizeof music, "%s/cmusic%d", dir, self != NULL);
    mkdir(music, 0755);
    snprintf(db, sizeof db, "%s/unread%d.db", dir, self != NULL);
    snprintf(art, sizeof art, "%s/art-unread%d", dir, self != NULL);
    snprintf(found, sizeof found, "%s/found-unread%d", dir, self != NULL);
    mkdir(found, 0755);
    library_set_found_covers(found);
    reader_set_helper(self);
    song(music, "Has Cover", "01.flac", 1);
    song(music, "Has Cover", "02.flac", 2);
    song(music, "Plain", "01.flac", 1);
    snprintf(cover, sizeof cover, "%s/Has Cover/cover.jpg", music);
    copy_file("tests/data/covers/cover.jpg", cover);
    const char *scan_sig = "SELECT value FROM meta WHERE key='scan_sig'";
    const char *art_sig = "SELECT art_sig FROM albums WHERE title='Has Cover'";
    library_t *lib = library_open(db, art);
    CHECK(lib && library_scan(lib, music, NULL, NULL) == 3);
    int64_t id = t_db_int_ro(db, "SELECT id FROM albums WHERE title='Has Cover'");
    lib_album_t a;
    CHECK(library_album_info(lib, id, &a) == 0);
    CHECK(library_found_cover_path(a.key, "over", over, sizeof over) == 0 && library_found_cover_path(a.key, "jpg", jpg, sizeof jpg) == 0 &&
          library_found_cover_path(a.key, "none", none, sizeof none) == 0);
    size_t n = (size_t)LIB_ART_SMALL * LIB_ART_SMALL * 2;
    uint16_t *was = malloc(n), *px = malloc(n);
    uint16_t *lg = calloc((size_t)LIB_ART_LARGE * LIB_ART_LARGE, 2), *sm = calloc((size_t)LIB_ART_SMALL * LIB_ART_SMALL, 2);
    CHECK(was && px && lg && sm);
    CHECK(library_album_cover(lib, id) == LIB_COVER_OWN && library_art(lib, id, LIB_ART_SMALL, was) == 0);
    CHECK(t_db_int_ro(db, scan_sig) != 0);
    if (geteuid() != 0) {                                        /* (root opens a file whatever its mode) */
        /* its image changed, and can't be opened */
        int64_t sig0 = t_db_int_ro(db, art_sig);
        touch_at(cover, 1700005000);
        chmod(cover, 0);
        CHECK(library_scan(lib, music, NULL, NULL) == 3);
        CHECK(library_album_cover(lib, id) == LIB_COVER_OWN && t_db_int_ro(db, art_sig) == sig0);
        CHECK(library_art(lib, id, LIB_ART_SMALL, px) == 0 && !memcmp(px, was, n));
        CHECK(t_db_int_ro(db, scan_sig) == 0);                        /* (the next scan looks again) */
        chmod(cover, 0644);
        CHECK(library_scan(lib, music, NULL, NULL) == 3);
        CHECK(library_album_cover(lib, id) == LIB_COVER_OWN && t_db_int_ro(db, art_sig) != sig0 && t_db_int_ro(db, scan_sig) != 0);
        /* a new album's */
        song(music, "New One", "01.flac", 1);
        snprintf(p, sizeof p, "%s/New One/cover.jpg", music);
        copy_file("tests/data/covers/cover.jpg", p);
        chmod(p, 0);
        CHECK(library_scan(lib, music, NULL, NULL) == 4);
        int64_t nid = t_db_int_ro(db, "SELECT id FROM albums WHERE title='New One'");
        CHECK(library_album_cover(lib, nid) == LIB_COVER_UNKNOWN && t_db_int_ro(db, scan_sig) == 0);
        chmod(p, 0644);
        CHECK(library_scan(lib, music, NULL, NULL) == 4);
        CHECK(library_album_cover(lib, nid) == LIB_COVER_OWN && t_db_int_ro(db, scan_sig) != 0);
        /* Remove cover on one picked over its own, its own not to be opened */
        CHECK(library_store_found_cover(lib, id, lg, sm) == 0 && library_album_cover(lib, id) == LIB_COVER_PICKED);
        chmod(cover, 0);
        CHECK(library_remove_found_cover(lib, id) == 0);
        CHECK(library_album_cover(lib, id) == LIB_COVER_UNKNOWN && t_db_int_ro(db, art_sig) == 0 && access(none, F_OK) != 0);
        chmod(cover, 0644);
        CHECK(library_scan(lib, music, NULL, NULL) == 4 && library_album_cover(lib, id) == LIB_COVER_OWN);
    }
    /* read, and no picture: none, its thumbnails gone; a picture again: its own */
    FILE *f = fopen(cover, "wb");
    if (f) { fputs("named like a picture, and not one: more than a few bytes of it\n", f); fclose(f); }
    touch_at(cover, 1700005100);
    CHECK(library_scan(lib, music, NULL, NULL) > 0);
    CHECK(library_album_cover(lib, id) == LIB_COVER_NONE && library_art(lib, id, LIB_ART_SMALL, px) != 0 && t_db_int_ro(db, scan_sig) != 0);
    copy_file("tests/data/covers/cover.jpg", cover);
    touch_at(cover, 1700005200);
    CHECK(library_scan(lib, music, NULL, NULL) > 0);
    CHECK(library_album_cover(lib, id) == LIB_COVER_OWN && library_art(lib, id, LIB_ART_SMALL, was) == 0);
    /* its state can't be read (a column it names away for a moment): a found
     * cover is not put over its own... */
    CHECK(db_run(db, "ALTER TABLE albums RENAME COLUMN art TO art_away") == SQLITE_OK);
    CHECK(library_store_found_cover(lib, id, lg, sm) != 0);
    CHECK(db_run(db, "ALTER TABLE albums RENAME COLUMN art_away TO art") == SQLITE_OK);
    CHECK(library_album_cover(lib, id) == LIB_COVER_OWN);
    CHECK(library_art(lib, id, LIB_ART_SMALL, px) == 0 && !memcmp(px, was, n));
    /* ...nor is one picked over its own removed */
    CHECK(library_store_found_cover(lib, id, lg, sm) == 0 && library_album_cover(lib, id) == LIB_COVER_PICKED);
    copy_file("tests/data/covers/cover.jpg", jpg);               /* (as the search keeps it) */
    CHECK(db_run(db, "ALTER TABLE albums RENAME COLUMN art TO art_away") == SQLITE_OK);
    CHECK(library_remove_found_cover(lib, id) != 0);
    CHECK(db_run(db, "ALTER TABLE albums RENAME COLUMN art_away TO art") == SQLITE_OK);
    CHECK(library_album_cover(lib, id) == LIB_COVER_PICKED && access(over, F_OK) == 0 && access(jpg, F_OK) == 0);
    CHECK(access(none, F_OK) != 0 && library_art(lib, id, LIB_ART_SMALL, px) == 0);
    /* where its own cover is can't be read: no cover for now, and the next scan makes it */
    CHECK(db_run(db, "ALTER TABLE albums RENAME COLUMN first_track TO first_away") == SQLITE_OK);
    CHECK(library_remove_found_cover(lib, id) == 0);
    CHECK(db_run(db, "ALTER TABLE albums RENAME COLUMN first_away TO first_track") == SQLITE_OK);
    CHECK(library_album_cover(lib, id) == LIB_COVER_UNKNOWN && t_db_int_ro(db, art_sig) == 0 && access(none, F_OK) != 0);
    CHECK(library_scan(lib, music, NULL, NULL) > 0 && library_album_cover(lib, id) == LIB_COVER_OWN);
    free(was);
    free(px);
    free(lg);
    free(sm);
    library_close(lib);
    library_set_found_covers(NULL);
    reader_close();
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--reader")) return reader_serve(helper_do);

    char self[PATH_MAX], dir[300], music[400], p[700], db[400], db2[400], db3[400], art[400], art2[400], art3[400];
    if (!realpath(argv[0], self)) { perror(argv[0]); return 1; }
    t_mkdtemp(dir, sizeof dir, "fathom-test-reader");
    snprintf(music, sizeof music, "%s/music", dir);
    mkdir(music, 0755);
    snprintf(db, sizeof db, "%s/helper.db", dir);
    snprintf(db2, sizeof db2, "%s/here.db", dir);
    snprintf(db3, sizeof db3, "%s/none.db", dir);
    snprintf(art, sizeof art, "%s/art", dir);
    snprintf(art2, sizeof art2, "%s/art2", dir);
    snprintf(art3, sizeof art3, "%s/art3", dir);

    /* Three good albums (one with a cover in its folder), an album whose
     * songs all kill the helper among two that don't, a file it hangs on,
     * and an album whose cover image kills it */
    for (int i = 1; i <= 6; i++) { snprintf(p, sizeof p, "%02d.flac", i); song(music, "Good One", p, i); }
    for (int i = 1; i <= 4; i++) { snprintf(p, sizeof p, "%02d.flac", i); song(music, "Good Two", p, i); }
    snprintf(p, sizeof p, "%s/Good Two/cover.jpg", music);
    copy_file("tests/data/covers/cover.jpg", p);
    song(music, "Bad Album", "01.flac", 1);
    song(music, "Bad Album", "02 KILL.flac", 2);
    song(music, "Bad Album", "03 KILL.flac", 3);
    song(music, "Bad Album", "04 KILL.flac", 4);
    song(music, "Bad Album", "05.flac", 5);
    song(music, "Slow", "01 HANG.flac", 1);
    song(music, "Slow", "02.flac", 2);
    song(music, "Cover Trouble", "01.flac", 1);
    snprintf(p, sizeof p, "%s/Cover Trouble/KILL.jpg", music);
    copy_file("tests/data/covers/cover.jpg", p);

    /* through the helper */
    reader_set_helper(self);
    reader_set_timeout_ms(1500);
    CHECK(reader_on());
    library_t *lib = library_open(db, art);
    int n = lib ? library_scan(lib, music, NULL, NULL) : -1;
    CHECK(n == 6 + 4 + 2 + 1 + 1);
    CHECK(t_db_int_ro(db, "SELECT COUNT(*) FROM cues WHERE kind=3 AND sheet='stopped'") == 4);
    CHECK(t_db_int_ro(db, "SELECT COUNT(*) FROM cues WHERE kind=3 AND sheet='stopped' AND path LIKE '%KILL.flac'") == 3);
    CHECK(t_db_int_ro(db, "SELECT COUNT(*) FROM cues WHERE kind=3 AND sheet='stopped' AND path LIKE '%HANG.flac'") == 1);
    CHECK(t_db_int_ro(db, "SELECT COUNT(*) FROM tracks WHERE path LIKE '%KILL.flac' OR path LIKE '%HANG.flac'") == 0);
    CHECK(t_db_int_ro(db, "SELECT COUNT(*) FROM tracks WHERE title='Good One 3' AND album='Good One' AND track=3 AND rate=44100") == 1);
    /* the songs it died on: two tries each; the one it hung on: one; the
     * cover that killed it: two, and that album simply has none */
    CHECK(reader_deaths() == 3 * 2 + 1 + 2);
    CHECK(t_db_int_ro(db, "SELECT art FROM albums WHERE title='Good Two'") == LIB_COVER_OWN);
    CHECK(t_db_int_ro(db, "SELECT art FROM albums WHERE title='Cover Trouble'") == LIB_COVER_NONE);
    CHECK(library_unread_count(lib) == 4);
    /* A scan asked to try the unread files again (Rescan), with a helper to
     * do the reading: those a reader stopped on are tried too. They stop it
     * again (two deaths each, one hang), and are set aside as before. */
    int deaths = reader_deaths();
    library_scan_retry_unread(lib, 1);
    CHECK(library_scan(lib, music, NULL, NULL) == n);
    CHECK(reader_deaths() == deaths + 3 * 2 + 1);
    CHECK(t_db_int_ro(db, "SELECT COUNT(*) FROM cues WHERE kind=3 AND sheet='stopped'") == 4);
    /* nothing more to do at the next scan, and no helper started for it */
    int starts = reader_starts();
    CHECK(library_scan(lib, music, NULL, NULL) == n && library_scan_changes(lib) == 0);
    CHECK(reader_starts() == starts);
    library_close(lib);

    test_stopped(music);
    test_remove_cover(dir, music);
    test_cover_unread(dir, self);
    test_cover_unread(dir, NULL);
    reader_set_helper(self);
    reader_set_timeout_ms(1500);

    /* the same music read in this process (no file kills this test: the
     * helper's whims aren't the reader's): what the helper read, it read
     * the same */
    reader_set_helper(NULL);
    CHECK(!reader_on());
    lib = library_open(db2, art2);
    CHECK(lib && library_scan(lib, music, NULL, NULL) == 6 + 4 + 5 + 2 + 1);
    library_close(lib);
    CHECK(t_db_int_ro(db2, "SELECT COUNT(*) FROM cues WHERE kind=3") == 0);
    static const char *const same[] = {
        "SELECT COUNT(*) FROM albums",
        "SELECT COUNT(*) FROM tracks WHERE album='Good One'",
        "SELECT SUM(duration_ms) FROM tracks WHERE album IN ('Good One', 'Good Two')",
        "SELECT SUM(length(title) + track + bits + channels) FROM tracks WHERE album IN ('Good One', 'Good Two')",
        "SELECT art FROM albums WHERE title='Good Two'",
    };
    for (size_t i = 0; i < sizeof same / sizeof same[0]; i++) CHECK(t_db_int_ro(db, same[i]) == t_db_int_ro(db2, same[i]));

    /* a helper that can't be started: said once, and the scan does the work itself */
    reader_set_helper("/no/such/program");
    lib = library_open(db3, art3);
    CHECK(lib && library_scan(lib, music, NULL, NULL) == 6 + 4 + 5 + 2 + 1);
    CHECK(!reader_on());
    library_close(lib);
    reader_set_helper(NULL);

    /* The helper short of memory for a file (the player sets what it may
     * take from what's available): a file that fails then isn't judged, nor
     * an album's cover; with enough memory at a later scan, they are */
    char db4[400], art4[400];
    snprintf(db4, sizeof db4, "%s/short.db", dir);
    snprintf(art4, sizeof art4, "%s/art4", dir);
    reader_set_helper(self);
    library_test_reader_budget(1000);
    lib = library_open(db4, art4);
    CHECK(lib && library_scan(lib, music, NULL, NULL) == 6 + 4 + 2 + 1 + 1);
    CHECK(t_db_int_ro(db4, "SELECT COUNT(*) FROM cues WHERE kind=3") == 0);
    CHECK(t_db_int_ro(db4, "SELECT art FROM albums WHERE title='Cover Trouble'") == LIB_COVER_UNKNOWN);
    CHECK(t_db_int_ro(db4, "SELECT art FROM albums WHERE title='Good Two'") == LIB_COVER_OWN);
    library_test_reader_budget(8000);
    CHECK(library_scan(lib, music, NULL, NULL) == 6 + 4 + 2 + 1 + 1);
    CHECK(t_db_int_ro(db4, "SELECT COUNT(*) FROM cues WHERE kind=3 AND sheet='stopped'") == 4);
    CHECK(t_db_int_ro(db4, "SELECT art FROM albums WHERE title='Cover Trouble'") == LIB_COVER_NONE);
    /* An album that has its answer already (no cover), then an image in its
     * folder, looked at while memory is short: left for later, and the next
     * scan does look (it took the listing being as it was for "nothing to
     * do", and the cover never came) */
    snprintf(p, sizeof p, "%s/Good One/KILL.jpg", music);
    copy_file("tests/data/covers/cover.jpg", p);
    CHECK(t_db_int_ro(db4, "SELECT art FROM albums WHERE title='Good One'") == LIB_COVER_NONE);
    library_test_reader_budget(1000);
    CHECK(library_scan(lib, music, NULL, NULL) == 6 + 4 + 2 + 1 + 1);
    deaths = reader_deaths();
    library_test_reader_budget(8000);
    CHECK(library_scan(lib, music, NULL, NULL) == 6 + 4 + 2 + 1 + 1);
    CHECK(reader_deaths() == deaths + 2);                  /* looked at again (and it kills the helper: twice) */
    deaths = reader_deaths();
    CHECK(library_scan(lib, music, NULL, NULL) == 6 + 4 + 2 + 1 + 1 && reader_deaths() == deaths);   /* settled now */
    /* A Rescan (the files not read tried again) while memory is short: what
     * was set aside stays set aside (it was dropped from the list, and read
     * again at every scan) */
    CHECK(library_unread_count(lib) == 4);
    library_test_reader_budget(1000);
    library_scan_retry_unread(lib, 1);
    CHECK(library_scan(lib, music, NULL, NULL) == 6 + 4 + 2 + 1 + 1);
    CHECK(library_unread_count(lib) == 4);
    CHECK(t_db_int_ro(db4, "SELECT COUNT(*) FROM cues WHERE kind=3 AND sheet='stopped'") == 4);
    library_close(lib);
    library_test_reader_budget(-1);
    reader_set_helper(NULL);

    t_rm_rf(dir);                             /* (left behind if it can't be: no harm) */
    return t_result();
}
