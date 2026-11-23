/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* The clock and HTTPS (netclock.h). */
#include "net/netclock.h"

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "core/datetime.h"
#include "core/lang.h"

int netclock_unset_at(int64_t t) { return t < NETCLOCK_FLOOR; }

static int64_t (*g_now)(void);

void netclock_set_now(int64_t (*now)(void)) { g_now = now; }

int netclock_unset(void) { return netclock_unset_at(g_now ? g_now() : (int64_t)time(NULL)); }

static volatile int g_confirmed;

void netclock_set_confirmed(int yes) { g_confirmed = yes; }
int netclock_confirmed(void) { return g_confirmed; }
int netclock_doubtful(void) { return netclock_unset() || !g_confirmed; }

static pthread_mutex_t g_fetching = PTHREAD_MUTEX_INITIALIZER;
static int (*g_fetch)(void);
static int64_t g_failed_at;              /* (monotonic s: settimeofday doesn't move it) */

void netclock_set_fetcher(int (*fetch)(void)) { g_fetch = fetch; }

static int64_t mono_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec;
}

static int fetch_locked(void)
{
    int r = g_fetch ? g_fetch() : -1;
    g_failed_at = r ? (mono_s() | 1) : 0;
    return r;
}

int netclock_fetch(void)
{
    pthread_mutex_lock(&g_fetching);
    int r = fetch_locked();
    pthread_mutex_unlock(&g_fetching);
    return r;
}

int netclock_ensure(void)
{
    if (!netclock_unset()) return 0;
    if (!g_fetch) return -1;
    pthread_mutex_lock(&g_fetching);
    /* (set meanwhile by the fetch another request waited for: nothing to do) */
    if (netclock_unset() && !(g_failed_at && mono_s() - g_failed_at < 30)) fetch_locked();
    pthread_mutex_unlock(&g_fetching);
    return netclock_unset() ? -1 : 0;
}

const char *netclock_unset_text(void)
{
    return N_("The clock isn't set yet: secure connections can't be checked");
}

static volatile int g_want;

void netclock_want(void) { g_want = 1; }

int netclock_wanted(void)
{
    int w = g_want;
    g_want = 0;
    return w;
}

const char *netclock_why(const char *otherwise)
{
    if (netclock_doubtful()) netclock_want();   /* (the time, now) */
    return netclock_unset() ? netclock_unset_text() : otherwise;
}

void netclock_net_failed(const char *url)
{
    if (!url || (strncasecmp(url, "https://", 8) && strncasecmp(url, "http://", 7))) return;
    if (netclock_doubtful()) netclock_want();
}

int netclock_take(int64_t got, int64_t now, int64_t back)
{
    return got > now - back && !netclock_unset_at(got);
}

static int digits(const char *s, int n, int *v)
{
    *v = 0;
    for (int i = 0; i < n; i++) {
        if (!isdigit((unsigned char)s[i])) return 0;
        *v = *v * 10 + (s[i] - '0');
    }
    return 1;
}

/* "Sun, 06 Nov 1994 08:49:37 GMT" (RFC 9110's IMF-fixdate), and nothing else */
static int64_t fixdate(const char *s, size_t n)
{
    static const char days[] = "MonTueWedThuFriSatSun", months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    if (n < 29 || s[3] != ',' || s[4] != ' ' || s[7] != ' ' || s[11] != ' ' || s[16] != ' ' ||
        s[19] != ':' || s[22] != ':' || memcmp(s + 25, " GMT", 4) || (n > 29 && s[29] != '\r' && s[29] != '\n' &&
                                                                    s[29] != ' ' && s[29] != '\t'))
        return -1;
    int wd = -1, mon = -1;
    for (int i = 0; i < 7; i++)
        if (!memcmp(s, days + 3 * i, 3)) wd = i;
    for (int i = 0; i < 12; i++)
        if (!memcmp(s + 8, months + 3 * i, 3)) mon = i + 1;
    int d, y, hh, mm, ss;
    if (wd < 0 || mon < 0 || !digits(s + 5, 2, &d) || !digits(s + 12, 4, &y) || !digits(s + 17, 2, &hh) ||
        !digits(s + 20, 2, &mm) || !digits(s + 23, 2, &ss))
        return -1;
    if (d < 1 || d > dt_days_in_month(y, mon) || hh > 23 || mm > 59 || ss > 60) return -1;
    int64_t days_n = dt_days_from_civil(y, mon, d);
    if ((days_n + 3) % 7 != wd) return -1;        /* (1970-01-01 was a Thursday) */
    return days_n * 86400 + hh * 3600 + mm * 60 + (ss == 60 ? 59 : ss);
}

