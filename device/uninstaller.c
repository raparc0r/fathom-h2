/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/*
 * Uninstall Fathom (Settings > About > Uninstall Fathom), on a worker thread
 * at the lowest priority (nice 19, as the updater's), one step at a time,
 * each only when asked:
 *
 *   check     HiBy's update file found at the card's top and read whole
 *             (every image against its MD5, its bootloader HiBy's): what
 *             it is, shown before anything changes
 *   apply     checked again, the card's space too; its bootloader listed and
 *             the file named update.upt (app/sys/uninstall.c); then it waits
 *             for the restart into HiBy's recovery (uninstaller_ask_recovery)
 *   cancel    everything put back (on the caller's thread: a sector and a
 *             rename or two)
 *
 * The battery is the player's to check (main.c), before apply.
 */
#include "uninstaller.h"
#include "core/thread.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "sys/hibyupt.h"
#include "sys/uninstall.h"

static struct {
    pthread_mutex_t mu;
    char card[256];
    ui_uninstall_t st;
    unsigned seq;
    int running;
} N = {.mu = PTHREAD_MUTEX_INITIALIZER};

static void set_state(int state)
{
    pthread_mutex_lock(&N.mu);
    N.st.state = state;
    N.st.done = N.st.total = 0;
    N.st.need_mb = 0;
    N.st.no_file = 0;
    N.st.message[0] = '\0';
    N.seq++;
    pthread_mutex_unlock(&N.mu);
}

static void fail(const char *text, int need_mb)
{
    pthread_mutex_lock(&N.mu);
    N.st.state = UI_UNI_FAILED;
    snprintf(N.st.message, sizeof N.st.message, "%s", text);
    N.st.need_mb = need_mb;
    N.seq++;
    pthread_mutex_unlock(&N.mu);
    fprintf(stderr, "uninstall: %s\n", text);
}

static void progress(void *ctx, uint64_t done, uint64_t total)
{
    (void)ctx;
    pthread_mutex_lock(&N.mu);
    N.st.done = done;
    N.st.total = total;
    N.seq++;
    pthread_mutex_unlock(&N.mu);
}

/* The file found, opened and checked: 0, or -1 having said why. */
static int find_and_check(upt_t *u, char *name, size_t cap, int writable)
{
    upt_err_t why = UPT_OK;
    uni_err_t f = uninstall_find(N.card, name, cap, &why);
    if (f) {
        fail(f == UNI_BAD ? upt_error_text(why) : uninstall_error_text(f), 0);
        if (f == UNI_NONE) {
            pthread_mutex_lock(&N.mu);
            N.st.no_file = 1;
            N.seq++;
            pthread_mutex_unlock(&N.mu);
        }
        return -1;
    }
    pthread_mutex_lock(&N.mu);
    snprintf(N.st.file, sizeof N.st.file, "%s", name);
    pthread_mutex_unlock(&N.mu);
    char p[600];
    snprintf(p, sizeof p, "%s/%s", N.card, name);
    upt_err_t e = upt_open(u, p, writable);
    if (!e) e = upt_check(u, progress, NULL);
    if (e) {
        fail(upt_error_text(e), 0);
        return -1;
    }
    /* not older than the stock firmware in the flash (h2boot's note) */
    if (uninstall_version_ok(N.card, u->ver) == UNI_OLDER) {
        fprintf(stderr, "uninstall: %s (%s) is older than the stock firmware in the flash\n", name, u->ver);
        fail(uninstall_error_text(UNI_OLDER), 0);
        return -1;
    }
    pthread_mutex_lock(&N.mu);
    snprintf(N.st.ver, sizeof N.st.ver, "%.10s", u->ver);   /* the date of "2025-11-11T18:59:20+08:00" */
    pthread_mutex_unlock(&N.mu);
    fprintf(stderr, "uninstall: %s: HiBy's %s, U-Boot built %s\n", name, u->ver, u->uboot_built);
    return 0;
}

static void *check_thread(void *arg)
{
    (void)arg;
    thread_nice(19);
    upt_t u;
    char name[256];
    u.fd = -1;
    set_state(UI_UNI_CHECKING);
    if (!find_and_check(&u, name, sizeof name, 0)) set_state(UI_UNI_READY);
    upt_close(&u);
    pthread_mutex_lock(&N.mu);
    N.running = 0;
    pthread_mutex_unlock(&N.mu);
    return NULL;
}

