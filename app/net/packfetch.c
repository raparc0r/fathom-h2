/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
#include "net/packfetch.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <stdint.h>

#include "core/lang.h"
#include "net/http.h"
#include "net/sha256.h"
#include "net/unzip.h"
#include "net/update.h"

/* The packs there are. Each is made by tools/fonts/mkpack.py (the same bytes
 * every time) and published once on a release of its own; the size and the
 * digest here are that file's, so a player only ever uses the pack it was
 * built to read. */
static const pack_info_t BUILT_IN[] = {
    {"ja", N_("Japanese"), "glyphs-ja.bin",
     "https://github.com/raparc0r/fathom-h2/releases/download/packs-1/glyphs-ja.zip",
     3304868, 5363578, "a258afedbad63a6e5ee9b8e954ffd2f603fdfc8e9edaf4b6599289bc2342da25", 1, N_("Japanese")},
    {"ko", N_("Korean"), "glyphs-ko.bin",
     "https://github.com/raparc0r/fathom-h2/releases/download/packs-1/glyphs-ko.zip",
     2553390, 8824862, "8f09625f49b261a1832a8cc264d253dfaf064d517ba8b47b35fef11a748baa23", 0, NULL},
    {"zh-hans", N_("Chinese (Simplified)"), "glyphs-zh-hans.bin",
     "https://github.com/raparc0r/fathom-h2/releases/download/packs-1/glyphs-zh-hans.zip",
     3282028, 5382400, "e4a800b3a31d174a8d433dcd56ab417cdac9093b827fa1d2b78559ad93054513", 1, N_("Simplified")},
    {"zh-hant", N_("Chinese (Traditional)"), "glyphs-zh-hant.bin",
     "https://github.com/raparc0r/fathom-h2/releases/download/packs-1/glyphs-zh-hant.zip",
     6637170, 10039024, "1fde7a771868b3479d96b69f8143d34ee69b7407900519148e6c14b005d2bb77", 1, N_("Traditional")},
};

static const pack_info_t *g_list = BUILT_IN;
static int g_n = (int)(sizeof BUILT_IN / sizeof BUILT_IN[0]);

void packs_test_list(const pack_info_t *list, int n)
{
    g_list = list ? list : BUILT_IN;
    g_n = list ? n : (int)(sizeof BUILT_IN / sizeof BUILT_IN[0]);
}

static char g_base[400];

void packs_url_base(const char *base)
{
    snprintf(g_base, sizeof g_base, "%s", base ? base : "");
    size_t n = strlen(g_base);
    while (n && g_base[n - 1] == '/') g_base[--n] = '\0';
}

/* Where the pack's zip is fetched from: its own address, or the same file's
 * name under the other place */
static void url_of(const pack_info_t *p, char *out, size_t cap)
{
    const char *name = strrchr(p->url, '/');
    if (g_base[0] && name) snprintf(out, cap, "%s%s", g_base, name);
    else snprintf(out, cap, "%s", p->url);
}

int packs_count(void) { return g_n; }
const pack_info_t *packs_info(int i) { return i >= 0 && i < g_n ? &g_list[i] : NULL; }

const char *pack_error_text(int err)
{
    switch (err) {
    case PACK_OK: return "";
    case PACK_ERR_NET: return N_("Couldn't reach the server");
    case PACK_ERR_SPACE: return N_("Not enough space on the card");
    case PACK_ERR_DOWNLOAD: return N_("Couldn't download it");          /* (the server answered, and not with the file) */
    case PACK_ERR_DAMAGED: return N_("The download was damaged");
    case PACK_ERR_WRITE: return N_("Couldn't write to the card");
    case PACK_ERR_CANCELLED: return N_("Cancelled");
    default: return N_("Couldn't download it");
    }
}

static void paths(const char *dir, const pack_info_t *p, char *file, char *part, char *zip, size_t cap)
{
    snprintf(file, cap, "%s/%s", dir, p->file);
    snprintf(part, cap, "%s/%s.part", dir, p->file);
    snprintf(zip, cap, "%s/%s.zip", dir, p->file);
}

int pack_installed(const char *dir, const pack_info_t *p)
{
    char file[700], part[700], zip[700];
    struct stat st;
    if (!dir || !p) return 0;
    paths(dir, p, file, part, zip, sizeof file);
    return stat(file, &st) == 0 && S_ISREG(st.st_mode);
}

/* What a fetch cut short (the power, a reset) left: the half-written file, the
 * zip, and the download's own half-written copy of the zip */
static void leftovers(const char *part, const char *zip)
{
    char zpart[720];
    snprintf(zpart, sizeof zpart, "%s.part", zip);
    unlink(part);
    unlink(zip);
    unlink(zpart);
}

