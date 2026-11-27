/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* A character pack fetched (app/net/packfetch.c): the download is a made-up
 * one (http_test_tries), the zips are written here. What reaches the folder
 * under the pack's name is the whole pack, or nothing does. */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "core/clock.h"
#include "net/http.h"
#include "net/packfetch.h"
#include "net/sha256.h"
#include "test_util.h"

enum { SIZE = 70000 };
static unsigned char g_pack[SIZE];
static char g_dir[512], g_sha[65];

/* ---- a zip of one stored file, written by hand ---- */
static uint32_t crc32_of(const unsigned char *d, size_t n)
{
    uint32_t c = 0xffffffffu;
    for (size_t i = 0; i < n; i++) {
        c ^= d[i];
        for (int k = 0; k < 8; k++) c = c >> 1 ^ (0xedb88320u & (0u - (c & 1)));
    }
    return ~c;
}
static void w16(FILE *f, unsigned v) { fputc((int)(v & 255), f); fputc((int)(v >> 8 & 255), f); }
static void w32(FILE *f, unsigned long v) { w16(f, (unsigned)(v & 0xffff)); w16(f, (unsigned)(v >> 16)); }

static void write_zip(const char *path, const char *name, const unsigned char *data, size_t n)
{
    FILE *f = fopen(path, "wb");
    if (!f) { t_fails++; printf("FAIL cannot write %s\n", path); return; }
    uint32_t crc = crc32_of(data, n);
    size_t nl = strlen(name);
    w32(f, 0x04034b50); w16(f, 20); w16(f, 0); w16(f, 0); w16(f, 0); w16(f, 0x21);
    w32(f, crc); w32(f, (unsigned long)n); w32(f, (unsigned long)n); w16(f, (unsigned)nl); w16(f, 0);
    fwrite(name, 1, nl, f);
    fwrite(data, 1, n, f);
    long cd = ftell(f);
    w32(f, 0x02014b50); w16(f, 20); w16(f, 20); w16(f, 0); w16(f, 0); w16(f, 0); w16(f, 0x21);
    w32(f, crc); w32(f, (unsigned long)n); w32(f, (unsigned long)n); w16(f, (unsigned)nl); w16(f, 0); w16(f, 0);
    w16(f, 0); w16(f, 0); w32(f, 0); w32(f, 0);
    fwrite(name, 1, nl, f);
    long end = ftell(f);
    w32(f, 0x06054b50); w16(f, 0); w16(f, 0); w16(f, 1); w16(f, 1);
    w32(f, (unsigned long)(end - cd)); w32(f, (unsigned long)cd); w16(f, 0);
    fclose(f);
}

/* ---- the made-up download ---- */
static char g_serve[600];            /* the file "the server" has; "": none */
static int64_t g_answer;             /* what the download returns in place of the file's length (0: the length) */
static int g_calls, g_cancel, g_slow_ms, g_deaf;
static char g_asked[900];            /* the address last asked for */
static volatile int g_abort;

static long fake_get(const char *url, char *buf, size_t cap, const volatile int *abort)
{
    (void)url; (void)buf; (void)cap; (void)abort;
    return -1;
}

static int64_t fake_download(const char *url, const char *path, http_progress_fn progress, void *ctx, const volatile int *abort)
{
    snprintf(g_asked, sizeof g_asked, "%s", url);
    g_calls++;
    for (int waited = 0; waited < g_slow_ms; waited += 20) {     /* a slow server */
        if (!g_deaf && abort && *abort) return -1;
        usleep(20000);
    }
    if (g_cancel) { g_abort = 1; return -1; }
    if (g_answer) return g_answer;
    FILE *in = fopen(g_serve, "rb"), *out = fopen(path, "wb");
    if (!in || !out) { if (in) fclose(in); if (out) fclose(out); return HTTP_ERR_WRITE; }
    char buf[4096];
    size_t n;
    int64_t total = 0;
    fseek(in, 0, SEEK_END);
    long len = ftell(in);
    rewind(in);
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) {
        fwrite(buf, 1, n, out);
        total += (int64_t)n;
        if (progress) progress((uint64_t)total, (uint64_t)len, ctx);
    }
    fclose(in);
    fclose(out);
    return total;
}

static long ms_now(void) { return (long)clock_ms(); }

static int g_last_pct, g_backwards, g_steps;
static void on_progress(int pct, void *ctx)
{
    (void)ctx;
    if (pct < g_last_pct) g_backwards = 1;
    g_last_pct = pct;
    g_steps++;
}

