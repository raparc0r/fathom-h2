/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* SSH (Settings > SSH): fathom-ssh sets root's password (the one on the
 * screen, also the serial console's) and runs dropbear while SSH is on, over
 * the USB link and Wi-Fi alike. A worker thread runs it, so the UI never
 * waits on it (the first start makes the host key, about a second). */
#include "ssh.h"
#include "shell.h"
#include "core/clock.h"
#include "core/thread.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define FATHOM_SSH "/usr/sbin/fathom-ssh"
#define DROPBEAR_PID "/var/run/dropbear.pid"   /* (the script's own "alive") */

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

static int apply(int on, const char *password)
{
    char qp[80], c[200];
    sh_quote(password, qp, sizeof qp);
    snprintf(c, sizeof c, "printf '%%s\\n' %s | " FATHOM_SSH " %s >/dev/null 2>&1", qp, on ? "on" : "off");
    memset(qp, 0, sizeof qp);
    int st = system(c);
    memset(c, 0, sizeof c);
    return st == 0 ? 0 : -1;
}

static void *worker(void *arg)
{
    (void)arg;
    int on = 0;                                  /* dropbear was started: looked for once a minute */
    sh_revive_t again = {0};                     /* (see shell.h) */
    sh_wait_home(3000);                          /* (Home first: shell.h) */
    for (;;) {
        pthread_mutex_lock(&S.mu);
        int look = 0;
        while (!S.pending && !look) {
            if (!on) pthread_cond_wait(&S.cv, &S.mu);
            else look = mono_cond_wait_ms(&S.cv, &S.mu, SH_LOOK_MS) != 0;
        }
        int asked = S.pending, want = S.want;
        char pass[32];
        snprintf(pass, sizeof pass, "%s", S.password);
        S.pending = 0;
        pthread_mutex_unlock(&S.mu);

        if (asked) {
            again = (sh_revive_t){0};
        } else {
            /* The minute's look. dropbear gone (killed for memory): nothing
             * looked, and the screen went on saying On with no SSH behind
             * it. Started again as below, three times in a row at most,
             * then left as one that couldn't be started. */
            int what = sh_revive(&again, sh_pid_alive(DROPBEAR_PID), clock_ms32());
            if (what == SH_REVIVE) fprintf(stderr, "ssh: dropbear is gone: starting it again (%d)\n", again.revived);
            if (what == SH_GIVE_UP) {
                fprintf(stderr, "ssh: dropbear is gone again: started again three times already, left off\n");
                on = 0;
                set_state(UI_SSH_FAILED);
            }
            if (what != SH_REVIVE) {
                memset(pass, 0, sizeof pass);
                continue;
            }
        }
        if (want) set_state(UI_SSH_STARTING);
        int ok = apply(want, pass) == 0;
        memset(pass, 0, sizeof pass);
        fprintf(stderr, "ssh: %s%s\n", want ? "on" : "off", ok ? "" : " (fathom-ssh failed)");
        set_state(!want ? UI_SSH_OFF : ok ? UI_SSH_ON : UI_SSH_FAILED);
        on = want && ok;
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
    mono_cond_init(&S.cv);              /* (the worker's minute: shell.h) */
    if (thread_detached(worker, NULL, 0)) return;
    request(on, password);
}

void ssh_set(int on, const char *password) { request(on, password); }

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
