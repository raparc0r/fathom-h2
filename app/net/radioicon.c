/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* Radio stations' icons, fetched and kept small on the card (radioicon.h). */
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE
#define _GNU_SOURCE   /* syscall (glibc, musl) */
#include "net/radioicon.h"

#include <ctype.h>
#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/resource.h>
#include <sys/syscall.h>
#endif

#include "core/fsafe.h"
#include "library/library.h"   /* library_logo_decode, LIB_ART_* */
#include "net/http.h"
#include "net/podcast.h"       /* pod_hash */

#define SMALL_BYTES ((size_t)LIB_ART_SMALL * LIB_ART_SMALL * 2)
#define LARGE_BYTES ((size_t)LIB_ART_LARGE * LIB_ART_LARGE * 2)
#define OFFLINE_AFTER 3        /* fetches in a row that reached no server: the network's gone, the rest wait */

/* A run's work: the icon addresses of the stations as they were when asked */
typedef struct {
    int n, net;
    char (*icon)[sizeof ((radio_station_t *)0)->icon];
} job_t;

static struct {
    pthread_mutex_t mu;
    char dir[512];
    int started;
    int running;               /* a thread is at work (under mu) */
    job_t *pending;            /* the list given while it was: its turn next (under mu) */
    volatile int abort;        /* ricon_stop: the fetch under way and the run give up */
    unsigned seq;
} R = {.mu = PTHREAD_MUTEX_INITIALIZER};

static ricon_fetch_fn g_fetch = http_get;
void ricon_set_fetch(ricon_fetch_fn fn) { g_fetch = fn ? fn : http_get; }

void ricon_id(const char *icon_url, char id[13])
{
    id[0] = '\0';
    if (radio_icon_ok(icon_url)) pod_hash(icon_url, id);
}

static void id_path(const char *id, const char *ext, char *out, size_t cap)
{
    snprintf(out, cap, "%s/%s.%s", R.dir, id, ext);
}

int ricon_path(const char *icon_url, char *out, size_t cap)
{
    char id[13];
    ricon_id(icon_url, id);
    if (cap) out[0] = '\0';
    if (!id[0] || !R.dir[0]) return -1;
    id_path(id, "565", out, cap);
    return 0;
}

int ricon_read(const char *id, int size, uint16_t *px)
{
    if (!id || strlen(id) != 12 || !R.dir[0] || (size != LIB_ART_SMALL && size != LIB_ART_LARGE)) return 0;
    char p[700];
    id_path(id, "565", p, sizeof p);
    FILE *f = fopen(p, "rbe");
    if (!f) return 0;
    size_t want = size == LIB_ART_SMALL ? SMALL_BYTES : LARGE_BYTES;
    int ok = (size == LIB_ART_SMALL || fseek(f, (long)SMALL_BYTES, SEEK_SET) == 0) && fread(px, 1, want, f) == want;
    fclose(f);
    return ok;
}

unsigned ricon_seq(void)
{
    pthread_mutex_lock(&R.mu);
    unsigned s = R.seq;
    pthread_mutex_unlock(&R.mu);
    return s;
}

int ricon_busy(void)
{
    pthread_mutex_lock(&R.mu);
    int b = R.running;
    pthread_mutex_unlock(&R.mu);
    return b;
}

/* ---------- a run ---------- */

static int exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

/* "0123456789ab.565" or ".none": the id into id, else 0 */
static int icon_file(const char *name, char id[13])
{
    size_t n = strlen(name);
    if (n < 14 || name[12] != '.' || (strcmp(name + 13, "565") && strcmp(name + 13, "none"))) return 0;
    for (int i = 0; i < 12; i++)
        if (!isxdigit((unsigned char)name[i])) return 0;
    memcpy(id, name, 12);
    id[12] = '\0';
    return 1;
}

/* What a run cut short left (*.tmp: only a run writes them, and there's one
 * at a time), and the icons of stations that aren't in the list any more */
