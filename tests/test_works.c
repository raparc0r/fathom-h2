/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* Works for shuffle's "Keep works together" (library_shuffle_tracks): consecutive
 * tracks of an album whose titles share a prefix before ": ", two or more, form
 * one unit; a prefix on its own, or split by another track, doesn't. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "library/library.h"
#include "test_flac.h"
#include "test_util.h"

static const char *const TITLES[] = {
    "Suite in D: I. Prelude", "Suite in D: II. Air", "Suite in D: III. Gigue",   /* a work in three parts */
    "Interlude: Rain",                                                          /* a prefix on its own */
    "Song: Live", "Another Song", "Song: Live again",                           /* split: no work */
};
#define N ((int)(sizeof TITLES / sizeof TITLES[0]))

static int write_flac(const char *path, int track)
{
    char tr[64];
    snprintf(tr, sizeof tr, "%d", track);
    return t_write_flac(path, &(t_flac_t){.frames = 4410, .tags = {{"TITLE", TITLES[track - 1]}, {"ARTIST", "Some Ensemble"},
                                                                   {"ALBUM", "Suites"}, {"TRACKNUMBER", tr}}});
}

int main(void)
{
    char dir[200];
    t_mkdtemp(dir, sizeof dir, "fathom-test-works");
    char path[300], music[300], db[300], art[300];
    snprintf(music, sizeof music, "%s/music", dir);
    mkdir(music, 0755);
    for (int i = 1; i <= N; i++) {
        snprintf(path, sizeof path, "%s/%02d.flac", music, i);
        if (write_flac(path, i)) { fprintf(stderr, "cannot write %s\n", path); return 1; }
    }
    snprintf(db, sizeof db, "%s/lib.db", dir);
    snprintf(art, sizeof art, "%s/art", dir);
    library_t *lib = library_open(db, art);
    CHECK(lib && library_scan(lib, music, NULL, NULL) == N);

    shuf_track_t *t = NULL;
    int n = library_shuffle_tracks(lib, &t);
    CHECK(n == N);
    uint32_t unit[N + 1] = {0};
    for (int i = 0; i < n; i++) if (t[i].track >= 1 && t[i].track <= N) unit[t[i].track] = t[i].unit;
    CHECK(unit[1] && unit[1] == unit[2] && unit[2] == unit[3]);   /* the suite, together */
    CHECK(unit[4] == 0);                                          /* a lone prefix */
    CHECK(unit[5] == 0 && unit[6] == 0 && unit[7] == 0);          /* split by another track */
    /* the same songs asked for by id, in another order (a playlist shuffled
     * without reading the whole library): the same rows, the same works; one
     * that's gone comes back as its id alone */
    int64_t ids[N + 1];
    shuf_track_t by[N + 1];
    for (int i = 0; i < n; i++) ids[i] = t[n - 1 - i].id;
    ids[n] = 99999;
    CHECK(library_shuffle_tracks_for(lib, ids, n + 1, by) == n + 1);
    for (int i = 0; i < n; i++) CHECK(!memcmp(&by[i], &t[n - 1 - i], sizeof by[i]));
    CHECK(by[n].id == 99999 && by[n].album == 0 && by[n].duration_ms == 0 && by[n].unit == 0);
    /* a few of them only: works among those asked for (two parts of the suite still go together) */
    int64_t two[2] = {0, 0};
    for (int i = 0; i < n; i++) {
        if (t[i].track == 1) two[0] = t[i].id;
        if (t[i].track == 3) two[1] = t[i].id;
    }
    CHECK(library_shuffle_tracks_for(lib, two, 2, by) == 2 && by[0].unit && by[0].unit == by[1].unit);
    CHECK(library_top_track(lib) >= N);
    free(t);
    library_close(lib);

    return t_result();
}
