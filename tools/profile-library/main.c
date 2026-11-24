/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/*
 * The library calls behind the actions that are slow on a big library, each
 * timed, on a copy of a real library.db:
 *
 *   FATHOM_SQL_SLOW_MS=20 build/profile-library DIR    (DIR: library.db, queue.txt)
 *
 * Start-up (open, the "any songs?" check, the queue's restore), Songs, the
 * other top lists, Playlists (each smart playlist), Shuffle all songs.
 * Opens a copy of the database (DIR/profile.db), so plays and the queue
 * written by the shuffle don't change the original. Then compacts the copy
 * (library_compact) and times it all again: what compacting gives. The page
 * cache is dropped before each round where it can be (root on Linux).
 * SQLite's temporary files go to DIR, as in the player (never /tmp: RAM on
 * the H2).
 *
 *   profile-library --search DIR "WORDS"...
 *
 * Each search on DIR/library.db itself (opened, and upgraded, as it is: give
 * it a copy), three times over, with what it found.
 *
 *   profile-library --rescan DIR MUSIC [DEV MOUNT]
 *
 * One scan of MUSIC into a copy of DIR/library.db, timed, as the player's
 * background scan after USB drive mode (DEV mounted at MOUNT: the quick FAT
 * listing, as fast_walk_setup() gives it). The library logs its steps.
 *
 *   profile-library --scan DIR MUSIC [DEV MOUNT]
 *
 * A first scan of MUSIC into a new library (DIR/scan.db, DIR/scan-art, both
 * removed first and left afterwards to look at), timed, then compacted as the
 * player does, and the lists timed on the result.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sqlite3.h>
#include <unistd.h>

#include "audio/engine.h"
#include "audio/output.h"
#include "library/library.h"
#include "player/player.h"

static double now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

#define TIME(label, expr) do { \
    fprintf(stderr, "-- %s\n", label); \
    double t0_ = now_ms(); \
    long r_ = (long)(expr); \
    printf("%-44s %9.1f ms   (%ld)\n", label, now_ms() - t0_, r_); \
    fflush(stdout); \
} while (0)

static int copy(const char *from, const char *to)
{
    FILE *a = fopen(from, "rb"), *b = a ? fopen(to, "wb") : NULL;
    if (!a || !b) { if (a) fclose(a); return -1; }
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, a)) > 0) fwrite(buf, 1, n, b);
    fclose(a);
    fclose(b);
    return 0;
}

static long list(library_t *lib, lib_view_t v, int64_t parent)
{
    lib_list_t l;
    int n = library_list(lib, v, parent, &l);
    if (n > 0) library_list_free(&l);
    return n;
}

static long rows(library_t *lib, lib_view_t v, int count)
{
    lib_list_t l;
    int n = library_list(lib, v, 0, &l);
    char m[256], meta[64];
    for (int i = 0; i < count && i < n; i++) library_row(lib, v, l.ids[i], m, sizeof m, meta, sizeof meta);
    if (n > 0) library_list_free(&l);
    return n;
}

/* One read of the whole tracks table, as the slow queries make (a LIKE no
 * index can answer), on a connection of its own */
