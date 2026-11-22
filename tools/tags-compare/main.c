/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/*
 * The one-pass FLAC tag reader (app/library/tags.c) against libFLAC's, on
 * every FLAC under a folder (read only): every field must match.
 *
 *   tags-compare DIR
 */
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

#include "library/tags.h"

static int g_files, g_diff;
static double g_new, g_ref;

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

#define SAME_STR(f) if (strcmp(a.f, b.f)) { printf("%s: %s \"%s\" vs \"%s\"\n", path, #f, a.f, b.f); bad = 1; }
#define SAME_NUM(f) if (a.f != b.f) { printf("%s: %s %g vs %g\n", path, #f, (double)a.f, (double)b.f); bad = 1; }

static void check(const char *path)
{
    tags_t a, b;
    double t0 = now();
    int ra = tags_read(path, &a);
    double t1 = now();
    int rb = tags_read_flac_reference(path, &b);
    g_new += t1 - t0;
    g_ref += now() - t1;
    g_files++;
    int bad = 0;
    if (ra != rb) { printf("%s: read %d vs %d\n", path, ra, rb); bad = 1; }
    else if (ra == 0) {
        SAME_STR(title) SAME_STR(artist) SAME_STR(album_artist) SAME_STR(album) SAME_STR(genre)
        SAME_STR(composer) SAME_STR(codec)
        SAME_NUM(year) SAME_NUM(disc) SAME_NUM(track) SAME_NUM(sample_rate) SAME_NUM(bits)
        SAME_NUM(channels) SAME_NUM(duration_ms) SAME_NUM(has_picture)
        SAME_NUM(has_rg_track) SAME_NUM(has_rg_album) SAME_NUM(rg_track_gain) SAME_NUM(rg_track_peak)
        SAME_NUM(rg_album_gain) SAME_NUM(rg_album_peak)
    }
    g_diff += bad;
}

static void walk(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    char path[4096];
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(path, &st)) continue;
        if (S_ISDIR(st.st_mode)) walk(path);
        else {
            size_t n = strlen(e->d_name);
            if (n > 5 && !strcasecmp(e->d_name + n - 5, ".flac")) check(path);
        }
    }
    closedir(d);
}

int main(int argc, char **argv)
{
    if (argc != 2) { fprintf(stderr, "usage: tags-compare DIR\n"); return 2; }
    walk(argv[1]);
    printf("%d FLAC files, %d differ; one pass %.2f s, libFLAC %.2f s\n", g_files, g_diff, g_new, g_ref);
    return g_diff ? 1 : 0;
}