int pack_remove(const char *dir, const pack_info_t *p)
{
    char file[700], part[700], zip[700];
    if (!dir || !p) return -1;
    paths(dir, p, file, part, zip, sizeof file);
    leftovers(part, zip);
    return unlink(file) == 0 || errno == ENOENT ? 0 : -1;
}

typedef struct { pack_progress_fn fn; void *ctx; int last; } prog_t;

static void say(prog_t *g, int pct)
{
    if (pct > 100) pct = 100;
    if (!g->fn || pct == g->last) return;
    g->last = pct;
    g->fn(pct, g->ctx);
}

static void on_download(uint64_t done, uint64_t total, void *ctx)
{
    if (total) say(ctx, (int)(done * 90 / total));
}

typedef struct { FILE *f; sha256_t sum; uint64_t done, want; prog_t *g; int failed; } sink_t;

/* What the zip gives: to the file and into the digest, and no more of it than
 * the pack is (a zip that says one length and holds another) */
static int to_file(const void *data, size_t len, void *ctx)
{
    sink_t *s = ctx;
    if (s->done + len > s->want) return -1;
    if (fwrite(data, 1, len, s->f) != len) { s->failed = 1; return -1; }
    sha256_update(&s->sum, data, len);
    s->done += len;
    say(s->g, 90 + (int)(s->done * 10 / (s->want ? s->want : 1)));
    return 0;
}

int pack_fetch(const char *dir, const pack_info_t *p, pack_progress_fn progress, void *ctx, const volatile int *abort)
{
    char file[700], part[700], zipp[700];
    prog_t g = {progress, ctx, -1};
    if (!dir || !p) return PACK_ERR_WRITE;
    if (mkdir(dir, 0755) && errno != EEXIST) return PACK_ERR_WRITE;
    paths(dir, p, file, part, zipp, sizeof file);
    leftovers(part, zipp);

    /* room for the zip and the file beside it, and a megabyte over */
    int64_t space = update_free_space(dir, NULL);
    if (space >= 0 && space < p->zip_size + p->size + (1 << 20)) return PACK_ERR_SPACE;

    say(&g, 0);
    int err = PACK_OK;
    char url[900];
    url_of(p, url, sizeof url);
    int64_t got = http_download_retry(url, zipp, on_download, &g, abort);
    if (abort && *abort) err = PACK_ERR_CANCELLED;
    else if (got == HTTP_ERR_FULL) err = PACK_ERR_SPACE;
    else if (got == HTTP_ERR_WRITE) err = PACK_ERR_WRITE;
    else if (got == -1) err = PACK_ERR_NET;
    else if (got <= 0) err = PACK_ERR_DOWNLOAD;     /* (the server answered, and not with the file) */
    if (err) { leftovers(part, zipp); return err; }

    /* the pack's one file out of the zip, into its digest as it is written */
    zip_t z;
    zip_entry_t e;
    int found = 0;
    if (zip_open(&z, zipp)) { unlink(zipp); return PACK_ERR_DAMAGED; }
    while (zip_next(&z, &e) == 1)
        if (!e.dir && !strcmp(e.name, p->file)) { found = 1; break; }
    err = PACK_ERR_DAMAGED;
    if (found && (int64_t)e.usize == p->size) {
        sink_t s;
        memset(&s, 0, sizeof s);
        s.f = fopen(part, "wbe");
        s.want = (uint64_t)p->size;
        s.g = &g;
        if (!s.f) err = PACK_ERR_WRITE;
        else {
            sha256_init(&s.sum);
            int bad = zip_extract(&z, &e, to_file, &s, abort);
            if (fflush(s.f) || fsync(fileno(s.f))) s.failed = 1;
            if (fclose(s.f)) s.failed = 1;
            uint8_t digest[32];
            char hex[65];
            sha256_final(&s.sum, digest);
            sha256_hex(digest, hex);
            if (abort && *abort) err = PACK_ERR_CANCELLED;
            else if (s.failed) err = PACK_ERR_WRITE;
            else if (!bad && s.done == s.want && !strcmp(hex, p->sha256)) err = PACK_OK;
        }
    }
    zip_close(&z);
    unlink(zipp);
    if (err == PACK_OK && rename(part, file)) err = PACK_ERR_WRITE;
    if (err) unlink(part);
    else say(&g, 100);
    return err;
}

/* ---------- one fetch at a time, on a thread of its own ---------- */

static struct {
    char dir[600];
    pthread_mutex_t mu;
    pthread_t th;
    int started;            /* a thread was made and not joined yet */
    atomic_int running;     /* ...and is still at work */
    int job;                /* the pack it fetches (-1: none) */
    int percent;
    int failed;             /* the pack whose fetch failed last (-1: none) */
    char why[64];
    volatile int abort;
    atomic_uint seq, gen;
} J = {.mu = PTHREAD_MUTEX_INITIALIZER, .job = -1, .failed = -1};

