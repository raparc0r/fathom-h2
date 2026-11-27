/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* Wi-Fi for the player: a worker thread runs fathom-wifi (on, off, scan,
 * connect, forget) so nothing slow happens on the UI thread, and polls its
 * status (state, network, address, signal) while Wi-Fi is on: the script's
 * while anything is changing, wpa_supplicant's own (over its control socket)
 * once connected. The UI thread reads a snapshot (wifi_ui_*) and asks for
 * things (wifi_ui_do). Like bt.c.
 * Network names are their real text both ways (fathom-wifi undoes wpa_cli's
 * escapes). */
#include "wifi.h"
#include "shell.h"
#include "core/lang.h"
#include "core/wifitxt.h"
#include "core/lat.h"
#include "core/clock.h"
#include "core/thread.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define FATHOM_WIFI "/usr/sbin/fathom-wifi"
#define MAX_NET 24
/* Status every 2 s while it's joining, every 10 s once connected; while it's
 * on but not joined (nothing saved in range) 2 s, then twice as long after
 * each look that brings nothing new, up to 30 s. Each look runs a shell and
 * wpa_cli two or three times, which costs the H2's 23 MB more than the news
 * is worth when nothing is happening; a network that comes in range is
 * joined by wpa_supplicant all the same, only shown a little later. Once
 * connected, the look is a question to wpa_supplicant itself (poll_quick), and
 * the script's only once a minute. */
#define POLL_MS 2000
#define POLL_IDLE_MAX_MS 30000
#define POLL_CONNECTED_MS 10000
#define SCRIPT_EVERY_MS 60000
/* Just after Wi-Fi starts, a saved network in range is being joined within a
 * second (status "connecting"), and its address comes once status sees the
 * join (fathom-wifi status wakes DHCP then): every 0.5 s while it's joining,
 * for up to 15 s.
 *
 * The list of networks in range is only looked for when Settings > Wi-Fi asks
 * for it (the screen opened, or its Search row). A look takes 4.5 to 6.5 s
 * with the radio off the network's channel and the worker busy, so nothing
 * wakes DHCP meanwhile: one made by itself 2.5 s after every start, as it was,
 * could hold the address back that long at every boot, for a list nobody was
 * looking at. A list asked for while Wi-Fi was off or starting (the screen
 * open, Wi-Fi then turned on from it) is remembered (W.list_wanted) and looked
 * for once the start allows: connected, and 5 s more (a station resumed is
 * filling its buffer then); or nothing being joined 2.5 s after the start
 * (nothing saved in range: the list is what's wanted), at once. A list
 * asked for more than 3 minutes before the start isn't waited on any more. */
#define POLL_JOIN_MS 500
#define JOIN_WINDOW_MS 15000
#define JOIN_SEEN_MS 2500
#define SCAN_AFTER_JOIN_MS 5000
#define LIST_WISH_MS 180000
/* Joined, and no address comes (a router whose DHCP is off or has none left
 * to give, a hotspot that lets the H2 in and then ignores it): the status
 * said "connecting" and was asked every 2 s for as long as that lasted, a
 * shell and three programs from the card each time, with "Connecting" on the
 * screen for ever. Once it has lasted 20 s with the link up, it's said
 * ("Joined %s, but it gave no address", once for that network; "No address"
 * where Settings > Wi-Fi shows what Wi-Fi is doing), and the status is asked
 * less often: after 4 s, then 8, 16, and every 30. Meanwhile the wait goes
 * 2 s at a time, looking at the link and the interface themselves (a file
 * read and a question to the kernel): an address come, or the link gone, and
 * the status is asked at once. udhcpc goes on asking by itself throughout. */
#define NOADDR_MS 20000

/* Searching while nothing saved is in range (out of the house with Wi-Fi
 * on): wpa_supplicant looks every 5 s, the radio off for half a second each
 * time (battery, and a likely gap in Bluetooth audio). It looks less often
 * the longer nothing turns up: 5 s, then 10, 20, 40, and every 60 s. Back
 * to 5 s, with a look at once, when there's a reason to expect a network:
 * the screen waking, Settings > Wi-Fi opened, Wi-Fi turned on, or a network
 * just left. */
