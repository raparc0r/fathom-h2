/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "engine.h"
#include "gain.h"

#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef __linux__
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

#define CHUNK_FRAMES 2048
#define STALL_MS 1500   /* a stream read waiting this long: "connecting" (it reconnects or buffers) */

static uint32_t mono_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    uint32_t ms = (uint32_t)((uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000);
    return ms ? ms : 1;
}
#define MAX_CH 8
#define OPEN_AGAIN 1    /* open_rc: the device was gone; try again at the resume */

typedef struct {
    char    path[1024];
    int64_t id;
    float   gain;      /* the track's own (volume levelling); 1.0 = none */
    uint32_t tag;      /* the caller's, for eng_status_t.tag */
    int     valid;
} pending_t;

struct engine {
    audio_output_t out;
    pthread_t thread;
    pthread_mutex_t mu;
    pthread_cond_t cv;

    /* commands (under mu) */
    pending_t cmd_play;
    pending_t next[ENGINE_AHEAD];   /* lined up to follow, in order */
    int n_next;
    int cmd_seek, quit;
    uint32_t seek_ms;

    /* state */
    eng_state_t state;
    decoder_t *dec;
    audio_format_t fmt;        /* current track */
    audio_format_t out_fmt;    /* what the output is open for */
    int out_open;
    int open_rc;               /* the output's last open(): 0, -1 (not this format), -2 (device gone) */
    int64_t track_id;
    uint32_t tag;              /* the current track's, as lined up (engine_queue_ahead) */
    uint32_t seq;
    uint64_t pos_frames;       /* frames of the current track written */
    int ended, error;
    unsigned stop_seq;         /* +1 each time playback stops by itself (ended, or a track failed) */
    int rewind;                /* paused, and the output dropped what it had: at the resume, */
    uint64_t rewind_to;        /* ...the decoder goes back here (what was heard) */
    float gain;                /* volume (a USB DAC without its own control) */
    float fade;                /* the sleep timer's fade-out; applied on top of gain */
    float track_gain;          /* the playing track's levelling; changes exactly at its first sample */
    float bal[2];              /* the balance: left, right (1 = untouched) */
    int writing;               /* the thread is in out.write/drain without mu */
    int hold;                  /* engine_switch_output() in progress: no new writes */
    int out_lost;              /* a write failed (device gone): paused */
    unsigned cut;              /* +1 when another thread drops the output's queue: a write
                                * failing meanwhile is that, not a device gone */
    int start_paused;          /* paused while a play command was still pending */
    eq_profile_t eq_prof;      /* the EQ wanted (engine_set_eq) */
    int eq_on;
    unsigned eq_gen, eq_built; /* changes asked for, and the one built */
    eq_t *eq;                  /* the filters for the current track (engine thread only) */
    int eq_in_out;             /* the output runs the EQ itself (set_eq) */
    int dop;                   /* DSD goes out as DoP (engine_set_dop) */
    char path[1024];           /* the current track's */
    /* internet streams */
    volatile int net_abort;    /* set (under mu) to make a stream's open or read give up */
    int connecting;            /* a stream is being opened, without mu */
    int dec_net;               /* the current track is a stream */
    volatile uint32_t net_read_ms;  /* a stream read under way since (monotonic ms, 0 = none) */
    unsigned meta_seq, dec_meta;  /* ours (for eng_status_t), and the decoder's last seen */
    char meta[256], meta_url[512];

    int32_t buf[CHUNK_FRAMES * MAX_CH];
};

/* How deep a sample format the output was opened for: 0 = up to 16 bits
 * (S16_LE first), 1 = up to 24 (lossy sources too), 2 = more. */
static int depth_class(const audio_format_t *f)
{
    return f->bits && f->bits <= 16 ? 0 : f->bits > 24 ? 2 : 1;
}

/* Can the open output (opened for `a`) carry `b` bit for bit? Same rate and
 * channels, and not deeper than it was opened for: a 24-bit track after a
 * 16-bit one at the same rate would otherwise go out cut to 16 bits. (A
 * shallower track on a deeper output is exact, and stays gapless.) */
