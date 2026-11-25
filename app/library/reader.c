/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
#include "reader.h"

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
    if (pipe(in)) return -1;
    if (pipe(out)) { close(in[0]); close(in[1]); return -1; }
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

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* n bytes by `deadline`: 0; -1 the helper is gone (or sent nonsense); -2 time's up */
static int read_all(int fd, void *p, size_t n, long deadline)
{
    unsigned char *b = p;
    while (n) {
        long left = deadline - now_ms();
        if (left <= 0) return -2;
        struct pollfd pf = {fd, POLLIN, 0};
        int pr = poll(&pf, 1, (int)left);
        if (pr < 0 && errno == EINTR) continue;
        if (pr < 0) return -1;
        if (pr == 0) return -2;
        ssize_t r = read(fd, b, n);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        b += r;
        n -= (size_t)r;
    }
    return 0;
}

/* One request: 0 answered; -1 the helper died on it; -2 it hung (and is killed) */
static int ask_locked(int op, const char *path, long budget_kb, int *status, unsigned char **out, size_t *len)
{
    uint32_t plen = (uint32_t)strlen(path);
    int32_t head[3] = {op, (int32_t)plen, (int32_t)budget_kb};
    if (start_locked()) return -3;
    if (write_all(g_to, head, sizeof head) || write_all(g_to, path, plen)) {
        stop_locked(1, "gone before it was asked");
        return -1;
    }
    long deadline = now_ms() + g_timeout_ms;
    int32_t reply[2];
    int r = read_all(g_from, reply, sizeof reply, deadline);
    unsigned char *b = NULL;
    if (r == 0 && (uint32_t)reply[1] > REPLY_MAX) r = -1;
    if (r == 0 && reply[1] > 0) {
        b = malloc((size_t)reply[1]);
        r = b ? read_all(g_from, b, (size_t)reply[1], deadline) : -1;
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

int reader_call(int op, const char *path, long budget_kb, int *status, unsigned char **out, size_t *len)
{
    *status = -1;
    *out = NULL;
    *len = 0;
    if (strlen(path) >= PATH_MAX_SENT) return 0;       /* (no such path on the card: not read) */
    pthread_mutex_lock(&g_mu);
    int r = ask_locked(op, path, budget_kb, status, out, len);
    if (r == -1) {
        /* Once could be chance (it was the biggest thing in memory when
         * something else ran out): a moment, and again. A file it hangs on
         * isn't asked twice: that's a wait of minutes. */
        struct timespec ts = {0, 300 * 1000 * 1000L};
        nanosleep(&ts, NULL);
        r = ask_locked(op, path, budget_kb, status, out, len);
    }
    if (r == -3) {                                     /* no helper to be had: said once, and done here from now on */
        g_no_start = 1;
        pthread_mutex_unlock(&g_mu);
        return READER_NONE;
    }
    pthread_mutex_unlock(&g_mu);
    return r ? READER_DIED : 0;
}

void reader_close(void)
{
    pthread_mutex_lock(&g_mu);
    stop_locked(0, NULL);                              /* (its stdin closed: it leaves by itself) */
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
        size_t got = 0;
        while (got < sizeof head) {
            ssize_t r = read(0, (char *)head + got, sizeof head - got);
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) { free(path); return 0; }      /* the player closed it, or is gone */
            got += (size_t)r;
        }
        if (head[1] < 0 || head[1] >= PATH_MAX_SENT) { free(path); return 1; }
        got = 0;
        while (got < (size_t)head[1]) {
            ssize_t r = read(0, path + got, (size_t)head[1] - got);
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) { free(path); return 0; }
            got += (size_t)r;
        }
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