int64_t netclock_parse_date(const char *head, size_t n)
{
    if (!head || n < 5 || memcmp(head, "HTTP/", 5)) return -1;
    const char *p = head, *end = head + n;
    /* (each line after the status line, to the first empty one) */
    for (;;) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        if (!nl) return -1;
        p = nl + 1;
        if (p >= end || *p == '\r' || *p == '\n') return -1;   /* the head's end: no Date */
        const char *eol = memchr(p, '\n', (size_t)(end - p));
        size_t len = (size_t)((eol ? eol : end) - p);
        if (len > 5 && !strncasecmp(p, "date:", 5)) {
            const char *v = p + 5;
            while (v < p + len && (*v == ' ' || *v == '\t')) v++;
            return fixdate(v, (size_t)(p + len - v));
        }
    }
}

#ifdef FATHOM_HAVE_MBEDTLS
/*
 * Why the certificate's dates are left unchecked, for this request only.
 *
 * With the clock in 1970 every certificate is "not valid yet", so a normal
 * HTTPS request can't be made: that's the problem being solved. A request
 * that trusts no certificate at all would let anyone on the network (a hotel's
 * Wi-Fi, a captive portal) give the H2 any time. So everything else is checked
 * as usual: the chain must lead to an authority in the bundle, every signature
 * must hold, and the certificate must be for the host asked (api.github.com).
 * What's given up is the check that the certificate is in date: someone who
 * holds the private key of an old certificate for that host, one that has
 * expired since, could still answer. That is rare (it needs a leaked key, not
 * just a network), and what they could do with it is limited: the time is
 * taken only from this answer's Date header, only if it's later than the
 * clock already is and not before 2025 (netclock_take), so the clock can only
 * be moved forward, never back to when some other expired certificate was
 * valid. (After the clock was set by hand, a day back is allowed too: the
 * answer may correct a time set a little ahead, but no more than that.) A clock moved too far forward makes certificates look expired (HTTPS
 * fails, as it did before) until NTP answers and sets it right.
 */
#include <errno.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#if defined(MBEDTLS_PSA_CRYPTO_C)
#include <psa/crypto.h>
#endif

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#define STEP_S 10              /* the connection, and each read or write */

/* Each certificate of the chain: its dates not held against it (only those) */
static int ignore_dates(void *ctx, mbedtls_x509_crt *crt, int depth, uint32_t *flags)
{
    (void)ctx; (void)crt; (void)depth;
    *flags &= ~(uint32_t)(MBEDTLS_X509_BADCERT_EXPIRED | MBEDTLS_X509_BADCERT_FUTURE);
    return 0;
}

static int bio_send(void *ctx, const unsigned char *b, size_t n)
{
    ssize_t k = send(*(int *)ctx, b, n, MSG_NOSIGNAL);
    if (k >= 0) return (int)k;
    return errno == EINTR ? MBEDTLS_ERR_SSL_WANT_WRITE : MBEDTLS_ERR_NET_SEND_FAILED;
}

static int bio_recv(void *ctx, unsigned char *b, size_t n)
{
    ssize_t k = recv(*(int *)ctx, b, n, 0);
    if (k >= 0) return (int)k;
    if (errno == EINTR) return MBEDTLS_ERR_SSL_WANT_READ;
    return errno == EAGAIN || errno == EWOULDBLOCK ? MBEDTLS_ERR_SSL_TIMEOUT : MBEDTLS_ERR_NET_RECV_FAILED;
}

