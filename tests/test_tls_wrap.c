/* SPDX-License-Identifier: GPL-3.0-or-later */
/* app/net/tls_wrap.c: one parsed certificate bundle lent to every connection,
 * verifying as a private parse would. Linked with the same --wrap options as
 * the player, so these calls go through the wrapper as FFmpeg's do.
 * Certificates: tests/tls (make.sh). */
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <mbedtls/x509_crt.h>

#include "audio/ff_util.h"
#include "net/tls_wrap.h"
#include "tmpdir.h"

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static char dir[256], bundle[300];

static void copy(const char *from, const char *to)
{
    FILE *a = fopen(from, "rb"), *b = fopen(to, "wb");
    char buf[4096];
    size_t n;
    while (a && b && (n = fread(buf, 1, sizeof buf, a)) > 0) fwrite(buf, 1, n, b);
    if (a) fclose(a);
    if (b) fclose(b);
}

static void load(mbedtls_x509_crt *c, const char *name)
{
    char p[300];
    snprintf(p, sizeof p, "tests/tls/%s.pem", name);
    mbedtls_x509_crt_init(c);
    if (mbedtls_x509_crt_parse_file(c, p)) { fprintf(stderr, "can't read %s\n", p); exit(1); }
}

static tls_wrap_stats_t stats(void)
{
    tls_wrap_stats_t s;
    tls_wrap_stats(&s);
    return s;
}

static void ms_sleep(int ms)
{
    struct timespec t = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&t, NULL);
}

/* The flags a verification of leaf against ca leaves (0: trusted). */
static uint32_t verify(mbedtls_x509_crt *leaf, mbedtls_x509_crt *ca, const char *host)
{
    uint32_t flags = 0;
    int r = mbedtls_x509_crt_verify(leaf, ca, NULL, host, &flags, NULL, NULL);
    if ((r == 0) != (flags == 0)) { fprintf(stderr, "verify returned %d with flags %x\n", r, flags); fails++; }
    return flags;
}

static mbedtls_x509_crt ok_crt;

static void *hammer(void *arg)
{
    (void)arg;
    for (int i = 0; i < 200; i++) {
        mbedtls_x509_crt ca;
        mbedtls_x509_crt_init(&ca);
        if (mbedtls_x509_crt_parse_file(&ca, bundle)) { fails++; break; }
        if (verify(&ok_crt, &ca, "localhost")) fails++;
        mbedtls_x509_crt_free(&ca);
    }
    return NULL;
}

