/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/*
 * The engine against outputs that behave as the H2's do: one whose write waits
 * while it's paused (ALSA: a paused USB DAC takes no more), one that can't open
 * some formats (Bluetooth: 22 kHz; 5.1), one that drops what's queued when it
 * pauses (Bluetooth ends the stream). Generated FLAC files; nothing is heard.
 */
#include <FLAC/stream_encoder.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "audio/crossfeed.h"
#include "audio/engine.h"
#include "audio/output.h"
#include "tmpdir.h"

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static void sleep_ms(int ms)
{
    struct timespec t = {ms / 1000, (long)(ms % 1000) * 1000000};
    nanosleep(&t, NULL);
}

/* A stereo 16-bit FLAC whose samples say where they are: frame i holds
 * i & 0x7fff on the left and i >> 15 on the right. */
static int write_ramp(const char *path, unsigned rate, unsigned frames)
{
    FLAC__StreamEncoder *e = FLAC__stream_encoder_new();
    FLAC__stream_encoder_set_channels(e, 2);
    FLAC__stream_encoder_set_bits_per_sample(e, 16);
    FLAC__stream_encoder_set_sample_rate(e, rate);
    if (FLAC__stream_encoder_init_file(e, path, NULL, NULL) != FLAC__STREAM_ENCODER_INIT_STATUS_OK) return -1;
    static FLAC__int32 pcm[2 * 4096];
    for (unsigned at = 0; at < frames;) {
        unsigned n = frames - at < 4096 ? frames - at : 4096;
        for (unsigned i = 0; i < n; i++) {
            pcm[2 * i] = (FLAC__int32)((at + i) & 0x7fff);
            pcm[2 * i + 1] = (FLAC__int32)((at + i) >> 15);
        }
        FLAC__stream_encoder_process_interleaved(e, pcm, n);
        at += n;
    }
    FLAC__stream_encoder_finish(e);
    FLAC__stream_encoder_delete(e);
    return 0;
}

/* A mono 16-bit FLAC: frame i holds i & 0x7fff. */
static int write_mono(const char *path, unsigned rate, unsigned frames)
{
    FLAC__StreamEncoder *e = FLAC__stream_encoder_new();
    FLAC__stream_encoder_set_channels(e, 1);
    FLAC__stream_encoder_set_bits_per_sample(e, 16);
    FLAC__stream_encoder_set_sample_rate(e, rate);
    if (FLAC__stream_encoder_init_file(e, path, NULL, NULL) != FLAC__STREAM_ENCODER_INIT_STATUS_OK) return -1;
    static FLAC__int32 pcm[4096];
    for (unsigned at = 0; at < frames;) {
        unsigned n = frames - at < 4096 ? frames - at : 4096;
        for (unsigned i = 0; i < n; i++) pcm[i] = (FLAC__int32)((at + i) & 0x7fff);
        FLAC__stream_encoder_process_interleaved(e, pcm, n);
        at += n;
    }
    FLAC__stream_encoder_finish(e);
    FLAC__stream_encoder_delete(e);
    return 0;
}

/* A mono file is given as two equal channels, sample for sample: the H2's DAC
 * takes two channels only, and a mono song was "This output can't play". */
static void test_mono(const char *dir)
{
    char a[512];
    snprintf(a, sizeof a, "%s/mono.flac", dir);
    CHECK(write_mono(a, 44100, 10000) == 0);
    audio_format_t f;
    decoder_t *d = decoder_open(a, &f);
    CHECK(d && f.channels == 2 && f.rate == 44100 && f.bits == 16 && f.total_frames == 10000);
    static int32_t buf[2 * 3000];
    long n, at = 0;
    int same = 1;
    while (d && (n = decoder_read(d, buf, 3000)) > 0) {
        for (long i = 0; i < n; i++)
            same &= buf[2 * i] == buf[2 * i + 1] && buf[2 * i] == (int32_t)(((at + i) & 0x7fff) << 16);
        at += n;
    }
    CHECK(at == 10000 && same);
    CHECK(d && decoder_seek(d, 5000) == 0 && decoder_read(d, buf, 10) == 10 &&
          buf[0] == (int32_t)(5000 << 16) && buf[1] == buf[0] && buf[19] == (int32_t)(5009 << 16));
    if (d) decoder_close(d);
}

static eng_status_t status(engine_t *e)
{
    eng_status_t s;
    engine_status(e, &s);
    return s;
}

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + t.tv_nsec / 1e9;
}

/* The track frame a written (left-justified) sample pair came from. */
static long frame_of(const int32_t *s) { return (long)(s[0] >> 16) | (long)(s[1] >> 16) << 15; }

#define WAIT(cond, ms) do { for (int w_ = 0; w_ < (ms) / 5 && !(cond); w_++) sleep_ms(5); } while (0)

/* ---- an output like ALSA's: a write waits while it's paused; a flush (DROP)
 * unpauses it, and a write waiting meanwhile fails (EBADFD) ---- */

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int paused, gate, in_write;   /* gate: the test holds writes up */
    int can_cut;                  /* its flush may cut a write short (as ALSA's) */
    int late;                     /* a write waiting wakes only after the flush's PREPARE:
                                     its chunk goes into the fresh buffer (ALSA can) */
    unsigned flushes;
    long queued;                  /* frames written since the last flush (what would play) */
    unsigned long long frames;
    unsigned only_rate;           /* opens only this rate (0: any) */
    unsigned max_bits;            /* opens only up to this depth (0: any) */
    int gone;                     /* unplugged: open() says so */
    int opens;
    int watch;                    /* from al_watch(): the earliest track frame written, and how many */
    long first_frame;
    unsigned long long watched;
    unsigned soft;                /* flush_soft()s (al_flush_soft) */
    unsigned drains;
} alsa_like_t;

static int al_open(void *ctx, const audio_format_t *f)
{
    alsa_like_t *a = ctx;
    a->opens++;
    return a->gone ? -2 : (a->only_rate && f->rate != a->only_rate) || (a->max_bits && f->bits > a->max_bits) ? -1 : 0;
}

static long al_write(void *ctx, const int32_t *x, long n)
{
    (void)x;
    alsa_like_t *a = ctx;
    pthread_mutex_lock(&a->mu);
    unsigned gen = a->flushes;
    while ((a->paused || a->gate) && a->flushes == gen) {
        a->in_write = 1;
        pthread_cond_broadcast(&a->cv);
        pthread_cond_wait(&a->cv, &a->mu);
    }
    a->in_write = 0;
    long r = a->flushes == gen || a->late ? n : -1;
    if (r > 0) {
        a->frames += (unsigned long long)n;
        a->queued += n;
        if (a->watch && x) {
            for (long i = 0; i < n; i++)
                if (frame_of(x + 2 * i) < a->first_frame) a->first_frame = frame_of(x + 2 * i);
            a->watched += (unsigned long long)n;
        }
    }
    pthread_mutex_unlock(&a->mu);
    return r;
}

static long al_delay(void *ctx) { (void)ctx; return 0; }
static void al_nop(void *ctx) { (void)ctx; }
/* ALSA's drain waits for what's queued to be played: a while, in which the
 * player queues the next track (an instant drain let a short track end
 * before the next was queued, on the H2's one core) */
static void al_drain(void *ctx) { ((alsa_like_t *)ctx)->drains++; sleep_ms(50); }

static void al_flush(void *ctx)
{
    alsa_like_t *a = ctx;
    pthread_mutex_lock(&a->mu);
    a->flushes++;
    a->queued = 0;
    a->paused = 0;
    pthread_cond_broadcast(&a->cv);
    pthread_mutex_unlock(&a->mu);
}

static void al_pause(void *ctx, int on)
{
    alsa_like_t *a = ctx;
    pthread_mutex_lock(&a->mu);
    a->paused = on;
    pthread_cond_broadcast(&a->cv);
    pthread_mutex_unlock(&a->mu);
}

static int al_can_cut(void *ctx) { return ((alsa_like_t *)ctx)->can_cut; }

