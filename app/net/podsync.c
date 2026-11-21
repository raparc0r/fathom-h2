/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Podcasts over the network: one worker, one job at a time (podsync.h). */
#include "net/podsync.h"

#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "library/library.h"   /* library_art_decode: a show's cover */
#include "net/update.h"   /* update_free_space */

enum { J_REFRESH, J_SUBSCRIBE, J_UNSUBSCRIBE, J_IMPORT, J_SEARCH, J_DOWNLOAD, J_AUTO };

#define MAX_JOBS 160
#define SEARCH_ANSWER (256 * 1024)
#define MIN_FREE (200LL * 1024 * 1024)   /* downloads stop with less free on the card */

typedef struct job {
    struct job *next;
    int kind;
    char id[13];
    char url[512];
    char title[128];
    pod_episode_t ep;         /* J_DOWNLOAD */
    pod_show_t show;          /* J_SUBSCRIBE */
    int64_t now;              /* J_AUTO: */
    int wifi, charging, keep, clean_days;
} job_t;

static struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    pthread_t th;
    int started, stop;
    char dir[512];
    job_t *head, *tail;
    int n_jobs;
    job_t *cur;               /* the job under way (owned by the worker) */
    volatile int abort;       /* the job under way gives up */
    int pct;                  /* its download's progress */
    unsigned changed;
    int last_failed;
    unsigned failed_seq;
    char failed_what[64];
    int imported;
    unsigned import_seq;
    /* the search */
    int search_state;         /* -1 none, 0 running, 1 done, -2 failed */
    unsigned search_gen;      /* the one asked last */
    pod_show_t *found;
    int n_found;
    int64_t auto_refreshed;   /* podsync_auto: the last time it read the feeds */
} P = {.mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER, .search_state = -1};

static podsync_fetch_fn g_fetch_once = http_get;
static podsync_download_fn g_download_once = http_download;

/* A name lookup or a connection fails now and then (a home router slow to
 * answer for a name it hasn't cached: "Try again"), and a download goes
 * through several hosts' redirects: up to 3 tries, 2 s and 5 s apart. */
static int wait_abortable(int ms)
{
    for (int t = 0; t < ms && !P.abort && !P.stop; t += 100) {
        struct timespec ts = {0, 100 * 1000 * 1000L};
        nanosleep(&ts, NULL);
    }
    return P.abort || P.stop;
}

static long g_fetch(const char *url, char *buf, size_t cap, const volatile int *abort)
{
    static const int WAIT[] = {2000, 5000};
    long n = -1;
    for (int i = 0; i < 3; i++) {
        n = g_fetch_once(url, buf, cap, abort);
        if (n >= 0 || n == -2 || n == -3 || i == 2 || wait_abortable(WAIT[i])) break;   /* (-3: not there) */
    }
    return n;
}

static int64_t g_download(const char *url, const char *path, http_progress_fn progress, void *ctx,
                          const volatile int *abort)
{
    static const int WAIT[] = {2000, 5000};
    int64_t n = -1;
    for (int i = 0; i < 3; i++) {
        n = g_download_once(url, path, progress, ctx, abort);
        if (n > 0 || n == -3 || i == 2 || wait_abortable(WAIT[i])) break;
    }
    return n;
}
void podsync_set_fetch(podsync_fetch_fn fn) { g_fetch_once = fn ? fn : http_get; }
void podsync_set_download(podsync_download_fn fn) { g_download_once = fn ? fn : http_download; }
const char *podsync_dir(void) { return P.dir; }

static void failed(const char *what)
{
    pthread_mutex_lock(&P.mu);
    P.last_failed = 1;
    P.failed_seq++;
    snprintf(P.failed_what, sizeof P.failed_what, "%s", what ? what : "");
    pthread_mutex_unlock(&P.mu);
}

static void touched(void)
{
    pthread_mutex_lock(&P.mu);
    P.changed++;
    pthread_mutex_unlock(&P.mu);
}

/* ---------- the jobs ---------- */

/* A show's cover, kept small (pod_cover_path) when there's none yet or its
 * image changed: downloaded beside, decoded, the download removed. */
