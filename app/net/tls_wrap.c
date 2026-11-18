/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * One certificate bundle for all HTTPS connections (tls_wrap.h). For each
 * connection, FFmpeg 6.1's tls_open (tls_mbedtls.c) parses the bundle into
 * the mbedtls_x509_crt in its context (~140 certificates: about 400 KB of
 * heap and 50-100 ms of the H2's CPU), hands it to
 * mbedtls_ssl_conf_ca_chain, and tls_close frees it whole with
 * mbedtls_x509_crt_free. Nothing in between writes to it: mbedTLS 3.6 only
 * reads the trusted chain while verifying (x509_crt_verify_chain and
 * x509_crt_find_parent_in walk it through ->next; the only state a CA's key
 * caches, RSA's Montgomery constant RN, is written under the key's own mutex,
 * MBEDTLS_THREADING_C being on; ECDSA uses the curves' static comb tables).
 * FFmpeg never appends to it or frees it in pieces. So the bundle is parsed once
 * into a chain kept here, and each connection gets a copy of its first node:
 * the copy's ->next is the shared second certificate, its buffers and key are
 * the shared first one's, and the chain verifies exactly as a private parse
 * would (same certificates, same order). A free of such a copy (recognised by
 * its raw buffer) only gives the loan back.
 *
 * The chain outlives its last connection by a grace period (30 s), so a
 * station change (the engine hangs up before it connects again), HLS segments
 * and playlist reloads, radiometa's song info every 15 s and a radio
 * directory's queries in a row don't parse it again; then it's freed (by a
 * short-lived thread), so leaving the radio gives the memory back.
 *
 * The bundle is checked (stat) at every connection: one that changed is parsed
 * again, the old chain kept for the connections still using it. One missing,
 * unreadable or failing to parse is passed to mbedTLS as before (the same error
 * back, the connection refused), and nothing is kept. Other files (the network
 * drive's own certificate, tlscert.c) go straight through.
 */
#include "net/tls_wrap.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include <mbedtls/x509_crt.h>

#include "audio/ff_util.h"

int __real_mbedtls_x509_crt_parse_file(mbedtls_x509_crt *chain, const char *path);
void __real_mbedtls_x509_crt_free(mbedtls_x509_crt *crt);
int __wrap_mbedtls_x509_crt_parse_file(mbedtls_x509_crt *chain, const char *path);
void __wrap_mbedtls_x509_crt_free(mbedtls_x509_crt *crt);

typedef struct chain {
    mbedtls_x509_crt crt;        /* the parsed bundle */
    int refs;                    /* connections holding a copy of its first node */
    dev_t dev;                   /* the file it was parsed from, as it was */
    ino_t ino;
    off_t size;
    struct timespec mtime;
    struct chain *next;
} chain_t;

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static chain_t *chains;          /* every chain held; cur is the bundle as it is now */
static chain_t *cur;
static unsigned n_parses;
static unsigned grace_ms = 30000;
static struct timespec deadline; /* cur, unused, is freed then */
static int reaping;              /* a thread is waiting for the deadline */

static long ms_to(const struct timespec *a, const struct timespec *b)
{
    return (b->tv_sec - a->tv_sec) * 1000L + (b->tv_nsec - a->tv_nsec) / 1000000L;
}

static int same_file(const chain_t *c, const struct stat *st)
{
    return c->dev == st->st_dev && c->ino == st->st_ino && c->size == st->st_size &&
           c->mtime.tv_sec == st->st_mtim.tv_sec && c->mtime.tv_nsec == st->st_mtim.tv_nsec;
}

/* (under mu) */
static void drop(chain_t *c)
{
    for (chain_t **p = &chains; *p; p = &(*p)->next)
        if (*p == c) { *p = c->next; break; }
    if (c == cur) cur = NULL;
    __real_mbedtls_x509_crt_free(&c->crt);
    free(c);
    fprintf(stderr, "tls: certificates freed\n");
}

/* The chain crt is a loan from, or NULL. (under mu) */
static chain_t *lender(const mbedtls_x509_crt *crt)
{
    if (!crt->raw.p) return NULL;
    for (chain_t *c = chains; c; c = c->next)
        if (crt != &c->crt && crt->raw.p == c->crt.raw.p) return c;
    return NULL;
}

static void *reap(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&mu);
    while (cur && !cur->refs) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long left = ms_to(&now, &deadline);
        if (left <= 0) { drop(cur); break; }
        pthread_mutex_unlock(&mu);       /* (a new deadline, set meanwhile, is seen on waking) */
        struct timespec t = {left / 1000, (left % 1000) * 1000000L};
        nanosleep(&t, NULL);
        pthread_mutex_lock(&mu);
    }
    reaping = 0;
    pthread_mutex_unlock(&mu);
    return NULL;
}