/* As ALSA's: queued audio goes (faded), the write isn't woken: called between writes */
static void al_flush_soft(void *ctx)
{
    alsa_like_t *a = ctx;
    pthread_mutex_lock(&a->mu);
    a->soft++;
    a->queued = 0;
    pthread_mutex_unlock(&a->mu);
}

static void al_describe(void *ctx, char *o, int cap) { (void)ctx; snprintf(o, (size_t)cap, "alsa-like"); }

static void al_create(audio_output_t *out, alsa_like_t *a)
{
    memset(a, 0, sizeof *a);
    pthread_mutex_init(&a->mu, NULL);
    pthread_cond_init(&a->cv, NULL);
    *out = (audio_output_t){al_open, al_write, al_delay, al_drain, al_flush, al_pause, al_nop, al_describe, a, NULL, al_can_cut, NULL, NULL};
}

static void al_gate(alsa_like_t *a, int on)
{
    pthread_mutex_lock(&a->mu);
    a->gate = on;
    pthread_cond_broadcast(&a->cv);
    pthread_mutex_unlock(&a->mu);
}

/* Waits until the engine's thread is held up in a write. */
static int al_wait_in_write(alsa_like_t *a)
{
    int in = 0;
    for (int i = 0; i < 400 && !in; i++) {
        pthread_mutex_lock(&a->mu);
        in = a->in_write;
        pthread_mutex_unlock(&a->mu);
        if (!in) sleep_ms(5);
    }
    return in;
}

static void al_watch(alsa_like_t *a)
{
    pthread_mutex_lock(&a->mu);
    a->watch = 1;
    a->first_frame = 0x7fffffff;
    a->watched = 0;
    pthread_mutex_unlock(&a->mu);
}

static unsigned long long al_watched(alsa_like_t *a, long *first)
{
    pthread_mutex_lock(&a->mu);
    unsigned long long n = a->watched;
    if (first) *first = a->first_frame;
    pthread_mutex_unlock(&a->mu);
    return n;
}

/* A track started part-way in (an episode where it was left), on an engine
 * that sat paused, so idle: nothing before that point is ever written. A
 * play and then a seek let the idle thread start on the play first (the
 * beginning heard for a moment). And the status says where it's going at
 * once, for a start part-way in and for a seek. */
static void test_play_at(const char *dir)
{
    char a[300];
    snprintf(a, sizeof a, "%s/at.flac", dir);
    CHECK(write_ramp(a, 44100, 44100 * 5) == 0);
    audio_output_t out;
    static alsa_like_t al;
    al_create(&out, &al);
    engine_t *e = engine_new(&out);
    for (int round = 0; round < 20; round++) {      /* (the race it closes was there only now and then) */
        engine_play(e, a, 1);
        WAIT(status(e).state == ENG_PLAYING && !status(e).starting, 2000);
        engine_pause(e, 1);
        WAIT(status(e).state == ENG_PAUSED, 1000);
        sleep_ms(20);                                 /* (paused: the thread waits, idle) */
        al_watch(&al);
        engine_play_at(e, a, 2, 1.0f, 3000, 0);
        eng_status_t s = status(e);
        CHECK(!s.starting || s.start_ms == 3000);
        long first = 0;
        WAIT(al_watched(&al, NULL) > 4410, 3000);
        al_watched(&al, &first);
        if (first < 3 * 44100) fprintf(stderr, "round %d: frame %ld written, before 3 s\n", round, first);
        CHECK(first >= 3 * 44100);
        s = status(e);
        CHECK(s.track_id == 2 && s.pos_ms >= 3000);
    }
    /* a seek: where it's going, before the thread has got there */
    engine_pause(e, 1);
    engine_seek_ms(e, 1234);
    eng_status_t s = status(e);
    CHECK(s.pos_ms == 1234 || (s.pos_ms >= 1234 && s.pos_ms < 1400));
    engine_free(e);
    unlink(a);
}

/* A track started paused (Next while paused): nothing of it is written until
 * the resume, and then it starts at its beginning (or where it was asked to).
 * A play and then a pause let the thread start it playing in between: a
 * moment of the new track heard at each press. */
static unsigned long long al_frames(alsa_like_t *a)
{
    pthread_mutex_lock(&a->mu);
    unsigned long long n = a->frames;
    pthread_mutex_unlock(&a->mu);
    return n;
}

static void test_play_paused(const char *dir)
{
    char a[300], b[300];
    snprintf(a, sizeof a, "%s/pp-a.flac", dir);
    snprintf(b, sizeof b, "%s/pp-b.flac", dir);
    CHECK(write_ramp(a, 44100, 44100 * 5) == 0 && write_ramp(b, 44100, 44100 * 5) == 0);
    audio_output_t out;
    static alsa_like_t al;
    al_create(&out, &al);
    engine_t *e = engine_new(&out);
    for (int round = 0; round < 20; round++) {
        uint32_t from = round % 2 ? 2000 : 0;
        engine_play(e, a, 1);
        WAIT(status(e).state == ENG_PLAYING && !status(e).starting && al_frames(&al) > 0, 2000);
        engine_pause(e, 1);
        WAIT(status(e).state == ENG_PAUSED, 1000);
        if (round % 4 < 2) sleep_ms(20);              /* (the thread waiting in its write, or not there yet) */
        al_watch(&al);
        engine_play_at(e, b, 2, 1.0f, from, 1);
        eng_status_t s = status(e);
        CHECK(!s.starting || s.start_paused);
        WAIT(status(e).track_id == 2 && !status(e).starting, 2000);
        sleep_ms(30);                                 /* (time for it to play, if it would) */
        s = status(e);
        CHECK(s.track_id == 2 && s.state == ENG_PAUSED && !s.output_lost && !s.error);
        CHECK(al_watched(&al, NULL) == 0);            /* nothing written since the skip */
        if (al_watched(&al, NULL)) fprintf(stderr, "round %d: %llu frames written while paused\n", round, al_watched(&al, NULL));
        /* a second skip while still paused: the same */
        engine_play_at(e, a, 3, 1.0f, from, 1);
        WAIT(status(e).track_id == 3 && !status(e).starting, 2000);
        sleep_ms(10);
        CHECK(status(e).state == ENG_PAUSED && al_watched(&al, NULL) == 0);
        engine_pause(e, 0);                           /* the resume: from its start (or where asked) */
        long first = 0;
        WAIT(al_watched(&al, NULL) > 0, 2000);
        CHECK(al_watched(&al, &first) > 0 && first == (long)from * 441 / 10);
        s = status(e);
        CHECK(s.track_id == 3 && s.state == ENG_PLAYING);
    }
    /* a pause or a resume before the thread has taken the command still decides */
    engine_pause(e, 1);
    WAIT(status(e).state == ENG_PAUSED, 1000);
    al_gate(&al, 1);
    engine_play_at(e, b, 4, 1.0f, 0, 1);
    engine_pause(e, 0);
    al_gate(&al, 0);
    WAIT(status(e).track_id == 4 && status(e).state == ENG_PLAYING, 2000);
    CHECK(status(e).track_id == 4 && status(e).state == ENG_PLAYING);
    engine_free(e);
    unlink(a);
    unlink(b);
}

/* Crossfeed on a 16-bit stereo track: the output goes 24-bit (its low bits
 * kept) and the track no longer counts as untouched; off again, it does (the
 * open 24-bit output kept, as for the EQ). Pure mode is the device's: it
 * just asks for XF_OFF. */
static void test_crossfeed(const char *dir)
{
    char a[300];
    snprintf(a, sizeof a, "%s/xf.flac", dir);
    CHECK(write_ramp(a, 44100, 44100 * 3) == 0);
    audio_output_t out;
    out_null_stats_t st = {0};
    out_null_create(&out, &st);
    engine_t *e = engine_new(&out);
    engine_play(e, a, 1);
    engine_pause(e, 1);
    WAIT(st.opens >= 1, 2000);
    CHECK(st.fmt.bits == 16 && status(e).untouched && status(e).crossfeed == XF_OFF);
    engine_set_crossfeed(e, XF_MEDIUM);
    engine_pause(e, 0);
    WAIT(st.fmt.bits == 24, 2000);
    eng_status_t s = status(e);
    CHECK(st.fmt.bits == 24 && s.crossfeed == XF_MEDIUM && !s.untouched);
    engine_set_crossfeed(e, XF_OFF);
    WAIT(status(e).crossfeed == XF_OFF, 2000);
    s = status(e);
    CHECK(s.crossfeed == XF_OFF && s.untouched);
    engine_free(e);
    unlink(a);
}

