/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
#include "net/lyricfind.h"

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "core/clock.h"
#include "core/thread.h"
#include "net/json.h"

#define ANSWER_MAX (96 * 1024)     /* LRCLIB's answer for a song: its lyrics three times over, about 12 KB */
#define NONE_DAYS 7                /* "looked for and not found" is said for this long */

/* ---------- asking ---------- */

static size_t url_put(char *out, size_t o, size_t cap, const char *s)
{
    for (const unsigned char *c = (const unsigned char *)s; *c && o + 4 < cap; c++) {
        if (isalnum(*c) || *c == '-' || *c == '.' || *c == '_') out[o++] = (char)*c;
        else if (*c == ' ') out[o++] = '+';
        else o += (size_t)snprintf(out + o, cap - o, "%%%02X", *c);
    }
    out[o < cap ? o : cap - 1] = '\0';
    return o;
}

void lf_url(const lf_song_t *s, int with_album, char *out, size_t cap)
{
    size_t o = (size_t)snprintf(out, cap, "https://lrclib.net/api/get?artist_name=");
    if (o >= cap) return;
    o = url_put(out, o, cap, s->artist);
    o += (size_t)snprintf(out + o, cap - o, "&track_name=");
    if (o >= cap) return;
    o = url_put(out, o, cap, s->title);
    if (with_album && s->album[0]) {
        o += (size_t)snprintf(out + o, cap - o, "&album_name=");
        if (o >= cap) return;
        o = url_put(out, o, cap, s->album);
    }
    if (s->duration_s > 0 && o < cap) snprintf(out + o, cap - o, "&duration=%d", s->duration_s);
}

/* A member's text, malloc'd (NULL: it has none, or it is empty) */
static char *member_text(json_t o, const char *name, size_t *len)
{
    json_t v;
    if (!json_member(o, name, &v) || json_type(v) != JSON_STRING || v.n > LYRICS_MAX_BYTES * 2) return NULL;
    char *t = malloc(v.n + 1);
    if (!t) return NULL;
    long n = json_string(v, t, v.n + 1);
    if (n > 0) n = (long)strlen(t);                /* (a \u0000 in it: the text ends there) */
    while (n > 0 && isspace((unsigned char)t[n - 1])) n--;
    if (n <= 0 || n > LYRICS_MAX_BYTES) { free(t); return NULL; }
    t[n] = '\0';
    *len = (size_t)n;
    return t;
}

int lf_parse(const char *json, size_t len, int duration_s, char **text, size_t *text_len)
{
    *text = NULL;
    *text_len = 0;
    json_t root = json_root(json, len), v;
    if (json_type(root) != JSON_OBJECT || !json_member(root, "trackName", &v)) return -1;
    /* timed ones are for one recording: taken when its length is ours (the
     * site matches within two seconds; an answer further off is another cut) */
    long long theirs = json_member(root, "duration", &v) ? json_int(v, 0) : 0;
    int same = duration_s > 0 && theirs > 0 && theirs - duration_s <= 3 && duration_s - theirs <= 3;
    if (same && (*text = member_text(root, "syncedLyrics", text_len))) return 1;
    if ((*text = member_text(root, "plainLyrics", text_len))) return 1;
    return json_member(root, "instrumental", &v) && json_bool(v, 0) ? 2 : 0;
}

/* ---------- the worker ---------- */

typedef struct {
    lf_song_t song;
    int online;
    volatile int abort;
} job_t;

static struct {
    pthread_mutex_t mu;
    char store[900];
    http_get_fn fetch;
    int started;
    int running;               /* a thread is at work (on `now`) */
    job_t *now, *pending;
    /* the last ask's answer */
    int state, online, tried;
    int64_t track;
    lyrics_t found;
    int from;
} L = {.mu = PTHREAD_MUTEX_INITIALIZER, .state = LF_IDLE};

void lf_set_fetch(http_get_fn fn) { L.fetch = fn; }

static long fetch(const char *url, char *buf, size_t cap, const volatile int *abort)
{
    return L.fetch ? L.fetch(url, buf, cap, abort) : http_get_retry(url, buf, cap, abort);
}

/* The job's answer, if it is still the one waited for */
static void answer(job_t *j, int state, lyrics_t *found, int from, int tried)
{
    pthread_mutex_lock(&L.mu);
    if (L.now == j && !j->abort) {
        lyrics_free(&L.found);
        if (found) { L.found = *found; memset(found, 0, sizeof *found); }
        L.from = from;
        L.tried = tried;
        L.state = state;
    }
    pthread_mutex_unlock(&L.mu);
    if (found) lyrics_free(found);
}

