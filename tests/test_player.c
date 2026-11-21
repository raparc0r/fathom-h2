/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * The player's queue, repeat modes, stop-after and saved queue, against a tiny
 * generated library: six 0.3 s FLAC files played through the real engine and a
 * silent real-time output.
 */
#include <FLAC/metadata.h>
#include <FLAC/stream_encoder.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "audio/engine.h"
#include "audio/output.h"
#include "library/library.h"
#include "library/playlist.h"
#include "player/player.h"
#include "tmpdir.h"

#define N 6
static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static int write_flac_rate(const char *path, int track, unsigned rate)
{
    FLAC__StreamEncoder *e = FLAC__stream_encoder_new();
    FLAC__stream_encoder_set_channels(e, 2);
    FLAC__stream_encoder_set_bits_per_sample(e, 16);
    FLAC__stream_encoder_set_sample_rate(e, rate);
    FLAC__StreamMetadata *vc = FLAC__metadata_object_new(FLAC__METADATA_TYPE_VORBIS_COMMENT);
    FLAC__StreamMetadata_VorbisComment_Entry en;
    char buf[64];
    /* guests (Artists): on track 6 in the artist tag, on track 5 in the title */
    snprintf(buf, sizeof buf, track == 5 ? "T%d (feat. Guest Two)" : "T%d", track);
    FLAC__metadata_object_vorbiscomment_entry_from_name_value_pair(&en, "TITLE", buf);
    FLAC__metadata_object_vorbiscomment_append_comment(vc, en, false);
    FLAC__metadata_object_vorbiscomment_entry_from_name_value_pair(&en, "ARTIST",
                                                                   track == 6 ? "Test Artist feat. Guest One" : "Test Artist");
    FLAC__metadata_object_vorbiscomment_append_comment(vc, en, false);
    FLAC__metadata_object_vorbiscomment_entry_from_name_value_pair(&en, "ALBUMARTIST", "Test Artist");
    FLAC__metadata_object_vorbiscomment_append_comment(vc, en, false);
    FLAC__metadata_object_vorbiscomment_entry_from_name_value_pair(&en, "ALBUM", "Test Album");
    FLAC__metadata_object_vorbiscomment_append_comment(vc, en, false);
    snprintf(buf, sizeof buf, "%d", track);
    FLAC__metadata_object_vorbiscomment_entry_from_name_value_pair(&en, "TRACKNUMBER", buf);
    FLAC__metadata_object_vorbiscomment_append_comment(vc, en, false);
    FLAC__stream_encoder_set_metadata(e, &vc, 1);
    if (FLAC__stream_encoder_init_file(e, path, NULL, NULL) != FLAC__STREAM_ENCODER_INIT_STATUS_OK) return -1;
    static FLAC__int32 pcm[2 * 13230];   /* 0.3 s of silence */
    FLAC__stream_encoder_process_interleaved(e, pcm, 13230);
    FLAC__stream_encoder_finish(e);
    FLAC__stream_encoder_delete(e);
    FLAC__metadata_object_delete(vc);
    return 0;
}

static int write_flac(const char *path, int track) { return write_flac_rate(path, track, 44100); }

/* A long silent FLAC with no tags (a podcast episode stand-in). */
static int write_flac_long(const char *path, int seconds)
{
    FLAC__StreamEncoder *e = FLAC__stream_encoder_new();
    FLAC__stream_encoder_set_channels(e, 2);
    FLAC__stream_encoder_set_bits_per_sample(e, 16);
    FLAC__stream_encoder_set_sample_rate(e, 44100);
    if (FLAC__stream_encoder_init_file(e, path, NULL, NULL) != FLAC__STREAM_ENCODER_INIT_STATUS_OK) return -1;
    static FLAC__int32 pcm[2 * 44100];
    for (int s = 0; s < seconds; s++) FLAC__stream_encoder_process_interleaved(e, pcm, 44100);
    FLAC__stream_encoder_finish(e);
    FLAC__stream_encoder_delete(e);
    return 0;
}

static uint32_t now_pos(player_t *p)
{
    player_now_t n;
    player_now(p, &n);
    return n.pos_ms;
}

static void sleep_ms(int ms)
{
    struct timespec t = {ms / 1000, (long)(ms % 1000) * 1000000};
    nanosleep(&t, NULL);
}

/* Tick the player until `cond` holds or `ms` pass. The checks here run in
 * real time, on a computer that may be busy (or run the test at a low
 * priority): waits end as soon as their condition holds, so they're long
 * (SLOW), and a check never depends on seeing a 0.3 s track as it plays. */