void packs_init(const char *dir)
{
    pthread_mutex_lock(&J.mu);
    snprintf(J.dir, sizeof J.dir, "%s", dir ? dir : "");
    pthread_mutex_unlock(&J.mu);
    atomic_fetch_add(&J.seq, 1);
}

static void job_progress(int pct, void *ctx)
{
    (void)ctx;
    pthread_mutex_lock(&J.mu);
    J.percent = pct;
    pthread_mutex_unlock(&J.mu);
    atomic_fetch_add(&J.seq, 1);
}

static void *job_thread(void *arg)
{
    const pack_info_t *p = arg;
    char dir[600];
    pthread_mutex_lock(&J.mu);
    snprintf(dir, sizeof dir, "%s", J.dir);
    pthread_mutex_unlock(&J.mu);
    int err = pack_fetch(dir, p, job_progress, NULL, &J.abort);
    pthread_mutex_lock(&J.mu);
    if (err && err != PACK_ERR_CANCELLED) {
        J.failed = J.job;
        snprintf(J.why, sizeof J.why, "%s", pack_error_text(err));
    }
    J.job = -1;
    pthread_mutex_unlock(&J.mu);
    if (!err) atomic_fetch_add(&J.gen, 1);
    atomic_fetch_add(&J.seq, 1);
    atomic_store(&J.running, 0);
    return NULL;
}

/* (with J.mu held) the thread of a fetch that has ended, joined */
static void reap(void)
{
    if (J.started && !atomic_load(&J.running)) {
        pthread_join(J.th, NULL);
        J.started = 0;
    }
}

void packs_status(int i, pack_status_t *out)
{
    memset(out, 0, sizeof *out);
    const pack_info_t *p = packs_info(i);
    if (!p) return;
    pthread_mutex_lock(&J.mu);
    if (J.job == i) {
        out->state = PACK_GETTING;
        out->percent = J.percent;
    } else if (J.failed == i) {
        out->state = PACK_FAILED;
        snprintf(out->why, sizeof out->why, "%s", J.why);
    } else {
        out->state = pack_installed(J.dir, p) ? PACK_THERE : PACK_ABSENT;
    }
    pthread_mutex_unlock(&J.mu);
}

int packs_get(int i)
{
    const pack_info_t *p = packs_info(i);
    if (!p) return -1;
    pthread_mutex_lock(&J.mu);
    reap();
    if (J.started || !J.dir[0]) { pthread_mutex_unlock(&J.mu); return -1; }
    J.job = i;
    J.percent = 0;
    J.failed = -1;
    J.abort = 0;
    atomic_store(&J.running, 1);
    /* (const away: the thread only reads it) */
    if (pthread_create(&J.th, NULL, job_thread, (void *)(uintptr_t)p)) {
        atomic_store(&J.running, 0);
        J.job = -1;
        J.failed = i;
        snprintf(J.why, sizeof J.why, "%s", pack_error_text(PACK_ERR_WRITE + 100));
        pthread_mutex_unlock(&J.mu);
        atomic_fetch_add(&J.seq, 1);
        return -1;
    }
    J.started = 1;
    pthread_mutex_unlock(&J.mu);
    atomic_fetch_add(&J.seq, 1);
    return 0;
}

void packs_cancel(void)
{
    J.abort = 1;
}

int packs_delete(int i)
{
    const pack_info_t *p = packs_info(i);
    if (!p) return -1;
    pthread_mutex_lock(&J.mu);
    reap();
    int r = J.job == i ? -1 : pack_remove(J.dir, p);
    if (J.failed == i) J.failed = -1;
    pthread_mutex_unlock(&J.mu);
    if (!r) atomic_fetch_add(&J.gen, 1);
    atomic_fetch_add(&J.seq, 1);
    return r;
}

void packs_refuse(int i, const char *why)
{
    if (!packs_info(i)) return;
    pthread_mutex_lock(&J.mu);
    if (J.job != i) {
        J.failed = i;
        snprintf(J.why, sizeof J.why, "%s", why ? why : "");
    }
    pthread_mutex_unlock(&J.mu);
    atomic_fetch_add(&J.seq, 1);
}

unsigned packs_seq(void) { return atomic_load(&J.seq); }
unsigned packs_gen(void) { return atomic_load(&J.gen); }
int packs_busy(void) { return atomic_load(&J.running); }

void packs_stop_ask(void) { J.abort = 1; }

int packs_stop_wait(int ms)
{
    struct timespec t0, t;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    while (atomic_load(&J.running)) {
        clock_gettime(CLOCK_MONOTONIC, &t);
        if (ms >= 0 && (t.tv_sec - t0.tv_sec) * 1000 + (t.tv_nsec - t0.tv_nsec) / 1000000 >= ms) return 0;
        usleep(20000);
    }
    pthread_mutex_lock(&J.mu);
    reap();
    pthread_mutex_unlock(&J.mu);
    return 1;
}
