/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Updates over Wi-Fi (Settings > About > Check for updates), on a worker
 * thread at the lowest priority (nice 19: the audio thread is SCHED_FIFO and
 * the UI comes first too), one step at a time, each only when asked:
 *
 *   check     GitHub's latest release (pre-releases and drafts aren't
 *             "latest"); offered only if it's newer than this one
 *   start     the zip to the card (fathom/update/), its size and GitHub's
 *             SHA-256 checked, its sd/ unpacked beside it and every file
 *             checked against the zip's SHA256SUMS; streamed throughout
 *   install   the files renamed into place, the kernel last; then the
 *             player restarts the H2 (updater_installed)
 *
 * Back cancels anything before the install, and the stage goes. Space on the
 * card is checked before the download (an estimate) and after it (exactly).
 */
#include "updater.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "net/http.h"
#include "net/update.h"

#define ANSWER_MAX (64 << 10)        /* GitHub's answer for 0.7.0: 8 KB, most of it the notes */

static struct {
    pthread_mutex_t mu;
    char card[256], stage[300], zip[512], api[512];
    ui_update_t st;
    update_release_t rel;
    unsigned seq;
    volatile int abort;
    int running;                     /* a worker is out */
    int installed;
} U = {.mu = PTHREAD_MUTEX_INITIALIZER};

static void changed_locked(void) { U.seq++; }

/* The worker's next step (not once cancelled: the screen has gone). */
static void set_state(int state, uint64_t total)
{
    pthread_mutex_lock(&U.mu);
    if (!U.abort) {
        U.st.state = state;
        U.st.done = 0;
        U.st.total = total;
        changed_locked();
    }
    pthread_mutex_unlock(&U.mu);
}

/* (with the lock) */
static void fail_locked(int err, const char *text)
{
    U.st.state = UI_UPD_FAILED;
    snprintf(U.st.message, sizeof U.st.message, "%s", text ? text : update_error_text(err));
    changed_locked();
}

static void progress(uint64_t done, uint64_t total, void *ctx)
{
    (void)ctx;
    pthread_mutex_lock(&U.mu);
    U.st.done = done;
    U.st.total = total;
    changed_locked();
    pthread_mutex_unlock(&U.mu);
}

/* The download's: its total is the size GitHub gave (the server may not say). */
static void dl_progress(uint64_t done, uint64_t total, void *ctx)
{
    (void)total;
    progress(done, (uint64_t)U.rel.size, ctx);
}

static void low_priority(void)
{
    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 19);
}

/* ---- the steps (each its own thread) ---- */

static void *check_thread(void *arg)
{
    (void)arg;
    low_priority();
    char *buf = malloc(ANSWER_MAX);
    long n = buf ? http_get(U.api, buf, ANSWER_MAX, &U.abort) : -1;
    update_release_t r;
    int parsed = n > 0 ? update_parse_release(buf, (size_t)n, &r) : -1;
    if (n <= 0) memset(&r, 0, sizeof r);
    fprintf(stderr, "update: asked %s: %ld bytes, latest %s, %s\n", U.api, n, r.tag[0] ? r.tag : "-",
            parsed ? "no zip" : r.asset);
    free(buf);
    pthread_mutex_lock(&U.mu);
    snprintf(U.st.latest, sizeof U.st.latest, "%s", r.tag);
    if (U.abort) U.st.state = UI_UPD_IDLE;
    else if (n < 0) fail_locked(UPDATE_ERR_NET, NULL);
    else if (n == 0) fail_locked(UPDATE_ERR_ANSWER, NULL);
    else if (r.tag[0] && !update_is_newer(r.tag, U.st.current)) U.st.state = UI_UPD_LATEST;
    else if (parsed) fail_locked(UPDATE_ERR_ANSWER, NULL);   /* newer, but nothing to install */
    else {
        U.rel = r;
        U.st.size = r.size;
        U.st.state = UI_UPD_AVAILABLE;
    }
    U.running = 0;
    changed_locked();
    pthread_mutex_unlock(&U.mu);
    return NULL;
}

