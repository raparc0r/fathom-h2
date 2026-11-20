/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The network drive: a worker thread runs fathom-share (lighttpd's WebDAV over
 * HTTPS, with the H2's own certificate from tlscert.c, and mdnsd, from the
 * card) while the drive is wanted and Wi-Fi is connected, and
 * stops it otherwise. While it runs, it reads lighttpd's access log (methods
 * and status codes only) to tell the player when music was added, moved or
 * deleted (for a rescan once it's quiet) and when files are being copied
 * (the H2 stays awake). Like wifi.c. */
#include "share.h"
#include "shell.h"
#include "tlscert.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define FATHOM_SHARE "/usr/sbin/fathom-share"
#define ACCESS_LOG   "/var/run/fathom-share/access.log"
#define HOST         "fathom-h2.local"
#define USER         "fathom"
#define CERT_KEY     "/mnt/sd/fathom/keys/drive.key"   /* (fathom-share reads them from there) */
#define CERT_CRT     "/mnt/sd/fathom/keys/drive.crt"
#define QUIET_MS     10000      /* no change for this long: the music folder is looked at again */
#define BUSY_MS      120000

static struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    char music[512];
    int want, wifi, restart;
    unsigned kick;              /* +1 with each of the above changed: the worker looks */
    char password[32];
    /* what the UI sees */
    int state;                  /* UI_SHARE_* */
    char ip[16];
    char cert[24];              /* the certificate's fingerprint, its start */
    unsigned seq;
    /* activity */
    uint32_t last_change, last_xfer;
    int changed;
} S = {.mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER};

static uint32_t ms_now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)(t.tv_sec * 1000 + t.tv_nsec / 1000000);
}

static void set_state(int st)
{
    pthread_mutex_lock(&S.mu);
    if (S.state != st) { S.state = st; S.seq++; }
    pthread_mutex_unlock(&S.mu);
}

static int start(const char *music, const char *password)
{
    char qm[600], qp[80], c[800], err[200] = "", fp[24];
    if (tlscert_ensure(CERT_KEY, CERT_CRT, HOST, fp, sizeof fp)) return -1;
    pthread_mutex_lock(&S.mu);
    snprintf(S.cert, sizeof S.cert, "%s", fp);
    pthread_mutex_unlock(&S.mu);
    sh_quote(music, qm, sizeof qm);
    sh_quote(password, qp, sizeof qp);
    snprintf(c, sizeof c, "printf '%%s\\n' %s | " FATHOM_SHARE " on %s 2>&1 >/dev/null", qp, qm);
    int st = sh_run(c, err, sizeof err);
    if (st) {
        err[strcspn(err, "\n")] = '\0';
        fprintf(stderr, "share: didn't start: %s\n", err);
        return -1;
    }
    fprintf(stderr, "share: on (https://" HOST "/, certificate %s)\n", fp);
    return 0;
}

static void stop(void)
{
    sh_run(FATHOM_SHARE " off >/dev/null 2>&1", NULL, 0);
    fprintf(stderr, "share: off\n");
}

/* New lines of the access log ("METHOD STATUS"): what happened. The log is
 * emptied once it's grown (lighttpd appends, so it carries on at the start). */
static long log_pos;

static void read_log(void)
{
    FILE *f = fopen(ACCESS_LOG, "re");
    if (!f) return;
    if (fseek(f, 0, SEEK_END) == 0 && ftell(f) < log_pos) log_pos = 0;   /* emptied */
    fseek(f, log_pos, SEEK_SET);
    char line[128];
    int change = 0, xfer = 0;
    while (fgets(line, sizeof line, f)) {
        char m[16];
        int code = 0;
        if (sscanf(line, "%15s %d", m, &code) != 2 || code < 200 || code >= 300) continue;
        if (!strcmp(m, "PUT") || !strcmp(m, "DELETE") || !strcmp(m, "MOVE") || !strcmp(m, "COPY") ||
            !strcmp(m, "MKCOL")) change = 1;
        if (change || !strcmp(m, "GET")) xfer = 1;
    }
    log_pos = ftell(f);
    fclose(f);
    if (log_pos > 32768) {                   /* empty it */
        FILE *t = fopen(ACCESS_LOG, "we");
        if (t) fclose(t);
        log_pos = 0;
    }
    uint32_t now = ms_now();
    pthread_mutex_lock(&S.mu);
    if (change) { S.last_change = now; S.changed = 1; }
    if (xfer) S.last_xfer = now;
    pthread_mutex_unlock(&S.mu);
}