/* Pause, seek, play and stop while the thread waits in a write on the paused
 * output: each takes effect, and none is taken for an unplugged device. */
static void test_paused_write(const char *dir)
{
    char a[300], b[300];
    snprintf(a, sizeof a, "%s/long.flac", dir);
    snprintf(b, sizeof b, "%s/short.flac", dir);
    CHECK(write_ramp(a, 44100, 44100 * 5) == 0 && write_ramp(b, 44100, 4410) == 0);
    audio_output_t out;
    alsa_like_t al;
    al_create(&out, &al);
    engine_t *e = engine_new(&out);

    /* a seek while paused, the thread waiting in its write */
    al_gate(&al, 1);
    engine_play(e, a, 1);
    CHECK(al_wait_in_write(&al));
    engine_pause(e, 1);
    al_gate(&al, 0);                              /* (still waiting: paused) */
    engine_seek_ms(e, 2000);
    WAIT(status(e).pos_ms >= 1900, 1000);
    eng_status_t s = status(e);
    CHECK(s.pos_ms >= 1900 && s.pos_ms <= 2100 && s.state == ENG_PAUSED && !s.output_lost);

    /* another track while paused */
    al_gate(&al, 1);
    engine_pause(e, 0);
    CHECK(al_wait_in_write(&al));
    engine_pause(e, 1);
    al_gate(&al, 0);
    engine_play(e, b, 2);
    WAIT(status(e).track_id == 2, 1000);
    s = status(e);
    CHECK(s.track_id == 2 && !s.output_lost);

    /* a stop while paused: stopped, not "the device went away" */
    al_gate(&al, 1);
    engine_play(e, a, 3);
    CHECK(al_wait_in_write(&al));
    engine_pause(e, 1);
    al_gate(&al, 0);
    engine_stop(e);
    sleep_ms(100);
    s = status(e);
    CHECK(s.state == ENG_STOPPED && !s.output_lost);
    engine_free(e);
    unlink(a);
    unlink(b);
}

/* A stop while paused, the write waiting on the paused output woken only once
 * the output is ready again (as ALSA's can be, after the flush's PREPARE): the
 * rest of the old track's chunk went into the fresh buffer and played (up to
 * 46 ms). Nothing of it is left queued now. */
static void test_paused_stop_late(const char *dir)
{
    char a[300];
    snprintf(a, sizeof a, "%s/long.flac", dir);
    CHECK(write_ramp(a, 44100, 44100 * 5) == 0);
    audio_output_t out;
    alsa_like_t al;
    al_create(&out, &al);
    engine_t *e = engine_new(&out);
    al_gate(&al, 1);
    engine_play(e, a, 1);
    CHECK(al_wait_in_write(&al));
    engine_pause(e, 1);
    pthread_mutex_lock(&al.mu);
    al.late = 1;
    pthread_mutex_unlock(&al.mu);
    al_gate(&al, 0);                              /* (still waiting: paused) */
    engine_stop(e);
    sleep_ms(100);
    eng_status_t s = status(e);
    pthread_mutex_lock(&al.mu);
    long queued = al.queued;
    pthread_mutex_unlock(&al.mu);
    CHECK(s.state == ENG_STOPPED && !s.output_lost);
    CHECK(queued == 0);
    engine_free(e);
    unlink(a);
}

/* An output that can't open a track's format: the track fails (stopped, an
 * error, reported as the track that's on), as it starts or gapless after
 * another; it isn't left "playing" at 0:00. A device that's gone: paused,
 * output lost, and opened again at the resume. */
static void test_open_fails(const char *dir)
{
    char a[300], b[300];
    snprintf(a, sizeof a, "%s/44k.flac", dir);
    snprintf(b, sizeof b, "%s/22k.flac", dir);
    CHECK(write_ramp(a, 44100, 4410) == 0 && write_ramp(b, 22050, 2205) == 0);
    audio_output_t out;
    alsa_like_t al;
    al_create(&out, &al);
    al.only_rate = 44100;
    engine_t *e = engine_new(&out);

    eng_status_t s0 = status(e);
    engine_play(e, b, 7);                         /* a 22 kHz track on an output that can't */
    WAIT(status(e).state == ENG_STOPPED && status(e).error, 1000);
    eng_status_t s = status(e);
    CHECK(s.state == ENG_STOPPED && s.error && s.ended && s.track_id == 7 && s.stop_seq == s0.stop_seq + 1);

    engine_play(e, a, 1);                         /* gapless onto it */
    engine_queue_next(e, b, 8);
    WAIT(status(e).track_id == 8 && status(e).state == ENG_STOPPED, 1000);
    s = status(e);
    CHECK(s.track_id == 8 && s.state == ENG_STOPPED && s.error && s.stop_seq == s0.stop_seq + 2);

    al.gone = 1;                                  /* unplugged */
    engine_play(e, a, 2);
    WAIT(status(e).output_lost, 1000);
    s = status(e);
    CHECK(s.state == ENG_PAUSED && s.output_lost && !s.error && s.track_id == 2);
    al.gone = 0;
    unsigned long long f0 = al.frames;
    engine_pause(e, 0);                           /* back: the resume opens it again */
    WAIT(status(e).ended, 1000);
    s = status(e);
    CHECK(s.ended && !s.error && al.frames - f0 == 4410);
    engine_free(e);
    unlink(a);
    unlink(b);
}

/* ---- an output like Bluetooth's: real time, half a second queued; a pause
 * ends the stream and what was queued goes ---- */

typedef struct {
    pthread_mutex_t mu;
    unsigned rate;
    double start;                 /* when frame `base` of what's written was heard */
    unsigned long long written;   /* frames queued since start */
    int paused;
    long base;                    /* the track frame heard at start */
    long heard_at_pause, first_after; /* the frame heard as it paused; the first written after */
    int stall;                    /* the link takes nothing: the next write fails and the stream goes */
    long lost;                    /* ...what was queued then, reported as the delay (as out_bt) */
    long heard_at_stall;
    unsigned flushes;
    int draining;                 /* bl_drain is waiting for the queue to play out */
} bt_like_t;

static long bl_played(bt_like_t *b)
{
    double el = now_s() - b->start;
    unsigned long long p = el > 0 ? (unsigned long long)(el * b->rate) : 0;
    return (long)(p < b->written ? p : b->written);
}

static int bl_open(void *ctx, const audio_format_t *f)
{
    bt_like_t *b = ctx;
    b->rate = f->rate;
    return 0;
}

static long bl_write(void *ctx, const int32_t *x, long n)
{
    bt_like_t *b = ctx;
    for (;;) {
        pthread_mutex_lock(&b->mu);
        if (b->paused) { pthread_mutex_unlock(&b->mu); return n; }   /* (dropped) */
        if (b->stall) {                                  /* stuck: the stream closes, its queue goes */
            long played = bl_played(b);
            b->heard_at_stall = b->base + played;
            b->lost = (long)b->written - played;
            b->written = 0;
            b->first_after = -1;
            pthread_mutex_unlock(&b->mu);
            return -1;
        }
        long q = (long)b->written - bl_played(b);
        if (q <= 0) {                                    /* ran dry (or a new stream): start here */
            b->start = now_s();
            b->written = 0;
            b->base = frame_of(x);
            if (b->first_after < 0) b->first_after = b->base;
        }
        if (q < (long)b->rate / 2) {
            b->written += (unsigned long long)n;
            pthread_mutex_unlock(&b->mu);
            return n;
        }
        pthread_mutex_unlock(&b->mu);
        sleep_ms(5);
    }
}

static long bl_delay(void *ctx)
{
    bt_like_t *b = ctx;
    pthread_mutex_lock(&b->mu);
    long d = b->paused ? 0 : b->lost ? b->lost : (long)b->written - bl_played(b);
    pthread_mutex_unlock(&b->mu);
    return d;
}