/* cur has just lost its last user. (under mu) */
static void idle(void)
{
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += grace_ms / 1000;
    deadline.tv_nsec += (long)(grace_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) { deadline.tv_sec++; deadline.tv_nsec -= 1000000000L; }
    if (reaping) return;
    pthread_attr_t a;
    pthread_t t;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&a, 64 * 1024);
    reaping = pthread_create(&t, &a, reap, NULL) == 0;
    pthread_attr_destroy(&a);
    if (!reaping) drop(cur);             /* no thread to wait: don't keep it */
}

int __wrap_mbedtls_x509_crt_parse_file(mbedtls_x509_crt *chain, const char *path)
{
    const char *bundle = ff_ca_file();
    if (!chain || !path || !bundle || strcmp(path, bundle))
        return __real_mbedtls_x509_crt_parse_file(chain, path);
    pthread_mutex_lock(&mu);
    if (lender(chain)) {                 /* adding to a loan would add to everyone's */
        pthread_mutex_unlock(&mu);
        return MBEDTLS_ERR_X509_BAD_INPUT_DATA;
    }
    struct stat st;
    if (chain->version || stat(path, &st)) {   /* a chain of its own to add to, or no file */
        pthread_mutex_unlock(&mu);
        return __real_mbedtls_x509_crt_parse_file(chain, path);
    }
    if (cur && !same_file(cur, &st)) {   /* changed: new connections get it anew */
        chain_t *old = cur;
        cur = NULL;
        if (!old->refs) drop(old);
    }
    if (!cur) {
        chain_t *c = calloc(1, sizeof *c);
        if (!c) { pthread_mutex_unlock(&mu); return MBEDTLS_ERR_X509_ALLOC_FAILED; }
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        mbedtls_x509_crt_init(&c->crt);
        int r = __real_mbedtls_x509_crt_parse_file(&c->crt, path);
        n_parses++;
        if (r) {                         /* as without the wrapper: refused, nothing kept */
            __real_mbedtls_x509_crt_free(&c->crt);
            free(c);
            pthread_mutex_unlock(&mu);
            return r;
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        c->dev = st.st_dev;              /* (as before the parse: a change during it is seen next time) */
        c->ino = st.st_ino;
        c->size = st.st_size;
        c->mtime = st.st_mtim;
        c->next = chains;
        chains = cur = c;
        int n = 0;
        for (const mbedtls_x509_crt *x = &c->crt; x; x = x->next) n++;
        fprintf(stderr, "tls: %d certificates parsed in %ld ms\n", n, ms_to(&t0, &t1));
    }
    cur->refs++;
    *chain = cur->crt;                   /* the first node; the rest through its ->next */
    pthread_mutex_unlock(&mu);
    return 0;
}

void __wrap_mbedtls_x509_crt_free(mbedtls_x509_crt *crt)
{
    if (!crt || !crt->raw.p) { __real_mbedtls_x509_crt_free(crt); return; }
    pthread_mutex_lock(&mu);
    chain_t *c = lender(crt);
    if (!c) {
        pthread_mutex_unlock(&mu);
        __real_mbedtls_x509_crt_free(crt);
        return;
    }
    memset(crt, 0, sizeof *crt);         /* as mbedtls_x509_crt_free leaves it */
    if (--c->refs == 0) {
        if (c != cur) drop(c);           /* an older bundle: nobody will want it again */
        else idle();
    }
    pthread_mutex_unlock(&mu);
}

void tls_wrap_stats(tls_wrap_stats_t *s)
{
    pthread_mutex_lock(&mu);
    s->parses = n_parses;
    s->chains = 0;
    s->lent = 0;
    for (const chain_t *c = chains; c; c = c->next) {
        s->chains++;
        s->lent += (unsigned)c->refs;
    }
    pthread_mutex_unlock(&mu);
}

void tls_wrap_set_grace_ms(unsigned ms)
{
    pthread_mutex_lock(&mu);
    grace_ms = ms;
    pthread_mutex_unlock(&mu);
}