#define PACE_MIN_S 5
#define PACE_MAX_S 60
#define WAKE_LOOK_MS 10000   /* a screen woken again within this: no new look */
enum { CMD_NONE, CMD_START, CMD_STOP, CMD_SCAN, CMD_CONNECT, CMD_FORGET, CMD_WAKE };
#define AWAY_LOOK_MS 2000

/* wlan0 has a link (joined a network): the kernel's own word, a file read */
static int link_up(void)
{
    FILE *f = fopen("/sys/class/net/wlan0/carrier", "re");
    int c = f ? fgetc(f) : 0;
    if (f) fclose(f);
    return c == '1';
}

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
    char import[256];    /* wifi_import's file, once Wi-Fi is on ("" none) */
    int screen;          /* the screen is on (wifi_screen): the signal is worth asking for */
    char note[32];       /* shown where busy is while nothing's being done: "No address" (see NOADDR_MS) */
    int list_wanted;     /* Settings > Wi-Fi asked for the networks and hasn't had them (see LIST_WISH_MS) */
    uint32_t list_wanted_at;
} W = {.mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER, .screen = 1};

static void changed_locked(void) { W.seq++; }

/* (the worker's own) a start's joining window: since and until (monotonic ms,
 * join_until 0 = none); a list of networks still to look for (scan_due), and
 * from when it may be (scan_at, 0: not yet: a join is under way) */
static uint32_t join_since, join_until, scan_at;
static int scan_due;
/* (the worker's own) the search interval wpa_supplicant has (s), and when it
 * goes up next (0: not backing off: connected, joining or off) */
static int pace_s = PACE_MIN_S;
static uint32_t pace_next;
/* (the worker's own) when fathom-wifi status last ran (0: not yet) */
static uint32_t script_at;
/* (the worker's own; see NOADDR_MS) the network it has been "connecting" to
 * and since when (0: it isn't); the pace of its looks once that has lasted
 * (0: not yet); the network it was last said of */
static char noaddr_ssid[33], noaddr_said[33];
static uint32_t noaddr_since;
static int noaddr_ms;

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

/* fathom-wifi status, read. Returns 1 if anything the UI shows changed. */
static int poll_status(void)
{
    char out[256];
    script_at = clock_ms32();
    if (!script_at) script_at = 1;
    if (sh_run(FATHOM_WIFI " status 2>/dev/null", out, sizeof out) != 0) return 0;
    ui_wifi_status_t st;
    memset(&st, 0, sizeof st);                           /* (every byte: compared whole below, and in poll_quick) */
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

/* A question to wpa_supplicant over its control socket: no shell, no wpa_cli.
 * Its answer into reply, as a string; returns its length, -1 if none came
 * (2 s at most each way: a wpa_supplicant that's stuck doesn't hold the worker,
 * sending included, as its queue fills). Each question from an address of its
 * own, so an answer that comes late isn't taken for the next one's. */
static int wpa_ask(const char *cmd, char *reply, size_t cap)
{
    static unsigned nth;
    int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    struct sockaddr_un me = {.sun_family = AF_UNIX}, it = {.sun_family = AF_UNIX};
    snprintf(me.sun_path, sizeof me.sun_path, "/var/run/fathom-wifi/ctrl-%d-%u", (int)getpid(), nth++ & 0xffu);
    snprintf(it.sun_path, sizeof it.sun_path, "/var/run/wpa_supplicant/wlan0");
    unlink(me.sun_path);
    int len = -1;
    struct timeval tv = {2, 0};
    reply[0] = '\0';
    if (bind(fd, (struct sockaddr *)&me, sizeof me) == 0) {
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        if (connect(fd, (struct sockaddr *)&it, sizeof it) == 0 && send(fd, cmd, strlen(cmd), 0) >= 0) {
            ssize_t n = recv(fd, reply, cap - 1, 0);
            if (n > 0) { reply[n] = '\0'; len = (int)n; }
        }
        unlink(me.sun_path);
    }
    close(fd);
    return len;
}

/* A command to it ("OK" back). 0 if it answered OK. */
static int wpa_ctrl(const char *cmd)
{
    char reply[64];
    return wpa_ask(cmd, reply, sizeof reply) >= 2 && !strncmp(reply, "OK", 2) ? 0 : -1;
}

/* While connected, fathom-wifi status counts its minute "without a
 * connection" (after which every saved network may be joined again) from the
 * last time it saw one: the same note from here, the kernel's uptime in
 * seconds as the script reads it. */
static void note_seen(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_BOOTTIME, &ts)) return;
    FILE *f = fopen("/var/run/fathom-wifi/seen", "we");
    if (!f) return;
    fprintf(f, "%ld\n", (long)ts.tv_sec);
    fclose(f);
}