static void bl_flush(void *ctx)
{
    bt_like_t *b = ctx;
    pthread_mutex_lock(&b->mu);
    b->written = 0;
    b->lost = 0;
    b->flushes++;
    pthread_mutex_unlock(&b->mu);
}

static void bl_pause(void *ctx, int on)
{
    bt_like_t *b = ctx;
    pthread_mutex_lock(&b->mu);
    if (on && !b->paused) {
        b->heard_at_pause = b->base + bl_played(b);
        b->written = 0;                                  /* the stream ends: its queue goes */
        b->first_after = -1;
    }
    b->paused = on;
    pthread_mutex_unlock(&b->mu);
}

static void bl_nop(void *ctx) { (void)ctx; }
/* As ALSA's drain: back once what's queued has played, or has been dropped
 * (a pause, a flush). */
static void bl_drain(void *ctx)
{
    bt_like_t *b = ctx;
    for (;;) {
        pthread_mutex_lock(&b->mu);
        long q = (long)b->written - (b->paused ? 0 : bl_played(b));
        b->draining = q > 0;
        pthread_mutex_unlock(&b->mu);
        if (q <= 0) return;
        sleep_ms(5);
    }
}
static void bl_describe(void *ctx, char *o, int cap) { (void)ctx; snprintf(o, (size_t)cap, "bt-like"); }

/* Paused on Bluetooth and resumed: it carries on from what was heard, not
 * from where the decoder had got to (half a second on); the position shown
 * while paused is what was heard. */
static void test_pause_drops(const char *dir)
{
    char a[300];
    snprintf(a, sizeof a, "%s/ramp.flac", dir);
    CHECK(write_ramp(a, 44100, 44100 * 4) == 0);
    bt_like_t bl;
    memset(&bl, 0, sizeof bl);
    pthread_mutex_init(&bl.mu, NULL);
    audio_output_t out = {bl_open, bl_write, bl_delay, bl_nop, bl_flush, bl_pause, bl_nop, bl_describe, &bl, NULL, NULL, NULL, NULL};
    engine_t *e = engine_new(&out);
    engine_play(e, a, 1);
    sleep_ms(1000);
    engine_pause(e, 1);
    eng_status_t s = status(e);
    long heard = bl.heard_at_pause;
    CHECK(heard > 44100 / 2 && heard < 44100 * 2);
    CHECK(labs((long)s.pos_ms - heard * 1000 / 44100) < 60);   /* shown: what was heard */
    sleep_ms(200);
    engine_pause(e, 0);
    WAIT(bl.first_after >= 0, 1000);
    CHECK(bl.first_after >= 0 && labs(bl.first_after - heard) < 44100 / 20);   /* on from there */
    if (labs(bl.first_after - heard) >= 44100 / 20)
        fprintf(stderr, "heard %ld at the pause, resumed at %ld\n", heard, bl.first_after);
    engine_free(e);
    unlink(a);
}

/* Paused in the last half second of the last track, while the output plays
 * out its queue, on an output that drops the queue at a pause (ALSA can't
 * pause while draining): the track isn't over (it ended, paused, and Play
 * started the song again); the resume plays the rest from what was heard,
 * and then it ends. */
static void test_pause_at_the_end(const char *dir)
{
    char a[300];
    snprintf(a, sizeof a, "%s/tail.flac", dir);
    CHECK(write_ramp(a, 44100, 44100 * 2) == 0);
    bt_like_t bl;
    memset(&bl, 0, sizeof bl);
    pthread_mutex_init(&bl.mu, NULL);
    audio_output_t out = {bl_open, bl_write, bl_delay, bl_drain, bl_flush, bl_pause, bl_nop, bl_describe, &bl, NULL, NULL, NULL, NULL};
    engine_t *e = engine_new(&out);
    engine_play(e, a, 1);
    WAIT(bl.draining, 4000);
    CHECK(bl.draining);
    sleep_ms(100);                                       /* (some 0.3 s of it still to be heard) */
    engine_pause(e, 1);
    long heard = bl.heard_at_pause;
    sleep_ms(300);
    eng_status_t s = status(e);
    CHECK(s.state == ENG_PAUSED && s.track_id == 1);
    CHECK(heard > 44100 && heard < 44100 * 2 && labs((long)s.pos_ms - heard * 1000 / 44100) < 60);
    if (s.state != ENG_PAUSED) fprintf(stderr, "paused at the end: state %d, heard %ld, shown %u ms\n", s.state, heard, s.pos_ms);
    engine_pause(e, 0);
    WAIT(bl.first_after >= 0, 1000);
    CHECK(bl.first_after >= 0 && labs(bl.first_after - heard) < 44100 / 20);   /* the rest, from there */
    WAIT(status(e).state == ENG_STOPPED, 3000);
    CHECK(status(e).state == ENG_STOPPED);                                       /* and then its end */
    engine_free(e);
    unlink(a);
}

/* The Bluetooth link stalls mid-track (the headphones took nothing for 2 s:
 * out_bt closes the stream and its queue goes): paused as output lost, the
 * position shown is what was heard, and the resume carries on from there,
 * not from where the decoder had got to. */
static void test_stall_drops(const char *dir)
{
    char a[300];
    snprintf(a, sizeof a, "%s/ramp.flac", dir);
    CHECK(write_ramp(a, 44100, 44100 * 4) == 0);
    bt_like_t bl;
    memset(&bl, 0, sizeof bl);
    pthread_mutex_init(&bl.mu, NULL);
    bl.first_after = 0;
    audio_output_t out = {bl_open, bl_write, bl_delay, bl_nop, bl_flush, bl_pause, bl_nop, bl_describe, &bl, NULL, NULL, NULL, NULL};
    engine_t *e = engine_new(&out);
    engine_play(e, a, 1);
    sleep_ms(1000);
    pthread_mutex_lock(&bl.mu);
    bl.stall = 1;
    pthread_mutex_unlock(&bl.mu);
    WAIT(status(e).output_lost, 2000);
    eng_status_t s = status(e);
    CHECK(s.output_lost && s.state == ENG_PAUSED);
    long heard = bl.heard_at_stall;
    CHECK(heard > 44100 / 2 && heard < 44100 * 2);
    CHECK(bl.lost > 44100 / 4);                                  /* about half a second went */
    CHECK(labs((long)s.pos_ms - heard * 1000 / 44100) < 60);    /* shown: what was heard */
    pthread_mutex_lock(&bl.mu);
    bl.stall = 0;
    pthread_mutex_unlock(&bl.mu);
    engine_pause(e, 0);                                          /* resumed (headphones back) */
    WAIT(bl.first_after >= 0, 1000);
    CHECK(bl.first_after >= 0 && labs(bl.first_after - heard) < 44100 / 20);   /* on from there */
    if (labs(bl.first_after - heard) >= 44100 / 20)
        fprintf(stderr, "heard %ld at the stall, resumed at %ld\n", heard, bl.first_after);
    engine_free(e);
    unlink(a);
}

/* A skip or a seek while the thread waits in a write as it plays (for room
 * in the output's buffer): on an output that allows it, the write is cut
 * short and the new track or position starts at once; on one that doesn't
 * (Bluetooth), it waits for the write, as before. Neither is a lost device. */