#define SLOW 15000
#define WAIT_FOR(p, cond, ms) do { \
    for (int w_ = 0; w_ < (ms) / 10 && !(cond); w_++) { player_tick(p); sleep_ms(10); } \
    player_tick(p); \
} while (0)
/* The same without a player: until `cond` holds or SLOW passes */
#define WAIT_UNTIL(cond) do { for (int w_ = 0; w_ < SLOW / 10 && !(cond); w_++) sleep_ms(10); } while (0)

static uint64_t mono_ms_now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}

static int64_t cur_id(player_t *p)
{
    player_now_t n;
    player_now(p, &n);
    return n.track.id;
}

static int playing(player_t *p)
{
    player_now_t n;
    player_now(p, &n);
    return n.playing;
}

/* Where the playing track is in t[0..n-1]: -1 if it isn't */
static int idx_of(player_t *p, const int64_t *t, int n)
{
    int64_t id = cur_id(p);
    for (int i = 0; i < n; i++)
        if (t[i] == id) return i;
    return -1;
}

/* A short silent FLAC at a given depth and rate (engine format tests). */
static int write_flac_fmt(const char *path, int bits, unsigned rate)
{
    FLAC__StreamEncoder *e = FLAC__stream_encoder_new();
    FLAC__stream_encoder_set_channels(e, 2);
    FLAC__stream_encoder_set_bits_per_sample(e, (unsigned)bits);
    FLAC__stream_encoder_set_sample_rate(e, rate);
    if (FLAC__stream_encoder_init_file(e, path, NULL, NULL) != FLAC__STREAM_ENCODER_INIT_STATUS_OK) return -1;
    static FLAC__int32 pcm[2 * 4410];
    FLAC__stream_encoder_process_interleaved(e, pcm, 4410);
    FLAC__stream_encoder_finish(e);
    FLAC__stream_encoder_delete(e);
    return 0;
}

/* The output is reopened when a deeper track follows at the same rate (a 24-bit
 * track must not go out cut to 16 bits), and kept (gapless) the other way. */
static void test_engine_depth(const char *dir)
{
    char a[300], b[300], c[300];
    snprintf(a, sizeof a, "%s/f16.flac", dir);
    snprintf(b, sizeof b, "%s/f24.flac", dir);
    snprintf(c, sizeof c, "%s/g16.flac", dir);
    CHECK(write_flac_fmt(a, 16, 44100) == 0 && write_flac_fmt(b, 24, 44100) == 0 && write_flac_fmt(c, 16, 44100) == 0);
    audio_output_t out;
    out_null_stats_t st = {0};
    out_null_create(&out, &st);
    engine_t *e = engine_new(&out);
    engine_play(e, a, 1);
    engine_queue_next(e, b, 2);
    for (int i = 0; i < SLOW / 10; i++) {
        eng_status_t s;
        engine_status(e, &s);
        if (s.track_id == 2 && !s.next_queued) break;
        sleep_ms(10);
    }
    CHECK(st.opens == 2 && st.fmt.bits == 24);   /* 16 -> 24: reopened for 24 bits */
    engine_queue_next(e, c, 3);
    for (int i = 0; i < SLOW / 10; i++) {
        eng_status_t s;
        engine_status(e, &s);
        if (s.ended || (s.track_id == 3 && s.state == ENG_STOPPED)) break;
        sleep_ms(10);
    }
    CHECK(st.opens == 2);                         /* 24 -> 16: exact on the open output, kept */
    engine_free(e);
    unlink(a); unlink(b); unlink(c);
}

/* The EQ: off, a 16-bit track opens a 16-bit output (bit-perfect); on, the
 * output opens at 24 bits, also when it comes on mid-track; off again, the
 * open 24-bit output is kept (16-bit samples are exact on it). */