static int dial(const char *host, int port)
{
    char ps[8];
    snprintf(ps, sizeof ps, "%d", port);
    struct addrinfo hints = {0}, *ai = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, ps, &hints, &ai) || !ai) return -1;
    int fd = -1;
    for (struct addrinfo *a = ai; a && fd < 0; a = a->ai_next) {
        fd = socket(a->ai_family, a->ai_socktype | SOCK_CLOEXEC, a->ai_protocol);
        if (fd < 0) continue;
        struct timeval tv = {STEP_S, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);   /* (connect too, on Linux) */
        if (connect(fd, a->ai_addr, a->ai_addrlen)) { close(fd); fd = -1; }
    }
    freeaddrinfo(ai);
    return fd;
}

int netclock_https_date(const char *host, int port, const char *ca_file, int64_t *out)
{
    if (!host || !ca_file || !out) return -1;
    int ret = -1, fd = -1, r;
    mbedtls_x509_crt ca;
    mbedtls_ssl_config conf;
    mbedtls_ssl_context ssl;
    mbedtls_entropy_context ent;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_x509_crt_init(&ca);
    mbedtls_ssl_config_init(&conf);
    mbedtls_ssl_init(&ssl);
    mbedtls_entropy_init(&ent);
    mbedtls_ctr_drbg_init(&drbg);
#if defined(MBEDTLS_PSA_CRYPTO_C)
    psa_crypto_init();
#endif
    static const char pers[] = "fathom-netclock";
    char head[4096];
    size_t n = 0;
    /* (on the H2 the bundle is the one FFmpeg's connections share: tls_wrap.c) */
    if (mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &ent, (const unsigned char *)pers, sizeof pers - 1) ||
        mbedtls_x509_crt_parse_file(&ca, ca_file) ||
        mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT))
        goto out;
    mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&conf, &ca, NULL);
    mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &drbg);
    mbedtls_ssl_conf_verify(&conf, ignore_dates, NULL);
    if (mbedtls_ssl_setup(&ssl, &conf) || mbedtls_ssl_set_hostname(&ssl, host)) goto out;
    if ((fd = dial(host, port)) < 0) goto out;
    mbedtls_ssl_set_bio(&ssl, &fd, bio_send, bio_recv, NULL);
    while ((r = mbedtls_ssl_handshake(&ssl)) != 0)
        if (r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE) {
            fprintf(stderr, "clock: %s: no secure connection (-0x%04x, flags 0x%x)\n", host, (unsigned)-r,
                    (unsigned)mbedtls_ssl_get_verify_result(&ssl));
            goto out;
        }
    if (mbedtls_ssl_get_verify_result(&ssl)) goto out;   /* (the handshake fails first; belt and braces) */
    char req[256];
    int len = snprintf(req, sizeof req,
                       "HEAD / HTTP/1.1\r\nHost: %s\r\nUser-Agent: Fathom (HiFi Walker H2)\r\nConnection: close\r\n\r\n",
                       host);
    for (int sent = 0; sent < len;) {
        r = mbedtls_ssl_write(&ssl, (const unsigned char *)req + sent, (size_t)(len - sent));
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (r <= 0) goto out;
        sent += r;
    }
    /* The head: up to its blank line (the Date is read from it alone) */
    while (n < sizeof head - 1) {
        r = mbedtls_ssl_read(&ssl, (unsigned char *)head + n, sizeof head - 1 - n);
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
#ifdef MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET
        if (r == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) continue;   /* (TLS 1.3) */
#endif
        if (r <= 0) break;
        n += (size_t)r;
        head[n] = '\0';
        if (strstr(head, "\r\n\r\n")) break;
    }
    head[n] = '\0';
    int64_t t = netclock_parse_date(head, n);
    if (t < 0) {
        fprintf(stderr, "clock: %s: no date in its answer\n", host);
        goto out;
    }
    *out = t;
    ret = 0;
    mbedtls_ssl_close_notify(&ssl);
out:
    if (fd >= 0) close(fd);
    mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&conf);
    mbedtls_x509_crt_free(&ca);
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&ent);
    return ret;
}

#else   /* no mbedTLS (the desktop build): no network */

int netclock_https_date(const char *host, int port, const char *ca_file, int64_t *out)
{
    (void)host; (void)port; (void)ca_file; (void)out;
    return -1;
}

#endif