static int same_out_format(const audio_format_t *a, const audio_format_t *b)
{
    return a->rate == b->rate && a->channels == b->channels && depth_class(b) <= depth_class(a) &&
           a->stream == b->stream;    /* (a stream's output buffers more) */
}

/* What the output opens for, for a track of format f: when the samples are
 * changed (the EQ, a gain: levelling, a software volume), at least 24 bits, or
 * the result would lose its low bits on a 16-bit output. Untouched, the track's
 * own depth (bit-perfect). */
static audio_format_t out_format(const engine_t *e, const audio_format_t *f)
{
    audio_format_t o = *f;
    o.stream = (uint8_t)e->dec_net;
    if (f->dop) return o;                      /* DoP: never changed, 24 bits already */
    int changed = (e->eq_on && !e->eq_in_out) || e->gain * e->track_gain != 1.0f ||
                  (f->channels == 2 && (e->bal[0] != 1.0f || e->bal[1] != 1.0f));
    if (changed && o.bits && o.bits < 24) o.bits = 24;
    return o;
}

/* Called with mu held: opens the output for `of` (paused if the engine is).
 * open_rc keeps why it couldn't: -2 the device isn't there, else the format. */
static void out_open_for(engine_t *e, const audio_format_t *of)
{
    e->open_rc = e->out.open(e->out.ctx, of);
    e->out_open = e->open_rc == 0;
    e->out_fmt = *of;
    if (e->out_open && e->state == ENG_PAUSED) e->out.pause(e->out.ctx, 1);
}

/* Called with mu held, on the engine thread: the EQ's filters follow the
 * profile and the track's format; the output gets deep enough for them. */
static void eq_sync(engine_t *e)
{
    if (e->eq_built != e->eq_gen) {             /* a new profile, or another output: ask it first */
        e->eq_built = e->eq_gen;
        e->eq_in_out = e->out.set_eq && e->out.set_eq(e->out.ctx, e->eq_on ? &e->eq_prof : NULL);
        eq_destroy(e->eq);
        e->eq = NULL;
    }
    int want = e->eq_on && !e->eq_in_out && !e->fmt.dop && e->fmt.rate && e->fmt.channels <= EQ_MAX_CH;
    if (!want) {
        if (e->eq) { eq_destroy(e->eq); e->eq = NULL; }
        return;
    }
    if (e->eq && eq_rate(e->eq) == e->fmt.rate && eq_channels(e->eq) == e->fmt.channels) return;
    eq_destroy(e->eq);
    e->eq = eq_create(&e->eq_prof, e->fmt.rate, e->fmt.channels);
    audio_format_t o = out_format(e, &e->fmt);
    if (e->out_open && !same_out_format(&e->out_fmt, &o)) {   /* EQ on, a 16-bit output: deeper */
        e->out.flush(e->out.ctx);
        e->out.close(e->out.ctx);
        out_open_for(e, &o);
    }
}

/* Called with mu held: the track that's on can't be played. Stopped, with an
 * error, so the player moves on past it. */
static void failed(engine_t *e)
{
    e->pos_frames = 0;
    e->rewind = 0;
    e->state = ENG_STOPPED;
    e->error = 1;
    e->ended = 1;
    e->stop_seq++;
}

/* Called with mu held: `p` couldn't be opened (gone, damaged, not a format the
 * H2 decodes). It's reported as the track that's on, failed. */
static void track_failed(engine_t *e, const pending_t *p)
{
    fprintf(stderr, "engine: can't open %s\n", p->path);
    e->track_id = p->id;
    e->tag = p->tag;
    e->seq++;
    failed(e);
}

/* Called with mu held. Opens `p` (or takes `d`, already opened, with its format
 * in *df); returns 0 and makes it current (the output may not have opened for
 * it: see run()), or -1. */
