/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* app/net/netclock.c: the Date header read, the year check, and (with mbedTLS)
 * the time from one HTTPS answer, its certificate's dates not checked but all
 * else: against a server here on 127.0.0.1, with certificates made here. */
#define _DEFAULT_SOURCE   /* (usleep) */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "net/netclock.h"

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static int64_t date_of(const char *head) { return netclock_parse_date(head, strlen(head)); }

/* The time for a calendar date, by the C library (to check against) */
static int64_t utc(int y, int mo, int d, int h, int mi, int s)
{
    struct tm tm = {0};
    tm.tm_year = y - 1900;
    tm.tm_mon = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min = mi;
    tm.tm_sec = s;
    return (int64_t)timegm(&tm);
}

static void parse_tests(void)
{
    CHECK(date_of("HTTP/1.1 200 OK\r\nServer: x\r\nDate: Sun, 06 Nov 1994 08:49:37 GMT\r\n\r\n") == 784111777);
    CHECK(date_of("HTTP/1.1 200 OK\r\nDate: Sun, 22 Nov 2026 18:05:09 GMT\r\n\r\n") == utc(2026, 11, 22, 18, 5, 9));
    CHECK(date_of("HTTP/2 404 Not Found\r\ndate:Thu, 01 Jan 1970 00:00:00 GMT\r\n\r\n") == 0);
    CHECK(date_of("HTTP/1.1 200 OK\nDATE: \tWed, 01 Jan 2025 00:00:00 GMT\n\n") == NETCLOCK_FLOOR);
    CHECK(date_of("HTTP/1.1 200 OK\r\nDate: Tue, 29 Feb 2028 12:00:00 GMT\r\n\r\n") == utc(2028, 2, 29, 12, 0, 0));
    CHECK(date_of("HTTP/1.1 200 OK\r\nDate: Tue, 29 Feb 2000 00:00:00 GMT\r\n\r\n") == utc(2000, 2, 29, 0, 0, 0));
    CHECK(date_of("HTTP/1.1 200 OK\r\nDate: Thu, 31 Dec 2099 23:59:60 GMT\r\n\r\n") == utc(2099, 12, 31, 23, 59, 59));
    /* the first Date of the head, other headers around it */
    CHECK(date_of("HTTP/1.1 200 OK\r\nLast-Modified: Mon, 01 Jan 2024 00:00:00 GMT\r\nX-Date: x\r\n"
                  "Date: Sun, 22 Nov 2026 18:05:09 GMT\r\nContent-Length: 0\r\n\r\n") == utc(2026, 11, 22, 18, 5, 9));
    /* a head cut short still has its Date */
    CHECK(date_of("HTTP/1.1 200 OK\r\nDate: Sun, 22 Nov 2026 18:05:09 GMT") == utc(2026, 11, 22, 18, 5, 9));

    /* none, or not in the head */
    CHECK(date_of("HTTP/1.1 200 OK\r\nServer: x\r\n\r\n") == -1);
    CHECK(date_of("HTTP/1.1 200 OK\r\nServer: x\r\n\r\nDate: Sun, 22 Nov 2026 18:05:09 GMT\r\n") == -1);
    CHECK(date_of("HTTP/1.1 200 OK\r\nX-Date: Sun, 22 Nov 2026 18:05:09 GMT\r\n\r\n") == -1);
    CHECK(date_of("Date: Sun, 22 Nov 2026 18:05:09 GMT\r\n\r\n") == -1);              /* no status line */
    CHECK(date_of("HTTP/1.1 200 OK\r\nDate: Sun, 22 Nov 2026 18:05\r\n\r\n") == -1);  /* cut short */
    CHECK(date_of("") == -1);
    CHECK(netclock_parse_date(NULL, 0) == -1);
    /* only the form servers send */
    CHECK(date_of("HTTP/1.1 200 OK\r\nDate: Sunday, 22-Nov-26 18:05:09 GMT\r\n\r\n") == -1);    /* RFC 850 */
    CHECK(date_of("HTTP/1.1 200 OK\r\nDate: Sun Nov 22 18:05:09 2026\r\n\r\n") == -1);          /* asctime */
    CHECK(date_of("HTTP/1.1 200 OK\r\nDate: Sun, 22 Nov 2026 18:05:09 UTC\r\n\r\n") == -1);
    CHECK(date_of("HTTP/1.1 200 OK\r\nDate: Sun, 22 Nov 2026 18:05:09 GMT+5\r\n\r\n") == -1);
    CHECK(date_of("HTTP/1.1 200 OK\r\nDate: Sun, 2 Nov 2026 18:05:09 GMT\r\n\r\n") == -1);
    /* not a real date */
    CHECK(date_of("HTTP/1.1 200 OK\r\nDate: Mon, 22 Nov 2026 18:05:09 GMT\r\n\r\n") == -1);   /* a Sunday */
    CHECK(date_of("HTTP/1.1 200 OK\r\nDate: Sun, 29 Feb 2026 00:00:00 GMT\r\n\r\n") == -1);
    CHECK(date_of("HTTP/1.1 200 OK\r\nDate: Thu, 31 Apr 2026 00:00:00 GMT\r\n\r\n") == -1);
    CHECK(date_of("HTTP/1.1 200 OK\r\nDate: Sun, 22 Nov 2026 24:00:00 GMT\r\n\r\n") == -1);
    CHECK(date_of("HTTP/1.1 200 OK\r\nDate: Sun, 22 Nox 2026 18:05:09 GMT\r\n\r\n") == -1);
    CHECK(date_of("HTTP/1.1 200 OK\r\nDate: Sun, 22 Nov 2O26 18:05:09 GMT\r\n\r\n") == -1);
}

