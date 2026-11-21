/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Playback engine: one thread decodes and writes to an audio output.
 * Gapless: a queued next track is opened when the current one ends; if the
 * format is the same, the output is never reopened, so there is no gap.
 * At gain 1.0 and with no EQ, samples pass through untouched (bit-perfect).
 */
#ifndef FATHOM_ENGINE_H
#define FATHOM_ENGINE_H

#include <stdint.h>

#include "audio/decoder.h"
#include "audio/eq.h"
#include "audio/output.h"

typedef enum { ENG_STOPPED, ENG_PLAYING, ENG_PAUSED } eng_state_t;

typedef struct {
    eng_state_t    state;
    uint32_t       seq;          /* +1 whenever a new track starts (play or gapless) */
    int64_t        track_id;     /* id passed with the track that is playing */
    uint32_t       tag;          /* ...and its tag (engine_queue_ahead; 0 for engine_play's) */
    uint32_t       pos_ms;       /* heard position (output delay subtracted) */
    uint32_t       len_ms;
    audio_format_t fmt;          /* source format */
    int            next_queued;  /* gapless successors lined up */
    int            ended;        /* playback ran out (no successor), or the track failed */
    int            error;        /* the track couldn't be played (unreadable, or the output can't
                                  * take its format); track_id is that track's */
    unsigned       stop_seq;     /* +1 each time playback stops by itself (ended or error) */
    int            output_lost;  /* a write or open failed (device gone); the engine paused */
    int            starting;     /* a track is about to start (engine_play not yet taken up) */
    uint32_t       start_ms;     /* ...from there in (engine_play_at) */
    int            start_paused; /* ...and it will start paused */
    int            eq;           /* the EQ is on for this track */
    int            crossfeed;    /* ...and the crossfeed (XF_LIGHT..; XF_OFF: off, or a mono track) */
    int            stream;       /* the track is an internet stream (no length, no seeking) */
    int            connecting;   /* ...and it is still connecting */
    unsigned       meta_seq;     /* changes with meta */
    char           meta[256];    /* what the stream says is playing ("Artist - Title") */
    char           meta_url[512];/* ...and the URL it gives with it (a cover on some stations) */
    /* The signal path: */
    audio_format_t out_fmt;      /* what the output was opened for */
    float          gain;         /* the volume's gain (a USB DAC's software volume), 1 = none */
    float          track_gain;   /* the track's levelling, 1 = none */
    float          balance[2];   /* left, right; 1, 1 = centred (or not stereo) */
    int            untouched;    /* the samples reach the output as decoded (no gain, EQ or balance) */
} eng_status_t;

typedef struct engine engine_t;

engine_t *engine_new(const audio_output_t *out);
void      engine_free(engine_t *e);

/* start now; an http(s):// path is a stream, opened without holding up the
 * caller (eng_status_t.connecting) */
void engine_play(engine_t *e, const char *path, int64_t id);
void engine_queue_next(engine_t *e, const char *path, int64_t id);  /* gapless successor */
/* The same with the track's own gain (volume levelling; 1.0 = bit-perfect), which
 * takes effect at the track's first sample, so gapless joins stay seamless. */
void engine_play_gain(engine_t *e, const char *path, int64_t id, float gain);
/* The same from start_ms in (a podcast episode, or a long file, where it was
 * left): nothing before it is ever heard, which a play and then a seek can't
 * promise (an idle engine starts on the play before the seek comes). */
void engine_play_at(engine_t *e, const char *path, int64_t id, float gain, uint32_t start_ms);
void engine_queue_next_gain(engine_t *e, const char *path, int64_t id, float gain);
/* Lines up what follows the current track, in order (replacing what was lined
 * up; n = 0: nothing), at most ENGINE_AHEAD: the engine goes on to each by
 * itself, gapless, however late the caller looks. `tag` (the caller's, not 0)
 * comes back in eng_status_t.tag while that one plays. */
#define ENGINE_AHEAD 8
typedef struct {
    const char *path;
    int64_t     id;
    float       gain;
    uint32_t    tag;
} engine_track_t;
void engine_queue_ahead(engine_t *e, const engine_track_t *t, int n);
void engine_pause(engine_t *e, int on);
void engine_stop(engine_t *e);                                      /* a stream disconnects */
void engine_seek_ms(engine_t *e, uint32_t ms);
void engine_set_gain(engine_t *e, float gain);                      /* 1.0 = bit-perfect */
/* The balance: a gain (0..1) on each channel of a stereo track; 1, 1 = none. */
void engine_set_balance(engine_t *e, float left, float right);
/* The parametric EQ (copied; NULL = off, bit-perfect). From the next chunk of
 * audio on; with it on, a 16-bit track opens the output at 24 bits. */
void engine_set_eq(engine_t *e, const eq_profile_t *p);
/* DSD as DoP (1) for an output whose DAC takes it, or converted to PCM (0, the
 * default). On from the next track; off at once (a DoP track carries on as
 * PCM). DoP is only used while the gain is 1 (the DAC's own volume): the EQ,
 * balance, levelling and fade are bypassed for it. engine_switch_output()
 * turns it off: say so again for the new output if it takes DoP too. */
void engine_set_dop(engine_t *e, int on);
/* Crossfeed for headphones (audio/crossfeed.h: XF_OFF, XF_LIGHT...), on stereo
 * PCM from the next chunk; on, the output is at least 24-bit, as with the EQ. */
void engine_set_crossfeed(engine_t *e, int level);
/* A fade (0..1) applied on top of the gain: the sleep timer's. 1.0 = none. */
void engine_set_fade(engine_t *e, float fade);
/* Faster without the pitch going up (audio/tempo.h), in percent: 100 = off
 * (the samples untouched). Mono and stereo PCM only; from the next chunk. The
 * position and length stay the track's own. */
void engine_set_speed(engine_t *e, int speed_pct);
void engine_status(engine_t *e, eng_status_t *st);
/* What the output runs at now (its describe(); "" when it's closed). */
void engine_describe_output(engine_t *e, char *out, int cap);
/* The calling thread first in line for the CPU (1: audio's playback threads)
 * or back to normal (0). Linux only; quietly does nothing without permission. */
void audio_thread_priority(int on);

/* Move playback to another device: waits for the write in progress, closes the
 * output, calls change(output ctx, arg) (e.g. out_alsa_set_device), and reopens
 * it for the current track. Audio still queued on the old device is dropped;
 * the play/pause state is kept. */
void engine_switch_output(engine_t *e, void (*change)(void *out_ctx, void *arg), void *arg);

#endif