static void *apply_thread(void *arg)
{
    (void)arg;
    thread_nice(19);
    upt_t u;
    char name[256], p[600];
    u.fd = -1;
    set_state(UI_UNI_APPLYING);
    if (!find_and_check(&u, name, sizeof name, 1)) {
        struct stat st;
        int need = 0;
        upt_err_t why = UPT_OK;
        snprintf(p, sizeof p, "%s/%s", N.card, name);
        if (!stat(p, &st) && uninstall_space(N.card, (uint64_t)st.st_size, &need)) {
            fail(uninstall_error_text(UNI_NO_SPACE), need);
        } else {
            uni_err_t e = uninstall_apply(N.card, name, &u, &why);
            if (e) fail(e == UNI_BAD ? upt_error_text(why) : uninstall_error_text(e), 0);
            else {
                fprintf(stderr, "uninstall: %s made ready as update.upt\n", name);
                set_state(UI_UNI_PENDING);
            }
        }
    }
    upt_close(&u);
    pthread_mutex_lock(&N.mu);
    N.running = 0;
    pthread_mutex_unlock(&N.mu);
    return NULL;
}

void uninstaller_init(const char *card)
{
    snprintf(N.card, sizeof N.card, "%s", card);
    if (uninstall_forget_stale(card))   /* (installed by HiBy's recovery, then Fathom again) */
        fprintf(stderr, "uninstall: update.upt isn't the file made ready any more: forgotten\n");
    N.st.state = uninstall_pending(card) ? UI_UNI_PENDING : UI_UNI_IDLE;
}

void uninstaller_ui_state(ui_uninstall_t *st)
{
    pthread_mutex_lock(&N.mu);
    *st = N.st;
    pthread_mutex_unlock(&N.mu);
}

static void start(void *(*fn)(void *))
{
    pthread_mutex_lock(&N.mu);
    if (N.running) { pthread_mutex_unlock(&N.mu); return; }
    N.running = 1;
    pthread_mutex_unlock(&N.mu);
    if (thread_detached(fn, NULL, 64 * 1024)) {
        pthread_mutex_lock(&N.mu);
        N.running = 0;
        pthread_mutex_unlock(&N.mu);
        fail("Couldn't use the update file", 0);
    }
}

void uninstaller_do(int what)
{
    pthread_mutex_lock(&N.mu);
    int running = N.running, state = N.st.state;
    pthread_mutex_unlock(&N.mu);
    switch (what) {
    case UI_UNI_CHECK:
        if (state != UI_UNI_PENDING) start(check_thread);
        break;
    case UI_UNI_APPLY:
        if (state == UI_UNI_READY) start(apply_thread);
        break;
    case UI_UNI_CANCEL:
        if (running) break;
        if (uninstall_cancel(N.card)) fprintf(stderr, "uninstall: cancelled, not all of it put back\n");
        else fprintf(stderr, "uninstall: cancelled\n");
        set_state(UI_UNI_IDLE);
        break;
    default: break;
    }
}

unsigned uninstaller_seq(void)
{
    pthread_mutex_lock(&N.mu);
    unsigned s = N.seq;
    pthread_mutex_unlock(&N.mu);
    return s;
}

int uninstaller_busy(void)
{
    pthread_mutex_lock(&N.mu);
    int r = N.running;
    pthread_mutex_unlock(&N.mu);
    return r;
}

int uninstaller_pending(void)
{
    pthread_mutex_lock(&N.mu);
    int r = N.st.state == UI_UNI_PENDING;
    pthread_mutex_unlock(&N.mu);
    return r;
}

/* HiBy's own "boot the recovery for an update" signal: 0x1a1a in the low half
 * of the X1000's CPM scratch register, which the stock updater sets and which
 * h2boot honours (bootloader patch 0007; it clears it as it acts on it). The
 * register keeps its value through a reboot (checked on the H2, 2026-11-19).
 * It's write-protected: 0x5a5a to the protect register opens it, 0xa5a5
 * closes it (Rockbox's boot-x1000.h). */
#define CPM_BASE         0x10000000
#define CPM_SCRATCH      0x34
#define CPM_SCRATCH_PROT 0x38

int uninstaller_ask_recovery(void)
{
    int fd = open("/dev/mem", O_RDWR | O_SYNC | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "uninstall: /dev/mem: %s\n", strerror(errno));
        return -1;
    }
    void *m = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, CPM_BASE);
    close(fd);
    if (m == MAP_FAILED) {
        fprintf(stderr, "uninstall: mmap: %s\n", strerror(errno));
        return -1;
    }
    volatile uint32_t *scratch = (volatile uint32_t *)((char *)m + CPM_SCRATCH);
    volatile uint32_t *prot = (volatile uint32_t *)((char *)m + CPM_SCRATCH_PROT);
    uint32_t v = (*scratch & 0xffff0000u) | 0x1a1a;
    *prot = 0x5a5a;
    *scratch = v;
    *prot = 0xa5a5;
    uint32_t now = *scratch;
    munmap(m, 4096);
    fprintf(stderr, "uninstall: recovery signal 0x%08x\n", (unsigned)now);
    return (now & 0xffff) == 0x1a1a ? 0 : -1;
}
