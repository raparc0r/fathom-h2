/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* Shell helpers shared by the workers that drive fathom-wifi, fathom-bt,
 * fathom-share and fathom-ssh (see shell.h). */
#define _GNU_SOURCE   /* pipe2 */
#include "shell.h"
#include "core/clock.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

/* A pipe that a program started meanwhile by another worker doesn't inherit
 * (it would hold the far end open, and the reader below would wait for it) */
static int pipe_cloexec(int fd[2])
{
#ifdef __linux__
    return pipe2(fd, O_CLOEXEC);
#else                                               /* (the tests' host) */
    if (pipe(fd)) return -1;
    fcntl(fd[0], F_SETFD, FD_CLOEXEC);
    fcntl(fd[1], F_SETFD, FD_CLOEXEC);
    return 0;
#endif
}

/* The program a command line runs, for the log: the first unquoted word that
 * is a path, without its folders (else the first unquoted word). Never the
 * rest of the line: an argument can be a password (fathom-wifi connect NAME
 * PASSWORD; the network drive's, printed into fathom-share). */
static void cmd_name(const char *cmd, char *out, size_t cap)
{
    const char *best = "?";
    size_t best_n = 1;
    int found = 0;
    for (const char *w = cmd; *w && found < 2;) {
        while (*w == ' ') w++;
        const char *e = w;
        int plain = 1;
        while (*e && *e != ' ') {
            if (*e == '\'' || *e == '"') {           /* (a quoted stretch, spaces and all) */
                char q = *e++;
                plain = 0;
                while (*e && *e != q) e++;
                if (*e) e++;
            } else if (*e == '\\') {
                plain = 0;
                if (*++e) e++;
            } else {
                e++;
            }
        }
        if (plain && e > w && (*w == '/' ? found < 2 : !found)) {
            found = *w == '/' ? 2 : 1;
            best = w;
            best_n = (size_t)(e - w);
            for (const char *c = w; c < e; c++)
                if (*c == '/') { best = c + 1; best_n = (size_t)(e - best); }
        }
        w = e;
    }
    snprintf(out, cap, "%.*s", (int)best_n, best);
}

/* (not popen: a deadline, and the whole of what it started killed when it
 * passes.) The shell is started with posix_spawn, not fork: fork gave the
 * child a copy of the player's page tables and write-protected every page of
 * the player to share them, so each page the audio and UI threads wrote to
 * next cost a fault and a copy, at every command (one every few seconds with
 * Wi-Fi on). musl's posix_spawn runs the child in the player's own memory
 * until it execs (clone with CLONE_VM | CLONE_VFORK): nothing copied, nothing
 * protected, and only the calling worker waits, for the moment the exec takes.
 * The child is as before: a process group of its own (so the deadline can end
 * all of it), its output on the pipe, its errors where the player's go, the
 * player's environment; its input is /dev/null, said outright (it was the
 * player's own input, which is /dev/null as S90fathom starts it). */
