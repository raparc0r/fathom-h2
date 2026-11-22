/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/*
 * The certificate bundle (ff_set_ca_file) parsed once and lent to every HTTPS
 * connection, not parsed again for each (about 400 KB and a CPU spike each):
 * tls_wrap.c. Linked in with
 *   -Wl,--wrap=mbedtls_x509_crt_parse_file -Wl,--wrap=mbedtls_x509_crt_free
 * so FFmpeg's tls_mbedtls (a static library we don't patch) calls it instead.
 * It works by being linked; tls_wrap_warm() parses ahead of a connection.
 */
#ifndef FATHOM_TLS_WRAP_H
#define FATHOM_TLS_WRAP_H

typedef struct {
    unsigned parses;    /* times the bundle was parsed */
    unsigned chains;    /* parsed chains held (the current one, and older ones still lent) */
    unsigned lent;      /* connections holding one */
} tls_wrap_stats_t;

void tls_wrap_stats(tls_wrap_stats_t *s);
/* The bundle parsed ahead, in the background (a connection is likely soon) */
void tls_wrap_warm(void);
/* How long an unused chain is kept for the next connection (default 30 s). */
void tls_wrap_set_grace_ms(unsigned ms);

#endif
