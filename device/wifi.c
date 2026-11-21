/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Wi-Fi for the player: a worker thread runs fathom-wifi (on, off, scan,
 * connect, forget) so nothing slow happens on the UI thread, and polls its
 * status (state, network, address, signal) while Wi-Fi is on. The UI thread
 * reads a snapshot (wifi_ui_*) and asks for things (wifi_ui_do). Like bt.c.
 * Network names are their real text both ways (fathom-wifi undoes wpa_cli's
 * escapes). */
#include "wifi.h"
#include "shell.h"
#include "core/lang.h"
#include "core/lat.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define FATHOM_WIFI "/usr/sbin/fathom-wifi"
#define MAX_NET 24
/* Status every 2 s while it's joining, every 10 s once connected; while it's
 * on but not joined (nothing saved in range) 2 s, then twice as long after
 * each look that brings nothing new, up to 30 s. Each look runs a shell and
 * wpa_cli two or three times, which costs the H2's 23 MB more than the news
 * is worth when nothing is happening; a network that comes in range is
 * joined by wpa_supplicant all the same, only shown a little later. */
#define POLL_MS 2000
#define POLL_IDLE_MAX_MS 30000
#define POLL_CONNECTED_MS 10000
/* Just after Wi-Fi starts, a saved network in range is being joined within a
 * second (status "connecting"), and its address comes once status sees the
 * join (fathom-wifi status wakes DHCP then): every 0.5 s while it's joining,
 * for up to 15 s. The first look for networks (for Settings > Wi-Fi) waits
 * until it's connected, and 5 s more (a station resumed is filling its buffer
 * then): a scan takes the radio off the network's channel for seconds, so a
 * join or an address under way would wait for it. Nothing being joined 2.5 s
 * after the start (nothing saved in range: the list is wanted), it looks at
 * once. */
#define POLL_JOIN_MS 500
#define JOIN_WINDOW_MS 15000
#define JOIN_SEEN_MS 2500
#define SCAN_AFTER_JOIN_MS 5000

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
    char msg_name[33];   /* a network's name for msg's %s ("Wi-Fi: %s"), "" if none */
    unsigned seq;
} W = {.mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER};

static void changed_locked(void) { W.seq++; }

/* (the worker's own) a start's joining window: since and until (monotonic ms,
 * join_until 0 = none); the first scan still to do, and when (0: not yet) */
static uint32_t join_since, join_until, scan_at;
static int scan_due;

static uint32_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000);
}

static void set_busy(const char *b)
{
    pthread_mutex_lock(&W.mu);
    snprintf(W.busy, sizeof W.busy, "%s", b);
    changed_locked();
    pthread_mutex_unlock(&W.mu);
}

/* A toast for the UI: text with a %s for name (translated when taken), or plain */
static void say_name(const char *text, const char *name)
{
    pthread_mutex_lock(&W.mu);
    snprintf(W.msg, sizeof W.msg, "%s", text);
    snprintf(W.msg_name, sizeof W.msg_name, "%s", name);
    changed_locked();
    pthread_mutex_unlock(&W.mu);
}

static void say(const char *m) { say_name(m, ""); }

/* The last n '|'-separated fields of line into f[1..n], what comes before
 * them into f[0]: a network's name may hold a '|' of its own. 0 if there are
 * fewer fields. */
static int split_right(char *line, char **f, int n)
{
    for (int i = n; i >= 1; i--) {
        char *bar = strrchr(line, '|');
        if (!bar) return 0;
        *bar = '\0';
        f[i] = bar + 1;
    }
    f[0] = line;
    return 1;
}

/* Returns 1 if anything the UI shows changed. */
static int poll_status(void)
{
    char out[256];
    if (sh_run(FATHOM_WIFI " status 2>/dev/null", out, sizeof out) != 0) return 0;
    ui_wifi_status_t st = {0};
    char *f[3];
    out[strcspn(out, "\n")] = '\0';
    if (!split_right(out, f, 2)) return 0;               /* STATE|SSID, IP, SIGNAL */
    char *ssid = strchr(f[0], '|');
    if (ssid) *ssid++ = '\0';
    const char *state = f[0];
    st.state = !strcmp(state, "connected") ? UI_WIFI_CONNECTED : !strcmp(state, "connecting") ? UI_WIFI_CONNECTING
             : !strcmp(state, "off") ? UI_WIFI_OFF : UI_WIFI_ON;
    snprintf(st.ssid, sizeof st.ssid, "%s", ssid ? ssid : "");
    snprintf(st.ip, sizeof st.ip, "%s", f[1]);
    st.dbm = *f[2] ? atoi(f[2]) : 0;
    pthread_mutex_lock(&W.mu);
    int changed = memcmp(&st, &W.st, sizeof st) != 0;
    if (st.state != W.st.state) LAT("wifi: status %s", state);
    if (changed) {
        if (st.state == UI_WIFI_CONNECTED && W.st.state != UI_WIFI_CONNECTED) {
            snprintf(W.msg, sizeof W.msg, "%s", "Wi-Fi: %s");
            snprintf(W.msg_name, sizeof W.msg_name, "%s", st.ssid);
            fprintf(stderr, "wifi: connected to %s (%s)\n", st.ssid, st.ip);
        }
        W.st = st;
        for (int i = 0; i < W.n_net; i++) W.net[i].connected = st.state == UI_WIFI_CONNECTED && !strcmp(W.net[i].ssid, st.ssid);
        changed_locked();
    }
    pthread_mutex_unlock(&W.mu);
    return changed;
}