static int start_track(engine_t *e, const pending_t *p, int gapless, decoder_t *d, const audio_format_t *df)
{
    audio_format_t f;
    /* DoP only while nothing would change the samples (a USB DAC with its own
     * volume): a gain on DoP would be noise at full scale */
    if (d) f = *df;
    else d = decoder_open_flags(p->path, &f, e->dop && e->gain == 1.0f ? DECODER_DOP : 0);
    if (!d || f.channels == 0 || f.channels > MAX_CH) {
        if (d && d != e->dec) decoder_close(d);
        e->error = 1;
        return -1;
    }
    if (e->dec && e->dec != d) {                     /* (the same: a CUE track carrying on) */
        if (gapless) decoder_join(d, e->dec);       /* DSD: the filter carries on across the join */
        decoder_close(e->dec);
    }
    e->dec = d;
    snprintf(e->path, sizeof e->path, "%s", p->path);
    e->dec_net = decoder_is_stream(p->path);
    e->meta[0] = e->meta_url[0] = '\0';
    e->meta_seq++;
    e->dec_meta = 0;
    e->fmt = f;
    e->track_id = p->id;
    e->tag = p->tag;
    e->track_gain = p->gain;
    e->pos_frames = 0;
    e->rewind = 0;
    e->seq++;
    e->error = 0;
    e->ended = 0;

    audio_format_t of = out_format(e, &f);
    if (!e->out_open || !same_out_format(&e->out_fmt, &of)) {
        if (e->out_open) {
            if (gapless) e->out.drain(e->out.ctx);   /* let the previous track finish */
            else e->out.flush(e->out.ctx);
        }
        out_open_for(e, &of);
    } else if (!gapless) {
        e->out.flush(e->out.ctx);                    /* same format: just drop queued audio */
    }
    if (!gapless) eq_reset(e->eq);                   /* (gapless: the filters carry on) */
    eq_sync(e);
    return 0;
}


/* Playback first in line for the CPU: decoding and writing a chunk takes a
 * few ms, but on a busy H2 (the UI scrolling, the network drive serving a
 * download) a thread among the others can wait longer than the output's
 * buffer lasts, and that's a gap. SCHED_FIFO at a modest priority (below the
 * kernel's interrupt threads, 50); the kernel's real-time throttling leaves the
 * rest of the system 5 % of every second whatever happens. Without the right
 * to it, nice -10; without that either, as it was. Per thread: never the UI's,
 * and not for what it starts (bluealsa-cli, fathom-avrcp: SCHED_RESET_ON_FORK).
 * FATHOM_NO_RT in the environment: as it was (tools/h2-tests.sh: a test's
 * playback thread mustn't compete with the player's, nor starve the test's). */
void audio_thread_priority(int on)
{
#ifdef __linux__
    static int said;
    if (on && getenv("FATHOM_NO_RT")) return;
    struct sched_param sp = {.sched_priority = on ? 10 : 0};
#ifndef SCHED_RESET_ON_FORK
#define SCHED_RESET_ON_FORK 0x40000000
#endif
    int err = pthread_setschedparam(pthread_self(), on ? SCHED_FIFO | SCHED_RESET_ON_FORK : SCHED_OTHER, &sp);
    if (!err) {
        if (on && !said++) fprintf(stderr, "engine: playback thread at SCHED_FIFO %d\n", sp.sched_priority);
        if (on) return;
    }
    if (setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), on ? -10 : 0) == 0) {
        if (on && !said++) fprintf(stderr, "engine: playback thread at nice -10 (no SCHED_FIFO: %s)\n", strerror(err));
    } else if (on && !said++) {
        fprintf(stderr, "engine: playback thread at normal priority (%s)\n", strerror(errno));
    }
#else
    (void)on;
#endif
}

/* Called with mu held. A write on a paused output can wait for ever (a paused
 * ALSA PCM takes no more): before a command the thread must take up (a new
 * track, a seek), drop what's queued and unpause the output, so it returns. */
static void unblock(engine_t *e)
{
    if (!e->writing || e->state == ENG_PLAYING || !e->out_open) return;
    e->cut++;
    e->out.flush(e->out.ctx);
    e->out.pause(e->out.ctx, 0);
}