#define COVER_MAX_FILE (4 * 1024 * 1024)
static void cover(const pod_show_t *s, const char *old_image)
{
    char path[800], img[820];
    pod_cover_path(P.dir, s->id, path, sizeof path);
    struct stat st;
    if (!s->image[0] || (stat(path, &st) == 0 && !strcmp(s->image, old_image))) return;
    snprintf(img, sizeof img, "%s.img", path);
    if (g_download(s->image, img, NULL, NULL, &P.abort) <= 0) { remove(img); return; }
    FILE *f = fopen(img, "rbe");
    long n = -1;
    unsigned char *buf = NULL;
    if (f) {
        fseek(f, 0, SEEK_END);
        n = ftell(f);
        fseek(f, 0, SEEK_SET);
        buf = n > 0 && n <= COVER_MAX_FILE ? malloc((size_t)n) : NULL;
        if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) n = -1;
        fclose(f);
    }
    remove(img);
    static uint16_t px[LIB_ART_LARGE * LIB_ART_LARGE];
    if (buf && n > 0 && library_art_decode(buf, (size_t)n, LIB_ART_LARGE, px) == 0) {
        char tmp[820];
        snprintf(tmp, sizeof tmp, "%s.tmp", path);
        FILE *o = fopen(tmp, "we");
        int ok = o && fwrite(px, 1, sizeof px, o) == sizeof px;
        if (o) ok &= fclose(o) == 0;
        if (!ok || rename(tmp, path)) remove(tmp);
        else touched();
    }
    free(buf);
}

/* Read a show's feed: its details and newest episodes onto the card. 0, -1. */
static int refresh_one(const char *url)
{
    pod_show_t *shows = calloc(POD_MAX_SHOWS, sizeof *shows);
    char *buf = malloc(POD_FEED_MAX);
    pod_episode_t *eps = calloc(POD_MAX_EPISODES, sizeof *eps);
    int r = -1;
    if (!shows || !buf || !eps) goto out;
    int ns = pod_shows_load(P.dir, shows, POD_MAX_SHOWS), at = -1;
    for (int i = 0; i < ns; i++)
        if (!strcmp(shows[i].url, url)) at = i;
    if (at < 0) goto out;                            /* unsubscribed meanwhile */
    long n = g_fetch(url, buf, POD_FEED_MAX, &P.abort);
    if (n == -2) n = (long)strlen(buf);              /* cut short: the episodes before the cut */
    if (n <= 0) { failed(shows[at].title); goto out; }
    pod_show_t s = shows[at], parsed = s;
    int m = pod_parse_feed(buf, (size_t)n, &parsed, eps, POD_MAX_EPISODES);
    if (m < 0) { failed(shows[at].title); goto out; }
    if (parsed.author[0]) snprintf(s.author, sizeof s.author, "%s", parsed.author);
    if (parsed.image[0]) snprintf(s.image, sizeof s.image, "%s", parsed.image);
    /* show.txt keeps the feed's own title (shows.txt the one it was added with) */
    if (parsed.title[0]) snprintf(s.title, sizeof s.title, "%s", parsed.title);
    if (!s.refreshed) pod_show_seen(P.dir, s.id, m > 1 ? eps[1].date : 0);   /* just added: only its newest is new */
    s.refreshed = (int64_t)time(NULL);
    s.latest = m ? eps[0].date : 0;
    char old_image[384];
    snprintf(old_image, sizeof old_image, "%s", shows[at].image);
    if (pod_episodes_save(P.dir, s.id, eps, m) == 0 && pod_show_save(P.dir, &s) == 0) r = 0;
    if (r == 0) cover(&s, old_image);
out:
    free(shows);
    free(buf);
    free(eps);
    if (r == 0) touched();
    return r;
}

static void refresh_all(void)
{
    pod_show_t *shows = calloc(POD_MAX_SHOWS, sizeof *shows);
    if (!shows) return;
    int n = pod_shows_load(P.dir, shows, POD_MAX_SHOWS);
    for (int i = 0; i < n && !P.abort && !P.stop; i++) refresh_one(shows[i].url);
    free(shows);
}

static void progress(uint64_t done, uint64_t total, void *ctx)
{
    const job_t *j = ctx;
    uint64_t all = total ? total : (uint64_t)j->ep.size;
    int pct = all ? (int)(done * 100 / all) : 1;
    pthread_mutex_lock(&P.mu);
    P.pct = pct < 1 ? 1 : pct > 99 ? 99 : pct;
    pthread_mutex_unlock(&P.mu);
}

/* The show is still subscribed to (a download queued before an unsubscribe
 * would make its folder again, and fill it with what nothing lists) */
static int subscribed(const char *id)
{
    pod_show_t *shows = calloc(POD_MAX_SHOWS, sizeof *shows);
    if (!shows) return 1;
    int n = pod_shows_load(P.dir, shows, POD_MAX_SHOWS), yes = 0;
    for (int i = 0; i < n; i++) yes |= !strcmp(shows[i].id, id);
    free(shows);
    return yes;
}

