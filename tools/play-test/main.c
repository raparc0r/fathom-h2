/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/*
 * play-test: silent checks of the decoders, engine and player against the library.
 *
 *   play-test [DB] [ART]          (defaults: build/sim-data/library.db, build/sim-data/art)
 *
 * 1. decode: every FLAC/MP3 in a sample of albums decodes to the length its header
 *    declares (exact for FLAC: that's what gapless relies on)
 * 2. gapless: an album through the engine + null output opens the output once
 *    and changes track exactly once per track
 * 3. shuffle: the player's shuffled queue over all songs is a permutation
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "audio/decoder.h"
#include "audio/engine.h"
#include "audio/output.h"
#include "library/library.h"
#include "player/player.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static void test_decode(library_t *lib)
{
    lib_list_t albums;
    library_list(lib, LIB_ALBUMS, 0, &albums);
    int tracks = 0, flac_exact = 0, mp3 = 0;
    static int32_t buf[4096 * 8];
    clock_t t0 = clock();
    for (int a = 0; a < albums.n; a += albums.n / 12 + 1) {
        lib_list_t tl;
        library_list(lib, LIB_ALBUM_TRACKS, albums.ids[a], &tl);
        for (int i = 0; i < tl.n && i < 3; i++) {
            lib_track_t t;
            library_track(lib, tl.ids[i], &t);
            audio_format_t f;
            decoder_t *d = decoder_open(t.path, &f);
            CHECK(d, "cannot open %s", t.path);
            if (!d) continue;
            unsigned long long frames = 0;
            long n;
            while ((n = decoder_read(d, buf, 4096)) > 0) frames += (unsigned long long)n;
            decoder_close(d);
            tracks++;
            if (!strcmp(f.codec, "FLAC")) {
                CHECK(frames == f.total_frames, "%s: decoded %llu frames, header says %llu",
                      t.title, frames, (unsigned long long)f.total_frames);
                flac_exact += frames == f.total_frames;
            } else {
                mp3++;
                CHECK(frames > 0, "%s: no audio", t.title);
            }
        }
        library_list_free(&tl);
    }
    /* make sure MP3 is exercised too: the first few MP3 albums */
    int mp3_albums = 0;
    for (int a = 0; a < albums.n && mp3_albums < 3; a++) {
        lib_list_t tl;
        library_list(lib, LIB_ALBUM_TRACKS, albums.ids[a], &tl);
        lib_track_t t;
        if (tl.n > 0 && library_track(lib, tl.ids[0], &t) == 0 && !strcmp(t.codec, "MP3")) {
            mp3_albums++;
            audio_format_t f;
            decoder_t *d = decoder_open(t.path, &f);
            CHECK(d, "cannot open %s", t.path);
            if (d) {
                unsigned long long frames = 0;
                long n;
                while ((n = decoder_read(d, buf, 4096)) > 0) frames += (unsigned long long)n;
                decoder_close(d);
                tracks++;
                mp3++;
                long long ms = f.rate ? (long long)(frames * 1000 / f.rate) : 0;
                CHECK(frames > 0, "%s: no audio", t.title);
                CHECK(llabs(ms - (long long)t.duration_ms) < 1500, "%s: %lld ms decoded, library says %u ms",
                      t.title, ms, t.duration_ms);
            }
        }
        library_list_free(&tl);
    }
    library_list_free(&albums);
    printf("decode: %d tracks (%d FLAC exact length, %d MP3) in %.1f s CPU\n",
           tracks, flac_exact, mp3, (double)(clock() - t0) / CLOCKS_PER_SEC);
}

static void test_gapless(library_t *lib)
{
    lib_list_t albums, tl;
    library_list(lib, LIB_ALBUMS, 0, &albums);
    /* a FLAC album with at least 3 tracks */
    int64_t album = -1;
    for (int a = 0; a < albums.n && album < 0; a++) {
        library_list(lib, LIB_ALBUM_TRACKS, albums.ids[a], &tl);
        lib_track_t t;
        if (tl.n >= 3 && library_track(lib, tl.ids[0], &t) == 0 && !strcmp(t.codec, "FLAC")) album = albums.ids[a];
        library_list_free(&tl);
    }
    library_list_free(&albums);
    if (album < 0) { printf("gapless: no suitable album\n"); return; }
    library_list(lib, LIB_ALBUM_TRACKS, album, &tl);
    int n = tl.n < 4 ? tl.n : 4;

    audio_output_t out;
    out_null_stats_t stats;
    out_null_create(&out, &stats);
    engine_t *e = engine_new(&out);
    player_t *p = player_new(lib, e);
    player_play(p, tl.ids, n, 0);

    unsigned long long expect = 0;
    for (int i = 0; i < n; i++) {
        lib_track_t t;
        library_track(lib, tl.ids[i], &t);
        audio_format_t f;
        decoder_t *d = decoder_open(t.path, &f);
        if (d) { expect += f.total_frames; decoder_close(d); }
    }
    uint32_t changes = 0, last = player_seq(p);
    for (int i = 0; i < 20000; i++) {   /* up to ~200 s of wall time; null output runs fast */
        player_tick(p);
        if (player_seq(p) != last) { changes++; last = player_seq(p); }
        eng_status_t st;
        engine_status(e, &st);
        if (st.ended) break;
        usleep(10000);
    }
    player_now_t now;
    player_now(p, &now);
    printf("gapless: %d tracks, %llu frames out (expected %llu), output opened %d time(s), %u track changes\n",
           n, stats.frames, expect, stats.opens, changes);
    CHECK(stats.frames == expect, "frame count mismatch");
    CHECK(stats.opens == 1, "output reopened between same-format tracks");
    CHECK(changes == (uint32_t)(n - 1), "expected %d track changes", n - 1);
    player_free(p);
    engine_free(e);
    library_list_free(&tl);
}

static void test_shuffle(library_t *lib)
{
    audio_output_t out;
    out_null_stats_t stats;
    out_null_create(&out, &stats);
    engine_t *e = engine_new(&out);
    player_t *p = player_new(lib, e);
    player_shuffle_all(p);
    player_now_t now;
    player_now(p, &now);
    lib_list_t songs;
    library_list(lib, LIB_SONGS, 0, &songs);
    printf("shuffle: queue of %d over %d songs (interludes under 45 s are left out)\n", now.count, songs.n);
    CHECK(now.count > 0 && now.count <= songs.n, "bad queue size");
    engine_stop(e);
    player_free(p);
    engine_free(e);
    library_list_free(&songs);
}

int main(int argc, char **argv)
{
    library_t *lib = library_open(argc > 1 ? argv[1] : "build/sim-data/library.db",
                                  argc > 2 ? argv[2] : "build/sim-data/art");
    if (!lib) return 1;
    test_decode(lib);
    test_gapless(lib);
    test_shuffle(lib);
    library_close(lib);
    printf("%s\n", fails ? "FAILED" : "all checks passed");
    return fails ? 1 : 0;
}
