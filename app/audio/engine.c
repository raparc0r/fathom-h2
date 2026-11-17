/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "engine.h"
#include "gain.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

typedef struct {
    char    path[1024];
    int64_t id;
    float   gain;      /* the track's own (volume levelling); 1.0 = none */
    int     valid;
} pending_t;

struct engine {
    audio_output_t out;
    pthread_t thread;
    pthread_mutex_t mu;
    pthread_cond_t cv;

    /* commands (under mu) */
    pending_t cmd_play, next;
    int cmd_seek, quit;
    uint32_t seek_ms;

    /* state */
    eng_state_t state;
    decoder_t *dec;
    audio_format_t fmt;        /* current track */
    audio_format_t out_fmt;    /* what the output is open for */
    int out_open;
    int64_t track_id;
    uint32_t seq;
    uint64_t pos_frames;       /* frames of the current track written */
    int ended, error;
    float gain;                /* volume (a USB DAC without its own control) */
    float fade;                /* the sleep timer's fade-out; applied on top of gain */
    float track_gain;          /* the playing track's levelling; changes exactly at its first sample */
    float bal[2];              /* the balance: left, right (1 = untouched) */
    int writing;               /* the thread is in out.write/drain without mu */
    int hold;                  /* engine_switch_output() in progress: no new writes */
    int out_lost;              /* a write failed (device gone): paused */
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
    return a->rate == b->rate && a->channels == b->channels && depth_class(b) <= depth_class(a);
}

/* What the output opens for, for a track of format f: when the samples are
 * changed (the EQ, a gain: levelling, a software volume), at least 24 bits, or
 * the result would lose its low bits on a 16-bit output. Untouched, the track's
 * own depth (bit-perfect). */
static audio_format_t out_format(const engine_t *e, const audio_format_t *f)
{
    audio_format_t o = *f;
    if (f->dop) return o;                      /* DoP: never changed, 24 bits already */
    int changed = (e->eq_on && !e->eq_in_out) || e->gain * e->track_gain != 1.0f ||
                  (f->channels == 2 && (e->bal[0] != 1.0f || e->bal[1] != 1.0f));
    if (changed && o.bits && o.bits < 24) o.bits = 24;
    return o;
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
        e->out_open = e->out.open(e->out.ctx, &o) == 0;
        e->out_fmt = o;
        if (e->state == ENG_PAUSED) e->out.pause(e->out.ctx, 1);
    }
}

/* Called with mu held. Opens `p` (or takes `d`, already opened, with its format
 * in *df); returns 0 and makes it current, or -1. */