static void year_tests(void)
{
    CHECK(netclock_unset_at(0));
    CHECK(netclock_unset_at(NETCLOCK_FLOOR - 1));
    CHECK(!netclock_unset_at(NETCLOCK_FLOOR));
    CHECK(NETCLOCK_FLOOR == utc(2025, 1, 1, 0, 0, 0));
    CHECK(!netclock_unset());                          /* (this computer's clock is right) */
    CHECK(!strcmp(netclock_why("Couldn't reach GitHub"), "Couldn't reach GitHub"));
    CHECK(strstr(netclock_unset_text(), "clock"));
    /* taken: later than now, and not before 2025 */
    int64_t nov = utc(2026, 11, 22, 18, 0, 0);
    CHECK(netclock_take(nov, 0, 0));
    CHECK(netclock_take(nov, nov - 1, 0));
    CHECK(!netclock_take(nov, nov, 0));
    CHECK(!netclock_take(nov - 3600, nov, 0));            /* never back */
    CHECK(!netclock_take(utc(2024, 6, 1, 0, 0, 0), 0, 0)); /* a time before 2025: not right either */
    CHECK(!netclock_take(-1, 0, 0));
    /* after the clock was set by hand: back a day at most */
    CHECK(netclock_take(nov - 3600, nov, NETCLOCK_HAND_BACK));
    CHECK(netclock_take(nov - 86399, nov, NETCLOCK_HAND_BACK));
    CHECK(!netclock_take(nov - 86400, nov, NETCLOCK_HAND_BACK));
    CHECK(!netclock_take(utc(2024, 12, 31, 23, 0, 0), utc(2025, 1, 1, 0, 0, 0), NETCLOCK_HAND_BACK));
}

#ifdef FATHOM_HAVE_MBEDTLS
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/ecp.h>
#include <mbedtls/entropy.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#if defined(MBEDTLS_PSA_CRYPTO_C)
#include <psa/crypto.h>
#endif

#include "tmpdir.h"

/* ---- certificates, made here (no keys kept anywhere) ---- */
static mbedtls_entropy_context g_ent;
static mbedtls_ctr_drbg_context g_drbg;
static mbedtls_pk_context g_ca_key, g_stranger_key, g_leaf_key;
static char g_ca_file[PATH_MAX + 64];                 /* the bundle the client trusts: Clock CA */
enum { PEM_MAX = 2048 };
static char g_expired[PEM_MAX], g_future[PEM_MAX], g_other[PEM_MAX], g_stranger[PEM_MAX];