static void test_cut_write(const char *dir)
{
    char a[300], b[300];
    snprintf(a, sizeof a, "%s/cut_a.flac", dir);
    snprintf(b, sizeof b, "%s/cut_b.flac", dir);
    CHECK(write_ramp(a, 44100, 44100 * 5) == 0 && write_ramp(b, 44100, 44100 * 5) == 0);
    audio_output_t out;
    static alsa_like_t al;
    al_create(&out, &al);
    engine_t *e = engine_new(&out);
    for (int cut = 1; cut >= 0; cut--) {
        al.can_cut = cut;
        al_gate(&al, 1);
        engine_play(e, a, 1);
        CHECK(al_wait_in_write(&al));
        pthread_mutex_lock(&al.mu);
        unsigned f0 = al.flushes;
        pthread_mutex_unlock(&al.mu);
        engine_play(e, b, 2);                     /* the write still held up */
        sleep_ms(150);
        eng_status_t s = status(e);
        pthread_mutex_lock(&al.mu);
        unsigned f1 = al.flushes;
        pthread_mutex_unlock(&al.mu);
        if (cut) CHECK(s.track_id == 2 && s.state == ENG_PLAYING && !s.output_lost && f1 > f0);
        else CHECK(f1 == f0);                     /* (no flush from here: it waits for the write) */
        CHECK(al_wait_in_write(&al));
        engine_seek_ms(e, 3000);
        sleep_ms(150);
        s = status(e);
        if (cut) CHECK(s.track_id == 2 && s.pos_ms >= 2900);
        al_gate(&al, 0);
        WAIT(status(e).track_id == 2 && status(e).pos_ms >= 2900, 2000);
        s = status(e);
        CHECK(s.track_id == 2 && s.pos_ms >= 2900 && !s.output_lost);
        engine_stop(e);
        sleep_ms(50);
    }
    engine_free(e);
    unlink(a);
    unlink(b);
}

/* Crossfeed turned on mid-track (a 16-bit track: the output opens again at
 * 24 bits, and what it had queued goes): it carries on from what was heard,
 * not half a second further along. */
static void test_crossfeed_keeps_place(const char *dir)
{
    char a[300];
    snprintf(a, sizeof a, "%s/xfplace.flac", dir);
    CHECK(write_ramp(a, 44100, 44100 * 4) == 0);
    bt_like_t bl;
    memset(&bl, 0, sizeof bl);
    pthread_mutex_init(&bl.mu, NULL);
    audio_output_t out = {bl_open, bl_write, bl_delay, bl_nop, bl_flush, bl_pause, bl_nop, bl_describe, &bl, NULL, NULL, NULL, NULL};
    engine_t *e = engine_new(&out);
    engine_play(e, a, 1);
    sleep_ms(1000);
    pthread_mutex_lock(&bl.mu);
    long heard = bl.base + bl_played(&bl);
    unsigned f0 = bl.flushes;
    pthread_mutex_unlock(&bl.mu);
    double t0 = now_s();
    engine_set_crossfeed(e, XF_MEDIUM);
    WAIT(bl.flushes != f0 && status(e).crossfeed == XF_MEDIUM, 2000);
    sleep_ms(300);
    eng_status_t s = status(e);
    long want_ms = heard * 1000 / 44100 + (long)((now_s() - t0) * 1000);
    CHECK(s.state == ENG_PLAYING && !s.error && s.crossfeed == XF_MEDIUM);
    CHECK(labs((long)s.pos_ms - want_ms) < 150);
    if (labs((long)s.pos_ms - want_ms) >= 150) fprintf(stderr, "at %u ms after crossfeed on, want about %ld\n", s.pos_ms, want_ms);
    engine_free(e);
    unlink(a);
}

/* Crossfeed turned on during a 16-bit track on an output that takes only
 * 16 bits: it keeps playing (as it was), not silent or failed. */
static void test_crossfeed_16bit_output(const char *dir)
{
    char a[300];
    snprintf(a, sizeof a, "%s/xf16.flac", dir);
    CHECK(write_ramp(a, 44100, 44100) == 0);
    audio_output_t out;
    alsa_like_t al;
    al_create(&out, &al);
    al.max_bits = 16;
    al_gate(&al, 1);                              /* writes held: the change lands mid-track */
    engine_t *e = engine_new(&out);
    engine_play(e, a, 1);
    CHECK(al_wait_in_write(&al));
    engine_set_crossfeed(e, XF_MEDIUM);
    al_gate(&al, 0);
    WAIT(status(e).ended, 3000);
    eng_status_t s = status(e);
    CHECK(s.ended && !s.error && s.crossfeed == XF_MEDIUM);
    CHECK(al.opens >= 3);                         /* 16 bits, 24 refused, 16 again */
    engine_free(e);
    unlink(a);
}

/* An output with a soft flush (ALSA's: a fade, not a click): a new track
 * doesn't cut the write under way (it ends within its chunk), and then what's
 * queued goes by the soft flush, on the engine's thread, not the hard one. */
static void test_flush_soft(const char *dir)
{
    char a[300], b[300];
    snprintf(a, sizeof a, "%s/soft-a.flac", dir);
    snprintf(b, sizeof b, "%s/soft-b.flac", dir);
    CHECK(write_ramp(a, 44100, 44100 * 2) == 0 && write_ramp(b, 44100, 44100) == 0);
    audio_output_t out;
    alsa_like_t al;
    al_create(&out, &al);
    al.can_cut = 1;
    out.flush_soft = al_flush_soft;
    engine_t *e = engine_new(&out);
    al_gate(&al, 1);                              /* writes held: the new track comes mid-write */
    engine_play(e, a, 1);
    CHECK(al_wait_in_write(&al));
    engine_play(e, b, 2);
    sleep_ms(50);
    CHECK(al.flushes == 0);                       /* the write wasn't cut */
    al_gate(&al, 0);
    WAIT(status(e).track_id == 2 && al.soft >= 1, 2000);
    CHECK(status(e).track_id == 2 && al.soft >= 1 && al.flushes == 0);
    engine_seek_ms(e, 500);                       /* a seek: the same */
    WAIT(al.soft >= 2, 2000);
    CHECK(al.soft >= 2 && al.flushes == 0);
    /* another rate: the output opens again; first the fade, played out (a
     * drain), not a drop mid-waveform */
    char c[300];
    snprintf(c, sizeof c, "%s/soft-c.flac", dir);
    CHECK(write_ramp(c, 48000, 48000) == 0);
    al_gate(&al, 1);                              /* b again, held in a write: playing, not ended */
    engine_play(e, b, 4);
    CHECK(al_wait_in_write(&al));
    unsigned soft0 = al.soft, drains0 = al.drains;
    int opens0 = al.opens;
    engine_play(e, c, 3);
    al_gate(&al, 0);
    WAIT(status(e).track_id == 3 && al.opens > opens0, 2000);
    /* (c is a second, written at once here: it may have ended, with a drain of its own, by now) */
    CHECK(status(e).track_id == 3 && al.opens == opens0 + 1 && al.soft == soft0 + 1 && al.drains >= drains0 + 1 &&
          al.flushes == 0);
    if (!(status(e).track_id == 3 && al.opens == opens0 + 1 && al.soft == soft0 + 1 && al.drains >= drains0 + 1 && al.flushes == 0))
        fprintf(stderr, "track %lld, opens +%d, soft +%u, drains +%u, flushes %u\n", (long long)status(e).track_id,
                al.opens - opens0, al.soft - soft0, al.drains - drains0, al.flushes);
    engine_free(e);
    unlink(a);
    unlink(b);
    unlink(c);
}

/* ---- a file that is slow to open: a named pipe called .flac. The engine's
 * open of it waits until something opens it for writing, then reads what is
 * written: a FLAC file's bytes (it plays) or anything else (it can't be
 * opened). The feeder waits for `go`, at most 5 s (a test that hangs says
 * less than one that fails). ---- */

typedef struct {
    const char *fifo, *src;       /* src NULL: not a FLAC file */
    volatile int go, fed;
    pthread_t th;
} feed_t;

static void *feed_run(void *arg)
{
    feed_t *f = arg;
    for (int i = 0; i < 1000 && !f->go; i++) sleep_ms(5);
    int fd = -1;
    for (int i = 0; i < 1000 && fd < 0; i++) {       /* (no reader yet: ENXIO) */
        fd = open(f->fifo, O_WRONLY | O_NONBLOCK);
        if (fd < 0) sleep_ms(5);
    }
    if (fd >= 0) {
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
        FILE *src = f->src ? fopen(f->src, "rb") : NULL;
        char buf[4096];
        size_t n;
        while (src && (n = fread(buf, 1, sizeof buf, src)) > 0)
            if (write(fd, buf, n) < 0) break;         /* (the reader gave it up: dropped) */
        if (!src && write(fd, "not a flac file", 15) < 0) { /* (the same) */ }
        if (src) fclose(src);
        close(fd);
        f->fed = 1;
    }
    return NULL;
}

