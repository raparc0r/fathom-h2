/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/*
 * DNS answers kept a while (app/net/dnscache.c): a name asked again within two
 * minutes isn't looked up again, with the port asked for; an older answer (up
 * to an hour) is given at once and looked up again in the background; results
 * are freed by the right free; lookups it doesn't keep go straight through;
 * the prefetch asks in the background; with IPv4 only, a lookup for either
 * family asks for IPv4.
 */
#define _DEFAULT_SOURCE
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "net/dnscache.h"

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static uint32_t t_now = 1000;
static uint32_t fake_clock(void) { return t_now; }

static int lookups, frees, fail_with;
static char last_node[256];
static int last_family = -1;       /* the hints' family at the last lookup (-1: no hints) */
static volatile int hold;          /* lookups don't come back while it's set (a slow router) */

static uint32_t real_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000);
}

/* Until the background has nothing left to look up */
static void settle(void)
{
    for (int i = 0; i < 500 && !dnscache_test_idle(); i++) usleep(10000);
}

/* "Resolves" any name to 10.0.0.N (N: the name's length), two addresses (v4, v6) */
static int fake_resolve(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res)
{
    __sync_fetch_and_add(&lookups, 1);
    snprintf(last_node, sizeof last_node, "%s", node ? node : "");
    last_family = hints ? hints->ai_family : -1;
    for (int i = 0; i < 5000 && hold; i++) usleep(1000);
    if (fail_with) return fail_with;
    struct addrinfo *a = calloc(2, sizeof *a);
    struct sockaddr_in *s4 = calloc(1, sizeof *s4);
    struct sockaddr_in6 *s6 = calloc(1, sizeof *s6);
    s4->sin_family = AF_INET;
    s4->sin_port = htons((uint16_t)atoi(service ? service : "0"));
    s4->sin_addr.s_addr = htonl(0x0a000000u | (uint32_t)strlen(node ? node : ""));
    s6->sin6_family = AF_INET6;
    s6->sin6_port = s4->sin_port;
    s6->sin6_addr.s6_addr[15] = 1;
    a[0] = (struct addrinfo){.ai_family = AF_INET, .ai_socktype = SOCK_STREAM, .ai_addrlen = sizeof *s4,
                             .ai_addr = (struct sockaddr *)s4, .ai_next = &a[1]};
    a[1] = (struct addrinfo){.ai_family = AF_INET6, .ai_socktype = SOCK_STREAM, .ai_addrlen = sizeof *s6,
                             .ai_addr = (struct sockaddr *)s6};
    *res = a;
    return 0;
}

static void fake_free(struct addrinfo *a)
{
    frees++;
    free(a[0].ai_addr);
    free(a[1].ai_addr);
    free(a);
}

static const struct addrinfo HINTS = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM};

static int ask(const char *node, const char *port, struct addrinfo **r)
{
    return dnscache_getaddrinfo(node, port, &HINTS, r, fake_resolve, fake_free);
}

static int port_of(const struct addrinfo *a)
{
    if (a->ai_addr->sa_family == AF_INET) return ntohs(((struct sockaddr_in *)a->ai_addr)->sin_port);
    return ntohs(((struct sockaddr_in6 *)a->ai_addr)->sin6_port);
}

static void test_kept(void)
{
    dnscache_test_reset();
    lookups = frees = fail_with = 0;
    struct addrinfo *r = NULL;
    CHECK(ask("stream.example.org", "443", &r) == 0 && lookups == 1);
    CHECK(r && port_of(r) == 443);
    dnscache_freeaddrinfo(r, fake_free);
    CHECK(frees == 1);                               /* the real result: the real free */

    t_now += 60000;                                  /* a minute later, another port */
    r = NULL;
    CHECK(ask("Stream.Example.org", "80", &r) == 0 && lookups == 1);
    CHECK(r && r->ai_family == AF_INET && port_of(r) == 80);
    CHECK(r && ((struct sockaddr_in *)r->ai_addr)->sin_addr.s_addr == htonl(0x0a000000u | 18));
    CHECK(r && r->ai_next && r->ai_next->ai_family == AF_INET6 && port_of(r->ai_next) == 80);
    CHECK(r && r->ai_next && !r->ai_next->ai_next);
    dnscache_freeaddrinfo(r, fake_free);
    CHECK(frees == 1);                               /* ours: not the real free */

    /* old: given at once all the same (the router slow to answer, or not
     * answering, holds nothing up), and asked again in the background */
    t_now += DNSCACHE_FRESH_MS;
    hold = 1;
    uint32_t w0 = real_ms();
    for (int i = 0; i < 3; i++) {                    /* (a burst of connections) */
        r = NULL;
        CHECK(ask("stream.example.org", "443", &r) == 0 && r && port_of(r) == 443);
        dnscache_freeaddrinfo(r, fake_free);
    }
    CHECK(real_ms() - w0 < 1000 && frees == 1);      /* not waited for; ours, not the real free's */
    hold = 0;
    settle();
    CHECK(lookups == 2 && frees == 2);               /* asked again once, its result freed */
    r = NULL;
    CHECK(ask("stream.example.org", "443", &r) == 0 && lookups == 2);   /* fresh again */
    dnscache_freeaddrinfo(r, fake_free);
    t_now += DNSCACHE_FRESH_MS - 1;
    CHECK(ask("stream.example.org", "443", &r) == 0 && lookups == 2);   /* ...from when it was asked again */
    dnscache_freeaddrinfo(r, fake_free);
    settle();
    CHECK(lookups == 2);

    /* asked again with the hints it was asked with (they keep entries apart) */
    struct addrinfo h = HINTS;
    h.ai_protocol = IPPROTO_TCP;
    CHECK(dnscache_getaddrinfo("tls.example.org", "443", &h, &r, fake_resolve, fake_free) == 0 && lookups == 3);
    dnscache_freeaddrinfo(r, fake_free);
    t_now += DNSCACHE_FRESH_MS + 1;
    CHECK(dnscache_getaddrinfo("tls.example.org", "443", &h, &r, fake_resolve, fake_free) == 0);
    dnscache_freeaddrinfo(r, fake_free);
    settle();
    CHECK(lookups == 4);
    CHECK(dnscache_getaddrinfo("tls.example.org", "443", &h, &r, fake_resolve, fake_free) == 0 && lookups == 4);
    dnscache_freeaddrinfo(r, fake_free);
    settle();
}

