/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
#include "radiometa.h"
#include "shell.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <libavformat/avio.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>

#include "audio/ff_util.h"
#include "core/lat.h"
#include "library/library.h"
#include "net/netclock.h"
#include "player/rfmeta.h"

#define MAX_ANSWER (256 * 1024)
#define MAX_IMAGE (1024 * 1024)
#define ART LIB_ART_LARGE

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int started;

/* what's wanted (under mu); the generations change with it, so a fetch for an
 * old one gives up (the interrupt callback reads them without the lock) */
static char want_info[64], want_art[512];
static volatile unsigned info_gen, art_gen;
static unsigned art_done = 0;
static time_t info_next;          /* monotonic s of the next info fetch */

/* results (under mu) */
static char got_artist[256], got_title[256], got_visual[512];
static int got_new;
static uint16_t *art_px;
static int art_ok, art_new;

typedef struct { volatile unsigned *now; unsigned mine; } job_t;

static int interrupted(void *opaque)
{
    const job_t *j = opaque;
    return *j->now != j->mine;
}

static time_t mono_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec;
}

/* GET url into a fresh buffer (NUL-terminated); NULL on failure. */
static unsigned char *fetch(const char *url, job_t *job, size_t max, size_t *len)
{
    AVIOContext *pb = NULL;
    AVIOInterruptCB icb = {interrupted, job};
    AVDictionary *opt = NULL;
    av_dict_set(&opt, "rw_timeout", "10000000", 0);
    av_dict_set(&opt, "user_agent", "Fathom (HiFi Walker H2)", 0);
    ff_tls_options(&opt);
    int r = avio_open2(&pb, url, AVIO_FLAG_READ, &icb, &opt);
    av_dict_free(&opt);
    if (r < 0 && !interrupted(job)) netclock_net_failed(url);   /* (the clock may be why: netclock.h) */
    if (r < 0) return NULL;
    unsigned char *buf = malloc(max + 1);
    size_t n = 0;
    int whole = 0;
    while (buf && n < max) {
        int k = avio_read(pb, buf + n, (int)(max - n));
        if (k == AVERROR_EOF) { whole = 1; break; }
        if (k <= 0) break;
        n += (size_t)k;
    }
    avio_closep(&pb);
    if (buf && (!whole || !n)) { free(buf); buf = NULL; }   /* cut short or too big */
    if (buf) buf[n] = '\0';
    if (len) *len = n;
    return buf;
}

static void do_art(void)
{
    char url[sizeof want_art];
    snprintf(url, sizeof url, "%s", want_art);
    job_t job = {&art_gen, art_gen};
    pthread_mutex_unlock(&mu);

    uint16_t *px = malloc((size_t)ART * ART * 2);
    int ok = 0;
    if (px && url[0]) {
        size_t n = 0;
        unsigned char *img = fetch(url, &job, MAX_IMAGE, &n);
        if (img) ok = library_art_decode(img, n, ART, px) == 0;
        free(img);
    }

    pthread_mutex_lock(&mu);
    if (job.mine == art_gen) {
        art_done = job.mine;
        if (ok && art_px) memcpy(art_px, px, (size_t)ART * ART * 2);
        art_ok = ok && art_px;
        art_new = 1;
    }
    free(px);
}