static void need(int ok, const char *what)
{
    if (!ok) { fprintf(stderr, "test setup: %s\n", what); exit(1); }
}

static void make_key(mbedtls_pk_context *k)
{
    mbedtls_pk_init(k);
    need(!mbedtls_pk_setup(k, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) &&
         !mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(*k), mbedtls_ctr_drbg_random, &g_drbg), "a key");
}

/* subject's certificate (key sk), signed by issuer (key ik), valid from-to, as PEM */
static void make_cert(char *out, const char *subject, mbedtls_pk_context *sk, const char *issuer,
                      mbedtls_pk_context *ik, int ca, const char *from, const char *to)
{
    static unsigned char serial = 1;
    mbedtls_x509write_cert w;
    mbedtls_x509write_crt_init(&w);
    mbedtls_x509write_crt_set_version(&w, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&w, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&w, sk);
    mbedtls_x509write_crt_set_issuer_key(&w, ik);
    unsigned char sn[1] = {serial++};
    need(!mbedtls_x509write_crt_set_subject_name(&w, subject) && !mbedtls_x509write_crt_set_issuer_name(&w, issuer) &&
         !mbedtls_x509write_crt_set_serial_raw(&w, sn, 1) && !mbedtls_x509write_crt_set_validity(&w, from, to) &&
         !mbedtls_x509write_crt_set_basic_constraints(&w, ca, -1) &&
         !mbedtls_x509write_crt_set_key_usage(&w, ca ? MBEDTLS_X509_KU_KEY_CERT_SIGN | MBEDTLS_X509_KU_CRL_SIGN
                                                     : MBEDTLS_X509_KU_DIGITAL_SIGNATURE) &&
         !mbedtls_x509write_crt_pem(&w, (unsigned char *)out, PEM_MAX, mbedtls_ctr_drbg_random, &g_drbg),
         "a certificate");
    mbedtls_x509write_crt_free(&w);
}

static void make_certs(void)
{
    mbedtls_entropy_init(&g_ent);
    mbedtls_ctr_drbg_init(&g_drbg);
    need(!mbedtls_ctr_drbg_seed(&g_drbg, mbedtls_entropy_func, &g_ent, NULL, 0), "the random numbers");
    make_key(&g_ca_key);
    make_key(&g_stranger_key);
    make_key(&g_leaf_key);
    char ca[PEM_MAX], dir[PATH_MAX + 32];
    make_cert(ca, "CN=Clock CA", &g_ca_key, "CN=Clock CA", &g_ca_key, 1, "20000101000000", "21200101000000");
    need(mkdtemp(test_tmp(dir, sizeof dir, "netclock")) != NULL, "a folder");
    snprintf(g_ca_file, sizeof g_ca_file, "%s/ca.pem", dir);
    FILE *f = fopen(g_ca_file, "w");
    need(f && fputs(ca, f) >= 0 && !fclose(f), "the bundle");
    /* localhost: out of date (2001-2002), not yet (from 2090); another host; from an authority not trusted */
    make_cert(g_expired, "CN=localhost", &g_leaf_key, "CN=Clock CA", &g_ca_key, 0, "20010101000000", "20020101000000");
    make_cert(g_future, "CN=localhost", &g_leaf_key, "CN=Clock CA", &g_ca_key, 0, "20900101000000", "21200101000000");
    make_cert(g_other, "CN=other.example", &g_leaf_key, "CN=Clock CA", &g_ca_key, 0, "20200101000000", "21200101000000");
    make_cert(g_stranger, "CN=localhost", &g_leaf_key, "CN=Clock Stranger CA", &g_stranger_key, 0,
              "20010101000000", "20020101000000");
}

static void free_certs(void)
{
    remove(g_ca_file);
    char *slash = strrchr(g_ca_file, '/');
    if (slash) { *slash = '\0'; rmdir(g_ca_file); }
    mbedtls_pk_free(&g_ca_key);
    mbedtls_pk_free(&g_stranger_key);
    mbedtls_pk_free(&g_leaf_key);
    mbedtls_ctr_drbg_free(&g_drbg);
    mbedtls_entropy_free(&g_ent);
}

