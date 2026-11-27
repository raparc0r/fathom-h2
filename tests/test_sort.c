/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* How albums and songs sort (app/library/library.c): leading punctuation
 * skipped, "the " too for albums, and an older library re-sorted on opening. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <sqlite3.h>

#include "library/library.h"
#include "test_flac.h"
#include "test_util.h"

static const char *const ALBUMS[][2] = {   /* album, its one song */
    {"Zebra", "Alpha"}, {"(What's the Story) Morning Glory", "(Intro)"}, {"Apple", "Beta"}, {"The Wall", "\"Quoted\""},
};
#define N 4

static int write_flac(const char *path, const char *album, const char *title)
{
    return t_write_flac(path, &(t_flac_t){.frames = 4410, .tags = {{"ALBUM", album}, {"TITLE", title}, {"ARTIST", "Someone"}}});
}

static void names(library_t *lib, lib_view_t v, char out[N][64])
{
    lib_list_t l;
    char meta[64];
    memset(out, 0, N * 64);
    int n = library_list(lib, v, 0, &l);
    for (int i = 0; i < n && i < N; i++) library_row(lib, v, l.ids[i], out[i], 64, meta, sizeof meta);
    library_list_free(&l);
}

static void check_order(library_t *lib)
{
    char a[N][64], s[N][64];
    names(lib, LIB_ALBUMS, a);
    CHECK(!strcmp(a[0], "Apple") && !strcmp(a[1], "The Wall") && !strcmp(a[2], "(What's the Story) Morning Glory") &&
          !strcmp(a[3], "Zebra"));
    names(lib, LIB_SONGS, s);
    CHECK(!strcmp(s[0], "Alpha") && !strcmp(s[1], "Beta") && !strcmp(s[2], "(Intro)") && !strcmp(s[3], "\"Quoted\""));
}

int main(void)
{
    char dir[200];
    t_mkdtemp(dir, sizeof dir, "fathom-test-sort");
    char music[300], path[400], db[300], art[300];
    snprintf(music, sizeof music, "%s/music", dir);
    mkdir(music, 0755);
    for (int i = 0; i < N; i++) {
        snprintf(path, sizeof path, "%s/%d.flac", music, i);
        if (write_flac(path, ALBUMS[i][0], ALBUMS[i][1])) { fprintf(stderr, "cannot write %s\n", path); return 1; }
    }
    snprintf(db, sizeof db, "%s/lib.db", dir);
    snprintf(art, sizeof art, "%s/art", dir);
    library_t *lib = library_open(db, art);
    CHECK(lib && library_scan(lib, music, NULL, NULL) == N);
    check_order(lib);
    library_close(lib);

    /* a library from before: songs by title, albums by the old sort (and
     * its schema's version then: opening upgrades only what's older) */
    sqlite3 *raw = NULL;
    CHECK(sqlite3_open(db, &raw) == SQLITE_OK);
    CHECK(sqlite3_exec(raw, "UPDATE tracks SET sort=NULL; UPDATE albums SET sort=lower(title); PRAGMA user_version=4;",
                       NULL, NULL, NULL) == SQLITE_OK);
    sqlite3_close(raw);
    lib = library_open(db, art);
    CHECK(lib != NULL);
    check_order(lib);
    library_close(lib);

    return t_result();
}