/* Not enough room: says how much is needed, in MB. */
static void no_space_locked(int64_t need)
{
    char t[96];
    snprintf(t, sizeof t, "%s: %d MB needed", update_error_text(UPDATE_ERR_SPACE), (int)((need + (1 << 20) - 1) >> 20));
    fail_locked(UPDATE_ERR_SPACE, t);
}

static void *download_thread(void *arg)
{
    (void)arg;
    low_priority();
    const update_release_t *r = &U.rel;
    int err = UPDATE_OK, nospace = 0;
    long cluster = 32768;
    int64_t free_now = update_free_space(U.card, &cluster);
    /* The zip, then its sd/ (about twice the zip) beside it, and the kernel's
     * copy: an estimate here, exact once the zip is in */
    int64_t need = r->size * 3 + (16 << 20);
    if (free_now >= 0 && free_now < need) { nospace = 1; goto out; }
    update_remove_tree(U.stage);
    if (mkdir(U.stage, 0755)) { err = UPDATE_ERR_WRITE; goto out; }
    snprintf(U.zip, sizeof U.zip, "%s/%s", U.stage, r->asset);
    set_state(UI_UPD_DOWNLOADING, (uint64_t)r->size);
    int64_t got = http_download(r->url, U.zip, dl_progress, NULL, &U.abort);
    fprintf(stderr, "update: %s: %lld of %lld bytes\n", r->url, (long long)got, (long long)r->size);
    if (U.abort) { err = UPDATE_ERR_CANCELLED; goto out; }
    if (got < 0) { err = UPDATE_ERR_DOWNLOAD; goto out; }
    if (got != r->size) { err = UPDATE_ERR_DAMAGED; goto out; }
    if (r->sha256[0]) {
        char hex[65];
        set_state(UI_UPD_VERIFYING, (uint64_t)r->size);
        if (update_file_sha256(U.zip, hex, progress, NULL, &U.abort)) {
            err = U.abort ? UPDATE_ERR_CANCELLED : UPDATE_ERR_WRITE;
            goto out;
        }
        if (strcmp(hex, r->sha256)) {
            fprintf(stderr, "update: SHA-256 %s, GitHub says %s\n", hex, r->sha256);
            err = UPDATE_ERR_DAMAGED;
            goto out;
        }
    }
    char kernel[300];
    struct stat st;
    snprintf(kernel, sizeof kernel, "%s/uImage", U.card);
    need = update_stage_needs(U.zip, cluster, stat(kernel, &st) == 0 ? (int64_t)st.st_size : 0);
    if (need < 0) { err = UPDATE_ERR_ZIP; goto out; }
    free_now = update_free_space(U.card, NULL);
    if (free_now >= 0 && free_now < need) { nospace = 1; goto out; }
    set_state(UI_UPD_UNPACKING, 0);
    err = update_stage(U.zip, U.stage, progress, NULL, &U.abort);
    unlink(U.zip);                   /* (its room is the kernel copy's at the install) */
    fprintf(stderr, "update: staged %s: %s\n", r->tag, err ? update_error_text(err) : "ok");
out:
    if (err != UPDATE_OK || nospace || U.abort) update_remove_tree(U.stage);
    pthread_mutex_lock(&U.mu);
    if (err == UPDATE_ERR_CANCELLED || U.abort) U.st.state = UI_UPD_AVAILABLE;   /* (offered again) */
    else if (err) fail_locked(err, NULL);
    else if (nospace) no_space_locked(need);
    else U.st.state = UI_UPD_READY;
    U.st.done = U.st.total = 0;
    U.running = 0;
    changed_locked();
    pthread_mutex_unlock(&U.mu);
    return NULL;
}

static void *install_thread(void *arg)
{
    (void)arg;
    int err = update_install(U.card, U.stage);
    fprintf(stderr, "update: install %s: %s\n", U.rel.tag, err ? "failed" : "done");
    pthread_mutex_lock(&U.mu);
    /* A failed install keeps its marker: the next start tries again */
    if (err) fail_locked(err, "Couldn't install it: restart the H2 to try again");
    else { U.st.state = UI_UPD_RESTARTING; U.installed = 1; }
    U.running = 0;
    changed_locked();
    pthread_mutex_unlock(&U.mu);
    return NULL;
}