/* ---- a server for one connection ---- */
typedef struct {
    const char *pem;         /* its certificate (the key: g_leaf_key) */
    const char *answer;      /* what it sends back */
    int lfd, port;
    char request[1024];      /* what it was asked */
    int version;             /* the TLS version spoken */
} server_t;

static int g_tls12;           /* the server speaks TLS 1.2 at most */

static int s_send(void *c, const unsigned char *b, size_t n) { return (int)send(*(int *)c, b, n, MSG_NOSIGNAL); }
static int s_recv(void *c, unsigned char *b, size_t n) { return (int)recv(*(int *)c, b, n, 0); }

/* One connection: the handshake, the request read, the answer sent */
static void *serve(void *arg)
{
    server_t *s = arg;
    mbedtls_x509_crt crt;
    mbedtls_ssl_config conf;
    mbedtls_ssl_context ssl;
    mbedtls_x509_crt_init(&crt);
    mbedtls_ssl_config_init(&conf);
    mbedtls_ssl_init(&ssl);
    int fd = -1;
    if (mbedtls_x509_crt_parse(&crt, (const unsigned char *)s->pem, strlen(s->pem) + 1) ||
        mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT)) {
        fprintf(stderr, "test server: can't set up\n");
        goto out;
    }
    mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &g_drbg);
    mbedtls_ssl_conf_own_cert(&conf, &crt, &g_leaf_key);
    if (g_tls12) mbedtls_ssl_conf_max_tls_version(&conf, MBEDTLS_SSL_VERSION_TLS1_2);
    if (mbedtls_ssl_setup(&ssl, &conf)) goto out;
    if ((fd = accept(s->lfd, NULL, NULL)) < 0) goto out;
    struct timeval tv = {5, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    mbedtls_ssl_set_bio(&ssl, &fd, s_send, s_recv, NULL);
    int r;
    while ((r = mbedtls_ssl_handshake(&ssl)) != 0)
        if (r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE) goto out;   /* (refused: fine) */
    s->version = (int)mbedtls_ssl_get_version_number(&ssl);
    size_t n = 0;
    while (n < sizeof s->request - 1 && !strstr(s->request, "\r\n\r\n")) {
        r = mbedtls_ssl_read(&ssl, (unsigned char *)s->request + n, sizeof s->request - 1 - n);
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (r <= 0) break;
        n += (size_t)r;
        s->request[n] = '\0';
    }
    mbedtls_ssl_write(&ssl, (const unsigned char *)s->answer, strlen(s->answer));
    mbedtls_ssl_close_notify(&ssl);
out:
    if (fd >= 0) close(fd);
    close(s->lfd);
    mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&conf);
    mbedtls_x509_crt_free(&crt);
    return NULL;
}

#define ANSWER "HTTP/1.1 200 OK\r\nServer: test\r\nDate: Sun, 22 Nov 2026 18:05:09 GMT\r\nContent-Length: 0\r\n\r\n"

/* netclock_https_date's result against the server with certificate pem */
static int ask(const char *pem, const char *answer, const char *ca, int64_t *t, server_t *s)
{
    memset(s, 0, sizeof *s);
    s->pem = pem;
    s->answer = answer;
    s->lfd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t al = sizeof a;
    struct timeval tv = {5, 0};
    setsockopt(s->lfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);   /* (accept gives up too) */
    if (bind(s->lfd, (struct sockaddr *)&a, sizeof a) || listen(s->lfd, 1) ||
        getsockname(s->lfd, (struct sockaddr *)&a, &al)) {
        perror("test server");
        exit(1);
    }
    s->port = ntohs(a.sin_port);
    pthread_t th;
    pthread_create(&th, NULL, serve, s);
    *t = -7;
    int r = netclock_https_date("localhost", s->port, ca, t);
    pthread_join(th, NULL);
    return r;
}