static void do_info(void)
{
    char info[sizeof want_info];
    snprintf(info, sizeof info, "%s", want_info);
    job_t job = {&info_gen, info_gen};
    pthread_mutex_unlock(&mu);

    char artist[256] = "", title[256] = "", visual[512] = "";
    int ok = -1, n = rf_station(info);
    int64_t step_start = 0, step_end = 0;
    if (n >= 0) {
        char url[128];
        rf_url(n, url, sizeof url);
        char *json = (char *)fetch(url, &job, MAX_ANSWER, NULL);
        if (json) {
            ok = rf_parse(json, artist, sizeof artist, title, sizeof title, visual, sizeof visual);
            if (ok == 0) rf_step_times(json, &step_start, &step_end);
            free(json);
        }
    }
    /* Asked again when the song on now ends, not every 15 s (a new TLS
     * connection each time, for as long as the station plays); every 15 s
     * still when its end can't be told by this clock (rf_next_fetch_s). */
    int again = ok == 0 ? rf_next_fetch_s((int64_t)time(NULL), step_start, step_end) : 5;
    LAT("radiometa: info %s, asked again in %d s", ok == 0 ? "read" : "not read", again);

    pthread_mutex_lock(&mu);
    if (job.mine != info_gen) return;   /* the station changed meanwhile: fetch again at once */
    info_next = mono_s() + again;
    if (ok == 0 && (strcmp(artist, got_artist) || strcmp(title, got_title) || strcmp(visual, got_visual))) {
        snprintf(got_artist, sizeof got_artist, "%s", artist);
        snprintf(got_title, sizeof got_title, "%s", title);
        snprintf(got_visual, sizeof got_visual, "%s", visual);
        got_new = 1;
    }
}

static void *run(void *arg)
{
    (void)arg;
    /* Song info and covers are extras: their TLS handshakes and picture
     * decoding give way to the stream's own connection, the UI and the music
     * on the one core (nice 10, this thread only: behind them, but not
     * starved into a timeout the way nice 19 can be). */
    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 10);
    pthread_mutex_lock(&mu);
    for (;;) {
        if (art_done != art_gen) { do_art(); continue; }        /* a new cover first: it's quick */
        if (want_info[0] && mono_s() >= info_next) { do_info(); continue; }
        if (want_info[0]) {
            mono_cond_wait_ms(&cv, &mu, (int)(info_next - mono_s() + 1) * 1000);
        } else {
            pthread_cond_wait(&cv, &mu);
        }
    }
    return NULL;
}

static void start(void)   /* under mu */
{
    if (started) return;
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 128 * 1024);
    mono_cond_init(&cv);
    if (pthread_create(&t, &a, run, NULL) == 0) {
        pthread_detach(t);
        started = 1;
    }
    pthread_attr_destroy(&a);
}

void radiometa_want(const char *info)
{
    pthread_mutex_lock(&mu);
    const char *w = info ? info : "";
    if (strcmp(w, want_info)) {
        snprintf(want_info, sizeof want_info, "%s", w);
        info_gen++;
        info_next = 0;
        got_artist[0] = got_title[0] = got_visual[0] = '\0';
        got_new = 0;
        if (want_info[0]) start();
        pthread_cond_signal(&cv);
    }
    pthread_mutex_unlock(&mu);
}

int radiometa_take(char *artist, size_t an, char *title, size_t tn, char *visual, size_t vn)
{
    pthread_mutex_lock(&mu);
    int r = got_new;
    if (r) {
        snprintf(artist, an, "%s", got_artist);
        snprintf(title, tn, "%s", got_title);
        snprintf(visual, vn, "%s", got_visual);
        got_new = 0;
    }
    pthread_mutex_unlock(&mu);
    return r;
}

void radiometa_want_art(const char *url)
{
    pthread_mutex_lock(&mu);
    const char *w = url ? url : "";
    if (strcmp(w, want_art)) {
        snprintf(want_art, sizeof want_art, "%s", w);
        art_gen++;
        if (!want_art[0]) {             /* none: no fetch, nothing to show */
            art_done = art_gen;
            art_ok = 0;
            art_new = 1;
            free(art_px);
            art_px = NULL;
        } else {
            if (!art_px) art_px = malloc((size_t)ART * ART * 2);
            art_ok = 0;
            art_new = 1;                 /* the old cover goes at once */
            start();
        }
        pthread_cond_signal(&cv);
    }
    pthread_mutex_unlock(&mu);
}

int radiometa_take_art(uint16_t *px, int size)
{
    pthread_mutex_lock(&mu);
    int r = -1;
    if (art_new) {
        art_new = 0;
        r = art_ok && art_px && size == ART ? 1 : 0;
        if (r) memcpy(px, art_px, (size_t)ART * ART * 2);
    }
    pthread_mutex_unlock(&mu);
    return r;
}