static long whole_read(const char *db)
{
    sqlite3 *d = NULL;
    long n = -1;
    if (sqlite3_open_v2(db, &d, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK) {
        sqlite3_exec(d, "PRAGMA cache_size=-512", NULL, NULL, NULL);
        sqlite3_stmt *s = NULL;
        if (sqlite3_prepare_v2(d, "SELECT count(*) FROM tracks WHERE title LIKE '%\x01%' OR path LIKE '%\x01%'",
                               -1, &s, NULL) == SQLITE_OK && sqlite3_step(s) == SQLITE_ROW)
            n = (long)sqlite3_column_int64(s, 0);
        sqlite3_finalize(s);
    }
    sqlite3_close(d);
    return n;
}

static void drop_caches(void)
{
    if (getenv("PROFILE_KEEP_CACHE")) return;   /* (a second run with everything read already: the CPU's share) */
    FILE *f = fopen("/proc/sys/vm/drop_caches", "w");
    if (f) { sync(); fputs("3\n", f); fclose(f); }
}

static void round_of(library_t *lib, player_t *p, const char *work, const char *queue)
{
    drop_caches();
    TIME("a whole read of the tracks table", whole_read(work));
    TIME("start: any songs? (library_empty)", !library_empty(lib));
    TIME("start: player_restore (queue.txt)", player_restore(p, queue) == 0);
    TIME("Songs: list + 5 rows", rows(lib, LIB_SONGS, 5));
    TIME("Albums: list + 5 rows", rows(lib, LIB_ALBUMS, 5));
    TIME("Artists: list + 5 rows", rows(lib, LIB_ARTISTS, 5));
    TIME("Album artists: list + 5 rows", rows(lib, LIB_ALBUM_ARTISTS, 5));
    TIME("Genres: list", list(lib, LIB_GENRES, 0));
    TIME("Playlists: list", list(lib, LIB_PLAYLISTS, 0));
    for (int k = 1; k < LIB_SMART_N; k++) {
        char label[80];
        snprintf(label, sizeof label, "Playlists: count %d (%s)", k, library_smart_name(k));
        TIME(label, library_smart_count(lib, k));
    }
    TIME("Never played: open (the whole list)", list(lib, LIB_SMART, LIB_SMART_NEVER));
    TIME("Favourites", list(lib, LIB_FAVOURITES, 0));
    TIME("History", list(lib, LIB_RECENT_PLAYED, 0));
    TIME("Recently added", list(lib, LIB_RECENT_ADDED, 0));
    TIME("Shuffle all songs (player_shuffle_all)", (player_shuffle_all(p), player_queue_len(p)));
}

static int rescan(int argc, char **argv)
{
    if (argc != 4 && argc != 6) { fprintf(stderr, "usage: %s --rescan DIR MUSIC [DEV MOUNT]\n", argv[0]); return 2; }
    char db[600], work[600], art[600];
    snprintf(db, sizeof db, "%s/library.db", argv[2]);
    snprintf(work, sizeof work, "%s/profile.db", argv[2]);
    snprintf(art, sizeof art, "%s/art", argv[2]);
    setenv("SQLITE_TMPDIR", argv[2], 1);
    remove(work);
    if (copy(db, work)) { perror(db); return 1; }
    library_t *lib = NULL;
    TIME("library_open (and any upgrade)", (lib = library_open(work, art)) != NULL);
    if (!lib) return 1;
    if (argc == 6) library_set_fast_walk(lib, argv[4], argv[5]);
    drop_caches();
    TIME("the scan (library_scan)", library_scan(lib, argv[3], NULL, NULL));
    printf("changes: %d\n", library_scan_changes(lib));
    library_close(lib);
    remove(work);
    return 0;
}

static int first_scan(int argc, char **argv)
{
    if (argc != 4 && argc != 6) { fprintf(stderr, "usage: %s --scan DIR MUSIC [DEV MOUNT]\n", argv[0]); return 2; }
    char work[600], art[600], cmd[1400];
    snprintf(work, sizeof work, "%s/scan.db", argv[2]);
    snprintf(art, sizeof art, "%s/scan-art", argv[2]);
    setenv("SQLITE_TMPDIR", argv[2], 1);
    snprintf(cmd, sizeof cmd, "rm -rf '%s' '%s-wal' '%s-shm' '%s'; mkdir -p '%s'", work, work, work, art, art);
    if (system(cmd)) return 1;
    library_t *lib = NULL;
    TIME("library_open (new)", (lib = library_open(work, art)) != NULL);
    if (!lib) return 1;
    if (argc == 6) library_set_fast_walk(lib, argv[4], argv[5]);
    drop_caches();
    if (getenv("PROFILE_NO_ART")) library_scan_defer_art(lib, 1);   /* (as the player's first scan: covers later) */
    TIME("the first scan (library_scan)", library_scan(lib, argv[3], NULL, NULL));
    TIME("compact (library_compact)", library_compact(lib));
    drop_caches();
    TIME("Songs: list + 5 rows", rows(lib, LIB_SONGS, 5));
    TIME("Albums: list + 5 rows", rows(lib, LIB_ALBUMS, 5));
    TIME("Never played: open (the whole list)", list(lib, LIB_SMART, LIB_SMART_NEVER));
    library_close(lib);
    return 0;
}

/* --again DIR MUSIC [DEV MOUNT]: DIR/scan.db as --scan left it, scanned again
 * (nothing copied: what a second scan costs when nothing changed, or after
 * files were added) */
static int again(int argc, char **argv)
{
    if (argc != 4 && argc != 6) { fprintf(stderr, "usage: %s --again DIR MUSIC [DEV MOUNT]\n", argv[0]); return 2; }
    char work[600], art[600];
    snprintf(work, sizeof work, "%s/scan.db", argv[2]);
    snprintf(art, sizeof art, "%s/scan-art", argv[2]);
    setenv("SQLITE_TMPDIR", argv[2], 1);
    library_t *lib = NULL;
    TIME("library_open", (lib = library_open(work, art)) != NULL);
    if (!lib) return 1;
    if (argc == 6) library_set_fast_walk(lib, argv[4], argv[5]);
    drop_caches();
    TIME("the scan (library_scan)", library_scan(lib, argv[3], NULL, NULL));
    printf("changes: %d\n", library_scan_changes(lib));
    library_close(lib);
    return 0;
}

/* --check DIR: DIR/scan.db as --scan left it, read through as the player does
 * after a stop that wasn't clean (cold, then again warm) */
static int check(int argc, char **argv)
{
    if (argc != 3) { fprintf(stderr, "usage: %s --check DIR\n", argv[0]); return 2; }
    char work[600], art[600];
    snprintf(work, sizeof work, "%s/scan.db", argv[2]);
    snprintf(art, sizeof art, "%s/scan-art", argv[2]);
    library_t *lib = NULL;
    TIME("library_open", (lib = library_open(work, art)) != NULL);
    if (!lib) return 1;
    drop_caches();
    TIME("the check, cold (library_check)", library_check(lib) == 0);
    TIME("the check, warm (library_check)", library_check(lib) == 0);
    library_close(lib);
    return 0;
}

/* --search DIR WORDS...: each of the searches typed on DIR/library.db (opened
 * as it is: a copy made for this), the first cold and every one three times
 * over: what a letter typed on Search waits for. */
static int search(int argc, char **argv)
{
    if (argc < 4) { fprintf(stderr, "usage: %s --search DIR \"WORDS\"...\n", argv[0]); return 2; }
    char db[600], art[600];
    snprintf(db, sizeof db, "%s/library.db", argv[2]);
    snprintf(art, sizeof art, "%s/art", argv[2]);
    setenv("SQLITE_TMPDIR", argv[2], 1);
    library_t *lib = NULL;
    TIME("library_open", (lib = library_open(db, art)) != NULL);
    if (!lib) return 1;
    drop_caches();
    for (int i = 3; i < argc; i++) {
        double ms[3];
        int n = 0, tiers[7] = {0};
        unsigned set = 0;                            /* (what was found, whatever its order: two builds compared) */
        for (int k = 0; k < 3; k++) {
            lib_search_t r;
            double t0 = now_ms();
            n = library_search(lib, argv[i], &r);
            ms[k] = now_ms() - t0;
            for (int j = 0; !k && j < r.n; j++) {
                tiers[r.hits[j].tier < 7 ? r.hits[j].tier : 0]++;
                set += (unsigned)(r.hits[j].id * 31 + r.hits[j].kind * 7 + r.hits[j].tier) * 2654435761u;
            }
            library_search_free(&r);
        }
        printf("search %-18s %8.1f %8.1f %8.1f ms   (%d found: %d %d %d %d %d %d by closeness, set %08x)%s\n", argv[i],
               ms[0], ms[1], ms[2], n, tiers[1], tiers[2], tiers[3], tiers[4], tiers[5], tiers[6], set,
               i == 3 ? "   (the first: nothing read yet)" : "");
        fflush(stdout);
    }
    library_close(lib);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "--search")) return search(argc, argv);
    if (argc > 1 && !strcmp(argv[1], "--check")) return check(argc, argv);
    if (argc > 1 && !strcmp(argv[1], "--rescan")) return rescan(argc, argv);
    if (argc > 1 && !strcmp(argv[1], "--again")) return again(argc, argv);
    if (argc > 1 && !strcmp(argv[1], "--scan")) return first_scan(argc, argv);
    if (argc != 2) { fprintf(stderr, "usage: %s DIR\n", argv[0]); return 2; }
    char db[600], work[600], art[600], queue[600];
    snprintf(db, sizeof db, "%s/library.db", argv[1]);
    snprintf(work, sizeof work, "%s/profile.db", argv[1]);
    snprintf(art, sizeof art, "%s/art", argv[1]);
    snprintf(queue, sizeof queue, "%s/queue.txt", argv[1]);
    setenv("SQLITE_TMPDIR", argv[1], 1);
    remove(work);
    if (copy(db, work)) { perror(db); return 1; }

    library_t *lib = NULL;
    TIME("start: library_open (and any upgrade)", (lib = library_open(work, art)) != NULL);
    if (!lib) return 1;
    printf("plan check: %d to fix\n", library_check_plans(lib, stdout));
    audio_output_t out;
    out_null_stats_t st;
    out_null_create(&out, &st);
    engine_t *eng = engine_new(&out);
    player_t *p = player_new(lib, eng);
    printf("== as it is\n");
    round_of(lib, p, work, queue);
    drop_caches();
    TIME("compact the database (library_compact)", library_compact(lib));
    printf("== compacted\n");
    round_of(lib, p, work, queue);
    player_free(p);
    engine_free(eng);
    library_close(lib);
    remove(work);
    return 0;
}
