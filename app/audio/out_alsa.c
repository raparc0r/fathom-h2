/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * ALSA output straight on the kernel's PCM ioctls (no alsa-lib/tinyalsa): the
 * player stays one static binary, and every step is under our control.
 *
 * Bit-perfect: the device runs at the source's rate. 16-bit sources go out as
 * S16_LE; anything deeper as the first 24/32-bit format the device takes:
 * S24_LE (the X1000 AIC), S32_LE or S24_3LE (USB DACs). Volume is the DAC's
 * own (mixer_alsa.c), never a gain on the samples.
 */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <sound/asound.h>

#include "audio/output.h"
#include "audio/pcmring.h"
#include "core/lat.h"

typedef struct {
    char     dev[64];
    int      fd;
    audio_format_t fmt;
    int      format;            /* SNDRV_PCM_FORMAT_* on the wire */
    int      sample_bytes;      /* 2, 3 or 4 */
    unsigned period, buffer;    /* frames */
    void    *conv;
    size_t   conv_cap;
    int      paused;
    volatile int flushes;       /* a_flush()es: a retry must not write what one dropped */
    char     desc[96];
    FILE *tap;                  /* out_alsa_set_tap(): a copy of every byte written */
    unsigned long lat_thr, lat_queued;   /* (LAT: the start threshold, frames written since the prepare) */
    pcmring_t hist;             /* what was last written, for a_flush_soft (up to the buffer, 256 KB at most) */
    int soft_tail;              /* a soft flush left a tail playing: an underrun after it is its end */
} alsa_out_t;

#define HIST_MAX_BYTES (256 * 1024)

/* ---- hw_params helpers (what alsa-lib's snd_pcm_hw_params_any/set_* do) ---- */

static struct snd_mask *hp_mask(struct snd_pcm_hw_params *p, int n)
{
    return &p->masks[n - SNDRV_PCM_HW_PARAM_FIRST_MASK];
}

