/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Bluetooth for the player: a worker thread runs fathom-bt (start, scan,
 * pair, connect, ...) so nothing slow happens on the UI thread, and watches
 * bluez-alsa for a connected device's playback PCM. The UI thread reads a
 * snapshot (bt_ui_*, bt_pcm) and asks for things (bt_ui_do, bt_volume).
 *
 * The PCM is looked up when bluez-alsa says something changed (a
 * `bluealsa-cli monitor` kept running while Bluetooth is on), and every 5 s
 * only while that monitor isn't running (each look wakes bluez-alsa, whose
 * code, idle, is out of memory: a couple of hundred page faults). Asking every 2 s loaded a shell, bluealsa-cli and its libraries
 * from the card each time and woke bluez-alsa and D-Bus: in the H2's 23 MB,
 * with Wi-Fi on too, that kept pushing the player's own code out of memory
 * (radio stuttered). */
#include "bt.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define FATHOM_BT "/usr/sbin/fathom-bt"
#define CLI "LD_LIBRARY_PATH=/mnt/sd/fathom/bt/usr/lib /mnt/sd/fathom/bt/usr/bin/bluealsa-cli"
#define MAX_DEV 16
#define POLL_MS 5000                /* only while the monitor isn't running */
#define BT_LIBS "/mnt/sd/fathom/bt/usr/lib"
#define BT_CLI "/mnt/sd/fathom/bt/usr/bin/bluealsa-cli"

enum { CMD_NONE, CMD_START, CMD_STOP, CMD_SCAN, CMD_PAIR, CMD_CONNECT, CMD_DISCONNECT, CMD_FORGET, CMD_VOLUME };

static struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int cmd;
    char cmd_addr[18];
    int want_vol;                 /* volume to set (-1: none) */
    /* what the UI sees */
    int on;
    char busy[32];
    ui_bt_dev_t dev[MAX_DEV];
    int n_dev;
    char pcm[200], pcm_addr[18], last[18];
    int vol;                      /* 0..127, -1 unknown */
    int max;                      /* the volume limit, 0..127 */
    char msg[96];                 /* a toast for the UI, once */
    unsigned seq;
    int pcm_dirty;                /* bluez-alsa said something changed: look again */
    pid_t mon_pid;                /* the bluealsa-cli monitor, 0 if none */
} B = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, .want_vol = -1, .vol = -1, .max = 127};

static int addr_ok(const char *a)
{
    if (!a || strlen(a) != 17) return 0;
    for (int i = 0; i < 17; i++)
        if (i % 3 == 2 ? a[i] != ':' : !strchr("0123456789ABCDEFabcdef", a[i])) return 0;
    return 1;
}

static void changed_locked(void) { B.seq++; }

static void set_busy(const char *b)
{
    pthread_mutex_lock(&B.mu);
    snprintf(B.busy, sizeof B.busy, "%s", b);
    changed_locked();
    pthread_mutex_unlock(&B.mu);
}

static void say(const char *m)
{
    pthread_mutex_lock(&B.mu);
    snprintf(B.msg, sizeof B.msg, "%s", m);
    changed_locked();
    pthread_mutex_unlock(&B.mu);
}

/* Runs a shell command; its first `cap` bytes of output to out. Returns the exit status. */
static int run(const char *cmd, char *out, size_t cap)
{
    FILE *f = popen(cmd, "r");
    if (!f) return -1;
    size_t got = 0;
    if (out && cap) {
        size_t r;
        while (got + 1 < cap && (r = fread(out + got, 1, cap - 1 - got, f)) > 0) got += r;
        out[got] = '\0';
    }
    char sink[256];
    while (fread(sink, 1, sizeof sink, f) > 0) {}
    int st = pclose(f);
    return st < 0 ? -1 : (st >> 8) & 0xff;
}

