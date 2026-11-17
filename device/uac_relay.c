/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * USB DAC mode: the computer plays to the H2 as to any USB DAC (USB Audio
 * Class 2, set up by fathom-usbgadget), and this copies what arrives on the
 * gadget's ALSA card ("UAC2Gadget", capture) to the H2's DAC, sample for
 * sample, at the rate the computer sends: nothing is resampled or mixed.
 *
 * The computer sends 32-bit samples (24-bit audio left-justified in them),
 * which is the output's own sample layout, so they go to the DAC as they come.
 * The gadget's "Capture Rate" control says what rate the computer has chosen
 * (0 while it sends nothing); a change reopens both sides.
 *
 * Clocks: the DAC runs on the H2's crystal and the computer sends at its own
 * idea of the rate, so the two drift apart by a few parts per million. The
 * link is asynchronous: the gadget tells the computer how many samples to
 * send per frame (feedback endpoint), scaled by "Capture Pitch 1000000".
 * The relay keeps the audio queued in the DAC near where it settled when the
 * stream started, by nudging that pitch, so the queue neither overflows nor
 * runs dry however long the music plays.
 */
#include "uac_relay.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <sound/asound.h>

#include "audio/output.h"

#define PITCH_NOMINAL 1000000
#define PITCH_SPAN    1000          /* never more than 0.1 % off nominal */

struct uac_relay {
    pthread_t th;
    volatile int stop;
    char dac_dev[64];
    pthread_mutex_t mu;
    uac_relay_status_t st;
    uint32_t last_audio;            /* ms_now() when audio last arrived */
};

static uint32_t ms_now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)(t.tv_sec * 1000 + t.tv_nsec / 1000000);
}

static void sleep_ms(int ms)
{
    struct timespec t = {ms / 1000, (long)(ms % 1000) * 1000000L};
    nanosleep(&t, NULL);
}

/* ---- the gadget's controls (PCM interface, not mixer) ---- */

static void pcm_id(struct snd_ctl_elem_id *id, const char *name)
{
    memset(id, 0, sizeof *id);
    id->iface = SNDRV_CTL_ELEM_IFACE_PCM;
    snprintf((char *)id->name, sizeof id->name, "%s", name);
}

static int ctl_get(int fd, const char *name, long *out)
{
    struct snd_ctl_elem_value v;
    memset(&v, 0, sizeof v);
    pcm_id(&v.id, name);
    if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_READ, &v)) return -1;
    *out = v.value.integer.value[0];
    return 0;
}

static int ctl_set(int fd, const char *name, long value)
{
    struct snd_ctl_elem_value v;
    memset(&v, 0, sizeof v);
    pcm_id(&v.id, name);
    v.value.integer.value[0] = value;
    return ioctl(fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &v) < 0 ? -1 : 0;
}

static int ctl_range(int fd, const char *name, long *lo, long *hi)
{
    struct snd_ctl_elem_info info;
    memset(&info, 0, sizeof info);
    pcm_id(&info.id, name);
    if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_INFO, &info)) return -1;
    *lo = info.value.integer.min;
    *hi = info.value.integer.max;
    return 0;
}

static const char *const RATE = "Capture Rate";
static const char *const PITCH = "Capture Pitch 1000000";

/* The gadget's card number, or -1 while it isn't there. */
static int find_card(void)
{
    for (int n = 0; n < 8; n++) {
        char path[48], id[32] = "";
        snprintf(path, sizeof path, "/proc/asound/card%d/id", n);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        if (!fgets(id, sizeof id, f)) id[0] = '\0';
        fclose(f);
        if (strstr(id, "UAC2")) return n;
    }
    return -1;
}

/* ---- the capture side: kernel PCM ioctls, as out_alsa.c does ---- */

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
        hp_int(p, n)->min = 0;
        hp_int(p, n)->max = UINT_MAX;
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