static void *run(void *arg)
{
    engine_t *e = arg;
    audio_thread_priority(1);
    pthread_mutex_lock(&e->mu);
    while (!e->quit) {
        if (e->cmd_play.valid) {
            pending_t p = e->cmd_play;
            e->cmd_play.valid = 0;
            decoder_t *nd = NULL;
            audio_format_t nf;
            if (decoder_is_stream(p.path)) {
                /* Connecting takes seconds: not with mu held. Whatever is playing
                 * stops meanwhile (a stream's connection is dropped). */
                if (e->dec) {
                    if (e->out_open) e->out.flush(e->out.ctx);
                    decoder_close(e->dec);
                    e->dec = NULL;
                    e->dec_net = 0;
                }
                e->state = ENG_STOPPED;
                e->net_abort = 0;
                e->connecting = 1;
                e->meta[0] = e->meta_url[0] = '\0';
                e->meta_seq++;
                pthread_mutex_unlock(&e->mu);
                /* Connecting (TLS, probing the stream) is seconds of CPU with
                 * nothing to play yet: not ahead of the UI. */
                audio_thread_priority(0);
                nd = decoder_open_ex(p.path, &nf, &e->net_abort);
                audio_thread_priority(1);
                pthread_mutex_lock(&e->mu);
                e->connecting = 0;
                if (e->cmd_play.valid || e->net_abort || e->quit) {   /* replaced or stopped meanwhile */
                    if (nd) decoder_close(nd);
                    e->start_paused = 0;
                    continue;
                }
                if (!nd) {
                    e->error = 1;
                    e->state = ENG_STOPPED;
                    e->start_paused = 0;
                    continue;
                }
            }
            if (start_track(e, &p, 0, nd, &nf) == 0) {
                e->state = e->start_paused ? ENG_PAUSED : ENG_PLAYING;
                if (e->out_open) e->out.pause(e->out.ctx, e->start_paused);
            } else {
                if (e->out_open) e->out.flush(e->out.ctx);   /* (what played before stops too) */
                track_failed(e, &p);
            }
            e->start_paused = 0;
            continue;
        }
        if (e->cmd_seek && e->dec) {
            e->cmd_seek = 0;
            uint64_t frame = (uint64_t)e->seek_ms * e->fmt.rate / 1000;
            if (decoder_seek(e->dec, frame) == 0) {
                e->out.flush(e->out.ctx);
                eq_reset(e->eq);
                e->pos_frames = frame;
                e->rewind = 0;
            }
            if (e->state == ENG_PAUSED && e->out_open) e->out.pause(e->out.ctx, 1);   /* (unblock() unpaused it) */
            continue;
        }
        if (e->rewind && e->state == ENG_PLAYING && e->dec) {
            /* resumed on an output that dropped its queue at the pause: from
             * where it was heard to stop, not from where the decoder got to */
            e->rewind = 0;
            if (decoder_seek(e->dec, e->rewind_to) == 0) {
                e->out.flush(e->out.ctx);
                eq_reset(e->eq);
                e->pos_frames = e->rewind_to;
            }
            continue;
        }
        if (e->state == ENG_STOPPED && e->dec && e->dec_net && !e->writing) {
            decoder_close(e->dec);              /* a stopped stream: hang up */
            e->dec = NULL;
            e->dec_net = 0;
            continue;
        }
        if (e->state == ENG_PLAYING && e->dec && !e->out_open && !e->hold) {
            /* The output didn't open for this track (as it started, after a
             * change of output or of EQ). Gone (a USB DAC unplugged, the
             * Bluetooth link down): paused, as when a write fails, and tried
             * again at the resume. Not a format it takes (a 22 kHz MP3 over
             * Bluetooth, 5.1): the track fails, and the player moves on.
             * Only while playing: paused, nothing starts by itself. */
            if (e->open_rc == OPEN_AGAIN) {
                audio_format_t of = out_format(e, &e->fmt);
                out_open_for(e, &of);
            } else if (e->open_rc == -2) {
                e->state = ENG_PAUSED;
                e->out_lost = 1;
                e->open_rc = OPEN_AGAIN;
            } else {
                fprintf(stderr, "engine: no output for %s\n", e->path);
                decoder_close(e->dec);
                e->dec = NULL;
                e->dec_net = 0;
                failed(e);
            }
            continue;
        }
        if (e->hold || e->state != ENG_PLAYING || !e->dec || !e->out_open) {
            pthread_cond_wait(&e->cv, &e->mu);
            continue;
        }

        eq_sync(e);                                  /* the EQ changed meanwhile */
        decoder_t *dec = e->dec;
        eq_t *eq = e->eq;
        int ch = e->fmt.channels;
        float gain = e->fmt.dop ? 1.0f : e->gain * e->fade * e->track_gain;
        float bl = ch == 2 && !e->fmt.dop ? e->bal[0] : 1.0f, br = ch == 2 && !e->fmt.dop ? e->bal[1] : 1.0f;
        long w = 0;
        unsigned cut = e->cut;
        e->writing = 1;
        int net = e->dec_net;
        decoder_set_next(dec, e->n_next && !decoder_is_stream(e->next[0].path) ? e->next[0].path : NULL);
        pthread_mutex_unlock(&e->mu);
        if (net) e->net_read_ms = mono_ms();
        long n = decoder_read(dec, e->buf, CHUNK_FRAMES);
        e->net_read_ms = 0;
        if (n > 0) {
            if (bl != 1.0f || br != 1.0f) gain_run2(e->buf, n, gain_q28(gain * bl), gain_q28(gain * br));
            else if (gain != 1.0f) gain_run(e->buf, n * ch, gain_q28(gain));
            if (eq) eq_run(eq, e->buf, n);
            /* a stop, a skip or a seek while it decoded: the old track's, not written */
            pthread_mutex_lock(&e->mu);
            int old = e->cut != cut;
            pthread_mutex_unlock(&e->mu);
            if (!old) w = e->out.write(e->out.ctx, e->buf, n);
        }
        pthread_mutex_lock(&e->mu);
        e->writing = 0;
        /* A cut while it wrote: the other thread's flush dropped what was
         * queued, but a write it woke can still put the rest of its chunk in
         * after (ALSA's, woken once the PCM is prepared again: it starts and
         * plays it; after a stop nothing would drop it). Dropped here, where
         * no write is under way. */
        if (e->cut != cut && e->out_open && !e->hold) e->out.flush(e->out.ctx);
        if (e->hold) pthread_cond_broadcast(&e->cv);
        if (dec != e->dec) continue;   /* a play command replaced the track meanwhile */
        if (e->dec_net) {
            char m[sizeof e->meta], u[sizeof e->meta_url];
            unsigned ms = decoder_meta(dec, m, sizeof m, u, sizeof u);
            if (ms != e->dec_meta) {
                e->dec_meta = ms;
                snprintf(e->meta, sizeof e->meta, "%s", m);
                snprintf(e->meta_url, sizeof e->meta_url, "%s", u);
                e->meta_seq++;
            }
        }
        if (w < 0 && !e->hold && e->cut == cut) {   /* the device went away (USB DAC unplugged) */
            e->state = ENG_PAUSED;
            e->out_lost = 1;
            /* What the output had queued went with it (a Bluetooth link that
             * stalled: about a second, which it still reports as its delay):
             * at the resume, from what was heard, as after a pause. */
            long lost = e->out_open ? e->out.delay(e->out.ctx) : 0;
            if (lost > (long)e->fmt.rate / 20 && !e->dec_net && !e->rewind) {
                e->rewind = 1;
                e->rewind_to = e->pos_frames > (uint64_t)lost ? e->pos_frames - (uint64_t)lost : 0;
            }
            continue;
        }
        if (n > 0) {
            e->pos_frames += (uint64_t)n;
            continue;
        }
        /* end of track: gapless successor, or stop (a play asked for meanwhile comes first) */
        pending_t bad = {.valid = 0};
        if (e->n_next && !e->cmd_play.valid) {
            pending_t p = e->next[0];
            audio_format_t cf;
            memmove(&e->next[0], &e->next[1], (size_t)--e->n_next * sizeof e->next[0]);
            /* the next CUE track of the same file: the decoder just carries on */
            if (decoder_continue(dec, p.path, &cf) == 0 && start_track(e, &p, 1, dec, &cf) == 0) continue;
            if (!decoder_is_stream(p.path) && start_track(e, &p, 1, NULL, NULL) == 0) continue;
            bad = p;                                 /* it can't be played: this one plays out first */
        }
        e->writing = 1;
        pthread_mutex_unlock(&e->mu);
        e->out.drain(e->out.ctx);
        pthread_mutex_lock(&e->mu);
        e->writing = 0;
        if (e->hold) pthread_cond_broadcast(&e->cv);
        if (dec == e->dec && !e->cmd_play.valid && e->state != ENG_STOPPED) {
            /* A next track queued while it drained: on to it (the end comes
             * round again). A track shorter than the output's buffer ends
             * before the player has queued its successor: on the H2's one
             * core this thread (real-time) gets there first, and it stopped. */
            if (!bad.valid && e->n_next) continue;
            if (bad.valid) {
                track_failed(e, &bad);
            } else {
                e->state = ENG_STOPPED;
                e->ended = 1;
                e->stop_seq++;
            }
        }
    }
    pthread_mutex_unlock(&e->mu);
    return NULL;
}