/* ADDR|NAME|...|FLAG lines into devices (paired = 1 for the paired list). */
static int parse_devices(char *text, ui_bt_dev_t *out, int max, int paired_list)
{
    int n = 0;
    for (char *line = strtok(text, "\n"); line && n < max; line = strtok(NULL, "\n")) {
        char *f[4] = {0};
        int k = 0;
        for (char *p = line; k < 4; k++) {
            f[k] = p;
            char *bar = strchr(p, '|');
            if (!bar) { k++; break; }
            *bar = '\0';
            p = bar + 1;
        }
        if (!addr_ok(f[0])) continue;
        ui_bt_dev_t *d = &out[n++];
        memset(d, 0, sizeof *d);
        snprintf(d->addr, sizeof d->addr, "%s", f[0]);
        snprintf(d->name, sizeof d->name, "%s", f[1] ? f[1] : "");
        int flag = f[3] && f[3][0] == '1';
        d->paired = paired_list ? 1 : flag;
        d->connected = paired_list ? flag : 0;
    }
    return n;
}

/* Paired devices first; then those found by the last scan that aren't paired. */
static void refresh_paired(const ui_bt_dev_t *found, int n_found)
{
    char text[2048];
    ui_bt_dev_t list[MAX_DEV];
    int n = 0;
    if (run(FATHOM_BT " paired 2>/dev/null", text, sizeof text) == 0) n = parse_devices(text, list, MAX_DEV, 1);
    for (int i = 0; i < n_found && n < MAX_DEV; i++) {
        int dup = 0;
        for (int j = 0; j < n; j++) dup |= !strcmp(list[j].addr, found[i].addr);
        if (!dup && !found[i].paired) list[n++] = found[i];
    }
    pthread_mutex_lock(&B.mu);
    memcpy(B.dev, list, sizeof list);
    B.n_dev = n;
    changed_locked();
    pthread_mutex_unlock(&B.mu);
}

/* The connected device's playback PCM, if any. */
static void poll_pcm(void)
{
    char out[256] = "", addr[18] = "";
    if (run(FATHOM_BT " pcm 2>/dev/null", out, sizeof out) == 0) {
        out[strcspn(out, "\r\n")] = '\0';
        const char *d = strstr(out, "/dev_");
        if (d && strlen(d) >= 22) {
            for (int i = 0; i < 17; i++) addr[i] = d[5 + i] == '_' ? ':' : d[5 + i];
            addr[17] = '\0';
        }
    } else {
        out[0] = '\0';
    }
    int vol = -1;
    if (out[0] && strcmp(out, B.pcm)) {          /* a new link: its volume */
        char cmd[400], v[128] = "";
        snprintf(cmd, sizeof cmd, CLI " volume %s 2>/dev/null", out);
        if (run(cmd, v, sizeof v) == 0) {
            const char *p = strpbrk(v, "0123456789");
            if (p) vol = atoi(p);
        }
    }
    pthread_mutex_lock(&B.mu);
    if (strcmp(out, B.pcm)) {
        snprintf(B.pcm, sizeof B.pcm, "%s", out);
        snprintf(B.pcm_addr, sizeof B.pcm_addr, "%s", addr);
        if (addr[0]) snprintf(B.last, sizeof B.last, "%s", addr);
        B.vol = vol;
        if (vol > B.max) {                       /* above the limit: down to it */
            B.vol = B.want_vol = B.max;
            pthread_cond_broadcast(&B.cv);   /* (the worker and the monitor wait on it) */
        }
        for (int i = 0; i < B.n_dev; i++) B.dev[i].connected = addr[0] && !strcmp(B.dev[i].addr, addr);
        changed_locked();
    }
    pthread_mutex_unlock(&B.mu);
}

