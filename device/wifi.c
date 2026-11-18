/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Wi-Fi for the player: a worker thread runs fathom-wifi (on, off, scan,
 * connect, forget) so nothing slow happens on the UI thread, and polls its
 * status (state, network, address, signal) while Wi-Fi is on. The UI thread
 * reads a snapshot (wifi_ui_*) and asks for things (wifi_ui_do). Like bt.c. */
#include "wifi.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define FATHOM_WIFI "/usr/sbin/fathom-wifi"
#define MAX_NET 24
/* Status every 2 s while it's joining, every 10 s once connected: each look
 * runs a shell and wpa_cli several times, which costs the H2's 23 MB more
 * than the news is worth when nothing is happening. */
#define POLL_MS 2000
#define POLL_CONNECTED_MS 10000

enum { CMD_NONE, CMD_START, CMD_STOP, CMD_SCAN, CMD_CONNECT, CMD_FORGET };

static struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int cmd;
    char cmd_ssid[33], cmd_pass[64];
    /* what the UI sees */
    int on;
    char busy[32];
    ui_wifi_status_t st;
    ui_wifi_net_t net[MAX_NET];
    int n_net;
    char msg[96];
    unsigned seq;
} W = {.mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER};

static void changed_locked(void) { W.seq++; }

static void set_busy(const char *b)
{
    pthread_mutex_lock(&W.mu);
    snprintf(W.busy, sizeof W.busy, "%s", b);
    changed_locked();
    pthread_mutex_unlock(&W.mu);
}