engine_t *engine_new(const audio_output_t *out)
{
    engine_t *e = calloc(1, sizeof *e);
    if (!e) return NULL;
    e->out = *out;
    e->gain = 1.0f;
    e->fade = 1.0f;
    e->track_gain = 1.0f;
    e->bal[0] = e->bal[1] = 1.0f;
    /* Priority inheritance: the UI thread holding the lock (engine_status)
     * runs at the playback thread's priority until it lets go, so a busy H2
     * can't hold playback up behind it. */
    pthread_mutexattr_t ma;
    pthread_mutexattr_init(&ma);
#if defined(__linux__) && defined(PTHREAD_PRIO_INHERIT)
    pthread_mutexattr_setprotocol(&ma, PTHREAD_PRIO_INHERIT);
#endif
    if (pthread_mutex_init(&e->mu, &ma)) pthread_mutex_init(&e->mu, NULL);
    pthread_mutexattr_destroy(&ma);
    pthread_cond_init(&e->cv, NULL);
    if (pthread_create(&e->thread, NULL, run, e)) { free(e); return NULL; }
    return e;
}

void engine_free(engine_t *e)
{
    if (!e) return;
    pthread_mutex_lock(&e->mu);
    e->quit = 1;
    e->net_abort = 1;
    pthread_cond_signal(&e->cv);
    pthread_mutex_unlock(&e->mu);
    /* The thread may be blocked in out.write() waiting for paused audio to
     * drain: unpause and drop the queue so the write returns. */
    if (e->out_open) {
        e->out.pause(e->out.ctx, 0);
        e->out.flush(e->out.ctx);
    }
    pthread_join(e->thread, NULL);
    if (e->dec) decoder_close(e->dec);
    if (e->out_open) e->out.close(e->out.ctx);
    eq_destroy(e->eq);
    pthread_mutex_destroy(&e->mu);
    pthread_cond_destroy(&e->cv);
    free(e);
}