/* One song: what it has; then, asked to, what LRCLIB has */
static void look(job_t *j, const char *store)
{
    const lf_song_t *s = &j->song;
    lyrics_song_t ls = {s->path, s->artist, s->title, s->album};
    lyrics_t l;
    int from = 0;
    if (lyrics_local(store, &ls, &l, &from) == 0) { answer(j, LF_FOUND, &l, from, 0); return; }
    int tried = lyrics_store_tried(store, &ls, NONE_DAYS);
    if (!j->online || j->abort) { answer(j, LF_NONE, NULL, 0, tried); return; }
    if (!store[0] || !s->artist[0] || !s->title[0]) { answer(j, LF_NOT_FOUND, NULL, 0, tried); return; }
    char *buf = malloc(ANSWER_MAX), url[2048];
    if (!buf) { answer(j, LF_FAILED, NULL, 0, tried); return; }
    int state = LF_NOT_FOUND;
    for (int with_album = s->album[0] ? 1 : 0; with_album >= 0 && !j->abort; with_album--) {
        lf_url(s, with_album, url, sizeof url);
        long n = fetch(url, buf, ANSWER_MAX, &j->abort);
        if (n == -3 || n == -2) continue;           /* not there under that album (or an answer too long to be one) */
        if (n < 0) { state = LF_FAILED; break; }
        char *text;
        size_t tl;
        int r = lf_parse(buf, (size_t)n, s->duration_s, &text, &tl);
        if (r == 2) state = LF_INSTRUMENTAL;
        if (r != 1) continue;
        int ok = lyrics_parse(text, tl, &l) == 0;
        if (ok) lyrics_store_save(store, &ls, text, tl);   /* (not kept: shown all the same, and asked again another day) */
        free(text);
        if (!ok) continue;
        free(buf);
        answer(j, LF_FOUND, &l, LYRICS_FROM_STORE, 0);
        return;
    }
    free(buf);
    if (j->abort) return;
    if (state != LF_FAILED) lyrics_store_none(store, &ls);
    answer(j, state, NULL, 0, state != LF_FAILED);
}

static void *run(void *arg)
{
    job_t *j = arg;
    thread_nice(10);
    for (;;) {
        char store[sizeof L.store];
        pthread_mutex_lock(&L.mu);
        snprintf(store, sizeof store, "%s", L.store);
        pthread_mutex_unlock(&L.mu);
        look(j, store);
        pthread_mutex_lock(&L.mu);
        if (L.now == j) L.now = NULL;
        free(j);
        j = L.pending;
        L.pending = NULL;
        if (j) L.now = j;
        else L.running = 0;
        pthread_mutex_unlock(&L.mu);
        if (!j) return NULL;
    }
}

void lf_start(const char *store)
{
    pthread_mutex_lock(&L.mu);
    snprintf(L.store, sizeof L.store, "%s", store ? store : "");
    L.started = 1;
    pthread_mutex_unlock(&L.mu);
}

int lf_ask(const lf_song_t *s, int online)
{
    job_t *j = calloc(1, sizeof *j);
    if (!j) return -1;
    j->song = *s;
    j->online = online;
    pthread_mutex_lock(&L.mu);
    if (!L.started) { pthread_mutex_unlock(&L.mu); free(j); return -1; }
    lyrics_free(&L.found);
    L.state = LF_LOOKING;
    L.online = online;
    L.tried = 0;
    L.track = s->track;
    if (L.running) {                               /* after the one at work, which is told to give up */
        if (L.now) L.now->abort = 1;
        free(L.pending);
        L.pending = j;
        pthread_mutex_unlock(&L.mu);
        return 0;
    }
    L.running = 1;
    L.now = j;
    pthread_mutex_unlock(&L.mu);
    if (thread_detached(run, j, 128 * 1024)) {       /* (as the radio's fetches) */
        pthread_mutex_lock(&L.mu);
        L.running = 0;
        L.now = NULL;
        L.state = LF_IDLE;
        pthread_mutex_unlock(&L.mu);
        free(j);
        return -1;
    }
    return 0;
}

int lf_state(int64_t *track, int *online, int *tried)
{
    pthread_mutex_lock(&L.mu);
    int st = L.state;
    if (track) *track = L.track;
    if (online) *online = L.online;
    if (tried) *tried = L.tried;
    pthread_mutex_unlock(&L.mu);
    return st;
}

int lf_take(lyrics_t *out, int *from)
{
    int r = -1;
    pthread_mutex_lock(&L.mu);
    if (L.state == LF_FOUND && L.found.n) {
        *out = L.found;
        memset(&L.found, 0, sizeof L.found);
        if (from) *from = L.from;
        r = 0;
    }
    pthread_mutex_unlock(&L.mu);
    return r;
}

void lf_stop_ask(void)
{
    pthread_mutex_lock(&L.mu);
    L.started = 0;
    if (L.now) L.now->abort = 1;
    free(L.pending);
    L.pending = NULL;
    lyrics_free(&L.found);
    L.state = LF_IDLE;
    pthread_mutex_unlock(&L.mu);
}

int lf_stop_wait(int ms)
{
    int64_t until = clock_ms() + ms;
    for (;;) {
        pthread_mutex_lock(&L.mu);
        int running = L.running;
        pthread_mutex_unlock(&L.mu);
        if (!running) return 1;
        if (ms >= 0 && clock_ms() >= until) return 0;
        struct timespec ts = {0, 20 * 1000000};
        nanosleep(&ts, NULL);
    }
}
