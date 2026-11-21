/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Clock output: accepts audio at real-time speed and discards it. Stands in
 * for a sound card on the H2 until the ALSA backend exists, so playback
 * position, gapless track changes and play history behave as with sound.
 */
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "audio/output.h"

typedef struct {
    pthread_mutex_t lock;
    uint32_t rate;
    double   start;         /* monotonic time of frame 0 of this run */
    unsigned long long written;
    int      paused;
    double   paused_at;
} clock_out_t;

static clock_out_t g_clock = {.lock = PTHREAD_MUTEX_INITIALIZER};

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + t.tv_nsec / 1e9;
}

/* Frames the "listener" has heard so far. */
static unsigned long long heard(clock_out_t *c, double t)
{
    if (!c->rate) return c->written;
    double el = (c->paused ? c->paused_at : t) - c->start;
    unsigned long long h = el > 0 ? (unsigned long long)(el * c->rate) : 0;
    return h < c->written ? h : c->written;
}

static void restart(clock_out_t *c)
{
    c->start = now();
    c->written = 0;
}

static int c_open(void *ctx, const audio_format_t *fmt)
{
    clock_out_t *c = ctx;
    pthread_mutex_lock(&c->lock);
    c->rate = fmt->rate;
    restart(c);
    pthread_mutex_unlock(&c->lock);
    return 0;
}

static long c_write(void *ctx, const int32_t *f, long n)
{
    (void)f;
    clock_out_t *c = ctx;
    for (;;) {
        pthread_mutex_lock(&c->lock);
        double t = now();
        if (!c->paused && heard(c, t) >= c->written)
            restart(c);                                  /* ran dry: rebase the clock on now */
        long ahead = (long)(c->written - heard(c, t));
        int paused = c->paused;
        if (!paused && ahead < (long)c->rate / 4) {      /* keep at most 250 ms "buffered" */
            c->written += (unsigned long long)n;
            pthread_mutex_unlock(&c->lock);
            return n;
        }
        pthread_mutex_unlock(&c->lock);
        struct timespec ts = {0, 20 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
}

static long c_delay(void *ctx)
{
    clock_out_t *c = ctx;
    pthread_mutex_lock(&c->lock);
    long d = (long)(c->written - heard(c, now()));
    pthread_mutex_unlock(&c->lock);
    return d;
}

static void c_drain(void *ctx)
{
    long d;
    while ((d = c_delay(ctx)) > 0) {
        struct timespec ts = {0, 20 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
}

static void c_flush(void *ctx)
{
    clock_out_t *c = ctx;
    pthread_mutex_lock(&c->lock);
    restart(c);
    pthread_mutex_unlock(&c->lock);
}

static void c_pause(void *ctx, int on)
{
    clock_out_t *c = ctx;
    pthread_mutex_lock(&c->lock);
    double t = now();
    if (on && !c->paused) { c->paused = 1; c->paused_at = t; }
    else if (!on && c->paused) { c->paused = 0; c->start += t - c->paused_at; }
    pthread_mutex_unlock(&c->lock);
}

static void c_close(void *ctx) { c_flush(ctx); }
static void c_describe(void *ctx, char *out, int cap) { (void)ctx; snprintf(out, (size_t)cap, "silent (no sound card yet)"); }

void out_clock_create(audio_output_t *out)
{
    *out = (audio_output_t){c_open, c_write, c_delay, c_drain, c_flush, c_pause, c_close, c_describe, &g_clock, NULL, NULL};
}