static void test_stale(void)
{
    dnscache_test_reset();
    lookups = frees = fail_with = 0;
    struct addrinfo *r = NULL;
    CHECK(ask("radio.example.net", "443", &r) == 0);
    dnscache_freeaddrinfo(r, fake_free);
    t_now += DNSCACHE_FRESH_MS + 1000;
    fail_with = EAI_AGAIN;                           /* the router doesn't answer */
    for (int i = 0; i < 2; i++) {                    /* the old address goes on being given, and asked for behind */
        r = NULL;
        CHECK(ask("radio.example.net", "443", &r) == 0 && r && port_of(r) == 443);
        dnscache_freeaddrinfo(r, fake_free);
        settle();
        CHECK(lookups == 2 + i);
        t_now += 15000;
    }
    fail_with = EAI_NONAME;                          /* no such name any more: the old address once */
    r = NULL;
    CHECK(ask("radio.example.net", "443", &r) == 0 && r);
    dnscache_freeaddrinfo(r, fake_free);
    settle();
    CHECK(lookups == 4);
    r = NULL;                                        /* ...and not again */
    CHECK(ask("radio.example.net", "443", &r) == EAI_NONAME && !r && lookups == 5);
    fail_with = 0;
    CHECK(ask("radio.example.net", "443", &r) == 0 && lookups == 6);
    dnscache_freeaddrinfo(r, fake_free);
    t_now += DNSCACHE_STALE_MS;                      /* too old to connect with: asked first */
    fail_with = EAI_AGAIN;
    CHECK(ask("radio.example.net", "443", &r) == EAI_AGAIN && lookups == 7);
    fail_with = EAI_AGAIN;
    CHECK(ask("never.example.net", "443", &r) == EAI_AGAIN);   /* nothing kept: the failure */
    fail_with = 0;
}

static void test_passed_through(void)
{
    dnscache_test_reset();
    lookups = frees = 0;
    struct addrinfo *r = NULL;
    struct addrinfo h = HINTS;
    h.ai_flags = AI_NUMERICHOST;
    for (int i = 0; i < 2; i++) {                    /* each asked every time */
        CHECK(dnscache_getaddrinfo("a.example.org", "443", &h, &r, fake_resolve, fake_free) == 0);
        dnscache_freeaddrinfo(r, fake_free);
        CHECK(ask("a.example.org", "https", &r) == 0);   /* a service name, not a port */
        dnscache_freeaddrinfo(r, fake_free);
        CHECK(ask("192.168.1.10", "80", &r) == 0);
        dnscache_freeaddrinfo(r, fake_free);
    }
    CHECK(lookups == 6 && frees == 6);
    /* other hints: kept apart */
    h.ai_flags = 0;
    h.ai_family = AF_INET;
    CHECK(ask("b.example.org", "443", &r) == 0);
    dnscache_freeaddrinfo(r, fake_free);
    CHECK(dnscache_getaddrinfo("b.example.org", "443", &h, &r, fake_resolve, fake_free) == 0);
    dnscache_freeaddrinfo(r, fake_free);
    CHECK(lookups == 8);
}

