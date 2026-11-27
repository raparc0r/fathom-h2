/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE                    /* pipe2 */
#endif
#include "reader.h"
#include "core/clock.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

#define PATH_MAX_SENT 4096
#define REPLY_MAX (8u * 1024 * 1024)   /* (a decoded cover is a few hundred KB, a sheet 256 KB at most) */

static char g_exe[512];
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pid_t g_pid;                    /* the helper (0: none running) */
static int g_to = -1, g_from = -1;     /* its stdin, its stdout */
static int g_timeout_ms = 60000;
static int g_deaths, g_starts;
static int g_no_start;                 /* it couldn't be started: said once, not tried for each file */

void reader_set_helper(const char *exe)
{
    pthread_mutex_lock(&g_mu);
    snprintf(g_exe, sizeof g_exe, "%s", exe ? exe : "");
    g_no_start = 0;
    pthread_mutex_unlock(&g_mu);
}

int reader_on(void) { return g_exe[0] != '\0' && !g_no_start; }
void reader_set_timeout_ms(int ms) { g_timeout_ms = ms > 0 ? ms : 60000; }
int reader_deaths(void) { return g_deaths; }
int reader_starts(void) { return g_starts; }

static void cloexec(int fd) { fcntl(fd, F_SETFD, fcntl(fd, F_GETFD) | FD_CLOEXEC); }

/* The helper gone, however it went: its pipes closed, itself killed if it's
 * still there (hung) and waited for. Says how it ended. */
static void stop_locked(int kill_it, const char *why)
{
    if (g_to >= 0) close(g_to);
    if (g_from >= 0) close(g_from);
    g_to = g_from = -1;
    if (!g_pid) return;
    if (kill_it) kill(g_pid, SIGKILL);
    int st = 0;
    while (waitpid(g_pid, &st, 0) < 0 && errno == EINTR) { }
    if (why) {
        if (WIFSIGNALED(st)) fprintf(stderr, "reader: %s (signal %d)\n", why, WTERMSIG(st));
        else fprintf(stderr, "reader: %s (exit %d)\n", why, WIFEXITED(st) ? WEXITSTATUS(st) : -1);
    }
    g_pid = 0;
}

static int start_locked(void)
{
    if (g_pid) return 0;
    int in[2], out[2];
    /* Close-on-exec from the moment they exist, all four ends (the helper's
     * two are passed on by the dup2 below): another thread's child started
     * in between (the Bluetooth sound helper, the SSH server) would keep
     * them, and with the helper's answering end held open elsewhere a helper
     * that died is waited for the whole minute and taken for one that hung;
     * with its asking end held, it never leaves. */
#ifdef __linux__
    if (pipe2(in, O_CLOEXEC)) return -1;
    if (pipe2(out, O_CLOEXEC)) { close(in[0]); close(in[1]); return -1; }
#else
    if (pipe(in)) return -1;
    if (pipe(out)) { close(in[0]); close(in[1]); return -1; }
    cloexec(in[0]);
    cloexec(out[1]);
#endif
    cloexec(in[1]);
    cloexec(out[0]);
    /* A helper that died takes its end of the pipe with it: writing to it is
     * an error to handle here, not a signal that ends the player. */
    signal(SIGPIPE, SIG_IGN);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, in[0], 0);
    posix_spawn_file_actions_adddup2(&fa, out[1], 1);
    posix_spawn_file_actions_addclose(&fa, in[0]);
    posix_spawn_file_actions_addclose(&fa, out[1]);
    char *argv[] = {g_exe, (char *)"--reader", NULL};
    pid_t pid = 0;
    int r = posix_spawn(&pid, g_exe, &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(in[0]);
    close(out[1]);
    if (r) {
        close(in[1]);
        close(out[0]);
        fprintf(stderr, "reader: can't start %s: %s\n", g_exe, strerror(r));
        return -1;
    }
    g_pid = pid;
    g_to = in[1];
    g_from = out[0];
    g_starts++;
    return 0;
}