/* The folder's files, as "a b c" in name order (what a fetch left behind) */
static const char *listing(void)
{
    static char out[512];
    char names[16][64];
    int n = 0;
    DIR *d = opendir(g_dir);
    struct dirent *e;
    while (d && (e = readdir(d)) && n < 16)
        if (e->d_name[0] != '.') snprintf(names[n++], sizeof names[0], "%s", e->d_name);
    if (d) closedir(d);
    qsort(names, (size_t)n, sizeof names[0], (int (*)(const void *, const void *))strcmp);
    out[0] = 0;
    for (int i = 0; i < n; i++) snprintf(out + strlen(out), sizeof out - strlen(out), "%s%s", i ? " " : "", names[i]);
    return out;
}

static int fetch(const pack_info_t *p)
{
    g_calls = 0;
    g_abort = 0;
    g_last_pct = -1;
    g_backwards = g_steps = 0;
    return pack_fetch(g_dir, p, on_progress, NULL, &g_abort);
}

static int file_is_pack(void)
{
    char path[700];
    static unsigned char got[SIZE + 1];
    snprintf(path, sizeof path, "%s/glyphs-xx.bin", g_dir);
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    size_t n = fread(got, 1, sizeof got, f);
    fclose(f);
    return n == SIZE && !memcmp(got, g_pack, SIZE);
}