static void download(job_t *j)
{
    if (pod_downloaded(P.dir, j->id, &j->ep) || !subscribed(j->id)) return;
    char path[800], sd[700];
    snprintf(sd, sizeof sd, "%s/%s", P.dir, j->id);
    mkdir(P.dir, 0755);
    mkdir(sd, 0755);
    long cluster;
    int64_t free_b = update_free_space(P.dir, &cluster);
    if (free_b >= 0 && free_b < MIN_FREE + j->ep.size) { failed("The card is nearly full"); touched(); return; }
    pod_episode_path(P.dir, j->id, &j->ep, path, sizeof path);
    int64_t got = g_download(j->ep.url, path, progress, j, &P.abort);
    if (got > 0 && j->ep.chapters[0]) {          /* the feed's chapters, kept beside it */
        char *buf = malloc(128 * 1024), cp[820];
        long n = buf ? g_fetch(j->ep.chapters, buf, 128 * 1024, &P.abort) : -1;
        pod_chapter_t one;
        if (n > 0 && pod_parse_chapters(buf, (size_t)n, &one, 1) > 0) {
            pod_chapters_path(P.dir, j->id, j->ep.key, cp, sizeof cp);
            FILE *f = fopen(cp, "we");
            if (f) { fwrite(buf, 1, (size_t)n, f); fclose(f); }
        }
        free(buf);
    }
    if (got <= 0) {
        if (!P.abort) failed(j->ep.title);
        char part[820];
        snprintf(part, sizeof part, "%s.part", path);
        remove(part);
    }
    touched();
}

static void search(job_t *j, unsigned gen)
{
    char url[600];
    pod_search_url(url, sizeof url, j->title);
    char *buf = malloc(SEARCH_ANSWER);
    pod_show_t *found = calloc(POD_SEARCH_MAX, sizeof *found);
    int n = -1;
    if (buf && found) {
        long len = g_fetch(url, buf, SEARCH_ANSWER, &P.abort);
        if (len == -2) len = (long)strlen(buf);
        if (len > 0) n = pod_parse_search(buf, (size_t)len, found, POD_SEARCH_MAX);
    }
    free(buf);
    pthread_mutex_lock(&P.mu);
    if (gen == P.search_gen) {                      /* (not replaced by a newer search) */
        free(P.found);
        P.found = n >= 0 ? found : NULL;
        P.n_found = n >= 0 ? n : 0;
        P.search_state = n >= 0 ? 1 : -2;
        if (n >= 0) found = NULL;                   /* (kept; a failed one's freed below) */
    }
    pthread_mutex_unlock(&P.mu);
    free(found);
}

static void import(job_t *j)
{
    FILE *f = fopen(j->url, "rbe");
    char *t = NULL;
    long n = -1;
    if (f) {
        fseek(f, 0, SEEK_END);
        n = ftell(f);
        fseek(f, 0, SEEK_SET);
        t = n > 0 && n < 2 * 1024 * 1024 ? malloc((size_t)n + 1) : NULL;
        n = t ? (long)fread(t, 1, (size_t)n, f) : -1;
        fclose(f);
    }
    int added = -1;
    pod_show_t *s = calloc(POD_MAX_SHOWS, sizeof *s);
    if (t && s && n > 0) {
        t[n] = '\0';
        int k = pod_parse_opml(t, (size_t)n, s, POD_MAX_SHOWS);
        added = 0;
        for (int i = 0; i < k; i++)
            if (pod_shows_add(P.dir, &s[i]) == 1) {
                added++;
                touched();
                refresh_one(s[i].url);
            }
    }
    free(t);
    free(s);
    pthread_mutex_lock(&P.mu);
    P.imported = added;
    P.import_seq++;
    pthread_mutex_unlock(&P.mu);
}

static int queued_download(const char *id, const char *key);
static int enqueue(job_t *j);

