/* SPDX-License-Identifier: GPL-3.0-or-later */
/* SSH (Settings > SSH): fathom-ssh sets root's password (the one on the
 * screen, also the serial console's) and runs dropbear while SSH is on, over
 * the USB link and Wi-Fi alike. A worker thread runs it, so the UI never
 * waits on it (the first start makes the host key, about a second). */
#include "ssh.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define FATHOM_SSH "/usr/sbin/fathom-ssh"

static struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int want, pending;
    char password[32];
    int state;                  /* UI_SSH_* */
    unsigned seq;
} S = {.mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER};

static void set_state(int st)
{
    pthread_mutex_lock(&S.mu);
    if (S.state != st) { S.state = st; S.seq++; }
    pthread_mutex_unlock(&S.mu);
}

/* s as one single-quoted shell word. */
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

static int apply(int on, const char *password)
{
    char qp[80], c[200];
    shell_quote(password, qp, sizeof qp);
    snprintf(c, sizeof c, "printf '%%s\\n' %s | " FATHOM_SSH " %s >/dev/null 2>&1", qp, on ? "on" : "off");
    memset(qp, 0, sizeof qp);
    int st = system(c);
    memset(c, 0, sizeof c);
    return st == 0 ? 0 : -1;
}

static void *worker(void *arg)
{
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&S.mu);
        while (!S.pending) pthread_cond_wait(&S.cv, &S.mu);
        int want = S.want;
        char pass[32];
        snprintf(pass, sizeof pass, "%s", S.password);
        S.pending = 0;
        pthread_mutex_unlock(&S.mu);

        if (want) set_state(UI_SSH_STARTING);
        int ok = apply(want, pass) == 0;
        memset(pass, 0, sizeof pass);
        fprintf(stderr, "ssh: %s%s\n", want ? "on" : "off", ok ? "" : " (fathom-ssh failed)");
        set_state(!want ? UI_SSH_OFF : ok ? UI_SSH_ON : UI_SSH_FAILED);
    }
    return NULL;
}

int ssh_available(void)
{
    return !access(FATHOM_SSH, X_OK) && !access("/usr/sbin/dropbear", X_OK);
}

static void request(int on, const char *password)
{
    pthread_mutex_lock(&S.mu);
    S.want = on;
    if (password) snprintf(S.password, sizeof S.password, "%s", password);
    S.pending = 1;
    S.seq++;
    pthread_cond_signal(&S.cv);
    pthread_mutex_unlock(&S.mu);
}

void ssh_init(int on, const char *password)
{
    if (!ssh_available()) return;
    S.state = on ? UI_SSH_STARTING : UI_SSH_OFF;
    pthread_t th;
    if (pthread_create(&th, NULL, worker, NULL)) return;
    pthread_detach(th);
    request(on, password);
}

void ssh_set(int on, const char *password) { request(on, password); }

/* An interface's IPv4 address, "" without one. */
static void if_ip(const char *name, char *out, size_t cap)
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

int ssh_ui_state(ui_ssh_t *st)
{
    pthread_mutex_lock(&S.mu);
    int on = S.want;
    if (st) {
        memset(st, 0, sizeof *st);
        st->state = S.state;
        snprintf(st->password, sizeof st->password, "%s", S.password);
    }
    pthread_mutex_unlock(&S.mu);
    if (st) {
        if_ip("usb0", st->usb_ip, sizeof st->usb_ip);
        if_ip("wlan0", st->wifi_ip, sizeof st->wifi_ip);
    }
    return on;
}

unsigned ssh_seq(void)
{
    pthread_mutex_lock(&S.mu);
    unsigned s = S.seq;
    pthread_mutex_unlock(&S.mu);
    return s;
}