/* Many results out at once (more than it records): all still right, all freed */
static void test_many_out(void)
{
    dnscache_test_reset();
    lookups = frees = 0;
    struct addrinfo *r[40];
    CHECK(ask("c.example.org", "443", &r[0]) == 0);
    for (int i = 1; i < 40; i++) {
        r[i] = NULL;
        CHECK(ask("c.example.org", "443", &r[i]) == 0 && r[i] && port_of(r[i]) == 443);
    }
    for (int i = 0; i < 40; i++) dnscache_freeaddrinfo(r[i], fake_free);
    CHECK(lookups == frees);                         /* the real ones by the real free */
    CHECK(lookups > 1 && lookups < 40);
    /* more names than it keeps: the oldest go, the newest stay */
    for (int i = 0; i < 40; i++) {
        char n[32];
        snprintf(n, sizeof n, "host%d.example.org", i);
        t_now++;
        CHECK(ask(n, "443", &r[0]) == 0);
        dnscache_freeaddrinfo(r[0], fake_free);
    }
    int before = lookups;
    CHECK(ask("host39.example.org", "443", &r[0]) == 0 && lookups == before);
    dnscache_freeaddrinfo(r[0], fake_free);
    CHECK(ask("host0.example.org", "443", &r[0]) == 0 && lookups == before + 1);
    dnscache_freeaddrinfo(r[0], fake_free);
}

static void test_prefetch(void)
{
    dnscache_test_reset();
    lookups = frees = fail_with = 0;
    dnscache_use(fake_resolve, fake_free);
    dnscache_prefetch_url("https://user@pre.example.org:8443/stream?x=1");
    dnscache_prefetch_url("http://10.1.2.3/stream");          /* an address: nothing to do */
    dnscache_prefetch_url("ftp://other.example.org/");        /* not ours */
    dnscache_prefetch_url("https://[2001:db8::1]/stream");
    settle();
    CHECK(lookups == 1 && !strcmp(last_node, "pre.example.org"));
    struct addrinfo *r = NULL;
    CHECK(ask("pre.example.org", "443", &r) == 0 && lookups == 1);   /* from the cache */
    dnscache_freeaddrinfo(r, fake_free);
    dnscache_prefetch_url("https://pre.example.org/again");   /* fresh: not asked again */
    usleep(100000);
    CHECK(lookups == 1);
}

/* A kernel without IPv6: a lookup for either family asks for IPv4 alone (musl
 * asks for both, and fails all of it when the IPv6 half isn't answered) */
static void test_ipv4_only(void)
{
    dnscache_test_reset();
    lookups = frees = fail_with = 0;
    dnscache_use(fake_resolve, fake_free);
    dnscache_ipv4_only(1);
    struct addrinfo *r = NULL;
    CHECK(ask("v.example.org", "443", &r) == 0 && lookups == 1 && last_family == AF_INET);
    dnscache_freeaddrinfo(r, fake_free);
    struct addrinfo h = HINTS;
    h.ai_family = AF_INET;                           /* the same lookup: the same entry */
    CHECK(dnscache_getaddrinfo("v.example.org", "443", &h, &r, fake_resolve, fake_free) == 0 && lookups == 1);
    dnscache_freeaddrinfo(r, fake_free);
    CHECK(dnscache_getaddrinfo("w.example.org", "443", NULL, &r, fake_resolve, fake_free) == 0);   /* no hints: either */
    CHECK(lookups == 2 && last_family == AF_INET);
    dnscache_freeaddrinfo(r, fake_free);
    CHECK(ask("w.example.org", "https", &r) == 0 && lookups == 3 && last_family == AF_INET);   /* not kept: still IPv4 */
    dnscache_freeaddrinfo(r, fake_free);
    h.ai_family = AF_INET6;                          /* asked for by name: left alone */
    CHECK(dnscache_getaddrinfo("v.example.org", "443", &h, &r, fake_resolve, fake_free) == 0);
    CHECK(lookups == 4 && last_family == AF_INET6);
    dnscache_freeaddrinfo(r, fake_free);
    dnscache_prefetch_url("https://ahead.example.org/stream");   /* the prefetch too, under the same entry */
    settle();
    CHECK(lookups == 5 && last_family == AF_INET && !strcmp(last_node, "ahead.example.org"));
    CHECK(ask("ahead.example.org", "443", &r) == 0 && lookups == 5);
    dnscache_freeaddrinfo(r, fake_free);
    t_now += DNSCACHE_FRESH_MS + 1;                  /* and the look again behind an old entry */
    CHECK(ask("ahead.example.org", "443", &r) == 0);
    dnscache_freeaddrinfo(r, fake_free);
    settle();
    CHECK(lookups == 6 && last_family == AF_INET);

    dnscache_ipv4_only(0);                           /* off (anywhere else): as asked */
    CHECK(ask("u.example.org", "443", &r) == 0 && last_family == AF_UNSPEC);
    dnscache_freeaddrinfo(r, fake_free);
    CHECK(dnscache_getaddrinfo("u.example.org", "80", NULL, &r, fake_resolve, fake_free) == 0 && last_family == -1);
    dnscache_freeaddrinfo(r, fake_free);
}

int main(void)
{
    dnscache_test_clock(fake_clock);
    test_kept();
    test_stale();
    test_passed_through();
    test_many_out();
    test_prefetch();
    test_ipv4_only();
    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    printf("dnscache: ok\n");
    return 0;
}
