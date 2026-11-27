/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* Bluetooth output: a bluez-alsa PCM, raw S16_LE stereo at the link's sampling
 * rate. The link's rate is fixed while connected (fathom-bt asks bluez-alsa for
 * 44.1 kHz: changing it mid-link drops many headphones); btconv brings each
 * track to it.
 *
 * The PCM is opened by `fathom-avrcp pcm-open` (D-Bus, which the player doesn't
 * link), which hands back its two descriptors: the audio FIFO, written here,
 * and the control socket. A pause ends the stream (bt_pause), and Drop (a skip,
 * a seek) throws away what's queued without ending it: the sound follows the
 * keys at once, and the headphones don't have to start a new stream for each
 * skip.
 *
 * Threads: the engine writes without its lock, so pause and flush can come from
 * the UI thread mid-write. Only the engine's side (write, open, close, drain)
 * closes the stream; a failed command from elsewhere marks it broken and the
 * next write reopens it, except a pause, which closes it under the lock each
 * write() holds. A write never waits while paused, so the engine's thread stays
 * free for the next command (a skip while paused). A flush makes a write in
 * progress drop the rest of its (old) audio. */
#define _GNU_SOURCE   /* pipe2 */
#include "audio/out_bt.h"
#include "audio/btconv.h"
#include "core/clock.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define LINK_DELAY_MS 150     /* bluez-alsa's buffer and the headphones' (typical SBC) */
/* What bluez-alsa has queued from us: ~740 ms at 44.1 kHz, so a stall on our
 * side (a page fault re-read from the card, the network) doesn't reach the
 * headphones. 32 kernel pages while Bluetooth plays; a skip or seek drops them
 * (Drop), a pause keeps them. */
#define PIPE_BYTES (128 * 1024)
#define STALL_MS 2000         /* the link took nothing for this long: it's stuck */
#define START_MS 6000         /* the same when a stream starts (connect, resume):
                               * headphones can take seconds to start one (AirPods: >2 s) */
#define OPEN_MS 8000          /* fathom-avrcp pcm-open */
#define CLI_MS 4000           /* bluealsa-cli info (it answers in a fraction of a second) */

typedef struct {
    char cli[256];            /* bluealsa-cli (info) */
    char helper[256];         /* fathom-avrcp (pcm-open) */
    char libs[256];           /* their LD_LIBRARY_PATH */
    char pcm[200];            /* D-Bus path of the playback PCM; "" = none */
    char codec[24];
    unsigned link_rate;       /* what the link was last set to */
    audio_format_t fmt;
    btconv_t *conv;
    int fd;                   /* the PCM's FIFO */
    int ctrl;                 /* its control socket */
    pthread_mutex_t mu;       /* the control socket: one command at a time; closing */
    volatile int paused;      /* bluez-alsa told to pause */
    volatile int broken;      /* a command failed: the engine's side reopens */
    volatile unsigned gen;    /* flushes so far */
    char *kept;               /* this write's audio not yet in the FIFO: */
    size_t kept_len, kept_off, kept_cap;   /* sent from kept_off on */
    unsigned kept_gen;        /* the flush count it belongs to */
    eq_profile_t eq_prof;     /* the EQ, run after the conversion at the link's rate (engine thread) */
    int eq_on;
    eq_t *eq;
    unsigned eq_seen;         /* the flush count its filters were last reset at */
    uint32_t start_until;     /* a stream starting (open, resume, skip): START_MS patience until then */
    long lost;                /* a stall closed the stream: what was queued then (frames at the
                               * track's rate), reported as the delay until the next one, so the
                               * engine resumes from what was heard (engine.c) */
    int16_t *buf;
    long cap;
} out_bt_t;

/* Runs bluealsa-cli ARGS...; its stdout to out (if any). Returns its exit
 * status; -1 if it couldn't run or had not finished its answer in CLI_MS (it
 * is stopped then: this is the playback thread, which mustn't wait for ever).
 * The pipe is close-on-exec: a daemon that another thread starts in the same
 * instant would keep the write end, and the read would wait for that daemon. */