static void do_cmd(int cmd, const char *addr)
{
    char c[160], out[256];
    switch (cmd) {
    case CMD_START: {
        set_busy("Starting");
        int ok = run(FATHOM_BT " start 2>/dev/null", out, sizeof out) == 0;
        pthread_mutex_lock(&B.mu);
        B.on = ok;
        pthread_cond_broadcast(&B.cv);          /* (the monitor waits for it) */
        char last[18];
        snprintf(last, sizeof last, "%s", B.last);
        changed_locked();
        pthread_mutex_unlock(&B.mu);
        if (!ok) { set_busy(""); say("Bluetooth didn't start"); return; }
        refresh_paired(NULL, 0);
        if (addr_ok(last)) {                     /* the headphones of last time, if they're on */
            set_busy("Connecting");
            snprintf(c, sizeof c, FATHOM_BT " connect %s 2>/dev/null", last);
            /* right after a start the first try can fail where the next works */
            for (int i = 0; i < 3 && run(c, NULL, 0) != 0; i++) sleep(3);
        }
        break;
    }
    case CMD_STOP:
        set_busy("Stopping");
        run(FATHOM_BT " stop 2>/dev/null", NULL, 0);
        pthread_mutex_lock(&B.mu);
        B.on = 0;
        if (B.mon_pid > 0) kill(B.mon_pid, SIGTERM);   /* its memory back too */
        B.n_dev = 0;
        B.pcm[0] = B.pcm_addr[0] = '\0';
        changed_locked();
        pthread_mutex_unlock(&B.mu);
        break;
    case CMD_SCAN: {
        set_busy("Searching");
        char text[2048] = "";
        ui_bt_dev_t found[MAX_DEV];
        int n = 0;
        if (run(FATHOM_BT " scan 8 2>/dev/null", text, sizeof text) == 0) n = parse_devices(text, found, MAX_DEV, 0);
        refresh_paired(found, n);
        if (!n) say("No headphones found: are they in pairing mode?");
        break;
    }
    case CMD_PAIR: {
        set_busy("Pairing");
        snprintf(c, sizeof c, FATHOM_BT " pair %s 2>/dev/null", addr);
        int st = run(c, NULL, 0);
        say(st == 0 ? "Paired and connected" : st == 2 ? "Paired: turn the headphones on to connect"
                                                       : "Pairing didn't work: try pairing mode again");
        refresh_paired(NULL, 0);
        break;
    }
    case CMD_CONNECT:
        set_busy("Connecting");
        snprintf(c, sizeof c, FATHOM_BT " connect %s 2>/dev/null", addr);
        if (run(c, NULL, 0) != 0) say("Couldn't connect: are the headphones on?");
        break;
    case CMD_DISCONNECT:
        set_busy("Disconnecting");
        snprintf(c, sizeof c, FATHOM_BT " disconnect %s 2>/dev/null", addr);
        run(c, NULL, 0);
        break;
    case CMD_FORGET:
        set_busy("Forgetting");
        snprintf(c, sizeof c, FATHOM_BT " forget %s 2>/dev/null", addr);
        run(c, NULL, 0);
        pthread_mutex_lock(&B.mu);
        if (!strcmp(B.last, addr)) B.last[0] = '\0';
        pthread_mutex_unlock(&B.mu);
        refresh_paired(NULL, 0);
        break;
    }
    if (cmd != CMD_STOP) poll_pcm();
    set_busy("");
}

static void *worker(void *arg)
{
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&B.mu);
        if (!B.cmd && B.want_vol < 0 && !B.pcm_dirty) {
            if (B.mon_pid > 0 || !B.on) {
                pthread_cond_wait(&B.cv, &B.mu);    /* the monitor, a command or a volume wakes us (off: only a command) */
            } else {
                struct timespec ts;
                clock_gettime(CLOCK_REALTIME, &ts);
                ts.tv_sec += POLL_MS / 1000;
                pthread_cond_timedwait(&B.cv, &B.mu, &ts);
            }
        }
        int cmd = B.cmd, vol = B.want_vol, on = B.on, look = B.pcm_dirty || B.mon_pid <= 0;
        B.pcm_dirty = 0;
        char addr[18], pcm[200];
        snprintf(addr, sizeof addr, "%s", B.cmd_addr);
        snprintf(pcm, sizeof pcm, "%s", B.pcm);
        B.cmd = CMD_NONE;
        B.want_vol = -1;
        pthread_mutex_unlock(&B.mu);
        if (vol >= 0 && pcm[0]) {
            char c[400];
            snprintf(c, sizeof c, CLI " volume %s %d 2>/dev/null", pcm, vol);
            run(c, NULL, 0);
        }
        if (cmd) do_cmd(cmd, addr);
        else if (on && look) poll_pcm();
    }
    return NULL;
}

/* While Bluetooth is on: `bluealsa-cli monitor`, each line it prints (a PCM
 * added or removed, the service starting or stopping) a reason to look again. */
