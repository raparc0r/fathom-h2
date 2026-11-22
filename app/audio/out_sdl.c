/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/*
 * Desktop output through SDL's audio queue. SDL converts to whatever the Mac's
 * device runs at, so this path is for listening and UI work, not bit-perfect
 * playback; that's the ALSA backend's job on the H2.
 */
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio/output.h"

typedef struct {
    SDL_AudioDeviceID dev;
    SDL_AudioSpec have;
    audio_format_t fmt;
    int frame_bytes;
} sdl_out_t;

#define QUEUE_MS 250

static int s_open(void *ctx, const audio_format_t *fmt)
{
    sdl_out_t *o = ctx;
    if (o->dev && o->fmt.rate == fmt->rate && o->fmt.channels == fmt->channels) {
        o->fmt = *fmt;
        return 0;
    }
    if (o->dev) SDL_CloseAudioDevice(o->dev);
    SDL_AudioSpec want;
    SDL_zero(want);
    want.freq = (int)fmt->rate;
    want.format = AUDIO_S32SYS;
    want.channels = fmt->channels;
    want.samples = 4096;
    o->dev = SDL_OpenAudioDevice(NULL, 0, &want, &o->have, 0);  /* SDL converts if needed */
    if (!o->dev) {
        fprintf(stderr, "out_sdl: %s\n", SDL_GetError());
        return -1;
    }
    o->fmt = *fmt;
    o->frame_bytes = (int)(sizeof(int32_t) * fmt->channels);
    SDL_PauseAudioDevice(o->dev, 0);
    return 0;
}

static long s_write(void *ctx, const int32_t *frames, long n)
{
    sdl_out_t *o = ctx;
    if (!o->dev) return -1;
    Uint32 limit = (Uint32)((uint64_t)o->fmt.rate * QUEUE_MS / 1000 * o->frame_bytes);
    while (SDL_GetQueuedAudioSize(o->dev) > limit) SDL_Delay(5);
    if (SDL_QueueAudio(o->dev, frames, (Uint32)(n * o->frame_bytes))) return -1;
    return n;
}

static long s_delay(void *ctx)
{
    sdl_out_t *o = ctx;
    return o->dev && o->frame_bytes ? (long)(SDL_GetQueuedAudioSize(o->dev) / (Uint32)o->frame_bytes) : 0;
}

static void s_drain(void *ctx)
{
    sdl_out_t *o = ctx;
    while (o->dev && SDL_GetQueuedAudioSize(o->dev) > 0 &&
           SDL_GetAudioDeviceStatus(o->dev) == SDL_AUDIO_PLAYING)
        SDL_Delay(5);
}

static void s_flush(void *ctx)
{
    sdl_out_t *o = ctx;
    if (o->dev) SDL_ClearQueuedAudio(o->dev);
}

static void s_pause(void *ctx, int on)
{
    sdl_out_t *o = ctx;
    if (o->dev) SDL_PauseAudioDevice(o->dev, on);
}

static void s_close(void *ctx)
{
    sdl_out_t *o = ctx;
    if (o->dev) SDL_CloseAudioDevice(o->dev);
    o->dev = 0;
}

static void s_describe(void *ctx, char *out, int cap)
{
    sdl_out_t *o = ctx;
    snprintf(out, (size_t)cap, "SDL %d Hz (%s)", o->have.freq, SDL_GetCurrentAudioDriver());
}

int out_sdl_create(audio_output_t *out)
{
    if (SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        fprintf(stderr, "out_sdl: %s\n", SDL_GetError());
        return -1;
    }
    sdl_out_t *o = calloc(1, sizeof *o);
    if (!o) return -1;
    *out = (audio_output_t){s_open, s_write, s_delay, s_drain, s_flush, s_pause, s_close, s_describe, o, NULL, NULL, NULL};
    return 0;
}

void out_sdl_destroy(audio_output_t *out)
{
    s_close(out->ctx);
    free(out->ctx);
    out->ctx = NULL;
}