static void sweep(const job_t *j, char (*ids)[13])
{
    DIR *d = opendir(R.dir);
    struct dirent *e;
    char p[900], id[13];
    while (d && (e = readdir(d))) {
        size_t n = strlen(e->d_name);
        int drop = n > 4 && !strcmp(e->d_name + n - 4, ".tmp");
        if (!drop && icon_file(e->d_name, id)) {
            drop = 1;
            for (int i = 0; i < j->n && drop; i++) drop = strcmp(ids[i], id) != 0;
        }
        if (drop) {
            snprintf(p, sizeof p, "%s/%s", R.dir, e->d_name);
            remove(p);
        }
    }
    if (d) closedir(d);
}

/* path written whole: beside (path.tmp), on the card, then moved over. 0, -1 */
static int put(const char *path, const void *a, size_t an, const void *b, size_t bn)
{
    char tmp[720];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    mkdir(R.dir, 0755);
    FILE *f = fopen(tmp, "wbe");
    if (!f) return -1;
    int ok = (!an || fwrite(a, 1, an, f) == an) && (!bn || fwrite(b, 1, bn, f) == bn) && fflush(f) == 0;
    ok &= fsync(fileno(f)) == 0;
    ok &= fclose(f) == 0;
    if (!ok || file_replace(tmp, path)) { remove(tmp); return -1; }
    return 0;
}

/* One icon: 1 its picture is on the card now, 0 it's no use (marked), -1 the
 * server wasn't reached (nothing kept: another time) */
static int fetch_one(const char *url, const char *id)
{
    char path[700], none[700];
    id_path(id, "565", path, sizeof path);
    id_path(id, "none", none, sizeof none);
    /* (one buffer a fetch, given back after it: nothing held between icons) */
    unsigned char *buf = malloc(RICON_MAX_FILE + 1);
    uint16_t *large = malloc(LARGE_BYTES), *small = malloc(SMALL_BYTES);
    int r = -1;
    if (buf && large && small) {
        long n = g_fetch(url, (char *)buf, RICON_MAX_FILE + 1, &R.abort);
        int side = 0;
        if (R.abort || n == -1) {
            r = -1;
        } else if (n < 8 || n == -2 || n == -3) {
            r = 0;                                   /* not there, or over the cap */
        } else if (memcmp(buf, "\xff\xd8\xff", 3) && memcmp(buf, "\x89PNG\r\n\x1a\n", 8)) {
            r = 0;                                   /* what it is, not what its name says: an .ico, SVG, WebP, a web page */
        } else if (library_logo_decode(buf, (size_t)n, large, small, &side) || side < RICON_SMALL_MIN) {
            r = 0;
        } else {
            int both = side >= RICON_LARGE_MIN;      /* (a favicon blown up to Now Playing's size is a blur: the list only) */
            r = put(path, small, SMALL_BYTES, large, both ? LARGE_BYTES : 0) == 0 ? 1 : -1;
        }
        if (r == 0) put(none, NULL, 0, NULL, 0);
    }
    free(buf);
    free(large);
    free(small);
    return r;
}

/* Icons whose server wasn't reached, and how often, since the player
 * started (the worker's own: one run at a time). One that can't be had (a
 * host that refuses the connection every time) was asked again at every
 * opening of Radio, a handshake each time: twice a session is enough. */
#define UNREACHED_MAX 2
static struct { char id[13]; int n; } g_unreached[48];

static int unreached_often(const char *id, int note)
{
    int free_at = -1;
    for (size_t i = 0; i < sizeof g_unreached / sizeof *g_unreached; i++) {
        if (!g_unreached[i].id[0]) { if (free_at < 0) free_at = (int)i; continue; }
        if (strcmp(g_unreached[i].id, id)) continue;
        if (note) g_unreached[i].n++;
        return g_unreached[i].n >= UNREACHED_MAX;
    }
    if (note && free_at >= 0) {
        snprintf(g_unreached[free_at].id, sizeof g_unreached[free_at].id, "%s", id);
        g_unreached[free_at].n = 1;
    }
    return 0;
}