static void set_pending(pending_t *p, const char *path, int64_t id, float gain, uint32_t tag)
{
    snprintf(p->path, sizeof p->path, "%s", path);
    p->id = id;
    p->gain = gain;
    p->tag = tag;
    p->valid = 1;
}

void engine_play(engine_t *e, const char *path, int64_t id) { engine_play_gain(e, path, id, 1.0f); }
void engine_queue_next(engine_t *e, const char *path, int64_t id) { engine_queue_next_gain(e, path, id, 1.0f); }

void engine_play_gain(engine_t *e, const char *path, int64_t id, float gain)
{
    pthread_mutex_lock(&e->mu);
    set_pending(&e->cmd_play, path, id, gain, 0);
    unblock(e);        /* (paused: the thread may be waiting in a write) */
    e->ended = e->error = 0;   /* (they were the last track's) */
    e->start_paused = 0;
    e->n_next = 0;
    e->cmd_seek = 0;   /* (a seek still pending was for the old track; a resume seeks after this) */
    if (e->connecting || e->dec_net) e->net_abort = 1;   /* stop waiting on the network */
    pthread_cond_signal(&e->cv);
    pthread_mutex_unlock(&e->mu);
}

void engine_queue_next_gain(engine_t *e, const char *path, int64_t id, float gain)
{
    engine_track_t t = {path, id, gain, 0};
    engine_queue_ahead(e, &t, path ? 1 : 0);
}

void engine_queue_ahead(engine_t *e, const engine_track_t *t, int n)
{
    if (n > ENGINE_AHEAD) n = ENGINE_AHEAD;
    pthread_mutex_lock(&e->mu);
    for (int i = 0; i < n; i++) set_pending(&e->next[i], t[i].path, t[i].id, t[i].gain, t[i].tag);
    e->n_next = n;
    pthread_mutex_unlock(&e->mu);
}

