/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/*
 * The clock and HTTPS (netclock.c). The H2 has no clock that runs while it's
 * off: a fresh install starts in 1970 until NTP answers (the udhcpc hook
 * fathom-ntp). Every HTTPS certificate is checked against the date, so until
 * then the radio directory, covers and updates all fail. These say so in
 * plain words, and, where NTP doesn't answer, take the time from an HTTPS
 * server's answer instead.
 */
#ifndef FATHOM_NETCLOCK_H
#define FATHOM_NETCLOCK_H

#include <stddef.h>
#include <stdint.h>

/* Before this (2025-01-01 00:00 UTC) the clock can't be right: never set. */
#define NETCLOCK_FLOOR 1735689600LL

/* The words for a secure connection that failed while the clock isn't set
 * (in English: tr() them where they're shown). */
const char *netclock_unset_text(void);

/* 1 while the clock is before NETCLOCK_FLOOR (t: a time, or the clock now). */
int netclock_unset_at(int64_t t);
int netclock_unset(void);

/* What to say about an HTTPS request that failed: netclock_unset_text() while
 * the clock isn't set (the likely reason), else otherwise. While it isn't set,
 * it also asks for the time now (netclock_want). */
const char *netclock_why(const char *otherwise);

/* A secure connection failed because the clock isn't set: the player asks an
 * HTTPS server for the time at once (device/main.c), rather than after NTP's
 * next try, minutes away. Any thread. netclock_wanted(): whether it was asked
 * for since the last call (and clears it). */
void netclock_want(void);
int netclock_wanted(void);

/* Getting the time before a secure connection, so it doesn't fail for want
 * of it. The player sets how (netclock_set_fetcher: blocking, 0 once it has
 * set the clock). netclock_ensure(): while the clock is unset, fetch it first;
 * one fetch at a time (others wait for it, then go on), and after one that
 * failed none for 30 s (no internet: each request isn't held up in turn).
 * 0 when the clock is set. Any thread but the UI's: it can take seconds.
 * netclock_fetch(): fetch now whether or not it looks set (NTP not answering
 * for a while), in turn with the others. */
void netclock_set_fetcher(int (*fetch)(void));
int netclock_ensure(void);
int netclock_fetch(void);

/* Tests: the clock netclock_unset() reads (NULL: time()). */
void netclock_set_now(int64_t (*now)(void));

/* The time in the Date header of an HTTP response's head (from the status
 * line to the blank line; anything after it is ignored), as seconds since
 * 1970: only the form HTTP servers send ("Sun, 06 Nov 1994 08:49:37 GMT").
 * -1 if there's none, or it doesn't read as one. */
int64_t netclock_parse_date(const char *head, size_t n);

/* Whether a time an HTTPS server gave should be taken: later than now less
 * back seconds (0: only later; NETCLOCK_HAND_BACK after the clock was set by
 * hand, which may be a little ahead), and not before NETCLOCK_FLOOR. */
#define NETCLOCK_HAND_BACK 86400
int netclock_take(int64_t got, int64_t now, int64_t back);

/* The time from one HTTPS answer of host:port (a HEAD request), checked
 * against ca_file (a PEM bundle) for its signature and host name but not its
 * dates (netclock.c says why). 0 and *out set, or -1. Blocking (about 10 s at
 * most for each step): call from a worker thread. Only where the player links
 * mbedTLS (FATHOM_HAVE_MBEDTLS); elsewhere it always fails. */
int netclock_https_date(const char *host, int port, const char *ca_file, int64_t *out);

#endif
