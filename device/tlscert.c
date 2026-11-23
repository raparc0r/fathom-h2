/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* The network drive's certificate, made on the H2 with the mbedTLS the player
 * already has (FFmpeg's https): an ECDSA P-256 key and a certificate it signs
 * itself, naming the H2 (fathom-h2.local). Nobody vouches for it, so a computer
 * asks once whether to trust it; the fingerprint on the H2's screen is how to
 * tell it's this one. Kept on the card, so the computer's trust lasts.
 *
 * What macOS asks of a server certificate even when the user trusts it: the
 * name in the subject alternative names, "server authentication" as its
 * extended key usage, and at most 825 days. It's renewed a month before it
 * ends (when the clock is set; the H2 has no battery-backed clock). */
#include "tlscert.h"
#include "core/fsafe.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifdef FATHOM_HAVE_MBEDTLS
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/ecp.h>
#include <mbedtls/entropy.h>
#include <mbedtls/oid.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>
#include <mbedtls/x509_crt.h>
#if defined(MBEDTLS_PSA_CRYPTO_C)
#include <psa/crypto.h>
#endif

#define DAYS 800

static int ymd(int y, int m, int d) { return y * 10000 + m * 100 + d; }

static int clock_set(void)
{
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    return tm.tm_year + 1900 >= 2026;
}

/* The one there: readable, and not ending within a month. */
static int usable(const char *key_path, const char *crt_path, const char *host, char *fp, size_t cap)
{
    if (access(key_path, R_OK)) return 0;
    mbedtls_x509_crt crt;
    mbedtls_x509_crt_init(&crt);
    int ok = mbedtls_x509_crt_parse_file(&crt, crt_path) == 0;
    if (ok) {                           /* (another name: made anew) */
        char dn[256], want[160];
        snprintf(want, sizeof want, "CN=%s", host);
        const char *cn = mbedtls_x509_dn_gets(dn, sizeof dn, &crt.subject) < 0 ? NULL : strstr(dn, want);
        size_t wl = strlen(want);
        if (!cn || (cn != dn && cn[-1] != ' ') || (cn[wl] && cn[wl] != ',')) ok = 0;
    }
    if (ok && clock_set()) {
        time_t soon = time(NULL) + 30 * 86400;
        struct tm tm;
        gmtime_r(&soon, &tm);
        if (ymd(crt.valid_to.year, crt.valid_to.mon, crt.valid_to.day) <
            ymd(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday))
            ok = 0;
    }
    if (ok && fp && cap) {
        unsigned char h[32];
        mbedtls_sha256(crt.raw.p, crt.raw.len, h, 0);
        size_t o = 0;
        fp[0] = '\0';
        for (int i = 0; i < 6 && o + 4 < cap; i++) o += (size_t)snprintf(fp + o, cap - o, i ? " %02X" : "%02X", h[i]);
    }
    mbedtls_x509_crt_free(&crt);
    return ok;
}

/* Written whole or not at all (a new file, then renamed over the old). */
static int put(const char *path, const unsigned char *buf, size_t n)
{
    char tmp[600];
    snprintf(tmp, sizeof tmp, "%s.new", path);
    FILE *f = fopen(tmp, "wbe");
    if (!f) return -1;
    int ok = fwrite(buf, 1, n, f) == n;
    ok = fflush(f) == 0 && ok;
    fsync(fileno(f));
    ok = fclose(f) == 0 && ok;
    if (!ok || file_replace(tmp, path)) { unlink(tmp); return -1; }
    return 0;
}

static void stamp(time_t t, char out[16])
{
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(out, 16, "%Y%m%d%H%M%S", &tm);
}

static int make(const char *key_path, const char *crt_path, const char *host)
{
    int ret = -1;
    mbedtls_entropy_context ent;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_pk_context key;
    mbedtls_x509write_cert w;
    mbedtls_entropy_init(&ent);
    mbedtls_ctr_drbg_init(&drbg);
    mbedtls_pk_init(&key);
    mbedtls_x509write_crt_init(&w);
#if defined(MBEDTLS_PSA_CRYPTO_C)
    psa_crypto_init();
#endif
    static unsigned char buf[4096];
    static const char pers[] = "fathom-drive-cert";
    if (mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &ent, (const unsigned char *)pers, sizeof pers - 1)) goto out;
    if (mbedtls_pk_setup(&key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY))) goto out;
    if (mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(key), mbedtls_ctr_drbg_random, &drbg)) goto out;

    /* From a day ago (computers' clocks differ), or from the start of 2026
     * while the H2's clock isn't set yet. */
    time_t from = clock_set() ? time(NULL) - 86400 : (time_t)1767225600;   /* 2026-01-01 */
    char nb[16], na[16], name[128];
    stamp(from, nb);
    stamp(from + (time_t)DAYS * 86400, na);
    snprintf(name, sizeof name, "CN=%s,O=Fathom", host);
    unsigned char serial[16];
    if (mbedtls_ctr_drbg_random(&drbg, serial, sizeof serial)) goto out;
    serial[0] = (unsigned char)((serial[0] & 0x7f) | 0x40);   /* positive, all 16 bytes */

    mbedtls_x509write_crt_set_version(&w, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&w, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&w, &key);
    mbedtls_x509write_crt_set_issuer_key(&w, &key);
    if (mbedtls_x509write_crt_set_subject_name(&w, name) || mbedtls_x509write_crt_set_issuer_name(&w, name)) goto out;
    if (mbedtls_x509write_crt_set_serial_raw(&w, serial, sizeof serial)) goto out;
    if (mbedtls_x509write_crt_set_validity(&w, nb, na)) goto out;
    if (mbedtls_x509write_crt_set_basic_constraints(&w, 0, -1)) goto out;
    if (mbedtls_x509write_crt_set_key_usage(&w, MBEDTLS_X509_KU_DIGITAL_SIGNATURE)) goto out;
    mbedtls_asn1_sequence eku = {0};
    eku.buf.tag = MBEDTLS_ASN1_OID;
    eku.buf.p = (unsigned char *)MBEDTLS_OID_SERVER_AUTH;
    eku.buf.len = MBEDTLS_OID_SIZE(MBEDTLS_OID_SERVER_AUTH);
    if (mbedtls_x509write_crt_set_ext_key_usage(&w, &eku)) goto out;
    mbedtls_x509_san_list san = {0};
    san.node.type = MBEDTLS_X509_SAN_DNS_NAME;
    san.node.san.unstructured_name.p = (unsigned char *)host;
    san.node.san.unstructured_name.len = strlen(host);
    if (mbedtls_x509write_crt_set_subject_alternative_name(&w, &san)) goto out;

    if (mbedtls_pk_write_key_pem(&key, buf, sizeof buf)) goto out;
    if (put(key_path, buf, strlen((const char *)buf))) goto out;
    if (mbedtls_x509write_crt_pem(&w, buf, sizeof buf, mbedtls_ctr_drbg_random, &drbg)) goto out;
    if (put(crt_path, buf, strlen((const char *)buf))) goto out;
    ret = 0;
out:
    memset(buf, 0, sizeof buf);
    mbedtls_x509write_crt_free(&w);
    mbedtls_pk_free(&key);
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&ent);
    return ret;
}

int tlscert_ensure(const char *key_path, const char *crt_path, const char *host, char *fp, size_t cap)
{
    if (fp && cap) fp[0] = '\0';
    if (usable(key_path, crt_path, host, fp, cap)) return 0;
    char dir[600];
    snprintf(dir, sizeof dir, "%s", key_path);
    char *slash = strrchr(dir, '/');
    if (slash) { *slash = '\0'; mkdir(dir, 0700); }
    if (make(key_path, crt_path, host)) {
        fprintf(stderr, "share: couldn't make a certificate\n");
        return -1;
    }
    fprintf(stderr, "share: a new certificate for %s\n", host);
    return usable(key_path, crt_path, host, fp, cap) ? 0 : -1;
}

#else   /* no mbedTLS: no HTTPS */

int tlscert_ensure(const char *key_path, const char *crt_path, const char *host, char *fp, size_t cap)
{
    (void)key_path; (void)crt_path; (void)host;
    if (fp && cap) fp[0] = '\0';
    return -1;
}

#endif