static void *monitor(void *arg)
{
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&B.mu);
        int on = B.on;
        pthread_mutex_unlock(&B.mu);
        if (!on) {                               /* until Bluetooth is on (no wakeups meanwhile) */
            pthread_mutex_lock(&B.mu);
            while (!B.on) pthread_cond_wait(&B.cv, &B.mu);
            pthread_mutex_unlock(&B.mu);
            continue;
        }
        int fd[2];
        if (pipe(fd)) { sleep(5); continue; }
        posix_spawn_file_actions_t fa;
        posix_spawn_file_actions_init(&fa);
        posix_spawn_file_actions_adddup2(&fa, fd[1], 1);
        posix_spawn_file_actions_addclose(&fa, fd[0]);
        posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
        char *argv[] = {(char *)BT_CLI, (char *)"monitor", NULL};
        char *envp[] = {(char *)"LD_LIBRARY_PATH=" BT_LIBS, NULL};
        pid_t pid = 0;
        int r = posix_spawn(&pid, BT_CLI, &fa, NULL, argv, envp);
        posix_spawn_file_actions_destroy(&fa);
        close(fd[1]);
        if (r) { close(fd[0]); sleep(5); continue; }
        pthread_mutex_lock(&B.mu);
        B.mon_pid = pid;
        pthread_mutex_unlock(&B.mu);
        /* read(), not stdio: a FILE blocked in fgets() holds its lock, and the
         * C library's exit() takes every FILE's lock to flush it, so the
         * player would hang on its way out. Any output is a reason to look. */
        char buf[256];
        ssize_t n;
        while ((n = read(fd[0], buf, sizeof buf)) > 0 || (n < 0 && errno == EINTR)) {
            if (n <= 0) continue;
            pthread_mutex_lock(&B.mu);
            B.pcm_dirty = 1;
            pthread_cond_broadcast(&B.cv);
            pthread_mutex_unlock(&B.mu);
        }
        close(fd[0]);
        pthread_mutex_lock(&B.mu);
        B.mon_pid = 0;
        B.pcm_dirty = 1;                         /* (the worker, waiting on us, polls again) */
        pthread_cond_broadcast(&B.cv);
        pthread_mutex_unlock(&B.mu);
        waitpid(pid, NULL, 0);
        sleep(2);                                /* (bluez-alsa restarting, or Bluetooth off) */
    }
    return NULL;
}

/* The monitor is a separate process: stopped when the player exits, and any
 * left by a player that crashed are stopped when the next one starts. */
static void monitor_kill(void)
{
    if (B.mon_pid > 0) kill(B.mon_pid, SIGTERM);
}

static void monitor_kill_strays(void)
{
    DIR *d = opendir("/proc");
    struct dirent *e;
    while (d && (e = readdir(d))) {
        int pid = atoi(e->d_name);
        if (pid <= 1) continue;
        char path[64], cmd[128] = "";
        snprintf(path, sizeof path, "/proc/%d/cmdline", pid);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        size_t n = fread(cmd, 1, sizeof cmd - 1, f);
        fclose(f);
        for (size_t i = 0; i < n; i++) if (!cmd[i]) cmd[i] = ' ';
        if (strstr(cmd, "bluealsa-cli monitor")) kill(pid, SIGTERM);
    }
    if (d) closedir(d);
}

void bt_init(int on, const char *last)
{
    if (access(FATHOM_BT, X_OK) || access("/mnt/sd/fathom/bt", F_OK)) return;
    if (addr_ok(last)) snprintf(B.last, sizeof B.last, "%s", last);
    pthread_t th;
    if (pthread_create(&th, NULL, worker, NULL)) return;
    pthread_detach(th);
    monitor_kill_strays();
    atexit(monitor_kill);
    if (pthread_create(&th, NULL, monitor, NULL) == 0) pthread_detach(th);
    if (on) bt_request(CMD_START, NULL);
    B.seq++;
}

int bt_available(void) { return !access(FATHOM_BT, X_OK) && !access("/mnt/sd/fathom/bt", F_OK); }

void bt_request(int cmd, const char *addr)
{
    pthread_mutex_lock(&B.mu);
    B.cmd = cmd;
    snprintf(B.cmd_addr, sizeof B.cmd_addr, "%s", addr && addr_ok(addr) ? addr : "");
    pthread_cond_broadcast(&B.cv);   /* (the worker and the monitor wait on it) */
    pthread_mutex_unlock(&B.mu);
}