static void *worker(void *arg)
{
    (void)arg;
    int running = 0, failed = 0;
    unsigned seen = ~0u;
    for (;;) {
        pthread_mutex_lock(&S.mu);
        if (running) {                       /* its log, once a second */
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += 1;
            pthread_cond_timedwait(&S.cv, &S.mu, &ts);
        } else {                             /* off: nothing to look at until something changes */
            while (S.kick == seen) pthread_cond_wait(&S.cv, &S.mu);
        }
        seen = S.kick;
        int want = S.want, wifi = S.wifi, restart = S.restart;
        char pass[32], music[512];
        snprintf(pass, sizeof pass, "%s", S.password);
        snprintf(music, sizeof music, "%s", S.music);
        S.restart = 0;
        pthread_mutex_unlock(&S.mu);

        if (!want || !wifi) failed = 0;      /* a new try when it's wanted again */
        if (running && (!want || !wifi || restart)) {
            stop();
            running = 0;
            log_pos = 0;
        }
        if (want && wifi && !running && !failed) {
            set_state(UI_SHARE_STARTING);
            if (start(music, pass) == 0) {
                running = 1;
                log_pos = 0;
                char ip[16];
                if_ip("wlan0", ip, sizeof ip);
                pthread_mutex_lock(&S.mu);
                snprintf(S.ip, sizeof S.ip, "%s", ip);
                S.seq++;
                pthread_mutex_unlock(&S.mu);
            } else {
                failed = 1;
            }
        }
        if (running) read_log();
        set_state(!want ? UI_SHARE_OFF : !wifi ? UI_SHARE_WAITING : running ? UI_SHARE_ON : UI_SHARE_FAILED);
        memset(pass, 0, sizeof pass);
    }
    return NULL;
}

int share_available(void)
{
    return !access(FATHOM_SHARE, X_OK) && !access("/mnt/sd/fathom/share/usr/sbin/lighttpd", X_OK);
}

void share_init(const char *music, int on, const char *password)
{
    if (!share_available()) return;
    pthread_mutex_lock(&S.mu);
    snprintf(S.music, sizeof S.music, "%s", music);
    snprintf(S.password, sizeof S.password, "%s", password ? password : "");
    S.want = on;
    S.state = on ? UI_SHARE_WAITING : UI_SHARE_OFF;
    pthread_mutex_unlock(&S.mu);
    sh_run(FATHOM_SHARE " off >/dev/null 2>&1", NULL, 0);    /* one left from a player that crashed */
    pthread_t th;
    if (pthread_create(&th, NULL, worker, NULL)) return;
    pthread_detach(th);
}

void share_set_wifi(int connected)
{
    pthread_mutex_lock(&S.mu);
    if (S.wifi != connected) { S.wifi = connected; S.kick++; pthread_cond_signal(&S.cv); }
    pthread_mutex_unlock(&S.mu);
}

void share_set(int on, const char *password)
{
    pthread_mutex_lock(&S.mu);
    if (password && strcmp(password, S.password)) {
        snprintf(S.password, sizeof S.password, "%s", password);
        S.restart = 1;
    }
    S.want = on;
    S.seq++;
    S.kick++;
    pthread_cond_signal(&S.cv);
    pthread_mutex_unlock(&S.mu);
}

int share_ui_state(ui_share_t *st)
{
    pthread_mutex_lock(&S.mu);
    int on = S.want;
    if (st) {
        memset(st, 0, sizeof *st);
        st->state = S.state;
        snprintf(st->host, sizeof st->host, "%s", HOST);
        snprintf(st->ip, sizeof st->ip, "%s", S.ip);
        snprintf(st->user, sizeof st->user, "%s", USER);
        snprintf(st->password, sizeof st->password, "%s", S.password);
        snprintf(st->cert, sizeof st->cert, "%s", S.cert);
    }
    pthread_mutex_unlock(&S.mu);
    return on;
}

unsigned share_seq(void)
{
    pthread_mutex_lock(&S.mu);
    unsigned s = S.seq;
    pthread_mutex_unlock(&S.mu);
    return s;
}

int share_take_changes(void)
{
    pthread_mutex_lock(&S.mu);
    int take = S.changed && ms_now() - S.last_change >= QUIET_MS;
    if (take) S.changed = 0;
    pthread_mutex_unlock(&S.mu);
    return take;
}

int share_busy(void)
{
    pthread_mutex_lock(&S.mu);
    int busy = S.last_xfer && ms_now() - S.last_xfer < BUSY_MS;
    pthread_mutex_unlock(&S.mu);
    return busy;
}

void share_shutdown(void)
{
    if (share_available()) stop();
}