static void say(const char *m)
{
    pthread_mutex_lock(&W.mu);
    snprintf(W.msg, sizeof W.msg, "%s", m);
    changed_locked();
    pthread_mutex_unlock(&W.mu);
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

/* s as one single-quoted shell word ('it'\''s'). */
static void shell_quote(const char *s, char *out, size_t cap)
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

static void poll_status(void)
{
    char out[256];
    if (run(FATHOM_WIFI " status 2>/dev/null", out, sizeof out) != 0) return;
    ui_wifi_status_t st = {0};
    char state[16] = "", *f[4] = {0};
    out[strcspn(out, "\n")] = '\0';
    char *p = out;
    for (int i = 0; i < 4; i++) { f[i] = p; p = strchr(p, '|'); if (!p) break; *p++ = '\0'; }
    snprintf(state, sizeof state, "%s", f[0] ? f[0] : "");
    st.state = !strcmp(state, "connected") ? UI_WIFI_CONNECTED : !strcmp(state, "connecting") ? UI_WIFI_CONNECTING
             : !strcmp(state, "off") ? UI_WIFI_OFF : UI_WIFI_ON;
    if (f[1]) snprintf(st.ssid, sizeof st.ssid, "%s", f[1]);
    if (f[2]) snprintf(st.ip, sizeof st.ip, "%s", f[2]);
    st.dbm = f[3] && *f[3] ? atoi(f[3]) : 0;
    pthread_mutex_lock(&W.mu);
    if (memcmp(&st, &W.st, sizeof st)) {
        if (st.state == UI_WIFI_CONNECTED && W.st.state != UI_WIFI_CONNECTED) {
            snprintf(W.msg, sizeof W.msg, "Wi-Fi: %s", st.ssid);
            fprintf(stderr, "wifi: connected to %s (%s)\n", st.ssid, st.ip);
        }
        W.st = st;
        for (int i = 0; i < W.n_net; i++) W.net[i].connected = st.state == UI_WIFI_CONNECTED && !strcmp(W.net[i].ssid, st.ssid);
        changed_locked();
    }
    pthread_mutex_unlock(&W.mu);
}

static void scan(void)
{
    set_busy("Searching");
    char out[4096];
    run(FATHOM_WIFI " scan 4 2>/dev/null", out, sizeof out);
    ui_wifi_net_t net[MAX_NET];
    int n = 0;
    for (char *line = strtok(out, "\n"); line && n < MAX_NET; line = strtok(NULL, "\n")) {
        char *f[4] = {0}, *p = line;
        for (int i = 0; i < 4 && p; i++) { f[i] = p; p = strchr(p, '|'); if (p) *p++ = '\0'; }
        if (!f[3]) continue;
        memset(&net[n], 0, sizeof net[n]);
        snprintf(net[n].ssid, sizeof net[n].ssid, "%s", f[0]);
        net[n].dbm = atoi(f[1]);
        net[n].secured = atoi(f[2]);
        net[n].saved = atoi(f[3]);
        n++;
    }
    pthread_mutex_lock(&W.mu);
    memcpy(W.net, net, (size_t)n * sizeof net[0]);
    W.n_net = n;
    for (int i = 0; i < n; i++) W.net[i].connected = W.st.state == UI_WIFI_CONNECTED && !strcmp(W.net[i].ssid, W.st.ssid);
    snprintf(W.busy, sizeof W.busy, "%s", "");
    changed_locked();
    pthread_mutex_unlock(&W.mu);
}

static void do_cmd(int cmd, const char *ssid, const char *pass)
{
    char c[400], q[140], qp[160];
    switch (cmd) {
    case CMD_START: {
        set_busy("Starting");
        char err[200] = "";
        int ok = run(FATHOM_WIFI " on 2>&1 >/dev/null", err, sizeof err) == 0;
        pthread_mutex_lock(&W.mu);
        W.on = ok;
        changed_locked();
        pthread_mutex_unlock(&W.mu);
        set_busy("");
        if (!ok) {
            err[strcspn(err, "\n")] = '\0';
            fprintf(stderr, "wifi: didn't start: %s\n", err);
            say(strstr(err, "firmware") ? "Wi-Fi: no firmware on the card" : "Wi-Fi didn't start");
            return;
        }
        fprintf(stderr, "wifi: on\n");
        poll_status();
        scan();
        break;
    }
    case CMD_STOP:
        set_busy("Stopping");
        run(FATHOM_WIFI " off >/dev/null 2>&1", NULL, 0);
        pthread_mutex_lock(&W.mu);
        W.on = 0;
        memset(&W.st, 0, sizeof W.st);
        W.n_net = 0;
        changed_locked();
        pthread_mutex_unlock(&W.mu);
        set_busy("");
        fprintf(stderr, "wifi: off\n");
        break;
    case CMD_SCAN:
        scan();
        break;
    case CMD_CONNECT: {
        set_busy("Connecting");
        shell_quote(ssid, q, sizeof q);
        shell_quote(pass, qp, sizeof qp);
        snprintf(c, sizeof c, FATHOM_WIFI " connect %s %s >/dev/null 2>&1", q, pass[0] ? qp : "");
        int st = run(c, NULL, 0);               /* (not logged: it has the password) */
        if (st == 3) say("That password isn't valid: 8 to 63 characters");
        else if (st) say("Couldn't join it");
        /* joining takes a few seconds: the status poll follows it */
        for (int i = 0; i < 10; i++) {
            usleep(1000 * 1000);
            poll_status();
            pthread_mutex_lock(&W.mu);
            int done = W.st.state == UI_WIFI_CONNECTED && !strcmp(W.st.ssid, ssid);
            pthread_mutex_unlock(&W.mu);
            if (done) break;
        }
        set_busy("");
        scan();                                  /* "saved" marks */
        break;
    }
    case CMD_FORGET:
        set_busy("Forgetting");
        shell_quote(ssid, q, sizeof q);
        snprintf(c, sizeof c, FATHOM_WIFI " forget %s >/dev/null 2>&1", q);
        run(c, NULL, 0);
        set_busy("");
        scan();
        break;
    }
}

static void *worker(void *arg)
{
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&W.mu);
        if (!W.cmd && !W.on) {
            pthread_cond_wait(&W.cv, &W.mu);     /* off: nothing to poll until a command */
        } else if (!W.cmd) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += (W.st.state == UI_WIFI_CONNECTED ? POLL_CONNECTED_MS : POLL_MS) / 1000;
            pthread_cond_timedwait(&W.cv, &W.mu, &ts);
        }
        int cmd = W.cmd, on = W.on;
        char ssid[33], pass[64];
        snprintf(ssid, sizeof ssid, "%s", W.cmd_ssid);
        snprintf(pass, sizeof pass, "%s", W.cmd_pass);
        W.cmd = CMD_NONE;
        memset(W.cmd_pass, 0, sizeof W.cmd_pass);
        pthread_mutex_unlock(&W.mu);
        if (cmd) do_cmd(cmd, ssid, pass);
        else if (on) poll_status();
        memset(pass, 0, sizeof pass);
    }
    return NULL;
}