static void feed_start(feed_t *f, const char *fifo, const char *src)
{
    f->fifo = fifo;
    f->src = src;
    f->go = f->fed = 0;
    pthread_create(&f->th, NULL, feed_run, f);
}

static int feed_finish(feed_t *f)
{
    f->go = 1;
    pthread_join(f->th, NULL);
    return f->fed;
}

static unsigned al_soft(alsa_like_t *a)
{
    pthread_mutex_lock(&a->mu);
    unsigned n = a->soft;
    pthread_mutex_unlock(&a->mu);
    return n;
}

static long al_queued(alsa_like_t *a)
{
    pthread_mutex_lock(&a->mu);
    long n = a->queued;
    pthread_mutex_unlock(&a->mu);
    return n;
}

/* The engine playing `path` as `id`, held in a write (the output takes
 * everything at once: without the gate a track is over in a moment). */
static void play_held(engine_t *e, alsa_like_t *al, const char *path, int64_t id)
{
    al_gate(al, 1);
    engine_play(e, path, id);
    WAIT(status(e).track_id == id && !status(e).starting, 2000);
    CHECK(al_wait_in_write(al) && status(e).track_id == id && status(e).state == ENG_PLAYING);
}

/* Next onto a file that is slow to open. What was playing stops at once: its
 * queue is dropped before the open, and nothing more of it is written (it
 * played on until the new file was open). The engine answers meanwhile (the
 * open held its lock: the UI waited on it at every tick). A pause, a stop or
 * another play command while it opens each take effect, and a file that
 * can't be opened is reported as before. */
static void test_slow_open(const char *dir)
{
    char a[300], b[300], c[300], fifo[300];
    snprintf(a, sizeof a, "%s/so-a.flac", dir);
    snprintf(b, sizeof b, "%s/so-b.flac", dir);
    snprintf(c, sizeof c, "%s/so-c.flac", dir);
    snprintf(fifo, sizeof fifo, "%s/so-slow.flac", dir);
    CHECK(write_ramp(a, 44100, 44100 * 5) == 0 && write_ramp(b, 44100, 44100 * 5) == 0 &&
          write_ramp(c, 44100, 22050) == 0);
    if (mkfifo(fifo, 0600) != 0) {
        /* (the H2's card is FAT, which holds no FIFOs: in /tmp there, which
         * is RAM, and a FIFO takes none of it) */
        snprintf(fifo, sizeof fifo, "/tmp/fathom-test-so-slow-%d.flac", (int)getpid());
        unlink(fifo);
        CHECK(mkfifo(fifo, 0600) == 0);
    }
    audio_output_t out;
    static alsa_like_t al;
    al_create(&out, &al);
    out.flush_soft = al_flush_soft;
    engine_t *e = engine_new(&out);
    feed_t f;
    eng_status_t s, s0;

    /* 1. the old track stops at the command, and the engine answers */
    play_held(e, &al, a, 1);
    s0 = status(e);
    unsigned soft0 = al_soft(&al);
    feed_start(&f, fifo, c);
    engine_play(e, fifo, 2);
    al_watch(&al);
    al_gate(&al, 0);                              /* the write under way ends; then the command is taken */
    WAIT(al_soft(&al) > soft0, 2000);
    CHECK(al_soft(&al) == soft0 + 1);             /* dropped already, the file not yet open */
    double t0 = now_s();
    s = status(e);
    CHECK(now_s() - t0 < 0.5);                    /* (not behind the open) */
    CHECK(s.starting && s.track_id == 1 && s.seq == s0.seq);
    unsigned long long w = al_watched(&al, NULL);
    CHECK(w <= 2048 && al_queued(&al) == 0);      /* the chunk it was writing, at most; and that dropped */
    sleep_ms(100);
    CHECK(al_watched(&al, NULL) == w && al_queued(&al) == 0 && status(e).starting);
    /* ...then it opens, and plays from its start to its end */
    al_watch(&al);
    CHECK(feed_finish(&f));
    WAIT(status(e).ended, 3000);
    s = status(e);
    long first = -1;
    CHECK(s.track_id == 2 && s.ended && !s.error && s.seq == s0.seq + 1 && s.stop_seq == s0.stop_seq + 1);
    CHECK(al_watched(&al, &first) == 22050 && first == 0);
    CHECK(al_soft(&al) == soft0 + 1 && al.flushes == 0);   /* (dropped once, not again as it started) */

    /* 2. another play command while it opens: that one wins, the slow one is never on */
    play_held(e, &al, a, 3);
    s0 = status(e);
    feed_start(&f, fifo, c);
    engine_play(e, fifo, 4);
    al_gate(&al, 0);
    WAIT(status(e).starting && al_queued(&al) == 0 && !al.in_write, 2000);
    sleep_ms(20);
    al_gate(&al, 1);
    al_watch(&al);
    engine_play(e, b, 5);
    CHECK(feed_finish(&f));
    WAIT(status(e).track_id == 5 && !status(e).starting, 3000);
    CHECK(al_wait_in_write(&al));
    s = status(e);
    CHECK(s.track_id == 5 && s.state == ENG_PLAYING && !s.error && !s.ended);
    CHECK(s.seq == s0.seq + 1 && s.stop_seq == s0.stop_seq);
    CHECK(al_watched(&al, NULL) == 0);            /* (b's first chunk is held; nothing of the slow one) */

    /* 3. a pause while it opens: it starts paused, nothing written until the resume */
    feed_start(&f, fifo, c);
    engine_play(e, fifo, 6);
    al_gate(&al, 0);
    WAIT(status(e).starting && !al.in_write, 2000);
    sleep_ms(20);
    al_watch(&al);
    engine_pause(e, 1);
    s = status(e);
    CHECK(s.starting && s.start_paused);
    CHECK(feed_finish(&f));
    WAIT(status(e).track_id == 6 && !status(e).starting, 3000);
    sleep_ms(30);
    s = status(e);
    CHECK(s.track_id == 6 && s.state == ENG_PAUSED && al_watched(&al, NULL) == 0);
    engine_pause(e, 0);
    WAIT(status(e).ended, 3000);
    CHECK(status(e).ended && !status(e).error && al_watched(&al, &first) == 22050 && first == 0);

    /* 4. a stop while it opens: stopped at once, and it never starts */
    play_held(e, &al, a, 7);
    s0 = status(e);
    feed_start(&f, fifo, c);
    engine_play(e, fifo, 8);
    al_gate(&al, 0);
    WAIT(status(e).starting && !al.in_write, 2000);
    sleep_ms(20);
    al_watch(&al);
    engine_stop(e);
    s = status(e);
    CHECK(!s.starting && s.state == ENG_STOPPED);
    CHECK(feed_finish(&f));
    sleep_ms(100);
    s = status(e);
    CHECK(s.state == ENG_STOPPED && !s.starting && s.track_id == 7 && s.seq == s0.seq && !s.error &&
          s.stop_seq == s0.stop_seq && al_watched(&al, NULL) == 0);

    /* 5. a file that can't be opened, slowly or at once, while another plays:
     * stopped, an error, reported as the track that's on; and the next plays */
    for (int slow = 1; slow >= 0; slow--) {
        char gone[300];
        snprintf(gone, sizeof gone, "%s/so-gone.flac", dir);
        play_held(e, &al, a, 9);
        s0 = status(e);
        if (slow) feed_start(&f, fifo, NULL);
        engine_play(e, slow ? fifo : gone, 10 + slow);
        al_gate(&al, 0);
        if (slow) {
            WAIT(status(e).starting && !al.in_write, 2000);
            sleep_ms(20);
            CHECK(feed_finish(&f));
        }
        WAIT(status(e).error, 3000);
        s = status(e);
        CHECK(s.state == ENG_STOPPED && s.error && !s.error_output && s.ended && !s.starting && s.track_id == 10 + slow);
        CHECK(s.seq == s0.seq + 1 && s.stop_seq == s0.stop_seq + 1 && s.pos_ms == 0 && al_queued(&al) == 0);
        al_watch(&al);
        engine_play(e, c, 20 + slow);
        WAIT(status(e).ended, 3000);
        s = status(e);
        CHECK(s.track_id == 20 + slow && s.ended && !s.error && al_watched(&al, &first) == 22050 && first == 0);
    }
    engine_free(e);
    unlink(a);
    unlink(b);
    unlink(c);
    unlink(fifo);
}