static int cli(out_bt_t *o, char *out, size_t cap, const char *const *args)
{
    int p[2] = {-1, -1};
    if (out && pipe2(p, O_CLOEXEC)) return -1;
    pid_t pid = fork();
    if (pid < 0) {
        if (out) { close(p[0]); close(p[1]); }
        return -1;
    }
    if (pid == 0) {
        int null = open("/dev/null", O_RDWR);
        dup2(null, 0);
        dup2(out ? p[1] : null, 1);            /* (the copy isn't close-on-exec) */
        dup2(null, 2);
        setenv("LD_LIBRARY_PATH", o->libs, 1);
        const char *argv[12] = {o->cli};
        int n = 1;
        for (; args[n - 1] && n < 11; n++) argv[n] = args[n - 1];
        argv[n] = NULL;
        execv(o->cli, (char *const *)argv);
        _exit(127);
    }
    int late = 0;
    if (out) {
        close(p[1]);
        size_t got = 0;
        uint32_t end = clock_ms32() + CLI_MS;
        while (got + 1 < cap) {
            int left = (int)(int32_t)(end - clock_ms32());
            if (left <= 0) { late = 1; break; }
            struct pollfd pf = {.fd = p[0], .events = POLLIN};
            int r = poll(&pf, 1, left);
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) { late = 1; break; }
            ssize_t n = read(p[0], out + got, cap - 1 - got);
            if (n <= 0) break;                 /* (its end) */
            got += (size_t)n;
        }
        out[got] = '\0';
        close(p[0]);
    }
    if (late) {
        fprintf(stderr, "bt: bluealsa-cli gave no answer in %d s, stopped\n", CLI_MS / 1000);
        kill(pid, SIGKILL);
    }
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    return late ? -1 : WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/* The link's codec and rate, from `bluealsa-cli info PCM`. */
static void link_info(out_bt_t *o)
{
    char info[2048];
    const char *args[] = {"info", o->pcm, NULL};
    if (cli(o, info, sizeof info, args) != 0) return;
    const char *c = strstr(info, "Selected codec: ");   /* "SBC:<config>" */
    if (c) sscanf(c + 16, "%23[^ :\n]", o->codec);
    else if ((c = strstr(info, "Codec: "))) sscanf(c + 7, "%23[^ :\n]", o->codec);
    const char *r = strstr(info, "Sampling: ");
    if (r) o->link_rate = (unsigned)strtoul(r + 10, NULL, 10);
}

/* Closing both ends the stream: bluez-alsa releases the PCM. */
static void pcm_close(out_bt_t *o)
{
    pthread_mutex_lock(&o->mu);
    if (o->fd >= 0) close(o->fd);
    if (o->ctrl >= 0) close(o->ctrl);
    o->fd = o->ctrl = -1;
    o->broken = 0;
    o->kept_len = o->kept_off = 0;             /* (only the engine's side closes) */
    pthread_mutex_unlock(&o->mu);
}

/* A control command (Drop, Drain): 0 when bluez-alsa says OK,
 * 1 when there's no stream to command, -1 when it failed. */
static int pcm_ctrl(out_bt_t *o, const char *cmd, int timeout_ms)
{
    pthread_mutex_lock(&o->mu);
    int rv = 1;
    if (o->ctrl >= 0) {
        rv = -1;
        char rep[32];
        struct pollfd pf = {.fd = o->ctrl, .events = POLLIN};
        while (recv(o->ctrl, rep, sizeof rep, MSG_DONTWAIT) > 0) {}   /* a late reply */
        if (send(o->ctrl, cmd, strlen(cmd), MSG_NOSIGNAL) > 0 && poll(&pf, 1, timeout_ms) == 1 &&
            read(o->ctrl, rep, sizeof rep) >= 2 && !strncmp(rep, "OK", 2))
            rv = 0;
    }
    pthread_mutex_unlock(&o->mu);
    return rv;
}