static int write_all(int fd, const void *p, size_t n)
{
    const unsigned char *b = p;
    while (n) {
        ssize_t w = write(fd, b, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return -1;
        b += w;
        n -= (size_t)w;
    }
    return 0;
}

/* n bytes by `deadline`: 0; -1 the helper is gone (or sent nonsense); -2
 * time's up; -4 *stop was set meanwhile (looked at 5 times a second) */
static int read_all(int fd, void *p, size_t n, long deadline, const volatile int *stop)
{
    unsigned char *b = p;
    while (n) {
        long left = (long)(deadline - clock_ms());
        if (stop && *stop) return -4;
        if (left <= 0) return -2;
        if (stop && left > 200) left = 200;
        struct pollfd pf = {fd, POLLIN, 0};
        int pr = poll(&pf, 1, (int)left);
        if (pr < 0 && errno == EINTR) continue;
        if (pr < 0) return -1;
        if (pr == 0) continue;                         /* (the deadline and the stop looked at again) */
        ssize_t r = read(fd, b, n);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        b += r;
        n -= (size_t)r;
    }
    return 0;
}

/* One request: 0 answered; -1 the helper died on it; -2 it hung (and is
 * killed); -3 none could be started; -4 the caller's stop was set (the
 * helper is killed: it was in the middle of the work, and nobody's fault) */
static int ask_locked(int op, const char *path, long budget_kb, int timeout_ms, const volatile int *stop,
                      int *status, unsigned char **out, size_t *len)
{
    uint32_t plen = (uint32_t)strlen(path);
    int32_t head[3] = {op, (int32_t)plen, (int32_t)budget_kb};
    if (start_locked()) return -3;
    if (write_all(g_to, head, sizeof head) || write_all(g_to, path, plen)) {
        stop_locked(1, "gone before it was asked");
        return -1;
    }
    long deadline = (long)(clock_ms() + timeout_ms);
    int32_t reply[2];
    int r = read_all(g_from, reply, sizeof reply, deadline, stop);
    unsigned char *b = NULL;
    if (r == 0 && (uint32_t)reply[1] > REPLY_MAX) r = -1;
    if (r == 0 && reply[1] > 0) {
        b = malloc((size_t)reply[1]);
        r = b ? read_all(g_from, b, (size_t)reply[1], deadline, stop) : -1;
    }
    if (r == -4) {
        free(b);
        stop_locked(1, NULL);
        return r;
    }
    if (r) {
        free(b);
        g_deaths++;
        stop_locked(1, r == -2 ? "took too long: stopped" : "stopped");
        return r;
    }
    *status = reply[0];
    *out = b;
    *len = (size_t)reply[1];
    return 0;
}

int reader_call_long(int op, const char *path, long budget_kb, int timeout_ms, const volatile int *stop,
                     int *status, unsigned char **out, size_t *len)
{
    *status = -1;
    *out = NULL;
    *len = 0;
    if (strlen(path) >= PATH_MAX_SENT) return 0;       /* (no such path on the card: not read) */
    pthread_mutex_lock(&g_mu);
    int r = ask_locked(op, path, budget_kb, timeout_ms, stop, status, out, len);
    if (r == -1) {
        /* Once could be chance (it was the biggest thing in memory when
         * something else ran out): a moment, and again. A file it hangs on
         * isn't asked twice: that's a wait of minutes. */
        struct timespec ts = {0, 300 * 1000 * 1000L};
        nanosleep(&ts, NULL);
        r = ask_locked(op, path, budget_kb, timeout_ms, stop, status, out, len);
        /* (no helper to be had now, after one died on this file: the file
         * isn't handed to the caller to read itself, as READER_NONE would) */
        if (r == -3) r = -1;
    }
    if (r == -3) g_no_start = 1;                       /* no helper to be had: said once, and done by the caller from now on */
    pthread_mutex_unlock(&g_mu);
    return r == 0 ? 0 : r == -3 ? READER_NONE : r == -4 ? READER_STOPPED : READER_DIED;
}

int reader_call(int op, const char *path, long budget_kb, int *status, unsigned char **out, size_t *len)
{
    return reader_call_long(op, path, budget_kb, g_timeout_ms, NULL, status, out, len);
}

int reader_call_nowait(int op, const char *path, long budget_kb, int *status, unsigned char **out, size_t *len)
{
    *status = -1;
    *out = NULL;
    *len = 0;
    if (pthread_mutex_trylock(&g_mu)) return READER_NONE;   /* (at work for another: a song being measured is minutes) */
    pthread_mutex_unlock(&g_mu);
    /* (taken by another between the two: a wait after all, of one file's
     * reading as a rule; the long calls are the levelling's, which this
     * caller's own thread starts and stops) */
    return reader_call(op, path, budget_kb, status, out, len);
}

void reader_close(void)
{
    pthread_mutex_lock(&g_mu);
    stop_locked(0, NULL);                              /* (its stdin closed: it leaves by itself) */
    pthread_mutex_unlock(&g_mu);
}

void reader_close_nowait(void)
{
    if (pthread_mutex_trylock(&g_mu)) return;          /* (at work for another: theirs to close) */
    stop_locked(0, NULL);
    pthread_mutex_unlock(&g_mu);
}

/* ---------- the helper's side ---------- */

/* The most memory this request may take on top of what the helper has now
 * (reader.h): its address space capped there, so that what a file asks for
 * beyond it is refused on the spot. Only where /proc says how big the helper
 * is (Linux). */
static void limit_memory(long budget_kb)
{
    FILE *f = fopen("/proc/self/statm", "re");
    long pages = 0;
    int got = f && fscanf(f, "%ld", &pages) == 1 && pages > 0;
    if (f) fclose(f);
    if (!got) return;
    struct rlimit rl;
    if (getrlimit(RLIMIT_AS, &rl)) return;
    rlim_t want = budget_kb > 0 ? (rlim_t)pages * (rlim_t)sysconf(_SC_PAGESIZE) + (rlim_t)budget_kb * 1024 : RLIM_INFINITY;
    if (rl.rlim_max != RLIM_INFINITY && want > rl.rlim_max) want = rl.rlim_max;
    rl.rlim_cur = want;
    setrlimit(RLIMIT_AS, &rl);
}

/* n bytes of the player's request: 0, or -1 (it closed its end, or is gone) */
static int read_request(void *p, size_t n)
{
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(0, (char *)p + got, n - got);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        got += (size_t)r;
    }
    return 0;
}