/* ---- An output that queues a second, like the H2's DAC with a file: what's
 * queued plays in real time, a write waits while it's full, and join_depth
 * says a fifth of a second is enough ---- */

typedef struct {
    pthread_mutex_t mu;
    unsigned rate;
    double start;                  /* when what's written started to play */
    unsigned long long written;    /* frames written since */
    long deep, enough;             /* what it holds; what join_depth says */
    long max_q;                    /* the most seen queued since it was last zeroed */
    long last;                     /* the track frame written last (-1: none) */
    long q_at_join;                /* what was queued as a track's first frames came after another's (-1: not yet) */
    int ran_dry;                   /* writes that found the queue played out */
} deep_like_t;

static long dl_queued(deep_like_t *d)          /* (mu held) */
{
    double el = now_s() - d->start;
    unsigned long long p = el > 0 ? (unsigned long long)(el * d->rate) : 0;
    return p < d->written ? (long)(d->written - p) : 0;
}

static int dl_open(void *ctx, const audio_format_t *f)
{
    deep_like_t *d = ctx;
    d->rate = f->rate;
    d->deep = (long)f->rate;
    d->enough = (long)f->rate / 5;
    return 0;
}

static long dl_write(void *ctx, const int32_t *x, long n)
{
    deep_like_t *d = ctx;
    for (;;) {
        pthread_mutex_lock(&d->mu);
        long q = dl_queued(d);
        if (q + n <= d->deep) {
            if (q == 0) {                        /* played out (or the first): from here */
                if (d->written) d->ran_dry++;
                d->start = now_s();
                d->written = 0;
            }
            long first = frame_of(x);
            if (d->last >= 0 && first < d->last && d->q_at_join < 0) d->q_at_join = q;   /* a ramp starting over */
            d->last = frame_of(x + 2 * (n - 1));
            d->written += (unsigned long long)n;
            if (q + n > d->max_q) d->max_q = q + n;
            pthread_mutex_unlock(&d->mu);
            return n;
        }
        pthread_mutex_unlock(&d->mu);
        sleep_ms(2);
    }
}

static long dl_delay(void *ctx)
{
    deep_like_t *d = ctx;
    pthread_mutex_lock(&d->mu);
    long q = dl_queued(d);
    pthread_mutex_unlock(&d->mu);
    return q;
}

static void dl_drain(void *ctx) { while (dl_delay(ctx) > 0) sleep_ms(5); }

static void dl_flush(void *ctx)
{
    deep_like_t *d = ctx;
    pthread_mutex_lock(&d->mu);
    d->written = 0;
    d->last = -1;
    pthread_mutex_unlock(&d->mu);
}

static void dl_pause(void *ctx, int on) { (void)ctx; (void)on; }
static void dl_nop(void *ctx) { (void)ctx; }
static void dl_describe(void *ctx, char *o, int cap) { (void)ctx; snprintf(o, (size_t)cap, "deep-like"); }
static long dl_join_depth(void *ctx) { return ((deep_like_t *)ctx)->enough; }

/* A file on an output that queues a second: the queue fills (a stall of the
 * decoder shorter than that isn't a gap), but a gapless successor becomes the
 * track that's on only once the queue has played down to what's enough, with
 * no gap; a fade under way and a sound setting just changed keep it down too. */
static void test_deep_queue(const char *dir)
{
    char a[300], b[300];
    snprintf(a, sizeof a, "%s/deep-a.flac", dir);
    snprintf(b, sizeof b, "%s/deep-b.flac", dir);
    CHECK(write_ramp(a, 44100, 44100 * 2) == 0 && write_ramp(b, 44100, 44100 * 4) == 0);
    deep_like_t dl = {.mu = PTHREAD_MUTEX_INITIALIZER, .last = -1, .q_at_join = -1};
    audio_output_t out = {dl_open, dl_write, dl_delay, dl_drain, dl_flush, dl_pause, dl_nop, dl_describe, &dl, NULL, NULL,
                          NULL, dl_join_depth};
    engine_t *e = engine_new(&out);
    engine_play(e, a, 1);
    engine_queue_next(e, b, 2);
    WAIT(dl_delay(&dl) > dl.deep * 3 / 4, 1500);
    CHECK(dl_delay(&dl) > dl.deep * 3 / 4);                   /* it queues its second */
    CHECK(status(e).track_id == 1);
    /* the first track's two seconds are decoded within moments; it stays the
     * one that's on while more than a fifth of a second of it is unheard */
    double t0 = now_s();
    int early = 0;
    while (status(e).track_id == 1 && now_s() - t0 < 4) {
        eng_status_t st = status(e);
        if (st.track_id == 2 && dl_delay(&dl) > dl.enough + 2048) early = 1;
        sleep_ms(5);
    }
    CHECK(status(e).track_id == 2);
    CHECK(!early);
    CHECK(now_s() - t0 > 0.5);                               /* (not at once: the queue played down first) */
    WAIT(dl.q_at_join >= 0, 500);
    pthread_mutex_lock(&dl.mu);
    long qj = dl.q_at_join;
    int dry = dl.ran_dry;
    pthread_mutex_unlock(&dl.mu);
    CHECK(qj > 0 && qj <= dl.enough);                        /* joined with what's enough queued: no gap, no second ahead */
    CHECK(dry == 0);
    CHECK(status(e).pos_ms < 400);
    WAIT(dl_delay(&dl) > dl.deep * 3 / 4, 1500);
    CHECK(dl_delay(&dl) > dl.deep * 3 / 4);                   /* and a second again after the join */

    /* a fade (the sleep timer's): the queue plays down and stays down */
    engine_set_fade(e, 0.5f);
    WAIT(dl_delay(&dl) <= dl.enough, 1500);
    CHECK(dl_delay(&dl) <= dl.enough);
    long most = 0;
    for (int i = 0; i < 40; i++) {
        long q = dl_delay(&dl);
        if (q > most) most = q;
        sleep_ms(5);
    }
    CHECK(most <= dl.enough + 2048);                          /* (a chunk goes in once it's under) */
    CHECK(most > 0);
    engine_set_fade(e, 1.0f);
    WAIT(dl_delay(&dl) > dl.deep * 3 / 4, 1500);
    CHECK(dl_delay(&dl) > dl.deep * 3 / 4);                   /* the fade over: a second again */

    /* a sound setting changed: down again, so the next change is heard at once */
    engine_set_balance(e, 0.5f, 1.0f);
    WAIT(dl_delay(&dl) <= dl.enough, 1500);
    CHECK(dl_delay(&dl) <= dl.enough);
    sleep_ms(200);
    CHECK(dl_delay(&dl) <= dl.enough + 2048);
    pthread_mutex_lock(&dl.mu);
    CHECK(dl.ran_dry == 0);
    pthread_mutex_unlock(&dl.mu);
    engine_stop(e);
    engine_free(e);
    unlink(a);
    unlink(b);
}

/* A file that is slow to open (a big picture to pass over, a busy card), as
 * the one to follow: it is opened while the second queued for the track
 * that's on plays out, not when only a fifth of a second is left, so the
 * sound doesn't run dry at the change. (Opened at the change, 0.6 s of
 * opening ran the queue dry.) And what was opened ahead is let go when the
 * queue changes or playback is replaced. */
