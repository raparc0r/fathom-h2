/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/*
 * The clock and HTTPS (netclock.c). The H2 has no clock that runs while it's
 * off: a fresh install starts in 1970 until NTP answers (the udhcpc hook
 * fathom-ntp). Every HTTPS certificate is checked against the date, so until
 * then the radio directory, covers and updates all fail. These say so in
 * plain words, and, where NTP doesn't answer, take the time from an HTTPS
 * server's answer instead.
 *
 * Later starts begin at the last shutdown's time (the player saves it): right
 * enough to look set, and days behind after days off, so a server whose
 * certificate is newer than that fails ("not valid yet") until the network
 * gives the time. Until it has, since the player started, the clock isn't
 * confirmed, and a connection that fails asks for the time as well.
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

/* The network gave the time since the player started (NTP, or an HTTPS
 * answer, whether it moved the clock or found it right): 1. Set by hand: 0
 * again. netclock_doubtful(): unset, or not confirmed. */
void netclock_set_confirmed(int yes);
int netclock_confirmed(void);
int netclock_doubtful(void);

/* What to say about an HTTPS request that failed: netclock_unset_text() while
 * the clock isn't set (the likely reason), else otherwise. While the clock is
 * doubtful, it also asks for the time now (netclock_want). */
const char *netclock_why(const char *otherwise);

/* A connection to url (http, https: a plain one may lead to a secure one)
 * couldn't be opened. While the clock is doubtful that may be why, so the
 * time is asked for (netclock_want): the next try finds it set. Any thread. */
/* (weak: the small tools that play or fetch without a clock to mind, the
 * bit-perfect check and the levelling bench, are linked without this file;
 * callers ask whether it's there) */
void netclock_net_failed(const char *url) __attribute__((weak));

/* A secure connection failed, perhaps for want of the time: the player asks
 * an HTTPS server for it at once (device/main.c), rather than after NTP's
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