/* Opens and starts the capture at `rate`: S32_LE stereo, 40 ms of buffer. */
static int cap_open(int card, unsigned rate, unsigned *period)
{
    char dev[32];
    snprintf(dev, sizeof dev, "/dev/snd/pcmC%dD0c", card);
    int fd = open(dev, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) { perror(dev); return -1; }
    struct snd_pcm_hw_params hp;
    int ok = 0;
    for (int attempt = 0; attempt < 2 && !ok; attempt++) {
        hp_any(&hp);
        hp_mask_set(&hp, SNDRV_PCM_HW_PARAM_ACCESS, SNDRV_PCM_ACCESS_RW_INTERLEAVED);
        hp_mask_set(&hp, SNDRV_PCM_HW_PARAM_FORMAT, SNDRV_PCM_FORMAT_S32_LE);
        hp_mask_set(&hp, SNDRV_PCM_HW_PARAM_SUBFORMAT, SNDRV_PCM_SUBFORMAT_STD);
        hp_int_set(&hp, SNDRV_PCM_HW_PARAM_CHANNELS, 2);
        hp_int_set(&hp, SNDRV_PCM_HW_PARAM_RATE, rate);
        if (attempt == 0) {          /* else whatever the gadget's buffer allows */
            /* u_audio: periods of at most a page (4 KiB), at most 64 KiB in all.
             * 5 ms periods, 8 of them; 2.5 ms and 16 above 96 kHz: 40 ms either way. */
            int fast = rate > 96000;
            hp_int_set(&hp, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, (rate / (fast ? 400 : 200) + 7) & ~7u);
            hp_int_set(&hp, SNDRV_PCM_HW_PARAM_PERIODS, fast ? 16 : 8);
        }
        ok = ioctl(fd, SNDRV_PCM_IOCTL_HW_PARAMS, &hp) == 0;
    }
    if (!ok) {
        fprintf(stderr, "usb dac: capture %u Hz: %s\n", rate, strerror(errno));
        close(fd);
        return -1;
    }
    *period = hp_int(&hp, SNDRV_PCM_HW_PARAM_PERIOD_SIZE)->min;
    unsigned buffer = hp_int(&hp, SNDRV_PCM_HW_PARAM_BUFFER_SIZE)->min;

    struct snd_pcm_sw_params sp;
    memset(&sp, 0, sizeof sp);
    sp.tstamp_mode = SNDRV_PCM_TSTAMP_NONE;
    sp.period_step = 1;
    sp.avail_min = *period;
    sp.start_threshold = 1;
    sp.stop_threshold = buffer;             /* full = overrun: stop, start again */
    sp.xfer_align = 1;
    unsigned long boundary = buffer;
    while (boundary * 2 <= LONG_MAX - buffer) boundary *= 2;
    sp.boundary = boundary;
    if (ioctl(fd, SNDRV_PCM_IOCTL_SW_PARAMS, &sp) || ioctl(fd, SNDRV_PCM_IOCTL_PREPARE) ||
        ioctl(fd, SNDRV_PCM_IOCTL_START)) {
        fprintf(stderr, "usb dac: capture start: %s\n", strerror(errno));
        close(fd);
        return -1;
    }
    fprintf(stderr, "usb dac: capture %u Hz, period %u, buffer %u\n", rate, *period, buffer);
    return fd;
}

/* ---- the relay ---- */

typedef struct {
    int card, ctl, cap;
    unsigned rate, period;
    audio_output_t out;
    int out_ok, out_open;
    int32_t *buf;
    uint32_t used;                  /* OR of the samples since the last look: which bits carry audio */
    uint32_t next_bits;
    FILE *tap;                      /* tests: /tmp/fathom-usbdac-tap exists */
    /* clock steering */
    long pitch_lo, pitch_hi;
    int have_pitch;
    long long fill_sum;
    int fill_n;
    long target;                    /* frames queued in the DAC to hold, once known */
    int settle;                     /* half-seconds of the stream so far */
    double integ;                   /* ppm */
    int pitch;
    uint32_t next_steer, next_rate;
} relay_state_t;

static void set_pitch(relay_state_t *s, int ppm)
{
    long v = PITCH_NOMINAL + ppm;
    if (v < s->pitch_lo) v = s->pitch_lo;
    if (v > s->pitch_hi) v = s->pitch_hi;
    s->pitch = (int)v;
    if (s->have_pitch) ctl_set(s->ctl, PITCH, v);
}