static struct snd_interval *hp_int(struct snd_pcm_hw_params *p, int n)
{
    return &p->intervals[n - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
}

static void hp_any(struct snd_pcm_hw_params *p)
{
    memset(p, 0, sizeof *p);
    for (int n = SNDRV_PCM_HW_PARAM_FIRST_MASK; n <= SNDRV_PCM_HW_PARAM_LAST_MASK; n++)
        memset(hp_mask(p, n)->bits, 0xff, sizeof hp_mask(p, n)->bits);
    for (int n = SNDRV_PCM_HW_PARAM_FIRST_INTERVAL; n <= SNDRV_PCM_HW_PARAM_LAST_INTERVAL; n++) {
        struct snd_interval *i = hp_int(p, n);
        i->min = 0;
        i->max = UINT_MAX;
    }
    p->rmask = ~0u;
    p->info = ~0u;
}

static void hp_mask_set(struct snd_pcm_hw_params *p, int n, unsigned v)
{
    struct snd_mask *m = hp_mask(p, n);
    memset(m->bits, 0, sizeof m->bits);
    m->bits[v >> 5] |= 1u << (v & 31);
}

static void hp_int_set(struct snd_pcm_hw_params *p, int n, unsigned v)
{
    struct snd_interval *i = hp_int(p, n);
    i->min = i->max = v;
    i->integer = 1;
}

/* ---- the output ---- */

static int format_bytes(int f)
{
    return f == SNDRV_PCM_FORMAT_S16_LE ? 2 : f == SNDRV_PCM_FORMAT_S24_3LE ? 3 : 4;
}

/* Files: ~25 ms periods, 8 of them, about 200 ms at every rate (a gapless
 * change of track reaches the screen that much before it's heard, so not
 * more). Internet streams have no gapless, and the network, a busy card or a
 * page fault re-read from it can hold playback up for longer than 200 ms: up
 * to 1 s for them, in 8 periods. The H2's own DAC has 512 KiB of DMA memory set
 * aside at boot (the kernel's dmaengine PCM), so that costs it nothing up to
 * there; a USB DAC's buffer is allocated as it opens, so less there. */
#define STREAM_BUFFER_MS 1000
#define OWN_DAC "/dev/snd/pcmC0D0p"
#define OWN_DAC_BYTES (448 * 1024)
#define OTHER_BYTES (192 * 1024)

static unsigned period_frames(const alsa_out_t *a, const audio_format_t *fmt, int format, int deep)
{
    unsigned small = ((fmt->rate / 40) + 31) & ~31u;
    if (!deep) return small;
    unsigned frame = (unsigned)fmt->channels * (unsigned)format_bytes(format);
    unsigned cap = (strcmp(a->dev, OWN_DAC) ? OTHER_BYTES : OWN_DAC_BYTES) / frame;
    unsigned want = fmt->rate * STREAM_BUFFER_MS / 1000;
    unsigned p = (want < cap ? want : cap) / 8 & ~31u;
    return p > small ? p : small;
}

static void a_close(void *ctx)
{
    alsa_out_t *a = ctx;
    if (a->fd >= 0) {
        ioctl(a->fd, SNDRV_PCM_IOCTL_DROP);
        close(a->fd);
    }
    a->fd = -1;
    pcmring_free(&a->hist);
}

static int a_open(void *ctx, const audio_format_t *fmt)
{
    alsa_out_t *a = ctx;
    a_close(a);
    a->fd = open(a->dev, O_RDWR | O_CLOEXEC);
    if (a->fd < 0) { perror(a->dev); return -2; }   /* (gone: unplugged) */

    a->fmt = *fmt;
    unsigned rate = fmt->rate;

    /* Lossy sources decode deeper than 16 bits, so only a known 16-bit source
     * goes out as S16_LE first; 16 bits is also the last resort. */
    static const int deep[] = {SNDRV_PCM_FORMAT_S24_LE, SNDRV_PCM_FORMAT_S32_LE, SNDRV_PCM_FORMAT_S24_3LE,
                               SNDRV_PCM_FORMAT_S16_LE};
    static const int shallow[] = {SNDRV_PCM_FORMAT_S16_LE, SNDRV_PCM_FORMAT_S24_LE, SNDRV_PCM_FORMAT_S32_LE,
                                  SNDRV_PCM_FORMAT_S24_3LE};
    /* 32-bit sources: S32_LE first, or the low 8 bits would be lost */
    static const int full[] = {SNDRV_PCM_FORMAT_S32_LE, SNDRV_PCM_FORMAT_S24_LE, SNDRV_PCM_FORMAT_S24_3LE,
                               SNDRV_PCM_FORMAT_S16_LE};
    const int *try = fmt->bits && fmt->bits <= 16 ? shallow : fmt->bits > 24 ? full : deep;
    struct snd_pcm_hw_params hp;
    int ok = 0, err = 0;
    /* a stream: the deep buffer, else (a device that won't) the usual one */
    for (int k = 0; k < 8 && !ok; k++) {
        int i = k % 4, deep = fmt->stream && k < 4;
        if (k >= 4 && !fmt->stream) break;
        if (fmt->dop && try[i] == SNDRV_PCM_FORMAT_S16_LE) continue;   /* DoP needs 24 bits */
        unsigned period = period_frames(a, fmt, try[i], deep);
        hp_any(&hp);
        hp_mask_set(&hp, SNDRV_PCM_HW_PARAM_ACCESS, SNDRV_PCM_ACCESS_RW_INTERLEAVED);
        hp_mask_set(&hp, SNDRV_PCM_HW_PARAM_FORMAT, (unsigned)try[i]);
        hp_mask_set(&hp, SNDRV_PCM_HW_PARAM_SUBFORMAT, SNDRV_PCM_SUBFORMAT_STD);
        hp_int_set(&hp, SNDRV_PCM_HW_PARAM_CHANNELS, fmt->channels);
        hp_int_set(&hp, SNDRV_PCM_HW_PARAM_RATE, rate);
        hp_int_set(&hp, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, period);
        hp_int_set(&hp, SNDRV_PCM_HW_PARAM_PERIODS, 8);
        if (ioctl(a->fd, SNDRV_PCM_IOCTL_HW_PARAMS, &hp) == 0) {
            ok = 1;
            a->format = try[i];
        } else {
            err = errno;
        }
    }
    if (!ok) {
        fprintf(stderr, "alsa: hw_params %u Hz %u-bit %u ch: %s\n", rate, fmt->bits, fmt->channels, strerror(err));
        a_close(a);
        return err == ENODEV ? -2 : -1;              /* unplugged meanwhile, or not a format it takes */
    }
    a->sample_bytes = format_bytes(a->format);
    a->period = hp_int(&hp, SNDRV_PCM_HW_PARAM_PERIOD_SIZE)->min;
    a->buffer = hp_int(&hp, SNDRV_PCM_HW_PARAM_BUFFER_SIZE)->min;
    {   /* a copy of what's queued, for a flush that fades instead of clicking */
        unsigned fb = (unsigned)fmt->channels * (unsigned)format_bytes(a->format);
        unsigned cap = a->buffer < HIST_MAX_BYTES / fb ? a->buffer : HIST_MAX_BYTES / fb;
        pcmring_free(&a->hist);
        if (!fmt->dop) pcmring_init(&a->hist, cap, fb);   /* (DoP: never faded, its markers would go) */
        a->soft_tail = 0;
    }

    struct snd_pcm_sw_params sp;
    memset(&sp, 0, sizeof sp);
    sp.tstamp_mode = SNDRV_PCM_TSTAMP_NONE;
    sp.period_step = 1;
    sp.avail_min = a->period;
    /* Files start once 50 ms are queued. A stream comes in at about its own
     * pace after the first burst, so what's queued at the start is all the
     * slack it has: half the buffer (a station takes seconds to connect anyway). */
    sp.start_threshold = fmt->stream && a->buffer >= rate / 2 ? a->buffer / 2 : a->period * 2;
    sp.stop_threshold = a->buffer;            /* underrun = stop (recovered in write) */
    a->lat_thr = sp.start_threshold;
    a->lat_queued = 0;
    sp.xfer_align = 1;
    unsigned long boundary = a->buffer;
    while (boundary * 2 <= LONG_MAX - a->buffer) boundary *= 2;
    sp.boundary = boundary;
    /* Zero each part of the ring as soon as it has played. Stopping (drain,
     * underrun) is only noticed at the next period interrupt, and until then
     * the DMA keeps going round the ring: without this it replays stale audio
     * from a buffer ago, which you hear as a crackle at the end of a stream. */
    sp.silence_threshold = 0;
    sp.silence_size = boundary;
    if (ioctl(a->fd, SNDRV_PCM_IOCTL_SW_PARAMS, &sp) || ioctl(a->fd, SNDRV_PCM_IOCTL_PREPARE)) {
        err = errno;
        fprintf(stderr, "alsa: sw_params/prepare: %s\n", strerror(err));
        a_close(a);
        return err == ENODEV ? -2 : -1;
    }
    a->paused = 0;
    const char *fname = a->format == SNDRV_PCM_FORMAT_S16_LE ? "S16_LE"
                      : a->format == SNDRV_PCM_FORMAT_S24_LE ? "S24_LE"
                      : a->format == SNDRV_PCM_FORMAT_S32_LE ? "S32_LE" : "S24_3LE";
    snprintf(a->desc, sizeof a->desc, "%s %s %u Hz", a->dev, fname, rate);
    fprintf(stderr, "alsa: %s for a %u-bit source, period %u, buffer %u\n", a->desc, fmt->bits, a->period, a->buffer);
    return 0;
}

/* Left-justified int32 -> the wire format. */
static const void *convert(alsa_out_t *a, const int32_t *in, long frames)
{
    size_t n = (size_t)frames * a->fmt.channels;
    size_t need = n * (size_t)a->sample_bytes;
    if (need > a->conv_cap) {
        void *p = realloc(a->conv, need);
        if (!p) return NULL;
        a->conv = p;
        a->conv_cap = need;
    }
    if (a->format == SNDRV_PCM_FORMAT_S16_LE) {
        int16_t *o = a->conv;
        for (size_t i = 0; i < n; i++) o[i] = (int16_t)(in[i] >> 16);
    } else if (a->format == SNDRV_PCM_FORMAT_S24_LE) {
        int32_t *o = a->conv;
        for (size_t i = 0; i < n; i++) o[i] = in[i] >> 8;   /* low 24 bits, sign-extended */
    } else if (a->format == SNDRV_PCM_FORMAT_S32_LE) {
        memcpy(a->conv, in, need);                          /* already left-justified */
    } else {
        uint8_t *o = a->conv;                               /* S24_3LE: 3 bytes, little-endian */
        for (size_t i = 0; i < n; i++) {
            uint32_t v = (uint32_t)in[i] >> 8;
            o[3 * i] = (uint8_t)v;
            o[3 * i + 1] = (uint8_t)(v >> 8);
            o[3 * i + 2] = (uint8_t)(v >> 16);
        }
    }
    return a->conv;
}

static long a_write(void *ctx, const int32_t *frames, long n)
{
    alsa_out_t *a = ctx;
    if (a->fd < 0) return -1;
    const char *buf = convert(a, frames, n);
    if (!buf) return -1;
    size_t frame_bytes = (size_t)a->fmt.channels * (size_t)a->sample_bytes;
    if (a->tap) fwrite(buf, frame_bytes, (size_t)n, a->tap);   /* exactly what the card gets */
    long left = n;
    int tries = 0, flushes = a->flushes;
    while (left > 0) {
        struct snd_xferi x = {.buf = (void *)buf, .frames = (snd_pcm_uframes_t)left};
        if (ioctl(a->fd, SNDRV_PCM_IOCTL_WRITEI_FRAMES, &x) == 0) {
            pcmring_put(&a->hist, buf, (unsigned)x.result);
            a->soft_tail = 0;
            buf += (size_t)x.result * frame_bytes;
            left -= x.result;
            if (lat_on() && a->lat_queued < a->lat_thr && (a->lat_queued += x.result) >= a->lat_thr)
                LAT("alsa: started (%lu frames queued)", a->lat_thr);
            continue;
        }
        if (errno == EINTR) continue;
        if (errno == EPIPE || errno == ESTRPIPE) {      /* underrun: start again */
            static unsigned underruns;               /* (a gap you hear: logged, to find them) */
            if (!a->soft_tail) fprintf(stderr, "alsa: underrun %u\n", ++underruns);
            a->soft_tail = 0;                        /* (a soft flush's tail ran out first: not a gap) */
            pcmring_reset(&a->hist);
            a->lat_queued = 0;
            if (ioctl(a->fd, SNDRV_PCM_IOCTL_PREPARE) == 0) continue;
        }
        /* The start failing (EIO; the kernel's -ENOMEM when its DMA can't
         * get memory at that moment, seen in the first seconds after boot)
         * isn't the device gone: prepared again and retried a few times
         * before the engine is told (an unplugged DAC fails the prepare) */
        int e = errno;
        if ((e == EIO || e == ENOMEM) && tries < 3 && a->flushes == flushes &&
            ioctl(a->fd, SNDRV_PCM_IOCTL_PREPARE) == 0) {
            fprintf(stderr, "alsa: write: %s, again\n", strerror(e));
            tries++;
            usleep(20000);
            if (a->flushes == flushes) continue;
            return n;                                /* (dropped meanwhile: as written) */
        }
        if (a->flushes == flushes) fprintf(stderr, "alsa: write: %s\n", strerror(e));
        return -1;                                   /* (cut short by a flush, a skip or a stop: not logged) */
    }
    return n;
}

static long a_delay(void *ctx)
{
    alsa_out_t *a = ctx;
    snd_pcm_sframes_t d = 0;
    if (a->fd < 0 || ioctl(a->fd, SNDRV_PCM_IOCTL_DELAY, &d)) return 0;
    return d > 0 ? (long)d : 0;
}

static void a_drain(void *ctx)
{
    alsa_out_t *a = ctx;
    if (a->fd < 0) return;
    ioctl(a->fd, SNDRV_PCM_IOCTL_DRAIN);
    ioctl(a->fd, SNDRV_PCM_IOCTL_PREPARE);
    a->lat_queued = 0;
}

static void a_flush(void *ctx)
{
    alsa_out_t *a = ctx;
    if (a->fd < 0) return;
    a->flushes++;
    ioctl(a->fd, SNDRV_PCM_IOCTL_DROP);
    ioctl(a->fd, SNDRV_PCM_IOCTL_PREPARE);
    a->paused = 0;
    a->lat_queued = 0;
    a->soft_tail = 0;
    pcmring_reset(&a->hist);
}

static int ring_format(int format)
{
    return format == SNDRV_PCM_FORMAT_S16_LE ? PCMRING_S16 : format == SNDRV_PCM_FORMAT_S24_LE ? PCMRING_S24_4
         : format == SNDRV_PCM_FORMAT_S32_LE ? PCMRING_S32 : PCMRING_S24_3;
}

/* As a_flush, without the click of sound stopped mid-waveform: all but a
 * period of what's queued is taken back (the DMA may read up to that far
 * ahead), and the 8 ms after it written again from the copy, faded to
 * silence. The PCM keeps running; what comes next follows the fade. Only
 * from the writing thread, between writes (output.h: flush_soft). Paused,
 * not started, DoP, or a rewind the driver refuses: a_flush. */
static void a_flush_soft(void *ctx)
{
    alsa_out_t *a = ctx;
    if (a->fd < 0) return;
    snd_pcm_sframes_t d = 0;
    if (a->paused || !a->hist.buf || ioctl(a->fd, SNDRV_PCM_IOCTL_DELAY, &d) || d <= 0) { a_flush(a); return; }
    unsigned fade = a->fmt.rate / 125 > 16 ? a->fmt.rate / 125 : 16;
    if ((unsigned long)d <= a->period + fade) return;     /* (as little as that left: it plays out) */
    unsigned long want = (unsigned long)d - a->period;
    if (want > a->hist.cap) want = a->hist.cap;           /* (no further back than the copy goes) */
    size_t fb = a->hist.frame_bytes;
    uint8_t *tail = malloc((size_t)fade * fb);
    if (!tail || pcmring_get(&a->hist, (unsigned)want, fade, tail)) { free(tail); a_flush(a); return; }
    snd_pcm_uframes_t rw = want;
    if (ioctl(a->fd, SNDRV_PCM_IOCTL_REWIND, &rw) || rw != want) {   /* (refused, or only part: the copy's out of step) */
        free(tail);
        a_flush(a);
        return;
    }
    pcmring_unput(&a->hist, (unsigned)want);
    pcm_fade_out(tail, fade, a->fmt.channels, ring_format(a->format));
    const uint8_t *p = tail;
    unsigned left = fade;
    while (left) {
        struct snd_xferi x = {.buf = (void *)p, .frames = left};
        if (ioctl(a->fd, SNDRV_PCM_IOCTL_WRITEI_FRAMES, &x)) {
            if (errno == EINTR) continue;
            free(tail);
            a_flush(a);
            return;
        }
        pcmring_put(&a->hist, p, (unsigned)x.result);
        p += (size_t)x.result * fb;
        left -= (unsigned)x.result;
    }
    free(tail);
    a->flushes++;
    a->soft_tail = 1;
    LAT("alsa: soft flush: %lu frames taken back, %u faded", (unsigned long)want, fade);
}

static void a_pause(void *ctx, int on)
{
    alsa_out_t *a = ctx;
    if (a->fd < 0 || a->paused == !!on) return;
    if (ioctl(a->fd, SNDRV_PCM_IOCTL_PAUSE, on ? 1 : 0) == 0) {
        a->paused = !!on;
        return;
    }
    /* No hardware pause (or not running yet): drop what's queued instead. */
    if (on) a_flush(a);
    a->paused = 0;
}

static void a_describe(void *ctx, char *out, int cap)
{
    alsa_out_t *a = ctx;
    snprintf(out, (size_t)cap, "%s", a->fd >= 0 ? a->desc : "ALSA (closed)");
}

void out_alsa_set_device(void *ctx, void *device)
{
    alsa_out_t *a = ctx;
    a_close(a);
    snprintf(a->dev, sizeof a->dev, "%s", (const char *)device);
}

static int a_can_cut(void *ctx) { (void)ctx; return 1; }

int out_alsa_create(audio_output_t *out, const char *device)
{
    alsa_out_t *a = calloc(1, sizeof *a);
    if (!a) return -1;
    snprintf(a->dev, sizeof a->dev, "%s", device ? device : "/dev/snd/pcmC0D0p");
    a->fd = -1;
    if (access(a->dev, W_OK)) { free(a); return -1; }
    *out = (audio_output_t){a_open, a_write, a_delay, a_drain, a_flush, a_pause, a_close, a_describe, a, NULL, a_can_cut,
                            a_flush_soft};
    return 0;
}

void out_alsa_set_tap(void *ctx, FILE *f)
{
    ((alsa_out_t *)ctx)->tap = f;
}
