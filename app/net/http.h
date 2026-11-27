/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/*
 * Fetching over HTTP(S) for things that aren't audio (the radio directory,
 * release updates): through FFmpeg's protocols, with the same certificate
 * checks as the radio (ff_set_ca_file), a timeout, and redirects followed.
 * Blocking: call from a worker thread, never the UI thread. Without FFmpeg
 * (the desktop build) every call fails with -1.
 */
#ifndef FATHOM_HTTP_H
#define FATHOM_HTTP_H

#include <stddef.h>
#include <stdint.h>

/* Requests say they're from this version of Fathom (its User-Agent). */
void http_set_version(const char *version);

/* How long a fetch may take. An answer (http_get: a feed, a search, a small
 * file) is given up HTTP_GET_MS after it was asked for, a download once it
 * has averaged under HTTP_SLOW_BPS bytes a second over the last HTTP_SLOW_MS:
 * both as a server not reached (-1). Each read had its 15 s and nothing
 * more: a server sending 1 KB/s held its worker for 25 minutes a feed. */
#define HTTP_GET_MS   60000
#define HTTP_SLOW_MS  60000
#define HTTP_SLOW_BPS 2048

/* The body of url into buf (NUL-terminated, at most cap - 1 bytes): its length,
 * or -1 (no network, an HTTP error, a certificate that doesn't check out, a
 * body that stopped short of the length the server gave, one not here whole
 * after HTTP_GET_MS, or *abort set meanwhile; abort may be NULL). A body
 * longer than cap - 1 is cut there and its full length isn't known: -2. -3:
 * the server says it isn't there (HTTP 4xx), so trying again won't help. */
long http_get(const char *url, char *buf, size_t cap, const volatile int *abort);
/* What http_get returns for a body read: n bytes of total (the length the
 * server gave; 0 or less: none given), cut (the buffer filled and there was
 * more), failed (the read ended in an error), aborted. Apart for the tests. */
long http_get_result(size_t n, int64_t total, int cut, int failed, int aborted);
/* A download's speed, looked at as it goes (done: its bytes so far; now_ms:
 * a clock that only goes forward): 1 once it's too slow to carry on. w: from
 * where it's counted, {the time it started, 0} before the first call. Apart
 * for the tests. */
typedef struct { int64_t since_ms; uint64_t since_done; } http_slow_t;
int http_too_slow(http_slow_t *w, uint64_t done, int64_t now_ms);

/* The body of url saved to path (written to path.part, renamed when complete):
 * its size, or -1 (-3: the server says it isn't there, as for http_get), or
 * what stopped it at the card's end, where trying again won't help either:
 * HTTP_ERR_FULL (the card would be left with too little free) or
 * HTTP_ERR_WRITE (it couldn't be written: the .part not made, a write or the
 * rename failed). A connection that goes partway is taken up again where it
 * stopped, when the server allows (http.c); one too slow (http_too_slow) is
 * given up: -1. progress (may be NULL) is called
 * now and then with the bytes so far and the total (0 if the server didn't
 * say). */
#define HTTP_ERR_FULL  (-4)
#define HTTP_ERR_WRITE (-5)
typedef void (*http_progress_fn)(uint64_t done, uint64_t total, void *ctx);
int64_t http_download(const char *url, const char *path, http_progress_fn progress, void *ctx,
                      const volatile int *abort);

/* The same, tried up to 3 times, 2 s and then 5 s apart, while they fail for
 * want of the network (-1): a name lookup fails now and then (a home router
 * slow to answer for a name it hasn't looked up lately: "Try again"), and a
 * download goes through several hosts' redirects. Not again after -2 or -3
 * (the server answered) or HTTP_ERR_FULL, HTTP_ERR_WRITE (the card), nor once
 * *abort is set (the wait ends at once). */
long http_get_retry(const char *url, char *buf, size_t cap, const volatile int *abort);
int64_t http_download_retry(const char *url, const char *path, http_progress_fn progress, void *ctx,
                            const volatile int *abort);

/* tests: what one try is (NULL: http_get, http_download) and the two waits (ms) */
typedef long (*http_get_fn)(const char *url, char *buf, size_t cap, const volatile int *abort);
typedef int64_t (*http_download_fn)(const char *url, const char *path, http_progress_fn progress, void *ctx,
                                    const volatile int *abort);
void http_test_tries(http_get_fn get, http_download_fn download, int wait1_ms, int wait2_ms);

#endif