int main(void)
{
    const char *tmp = getenv("TMPDIR");
    char base[400], zip[600];
    snprintf(base, sizeof base, "%s/fathom-test-packfetch-%d", tmp && *tmp ? tmp : "/tmp", (int)getpid());
    snprintf(g_dir, sizeof g_dir, "%s/packs", base);
    snprintf(zip, sizeof zip, "%s/served.zip", base);
    CHECK(mkdir(base, 0755) == 0);

    for (int i = 0; i < SIZE; i++) g_pack[i] = (unsigned char)(i * 7 + i / 251);
    sha256_t s;
    uint8_t d[32];
    sha256_init(&s);
    sha256_update(&s, g_pack, SIZE);
    sha256_final(&s, d);
    sha256_hex(d, g_sha);
    const pack_info_t P = {"xx", "Test", "glyphs-xx.bin", "https://example.invalid/glyphs-xx.zip", 70200, SIZE, g_sha, 0, NULL};
    packs_test_list(&P, 1);
    CHECK(packs_count() == 1 && packs_info(0) == &P && packs_info(1) == NULL && packs_info(-1) == NULL);
    http_test_tries(fake_get, fake_download, 1, 1);
    snprintf(g_serve, sizeof g_serve, "%s", zip);

    /* ---- nothing there yet ---- */
    CHECK(!pack_installed(g_dir, &P));
    CHECK(pack_remove(g_dir, &P) == 0);

    /* ---- the pack, whole: named, the same bytes, nothing else left, progress to 100 and never back ---- */
    write_zip(zip, "glyphs-xx.bin", g_pack, SIZE);
    CHECK(fetch(&P) == PACK_OK);
    CHECK(pack_installed(g_dir, &P) && file_is_pack());
    CHECK(!strcmp(listing(), "glyphs-xx.bin"));
    CHECK(g_last_pct == 100 && !g_backwards && g_steps > 3 && g_calls == 1);
    CHECK(!strcmp(g_asked, "https://example.invalid/glyphs-xx.zip"));

    /* ---- from another place (a pack tried before it is published): the same file's name there ---- */
    packs_url_base("http://10.42.0.2:8000/packs/");
    CHECK(fetch(&P) == PACK_OK && !strcmp(g_asked, "http://10.42.0.2:8000/packs/glyphs-xx.zip") && file_is_pack());
    packs_url_base("");
    CHECK(fetch(&P) == PACK_OK && !strcmp(g_asked, "https://example.invalid/glyphs-xx.zip"));
    packs_url_base(NULL);

    /* ---- every way it can go wrong: said, and the pack that was there stays as it was ---- */
    g_answer = -1;                                   /* no network: three tries */
    CHECK(fetch(&P) == PACK_ERR_NET && g_calls == 3);
    g_answer = -2;                                   /* the server answered, not with the file */
    CHECK(fetch(&P) == PACK_ERR_DOWNLOAD && g_calls == 1);
    g_answer = HTTP_ERR_FULL;
    CHECK(fetch(&P) == PACK_ERR_SPACE);
    g_answer = HTTP_ERR_WRITE;
    CHECK(fetch(&P) == PACK_ERR_WRITE);
    g_answer = 0;
    g_cancel = 1;                                    /* cancelled while it downloads */
    CHECK(fetch(&P) == PACK_ERR_CANCELLED && g_calls == 1);
    g_cancel = 0;

    FILE *f = fopen(zip, "wb");                      /* not a zip: a sign-in page */
    fputs("<html><body>Sign in to the network</body></html>", f);
    fclose(f);
    CHECK(fetch(&P) == PACK_ERR_DAMAGED);
    write_zip(zip, "glyphs-yy.bin", g_pack, SIZE);   /* a zip of something else */
    CHECK(fetch(&P) == PACK_ERR_DAMAGED);
    write_zip(zip, "glyphs-xx.bin", g_pack, SIZE - 1);   /* the file of another length */
    CHECK(fetch(&P) == PACK_ERR_DAMAGED);
    g_pack[1234] ^= 1;                               /* the right length, one bit wrong */
    write_zip(zip, "glyphs-xx.bin", g_pack, SIZE);
    g_pack[1234] ^= 1;
    CHECK(fetch(&P) == PACK_ERR_DAMAGED);
    write_zip(zip, "glyphs-xx.bin", g_pack, SIZE);   /* the zip cut short */
    CHECK(truncate(zip, 30000) == 0);
    CHECK(fetch(&P) == PACK_ERR_DAMAGED);

    CHECK(pack_installed(g_dir, &P) && file_is_pack());      /* still the pack of before */
    CHECK(!strcmp(listing(), "glyphs-xx.bin"));              /* and nothing left beside it */

    /* ---- no room: said before anything is fetched ---- */
    const pack_info_t BIG = {"xx", "Test", "glyphs-xx.bin", "https://example.invalid/glyphs-xx.zip", (int64_t)1 << 60, SIZE, g_sha, 0, NULL};
    CHECK(fetch(&BIG) == PACK_ERR_SPACE && g_calls == 0);

    /* ---- what a fetch cut short by the power left is cleared by the next, and by Remove ---- */
    char path[700];
    static const char *const LEFT[] = {"glyphs-xx.bin.part", "glyphs-xx.bin.zip", "glyphs-xx.bin.zip.part"};
    for (int round = 0; round < 2; round++) {
        for (int i = 0; i < 3; i++) {
            snprintf(path, sizeof path, "%s/%s", g_dir, LEFT[i]);
            f = fopen(path, "wb");
            fputs("half", f);
            fclose(f);
        }
        if (round == 0) {
            write_zip(zip, "glyphs-xx.bin", g_pack, SIZE);
            CHECK(fetch(&P) == PACK_OK && !strcmp(listing(), "glyphs-xx.bin") && file_is_pack());
        } else {
            CHECK(pack_remove(g_dir, &P) == 0 && !strcmp(listing(), "") && !pack_installed(g_dir, &P));
        }
    }

    /* ---- one fetch at a time, on its own thread ---- */
    pack_status_t st;
    unsigned seq0 = packs_seq(), gen0 = packs_gen();
    packs_status(0, &st);
    CHECK(st.state == PACK_ABSENT);                  /* (no folder named yet) */
    CHECK(packs_get(0) == -1);                       /* ...so nothing to fetch into */
    packs_init(g_dir);
    CHECK(packs_seq() != seq0);
    packs_status(0, &st);
    CHECK(st.state == PACK_ABSENT && !packs_busy());
    CHECK(packs_get(1) == -1 && packs_get(-1) == -1 && packs_delete(5) == -1);
    packs_status(3, &st);
    CHECK(st.state == PACK_ABSENT);

    /* fetched: getting, with a percentage, then there; the text to be drawn again */
    write_zip(zip, "glyphs-xx.bin", g_pack, SIZE);
    g_slow_ms = 300;
    seq0 = packs_seq();
    CHECK(packs_get(0) == 0);
    CHECK(packs_get(0) == -1);                       /* one at a time */
    CHECK(packs_busy());
    packs_status(0, &st);
    CHECK(st.state == PACK_GETTING);
    CHECK(packs_delete(0) == -1);                    /* not while it is being fetched */
    CHECK(packs_stop_wait(5000) == 1);
    packs_status(0, &st);
    CHECK(st.state == PACK_THERE && !packs_busy() && file_is_pack());
    CHECK(packs_seq() != seq0 && packs_gen() == gen0 + 1);

    /* removed: absent, and drawn again */
    CHECK(packs_delete(0) == 0);
    packs_status(0, &st);
    CHECK(st.state == PACK_ABSENT && packs_gen() == gen0 + 2 && !strcmp(listing(), ""));

    /* a fetch that fails: failed, with why; then one that can't start, with its why; then a good one clears it */
    g_slow_ms = 0;
    g_answer = -1;
    CHECK(packs_get(0) == 0 && packs_stop_wait(5000) == 1);
    packs_status(0, &st);
    CHECK(st.state == PACK_FAILED && !strcmp(st.why, pack_error_text(PACK_ERR_NET)) && packs_gen() == gen0 + 2);
    packs_refuse(0, "Wi-Fi is off: Settings > Wi-Fi");
    packs_status(0, &st);
    CHECK(st.state == PACK_FAILED && !strcmp(st.why, "Wi-Fi is off: Settings > Wi-Fi"));
    g_answer = 0;
    CHECK(packs_get(0) == 0 && packs_stop_wait(5000) == 1);
    packs_status(0, &st);
    CHECK(st.state == PACK_THERE && packs_gen() == gen0 + 3);

    /* cancelled while it downloads: absent again (the one there was removed first), no failure shown */
    CHECK(packs_delete(0) == 0);
    g_slow_ms = 2000;
    CHECK(packs_get(0) == 0);
    usleep(100000);
    long t0 = ms_now();
    packs_cancel();
    CHECK(packs_stop_wait(5000) == 1);
    CHECK(ms_now() - t0 < 1500);                     /* (the made-up download looks at the flag every 20 ms) */
    packs_status(0, &st);
    CHECK(st.state == PACK_ABSENT && !strcmp(listing(), ""));

    /* the player ending during a fetch: asked, and waited for no longer than said */
    g_slow_ms = 2000;
    g_deaf = 1;                                      /* a download that doesn't look at the flag (a name lookup) */
    CHECK(packs_get(0) == 0);
    usleep(50000);
    packs_stop_ask();
    t0 = ms_now();
    CHECK(packs_stop_wait(200) == 0);
    CHECK(ms_now() - t0 < 600);
    CHECK(packs_stop_wait(5000) == 1);               /* (it does end: nothing is left running for the next test) */
    g_deaf = 0;
    g_slow_ms = 0;
    pack_remove(g_dir, &P);

    /* ---- the texts, and the built-in list ---- */
    for (int e = PACK_ERR_NET; e <= PACK_ERR_CANCELLED; e++) CHECK(pack_error_text(e)[0]);
    CHECK(!pack_error_text(PACK_OK)[0]);
    packs_test_list(NULL, 0);
    CHECK(packs_count() >= 1);
    for (int i = 0; i < packs_count(); i++) {
        const pack_info_t *b = packs_info(i);
        CHECK(b && strlen(b->sha256) == 64 && b->size > 0 && b->zip_size > 0 && !strncmp(b->url, "https://", 8));
        CHECK(!strncmp(b->file, "glyphs-", 7) && !strcmp(b->file + strlen(b->file) - 4, ".bin"));
    }
    CHECK(!pack_installed(NULL, &P) && !pack_installed(g_dir, NULL) && pack_remove(NULL, &P) == -1);

    /* ---- the real thing, when it is at hand (FATHOM_TEST_PACK_ZIP: the zip
     * tools/fonts/mkpack.py made, which is not in the repository): each
     * built-in pack whose zip that is comes through the player's own unzip and
     * passes its size and digest ---- */
    const char *real = getenv("FATHOM_TEST_PACK_ZIP");
    if (real && *real) {
        snprintf(g_serve, sizeof g_serve, "%s", real);
        g_answer = 0;
        int ok = 0;
        for (int i = 0; i < packs_count(); i++) {
            const pack_info_t *b = packs_info(i);
            /* (the zip of this pack: its file's name, ".zip" for ".bin") */
            char zname[80];
            snprintf(zname, sizeof zname, "%.*s.zip", (int)strlen(b->file) - 4, b->file);
            const char *leaf = strrchr(real, '/');
            if (strcmp(leaf ? leaf + 1 : real, zname)) continue;
            g_last_pct = -1;
            g_abort = 0;
            int err = pack_fetch(g_dir, b, on_progress, NULL, &g_abort);
            struct stat zs;
            CHECK(err == PACK_OK && pack_installed(g_dir, b) && g_last_pct == 100);
            CHECK(stat(real, &zs) == 0 && (int64_t)zs.st_size == b->zip_size);
            printf("packfetch: %s from %s: %s\n", b->file, real, err ? pack_error_text(err) : "whole");
            pack_remove(g_dir, b);
            ok++;
        }
        CHECK(ok == 1);
    } else {
        printf("packfetch: the published zip not tried (FATHOM_TEST_PACK_ZIP not set)\n");
    }

    http_test_tries(NULL, NULL, 2000, 5000);
    unlink(zip);
    rmdir(g_dir);
    rmdir(base);
    return t_result();
}