static void run_job(const job_t *j)
{
    char (*ids)[13] = calloc((size_t)(j->n > 0 ? j->n : 1), sizeof *ids);
    if (!ids) return;
    for (int i = 0; i < j->n; i++) ricon_id(j->icon[i], ids[i]);
    sweep(j, ids);
    int unreached = 0;
    for (int i = 0; i < j->n && j->net && !R.abort && unreached < OFFLINE_AFTER; i++) {
        char path[700], none[700];
        if (!ids[i][0]) continue;
        id_path(ids[i], "565", path, sizeof path);
        id_path(ids[i], "none", none, sizeof none);
        if (exists(path) || exists(none) || unreached_often(ids[i], 0)) continue;
        int r = fetch_one(j->icon[i], ids[i]);
        unreached = r < 0 ? unreached + 1 : 0;
        if (r < 0 && !R.abort) unreached_often(ids[i], 1);
        if (r == 1) {
            pthread_mutex_lock(&R.mu);
            R.seq++;
            pthread_mutex_unlock(&R.mu);
        }
    }
    free(ids);
}

static void job_free(job_t *j)
{
    if (j) free(j->icon);
    free(j);
}

static void *run(void *arg)
{
    job_t *j = arg;
#ifdef __linux__
    /* below the music and the UI (as the scan and the updater): a logo's
     * HTTPS handshake and its decoding are work for the H2's one core, just
     * when a station is likely to be started */
    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 19);
#endif
    while (j) {
        if (!R.abort) run_job(j);
        job_free(j);
        pthread_mutex_lock(&R.mu);
        j = R.pending;                               /* a list given meanwhile: its turn */
        R.pending = NULL;
        if (!j) R.running = 0;
        pthread_mutex_unlock(&R.mu);
    }
    return NULL;
}

void ricon_sync(const radio_station_t *st, int n, int net)
{
    if (!R.started || n < 0 || (n > 0 && !st)) return;
    job_t *j = calloc(1, sizeof *j);
    if (j) j->icon = calloc((size_t)(n > 0 ? n : 1), sizeof *j->icon);
    if (!j || !j->icon) { job_free(j); return; }
    j->n = n;
    j->net = net;
    for (int i = 0; i < n; i++) memcpy(j->icon[i], st[i].icon, sizeof j->icon[i]);
    pthread_mutex_lock(&R.mu);
    if (R.running) {                                 /* after the run under way (a list waiting already: replaced) */
        job_free(R.pending);
        R.pending = j;
        pthread_mutex_unlock(&R.mu);
        return;
    }
    R.running = 1;
    pthread_mutex_unlock(&R.mu);
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&a, 128 * 1024);       /* (as the radio's other fetches: device/radiometa.c) */
    int r = pthread_create(&t, &a, run, j);
    pthread_attr_destroy(&a);
    if (r) {
        job_free(j);
        pthread_mutex_lock(&R.mu);
        R.running = 0;
        pthread_mutex_unlock(&R.mu);
    }
}

int ricon_start(const char *dir)
{
    pthread_mutex_lock(&R.mu);
    snprintf(R.dir, sizeof R.dir, "%s", dir ? dir : "");
    R.abort = 0;
    R.started = R.dir[0] != '\0';
    pthread_mutex_unlock(&R.mu);
    return 0;
}

void ricon_stop(void)
{
    pthread_mutex_lock(&R.mu);
    R.started = 0;
    R.abort = 1;
    job_free(R.pending);
    R.pending = NULL;
    pthread_mutex_unlock(&R.mu);
    for (int i = 0; i < 100 && ricon_busy(); i++) {  /* (its fetch ends on the flag; a decode is under a second) */
        struct timespec ts = {0, 50 * 1000 * 1000L};
        nanosleep(&ts, NULL);
    }
}