int sh_run_for(const char *cmd, char *out, size_t cap, int secs)
{
    int fd[2];
    if (out && cap) out[0] = '\0';
    if (pipe_cloexec(fd)) return -1;
    posix_spawn_file_actions_t fa;
    posix_spawnattr_t at;
    char *argv[] = {(char *)"sh", (char *)"-c", (char *)cmd, NULL};
    pid_t pid = -1;
    if (posix_spawn_file_actions_init(&fa)) { close(fd[0]); close(fd[1]); return -1; }
    if (posix_spawnattr_init(&at)) { posix_spawn_file_actions_destroy(&fa); close(fd[0]); close(fd[1]); return -1; }
    int err = posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    if (!err) err = posix_spawn_file_actions_adddup2(&fa, fd[1], 1);   /* (the copy isn't close-on-exec) */
    if (!err) err = posix_spawnattr_setpgroup(&at, 0);                 /* (a group of its own: killed together) */
    if (!err) err = posix_spawnattr_setflags(&at, POSIX_SPAWN_SETPGROUP);
    if (!err) err = posix_spawn(&pid, "/bin/sh", &fa, &at, argv, environ);
    posix_spawnattr_destroy(&at);
    posix_spawn_file_actions_destroy(&fa);
    close(fd[1]);
    if (err) { close(fd[0]); return -1; }
    long end = (long)clock_ms() + (long)secs * 1000;
    size_t got = 0;
    int late = 0;
    for (;;) {
        long left = end - (long)clock_ms();
        if (left <= 0) { late = 1; break; }
        struct pollfd p = {.fd = fd[0], .events = POLLIN};
        int r = poll(&p, 1, left > 1000 ? 1000 : (int)left);
        if (r < 0 && errno != EINTR) break;
        if (r <= 0) continue;
        char sink[256];
        int keep = out && got + 1 < cap;
        ssize_t n = read(fd[0], keep ? out + got : sink, keep ? cap - 1 - got : sizeof sink);
        if (n <= 0) break;                          /* (its end: every writer gone) */
        if (keep) { got += (size_t)n; out[got] = '\0'; }
    }
    close(fd[0]);
    int st = 0;
    /* Its output ended: usually because it did. Not always: a command whose
     * own output goes elsewhere ("fathom-wifi connect ... >/dev/null": the
     * H2's shell becomes the command, and nothing holds the pipe) runs on
     * with the pipe closed. Waited for here with no limit, such a helper
     * that hung held its caller for good, the deadline never looked at
     * (found by this file's test on the H2, 2026-11-26). So: until the same
     * deadline, looked at often at first (most have just ended). */
    for (useconds_t nap = 500; !late; nap = nap < 50000 ? nap * 2 : 50000) {
        pid_t w = waitpid(pid, &st, WNOHANG);
        if (w == pid) return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
        if (w < 0 && errno != EINTR) return -1;
        if (end - (long)clock_ms() <= 0) late = 1;
        else usleep(nap);
    }
    char name[40];
    cmd_name(cmd, name, sizeof name);
    fprintf(stderr, "shell: no answer in %d s, stopped: %s\n", secs, name);
    kill(-pid, SIGKILL);
    kill(pid, SIGKILL);
    /* (one stuck in the kernel can't be reaped: left) */
    for (int i = 0; i < 50 && waitpid(pid, &st, WNOHANG) == 0; i++) usleep(100000);
    return 124;
}

int sh_run(const char *cmd, char *out, size_t cap) { return sh_run_for(cmd, out, cap, SH_RUN_SECS); }

void mono_cond_init(pthread_cond_t *cv)
{
    pthread_condattr_t a;
    pthread_condattr_init(&a);
    pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
    pthread_cond_init(cv, &a);
    pthread_condattr_destroy(&a);
}

int mono_cond_wait_ms(pthread_cond_t *cv, pthread_mutex_t *mu, int ms)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    if (ms < 0) ms = 0;
    ts.tv_sec += ms / 1000;
    ts.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    return pthread_cond_timedwait(cv, mu, &ts);
}

int sh_pid_alive(const char *pidfile)
{
    FILE *f = fopen(pidfile, "re");
    long pid = 0;
    if (!f) return 0;
    if (fscanf(f, "%ld", &pid) != 1) pid = 0;
    fclose(f);
    if (pid <= 0) return 0;                            /* (0 and below aren't one process to kill()) */
    return kill((pid_t)pid, 0) == 0 || errno == EPERM;
}

int sh_revive(sh_revive_t *r, int alive, uint32_t now_ms)
{
    if (alive) {
        if (r->revived && now_ms - r->revived_at >= 5 * SH_LOOK_MS) r->revived = 0;
        return SH_WELL;
    }
    if (r->revived >= 3) return SH_GIVE_UP;
    r->revived++;
    r->revived_at = now_ms;
    return SH_REVIVE;
}

void sh_quote(const char *s, char *out, size_t cap)
{
    size_t o = 0;
    if (cap < 3) { if (cap) out[0] = '\0'; return; }
    out[o++] = '\'';
    for (; *s && o + 5 < cap; s++) {
        if (*s == '\'') { memcpy(out + o, "'\\''", 4); o += 4; }
        else out[o++] = *s;
    }
    out[o++] = '\'';
    out[o] = '\0';
}