/* Stage 2: downloads to queue and old ones to clean up, show by show */
static void auto_job(job_t *j)
{
    pod_show_t *shows = calloc(POD_MAX_SHOWS, sizeof *shows);
    pod_episode_t *eps = calloc(POD_MAX_EPISODES, sizeof *eps);
    pod_state_t *st = calloc(400, sizeof *st);
    if (!shows || !eps || !st) goto out;
    int ns = pod_shows_load(P.dir, shows, POD_MAX_SHOWS);
    for (int i = 0; i < ns && !P.stop; i++) {
        int ne = pod_episodes_load(P.dir, shows[i].id, eps, POD_MAX_EPISODES);
        int nst = pod_state_load(P.dir, shows[i].id, st, 400), want = 0;
        for (int e = 0; e < ne; e++) {
            pod_state_t s;
            pod_state_find(st, nst, eps[e].key, &s);
            int on_card = pod_downloaded(P.dir, shows[i].id, &eps[e]);
            if (s.played && on_card && j->clean_days > 0 && s.played_at &&
                s.played_at + (int64_t)j->clean_days * 86400 < j->now) {
                pod_delete_download(P.dir, shows[i].id, &eps[e]);   /* played a while ago */
                touched();
                continue;
            }
            if (j->charging && j->wifi && !s.played && want < j->keep) {
                want++;                                  /* the newest unplayed ones */
                pthread_mutex_lock(&P.mu);
                int dup = queued_download(shows[i].id, eps[e].key);
                pthread_mutex_unlock(&P.mu);
                if (!on_card && !dup) {
                    job_t *d = calloc(1, sizeof *d);
                    if (!d) continue;
                    d->kind = J_DOWNLOAD;
                    snprintf(d->id, sizeof d->id, "%s", shows[i].id);
                    d->ep = eps[e];
                    if (enqueue(d)) free(d);
                }
            }
        }
    }
out:
    free(shows);
    free(eps);
    free(st);
}

static void run(job_t *j, unsigned search_gen)
{
    switch (j->kind) {
    case J_REFRESH:
        pthread_mutex_lock(&P.mu);
        P.last_failed = 0;
        pthread_mutex_unlock(&P.mu);
        if (j->url[0]) refresh_one(j->url);
        else refresh_all();
        break;
    case J_SUBSCRIBE:
        if (pod_shows_add(P.dir, &j->show) >= 0) { touched(); refresh_one(j->show.url); }
        break;
    case J_UNSUBSCRIBE: pod_shows_remove(P.dir, j->url); touched(); break;
    case J_IMPORT: import(j); break;
    case J_SEARCH: search(j, search_gen); break;
    case J_DOWNLOAD: download(j); break;
    case J_AUTO: auto_job(j); break;
    default: break;
    }
}

/* What a download or a cover cut short left (the player killed, the card
 * pulled, the power gone): KEY.mp3.part, cover.565.img(.part), cover.565.tmp.
 * Only this worker writes those, so at its start none is under way. */
static void sweep(void)
{
    DIR *d = opendir(P.dir);
    struct dirent *e;
    char sd[1200], f[1500];
    while (d && (e = readdir(d))) {
        if (e->d_name[0] == '.' || strlen(e->d_name) != 12) continue;   /* the shows' folders: 12 hex */
        snprintf(sd, sizeof sd, "%s/%s", P.dir, e->d_name);
        DIR *s = opendir(sd);
        struct dirent *g;
        while (s && (g = readdir(s))) {
            size_t n = strlen(g->d_name);
            /* (only the worker's own: the UI may be writing a state.txt.tmp now) */
            if ((n > 5 && !strcmp(g->d_name + n - 5, ".part")) || !strcmp(g->d_name, "cover.565.tmp") ||
                !strcmp(g->d_name, "cover.565.img")) {
                snprintf(f, sizeof f, "%s/%s", sd, g->d_name);
                remove(f);
            }
        }
        if (s) closedir(s);
    }
    if (d) closedir(d);
}

static void *worker(void *arg)
{
    (void)arg;
    sweep();
    pthread_mutex_lock(&P.mu);
    while (!P.stop) {
        if (!P.head) { pthread_cond_wait(&P.cv, &P.mu); continue; }
        job_t *j = P.head;
        P.head = j->next;
        if (!P.head) P.tail = NULL;
        P.n_jobs--;
        P.cur = j;
        P.abort = 0;
        P.pct = j->kind == J_DOWNLOAD ? 1 : 0;
        unsigned gen = P.search_gen;
        pthread_mutex_unlock(&P.mu);
        run(j, gen);
        pthread_mutex_lock(&P.mu);
        P.cur = NULL;
        free(j);
    }
    pthread_mutex_unlock(&P.mu);
    return NULL;
}

/* (with P.mu held, or from the worker) */
static int queued_download(const char *id, const char *key)
{
    for (job_t *q = P.head; q; q = q->next)
        if (q->kind == J_DOWNLOAD && !strcmp(q->id, id) && !strcmp(q->ep.key, key)) return 1;
    return P.cur && P.cur->kind == J_DOWNLOAD && !strcmp(P.cur->id, id) && !strcmp(P.cur->ep.key, key);
}