static void stream_close(relay_state_t *s)
{
    if (s->cap >= 0) close(s->cap);
    s->cap = -1;
    if (s->out_open) {
        s->out.drain(s->out.ctx);
        s->out.close(s->out.ctx);
    }
    s->out_open = 0;
    if (s->tap) {
        out_alsa_set_tap(s->out.ctx, NULL);
        fclose(s->tap);
        s->tap = NULL;
    }
    free(s->buf);
    s->buf = NULL;
}

static void stream_open(relay_state_t *s, unsigned rate)
{
    s->rate = rate;
    s->target = -1;
    s->settle = 0;
    s->integ = 0;
    s->fill_sum = 0;
    s->fill_n = 0;
    set_pitch(s, 0);
    if (!rate) return;
    s->cap = cap_open(s->card, rate, &s->period);
    if (s->cap < 0) return;
    s->buf = malloc((size_t)s->period * 2 * sizeof(int32_t));
    audio_format_t fmt = {0};
    fmt.rate = rate;
    fmt.channels = 2;
    fmt.bits = 32;
    s->out_open = s->buf && s->out_ok && s->out.open(s->out.ctx, &fmt) == 0;
    if (!s->out_open) { stream_close(s); return; }
    /* Bit-perfect checks (tools/usbdac): what the DAC gets, as it gets it */
    if (access("/tmp/fathom-usbdac-tap", F_OK) == 0 && (s->tap = fopen("/tmp/fathom-usbdac.raw", "wb"))) {
        out_alsa_set_tap(s->out.ctx, s->tap);
        char d[96];
        s->out.describe(s->out.ctx, d, sizeof d);
        fprintf(stderr, "usb dac: tap on, %s\n", d);
    }
}

/* Every half second while audio flows: hold the DAC's queue where it settled. */
static void steer(relay_state_t *s)
{
    if (!s->fill_n) return;
    long fill = (long)(s->fill_sum / s->fill_n);
    s->fill_sum = 0;
    s->fill_n = 0;
    if (++s->settle < 4) return;                 /* the first 2 s: settling */
    if (s->target < 0) { s->target = fill; return; }
    double err_ms = (double)(fill - s->target) * 1000.0 / s->rate;
    /* more queued than wanted: the computer is ahead, ask it for less */
    s->integ -= 1.0 * err_ms;
    if (s->integ > PITCH_SPAN) s->integ = PITCH_SPAN;
    if (s->integ < -PITCH_SPAN) s->integ = -PITCH_SPAN;
    int ppm = (int)(s->integ - 10.0 * err_ms);
    if (ppm > PITCH_SPAN) ppm = PITCH_SPAN;
    if (ppm < -PITCH_SPAN) ppm = -PITCH_SPAN;
    set_pitch(s, ppm);
}

