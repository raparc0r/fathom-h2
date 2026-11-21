/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DNS results kept for a while (dnscache.h).
 *
 * Measured on the H2 (2026-11-21, the owner's router): a lookup takes 30 ms to
 * 900 ms, and now and then the router answers REFUSED, which musl ignores, so
 * the lookup fails after its 5 s timeout ("Try again"). A station's start, a
 * station change, a reconnect, FIP's song info every 15 s and each HLS
 * segment asked again each time. Here a result is given again for 2 minutes
 * without asking, and for up to an hour when asking fails for the moment (the
 * address is still the station's; were it not, the connection fails as it
 * would have, and the next lookup asks again).
 *
 * A result handed out is a copy in one block of ours, recorded until it's
 * freed (freeaddrinfo() tells ours from musl's by that record). */
#include "net/dnscache.h"

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "core/lat.h"

#define ENTRIES 16
#define ADDRS 8           /* addresses kept per name */
#define OUTSTANDING 32    /* results handed out and not yet freed */
#define PREFETCH_Q 8

typedef struct {
    char host[256];
    int family, socktype, protocol, flags;   /* the hints it was asked with */
    int n;
    struct { int family, socktype, protocol; socklen_t len; struct sockaddr_storage sa; } a[ADDRS];
    uint32_t at;          /* when it was looked up (0: empty) */
} entry_t;

typedef struct {          /* what's handed out: n of these in one block */
    struct addrinfo ai;
    struct sockaddr_storage sa;
} slot_t;

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static entry_t cache[ENTRIES];
static void *out[OUTSTANDING];
static uint32_t (*clock_fn)(void);
static dns_resolve_fn g_resolve;      /* (for the prefetch: the last ones used) */
static dns_free_fn g_free;

static uint32_t now_ms(void)
{
    if (clock_fn) return clock_fn();
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint32_t t = (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000);
    return t ? t : 1;     /* (0 marks an empty entry) */
}

void dnscache_test_clock(uint32_t (*now)(void)) { clock_fn = now; }

void dnscache_test_reset(void)
{
    pthread_mutex_lock(&mu);
    memset(cache, 0, sizeof cache);
    pthread_mutex_unlock(&mu);
}

/* A port number in service ("443"), or -1 */
static int numeric_port(const char *service)
{
    if (!service || !*service) return -1;
    long p = 0;
    for (const char *c = service; *c; c++) {
        if (!isdigit((unsigned char)*c)) return -1;
        p = p * 10 + (*c - '0');
        if (p > 65535) return -1;
    }
    return (int)p;
}

static int cacheable(const char *node, const char *service, const struct addrinfo *h)
{
    if (!node || !*node || strlen(node) >= sizeof cache[0].host || numeric_port(service) < 0) return 0;
    if (h && (h->ai_flags & (AI_PASSIVE | AI_NUMERICHOST | AI_CANONNAME))) return 0;
    struct in6_addr a6;                    /* an address already: nothing to look up */
    if (inet_pton(AF_INET, node, &a6) == 1 || inet_pton(AF_INET6, node, &a6) == 1) return 0;
    return 1;
}

/* (under mu) */
static entry_t *find(const char *node, const struct addrinfo *h)
{
    int fam = h ? h->ai_family : AF_UNSPEC, st = h ? h->ai_socktype : 0;
    int pr = h ? h->ai_protocol : 0, fl = h ? h->ai_flags : 0;
    for (int i = 0; i < ENTRIES; i++) {
        entry_t *e = &cache[i];
        if (e->at && e->family == fam && e->socktype == st && e->protocol == pr && e->flags == fl &&
            !strcasecmp(e->host, node))
            return e;
    }
    return NULL;
}

/* A copy of e for port, recorded as handed out; NULL if none can be. (under mu) */
static struct addrinfo *hand_out(const entry_t *e, int port)
{
    int k = 0;
    while (k < OUTSTANDING && out[k]) k++;
    if (k == OUTSTANDING || e->n <= 0) return NULL;
    slot_t *s = calloc((size_t)e->n, sizeof *s);
    if (!s) return NULL;
    for (int i = 0; i < e->n; i++) {
        memcpy(&s[i].sa, &e->a[i].sa, e->a[i].len);
        if (s[i].sa.ss_family == AF_INET) ((struct sockaddr_in *)&s[i].sa)->sin_port = htons((uint16_t)port);
        else if (s[i].sa.ss_family == AF_INET6) ((struct sockaddr_in6 *)&s[i].sa)->sin6_port = htons((uint16_t)port);
        s[i].ai.ai_family = e->a[i].family;
        s[i].ai.ai_socktype = e->a[i].socktype;
        s[i].ai.ai_protocol = e->a[i].protocol;
        s[i].ai.ai_addrlen = e->a[i].len;
        s[i].ai.ai_addr = (struct sockaddr *)&s[i].sa;
        s[i].ai.ai_next = i + 1 < e->n ? &s[i + 1].ai : NULL;
    }
    out[k] = s;
    return &s[0].ai;
}

/* What resolve found, kept (the oldest entry makes room). (under mu) */
static void keep(const char *node, const struct addrinfo *h, const struct addrinfo *res)
{
    entry_t *e = find(node, h);
    if (!e) {
        e = &cache[0];
        for (int i = 0; i < ENTRIES; i++) {
            if (!cache[i].at) { e = &cache[i]; break; }
            if ((int32_t)(cache[i].at - e->at) < 0) e = &cache[i];
        }
    }
    entry_t n = {0};
    snprintf(n.host, sizeof n.host, "%s", node);
    n.family = h ? h->ai_family : AF_UNSPEC;
    n.socktype = h ? h->ai_socktype : 0;
    n.protocol = h ? h->ai_protocol : 0;
    n.flags = h ? h->ai_flags : 0;
    for (const struct addrinfo *a = res; a && n.n < ADDRS; a = a->ai_next) {
        if (!a->ai_addr || a->ai_addrlen > sizeof n.a[0].sa) continue;
        if (a->ai_addr->sa_family != AF_INET && a->ai_addr->sa_family != AF_INET6) continue;
        n.a[n.n].family = a->ai_family;
        n.a[n.n].socktype = a->ai_socktype;
        n.a[n.n].protocol = a->ai_protocol;
        n.a[n.n].len = a->ai_addrlen;
        memcpy(&n.a[n.n].sa, a->ai_addr, a->ai_addrlen);
        n.n++;
    }
    if (!n.n) return;
    n.at = now_ms();
    *e = n;
}

int dnscache_getaddrinfo(const char *node, const char *service, const struct addrinfo *hints,
                         struct addrinfo **res, dns_resolve_fn resolve, dns_free_fn real_free)
{
    if (!cacheable(node, service, hints)) return resolve(node, service, hints, res);
    int port = numeric_port(service);
    uint32_t t0 = now_ms();
    pthread_mutex_lock(&mu);
    g_resolve = resolve;
    g_free = real_free;
    entry_t *e = find(node, hints);
    if (e && (uint32_t)(t0 - e->at) < DNSCACHE_FRESH_MS) {
        struct addrinfo *r = hand_out(e, port);
        pthread_mutex_unlock(&mu);
        if (r) {
            LAT("dns: %s from the cache", node);
            *res = r;
            return 0;
        }
        return resolve(node, service, hints, res);   /* (too many out: as without the cache) */
    }
    pthread_mutex_unlock(&mu);

    struct addrinfo *got = NULL;
    int rc = resolve(node, service, hints, &got);
    LAT("dns: %s looked up (%d) in %u ms", node, rc, now_ms() - t0);
    pthread_mutex_lock(&mu);
    if (rc == 0) {
        keep(node, hints, got);
        pthread_mutex_unlock(&mu);
        *res = got;                       /* (the real result: freed by the real free) */
        return 0;
    }
    /* Asking failed for the moment: what it was lately, if anything */
    e = rc == EAI_AGAIN ? find(node, hints) : NULL;
    struct addrinfo *r = e && (uint32_t)(now_ms() - e->at) < DNSCACHE_STALE_MS ? hand_out(e, port) : NULL;
    pthread_mutex_unlock(&mu);
    if (r) {
        fprintf(stderr, "dns: %s didn't answer: its address from before\n", node);
        *res = r;
        return 0;
    }
    return rc;
}

void dnscache_freeaddrinfo(struct addrinfo *ai, dns_free_fn real_free)
{
    if (!ai) return;
    pthread_mutex_lock(&mu);
    for (int k = 0; k < OUTSTANDING; k++) {
        if (out[k] && out[k] == (void *)ai) {     /* (ai is the block's first slot) */
            out[k] = NULL;
            pthread_mutex_unlock(&mu);
            free(ai);
            return;
        }
    }
    pthread_mutex_unlock(&mu);
    real_free(ai);
}

/* ---- prefetch ---- */

static char queue[PREFETCH_Q][256 + 8];   /* "host port" */
static int q_n, q_running;

static void *prefetch_run(void *arg)
{
    (void)arg;
    for (;;) {
        char item[sizeof queue[0]];
        pthread_mutex_lock(&mu);
        if (!q_n || !g_resolve) { q_n = 0; q_running = 0; pthread_mutex_unlock(&mu); return NULL; }
        memcpy(item, queue[0], sizeof item);
        memmove(queue[0], queue[1], (size_t)(q_n - 1) * sizeof queue[0]);
        q_n--;
        dns_resolve_fn resolve = g_resolve;
        dns_free_fn real_free = g_free;
        pthread_mutex_unlock(&mu);
        char *sp = strchr(item, ' ');
        if (!sp) continue;
        *sp = '\0';
        struct addrinfo h = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM}, *r = NULL;   /* (FFmpeg's) */
        if (dnscache_getaddrinfo(item, sp + 1, &h, &r, resolve, real_free) == 0) dnscache_freeaddrinfo(r, real_free);
    }
}

void dnscache_prefetch_url(const char *url)
{
    if (!url) return;
    const char *p;
    int port;
    if (!strncasecmp(url, "https://", 8)) { p = url + 8; port = 443; }
    else if (!strncasecmp(url, "http://", 7)) { p = url + 7; port = 80; }
    else return;
    const char *at = p + strcspn(p, "/?#");           /* user@ before the host */
    for (const char *c = p; c < at; c++) if (*c == '@') p = c + 1;
    if (*p == '[') return;                            /* an IPv6 address */
    size_t n = strcspn(p, ":/?#");
    if (!n || n >= 256) return;
    char host[256];
    memcpy(host, p, n);
    host[n] = '\0';
    if (p[n] == ':') port = atoi(p + n + 1);
    if (port <= 0 || port > 65535) return;
    struct addrinfo h = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM};
    char ps[8];
    snprintf(ps, sizeof ps, "%d", port);
    if (!cacheable(host, ps, &h)) return;
    pthread_mutex_lock(&mu);
    entry_t *e = find(host, &h);
    int fresh = e && (uint32_t)(now_ms() - e->at) < DNSCACHE_FRESH_MS / 2;
    int queued = 0;
    for (int i = 0; i < q_n && !queued; i++) {
        size_t hl = strlen(host);
        queued = !strncasecmp(queue[i], host, hl) && queue[i][hl] == ' ';
    }
    if (fresh || queued || q_n == PREFETCH_Q || !g_resolve) { pthread_mutex_unlock(&mu); return; }
    snprintf(queue[q_n++], sizeof queue[0], "%s %d", host, port);
    if (!q_running) {
        pthread_attr_t a;
        pthread_t t;
        pthread_attr_init(&a);
        pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
        pthread_attr_setstacksize(&a, 64 * 1024);
        q_running = pthread_create(&t, &a, prefetch_run, NULL) == 0;
        pthread_attr_destroy(&a);
        if (!q_running) q_n = 0;
    }
    pthread_mutex_unlock(&mu);
}

void dnscache_use(dns_resolve_fn resolve, dns_free_fn real_free)
{
    pthread_mutex_lock(&mu);
    g_resolve = resolve;
    g_free = real_free;
    pthread_mutex_unlock(&mu);
}

#ifdef DNSCACHE_WRAP
/* The player (linked with --wrap=getaddrinfo --wrap=freeaddrinfo): every
 * lookup in it, FFmpeg's included, goes through the cache */
int __real_getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res);
void __real_freeaddrinfo(struct addrinfo *ai);

int __wrap_getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res)
{
    return dnscache_getaddrinfo(node, service, hints, res, __real_getaddrinfo, __real_freeaddrinfo);
}

void __wrap_freeaddrinfo(struct addrinfo *ai)
{
    dnscache_freeaddrinfo(ai, __real_freeaddrinfo);
}

__attribute__((constructor)) static void dnscache_wrap_init(void)
{
    dnscache_use(__real_getaddrinfo, __real_freeaddrinfo);
}
#endif