/* Queue j (taken over on success): 0, or -1 (full, or not started). */
static int enqueue_locked(job_t *j)
{
    if (!P.started || P.n_jobs >= MAX_JOBS) return -1;
    j->next = NULL;
    if (P.tail) P.tail->next = j;
    else P.head = j;
    P.tail = j;
    P.n_jobs++;
    pthread_cond_signal(&P.cv);
    return 0;
}

static int enqueue(job_t *j)
{
    pthread_mutex_lock(&P.mu);
    int r = enqueue_locked(j);
    pthread_mutex_unlock(&P.mu);
    return r;
}

static int add(int kind, const char *id, const char *url, const char *title)
{
    job_t *j = calloc(1, sizeof *j);
    if (!j) return -1;
    j->kind = kind;
    if (id) snprintf(j->id, sizeof j->id, "%s", id);
    if (url) snprintf(j->url, sizeof j->url, "%s", url);
    if (title) snprintf(j->title, sizeof j->title, "%s", title);
    if (enqueue(j)) { free(j); return -1; }
    return 0;
}

/* ---------- the UI's side ---------- */

int podsync_start(const char *dir)
{
    if (P.started) return 0;
    snprintf(P.dir, sizeof P.dir, "%s", dir);
    P.stop = 0;
    if (pthread_create(&P.th, NULL, worker, NULL)) return -1;
    P.started = 1;
    return 0;
}

void podsync_stop(void)
{
    if (!P.started) return;
    pthread_mutex_lock(&P.mu);
    P.stop = 1;
    P.abort = 1;
    pthread_cond_signal(&P.cv);
    pthread_mutex_unlock(&P.mu);
    pthread_join(P.th, NULL);
    while (P.head) { job_t *j = P.head; P.head = j->next; free(j); }
    P.tail = NULL;
    P.n_jobs = 0;
    P.started = 0;
    free(P.found);
    P.found = NULL;
}

int podsync_refresh(const char *url)
{
    pthread_mutex_lock(&P.mu);
    int dup = 0;   /* the same refresh already waiting */
    for (job_t *q = P.head; q; q = q->next)
        dup |= q->kind == J_REFRESH && !strcmp(q->url, url ? url : "");
    pthread_mutex_unlock(&P.mu);
    return dup ? 0 : add(J_REFRESH, NULL, url ? url : "", NULL);
}

int podsync_subscribe(const pod_show_t *s)
{
    job_t *j = calloc(1, sizeof *j);
    if (!j) return -1;
    j->kind = J_SUBSCRIBE;
    j->show = *s;
    if (enqueue(j)) { free(j); return -1; }
    return 0;
}

int podsync_unsubscribe(const char *url) { return add(J_UNSUBSCRIBE, NULL, url, NULL); }
int podsync_import_opml(const char *path) { return add(J_IMPORT, NULL, path, NULL); }

int podsync_search(const char *term)
{
    pthread_mutex_lock(&P.mu);
    /* one search at a time: a waiting one is replaced, one under way is dropped */
    for (job_t **pp = &P.head; *pp;) {
        if ((*pp)->kind == J_SEARCH) {
            job_t *d = *pp;
            *pp = d->next;
            if (P.tail == d) P.tail = NULL;
            P.n_jobs--;
            free(d);
        } else {
            pp = &(*pp)->next;
        }
    }
    if (!P.tail) for (job_t *q = P.head; q; q = q->next) P.tail = q;
    if (P.cur && P.cur->kind == J_SEARCH) P.abort = 1;
    P.search_gen++;
    P.search_state = 0;
    free(P.found);
    P.found = NULL;
    P.n_found = 0;
    pthread_mutex_unlock(&P.mu);
    if (add(J_SEARCH, NULL, NULL, term)) {
        pthread_mutex_lock(&P.mu);
        P.search_state = -2;
        pthread_mutex_unlock(&P.mu);
        return -1;
    }
    return 0;
}

int podsync_search_result(pod_show_t *out, int max, int *n)
{
    pthread_mutex_lock(&P.mu);
    int st = P.search_state;
    *n = 0;
    if (st == 1)
        for (int i = 0; i < P.n_found && i < max; i++) out[(*n)++] = P.found[i];
    pthread_mutex_unlock(&P.mu);
    return st;
}