void engine_pause(engine_t *e, int on)
{
    pthread_mutex_lock(&e->mu);
    if (on && e->cmd_play.valid) {
        e->start_paused = 1;            /* the track about to start starts paused */
    } else if (!on && e->cmd_play.valid) {
        e->start_paused = 0;
    } else if (on && e->state == ENG_PLAYING) {
        e->state = ENG_PAUSED;
        long before = e->out_open ? e->out.delay(e->out.ctx) : 0;
        e->out.pause(e->out.ctx, 1);
        long after = e->out_open ? e->out.delay(e->out.ctx) : 0;
        /* An output that drops what's queued as it pauses (Bluetooth ends the
         * stream: up to a second) would resume past what was heard: go back
         * to it at the resume. (Less than 50 ms isn't worth a seek.) */
        if (before - after > (long)e->fmt.rate / 20 && e->dec && !e->dec_net && !e->rewind) {
            e->rewind = 1;
            e->rewind_to = e->pos_frames > (uint64_t)before ? e->pos_frames - (uint64_t)before : 0;
        }
    } else if (!on && e->state == ENG_PAUSED) {
        e->state = ENG_PLAYING;
        e->out.pause(e->out.ctx, 0);
        pthread_cond_signal(&e->cv);
    }
    pthread_mutex_unlock(&e->mu);
}

void engine_stop(engine_t *e)
{
    pthread_mutex_lock(&e->mu);
    e->n_next = 0;
    e->cmd_play.valid = 0;
    if (e->connecting || e->dec_net) e->net_abort = 1;
    if (e->out_open) {
        e->cut++;                       /* a write this cuts short isn't an unplugged device */
        e->out.flush(e->out.ctx);
        if (e->state == ENG_PAUSED) e->out.pause(e->out.ctx, 0);   /* (a write waiting on it returns) */
    }
    e->state = ENG_STOPPED;
    pthread_cond_signal(&e->cv);
    pthread_mutex_unlock(&e->mu);
}

void engine_seek_ms(engine_t *e, uint32_t ms)
{
    pthread_mutex_lock(&e->mu);
    e->seek_ms = ms;
    e->cmd_seek = 1;
    unblock(e);        /* (paused: the thread may be waiting in a write) */
    pthread_cond_signal(&e->cv);
    pthread_mutex_unlock(&e->mu);
}

void engine_set_gain(engine_t *e, float gain)
{
    pthread_mutex_lock(&e->mu);
    e->gain = gain < 0 ? 0 : gain;
    pthread_mutex_unlock(&e->mu);
}

void engine_describe_output(engine_t *e, char *out, int cap)
{
    pthread_mutex_lock(&e->mu);
    if (e->out_open && e->out.describe) e->out.describe(e->out.ctx, out, cap);
    else if (cap > 0) out[0] = '\0';
    pthread_mutex_unlock(&e->mu);
}

void engine_set_balance(engine_t *e, float left, float right)
{
    pthread_mutex_lock(&e->mu);
    e->bal[0] = left < 0 ? 0 : left > 1 ? 1 : left;
    e->bal[1] = right < 0 ? 0 : right > 1 ? 1 : right;
    pthread_mutex_unlock(&e->mu);
}

void engine_set_eq(engine_t *e, const eq_profile_t *p)
{
    pthread_mutex_lock(&e->mu);
    e->eq_on = p != NULL;
    if (p) e->eq_prof = *p;
    e->eq_gen++;
    pthread_cond_signal(&e->cv);
    pthread_mutex_unlock(&e->mu);
}

void engine_set_dop(engine_t *e, int on)
{
    pthread_mutex_lock(&e->mu);
    e->dop = !!on;
    int now = !on && e->dec && e->fmt.dop;
    pthread_mutex_unlock(&e->mu);
    if (now) engine_switch_output(e, NULL, NULL);   /* a DoP track carries on as PCM */
}

/* Called with mu held, the thread out of the way, the output closed: the DoP
 * track carries on as PCM from where it was, or stops. DoP reaching a DAC that
 * doesn't know it is noise at full scale. */