static void https_tests(void)
{
    int64_t t, nov = utc(2026, 11, 22, 18, 5, 9);
    server_t s;
    /* a certificate out of date (as every one is to a clock in 1970), all else right: taken */
    CHECK(ask(g_expired, ANSWER, g_ca_file, &t, &s) == 0 && t == nov);
    CHECK(!strncmp(s.request, "HEAD / HTTP/1.1\r\n", 17) && strstr(s.request, "\r\nHost: localhost\r\n"));
    CHECK(ask(g_future, ANSWER, g_ca_file, &t, &s) == 0 && t == nov);
    /* the wrong host, or an authority not in the bundle: refused */
    CHECK(ask(g_other, ANSWER, g_ca_file, &t, &s) == -1 && t == -7);
    CHECK(ask(g_stranger, ANSWER, g_ca_file, &t, &s) == -1 && t == -7);
    CHECK(ask(g_expired, ANSWER, "/nonexistent/ca.pem", &t, &s) == -1);
    /* no Date in the head (one in the body doesn't count) */
    CHECK(ask(g_expired, "HTTP/1.1 200 OK\r\nContent-Length: 37\r\n\r\nDate: Sun, 22 Nov 2026 18:05:09 GMT\r\n",
              g_ca_file, &t, &s) == -1);
    CHECK(ask(g_expired, "nonsense\r\n\r\n", g_ca_file, &t, &s) == -1);
}

static void https_both(void)
{
#if defined(MBEDTLS_PSA_CRYPTO_C)
    psa_crypto_init();
#endif
    make_certs();
    server_t s;
    int64_t t;
    g_tls12 = 0;
    https_tests();
    ask(g_expired, ANSWER, g_ca_file, &t, &s);
    printf("netclock: HTTPS with TLS 0x%04x done\n", (unsigned)s.version);
    g_tls12 = 1;
    https_tests();
    ask(g_expired, ANSWER, g_ca_file, &t, &s);
    CHECK(s.version == MBEDTLS_SSL_VERSION_TLS1_2);
    printf("netclock: HTTPS with TLS 0x%04x done\n", (unsigned)s.version);
    free_certs();
}
#endif

/* netclock_ensure: a clock and a fetcher of the test's own */
static volatile int64_t fake_t;
static int fetch_calls, fetch_ok;
static int64_t fake_now(void) { return fake_t; }
static int fake_fetch(void)
{
    fetch_calls++;
    usleep(100000);                           /* (a while: another request waits meanwhile) */
    if (!fetch_ok) return -1;
    fake_t = 1795000000;
    return 0;
}
static void *ensure_thread(void *arg)
{
    *(int *)arg = netclock_ensure();
    return NULL;
}

static void test_ensure(void)
{
    netclock_set_now(fake_now);
    netclock_set_fetcher(fake_fetch);
    /* unset: fetched once, by two requests at the same time; then not again */
    fake_t = 86400;
    fetch_ok = 1;
    fetch_calls = 0;
    int r1 = -9, r2 = -9;
    pthread_t a, b;
    pthread_create(&a, NULL, ensure_thread, &r1);
    pthread_create(&b, NULL, ensure_thread, &r2);
    pthread_join(a, NULL);
    pthread_join(b, NULL);
    CHECK(r1 == 0 && r2 == 0 && fetch_calls == 1 && !netclock_unset());
    CHECK(netclock_ensure() == 0 && fetch_calls == 1);
    /* no answer (no internet): -1, and not asked again within 30 s */
    fake_t = 86400;
    fetch_ok = 0;
    CHECK(netclock_ensure() == -1 && fetch_calls == 2);
    CHECK(netclock_ensure() == -1 && fetch_calls == 2);
    netclock_set_fetcher(NULL);
    netclock_set_now(NULL);
}

int main(void)
{
    test_ensure();
    /* asking for the time now: once per want, and not when the clock is set */
    CHECK(!netclock_wanted());
    netclock_want();
    CHECK(netclock_wanted() && !netclock_wanted());
    if (!netclock_unset()) {
        CHECK(!strcmp(netclock_why("other"), "other") && !netclock_wanted());
    }

    parse_tests();
    year_tests();
#ifdef FATHOM_HAVE_MBEDTLS
    https_both();
#else
    printf("(no mbedTLS here: the HTTPS request not tried)\n");
#endif
    if (fails) {
        fprintf(stderr, "%d failed\n", fails);
        return 1;
    }
    printf("netclock: all passed\n");
    return 0;
}