int main(void)
{
    test_tmp(dir, sizeof dir, "test_tls_wrap");
    if (!mkdtemp(dir)) return 1;
    snprintf(bundle, sizeof bundle, "%s/ca.pem", dir);
    copy("tests/tls/bundle.pem", bundle);
    ff_set_ca_file(bundle);

    mbedtls_x509_crt ok, future, expired, stranger;
    load(&ok, "ok");
    load(&future, "future");
    load(&expired, "expired");
    load(&stranger, "stranger");
    CHECK(stats().parses == 0);          /* (other files go straight through) */

    /* two connections: one parse, both verify through the shared chain
     * (Test CA is the second: found through the copy's ->next) */
    mbedtls_x509_crt a, b;
    mbedtls_x509_crt_init(&a);
    mbedtls_x509_crt_init(&b);
    CHECK(mbedtls_x509_crt_parse_file(&a, bundle) == 0);
    CHECK(mbedtls_x509_crt_parse_file(&b, bundle) == 0);
    tls_wrap_stats_t s = stats();
    CHECK(s.parses == 1 && s.chains == 1 && s.lent == 2);
    CHECK(a.next && a.next == b.next && a.raw.p == b.raw.p && &a != &b);
    CHECK(verify(&ok, &a, "localhost") == 0);
    CHECK(verify(&ok, &b, "localhost") == 0);
    CHECK(verify(&stranger, &a, "localhost") == MBEDTLS_X509_BADCERT_NOT_TRUSTED);
    CHECK(verify(&ok, &a, "example.com") == MBEDTLS_X509_BADCERT_CN_MISMATCH);
    /* adding to a loan would add to everyone's chain */
    CHECK(mbedtls_x509_crt_parse_file(&a, bundle) != 0 && stats().parses == 1);

    /* dates checked as ever, through the copy and the shared rest alike */
    CHECK(verify(&future, &a, "localhost") == MBEDTLS_X509_BADCERT_FUTURE);
    CHECK(verify(&expired, &b, "localhost") == MBEDTLS_X509_BADCERT_EXPIRED);

    /* loans back: kept for the grace period, then freed */
    tls_wrap_set_grace_ms(200);
    mbedtls_x509_crt_free(&a);
    CHECK(a.raw.p == NULL && b.raw.p != NULL);
    mbedtls_x509_crt_free(&b);
    mbedtls_x509_crt_free(&b);           /* (twice: harmless, as with mbedTLS) */
    s = stats();
    CHECK(s.chains == 1 && s.lent == 0);
    mbedtls_x509_crt_init(&a);
    CHECK(mbedtls_x509_crt_parse_file(&a, bundle) == 0 && stats().parses == 1);   /* within it: no parse */
    mbedtls_x509_crt_free(&a);
    ms_sleep(600);
    CHECK(stats().chains == 0);
    mbedtls_x509_crt_init(&a);
    CHECK(mbedtls_x509_crt_parse_file(&a, bundle) == 0 && stats().parses == 2);

    /* the bundle changes: new connections get the new one, the old stays
     * for the connection using it until it's done */
    FILE *f = fopen(bundle, "a");
    if (f) { fputs("\n", f); fclose(f); }
    struct timespec t[2] = {{0, UTIME_OMIT}, {time(NULL) + 10, 0}};
    utimensat(AT_FDCWD, bundle, t, 0);
    mbedtls_x509_crt_init(&b);
    CHECK(mbedtls_x509_crt_parse_file(&b, bundle) == 0);
    s = stats();
    CHECK(s.parses == 3 && s.chains == 2 && s.lent == 2 && a.raw.p != b.raw.p);
    CHECK(verify(&ok, &a, "localhost") == 0 && verify(&ok, &b, "localhost") == 0);
    mbedtls_x509_crt_free(&a);
    CHECK(stats().chains == 1);          /* the old one at once: nobody will want it */
    mbedtls_x509_crt_free(&b);

    /* other files: their own parse, as ever */
    char other[300];
    snprintf(other, sizeof other, "%s/other.pem", dir);
    copy("tests/tls/bundle.pem", other);
    mbedtls_x509_crt_init(&a);
    CHECK(mbedtls_x509_crt_parse_file(&a, other) == 0 && stats().parses == 3 && stats().lent == 0);
    mbedtls_x509_crt_free(&a);

    /* the bundle gone: the same error as mbedTLS gives, nothing kept */
    ms_sleep(400);
    unlink(bundle);
    mbedtls_x509_crt_init(&a);
    int r = mbedtls_x509_crt_parse_file(&a, bundle);
    CHECK(r != 0 && r == mbedtls_x509_crt_parse_file(&b, "/nonexistent/ca.pem"));
    CHECK(stats().chains == 0 && stats().lent == 0);
    mbedtls_x509_crt_free(&a);
    /* ... and one that doesn't parse */
    f = fopen(bundle, "w");
    if (f) { fputs("-----BEGIN CERTIFICATE-----\nnonsense\n-----END CERTIFICATE-----\n", f); fclose(f); }
    mbedtls_x509_crt_init(&a);
    CHECK(mbedtls_x509_crt_parse_file(&a, bundle) != 0 && stats().chains == 0);
    mbedtls_x509_crt_free(&a);

    /* threads: parse, verify, free at once from several; one parse in all */
    copy("tests/tls/bundle.pem", bundle);
    tls_wrap_set_grace_ms(30000);
    unsigned before = stats().parses;
    ok_crt = ok;
    pthread_t th[4];
    for (int i = 0; i < 4; i++) pthread_create(&th[i], NULL, hammer, NULL);
    for (int i = 0; i < 4; i++) pthread_join(th[i], NULL);
    s = stats();
    CHECK(s.parses == before + 1 && s.lent == 0 && s.chains == 1);

    mbedtls_x509_crt_free(&ok);
    mbedtls_x509_crt_free(&future);
    mbedtls_x509_crt_free(&expired);
    mbedtls_x509_crt_free(&stranger);
    unlink(bundle);
    unlink(other);
    rmdir(dir);
    if (fails) { fprintf(stderr, "%d failed\n", fails); return 1; }
    printf("tls_wrap: ok\n");
    return 0;
}