int reader_serve(reader_fn fn)
{
    /* The answers go out on what was stdout; anything else this program or
     * its libraries print there goes to the log instead. Nothing else of
     * the player's is kept open here. */
    int out = dup(1);
    if (out < 0) return 1;
    dup2(2, 1);
    for (int fd = 3; fd < 1024; fd++)
        if (fd != out) close(fd);
    char *path = malloc(PATH_MAX_SENT);
    if (!path) return 1;
    for (;;) {
        int32_t head[3];
        if (read_request(head, sizeof head)) { free(path); return 0; }      /* the player closed it, or is gone */
        if (head[1] < 0 || head[1] >= PATH_MAX_SENT) { free(path); return 1; }
        if (read_request(path, (size_t)head[1])) { free(path); return 0; }
        path[head[1]] = '\0';
        limit_memory(head[2]);
        unsigned char *b = NULL;
        size_t n = 0;
        int32_t reply[2];
        reply[0] = fn(head[0], path, &b, &n);
        if (n > REPLY_MAX) { free(b); b = NULL; n = 0; reply[0] = -1; }
        reply[1] = (int32_t)n;
        int bad = write_all(out, reply, sizeof reply) || (n && write_all(out, b, n));
        free(b);
        if (bad) { free(path); return 0; }
    }
}