void if_ip(const char *name, char *out, size_t cap)
{
    out[0] = '\0';
    struct ifaddrs *ifs;
    if (getifaddrs(&ifs)) return;
    for (struct ifaddrs *i = ifs; i; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || strcmp(i->ifa_name, name)) continue;
        inet_ntop(AF_INET, &((struct sockaddr_in *)i->ifa_addr)->sin_addr, out, (socklen_t)cap);
        break;
    }
    freeifaddrs(ifs);
}

/* KEY's value in an answer's KEY=VALUE lines: where it starts and how long it
 * is, NULL without one (the first such line, as fathom-wifi's val) */
static const char *reply_val(const char *reply, const char *key, size_t *len)
{
    size_t k = strlen(key);
    const char *l = reply;
    while (l && *l) {
        if (!strncmp(l, key, k) && l[k] == '=') {
            *len = strcspn(l + k + 1, "\n");
            return l + k + 1;
        }
        l = strchr(l, '\n');
        if (l) l++;
    }
    return NULL;
}

static int hex_digit(char c) { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; }

/* A name as wpa_supplicant prints it (printf_encode: \" \\ \e \n \r \t, and
 * \xHH for each byte outside printable ASCII) back to its bytes, exactly as
 * fathom-wifi's fmt() does it: a control character, however it was written,
 * becomes "?" (the script's names go out one per line), so the name from here
 * and the name from the script are the same bytes. */
static void wpa_unescape(const char *s, size_t n, char *out, size_t cap)
{
    size_t o = 0;
    for (size_t i = 0; i < n && o + 1 < cap; i++) {
        int c = (unsigned char)s[i];
        if (c == '\\') {
            char e = i + 1 < n ? s[++i] : '\0';
            if (e == 'x') {                         /* (two characters taken whatever they are, as fmt() does) */
                int ok = i + 2 < n && isxdigit((unsigned char)s[i + 1]) && isxdigit((unsigned char)s[i + 2]);
                c = ok ? hex_digit(s[i + 1]) * 16 + hex_digit(s[i + 2]) : '?';
                i += 2;
            } else {
                c = e == '"' || e == '\\' ? e : '?';
            }
            if (c < 32 || c == 127) c = '?';
        } else if (c < 32 || c >= 127) {
            c = '?';                                /* (never sent unescaped) */
        }
        out[o++] = (char)c;
    }
    out[o] = '\0';
}

int wpa_status_parse(const char *reply, wpa_status_t *out)
{
    size_t n;
    const char *v;
    memset(out, 0, sizeof *out);
    if (!(v = reply_val(reply, "wpa_state", &n)) || !n) return 0;
    snprintf(out->state, sizeof out->state, "%.*s", (int)n, v);
    if ((v = reply_val(reply, "ssid", &n))) wpa_unescape(v, n, out->ssid, sizeof out->ssid);
    if ((v = reply_val(reply, "ip_address", &n))) snprintf(out->ip, sizeof out->ip, "%.*s", (int)n, v);
    return 1;
}

int wpa_reply_int(const char *reply, const char *key, int *out)
{
    size_t n;
    const char *v = reply_val(reply, key, &n);
    if (!v || !n || !(isdigit((unsigned char)v[0]) || (v[0] == '-' && n > 1 && isdigit((unsigned char)v[1])))) return 0;
    *out = atoi(v);
    return 1;
}

static pthread_mutex_t home_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t home_cv;
static pthread_once_t home_once = PTHREAD_ONCE_INIT;
static int home_up;

static void home_init(void) { mono_cond_init(&home_cv); }

void sh_home_shown(void)
{
    pthread_once(&home_once, home_init);
    pthread_mutex_lock(&home_mu);
    home_up = 1;
    pthread_cond_broadcast(&home_cv);
    pthread_mutex_unlock(&home_mu);
}

void sh_wait_home(int max_ms)
{
    pthread_once(&home_once, home_init);
    struct timespec t0, t;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    pthread_mutex_lock(&home_mu);
    while (!home_up) {
        clock_gettime(CLOCK_MONOTONIC, &t);
        int left = max_ms - (int)((t.tv_sec - t0.tv_sec) * 1000 + (t.tv_nsec - t0.tv_nsec) / 1000000);
        if (left <= 0) break;
        mono_cond_wait_ms(&home_cv, &home_mu, left);
    }
    pthread_mutex_unlock(&home_mu);
}
