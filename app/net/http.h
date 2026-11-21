/* SPDX-License-Identifier: GPL-3.0-or-later */
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

/* The body of url into buf (NUL-terminated, at most cap - 1 bytes): its length,
 * or -1 (no network, an HTTP error, a certificate that doesn't check out, or
 * *abort set meanwhile; abort may be NULL). A body longer than cap - 1 is cut
 * there and its full length isn't known: -2. -3: the server says it isn't
 * there (HTTP 4xx), so trying again won't help. */
long http_get(const char *url, char *buf, size_t cap, const volatile int *abort);

/* The body of url saved to path (written to path.part, renamed when complete):
 * its size, or -1 (-3: the server says it isn't there, as for http_get). progress (may be NULL) is called now and then with the bytes
 * so far and the total (0 if the server didn't say). */
typedef void (*http_progress_fn)(uint64_t done, uint64_t total, void *ctx);
int64_t http_download(const char *url, const char *path, http_progress_fn progress, void *ctx,
                      const volatile int *abort);

#endif
