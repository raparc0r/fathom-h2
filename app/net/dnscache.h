/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* Names looked up lately, kept for a while (musl keeps none, and every
 * connection asks the router again: 30 ms to a second on the H2's Wi-Fi, and a
 * router that refuses now and then costs musl its whole 5 s timeout). In the
 * player, getaddrinfo() and freeaddrinfo() go through these (--wrap), so
 * FFmpeg's connections (radio, podcasts, covers, the radio directory) use it. */
#ifndef FATHOM_DNSCACHE_H
#define FATHOM_DNSCACHE_H

#include <netdb.h>
#include <stdint.h>

typedef int (*dns_resolve_fn)(const char *node, const char *service, const struct addrinfo *hints,
                              struct addrinfo **res);
typedef void (*dns_free_fn)(struct addrinfo *ai);

/* A result younger than this is given without asking (ms) */
#define DNSCACHE_FRESH_MS 120000
/* An older one (up to this) is given at once too, and looked up again in the
 * background for the next time: the name is still the station's, and a
 * reconnect doesn't wait for the router. Past this it's asked for first. */
#define DNSCACHE_STALE_MS 3600000

/* getaddrinfo() through the cache: resolve does the real lookup. A name with a
 * numeric port and plain hints (no AI_PASSIVE, AI_NUMERICHOST, AI_CANONNAME) is
 * cached; anything else goes straight to resolve. The result is freed with
 * dnscache_freeaddrinfo(). */
int dnscache_getaddrinfo(const char *node, const char *service, const struct addrinfo *hints,
                         struct addrinfo **res, dns_resolve_fn resolve, dns_free_fn real_free);
/* freeaddrinfo(): ours freed here, anything else by real_free */
void dnscache_freeaddrinfo(struct addrinfo *ai, dns_free_fn real_free);

/* Looks up the host of a URL (http, https) in the background, so a connection
 * to it soon doesn't wait for DNS: a station about to be chosen. At most a few
 * at once; the rest dropped. */
void dnscache_prefetch_url(const char *url);

/* The resolver the prefetch uses (the player: libc's own, under the wrap) */
void dnscache_use(dns_resolve_fn resolve, dns_free_fn real_free);

/* On: a lookup for either family (AF_UNSPEC, or no hints) asks for IPv4 only.
 * For a kernel without IPv6 (the H2's): musl asks for both addresses and fails
 * the whole lookup after 5 s when the IPv6 half isn't answered. The player
 * turns it on (with the wrap). */
void dnscache_ipv4_only(int on);

/* tests: the clock (ms) instead of CLOCK_MONOTONIC (NULL: the real one), and
 * everything forgotten */
void dnscache_test_clock(uint32_t (*now)(void));
void dnscache_test_reset(void);
int  dnscache_test_idle(void);   /* tests: nothing is being looked up in the background */

#endif