static void dop_to_pcm(engine_t *e)
{
    audio_format_t f;
    decoder_t *d = decoder_open_flags(e->path, &f, 0);
    uint64_t at = d ? e->pos_frames * f.rate / e->fmt.rate : 0;
    if (d && decoder_seek(d, at) == 0) {
        decoder_close(e->dec);
        e->dec = d;
        e->rewind_to = e->rewind_to * f.rate / e->fmt.rate;
        e->fmt = f;
        e->pos_frames = at;
        return;
    }
    if (d) decoder_close(d);
    decoder_close(e->dec);
    e->dec = NULL;
    e->state = ENG_STOPPED;
    e->error = 1;
}

void engine_set_fade(engine_t *e, float fade)
{
    pthread_mutex_lock(&e->mu);
    e->fade = fade < 0 ? 0 : fade > 1 ? 1 : fade;
    pthread_mutex_unlock(&e->mu);
}

void engine_switch_output(engine_t *e, void (*change)(void *out_ctx, void *arg), void *arg)
{
    pthread_mutex_lock(&e->mu);
    e->hold = 1;
    if (e->writing && e->out_open) {
        /* A write may be blocked on a paused stream: drop the queue so it
         * returns (the audio is being moved to another device anyway). */
        e->out.pause(e->out.ctx, 0);
        e->out.flush(e->out.ctx);
    }
    while (e->writing) pthread_cond_wait(&e->cv, &e->mu);
    if (e->out_open) e->out.close(e->out.ctx);
    e->out_open = 0;
    if (change) change(e->out.ctx, arg);
    e->dop = 0;                                 /* until the new output's owner says it takes DoP */
    if (e->dec && e->fmt.dop) dop_to_pcm(e);
    e->eq_in_out = 0;                           /* the new output is asked at the next chunk */
    e->eq_gen++;
    if (e->dec) {
        audio_format_t of = out_format(e, &e->fmt);
        out_open_for(e, &of);                   /* (if it can't: see run()) */
    }
    e->out_lost = 0;
    e->hold = 0;
    pthread_cond_broadcast(&e->cv);
    pthread_mutex_unlock(&e->mu);
}

void engine_status(engine_t *e, eng_status_t *st)
{
    pthread_mutex_lock(&e->mu);
    memset(st, 0, sizeof *st);
    st->state = e->state;
    st->seq = e->seq;
    st->track_id = e->track_id;
    st->fmt = e->fmt;
    st->next_queued = e->n_next;
    st->tag = e->tag;
    st->ended = e->ended;
    st->error = e->error;
    st->stop_seq = e->stop_seq;
    st->output_lost = e->out_lost;
    st->starting = e->cmd_play.valid;
    st->start_paused = e->start_paused;
    st->eq = e->eq_on && !e->fmt.dop;
    st->stream = e->connecting || e->dec_net;
    uint32_t since = e->net_read_ms;
    st->connecting = e->connecting || (e->dec_net && since && mono_ms() - since > STALL_MS);
    st->starting |= e->connecting;
    st->meta_seq = e->meta_seq;
    snprintf(st->meta, sizeof st->meta, "%s", e->meta);
    snprintf(st->meta_url, sizeof st->meta_url, "%s", e->meta_url);
    st->out_fmt = e->out_fmt;
    st->gain = e->gain;
    st->track_gain = e->track_gain;
    int stereo = e->fmt.channels == 2;
    st->balance[0] = stereo ? e->bal[0] : 1.0f;
    st->balance[1] = stereo ? e->bal[1] : 1.0f;
    st->untouched = e->fmt.dop || (!e->eq_on && e->gain * e->track_gain == 1.0f && e->fade == 1.0f &&
                                   st->balance[0] == 1.0f && st->balance[1] == 1.0f);
    if (e->fmt.rate) {
        long delay = e->out_open ? e->out.delay(e->out.ctx) : 0;
        int64_t heard = e->rewind ? (int64_t)e->rewind_to : (int64_t)e->pos_frames - delay;
        if (heard < 0) heard = 0;
        st->pos_ms = (uint32_t)(heard * 1000 / e->fmt.rate);
        st->len_ms = (uint32_t)(e->fmt.total_frames * 1000 / e->fmt.rate);
    }
    pthread_mutex_unlock(&e->mu);
}