static void *relay_main(void *arg)
{
    uac_relay_t *r = arg;
    relay_state_t s = {.card = -1, .ctl = -1, .cap = -1};
    s.out_ok = out_alsa_create(&s.out, r->dac_dev) == 0;
    if (!s.out_ok) fprintf(stderr, "usb dac: no DAC at %s\n", r->dac_dev);

    while (!r->stop) {
        uint32_t now = ms_now();
        if (s.card < 0) {
            s.card = find_card();
            if (s.card < 0) { sleep_ms(200); continue; }
            char ctl[32];
            snprintf(ctl, sizeof ctl, "/dev/snd/controlC%d", s.card);
            s.ctl = open(ctl, O_RDWR | O_CLOEXEC);
            if (s.ctl < 0) { s.card = -1; sleep_ms(200); continue; }
            s.have_pitch = ctl_range(s.ctl, PITCH, &s.pitch_lo, &s.pitch_hi) == 0;
            if (s.pitch_lo < PITCH_NOMINAL - PITCH_SPAN) s.pitch_lo = PITCH_NOMINAL - PITCH_SPAN;
            if (s.pitch_hi > PITCH_NOMINAL + PITCH_SPAN) s.pitch_hi = PITCH_NOMINAL + PITCH_SPAN;
            s.rate = 0;
            s.pitch = PITCH_NOMINAL;
            fprintf(stderr, "usb dac: gadget is card %d%s\n", s.card, s.have_pitch ? "" : " (no pitch control)");
        }
        if ((int32_t)(now - s.next_rate) >= 0) {
            s.next_rate = now + 100;
            long rate;
            if (ctl_get(s.ctl, RATE, &rate)) {   /* the gadget went away */
                stream_close(&s);
                close(s.ctl);
                s.ctl = -1;
                s.card = -1;
                continue;
            }
            if ((unsigned)rate != s.rate) {
                stream_close(&s);
                fprintf(stderr, "usb dac: computer sends %ld Hz\n", rate);
                stream_open(&s, (unsigned)rate);
                pthread_mutex_lock(&r->mu);
                r->st.rate = (unsigned)rate;
                r->st.bits = 0;
                pthread_mutex_unlock(&r->mu);
                s.used = 0;
            }
        }
        if (s.cap < 0) { sleep_ms(100); continue; }

        struct pollfd pf = {.fd = s.cap, .events = POLLIN};
        if (poll(&pf, 1, 100) <= 0) continue;
        struct snd_xferi x = {.buf = s.buf, .frames = s.period};
        if (ioctl(s.cap, SNDRV_PCM_IOCTL_READI_FRAMES, &x)) {
            if (errno == EPIPE) {                /* fell behind: start again */
                ioctl(s.cap, SNDRV_PCM_IOCTL_PREPARE);
                ioctl(s.cap, SNDRV_PCM_IOCTL_START);
                pthread_mutex_lock(&r->mu);
                r->st.overruns++;
                pthread_mutex_unlock(&r->mu);
            } else if (errno != EAGAIN && errno != EINTR) {
                fprintf(stderr, "usb dac: capture read: %s\n", strerror(errno));
                stream_close(&s);
                s.rate = UINT_MAX;               /* reopen at the next rate check */
            }
            continue;
        }
        if (x.result <= 0) continue;
        if (s.out.write(s.out.ctx, s.buf, (long)x.result) < 0) {
            stream_close(&s);
            s.rate = UINT_MAX;
            continue;
        }
        for (long i = 0; i < 2 * (long)x.result; i++) s.used |= (uint32_t)s.buf[i];
        s.fill_sum += s.out.delay(s.out.ctx);
        s.fill_n++;
        now = ms_now();
        if ((int32_t)(now - s.next_steer) >= 0) {
            s.next_steer = now + 500;
            steer(&s);
        }
        /* Bit depth, from which bits the samples use (half a second at a time;
         * silence says nothing). macOS with no volume control on the device
         * sends a 16-bit file as 16 bits, zeros below; float sources use all 32. */
        int bits = -1;
        if ((int32_t)(now - s.next_bits) >= 0) {
            s.next_bits = now + 500;
            if (s.used) bits = !(s.used & 0xffffu) ? 16 : !(s.used & 0xffu) ? 24 : 32;
            s.used = 0;
        }
        pthread_mutex_lock(&r->mu);
        if (bits > 0) r->st.bits = bits;
        r->st.frames += (unsigned long long)x.result;
        r->last_audio = now;
        r->st.pitch = s.pitch;
        s.out.describe(s.out.ctx, r->st.dac, sizeof r->st.dac);
        pthread_mutex_unlock(&r->mu);
    }
    stream_close(&s);
    if (s.ctl >= 0) {
        if (s.have_pitch) ctl_set(s.ctl, PITCH, PITCH_NOMINAL);
        close(s.ctl);
    }
    return NULL;
}

uac_relay_t *uac_relay_start(const char *dac)
{
    uac_relay_t *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    snprintf(r->dac_dev, sizeof r->dac_dev, "%s", dac);
    pthread_mutex_init(&r->mu, NULL);
    r->st.pitch = PITCH_NOMINAL;
    if (pthread_create(&r->th, NULL, relay_main, r)) {
        free(r);
        return NULL;
    }
    return r;
}

void uac_relay_status(uac_relay_t *r, uac_relay_status_t *st)
{
    pthread_mutex_lock(&r->mu);
    *st = r->st;
    st->playing = r->st.rate && r->last_audio && ms_now() - r->last_audio < 500;
    pthread_mutex_unlock(&r->mu);
}

void uac_relay_stop(uac_relay_t *r)
{
    if (!r) return;
    r->stop = 1;
    pthread_join(r->th, NULL);
    pthread_mutex_destroy(&r->mu);
    free(r);
}
