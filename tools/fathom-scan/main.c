/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * fathom-scan: build or update the library database from a music folder (read-only).
 *
 *   fathom-scan MUSIC_DIR [DB_PATH] [ART_DIR]
 *
 * Defaults: build/sim-data/library.db and build/sim-data/art.
 */
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "library/library.h"

static void progress(int done, int total, const char *path, void *ctx)
{
    (void)ctx;
    if (total > 0) fprintf(stderr, "\r%d/%d  %-60.60s", done, total, path);
    else fprintf(stderr, "\r%d  %-60.60s", done, path);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s MUSIC_DIR [DB_PATH] [ART_DIR]\n", argv[0]);
        return 2;
    }
    const char *db = argc > 2 ? argv[2] : "build/sim-data/library.db";
    const char *art = argc > 3 ? argv[3] : "build/sim-data/art";
    mkdir("build/sim-data", 0755);

    library_t *lib = library_open(db, art);
    if (!lib) return 1;
    clock_t t0 = clock();
    int n = library_scan(lib, argv[1], progress, NULL);
    double secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
    fprintf(stderr, "\n");

    lib_list_t albums, artists, songs;
    library_list(lib, LIB_ALBUMS, 0, &albums);
    library_list(lib, LIB_ARTISTS, 0, &artists);
    library_list(lib, LIB_SONGS, 0, &songs);
    printf("%d tracks, %d albums, %d artists (CPU %.1f s)\n", n, albums.n, artists.n, secs);

    int with_art = 0;
    static uint16_t px[LIB_ART_LARGE * LIB_ART_LARGE];
    for (int i = 0; i < albums.n; i++) with_art += library_art(lib, albums.ids[i], LIB_ART_LARGE, px) == 0;
    printf("cover art cached for %d of %d albums\n", with_art, albums.n);

    char main_txt[256], meta[32];
    printf("\nFirst albums:\n");
    for (int i = 0; i < albums.n && i < 8; i++) {
        library_row(lib, LIB_ALBUMS, albums.ids[i], main_txt, sizeof main_txt, meta, sizeof meta);
        printf("  %-40.40s %s\n", main_txt, meta);
    }
    library_list_free(&albums);
    library_list_free(&artists);
    library_list_free(&songs);
    library_close(lib);
    return n < 0;
}