int bt_ui_state(char *busy, size_t cap)
{
    pthread_mutex_lock(&B.mu);
    int on = B.on;
    snprintf(busy, cap, "%s", B.busy);
    pthread_mutex_unlock(&B.mu);
    return on;
}

int bt_ui_devices(ui_bt_dev_t *out, int max)
{
    pthread_mutex_lock(&B.mu);
    int n = B.n_dev < max ? B.n_dev : max;
    memcpy(out, B.dev, (size_t)n * sizeof *out);
    pthread_mutex_unlock(&B.mu);
    return n;
}

/* (UI thread) */
void bt_ui_do(int what, const char *addr)
{
    pthread_mutex_lock(&B.mu);
    int on = B.on, busy = B.busy[0] != '\0';
    ui_bt_dev_t d = {0};
    for (int i = 0; addr && i < B.n_dev; i++)
        if (!strcmp(B.dev[i].addr, addr)) d = B.dev[i];
    pthread_mutex_unlock(&B.mu);
    if (busy) return;                            /* one thing at a time */
    switch (what) {
    case UI_BT_TOGGLE: bt_request(on ? CMD_STOP : CMD_START, NULL); break;
    case UI_BT_SCAN: bt_request(CMD_SCAN, NULL); break;
    case UI_BT_PRESS:
        if (!d.addr[0]) break;
        bt_request(!d.paired ? CMD_PAIR : d.connected ? CMD_DISCONNECT : CMD_CONNECT, d.addr);
        break;
    case UI_BT_FORGET: if (d.addr[0]) bt_request(CMD_FORGET, d.addr); break;
    }
}

unsigned bt_seq(void)
{
    pthread_mutex_lock(&B.mu);
    unsigned s = B.seq;
    pthread_mutex_unlock(&B.mu);
    return s;
}

int bt_pcm(char *pcm, size_t cap, char *name, size_t ncap)
{
    pthread_mutex_lock(&B.mu);
    snprintf(pcm, cap, "%s", B.pcm);
    if (name) {
        snprintf(name, ncap, "Bluetooth");
        for (int i = 0; i < B.n_dev; i++)
            if (!strcmp(B.dev[i].addr, B.pcm_addr) && B.dev[i].name[0]) snprintf(name, ncap, "%s", B.dev[i].name);
    }
    int n = B.pcm[0] != '\0';
    pthread_mutex_unlock(&B.mu);
    return n;
}

int bt_on(void)
{
    pthread_mutex_lock(&B.mu);
    int on = B.on;
    pthread_mutex_unlock(&B.mu);
    return on;
}

int bt_last(char *addr, size_t cap)
{
    pthread_mutex_lock(&B.mu);
    snprintf(addr, cap, "%s", B.last);
    pthread_mutex_unlock(&B.mu);
    return addr[0] != '\0';
}

int bt_take_message(char *out, size_t cap)
{
    pthread_mutex_lock(&B.mu);
    int any = B.msg[0] != '\0';
    snprintf(out, cap, "%s", B.msg);
    B.msg[0] = '\0';
    pthread_mutex_unlock(&B.mu);
    return any;
}

void bt_set(int on) { bt_request(on ? CMD_START : CMD_STOP, NULL); }

int bt_volume_step(int dir)
{
    pthread_mutex_lock(&B.mu);
    if (B.vol < 0) B.vol = 64;
    int v = B.vol + dir * 4;
    if (v > B.max) v = B.max;
    v = v < 0 ? 0 : v > 127 ? 127 : v;
    B.vol = v;
    B.want_vol = v;
    pthread_cond_broadcast(&B.cv);   /* (the worker and the monitor wait on it) */
    pthread_mutex_unlock(&B.mu);
    return v;
}

void bt_volume_limit(int max)
{
    pthread_mutex_lock(&B.mu);
    B.max = max < 0 ? 0 : max > 127 ? 127 : max;
    if (B.vol > B.max) {
        B.vol = B.want_vol = B.max;
        pthread_cond_broadcast(&B.cv);   /* (the worker and the monitor wait on it) */
    }
    pthread_mutex_unlock(&B.mu);
}