/* Connected, and nothing happening: the look every 10 s asked of
 * wpa_supplicant itself (two questions on its socket, two file reads). It was
 * `fathom-wifi status` each time: the whole player forked, a shell reading a
 * 400-line script, wpa_cli from the card twice, for as long as Wi-Fi stayed
 * connected. Returns 1 if it's still joined to the same network, with a link
 * and an address (the snapshot brought up to date, *changed as poll_status
 * returns); 0 for anything else at all, no answer included: the script then
 * looks, as before, and does what only it does (waking DHCP, a new address
 * after moving to another network, noticing a daemon gone). */
static int poll_quick(int *changed)
{
    char reply[2048];
    wpa_status_t ws;
    ui_wifi_status_t st;
    memset(&st, 0, sizeof st);
    if (!link_up()) return 0;
    if (wpa_ask("STATUS", reply, sizeof reply) <= 0 || !wpa_status_parse(reply, &ws)) return 0;
    if (strcmp(ws.state, "COMPLETED")) return 0;
    st.state = UI_WIFI_CONNECTED;
    snprintf(st.ssid, sizeof st.ssid, "%s", ws.ssid);
    snprintf(st.ip, sizeof st.ip, "%s", ws.ip);
    if (!st.ip[0]) if_ip("wlan0", st.ip, sizeof st.ip);
    if (!st.ip[0]) return 0;
    pthread_mutex_lock(&W.mu);
    int same = W.st.state == UI_WIFI_CONNECTED && !strcmp(W.st.ssid, st.ssid);
    int screen = W.screen;
    st.dbm = W.st.dbm;
    pthread_mutex_unlock(&W.mu);
    if (!same) return 0;
    /* (the signal only matters to someone looking: asking makes the driver ask the chip) */
    if (screen && (wpa_ask("SIGNAL_POLL", reply, sizeof reply) <= 0 || !wpa_reply_int(reply, "RSSI", &st.dbm))) st.dbm = 0;
    note_seen();
    pthread_mutex_lock(&W.mu);
    *changed = memcmp(&st, &W.st, sizeof st) != 0;
    if (*changed) {
        W.st = st;
        changed_locked();
    }
    pthread_mutex_unlock(&W.mu);
    return 1;
}

/* The worker's regular look: wpa_supplicant itself while connected; the script
 * for everything else, and once a minute whatever wpa_supplicant says (should
 * the two ever disagree, or udhcpc have died, it isn't for long). Returns 1 if
 * anything the UI shows changed. */
static int look(void)
{
    int changed = 0;
    if (script_at && clock_ms32() - script_at < SCRIPT_EVERY_MS && poll_quick(&changed)) return changed;
    return poll_status();
}

/* wpa_supplicant's search interval; look: one search now as well */
static void pace_set(int s, int look)
{
    char c[32];
    snprintf(c, sizeof c, "SCAN_INTERVAL %d", s);
    if (wpa_ctrl(c) == 0) {
        if (s != pace_s) fprintf(stderr, "wifi: nothing joined: searching every %d s\n", s);
        pace_s = s;
    }
    if (look) wpa_ctrl("SCAN");
    pace_next = 0;
}

/* After a status poll: back off while nothing is joined, start over once
 * something is (for the next time it's left) */