static int pcm_open(out_bt_t *o)
{
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv)) return -1;
    pid_t pid = fork();
    if (pid < 0) { close(sv[0]); close(sv[1]); return -1; }
    if (pid == 0) {
        int null = open("/dev/null", O_RDWR);
        dup2(null, 0);
        dup2(null, 1);                         /* stderr stays: the player's log */
        if (sv[1] == 3) fcntl(3, F_SETFD, 0);  /* dup2 onto itself keeps CLOEXEC */
        else dup2(sv[1], 3);
        setenv("LD_LIBRARY_PATH", o->libs, 1);
        execl(o->helper, o->helper, "pcm-open", o->pcm, (char *)NULL);
        _exit(127);
    }
    close(sv[1]);
    int fds[2] = {-1, -1};
    struct pollfd pf = {.fd = sv[0], .events = POLLIN};
    if (poll(&pf, 1, OPEN_MS) == 1) {
        char byte;
        struct iovec iov = {.iov_base = &byte, .iov_len = 1};
        union { struct cmsghdr h; char buf[CMSG_SPACE(sizeof fds)]; } u;
        struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = u.buf, .msg_controllen = sizeof u.buf};
        if (recvmsg(sv[0], &msg, MSG_CMSG_CLOEXEC) == 1) {
            struct cmsghdr *h = CMSG_FIRSTHDR(&msg);
            if (h && h->cmsg_type == SCM_RIGHTS && h->cmsg_len == CMSG_LEN(sizeof fds))
                memcpy(fds, CMSG_DATA(h), sizeof fds);
        }
    }
    close(sv[0]);
    kill(pid, SIGKILL);                        /* done, or stuck past OPEN_MS */
    while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
    if (fds[0] < 0 || fds[1] < 0) {
        if (fds[0] >= 0) close(fds[0]);
        if (fds[1] >= 0) close(fds[1]);
        fprintf(stderr, "bt: couldn't open the link's PCM\n");
        return -1;
    }
    fcntl(fds[0], F_SETFL, O_NONBLOCK);        /* bt_write waits with a limit, never for ever */
#ifdef F_SETPIPE_SZ
    fcntl(fds[0], F_SETPIPE_SZ, PIPE_BYTES);
#endif
    pthread_mutex_lock(&o->mu);
    o->fd = fds[0];
    o->ctrl = fds[1];
    o->broken = 0;
    pthread_mutex_unlock(&o->mu);
    o->start_until = clock_ms32() + START_MS;      /* a new stream */
    o->lost = 0;
    return 0;
}

/* A command that must work: if it doesn't, the stream is in an unknown state,
 * so the engine's side opens a new one at its next write. */
static void pcm_must(out_bt_t *o, const char *cmd, int timeout_ms)
{
    if (o->ctrl < 0 || pcm_ctrl(o, cmd, timeout_ms) >= 0) return;
    fprintf(stderr, "bt: %s didn't work: the stream starts again\n", cmd);
    o->broken = 1;
}

static int bt_open(void *ctx, const audio_format_t *fmt)
{
    out_bt_t *o = ctx;
    if (!o->pcm[0]) return -2;                 /* no link */
    if (!btconv_rate_for(fmt->rate)) {
        fprintf(stderr, "bt: can't play %u Hz over Bluetooth\n", fmt->rate);
        return -1;
    }
    link_info(o);                              /* every time: a restarted stack reuses the path */
    if (!o->link_rate) o->link_rate = 44100;
    btconv_destroy(o->conv);
    o->conv = btconv_create(fmt->rate, fmt->channels, o->link_rate);
    if (!o->conv) return -1;
    if (o->eq_on && (!o->eq || eq_rate(o->eq) != o->link_rate)) {   /* the EQ at the link's rate */
        eq_destroy(o->eq);
        o->eq = eq_create(&o->eq_prof, o->link_rate, 2);
    }
    btconv_set_eq(o->conv, o->eq_on ? o->eq : NULL);
    o->fmt = *fmt;
    fprintf(stderr, "bt: %s %u Hz, from %u Hz %d-bit\n", o->codec, o->link_rate, fmt->rate, fmt->bits);
    /* the stream stays open across tracks; paused, it opens at the resume */
    return o->fd >= 0 || o->paused || pcm_open(o) == 0 ? 0 : -2;
}

/* Adds `len` bytes to what's to be written. */
static int keep(out_bt_t *o, const char *p, size_t len)
{
    if (o->kept_off) {
        memmove(o->kept, o->kept + o->kept_off, o->kept_len - o->kept_off);
        o->kept_len -= o->kept_off;
        o->kept_off = 0;
    }
    if (o->kept_len + len > o->kept_cap) {
        char *b = realloc(o->kept, o->kept_len + len);
        if (!b) return -1;
        o->kept = b;
        o->kept_cap = o->kept_len + len;
    }
    memcpy(o->kept + o->kept_len, p, len);
    o->kept_len += len;
    return 0;
}