int wifi_available(void)
{
    return !access(FATHOM_WIFI, X_OK) && !access("/mnt/sd/fathom/wifi/usr/sbin/wpa_supplicant", X_OK);
}

static void request(int cmd, const char *ssid, const char *pass)
{
    pthread_mutex_lock(&W.mu);
    W.cmd = cmd;
    snprintf(W.cmd_ssid, sizeof W.cmd_ssid, "%s", ssid ? ssid : "");
    snprintf(W.cmd_pass, sizeof W.cmd_pass, "%s", pass ? pass : "");
    pthread_cond_signal(&W.cv);
    pthread_mutex_unlock(&W.mu);
}

void wifi_init(int on)
{
    if (!wifi_available()) return;
    pthread_t th;
    if (pthread_create(&th, NULL, worker, NULL)) return;
    pthread_detach(th);
    if (on) request(CMD_START, NULL, NULL);
    W.seq++;
}

int wifi_ui_state(char *busy, size_t cap, ui_wifi_status_t *st)
{
    pthread_mutex_lock(&W.mu);
    int on = W.on;
    snprintf(busy, cap, "%s", W.busy);
    if (st) *st = W.st;
    pthread_mutex_unlock(&W.mu);
    return on;
}

int wifi_ui_networks(ui_wifi_net_t *out, int max)
{
    pthread_mutex_lock(&W.mu);
    int n = W.n_net < max ? W.n_net : max;
    memcpy(out, W.net, (size_t)n * sizeof *out);
    pthread_mutex_unlock(&W.mu);
    return n;
}

void wifi_ui_do(int what, const char *ssid, const char *password)
{
    pthread_mutex_lock(&W.mu);
    int on = W.on, busy = W.busy[0] != '\0';
    pthread_mutex_unlock(&W.mu);
    if (busy && what != UI_WIFI_TOGGLE) return;
    switch (what) {
    case UI_WIFI_TOGGLE: request(on ? CMD_STOP : CMD_START, NULL, NULL); break;
    case UI_WIFI_SCAN: if (on) request(CMD_SCAN, NULL, NULL); break;
    case UI_WIFI_CONNECT: if (on && ssid && *ssid) request(CMD_CONNECT, ssid, password); break;
    case UI_WIFI_FORGET: if (on && ssid && *ssid) request(CMD_FORGET, ssid, NULL); break;
    }
}

unsigned wifi_seq(void)
{
    pthread_mutex_lock(&W.mu);
    unsigned s = W.seq;
    pthread_mutex_unlock(&W.mu);
    return s;
}

int wifi_on(void)
{
    pthread_mutex_lock(&W.mu);
    int on = W.on;
    pthread_mutex_unlock(&W.mu);
    return on;
}

int wifi_connected(void)
{
    pthread_mutex_lock(&W.mu);
    int c = W.st.state == UI_WIFI_CONNECTED;
    pthread_mutex_unlock(&W.mu);
    return c;
}

int wifi_take_message(char *out, size_t cap)
{
    pthread_mutex_lock(&W.mu);
    int have = W.msg[0] != '\0';
    if (have) snprintf(out, cap, "%s", W.msg);
    W.msg[0] = '\0';
    pthread_mutex_unlock(&W.mu);
    return have;
}