static void pace_follow(int state)
{
    uint32_t t = clock_ms32();
    if (join_until || state == UI_WIFI_CONNECTED || state == UI_WIFI_CONNECTING || state == UI_WIFI_OFF) {
        if (pace_s != PACE_MIN_S) pace_set(PACE_MIN_S, 0);
        pace_next = 0;
    } else if (!pace_next) {
        pace_next = t + (uint32_t)pace_s * 1000u;
        if (!pace_next) pace_next = 1;
    } else if ((int32_t)(pace_next - t) <= 0 && pace_s < PACE_MAX_S) {
        pace_set(pace_s * 2 > PACE_MAX_S ? PACE_MAX_S : pace_s * 2, 0);
        pace_next = t + (uint32_t)pace_s * 1000u;
        if (!pace_next) pace_next = 1;
    }
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
    W.list_wanted = 0;                           /* (it has its list) */
    changed_locked();
    pthread_mutex_unlock(&W.mu);
}

/* Which of the networks listed are saved, asked again: after a connect or a
 * forget that is all that changed in the list. It was a whole new look for
 * networks (4.5 to 6.5 s of "Searching", every key press dropped meanwhile,
 * the radio off its channel), only to redraw those marks. */
static void saved_marks(void)
{
    char out[4096] = "", *save = NULL;
    pthread_mutex_lock(&W.mu);
    int n = W.n_net;
    pthread_mutex_unlock(&W.mu);
    if (!n) return;                              /* (no list shown: nothing to mark) */
    if (sh_run(FATHOM_WIFI " saved 2>/dev/null", out, sizeof out) != 0) {
        scan();                                  /* (it couldn't say: as before) */
        return;
    }
    pthread_mutex_lock(&W.mu);
    for (int i = 0; i < W.n_net; i++) W.net[i].saved = 0;
    for (char *line = strtok_r(out, "\n", &save); line; line = strtok_r(NULL, "\n", &save))
        for (int i = 0; i < W.n_net; i++)
            if (!strcmp(W.net[i].ssid, line)) W.net[i].saved = 1;
    changed_locked();
    pthread_mutex_unlock(&W.mu);
}

/* The address just leased is from ssid, and it's on the interface: read from
 * what fathom-wifi-lease notes and the kernel (no program run). After a
 * connect, when to ask the script's status. */
static int lease_ready(const char *ssid)
{
    char line[80] = "", ip[16];
    FILE *f = fopen("/var/run/fathom-wifi/lease-ssid", "re");
    if (!f) return 0;
    if (!fgets(line, sizeof line, f)) line[0] = '\0';
    fclose(f);
    line[strcspn(line, "\n")] = '\0';
    if (strcmp(line, ssid) || !link_up()) return 0;
    if_ip("wlan0", ip, sizeof ip);
    return ip[0] != '\0';
}

static void noaddr_reset(void)
{
    noaddr_since = 0;
    noaddr_ms = 0;
    noaddr_ssid[0] = '\0';
    pthread_mutex_lock(&W.mu);
    if (W.note[0]) {
        W.note[0] = '\0';
        changed_locked();
    }
    pthread_mutex_unlock(&W.mu);
}

/* After a status look: see NOADDR_MS */
static void noaddr_follow(void)
{
    char ssid[33];
    pthread_mutex_lock(&W.mu);
    int state = W.st.state;
    snprintf(ssid, sizeof ssid, "%s", W.st.ssid);
    pthread_mutex_unlock(&W.mu);
    uint32_t t = clock_ms32();
    if (state == UI_WIFI_CONNECTED) noaddr_said[0] = '\0';   /* (it has one: worth saying again if it's lost) */
    if (state != UI_WIFI_CONNECTING || !ssid[0] || strcmp(ssid, noaddr_ssid)) {
        noaddr_reset();                          /* not that, or another network: from the start */
        if (state == UI_WIFI_CONNECTING && ssid[0]) {
            snprintf(noaddr_ssid, sizeof noaddr_ssid, "%s", ssid);
            noaddr_since = t ? t : 1;
        }
        return;
    }
    if (noaddr_ms) {                             /* said already: less and less often */
        noaddr_ms = noaddr_ms * 2 > POLL_IDLE_MAX_MS ? POLL_IDLE_MAX_MS : noaddr_ms * 2;
        return;
    }
    if (t - noaddr_since < NOADDR_MS || !link_up()) return;   /* (no link: still being let in, not this) */
    noaddr_ms = POLL_MS * 2;
    fprintf(stderr, "wifi: joined %s, but no address in %d s: asking less often\n", ssid, NOADDR_MS / 1000);
    pthread_mutex_lock(&W.mu);
    snprintf(W.note, sizeof W.note, "%s", "No address");
    changed_locked();
    pthread_mutex_unlock(&W.mu);
    if (strcmp(noaddr_said, ssid)) {
        snprintf(noaddr_said, sizeof noaddr_said, "%s", ssid);
        say_name("Joined %s, but it gave no address", ssid);
    }
}