/* Writes what's kept to the FIFO. 0: all of it (or flushed meanwhile), 1:
 * paused with some left (dropped: the pause bumped gen), -1: the stream is gone. Waits
 * while the link isn't taking audio, but never while paused (the engine's
 * thread must stay free to take the next command, such as a skip while
 * paused), and never for ever. */
static int send_kept(out_bt_t *o, unsigned gen)
{
    uint32_t since = 0, deadline = 0;          /* the link stopped taking audio at, and give up at */
    while (o->kept_off < o->kept_len) {
        if (o->gen != gen) break;              /* flushed meanwhile: the rest is old audio */
        pthread_mutex_lock(&o->mu);            /* a pause may close the stream meanwhile */
        int fd = o->fd;
        ssize_t r = fd >= 0 ? write(fd, o->kept + o->kept_off, o->kept_len - o->kept_off) : -1;
        int err = fd >= 0 ? errno : EAGAIN;
        pthread_mutex_unlock(&o->mu);
        errno = err;
        if (fd < 0 && !o->paused) return -1;
        if (r > 0) {
            o->kept_off += (size_t)r;
            deadline = 0;
            continue;
        }
        if (r < 0 && errno == EINTR) continue;
        if (r < 0 && errno != EAGAIN) {
            fprintf(stderr, "bt: the link is gone (%s)\n", strerror(errno));
            return -1;
        }
        if (o->paused) return 1;               /* full because bluez-alsa is paused */
        /* full: bluez-alsa isn't taking audio (the headphones paused the
         * stream, another device took them over, ...). Don't wait for ever:
         * the engine would hang with it, and play/pause and skips with it.
         * A stream starting (or starting again after a pause): more patience. */
        uint32_t t = clock_ms32();
        if (!deadline) {
            since = t;
            deadline = t + STALL_MS;
            if ((int32_t)(o->start_until - deadline) > 0) deadline = o->start_until;
        }
        if ((int32_t)(t - deadline) >= 0) {
            fprintf(stderr, "bt: the link took no audio for %.1f s\n", (t - since) / 1000.0);
            return -1;
        }
        struct pollfd pf = {.fd = fd, .events = POLLOUT};
        poll(&pf, 1, 250);
    }
    o->kept_len = o->kept_off = 0;
    return 0;
}

static long bt_delay(void *ctx);

static long bt_write(void *ctx, const int32_t *frames, long n)
{
    out_bt_t *o = ctx;
    if (!o->conv) return -1;
    if (o->broken) pcm_close(o);
    if (o->paused) return n;                   /* the write in flight at a pause: its audio goes */
    if (o->fd < 0 && pcm_open(o)) return -1;
    unsigned gen = o->gen;
    if (o->kept_gen != gen) o->kept_len = o->kept_off = 0;   /* kept before a flush: old */
    if (o->eq_seen != gen) { eq_reset(o->eq); o->eq_seen = gen; }   /* and the EQ's history */
    o->kept_gen = gen;
    long need = btconv_max_out(o->conv, n);
    if (need > o->cap) {
        int16_t *b = realloc(o->buf, (size_t)need * 2 * sizeof *b);
        if (!b) return -1;
        o->buf = b;
        o->cap = need;
    }
    long w = btconv_run(o->conv, frames, n, o->buf);
    if (keep(o, (const char *)o->buf, (size_t)w * 4)) return -1;
    if (send_kept(o, gen) < 0) {
        long q = bt_delay(o);                  /* what the headphones never got goes with the stream */
        pcm_close(o);
        o->lost = q;
        return -1;
    }
    return n;
}

static long bt_delay(void *ctx)
{
    out_bt_t *o = ctx;
    if (!o->conv) return 0;
    if (o->fd < 0) return o->lost;             /* no stream: paused (nothing queued), or a stall's loss */
    int q = 0;
    ioctl(o->fd, FIONREAD, &q);
    return (long)((double)q / 4 * btconv_ratio(o->conv)) + (long)o->fmt.rate * LINK_DELAY_MS / 1000;
}

static void bt_drain(void *ctx)
{
    out_bt_t *o = ctx;
    pcm_must(o, "Drain", 3000);                /* bluez-alsa plays out what's queued */
}

static void bt_flush(void *ctx)
{
    out_bt_t *o = ctx;
    o->gen++;
    o->start_until = clock_ms32() + START_MS;
    o->lost = 0;
    pcm_must(o, "Drop", 500);                  /* what's queued goes; the stream stays */
    if (o->conv) btconv_reset(o->conv);
}