static void test_engine_eq(const char *dir)
{
    char a[300];
    snprintf(a, sizeof a, "%s/eq16.flac", dir);
    CHECK(write_flac_fmt(a, 16, 44100) == 0);
    audio_output_t out;
    out_null_stats_t st = {0};
    out_null_create(&out, &st);
    engine_t *e = engine_new(&out);
    engine_play(e, a, 1);
    engine_pause(e, 1);                           /* starts paused: it mustn't end first (0.1 s) */
    WAIT_UNTIL(st.opens >= 1);
    CHECK(st.opens == 1 && st.fmt.bits == 16);
    eq_profile_t p = {.preamp_db = -3, .n = 1, .band = {{EQ_PEAK, 1000, 3, 1}}};
    engine_set_eq(e, &p);
    engine_pause(e, 0);
    WAIT_UNTIL(st.opens >= 2);
    CHECK(st.opens == 2 && st.fmt.bits == 24);   /* on mid-track: reopened deeper */
    eng_status_t s;
    engine_status(e, &s);
    CHECK(s.eq == 1);
    engine_play(e, a, 2);                         /* a new track with the EQ on: 24 bits */
    WAIT_UNTIL((engine_status(e, &s), s.track_id == 2));
    CHECK(st.fmt.bits == 24);
    engine_set_eq(e, NULL);
    engine_play(e, a, 3);
    WAIT_UNTIL((engine_status(e, &s), s.track_id == 3));
    CHECK(s.eq == 0 && st.fmt.bits == 24);        /* off: the deeper output carries 16 bits exactly */
    engine_free(e);
    /* volume levelling (a gain) on a 16-bit track: 24 bits too; at 1.0, 16 */
    out_null_stats_t st2 = {0};
    out_null_create(&out, &st2);
    e = engine_new(&out);
    engine_play_gain(e, a, 1, 0.5f);
    engine_pause(e, 1);
    WAIT_UNTIL(st2.opens >= 1);
    CHECK(st2.fmt.bits == 24);
    engine_free(e);
    out_null_stats_t st3 = {0};
    out_null_create(&out, &st3);
    e = engine_new(&out);
    engine_play_gain(e, a, 1, 1.0f);
    engine_pause(e, 1);
    WAIT_UNTIL(st3.opens >= 1);
    CHECK(st3.fmt.bits == 16);
    engine_free(e);
    unlink(a);
}

/* An output that opens 44.1 kHz only (as Bluetooth can't play 22 kHz). */
static audio_output_t g_inner;
static int g_opens;
static int only44_open(void *ctx, const audio_format_t *f)
{
    g_opens++;
    return f->rate == 44100 ? g_inner.open(ctx, f) : -1;
}

/* Tracks the output can't take are skipped, as they start or gapless; a
 * queue of nothing but those stops after one pass, even with repeat on. */
static void test_unplayable(const char *dir)
{
    char music[300], path[400], db[300], art[300];
    snprintf(music, sizeof music, "%s/rates", dir);
    snprintf(db, sizeof db, "%s/rates.db", dir);
    snprintf(art, sizeof art, "%s/rates-art", dir);
    mkdir(music, 0755);
    static const unsigned rates[] = {44100, 22050, 22050, 44100};   /* tracks 1..4 */
    for (int i = 0; i < 4; i++) {
        snprintf(path, sizeof path, "%s/%02d.flac", music, i + 1);
        CHECK(write_flac_rate(path, i + 1, rates[i]) == 0);
    }
    library_t *lib = library_open(db, art);
    CHECK(lib && library_scan(lib, music, NULL, NULL) == 4);
    lib_list_t albums, tr;
    CHECK(library_list(lib, LIB_ALBUMS, 0, &albums) == 1);
    CHECK(library_list(lib, LIB_ALBUM_TRACKS, albums.ids[0], &tr) == 4);
    out_null_stats_t ns;
    out_null_create(&g_inner, &ns);
    audio_output_t out = g_inner;
    out.open = only44_open;
    engine_t *eng = engine_new(&out);
    player_t *p = player_new(lib, eng);

    player_play(p, tr.ids, 4, 0);                 /* 1 plays; 2 and 3 can't; on to 4 */
    WAIT_FOR(p, cur_id(p) == tr.ids[3] && !playing(p), SLOW);
    CHECK(cur_id(p) == tr.ids[3] && ns.frames == 2 * 13230);

    player_set_repeat(p, REPEAT_ALL);             /* nothing but tracks it can't play */
    player_play(p, &tr.ids[1], 2, 0);
    WAIT_FOR(p, !playing(p) && g_opens >= 2, SLOW);   /* (tried each, then gave up) */
    WAIT_FOR(p, 0, 300);
    int opens = g_opens;
    WAIT_FOR(p, 0, 300);
    CHECK(!playing(p) && g_opens == opens && opens < 10);

    player_free(p);
    engine_free(eng);
    library_list_free(&tr);
    library_list_free(&albums);
    library_close(lib);
}