/* wlan0 has an address (the kernel's word) */
static int has_ip(void)
{
    char ip[16];
    if_ip("wlan0", ip, sizeof ip);
    return ip[0] != '\0';
}

static int connect_st;   /* the last CMD_CONNECT's fathom-wifi exit (0: saved) */

static void do_cmd(int cmd, const char *ssid, const char *pass)
{
    char c[400], q[140], qp[160];
    switch (cmd) {
    case CMD_START: {
        set_busy("Starting");
        char err[200] = "";
        LAT("wifi: starting");
        scan_due = 0;                             /* (a list only if Settings > Wi-Fi wants one) */
        scan_at = 0;
        noaddr_reset();
        noaddr_said[0] = '\0';
        pthread_mutex_lock(&W.mu);
        if (W.list_wanted && clock_ms32() - W.list_wanted_at > LIST_WISH_MS) W.list_wanted = 0;
        pthread_mutex_unlock(&W.mu);
        int ok = sh_run(FATHOM_WIFI " on 2>&1 >/dev/null", err, sizeof err) == 0;
        /* A start that failed leaves the driver loaded and the chip powered
         * (over 1 MB, and current, with Wi-Fi shown off), and the next start
         * found them so and failed the same way. Put away, and once more: the
         * usual cause is memory too fragmented for the firmware's pieces, which
         * the first try's clean-up helps. (Not after "didn't start in 45 s":
         * the driver may still be starting, and unloading it then deadlocks.) */
        if (!ok && !strstr(err, "firmware at") && !strstr(err, "45 s")) {
            fprintf(stderr, "wifi: didn't start (%.80s): put away, once more\n", err);
            sh_run(FATHOM_WIFI " off >/dev/null 2>&1", NULL, 0);
            sleep(2);
            err[0] = '\0';
            ok = sh_run(FATHOM_WIFI " on 2>&1 >/dev/null", err, sizeof err) == 0;
            if (!ok && !strstr(err, "45 s")) sh_run(FATHOM_WIFI " off >/dev/null 2>&1", NULL, 0);
        }
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
        pace_s = PACE_MIN_S;                     /* (a new wpa_supplicant: its own 5 s) */
        pace_next = 0;
        poll_status();
        join_since = clock_ms32();                    /* (see POLL_JOIN_MS) */
        join_until = join_since + JOIN_WINDOW_MS;
        if (!join_until) join_until = 1;
        break;
    }
    case CMD_STOP:
        join_until = scan_at = 0;
        scan_due = 0;
        pace_s = PACE_MIN_S;
        pace_next = 0;
        noaddr_reset();
        noaddr_said[0] = '\0';
        set_busy("Stopping");
        sh_run(FATHOM_WIFI " off >/dev/null 2>&1", NULL, 0);
        pthread_mutex_lock(&W.mu);
        W.on = 0;
        memset(&W.st, 0, sizeof W.st);
        if (W.n_net) {                           /* its list was showing: wanted again if it's turned back on */
            W.list_wanted = 1;
            W.list_wanted_at = clock_ms32();
        }
        W.n_net = 0;
        changed_locked();
        pthread_mutex_unlock(&W.mu);
        set_busy("");
        fprintf(stderr, "wifi: off\n");
        break;
    case CMD_SCAN:
        if (pace_s != PACE_MIN_S) pace_set(PACE_MIN_S, 0);   /* (Settings > Wi-Fi open: a network may be wanted) */
        pace_next = 0;
        scan();
        scan_due = 0;
        break;
    case CMD_WAKE: {                             /* the screen on: nothing joined, look now */
        static uint32_t last;
        pthread_mutex_lock(&W.mu);
        int state = W.st.state;
        pthread_mutex_unlock(&W.mu);
        uint32_t t = clock_ms32();
        if (join_until || state != UI_WIFI_ON || (last && t - last < WAKE_LOOK_MS)) break;
        last = t ? t : 1;
        pace_set(PACE_MIN_S, 1);
        break;
    }
    case CMD_CONNECT: {
        join_until = scan_at = 0;
        scan_due = 0;
        set_busy("Connecting");
        sh_quote(ssid, q, sizeof q);
        sh_quote(pass, qp, sizeof qp);
        snprintf(c, sizeof c, FATHOM_WIFI " connect %s %s >/dev/null 2>&1", q, pass[0] ? qp : "");
        /* It waits for the join (up to 20 s): joined, or why not. A network
         * it couldn't join isn't kept, nor one whose password is wrong. */
        int st = sh_run(c, NULL, 0);             /* (not logged: it has the password) */
        connect_st = st;
        memset(c, 0, sizeof c);
        memset(qp, 0, sizeof qp);
        if (st) fprintf(stderr, "wifi: couldn't join %s (%d)\n", ssid, st);
        if (st == 3) say("That password isn't valid: 8 to 63 characters");
        else if (st == 4) say_name("Wrong password for %s", ssid);
        else if (st == 6) say_name("Couldn't join %s yet", ssid);   /* (kept: joined when it answers) */
        else if (st) say_name("Couldn't join %s", ssid);
        /* Joined: the address comes a moment later (fathom-wifi woke DHCP at
         * the join). Watched for every 250 ms by what costs nothing (the
         * lease's note of its network, the link, the interface's address),
         * and the status asked as soon as it's there; and asked every second
         * whatever those say, for 10 s, as before. It was a fixed second
         * before the first look and a second between looks. */
        if (st) poll_status();
        for (int i = 1, early = 0; !st && i <= 40; i++) {
            usleep(250 * 1000);
            int ready = !early && lease_ready(ssid);
            if (!ready && i % 4) continue;
            if (ready) early = 1;                /* (once: if the status disagrees, the second's pace) */
            poll_status();
            pthread_mutex_lock(&W.mu);
            int done = W.st.state == UI_WIFI_CONNECTED && !strcmp(W.st.ssid, ssid);
            pthread_mutex_unlock(&W.mu);
            if (done) break;
        }
        set_busy("");
        saved_marks();
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
        poll_status();                           /* (it may have been the one joined) */
        saved_marks();
        break;
    }
}

/* fathom/wifi.txt: its networks joined, and so saved, as if typed (one not in
 * range is saved all the same, joined when it is); the file gone first, its
 * passwords overwritten, so they're not left on the card whatever happens */
static void import_file(const char *path)
{
    char text[4096];
    int fd = open(path, O_RDWR);
    if (fd < 0) return;
    ssize_t n = read(fd, text, sizeof text);
    if (n > 0) {
        char zero[sizeof text] = {0};
        lseek(fd, 0, SEEK_SET);
        if (write(fd, zero, (size_t)n) != n) fprintf(stderr, "wifi: wifi.txt not overwritten\n");
        fsync(fd);
    }
    close(fd);
    unlink(path);
    if (n <= 0) return;
    static wifitxt_net_t nets[WIFITXT_MAX];
    int bad = 0, k = wifitxt_parse(text, (size_t)n, nets, WIFITXT_MAX, &bad);
    memset(text, 0, sizeof text);
    fprintf(stderr, "wifi: wifi.txt: %d network(s), %d line(s) not used\n", k, bad);
    if (!k) say(bad ? "wifi.txt: no network in it could be used" : "wifi.txt was empty");
    for (int i = 0; i < k; i++) {
        connect_st = -1;
        do_cmd(CMD_CONNECT, nets[i].ssid, nets[i].pass);
        if (!connect_st) say_name("Wi-Fi network %s saved", nets[i].ssid);
        if (!connect_st && i + 1 < k) sleep(3);  /* (each said in turn) */
    }
    memset(nets, 0, sizeof nets);
}

static void *worker(void *arg)
{
    (void)arg;
    int idle_ms = POLL_MS;                       /* on, not joined: grows while nothing changes */
    int away = 0, away_left = 0;                 /* (see AWAY_LOOK_MS) */
    int stuck = 0;                               /* (see NOADDR_MS) */
    int revived = 0;                             /* times in a row the daemon was started again */
    uint32_t revived_at = 0;
    sh_wait_home(3000);                          /* (Home first: shell.h) */
    for (;;) {
        away = stuck = 0;
        pthread_mutex_lock(&W.mu);
        if (!W.cmd && !W.on) {
            pthread_cond_wait(&W.cv, &W.mu);     /* off: nothing to poll until a command */
        } else if (!W.cmd) {
            int ms = join_until ? POLL_JOIN_MS
                   : W.st.state == UI_WIFI_CONNECTED ? POLL_CONNECTED_MS
                   : W.st.state == UI_WIFI_CONNECTING ? (noaddr_ms ? noaddr_ms : POLL_MS) : idle_ms;
            if (scan_due && scan_at) {           /* the first scan, when it's due */
                int32_t left = (int32_t)(scan_at - clock_ms32());
                if (left < ms) ms = left < 0 ? 0 : left;
            }
            if (pace_next && pace_s < PACE_MAX_S) {   /* searching less often, when due */
                int32_t left = (int32_t)(pace_next - clock_ms32());
                if (left < ms) ms = left < 0 ? 0 : left;
            }
            /* On, with no network joined: the status is asked less and less
             * often (idle_ms, up to 30 s: two or three programs from the card
             * each time). So a network coming back isn't missed for that
             * long, the wait goes 2 s at a time and looks at the link itself
             * (a file read): up, the status is asked at once. */
            away = !join_until && W.st.state == UI_WIFI_ON && ms == idle_ms && ms > AWAY_LOOK_MS;
            /* (joined with no address, asked less often: the same 2 s steps) */
            stuck = !join_until && W.st.state == UI_WIFI_CONNECTING && noaddr_ms && ms == noaddr_ms && ms > AWAY_LOOK_MS;
            if (away || stuck) {
                if (away_left <= 0) away_left = ms;
                ms = away_left < AWAY_LOOK_MS ? away_left : AWAY_LOOK_MS;
                away_left -= ms;
            } else away_left = 0;
            mono_cond_wait_ms(&W.cv, &W.mu, ms);
        }
        int cmd = W.cmd, on = W.on, wish = W.list_wanted;
        char ssid[33], pass[64];
        snprintf(ssid, sizeof ssid, "%s", W.cmd_ssid);
        snprintf(pass, sizeof pass, "%s", W.cmd_pass);
        W.cmd = CMD_NONE;
        memset(W.cmd_pass, 0, sizeof W.cmd_pass);
        pthread_mutex_unlock(&W.mu);
        if (cmd) {
            do_cmd(cmd, ssid, pass);
            idle_ms = POLL_MS;
            away_left = 0;
        } else if (on && away && away_left > 0 && !link_up()) {
            /* (not due yet, and no link: nothing to ask) */
        } else if (on && stuck && away_left > 0 && link_up() && !has_ip()) {
            /* (not due yet; still joined, still no address: nothing to ask) */
        } else if (on) {
            away_left = 0;
            int changed = look();
            noaddr_follow();
            /* wpa_supplicant gone (killed for memory, or the chip reset under
             * it): the status says off while Wi-Fi is on. Nothing noticed, and
             * it stayed "on, not connected" until turned off and on by hand.
             * Put away and started again: at once, then a minute apart, three
             * times in a row at most. */
            pthread_mutex_lock(&W.mu);
            int dead = W.on && W.st.state == UI_WIFI_OFF;
            if (W.st.state == UI_WIFI_CONNECTED) revived = 0;
            pthread_mutex_unlock(&W.mu);
            if (dead && revived < 3 && (!revived || clock_ms32() - revived_at >= 60000)) {
                revived++;
                revived_at = clock_ms32();
                fprintf(stderr, "wifi: its daemon is gone: starting it again (%d)\n", revived);
                sh_run(FATHOM_WIFI " off >/dev/null 2>&1", NULL, 0);
                do_cmd(CMD_START, "", "");
                idle_ms = POLL_MS;
                continue;
            }
            idle_ms = changed ? POLL_MS : idle_ms * 2 > POLL_IDLE_MAX_MS ? POLL_IDLE_MAX_MS : idle_ms * 2;
            if (join_until) {
                /* connected: a list that's wanted, a little later; nothing being
                 * joined (or the window over): now. Then the usual pace. */
                pthread_mutex_lock(&W.mu);
                int state = W.st.state;
                pthread_mutex_unlock(&W.mu);
                uint32_t t = clock_ms32();
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
            if (wish && !scan_due) {             /* Settings > Wi-Fi is waiting for its list */
                uint32_t t = clock_ms32();
                scan_due = 1;
                /* (no join to wait for, or one whose 5 s are long over: now) */
                if (!join_until && (!scan_at || t - scan_at < 0x80000000u || scan_at - t > SCAN_AFTER_JOIN_MS))
                    scan_at = t ? t : 1;
            }
            if (scan_due && scan_at && (int32_t)(scan_at - clock_ms32()) <= 0) {
                scan_due = 0;
                scan_at = 0;
                scan();
            }
            pthread_mutex_lock(&W.mu);
            int st_now = W.st.state;
            pthread_mutex_unlock(&W.mu);
            pace_follow(st_now);
        }
        memset(pass, 0, sizeof pass);
        pthread_mutex_lock(&W.mu);
        char imp[sizeof W.import] = "";
        if (W.import[0] && W.on && !W.cmd) {
            snprintf(imp, sizeof imp, "%s", W.import);
            W.import[0] = '\0';
        }
        pthread_mutex_unlock(&W.mu);
        if (imp[0]) import_file(imp);
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
    mono_cond_init(&W.cv);
    if (thread_detached(worker, NULL, 0)) return;
    if (on) {
        set_busy("Starting");                    /* (said from now: the worker lets Home be drawn first) */
        request(CMD_START, NULL, NULL);
    }
    W.seq++;
}

int wifi_ui_state(char *busy, size_t cap, ui_wifi_status_t *st)
{
    pthread_mutex_lock(&W.mu);
    int on = W.on;
    snprintf(busy, cap, "%s", W.busy[0] ? W.busy : W.note);   /* (the note is only shown: it holds nothing up) */
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
    /* The list asked for while Wi-Fi is off or busy (Settings > Wi-Fi opened
     * before or during a start): remembered, and looked for when it can be */
    if (what == UI_WIFI_SCAN && (busy || !on)) {
        pthread_mutex_lock(&W.mu);
        W.list_wanted = 1;
        W.list_wanted_at = clock_ms32();
        pthread_mutex_unlock(&W.mu);
        return;
    }
    if (busy && what != UI_WIFI_TOGGLE) return;
    switch (what) {
    case UI_WIFI_TOGGLE: request(on ? CMD_STOP : CMD_START, NULL, NULL); break;
    case UI_WIFI_SCAN: request(CMD_SCAN, NULL, NULL); break;
    case UI_WIFI_CONNECT: if (on && ssid && *ssid) request(CMD_CONNECT, ssid, password); break;
    case UI_WIFI_FORGET: if (on && ssid && *ssid) request(CMD_FORGET, ssid, NULL); break;
    }
}

void wifi_import(const char *path)
{
    pthread_mutex_lock(&W.mu);
    snprintf(W.import, sizeof W.import, "%s", path);
    pthread_cond_signal(&W.cv);
    pthread_mutex_unlock(&W.mu);
}

void wifi_wake(void)
{
    pthread_mutex_lock(&W.mu);
    if (W.on && !W.cmd && W.st.state == UI_WIFI_ON) {   /* (a command waiting stays: it's the user's) */
        W.cmd = CMD_WAKE;
        pthread_cond_signal(&W.cv);
    }
    pthread_mutex_unlock(&W.mu);
}

void wifi_screen(int on)
{
    pthread_mutex_lock(&W.mu);
    W.screen = on;
    pthread_mutex_unlock(&W.mu);
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