static void *discard_thread(void *arg)
{
    (void)arg;
    low_priority();
    update_remove_tree(U.stage);
    pthread_mutex_lock(&U.mu);
    U.running = 0;
    changed_locked();
    pthread_mutex_unlock(&U.mu);
    return NULL;
}

/* (with the lock) A step on its thread; 0 if one is already out. */
static int run_locked(void *(*fn)(void *))
{
    if (U.running) return 0;
    pthread_t th;
    U.running = 1;
    U.abort = 0;
    if (pthread_create(&th, NULL, fn, NULL)) { U.running = 0; return 0; }
    pthread_detach(th);
    return 1;
}

/* ---- the platform's side ---- */

void updater_init(const char *card, const char *version, const char *api)
{
    pthread_mutex_lock(&U.mu);
    snprintf(U.card, sizeof U.card, "%s", card);
    snprintf(U.stage, sizeof U.stage, "%s/fathom/update", card);
    snprintf(U.api, sizeof U.api, "%s", api && api[0] ? api : UPDATE_API);
    snprintf(U.st.current, sizeof U.st.current, "%s", version);
    U.st.state = UI_UPD_IDLE;
    pthread_mutex_unlock(&U.mu);
}

void updater_ui_state(ui_update_t *st)
{
    pthread_mutex_lock(&U.mu);
    *st = U.st;
    pthread_mutex_unlock(&U.mu);
}

void updater_do(int what)
{
    pthread_mutex_lock(&U.mu);
    int s = U.st.state;
    switch (what) {
    case UI_UPD_CHECK:
        if (s <= UI_UPD_AVAILABLE || s == UI_UPD_FAILED) {
            if (run_locked(check_thread)) { U.st.state = UI_UPD_CHECKING; U.st.message[0] = '\0'; }
        }
        break;
    case UI_UPD_START:
        if (s == UI_UPD_AVAILABLE && run_locked(download_thread)) {
            U.st.state = UI_UPD_DOWNLOADING;
            U.st.done = 0;
            U.st.total = (uint64_t)U.rel.size;
        }
        break;
    case UI_UPD_CANCEL:
        if (s >= UI_UPD_DOWNLOADING && s <= UI_UPD_UNPACKING) {
            U.abort = 1;                                 /* the worker cleans up */
            U.st.state = UI_UPD_AVAILABLE;
        } else if (s == UI_UPD_READY) {
            U.st.state = UI_UPD_AVAILABLE;
            run_locked(discard_thread);
        } else if (s == UI_UPD_CHECKING) {
            U.abort = 1;
        }
        break;
    case UI_UPD_INSTALL:
        if (s == UI_UPD_READY && run_locked(install_thread)) U.st.state = UI_UPD_INSTALLING;
        break;
    }
    changed_locked();
    pthread_mutex_unlock(&U.mu);
}

void updater_refuse(const char *why)
{
    pthread_mutex_lock(&U.mu);
    if (!U.running && U.st.state < UI_UPD_DOWNLOADING) fail_locked(UPDATE_ERR_NET, why);
    pthread_mutex_unlock(&U.mu);
}

unsigned updater_seq(void)
{
    pthread_mutex_lock(&U.mu);
    unsigned s = U.seq;
    pthread_mutex_unlock(&U.mu);
    return s;
}

int updater_busy(void)
{
    pthread_mutex_lock(&U.mu);
    int b = U.st.state >= UI_UPD_DOWNLOADING && U.st.state <= UI_UPD_RESTARTING;
    pthread_mutex_unlock(&U.mu);
    return b;
}

int updater_installed(void)
{
    pthread_mutex_lock(&U.mu);
    int i = U.installed;
    U.installed = 0;
    pthread_mutex_unlock(&U.mu);
    return i;
}