static int start_track(engine_t *e, const pending_t *p, int gapless, decoder_t *d, const audio_format_t *df)
{
    audio_format_t f;
    /* DoP only while nothing would change the samples (a USB DAC with its own
     * volume): a gain on DoP would be noise at full scale */
    if (d) f = *df;
    else d = decoder_open_flags(p->path, &f, e->dop && e->gain == 1.0f ? DECODER_DOP : 0);
    if (!d || f.channels == 0 || f.channels > MAX_CH) {
        if (d) decoder_close(d);
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
    e->track_gain = p->gain;
    e->pos_frames = 0;
    e->seq++;
    e->error = 0;
    e->ended = 0;

    audio_format_t of = out_format(e, &f);
    if (!e->out_open || !same_out_format(&e->out_fmt, &of)) {
        if (e->out_open) {
            if (gapless) e->out.drain(e->out.ctx);   /* let the previous track finish */
            else e->out.flush(e->out.ctx);
        }
        e->out_open = e->out.open(e->out.ctx, &of) == 0;
        e->out_fmt = of;
    } else if (!gapless) {
        e->out.flush(e->out.ctx);                    /* same format: just drop queued audio */
    }
    if (!gapless) eq_reset(e->eq);                   /* (gapless: the filters carry on) */
    eq_sync(e);
    return 0;
}


static void *run(void *arg)
{
    engine_t *e = arg;
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
                nd = decoder_open_ex(p.path, &nf, &e->net_abort);
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
                e->out.pause(e->out.ctx, e->start_paused);
                e->start_paused = 0;
            } else {
                e->state = ENG_STOPPED;
            }
            continue;
        }
        if (e->cmd_seek && e->dec) {
            e->cmd_seek = 0;
            uint64_t frame = (uint64_t)e->seek_ms * e->fmt.rate / 1000;
            if (decoder_seek(e->dec, frame) == 0) {
                e->out.flush(e->out.ctx);
                eq_reset(e->eq);
                e->pos_frames = frame;
            }
            continue;
        }
        if (e->state == ENG_STOPPED && e->dec && e->dec_net && !e->writing) {
            decoder_close(e->dec);              /* a stopped stream: hang up */
            e->dec = NULL;
            e->dec_net = 0;
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
        e->writing = 1;
        int net = e->dec_net;
        decoder_set_next(dec, e->next.valid && !decoder_is_stream(e->next.path) ? e->next.path : NULL);
        pthread_mutex_unlock(&e->mu);
        if (net) e->net_read_ms = mono_ms();
        long n = decoder_read(dec, e->buf, CHUNK_FRAMES);
        e->net_read_ms = 0;
        if (n > 0) {
            if (bl != 1.0f || br != 1.0f) gain_run2(e->buf, n, gain_q28(gain * bl), gain_q28(gain * br));
            else if (gain != 1.0f) gain_run(e->buf, n * ch, gain_q28(gain));
            if (eq) eq_run(eq, e->buf, n);
            w = e->out.write(e->out.ctx, e->buf, n);
        }
        pthread_mutex_lock(&e->mu);
        e->writing = 0;
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
        if (w < 0 && !e->hold) {       /* the device went away (USB DAC unplugged) */
            e->state = ENG_PAUSED;
            e->out_lost = 1;
            continue;
        }
        if (n > 0) {
            e->pos_frames += (uint64_t)n;
            continue;
        }
        /* end of track: gapless successor, or stop */
        if (e->next.valid) {
            pending_t p = e->next;
            audio_format_t cf;
            e->next.valid = 0;
            /* the next CUE track of the same file: the decoder just carries on */
            if (decoder_continue(dec, p.path, &cf) == 0 && start_track(e, &p, 1, dec, &cf) == 0) continue;
            if (!decoder_is_stream(p.path) && start_track(e, &p, 1, NULL, NULL) == 0) continue;
        }
        e->writing = 1;
        pthread_mutex_unlock(&e->mu);
        e->out.drain(e->out.ctx);
        pthread_mutex_lock(&e->mu);
        e->writing = 0;
        if (e->hold) pthread_cond_broadcast(&e->cv);
        if (dec == e->dec && !e->cmd_play.valid) {
            e->state = ENG_STOPPED;
            e->ended = 1;
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
    pthread_mutex_init(&e->mu, NULL);
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

static void set_pending(pending_t *p, const char *path, int64_t id, float gain)
{
    snprintf(p->path, sizeof p->path, "%s", path);
    p->id = id;
    p->gain = gain;
    p->valid = 1;
}

void engine_play(engine_t *e, const char *path, int64_t id) { engine_play_gain(e, path, id, 1.0f); }
void engine_queue_next(engine_t *e, const char *path, int64_t id) { engine_queue_next_gain(e, path, id, 1.0f); }

void engine_play_gain(engine_t *e, const char *path, int64_t id, float gain)
{
    pthread_mutex_lock(&e->mu);
    set_pending(&e->cmd_play, path, id, gain);
    e->start_paused = 0;
    e->next.valid = 0;
    if (e->connecting || e->dec_net) e->net_abort = 1;   /* stop waiting on the network */
    pthread_cond_signal(&e->cv);
    pthread_mutex_unlock(&e->mu);
}

void engine_queue_next_gain(engine_t *e, const char *path, int64_t id, float gain)
{
    pthread_mutex_lock(&e->mu);
    if (path) set_pending(&e->next, path, id, gain);
    else e->next.valid = 0;
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
        e->out.pause(e->out.ctx, 1);
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
    e->state = ENG_STOPPED;
    e->next.valid = 0;
    e->cmd_play.valid = 0;
    if (e->connecting || e->dec_net) e->net_abort = 1;
    if (e->out_open) e->out.flush(e->out.ctx);
    pthread_cond_signal(&e->cv);
    pthread_mutex_unlock(&e->mu);
}

void engine_seek_ms(engine_t *e, uint32_t ms)
{
    pthread_mutex_lock(&e->mu);
    e->seek_ms = ms;
    e->cmd_seek = 1;
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
        e->out_open = e->out.open(e->out.ctx, &of) == 0;
        e->out_fmt = of;
        if (e->state == ENG_PAUSED) e->out.pause(e->out.ctx, 1);
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
    st->next_queued = e->next.valid;
    st->ended = e->ended;
    st->error = e->error;
    st->output_lost = e->out_lost;
    st->starting = e->cmd_play.valid;
    st->start_paused = e->start_paused;
    st->eq = e->eq_on && !e->fmt.dop;
    st->eq_in_output = e->eq_in_out;
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
        int64_t heard = (int64_t)e->pos_frames - delay;
        if (heard < 0) heard = 0;
        st->pos_ms = (uint32_t)(heard * 1000 / e->fmt.rate);
        st->len_ms = (uint32_t)(e->fmt.total_frames * 1000 / e->fmt.rate);
    }
    pthread_mutex_unlock(&e->mu);
}