/* Paused, the stream ends: bluez-alsa releases the transport (the headphones
 * see the stream suspended, as from a phone) and what's queued goes at once.
 * Resumed, the next write opens a new one. (bluez-alsa's own Pause left the
 * stream running with nothing in it, and AirPods, told the player was paused,
 * sometimes took no more audio after the resume.) */
static void bt_pause(void *ctx, int on)
{
    out_bt_t *o = ctx;
    if (!on) {
        o->start_until = clock_ms32() + START_MS;
        o->paused = 0;
        return;
    }
    pthread_mutex_lock(&o->mu);
    o->paused = 1;
    o->lost = 0;
    o->gen++;                                  /* what the engine's side kept is dropped */
    if (o->fd >= 0) close(o->fd);
    if (o->ctrl >= 0) close(o->ctrl);
    o->fd = o->ctrl = -1;
    pthread_mutex_unlock(&o->mu);
}

static void bt_close(void *ctx)
{
    out_bt_t *o = ctx;
    pcm_close(o);
    o->lost = 0;
    btconv_destroy(o->conv);
    o->conv = NULL;
}

/* The EQ: here rather than in the engine, after the conversion, at the link's
 * rate (a 192 kHz track costs a quarter as much). Called on the engine's thread
 * while it isn't writing. */
static int bt_set_eq(void *ctx, const eq_profile_t *p)
{
    out_bt_t *o = ctx;
    eq_destroy(o->eq);
    o->eq = NULL;
    o->eq_on = p != NULL;
    if (p) {
        o->eq_prof = *p;
        o->eq = eq_create(p, o->link_rate ? o->link_rate : 44100, 2);
    }
    if (o->conv) btconv_set_eq(o->conv, o->eq);
    fprintf(stderr, "bt: EQ %s\n", p ? "on, at the link's rate" : "off");
    return 1;
}

static void bt_describe(void *ctx, char *out, int cap)
{
    out_bt_t *o = ctx;
    if (!o->conv) { snprintf(out, (size_t)cap, "Bluetooth"); return; }
    if (o->fmt.rate != o->link_rate)
        snprintf(out, (size_t)cap, "Bluetooth %s %.1f kHz 16-bit (from %.1f kHz)", o->codec,
                 o->link_rate / 1000.0, o->fmt.rate / 1000.0);
    else
        snprintf(out, (size_t)cap, "Bluetooth %s %.1f kHz 16-bit", o->codec, o->link_rate / 1000.0);
}

int out_bt_create(audio_output_t *out, const char *bt_dir)
{
    out_bt_t *o = calloc(1, sizeof *o);
    if (!o) return -1;
    snprintf(o->cli, sizeof o->cli, "%s/usr/bin/bluealsa-cli", bt_dir);
    snprintf(o->helper, sizeof o->helper, "%s/../bin/fathom-avrcp", bt_dir);
    snprintf(o->libs, sizeof o->libs, "%s/usr/lib:%s/lib", bt_dir, bt_dir);
    o->fd = o->ctrl = -1;
    pthread_mutexattr_t ma;                    /* priority inheritance: the playback thread waits on it (engine.c) */
    pthread_mutexattr_init(&ma);
#if defined(__linux__) && defined(PTHREAD_PRIO_INHERIT)
    pthread_mutexattr_setprotocol(&ma, PTHREAD_PRIO_INHERIT);
#endif
    if (pthread_mutex_init(&o->mu, &ma)) pthread_mutex_init(&o->mu, NULL);
    pthread_mutexattr_destroy(&ma);
    signal(SIGPIPE, SIG_IGN);                  /* a dropped link is a write error, not a kill */
    *out = (audio_output_t){bt_open, bt_write, bt_delay, bt_drain, bt_flush, bt_pause,
                            bt_close, bt_describe, o, bt_set_eq, NULL, NULL, NULL};
    return 0;
}

void out_bt_set_pcm(void *ctx, void *pcm)
{
    out_bt_t *o = ctx;
    const char *p = pcm ? pcm : "";
    if (strcmp(p, o->pcm)) {
        o->link_rate = 0;                      /* a new device: ask it */
        o->codec[0] = '\0';
    }
    snprintf(o->pcm, sizeof o->pcm, "%s", p);
}