static void test_open_ahead(const char *dir)
{
    char a[300], b[300], c[300];
    snprintf(a, sizeof a, "%s/slow-a.flac", dir);
    snprintf(b, sizeof b, "%s/slow-b.flac", dir);
    snprintf(c, sizeof c, "%s/slow-c.flac", dir);
    CHECK(write_ramp(a, 44100, 44100 * 2) == 0 && write_ramp(b, 44100, 44100 * 3) == 0 &&
          write_ramp(c, 44100, 44100 * 3) == 0);
    deep_like_t dl = {.mu = PTHREAD_MUTEX_INITIALIZER, .last = -1, .q_at_join = -1};
    audio_output_t out = {dl_open, dl_write, dl_delay, dl_drain, dl_flush, dl_pause, dl_nop, dl_describe, &dl, NULL, NULL,
                          NULL, dl_join_depth};
    engine_t *e = engine_new(&out);
    engine_play(e, a, 1);
    WAIT(status(e).track_id == 1 && dl_delay(&dl) > dl.deep / 2, 1500);
    decoder_test_open_delay(600);                            /* from here on, a file takes 0.6 s to open */
    engine_queue_next(e, b, 2);
    WAIT(status(e).track_id == 2, 4000);
    CHECK(status(e).track_id == 2);
    WAIT(dl.q_at_join >= 0, 500);
    pthread_mutex_lock(&dl.mu);
    long qj = dl.q_at_join;
    int dry = dl.ran_dry;
    pthread_mutex_unlock(&dl.mu);
    CHECK(dry == 0);                                         /* no gap at the change */
    CHECK(qj > 0 && qj <= dl.enough);                        /* and the change itself as late as ever */
    /* the one opened ahead isn't used for another file: the queue changed
     * to c while b's successor was being waited for */
    engine_queue_next(e, c, 3);
    WAIT(status(e).track_id == 3, 6000);
    CHECK(status(e).track_id == 3);
    pthread_mutex_lock(&dl.mu);
    CHECK(dl.ran_dry == 0);
    pthread_mutex_unlock(&dl.mu);
    /* a play command while a file opens ahead: the new one plays, nothing leaks or hangs */
    engine_queue_next(e, a, 4);
    sleep_ms(2200);                                          /* (c decoded to its end by now: a is being opened ahead) */
    decoder_test_open_delay(0);
    engine_play(e, b, 5);
    WAIT(status(e).track_id == 5, 3000);
    CHECK(status(e).track_id == 5);
    engine_stop(e);
    engine_free(e);
    unlink(a);
    unlink(b);
    unlink(c);
}

/* ---- a FLAC whose frames aren't what its header says: stereo in the
 * header and its first frames, six channels after. The decoder's buffer is
 * sized by a frame's channels when it grows and read out by the header's:
 * six channels written into room for two. The track ends at the first such
 * frame. (A stereo file with a six-channel file's frames put after its own,
 * its length taken out of the header so they are reached.) ---- */
static long flac_frames_at(const unsigned char *b, long n)
{
    long at = 4;
    for (;;) {
        if (at + 4 > n) return -1;
        int last = b[at] & 0x80;
        at += 4 + ((long)b[at + 1] << 16 | (long)b[at + 2] << 8 | b[at + 3]);
        if (last) return at;
    }
}

static void test_flac_frame_lies(const char *dir)
{
    char a[300], six[300];
    snprintf(a, sizeof a, "%s/lies.flac", dir);
    snprintf(six, sizeof six, "%s/six.flac", dir);
    CHECK(write_ramp(a, 44100, 8192) == 0);
    FLAC__StreamEncoder *enc = FLAC__stream_encoder_new();
    FLAC__stream_encoder_set_channels(enc, 6);
    FLAC__stream_encoder_set_bits_per_sample(enc, 16);
    FLAC__stream_encoder_set_sample_rate(enc, 44100);
    CHECK(FLAC__stream_encoder_init_file(enc, six, NULL, NULL) == FLAC__STREAM_ENCODER_INIT_STATUS_OK);
    static FLAC__int32 pcm6[6 * 4096];
    for (unsigned i = 0; i < 6 * 4096; i++) pcm6[i] = (FLAC__int32)(i * 31 % 20000) - 10000;
    for (int k = 0; k < 4; k++) FLAC__stream_encoder_process_interleaved(enc, pcm6, 4096);
    FLAC__stream_encoder_finish(enc);
    FLAC__stream_encoder_delete(enc);
    static unsigned char fa[1 << 20], fb[1 << 20];
    FILE *f = fopen(a, "rb");
    long na = f ? (long)fread(fa, 1, sizeof fa, f) : 0;
    if (f) fclose(f);
    f = fopen(six, "rb");
    long nb = f ? (long)fread(fb, 1, sizeof fb, f) : 0;
    if (f) fclose(f);
    long fr = flac_frames_at(fb, nb);
    CHECK(na > 42 && nb > 42 && fr > 0 && fr < nb);
    fa[8 + 13] &= 0xF0;                                      /* STREAMINFO's total samples: unknown */
    memset(fa + 8 + 14, 0, 4);
    f = fopen(a, "wb");
    if (f) {
        fwrite(fa, 1, (size_t)na, f);
        if (fr > 0) fwrite(fb + fr, 1, (size_t)(nb - fr), f);
        fclose(f);
    }
    audio_format_t fmt;
    decoder_t *d = decoder_open_flags(a, &fmt, 0);
    CHECK(d != NULL && fmt.channels == 2);
    static int32_t out[4096 * 8];
    long total = 0, n = 0;
    for (int i = 0; d && i < 64 && (n = decoder_read(d, out, 4096)) > 0; i++) total += n;
    CHECK(total == 8192);                                    /* its own two frames, and no more */
    if (total != 8192) fprintf(stderr, "frames that lie: %ld frames read (last answer %ld)\n", total, n);
    if (d) decoder_close(d);
    unlink(a);
    unlink(six);
}

/* ---- a pause while a stream connects (an episode streamed, Play pressed
 * again before it sounds): it starts paused. The state is STOPPED all
 * through a connection, and the pause was dropped. (A connection that takes
 * 3 s: the decoder's test delay, given up when it's told to, as a real one.) ---- */
static void test_pause_while_connecting(const char *dir)
{
    char f[300];
    snprintf(f, sizeof f, "%s/conn.flac", dir);
    CHECK(write_ramp(f, 44100, 44100) == 0);
    deep_like_t dl = {.mu = PTHREAD_MUTEX_INITIALIZER, .last = -1, .q_at_join = -1};
    audio_output_t out = {dl_open, dl_write, dl_delay, dl_drain, dl_flush, dl_pause, dl_nop, dl_describe, &dl, NULL, NULL,
                          NULL, dl_join_depth};
    engine_t *e = engine_new(&out);
    decoder_test_open_delay(3000);
    engine_play(e, "http://127.0.0.1:9/episode.mp3", 1);
    WAIT(status(e).connecting, 2000);
    CHECK(status(e).connecting && status(e).starting && !status(e).start_paused);
    engine_pause(e, 1);
    CHECK(status(e).start_paused == 1);
    engine_pause(e, 0);
    CHECK(status(e).start_paused == 0);
    /* another asked for, to start paused, while this one connects: the
     * connection given up doesn't take that with it */
    engine_play_at(e, f, 2, 1.0f, 0, 1);
    decoder_test_open_delay(0);
    WAIT(status(e).track_id == 2 && status(e).state != ENG_STOPPED, 3000);
    CHECK(status(e).track_id == 2 && status(e).state == ENG_PAUSED);
    engine_stop(e);
    engine_free(e);
    unlink(f);
}

int main(void)
{
    char dir[200];
    test_tmp(dir, sizeof dir, "fathom-test-engine");
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; }
    test_paused_write(dir);
    test_open_ahead(dir);
    test_pause_while_connecting(dir);
    test_flac_frame_lies(dir);
    test_paused_stop_late(dir);
    test_open_fails(dir);
    test_pause_drops(dir);
    test_pause_at_the_end(dir);
    test_stall_drops(dir);
    test_play_at(dir);
    test_play_paused(dir);
    test_cut_write(dir);
    test_crossfeed(dir);
    test_crossfeed_keeps_place(dir);
    test_crossfeed_16bit_output(dir);
    test_mono(dir);
    test_flush_soft(dir);
    test_deep_queue(dir);
    signal(SIGPIPE, SIG_IGN);                     /* (a pipe's reader gone: an error, not the test's end) */
    test_slow_open(dir);
    rmdir(dir);
    if (failures) fprintf(stderr, "%d failure(s)\n", failures);
    else printf("engine: all checks passed\n");
    return failures != 0;
}