static void scan(void)
{
    LAT("wifi: scan");
    set_busy("Searching");
    char out[4096] = "", *save = NULL;
    sh_run(FATHOM_WIFI " scan 4 2>/dev/null", out, sizeof out);   /* (out stays empty if it can't start) */
    ui_wifi_net_t net[MAX_NET];
    int n = 0;
    /* strtok_r: the Bluetooth worker parses its lists at the same time */
    for (char *line = strtok_r(out, "\n", &save); line && n < MAX_NET; line = strtok_r(NULL, "\n", &save)) {
        char *f[4];
        if (!split_right(line, f, 3)) continue;           /* SSID, DBM, SECURED, SAVED */
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
        LAT("wifi: starting");
        int ok = sh_run(FATHOM_WIFI " on 2>&1 >/dev/null", err, sizeof err) == 0;
        LAT("wifi: started (%d)", ok);
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
        join_since = now_ms();                    /* (see POLL_JOIN_MS) */
        join_until = join_since + JOIN_WINDOW_MS;
        if (!join_until) join_until = 1;
        scan_due = 1;
        scan_at = 0;
        break;
    }
    case CMD_STOP:
        join_until = scan_at = 0;
        scan_due = 0;
        set_busy("Stopping");
        sh_run(FATHOM_WIFI " off >/dev/null 2>&1", NULL, 0);
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
        scan_due = 0;
        break;
    case CMD_CONNECT: {
        join_until = scan_at = 0;                /* (it scans when done) */
        scan_due = 0;
        set_busy("Connecting");
        sh_quote(ssid, q, sizeof q);
        sh_quote(pass, qp, sizeof qp);
        snprintf(c, sizeof c, FATHOM_WIFI " connect %s %s >/dev/null 2>&1", q, pass[0] ? qp : "");
        /* It waits for the join (up to 20 s): joined, or why not. A network
         * it couldn't join isn't kept, nor one whose password is wrong. */
        int st = sh_run(c, NULL, 0);             /* (not logged: it has the password) */
        memset(c, 0, sizeof c);
        memset(qp, 0, sizeof qp);
        if (st) fprintf(stderr, "wifi: couldn't join %s (%d)\n", ssid, st);
        if (st == 3) say("That password isn't valid: 8 to 63 characters");
        else if (st == 4) say_name("Wrong password for %s", ssid);
        else if (st) say_name("Couldn't join %s", ssid);
        /* joined: the address comes a few seconds later, which the status follows */
        for (int i = 0; i < (st ? 1 : 10); i++) {
            if (!st) usleep(1000 * 1000);
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
        join_until = scan_at = 0;
        scan_due = 0;
        set_busy("Forgetting");
        sh_quote(ssid, q, sizeof q);
        snprintf(c, sizeof c, FATHOM_WIFI " forget %s >/dev/null 2>&1", q);
        sh_run(c, NULL, 0);
        set_busy("");
        scan();
        break;
    }
}

static void *worker(void *arg)
{
    (void)arg;
    int idle_ms = POLL_MS;                       /* on, not joined: grows while nothing changes */
    for (;;) {
        pthread_mutex_lock(&W.mu);
        if (!W.cmd && !W.on) {
            pthread_cond_wait(&W.cv, &W.mu);     /* off: nothing to poll until a command */
        } else if (!W.cmd) {
            int ms = join_until ? POLL_JOIN_MS
                   : W.st.state == UI_WIFI_CONNECTED ? POLL_CONNECTED_MS
                   : W.st.state == UI_WIFI_CONNECTING ? POLL_MS : idle_ms;
            if (scan_due && scan_at) {           /* the first scan, when it's due */
                int32_t left = (int32_t)(scan_at - now_ms());
                if (left < ms) ms = left < 0 ? 0 : left;
            }
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += ms / 1000;
            ts.tv_nsec += (long)(ms % 1000) * 1000000L;
            if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
            pthread_cond_timedwait(&W.cv, &W.mu, &ts);
        }
        int cmd = W.cmd, on = W.on;
        char ssid[33], pass[64];
        snprintf(ssid, sizeof ssid, "%s", W.cmd_ssid);
        snprintf(pass, sizeof pass, "%s", W.cmd_pass);
        W.cmd = CMD_NONE;
        memset(W.cmd_pass, 0, sizeof W.cmd_pass);
        pthread_mutex_unlock(&W.mu);
        if (cmd) {
            do_cmd(cmd, ssid, pass);
            idle_ms = POLL_MS;
        } else if (on) {
            int changed = poll_status();
            idle_ms = changed ? POLL_MS : idle_ms * 2 > POLL_IDLE_MAX_MS ? POLL_IDLE_MAX_MS : idle_ms * 2;
            if (join_until) {
                /* connected: the first scan a little later; nothing being joined
                 * (or the window over): now. Then the usual pace. */
                pthread_mutex_lock(&W.mu);
                int state = W.st.state;
                pthread_mutex_unlock(&W.mu);
                uint32_t t = now_ms();
                if (state == UI_WIFI_CONNECTED) {
                    join_until = 0;
                    scan_at = t + SCAN_AFTER_JOIN_MS;
                    if (!scan_at) scan_at = 1;
                } else if ((int32_t)(join_until - t) <= 0 ||
                           (state != UI_WIFI_CONNECTING && t - join_since >= JOIN_SEEN_MS)) {
                    join_until = 0;
                    scan_at = t ? t : 1;
                }
            }
            if (scan_due && scan_at && (int32_t)(scan_at - now_ms()) <= 0) {
                scan_due = 0;
                scan_at = 0;
                scan();
            }
        }
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
    if (have && W.msg_name[0]) tr_fill(out, cap, W.msg, W.msg_name, NULL);   /* (on the UI's thread: the language's) */
    else if (have) snprintf(out, cap, "%s", W.msg);
    W.msg[0] = W.msg_name[0] = '\0';
    pthread_mutex_unlock(&W.mu);
    return have;
}
