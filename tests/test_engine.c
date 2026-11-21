/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * The engine against outputs that behave as the H2's do: one whose write waits
 * while it's paused (ALSA: a paused USB DAC takes no more), one that can't open
 * some formats (Bluetooth: 22 kHz; 5.1), one that drops what's queued when it
 * pauses (Bluetooth ends the stream). Generated FLAC files; nothing is heard.
 */
#include <FLAC/stream_encoder.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

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
    int late;                     /* a write waiting wakes only after the flush's PREPARE:
                                     its chunk goes into the fresh buffer (ALSA can) */
    unsigned flushes;
    long queued;                  /* frames written since the last flush (what would play) */
    unsigned long long frames;
    unsigned only_rate;           /* opens only this rate (0: any) */
    int gone;                     /* unplugged: open() says so */
    int opens;
    int watch;                    /* from al_watch(): the earliest track frame written, and how many */
    long first_frame;
    unsigned long long watched;
} alsa_like_t;

static int al_open(void *ctx, const audio_format_t *f)
{
    alsa_like_t *a = ctx;
    a->opens++;
    return a->gone ? -2 : a->only_rate && f->rate != a->only_rate ? -1 : 0;
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
static void al_drain(void *ctx) { (void)ctx; sleep_ms(50); }

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

static void al_describe(void *ctx, char *o, int cap) { (void)ctx; snprintf(o, (size_t)cap, "alsa-like"); }

static void al_create(audio_output_t *out, alsa_like_t *a)
{
    memset(a, 0, sizeof *a);
    pthread_mutex_init(&a->mu, NULL);
    pthread_cond_init(&a->cv, NULL);
    *out = (audio_output_t){al_open, al_write, al_delay, al_drain, al_flush, al_pause, al_nop, al_describe, a, NULL};
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
        engine_play_at(e, a, 2, 1.0f, 3000);
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
    audio_output_t out = {bl_open, bl_write, bl_delay, bl_nop, bl_flush, bl_pause, bl_nop, bl_describe, &bl, NULL};
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
    audio_output_t out = {bl_open, bl_write, bl_delay, bl_nop, bl_flush, bl_pause, bl_nop, bl_describe, &bl, NULL};
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

int main(void)
{
    char dir[200];
    test_tmp(dir, sizeof dir, "fathom-test-engine");
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; }
    test_paused_write(dir);
    test_paused_stop_late(dir);
    test_open_fails(dir);
    test_pause_drops(dir);
    test_stall_drops(dir);
    test_play_at(dir);
    rmdir(dir);
    if (failures) fprintf(stderr, "%d failure(s)\n", failures);
    else printf("engine: all checks passed\n");
    return failures != 0;
}
