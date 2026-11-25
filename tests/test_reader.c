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
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <FLAC/metadata.h>
#include <FLAC/stream_encoder.h>
#include <sqlite3.h>

#include "library/library.h"
#include "library/reader.h"
#include "tmpdir.h"

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static int helper_do(int op, const char *path, unsigned char **out, size_t *len)
{
    if (strstr(path, "KILL")) raise(SIGKILL);
    if (strstr(path, "HANG")) for (;;) sleep(100);
    return library_reader_do(op, path, out, len);
}

static int write_flac(const char *path, const char *title, const char *album, int track)
{
    FLAC__StreamEncoder *e = FLAC__stream_encoder_new();
    FLAC__stream_encoder_set_channels(e, 2);
    FLAC__stream_encoder_set_bits_per_sample(e, 16);
    FLAC__stream_encoder_set_sample_rate(e, 44100);
    FLAC__StreamMetadata *vc = FLAC__metadata_object_new(FLAC__METADATA_TYPE_VORBIS_COMMENT);
    char tr[8];
    snprintf(tr, sizeof tr, "%d", track);
    const char *kv[][2] = {{"TITLE", title}, {"ARTIST", "Some Artist"}, {"ALBUM", album}, {"TRACKNUMBER", tr}};
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

static int64_t db_int(const char *db, const char *sql)
{
    sqlite3 *d = NULL;
    sqlite3_stmt *s = NULL;
    int64_t v = -1;
    if (sqlite3_open_v2(db, &d, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK &&
        sqlite3_prepare_v2(d, sql, -1, &s, NULL) == SQLITE_OK && sqlite3_step(s) == SQLITE_ROW)
        v = sqlite3_column_int64(s, 0);
    sqlite3_finalize(s);
    sqlite3_close(d);
    return v;
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--reader")) return reader_serve(helper_do);

    char self[PATH_MAX], dir[300], music[400], p[700], db[400], db2[400], db3[400], art[400], art2[400], art3[400];
    if (!realpath(argv[0], self)) { perror(argv[0]); return 1; }
    test_tmp(dir, sizeof dir, "fathom-test-reader");
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; }
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
    CHECK(db_int(db, "SELECT COUNT(*) FROM cues WHERE kind=3 AND sheet='stopped'") == 4);
    CHECK(db_int(db, "SELECT COUNT(*) FROM cues WHERE kind=3 AND sheet='stopped' AND path LIKE '%KILL.flac'") == 3);
    CHECK(db_int(db, "SELECT COUNT(*) FROM cues WHERE kind=3 AND sheet='stopped' AND path LIKE '%HANG.flac'") == 1);
    CHECK(db_int(db, "SELECT COUNT(*) FROM tracks WHERE path LIKE '%KILL.flac' OR path LIKE '%HANG.flac'") == 0);
    CHECK(db_int(db, "SELECT COUNT(*) FROM tracks WHERE title='Good One 3' AND album='Good One' AND track=3 AND rate=44100") == 1);
    /* the songs it died on: two tries each; the one it hung on: one; the
     * cover that killed it: two, and that album simply has none */
    CHECK(reader_deaths() == 3 * 2 + 1 + 2);
    CHECK(db_int(db, "SELECT art FROM albums WHERE title='Good Two'") == LIB_COVER_OWN);
    CHECK(db_int(db, "SELECT art FROM albums WHERE title='Cover Trouble'") == LIB_COVER_NONE);
    CHECK(library_unread_count(lib) == 4);
    /* nothing more to do at the next scan, and no helper started for it */
    int starts = reader_starts();
    CHECK(library_scan(lib, music, NULL, NULL) == n && library_scan_changes(lib) == 0);
    CHECK(reader_starts() == starts);
    library_close(lib);

    /* the same music read in this process (no file kills this test: the
     * helper's whims aren't the reader's): what the helper read, it read
     * the same */
    reader_set_helper(NULL);
    CHECK(!reader_on());
    lib = library_open(db2, art2);
    CHECK(lib && library_scan(lib, music, NULL, NULL) == 6 + 4 + 5 + 2 + 1);
    library_close(lib);
    CHECK(db_int(db2, "SELECT COUNT(*) FROM cues WHERE kind=3") == 0);
    static const char *const same[] = {
        "SELECT COUNT(*) FROM albums",
        "SELECT COUNT(*) FROM tracks WHERE album='Good One'",
        "SELECT SUM(duration_ms) FROM tracks WHERE album IN ('Good One', 'Good Two')",
        "SELECT SUM(length(title) + track + bits + channels) FROM tracks WHERE album IN ('Good One', 'Good Two')",
        "SELECT art FROM albums WHERE title='Good Two'",
    };
    for (size_t i = 0; i < sizeof same / sizeof same[0]; i++) CHECK(db_int(db, same[i]) == db_int(db2, same[i]));

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
    CHECK(db_int(db4, "SELECT COUNT(*) FROM cues WHERE kind=3") == 0);
    CHECK(db_int(db4, "SELECT art FROM albums WHERE title='Cover Trouble'") == LIB_COVER_UNKNOWN);
    CHECK(db_int(db4, "SELECT art FROM albums WHERE title='Good Two'") == LIB_COVER_OWN);
    library_test_reader_budget(8000);
    CHECK(library_scan(lib, music, NULL, NULL) == 6 + 4 + 2 + 1 + 1);
    CHECK(db_int(db4, "SELECT COUNT(*) FROM cues WHERE kind=3 AND sheet='stopped'") == 4);
    CHECK(db_int(db4, "SELECT art FROM albums WHERE title='Cover Trouble'") == LIB_COVER_NONE);
    library_close(lib);
    library_test_reader_budget(-1);
    reader_set_helper(NULL);

    snprintf(p, sizeof p, "rm -rf '%s'", dir);
    if (system(p)) { /* (left behind: no harm) */ }
    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    printf("reader: all good\n");
    return 0;
}