int main(void)
{
    char dir[200];
    test_tmp(dir, sizeof dir, "fathom-test-player");
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; }
    char path[256], music[300];
    snprintf(music, sizeof music, "%s/music", dir);
    mkdir(music, 0755);
    for (int i = 1; i <= N; i++) {
        snprintf(path, sizeof path, "%s/%02d.flac", music, i);
        if (write_flac(path, i)) { fprintf(stderr, "cannot write %s\n", path); return 1; }
    }
    test_engine_depth(dir);
    test_engine_eq(dir);
    test_unplayable(dir);
    char db[300], art[300], queue[300];
    snprintf(db, sizeof db, "%s/lib.db", dir);
    snprintf(art, sizeof art, "%s/art", dir);
    snprintf(queue, sizeof queue, "%s/queue.txt", dir);
    library_t *lib = library_open(db, art);
    CHECK(lib && library_scan(lib, music, NULL, NULL) == N);
    lib_list_t albums, tr;
    CHECK(library_list(lib, LIB_ALBUMS, 0, &albums) == 1);
    CHECK(library_list(lib, LIB_ALBUM_TRACKS, albums.ids[0], &tr) == N);
    int64_t *t = tr.ids;   /* t[0..5] = tracks 1..6 */

    /* row text: track lists show the length, not raw milliseconds; albums their name */
    char rm[256], rmeta[32];
    CHECK(library_row(lib, LIB_ALBUM_TRACKS, t[0], rm, sizeof rm, rmeta, sizeof rmeta) == 0 &&
          !strcmp(rm, "T1") && !strcmp(rmeta, "0:00"));
    CHECK(library_row(lib, LIB_SONGS, t[1], rm, sizeof rm, rmeta, sizeof rmeta) == 0 && !strcmp(rmeta, "0:00"));
    CHECK(library_row(lib, LIB_ALBUMS, albums.ids[0], rm, sizeof rm, rmeta, sizeof rmeta) == 0 &&
          !strcmp(rm, "Test Album"));

    /* Album artists: one; Artists: everyone credited, guests too, by name */
    lib_list_t aa, ar, ca, ct;
    CHECK(library_list(lib, LIB_ALBUM_ARTISTS, 0, &aa) == 1);
    CHECK(library_list(lib, LIB_ARTISTS, 0, &ar) == 3);
    char an[3][64] = {"", "", ""};
    for (int i = 0; i < ar.n && i < 3; i++) library_artist_name(lib, ar.ids[i], an[i], sizeof an[i]);
    CHECK(!strcmp(an[0], "Guest One") && !strcmp(an[1], "Guest Two") && !strcmp(an[2], "Test Artist"));
    if (ar.n == 3) {
        int na, nc;
        library_artist_counts(lib, ar.ids[0], &na, &nc);
        CHECK(na == 0 && nc == 1);
        CHECK(library_list(lib, LIB_CREDIT_ALBUMS, ar.ids[0], &ca) == 1 && ca.ids[0] == albums.ids[0]);
        CHECK(library_list_credit_tracks(lib, ar.ids[0], albums.ids[0], &ct) == 1 && ct.ids[0] == t[5]);
        library_list_free(&ct);
        CHECK(library_list_credit_tracks(lib, ar.ids[1], albums.ids[0], &ct) == 1 && ct.ids[0] == t[4]);
        library_list_free(&ct);
        CHECK(library_list_credit_tracks(lib, ar.ids[2], albums.ids[0], &ct) == N);   /* the main artist: all */
        library_list_free(&ct);
        library_list_free(&ca);
        CHECK(library_row(lib, LIB_ARTISTS, ar.ids[2], rm, sizeof rm, rmeta, sizeof rmeta) == 0 &&
              !strcmp(rm, "Test Artist") && !strcmp(rmeta, "1"));
    }
    library_list_free(&aa);
    library_list_free(&ar);

    /* rescans: nothing changed (no rebuild), a file added, then removed again */
    CHECK(library_scan(lib, music, NULL, NULL) == N && library_scan_changes(lib) == 0);
    CHECK(library_list(lib, LIB_ARTISTS, 0, &ar) == 3);
    library_list_free(&ar);
    snprintf(path, sizeof path, "%s/%02d.flac", music, N + 1);
    CHECK(write_flac(path, N + 1) == 0);
    CHECK(library_scan(lib, music, NULL, NULL) == N + 1 && library_scan_changes(lib) == 1);
    unlink(path);
    CHECK(library_scan(lib, music, NULL, NULL) == N && library_scan_changes(lib) == 1);
    /* the music folder gone (renamed, card away): the library is kept */
    snprintf(path, sizeof path, "%s-renamed", music);
    CHECK(library_scan(lib, path, NULL, NULL) == -1);
    CHECK(library_list(lib, LIB_SONGS, 0, &ar) == N);
    library_list_free(&ar);
    CHECK(library_list(lib, LIB_ALBUMS, 0, &aa) == 1 && aa.ids[0] == albums.ids[0]);
    library_list_free(&aa);

    /* volume levelling: track gain, clipping held off, album gain from its tracks */
    CHECK(library_rg_gain(lib, t[0], 0) == 1.0f);                          /* not known yet */
    lib_rg_t rg = {2, -6.0f, 0.5f, 0, 0, 0};
    library_rg_store(lib, t[0], &rg);
    CHECK(fabsf(library_rg_gain(lib, t[0], 0) - 0.5012f) < 0.001f);         /* -6 dB */
    CHECK(fabsf(library_rg_gain(lib, t[0], 1) - 0.5012f) < 0.001f);         /* album not complete: the track's */
    rg.track_gain = 10.0f;
    library_rg_store(lib, t[0], &rg);
    CHECK(fabsf(library_rg_gain(lib, t[0], 0) - 2.0f) < 0.001f);            /* +10 dB, held to 1/peak */
    for (int i = 0; i < N; i++) {
        lib_rg_t r = {2, i == 0 ? -6.0f : -12.0f, 0.25f, 0, 0, 0};
        library_rg_store(lib, t[i], &r);
    }
    /* one track at -12 LUFS (gain -6), five at -6 LUFS (gain -12), all the same length:
     * the album's loudness is their mean energy, its gain -18 minus that */
    double album_lufs = 10.0 * log10((pow(10, -12.0 / 10) + 5 * pow(10, -6.0 / 10)) / 6.0);
    double want_db = -18.0 - album_lufs;
    CHECK(fabs(20 * log10(library_rg_gain(lib, t[0], 1)) - want_db) < 0.01);
    int done, total;
    library_rg_progress(lib, &done, &total);
    CHECK(done == N && total == N);
    rg = (lib_rg_t){1, -3.0f, 0.5f, 1, -8.0f, 0.9f};                          /* tagged album gain wins */
    library_rg_store(lib, t[1], &rg);
    CHECK(fabsf(20 * log10f(library_rg_gain(lib, t[1], 1)) + 8.0f) < 0.01f);

    /* search: words must start words; case and accents ignored; title, artist, album */
    lib_search_t sr;
    CHECK(library_search(lib, "tes alb", &sr) > 0 && sr.albums.n == 1 && sr.artists.n == 0 && sr.songs.n == N);
    library_search_free(&sr);
    CHECK(library_search(lib, "TEST art", &sr) > 0 && sr.artists.n == 1 && sr.albums.n == 1);
    library_search_free(&sr);
    CHECK(library_search(lib, "T3", &sr) == 1 && sr.songs.n == 1 && sr.songs.ids[0] == t[2]);
    library_search_free(&sr);
    CHECK(library_search(lib, "guest", &sr) > 0 && sr.artists.n == 2);   /* guests are found too */
    library_search_free(&sr);
    CHECK(library_search(lib, "est", &sr) == 0);              /* not the start of a word */
    library_search_free(&sr);
    CHECK(library_search(lib, "", &sr) == 0);
    library_search_free(&sr);

    audio_output_t out;
    out_clock_create(&out);
    engine_t *eng = engine_new(&out);
    player_t *p = player_new(lib, eng);

    /* play next / add to queue / move / remove / clear (order logic) */
    player_play(p, t, N, 0);
    CHECK(cur_id(p) == t[0] && player_queue_len(p) == 5);
    player_play_next(p, &t[5], 1);                      /* 1 | 6 2 3 4 5 6 */
    CHECK(player_queue_at(p, 0) == t[5] && player_queue_len(p) == 6);
    int64_t two[] = {t[4], t[3]};
    player_add_to_queue(p, two, 2);                     /* 1 | 6 5 4 2 3 4 5 6 */
    CHECK(player_queue_at(p, 1) == t[4] && player_queue_at(p, 2) == t[3] && player_queue_at(p, 3) == t[1]);
    player_add_to_queue(p, &t[2], 1);                   /* after the ones already added */
    CHECK(player_queue_at(p, 3) == t[2] && player_queue_at(p, 4) == t[1]);
    player_play_next(p, &t[0], 1);                      /* in front of everything */
    CHECK(player_queue_at(p, 0) == t[0] && player_queue_at(p, 1) == t[5]);
    player_queue_move(p, 0, +1);
    CHECK(player_queue_at(p, 0) == t[5] && player_queue_at(p, 1) == t[0]);
    player_queue_remove(p, 1);
    CHECK(player_queue_at(p, 1) == t[4]);
    player_queue_clear(p);
    CHECK(player_queue_len(p) == 0);

    /* stop after this track: stops, and the next track is cued */
    player_play(p, t, N, 0);
    player_set_stop_after(p, 1);
    WAIT_FOR(p, !playing(p) && cur_id(p) == t[1], SLOW);
    CHECK(!playing(p) && cur_id(p) == t[1] && !player_stop_after(p));

    /* gapless: the player follows the engine to the next track */
    player_play(p, t, N, 2);
    WAIT_FOR(p, idx_of(p, t, N) >= 3, SLOW);          /* (on from the third: past it, if slow to look) */
    CHECK(idx_of(p, t, N) >= 3);

    /* repeat one: the same track again */
    player_set_repeat(p, REPEAT_ONE);
    player_play(p, t, N, 4);
    uint32_t s0 = player_seq(p);
    WAIT_FOR(p, player_seq(p) != s0, SLOW);
    CHECK(player_seq(p) != s0 && cur_id(p) == t[4] && playing(p));

    /* repeat all: the last track wraps to the first */
    player_set_repeat(p, REPEAT_ALL);
    player_play(p, t, N, 5);
    WAIT_FOR(p, idx_of(p, t, N) >= 0 && idx_of(p, t, N) < 5, SLOW);   /* (wrapped: the first, or past it) */
    CHECK(idx_of(p, t, N) >= 0 && idx_of(p, t, N) < 5 && player_repeat(p) == REPEAT_ALL);

    /* repeat off: the last track ends and playback stops */
    player_set_repeat(p, REPEAT_OFF);
    player_play(p, t, N, 5);
    WAIT_FOR(p, !playing(p), SLOW);
    sleep_ms(100);
    player_tick(p);
    CHECK(!playing(p) && cur_id(p) == t[5]);

    /* a podcast episode: from where it was left, the skip keys, its end; the queue kept */
    {
        char ep_path[300];
        snprintf(ep_path, sizeof ep_path, "%s/episode.flac", dir);
        CHECK(write_flac_long(ep_path, 90) == 0);
        player_set_repeat(p, REPEAT_OFF);
        player_play(p, t, N, 2);
        WAIT_FOR(p, playing(p), SLOW);
        player_episode_t ep = {0};
        snprintf(ep.show, sizeof ep.show, "The Show");
        snprintf(ep.title, sizeof ep.title, "Episode 7");
        snprintf(ep.path, sizeof ep.path, "%s", ep_path);
        ep.start_ms = 40000;
        player_episode_play(p, &ep);
        player_now_t n;
        uint64_t t_start = mono_ms_now();
        WAIT_FOR(p, (player_now(p, &n), n.pos_ms >= 40000 && n.len_ms > 0 && n.playing), SLOW);
        player_now(p, &n);
        CHECK(n.podcast && !n.podcast_stream && !strcmp(n.track.title, "Episode 7") && !strcmp(n.track.artist, "The Show"));
        /* from 40 s on, by no more than the time that's passed (and a little) */
        uint32_t late = (uint32_t)(mono_ms_now() - t_start) + 500;
        if (!(n.pos_ms >= 40000 && n.pos_ms < 40000 + late && n.len_ms >= 89000 && n.len_ms <= 91000))
            fprintf(stderr, "episode pos %u len %u (%u ms on)\n", n.pos_ms, n.len_ms, late);
        CHECK(n.pos_ms >= 40000 && n.pos_ms < 40000 + late && n.len_ms >= 89000 && n.len_ms <= 91000);
        /* the skip keys: 30 s on, 15 s back, from where it was (and what's played since;
         * 100 ms either way: the player's reading of where it was, a moment apart) */
        uint32_t before = now_pos(p);
        uint64_t t_skip = mono_ms_now();
        player_next(p);                                   /* +30 s */
        WAIT_FOR(p, now_pos(p) + 100 >= before + 30000, SLOW);
        uint32_t after = now_pos(p), since = (uint32_t)(mono_ms_now() - t_skip) + 500;
        if (!(after + 100 >= before + 30000 && after < before + 30000 + since)) fprintf(stderr, "next: %u -> %u\n", before, after);
        CHECK(after + 100 >= before + 30000 && after < before + 30000 + since);
        before = after;
        t_skip = mono_ms_now();
        player_prev(p);                                   /* -15 s */
        WAIT_FOR(p, now_pos(p) < before, SLOW);
        after = now_pos(p);
        since = (uint32_t)(mono_ms_now() - t_skip) + 500;
        if (!(after + 15100 >= before && after + 15000 < before + since)) fprintf(stderr, "prev: %u -> %u\n", before, after);
        CHECK(after + 15100 >= before && after + 15000 < before + since);
        uint32_t pos = 0, len = 0;
        CHECK(player_episode(p, &pos, &len) != NULL && pos + 15100 >= before && len >= 89000);
        player_toggle_pause(p);                           /* paused: it stays */
        WAIT_FOR(p, !playing(p), SLOW);
        CHECK(!playing(p) && player_episode(p, NULL, NULL) && !player_episode_ended(p));
        /* to its end: said once, then cued from the start */
        player_seek_by(p, 31000);
        player_toggle_pause(p);
        WAIT_FOR(p, player_episode(p, NULL, NULL) && !playing(p) && now_pos(p) == 0, SLOW);
        CHECK(player_episode_ended(p) && !player_episode_ended(p));
        CHECK(player_episode(p, &pos, NULL) && pos == 0);
        /* music again: the episode is left, the queue cued where it was */
        player_toggle_pause(p);                           /* (first: it plays again, from the start) */
        WAIT_FOR(p, playing(p), SLOW);
        CHECK(playing(p) && now_pos(p) < 5000);
        player_play(p, t, N, 1);
        player_now(p, &n);
        CHECK(!n.podcast && cur_id(p) == t[1] && !player_episode(p, NULL, NULL));
        /* faster: about 3 s of it in 2 s; music as it is again after */
        player_set_speed(p, 150);
        ep.start_ms = 10000;
        player_episode_play(p, &ep);
        WAIT_FOR(p, (player_now(p, &n), n.playing && n.pos_ms >= 10000), SLOW);
        /* about 1.5 s of it for each second that passes: measured up to three
         * times (a busy computer can hold the playback thread up for a while) */
        int fast = 0;
        for (int a = 0; a < 3 && !fast; a++) {
            player_tick(p);
            uint32_t p0 = now_pos(p);
            uint64_t w0 = mono_ms_now();
            sleep_ms(1000);
            player_tick(p);
            double r = (double)(now_pos(p) - p0) / (double)(mono_ms_now() - w0);
            fast = r >= 1.3 && r <= 1.7;
            if (!fast) fprintf(stderr, "at 1.5x: %.2fx\n", r);
        }
        CHECK(fast);
        CHECK(player_speed(p) == 150);
        player_play(p, t, N, 1);                          /* music: not sped up */
        WAIT_FOR(p, playing(p), SLOW);
        int normal = 0;                                   /* (a slow computer only makes it slower) */
        for (int a = 0; a < 5 && !normal; a++) {
            player_tick(p);
            int64_t id0 = cur_id(p);
            uint32_t p0 = now_pos(p);
            uint64_t w0 = mono_ms_now();
            sleep_ms(120);
            player_tick(p);
            if (cur_id(p) != id0 || !playing(p)) continue;   /* (the 0.3 s track ended meanwhile) */
            double r = (double)(now_pos(p) - p0) / (double)(mono_ms_now() - w0);
            normal = r <= 1.2;
            if (!normal) fprintf(stderr, "music after an episode at 1.5x: %.2fx\n", r);
        }
        CHECK(normal);
        player_set_speed(p, 100);
        /* the radio leaves an episode too, and an episode the radio */
        player_episode_play(p, &ep);
        radio_station_t rs = {"A station", "http://127.0.0.1:9/none", ""};
        player_radio_play(p, &rs, 1, 0);
        CHECK(!player_episode(p, NULL, NULL) && player_radio(p));
        player_episode_play(p, &ep);
        CHECK(player_episode(p, NULL, NULL) && !player_radio(p));
        player_play(p, t, N, 0);
    }

    /* very short tracks and no tick from the UI meanwhile (busy): the engine
     * goes on through all of them by itself, gapless, and stops once, at the
     * end (with only the next one lined up, it stopped after that) */
    {
        player_play(p, t, N, 0);
        eng_status_t before, after;
        engine_status(eng, &before);
        WAIT_UNTIL((engine_status(eng, &after), after.stop_seq != before.stop_seq));   /* six 0.3 s tracks */
        sleep_ms(300);                                    /* (and nothing after) */
        engine_status(eng, &after);
        CHECK(after.track_id == t[5] && after.stop_seq == before.stop_seq + 1 && after.ended && !after.error);
        player_tick(p);
        CHECK(cur_id(p) == t[5]);

        /* a skip before the tick has caught up: past the track that plays
         * now, not the one the player last saw */
        player_play(p, t, N, 0);
        WAIT_UNTIL((engine_status(eng, &after), after.track_id == t[1] || after.track_id == t[2] || after.track_id == t[3]));
        engine_status(eng, &after);
        int k = 0;
        while (k < N - 1 && t[k] != after.track_id) k++;
        player_next(p);
        int nk = idx_of(p, t, N);                         /* (the engine may have gone on as it was asked) */
        CHECK(k >= 1 && k < N - 1 && (nk == k + 1 || (k + 2 < N && nk == k + 2)));
        WAIT_FOR(p, !playing(p), SLOW);
    }

    /* a damaged next track: skipped, not the end of the queue */
    {
        char bad[320];
        snprintf(bad, sizeof bad, "%s/04.flac", music);
        FILE *f = fopen(bad, "wb");
        CHECK(f && fwrite("not a flac file", 1, 15, f) == 15);
        if (f) fclose(f);
        player_set_repeat(p, REPEAT_OFF);
        player_play(p, t, N, 2);                          /* 03, then 04 (damaged), 05 */
        WAIT_FOR(p, idx_of(p, t, N) >= 4, SLOW);          /* (05, or 06 after it) */
        CHECK(idx_of(p, t, N) >= 4);
        CHECK(write_flac(bad, 4) == 0);                   /* (as it was, for what follows) */
    }

    /* saved queue: restored paused on the same track, with the same queue */
    player_play(p, t, N, 1);
    player_play_next(p, &t[5], 1);
    player_pause(p, 1);
    CHECK(player_save(p, queue) == 0);
    player_t *q = player_new(lib, eng);
    CHECK(player_restore(q, queue) == 0);
    CHECK(cur_id(q) == t[1]);
    CHECK(!playing(q));
    CHECK(player_queue_len(q) == player_queue_len(p));
    if (player_queue_len(q) != player_queue_len(p))
        fprintf(stderr, "queue: restored %d, saved %d\n", player_queue_len(q), player_queue_len(p));
    CHECK(player_queue_at(q, 0) == t[5]);
    player_free(q);

    /* your own playlists: written relative to their folder, read back, kept by a rescan */
    char pldir[300], plfile[400];
    snprintf(pldir, sizeof pldir, "%s/playlists", dir);
    library_set_playlist_dir(lib, pldir);
    int64_t pl = library_playlist_create(lib, "My List");
    CHECK(pl > 0 && library_playlist_writable(lib, pl));
    int64_t three[] = {t[2], t[0], t[4]};
    CHECK(library_playlist_add(lib, pl, three, 3) == 0);
    snprintf(plfile, sizeof plfile, "%s/My List.m3u8", pldir);
    m3u_t m;
    CHECK(m3u_read(plfile, &m) == 0 && m.n == 3 && !strcmp(m.name, "My List"));
    lib_track_t lt;
    if (m.n == 3 && library_track(lib, t[2], &lt) == 0) CHECK(!strcmp(m.paths[0], lt.path));
    CHECK(m.n == 3 && m.infos[0] && strstr(m.infos[0], "T3"));
    m3u_free(&m);
    CHECK(library_scan(lib, music, NULL, NULL) == N);       /* the rescan keeps it, editable */
    lib_list_t pls, items;
    CHECK(library_list(lib, LIB_PLAYLISTS, 0, &pls) == 1);
    CHECK(pls.n == 1 && library_playlist_writable(lib, pls.ids[0]));
    CHECK(pls.n == 1 && library_list(lib, LIB_PLAYLIST_TRACKS, pls.ids[0], &items) == 3);
    library_list_free(&items);
    if (pls.n == 1) {
        pl = pls.ids[0];
        CHECK(library_playlist_move(lib, pl, 0, +1) == 0);    /* T1 T3 T5 */
        CHECK(library_playlist_remove(lib, pl, 2) == 0);      /* T1 T3 */
        CHECK(library_list(lib, LIB_PLAYLIST_TRACKS, pl, &items) == 2 && items.ids[0] == t[0] && items.ids[1] == t[2]);
        library_list_free(&items);
        char renamed[400];
        snprintf(renamed, sizeof renamed, "%s/Late Night.m3u8", pldir);
        CHECK(library_playlist_rename(lib, pl, "Late Night") == 0 && access(renamed, F_OK) == 0 &&
              access(plfile, F_OK) != 0);
        CHECK(m3u_read(renamed, &m) == 0 && !strcmp(m.name, "Late Night") && m.n == 2);
        m3u_free(&m);
        char nm[64];
        CHECK(library_playlist_name(lib, pl, nm, sizeof nm) == 0 && !strcmp(nm, "Late Night"));
        CHECK(library_playlist_delete(lib, pl) == 0 && access(renamed, F_OK) != 0);
    }
    library_list_free(&pls);

    player_free(p);
    engine_free(eng);
    library_list_free(&tr);
    library_list_free(&albums);
    library_close(lib);
    char cmd[400];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    if (system(cmd) != 0) fprintf(stderr, "could not remove %s\n", dir);

    if (failures) fprintf(stderr, "%d failure(s)\n", failures);
    else printf("player: all checks passed\n");
    return failures != 0;
}