int podsync_download(const char *id, const pod_episode_t *e)
{
    pthread_mutex_lock(&P.mu);
    int dup = queued_download(id, e->key);
    pthread_mutex_unlock(&P.mu);
    if (dup) return 0;
    job_t *j = calloc(1, sizeof *j);
    if (!j) return -1;
    j->kind = J_DOWNLOAD;
    snprintf(j->id, sizeof j->id, "%s", id);
    j->ep = *e;
    if (enqueue(j)) { free(j); return -1; }
    return 0;
}

void podsync_cancel_download(const char *id, const char *key)
{
    pthread_mutex_lock(&P.mu);
    for (job_t **pp = &P.head; *pp;) {
        job_t *q = *pp;
        if (q->kind == J_DOWNLOAD && !strcmp(q->id, id) && !strcmp(q->ep.key, key)) {
            *pp = q->next;
            P.n_jobs--;
            free(q);
        } else {
            pp = &q->next;
        }
    }
    P.tail = NULL;
    for (job_t *q = P.head; q; q = q->next) P.tail = q;
    if (P.cur && P.cur->kind == J_DOWNLOAD && !strcmp(P.cur->id, id) && !strcmp(P.cur->ep.key, key)) P.abort = 1;
    pthread_mutex_unlock(&P.mu);
}

int podsync_download_state(const char *id, const char *key)
{
    pthread_mutex_lock(&P.mu);
    int r = -1;
    if (P.cur && P.cur->kind == J_DOWNLOAD && !strcmp(P.cur->id, id) && !strcmp(P.cur->ep.key, key)) r = P.pct;
    else
        for (job_t *q = P.head; q; q = q->next)
            if (q->kind == J_DOWNLOAD && !strcmp(q->id, id) && !strcmp(q->ep.key, key)) r = 0;
    pthread_mutex_unlock(&P.mu);
    return r;
}

void podsync_status(podsync_status_t *st)
{
    pthread_mutex_lock(&P.mu);
    st->busy = P.cur != NULL;
    st->queued = P.n_jobs;
    st->changed = P.changed;
    st->refreshing = P.cur && (P.cur->kind == J_REFRESH || P.cur->kind == J_SUBSCRIBE || P.cur->kind == J_IMPORT);
    for (job_t *q = P.head; q && !st->refreshing; q = q->next) st->refreshing = q->kind == J_REFRESH;
    st->last_failed = P.last_failed;
    st->failed_seq = P.failed_seq;
    snprintf(st->failed_what, sizeof st->failed_what, "%s", P.failed_what);
    st->imported = P.imported;
    st->import_seq = P.import_seq;
    pthread_mutex_unlock(&P.mu);
}

int podsync_auto(int64_t now, int wifi, int charging, int every_s, int keep, int clean_days)
{
    if (!P.started) return 0;
    int queued = 0;
    pthread_mutex_lock(&P.mu);
    int auto_waiting = 0, refresh_waiting = 0, dl_waiting = 0;
    for (job_t *q = P.head; q; q = q->next) {
        auto_waiting |= q->kind == J_AUTO;
        refresh_waiting |= q->kind == J_REFRESH;
        dl_waiting |= q->kind == J_DOWNLOAD;
    }
    if (!P.auto_refreshed) {                        /* since the feeds were last read (after a restart) */
        pod_show_t *s = calloc(POD_MAX_SHOWS, sizeof *s);
        int n = s ? pod_shows_load(P.dir, s, POD_MAX_SHOWS) : 0;
        int64_t oldest = now;
        for (int i = 0; i < n; i++)
            if (s[i].refreshed < oldest) oldest = s[i].refreshed;
        P.auto_refreshed = n ? oldest : now;
        free(s);
    }
    int due = wifi && every_s > 0 && (now - P.auto_refreshed >= every_s || now < P.auto_refreshed);
    pthread_mutex_unlock(&P.mu);
    if (due && !refresh_waiting && podsync_refresh("") == 0) {
        P.auto_refreshed = now;
        queued = 1;
    }
    /* downloads and clean-up after the feeds: an AUTO job behind them */
    if (!auto_waiting && !dl_waiting && (clean_days > 0 || (charging && wifi))) {
        job_t *j = calloc(1, sizeof *j);
        if (j) {
            j->kind = J_AUTO;
            j->now = now;
            j->wifi = wifi;
            j->charging = charging;
            j->keep = keep;
            j->clean_days = clean_days;
            if (enqueue(j)) free(j);
            else queued = 1;
        }
    }
    return queued;
}
