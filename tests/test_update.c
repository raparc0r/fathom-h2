/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Updates over Wi-Fi without the network (app/net/update.c, unzip.c, sha256.c):
 * versions, GitHub's answer (a real one, tests/data; its author and uploader
 * objects left out), SHA-256, and staging and
 * installing a release zip made here onto a pretend card: what's replaced,
 * what stays, damaged zips, and an install cut short at various points.
 *
 * FATHOM_TEST_RELEASE_ZIP=dist/fathom-h2-0.7.0.zip also stages a real release.
 */
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#include "net/sha256.h"
#include "net/unzip.h"
#include "net/update.h"

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

/* ---------- files ---------- */

static char g_tmp[256];

static void put(const char *path, const char *text)
{
    char p[1024];
    snprintf(p, sizeof p, "%s", path);
    for (char *s = p + 1; *s; s++)
        if (*s == '/') { *s = '\0'; mkdir(p, 0755); *s = '/'; }
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    fputs(text, f);
    fclose(f);
}

/* The file's contents (static buffer, NUL-terminated), or NULL. */
static const char *get(const char *path)
{
    static char buf[4096];
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = '\0';
    return buf;
}

static int is(const char *path, const char *text)
{
    const char *s = get(path);
    return s && !strcmp(s, text);
}

static int exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

static const char *at(const char *rel)
{
    static char p[8][1024];
    static int k;
    k = (k + 1) % 8;
    snprintf(p[k], sizeof p[k], "%s/%s", g_tmp, rel);
    return p[k];
}

/* ---------- a zip writer, enough for release-like zips ---------- */

typedef struct { char name[256]; uint32_t crc, cs, us, off; uint16_t method; } zent_t;
static zent_t g_ents[64];
static int g_nents;
static FILE *g_zf;

static void w16(uint16_t v) { fputc(v & 255, g_zf); fputc(v >> 8, g_zf); }
static void w32(uint32_t v) { w16((uint16_t)v); w16((uint16_t)(v >> 16)); }

static void zip_begin(const char *path)
{
    g_zf = fopen(path, "wb");
    if (!g_zf) { perror(path); exit(1); }
    g_nents = 0;
}

/* An entry (a folder if name ends in '/'); deflated if big enough.
 * corrupt: one byte of the stored data flipped after its CRC. */
static void zip_add(const char *name, const void *data, size_t n, int corrupt)
{
    zent_t *e = &g_ents[g_nents++];
    snprintf(e->name, sizeof e->name, "%s", name);
    e->us = (uint32_t)n;
    e->crc = (uint32_t)crc32(0L, data, (uInt)n);
    e->off = (uint32_t)ftell(g_zf);
    unsigned char *out = NULL;
    size_t on = n;
    e->method = n > 64 ? 8 : 0;
    if (e->method == 8) {
        z_stream s;
        memset(&s, 0, sizeof s);
        deflateInit2(&s, 6, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
        out = malloc(deflateBound(&s, (uLong)n));
        s.next_in = (Bytef *)data;
        s.avail_in = (uInt)n;
        s.next_out = out;
        s.avail_out = (uInt)deflateBound(&s, (uLong)n);
        deflate(&s, Z_FINISH);
        on = s.total_out;
        deflateEnd(&s);
    } else if (n) {
        out = malloc(n);
        memcpy(out, data, n);
    }
    if (corrupt && on) out[on / 2] ^= 0x55;
    e->cs = (uint32_t)on;
    w32(0x04034b50); w16(20); w16(0); w16(e->method); w16(0); w16(0);
    w32(e->crc); w32(e->cs); w32(e->us); w16((uint16_t)strlen(name)); w16(0);
    fwrite(name, 1, strlen(name), g_zf);
    if (on) fwrite(out, 1, on, g_zf);
    free(out);
}

static void zip_end(void)
{
    uint32_t cd = (uint32_t)ftell(g_zf);
    for (int i = 0; i < g_nents; i++) {
        zent_t *e = &g_ents[i];
        w32(0x02014b50); w16(20); w16(20); w16(0); w16(e->method); w16(0); w16(0);
        w32(e->crc); w32(e->cs); w32(e->us); w16((uint16_t)strlen(e->name)); w16(0); w16(0);
        w16(0); w16(0); w32(0); w32(e->off);
        fwrite(e->name, 1, strlen(e->name), g_zf);
    }
    uint32_t end = (uint32_t)ftell(g_zf);
    w32(0x06054b50); w16(0); w16(0); w16((uint16_t)g_nents); w16((uint16_t)g_nents);
    w32(end - cd); w32(cd); w16(0);
    fclose(g_zf);
}

static void hex_of(const void *data, size_t n, char hex[65])
{
    sha256_t s;
    uint8_t d[32];
    sha256_init(&s);
    sha256_update(&s, data, n);
    sha256_final(&s, d);
    sha256_hex(d, hex);
}

/* A release as make-release.sh lays it out. What can go wrong with it: */
enum { Z_GOOD, Z_BAD_HASH, Z_UNLISTED, Z_MISSING, Z_BAD_CRC, Z_ESCAPE, Z_NO_KERNEL };

static unsigned char g_big[300000];     /* "the player": several inflate rounds */

static void make_release(const char *path, const char *kernel, int how)
{
    struct { const char *name; const void *data; size_t n; } f[] = {
        {"sd/uImage", kernel, strlen(kernel)},
        {"sd/linux_cmdline.txt", "console=ttyS2,115200 quiet\n", 27},
        {"sd/update.upt", "bootloader", 10},
        {"sd/fathom/bin/fathom-player", g_big, sizeof g_big},
        {"sd/fathom/wifi/modules/brcmfmac.ko", "new module", 10},
        {"sd/fathom/assets/radio.txt", "FIP = https://example.org/fip\n", 30},
        {"INSTALL.md", "# Installing\n", 13},
    };
    int nf = (int)(sizeof f / sizeof f[0]);
    if (how == Z_NO_KERNEL) { f[0] = f[nf - 1]; nf--; }
    char sums[8192] = "", hex[65], line[600], name[300];
    for (int i = 0; i < nf; i++) {
        hex_of(f[i].data, f[i].n, hex);
        if (how == Z_BAD_HASH && !strcmp(f[i].name, "sd/fathom/wifi/modules/brcmfmac.ko")) hex[0] = hex[0] == '0' ? '1' : '0';
        if (how == Z_UNLISTED && !strcmp(f[i].name, "sd/update.upt")) continue;
        snprintf(line, sizeof line, "%s  ./%s\n", hex, f[i].name);
        strcat(sums, line);
    }
    if (how == Z_MISSING) strcat(sums, "0000000000000000000000000000000000000000000000000000000000000000  ./sd/fathom/gone\n");
    zip_begin(path);
    zip_add("fathom-h2-0.8.0/", "", 0, 0);
    zip_add("fathom-h2-0.8.0/sd/", "", 0, 0);
    zip_add("fathom-h2-0.8.0/sd/fathom/", "", 0, 0);
    zip_add("fathom-h2-0.8.0/sd/fathom/bin/", "", 0, 0);
    for (int i = 0; i < nf; i++) {
        snprintf(name, sizeof name, "fathom-h2-0.8.0/%s", f[i].name);
        zip_add(name, f[i].data, f[i].n, how == Z_BAD_CRC && i == 3);
    }
    if (how == Z_ESCAPE) zip_add("fathom-h2-0.8.0/sd/../../evil", "x", 1, 0);
    zip_add("fathom-h2-0.8.0/about/SHA256SUMS", sums, strlen(sums), 0);
    zip_end();
}

/* A card with Fathom 0.7.0 on it, and the owner's things. */
static void make_card(void)
{
    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf '%s/card' '%s/card/fathom/update'", g_tmp, g_tmp);
    if (system(cmd)) exit(1);
    put(at("card/uImage"), "old kernel");
    put(at("card/linux_cmdline.txt"), "old cmdline");
    put(at("card/fathom/bin/fathom-player"), "old player");
    put(at("card/fathom/wifi/modules/brcmfmac.ko"), "old module");
    put(at("card/fathom/wifi/networks.conf"), "network={ssid=\"home\"}");
    put(at("card/fathom/assets/radio.txt"), "old stations");
    put(at("card/fathom/assets/gone-in-0.8.txt"), "an old release's file");
    put(at("card/fathom/settings.ini"), "ui.theme=Studio Ember");
    put(at("card/fathom/radio.txt"), "My station = http://example.org/s");
    put(at("card/fathom/eq/Mine.txt"), "Preamp: -3 dB");
    put(at("card/fathom/keys/authorized_keys"), "ssh-ed25519 AAAA");
    put(at("card/fathom/playlists/Road.m3u8"), "#EXTM3U");
    put(at("card/Music/a.flac"), "music");
}

static void check_owner_things(void)
{
    CHECK(is(at("card/fathom/wifi/networks.conf"), "network={ssid=\"home\"}"));
    CHECK(is(at("card/fathom/settings.ini"), "ui.theme=Studio Ember"));
    CHECK(is(at("card/fathom/radio.txt"), "My station = http://example.org/s"));
    CHECK(is(at("card/fathom/eq/Mine.txt"), "Preamp: -3 dB"));
    CHECK(is(at("card/fathom/keys/authorized_keys"), "ssh-ed25519 AAAA"));
    CHECK(is(at("card/fathom/playlists/Road.m3u8"), "#EXTM3U"));
    CHECK(is(at("card/Music/a.flac"), "music"));
}

static int same_file(const char *path, const void *data, size_t n)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    unsigned char *b = malloc(n + 1);
    size_t k = fread(b, 1, n + 1, f);
    fclose(f);
    int same = k == n && !memcmp(b, data, n);
    free(b);
    return same;
}

static void check_installed(const char *kernel)
{
    CHECK(is(at("card/uImage"), kernel));
    CHECK(is(at("card/linux_cmdline.txt"), "console=ttyS2,115200 quiet\n"));
    CHECK(is(at("card/update.upt"), "bootloader"));
    CHECK(same_file(at("card/fathom/bin/fathom-player"), g_big, sizeof g_big));
    CHECK(is(at("card/fathom/wifi/modules/brcmfmac.ko"), "new module"));
    CHECK(is(at("card/fathom/assets/radio.txt"), "FIP = https://example.org/fip\n"));
    CHECK(is(at("card/fathom/assets/gone-in-0.8.txt"), "an old release's file"));   /* left, as by hand */
    CHECK(!exists(at("card/INSTALL.md")));                                          /* only sd/ */
    CHECK(!exists(at("card/fathom/update")));                                       /* the stage went */
    check_owner_things();
}

/* ---------- the tests ---------- */

static void test_versions(void)
{
    int v[3];
    CHECK(update_parse_version("v0.7.0", v) == 0 && v[0] == 0 && v[1] == 7 && v[2] == 0);
    CHECK(update_parse_version("0.12.3", v) == 0 && v[1] == 12 && v[2] == 3);
    CHECK(update_parse_version("v0.7.0-12-gabc1234", v) == 0 && v[1] == 7);
    CHECK(update_parse_version("v0.7.0-12-gabc1234-dirty", v) == 0);
    CHECK(update_parse_version("v0.7.0-dirty", v) == 0);
    CHECK(update_parse_version("v0.8.0-rc1", v) == 1 && v[1] == 8);
    CHECK(update_parse_version("unknown", v) == -1);
    CHECK(update_parse_version("abc1234", v) == -1);       /* git describe --always, no tag */
    CHECK(update_parse_version("v0.7", v) == -1);
    CHECK(update_parse_version("v0.7.0x", v) == -1);

    CHECK(update_is_newer("v0.8.0", "v0.7.0"));
    CHECK(update_is_newer("v0.7.1", "v0.7.0"));
    CHECK(update_is_newer("v1.0.0", "v0.12.9"));
    CHECK(update_is_newer("v0.10.0", "v0.9.0"));            /* numbers, not text */
    CHECK(!update_is_newer("v0.7.0", "v0.7.0"));
    CHECK(!update_is_newer("v0.6.2", "v0.7.0"));             /* never a downgrade */
    CHECK(!update_is_newer("v0.7.0", "v0.7.0-12-gabc1234"));  /* a dev build counts as its tag */
    CHECK(update_is_newer("v0.7.1", "v0.7.0-12-gabc1234-dirty"));
    CHECK(!update_is_newer("v0.8.0-rc1", "v0.7.0"));          /* pre-releases aren't offered */
    CHECK(update_is_newer("v0.8.0", "v0.8.0-rc1"));
    CHECK(update_is_newer("v0.8.0", "unknown"));
    CHECK(!update_is_newer("garbage", "v0.7.0"));

    /* automatic checks: once a day, a failed try again after an hour */
    long day = UPDATE_AUTO_EVERY, t = 1795000000;
    CHECK(update_auto_due(t, 0, 0));                          /* never checked */
    CHECK(!update_auto_due(t, t - 60, t - 60));               /* answered a minute ago */
    CHECK(!update_auto_due(t, t - day + 60, t - day + 60));
    CHECK(update_auto_due(t, t - day, t - day));
    CHECK(!update_auto_due(t, t - 2 * day, t - 600));         /* failed 10 min ago */
    CHECK(update_auto_due(t, t - 2 * day, t - UPDATE_AUTO_RETRY));
    CHECK(update_auto_due(t, t + 3600, 0));                   /* the clock went back */
    CHECK(update_auto_due(t, 0, t + 60));
}

static void test_release(void)
{
    /* GitHub's real answer for 0.7.0 */
    FILE *f = fopen("tests/data/github-release-latest.json", "rb");
    CHECK(f != NULL);
    if (f) {
        static char json[65536];
        size_t n = fread(json, 1, sizeof json - 1, f);
        fclose(f);
        update_release_t r;
        CHECK(update_parse_release(json, n, &r) == 0);
        CHECK(!strcmp(r.tag, "v0.7.0"));
        CHECK(!strcmp(r.asset, "fathom-h2-0.7.0.zip"));
        CHECK(r.size == 15689654);
        CHECK(!strcmp(r.url, "https://github.com/raparc0r/fathom-h2/releases/download/v0.7.0/fathom-h2-0.7.0.zip"));
        CHECK(!strcmp(r.sha256, "014fa7f8f8e2323c7873f23dd825cbd907d3a31893d1e57c2c95d6006c451236"));
        CHECK(!update_is_newer(r.tag, "v0.7.0"));
        CHECK(update_is_newer(r.tag, "v0.6.2"));
    }
    update_release_t r;
    const char *pre = "{\"tag_name\":\"v0.8.0\",\"prerelease\":true,\"assets\":[{\"name\":\"fathom-h2-0.8.0.zip\","
                      "\"size\":5,\"browser_download_url\":\"https://x/a.zip\"}]}";
    CHECK(update_parse_release(pre, strlen(pre), &r) == -1);
    const char *draft = "{\"tag_name\":\"v0.8.0\",\"draft\":true,\"assets\":[{\"name\":\"fathom-h2-0.8.0.zip\","
                        "\"size\":5,\"browser_download_url\":\"https://x/a.zip\"}]}";
    CHECK(update_parse_release(draft, strlen(draft), &r) == -1);
    const char *nozip = "{\"tag_name\":\"v0.8.0\",\"assets\":[{\"name\":\"notes.txt\",\"size\":5,"
                        "\"browser_download_url\":\"https://x/notes.txt\"}]}";
    CHECK(update_parse_release(nozip, strlen(nozip), &r) == -1 && !strcmp(r.tag, "v0.8.0"));   /* (tag kept) */
    /* the exact name wins over another fathom-h2 zip; no digest is fine over https */
    const char *two = "{\"tag_name\":\"v0.8.0\",\"assets\":["
                      "{\"name\":\"fathom-h2-0.8.0-extras.zip\",\"size\":7,\"browser_download_url\":\"https://x/e.zip\"},"
                      "{\"name\":\"fathom-h2-0.8.0.zip\",\"size\":9,\"state\":\"uploaded\",\"browser_download_url\":\"https://x/r.zip\"}]}";
    CHECK(update_parse_release(two, strlen(two), &r) == 0 && r.size == 9 && !strcmp(r.url, "https://x/r.zip") && !r.sha256[0]);
    const char *uploading = "{\"tag_name\":\"v0.8.0\",\"assets\":[{\"name\":\"fathom-h2-0.8.0.zip\",\"size\":9,"
                            "\"state\":\"starter\",\"browser_download_url\":\"https://x/r.zip\"}]}";
    CHECK(update_parse_release(uploading, strlen(uploading), &r) == -1);
    /* plain http only with a digest (a test server) */
    const char *http = "{\"tag_name\":\"v0.8.0\",\"assets\":[{\"name\":\"fathom-h2-0.8.0.zip\",\"size\":9,"
                       "\"browser_download_url\":\"http://10.42.0.2:8000/r.zip\"}]}";
    CHECK(update_parse_release(http, strlen(http), &r) == -1);
    const char *http_d = "{\"tag_name\":\"v0.8.0\",\"assets\":[{\"name\":\"fathom-h2-0.8.0.zip\",\"size\":9,"
                         "\"digest\":\"sha256:014FA7F8F8E2323C7873F23DD825CBD907D3A31893D1E57C2C95D6006C451236\","
                         "\"browser_download_url\":\"http://10.42.0.2:8000/r.zip\"}]}";
    CHECK(update_parse_release(http_d, strlen(http_d), &r) == 0 &&
          !strcmp(r.sha256, "014fa7f8f8e2323c7873f23dd825cbd907d3a31893d1e57c2c95d6006c451236"));
    /* OTA_fathom-h2-VERSION (the same zip, for this updater) wins over the zip,
     * listed before or after it; one being uploaded still, or another
     * version's, is passed over for the zip */
    const char *ota = "{\"tag_name\":\"v0.13.0\",\"assets\":["
                      "{\"name\":\"fathom-h2-0.13.0.zip\",\"size\":9,\"browser_download_url\":\"https://x/r.zip\"},"
                      "{\"name\":\"OTA_fathom-h2-0.13.0\",\"size\":9,\"state\":\"uploaded\",\"browser_download_url\":\"https://x/OTA_r\"}]}";
    CHECK(update_parse_release(ota, strlen(ota), &r) == 0 && !strcmp(r.asset, "OTA_fathom-h2-0.13.0") && !strcmp(r.url, "https://x/OTA_r"));
    const char *ota_first = "{\"tag_name\":\"v0.13.0\",\"assets\":["
                            "{\"name\":\"OTA_fathom-h2-0.13.0\",\"size\":9,\"browser_download_url\":\"https://x/OTA_r\"},"
                            "{\"name\":\"fathom-h2-0.13.0.zip\",\"size\":9,\"browser_download_url\":\"https://x/r.zip\"}]}";
    CHECK(update_parse_release(ota_first, strlen(ota_first), &r) == 0 && !strcmp(r.asset, "OTA_fathom-h2-0.13.0"));
    const char *ota_up = "{\"tag_name\":\"v0.13.0\",\"assets\":["
                         "{\"name\":\"OTA_fathom-h2-0.13.0\",\"size\":9,\"state\":\"starter\",\"browser_download_url\":\"https://x/OTA_r\"},"
                         "{\"name\":\"fathom-h2-0.13.0.zip\",\"size\":9,\"browser_download_url\":\"https://x/r.zip\"}]}";
    CHECK(update_parse_release(ota_up, strlen(ota_up), &r) == 0 && !strcmp(r.asset, "fathom-h2-0.13.0.zip"));
    const char *ota_old = "{\"tag_name\":\"v0.13.0\",\"assets\":["
                          "{\"name\":\"OTA_fathom-h2-0.12.0\",\"size\":9,\"browser_download_url\":\"https://x/OTA_o\"},"
                          "{\"name\":\"fathom-h2-0.13.0.zip\",\"size\":9,\"browser_download_url\":\"https://x/r.zip\"}]}";
    CHECK(update_parse_release(ota_old, strlen(ota_old), &r) == 0 && !strcmp(r.asset, "fathom-h2-0.13.0.zip"));
    CHECK(update_parse_release("[]", 2, &r) == -1);
    CHECK(update_parse_release("{\"tag_name\":", 12, &r) == -1);
}

static void test_sha256(void)
{
    char hex[65];
    hex_of("", 0, hex);
    CHECK(!strcmp(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    hex_of("abc", 3, hex);
    CHECK(!strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    const char *two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";   /* 56 bytes: two blocks of padding */
    hex_of(two, strlen(two), hex);
    CHECK(!strcmp(hex, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
    /* in pieces, as a stream arrives */
    sha256_t s;
    uint8_t d[32];
    char a[65];
    sha256_init(&s);
    for (size_t i = 0; i < sizeof g_big; i += 1000) sha256_update(&s, g_big + i, sizeof g_big - i < 1000 ? sizeof g_big - i : 1000);
    sha256_final(&s, d);
    sha256_hex(d, a);
    hex_of(g_big, sizeof g_big, hex);
    CHECK(!strcmp(a, hex));
    FILE *f = fopen(at("big"), "wb");
    fwrite(g_big, 1, sizeof g_big, f);
    fclose(f);
    CHECK(update_file_sha256(at("big"), a, NULL, NULL, NULL) == 0 && !strcmp(a, hex));
}

static void test_zip(void)
{
    make_release(at("r.zip"), "new kernel", Z_GOOD);
    zip_t z;
    zip_entry_t e;
    CHECK(zip_open(&z, at("r.zip")) == 0);
    int n = 0, dirs = 0;
    while (zip_next(&z, &e) == 1) { n++; dirs += e.dir; }
    CHECK(n == 12 && dirs == 4);
    zip_close(&z);
    put(at("notzip"), "PK but not really a zip file at all, no end record");
    CHECK(zip_open(&z, at("notzip")) == -1);
}

static void test_stage_and_install(void)
{
    char stage[1024];
    snprintf(stage, sizeof stage, "%s", at("card/fathom/update"));
    make_card();
    make_release(at("r.zip"), "new kernel", Z_GOOD);
    long cluster = 0;
    CHECK(update_free_space(at("card"), &cluster) > 0 && cluster > 0);
    int64_t need = update_stage_needs(at("r.zip"), 32768, 4 << 20);
    CHECK(need >= (int64_t)sizeof g_big + (4 << 20));
    mkdir(stage, 0755);
    CHECK(update_stage(at("r.zip"), stage, NULL, NULL, NULL) == UPDATE_OK);
    CHECK(is(at("card/fathom/update/READY"), "fathom-h2-0.8.0"));
    CHECK(is(at("card/fathom/update/sd/uImage"), "new kernel"));
    CHECK(!exists(at("card/fathom/update/sd/INSTALL.md")));
    CHECK(is(at("card/uImage"), "old kernel"));                  /* nothing on the card yet */
    CHECK(update_install(at("card"), stage) == UPDATE_OK);
    check_installed("new kernel");
    CHECK(is(at("card/uImage.prev"), "old kernel"));
    CHECK(is(at("card/fathom/wifi/modules.prev/brcmfmac.ko"), "old module"));   /* with the old kernel */
    CHECK(exists(at("card/fathom/boot-new")));                   /* for h2boot: a kernel to try */
    CHECK(update_resume(at("card"), stage) == 0);                /* nothing left to do */

    /* A second update: uImage.prev is the one before it */
    make_release(at("r.zip"), "newer kernel", Z_GOOD);
    mkdir(stage, 0755);
    CHECK(update_stage(at("r.zip"), stage, NULL, NULL, NULL) == UPDATE_OK);
    CHECK(update_install(at("card"), stage) == UPDATE_OK);
    CHECK(is(at("card/uImage"), "newer kernel") && is(at("card/uImage.prev"), "new kernel"));
    CHECK(is(at("card/fathom/wifi/modules.prev/brcmfmac.ko"), "new module"));
    /* after a fallback (h2boot): the failed kernel and modules go at the next install */
    put(at("card/uImage.failed"), "a kernel that didn't start");
    mkdir(at("card/fathom/wifi/modules.failed"), 0755);
    put(at("card/fathom/wifi/modules.failed/brcmfmac.ko"), "its module");
    make_release(at("r.zip"), "fixed kernel", Z_GOOD);
    mkdir(stage, 0755);
    CHECK(update_stage(at("r.zip"), stage, NULL, NULL, NULL) == UPDATE_OK);
    CHECK(update_install(at("card"), stage) == UPDATE_OK);
    CHECK(!exists(at("card/uImage.failed")) && !exists(at("card/fathom/wifi/modules.failed")));
}

static void test_damaged(void)
{
    static const int kinds[] = {Z_BAD_HASH, Z_UNLISTED, Z_MISSING, Z_BAD_CRC, Z_ESCAPE, Z_NO_KERNEL};
    static const int want[] = {UPDATE_ERR_DAMAGED, UPDATE_ERR_DAMAGED, UPDATE_ERR_DAMAGED, UPDATE_ERR_DAMAGED,
                               UPDATE_ERR_ZIP, UPDATE_ERR_DAMAGED};
    char stage[1024];
    snprintf(stage, sizeof stage, "%s", at("card/fathom/update"));
    for (size_t i = 0; i < sizeof kinds / sizeof kinds[0]; i++) {
        make_card();
        make_release(at("bad.zip"), "new kernel", kinds[i]);
        mkdir(stage, 0755);
        int r = update_stage(at("bad.zip"), stage, NULL, NULL, NULL);
        if (r != want[i]) fprintf(stderr, "damaged zip %zu: %d, not %d\n", i, r, want[i]);
        CHECK(r == want[i]);
        CHECK(!exists(at("card/fathom/update/READY")));
        CHECK(update_install(at("card"), stage) == UPDATE_ERR_INSTALL);   /* not ready: refused */
        CHECK(is(at("card/uImage"), "old kernel") && is(at("card/fathom/bin/fathom-player"), "old player"));
        CHECK(!exists(at("evil")) && !exists(at("card/evil")));
        update_remove_tree(stage);
    }
    /* cancelled while unpacking */
    make_card();
    make_release(at("r.zip"), "new kernel", Z_GOOD);
    mkdir(stage, 0755);
    volatile int stop = 1;
    CHECK(update_stage(at("r.zip"), stage, NULL, NULL, &stop) == UPDATE_ERR_CANCELLED);
    CHECK(!exists(at("card/fathom/update/READY")));
}

/* Power lost during an install, at three points: the next start finishes it. */
static void test_resume(void)
{
    char stage[1024];
    snprintf(stage, sizeof stage, "%s", at("card/fathom/update"));
    /* 1. right after it began */
    make_card();
    make_release(at("r.zip"), "new kernel", Z_GOOD);
    mkdir(stage, 0755);
    CHECK(update_stage(at("r.zip"), stage, NULL, NULL, NULL) == UPDATE_OK);
    put(at("card/fathom/update/INSTALLING"), "installing\n");
    CHECK(update_resume(at("card"), stage) == 1);
    check_installed("new kernel");
    CHECK(is(at("card/uImage.prev"), "old kernel"));

    /* 2. halfway: the player and the modules in, the kernel not */
    make_card();
    mkdir(stage, 0755);
    CHECK(update_stage(at("r.zip"), stage, NULL, NULL, NULL) == UPDATE_OK);
    put(at("card/fathom/update/INSTALLING"), "installing\n");
    CHECK(rename(at("card/fathom/update/sd/fathom/bin/fathom-player"), at("card/fathom/bin/fathom-player")) == 0);
    CHECK(rename(at("card/fathom/update/sd/fathom/wifi/modules/brcmfmac.ko"), at("card/fathom/wifi/modules/brcmfmac.ko")) == 0);
    CHECK(update_resume(at("card"), stage) == 1);
    check_installed("new kernel");
    CHECK(is(at("card/uImage.prev"), "old kernel"));

    /* 3. the kernel in, the stage not yet removed: uImage.prev must stay the old one */
    make_card();
    mkdir(stage, 0755);
    CHECK(update_stage(at("r.zip"), stage, NULL, NULL, NULL) == UPDATE_OK);
    put(at("card/fathom/update/INSTALLING"), "installing\n");
    char cmd[1200];
    snprintf(cmd, sizeof cmd, "cd '%s/card/fathom/update/sd' && find . -type f ! -name uImage | while read f; do "
             "mkdir -p \"../../../$(dirname \"$f\")\" && mv \"$f\" \"../../../$f\"; done", g_tmp);
    CHECK(system(cmd) == 0);
    put(at("card/uImage.prev"), "old kernel");                    /* as the install copies it */
    CHECK(rename(at("card/fathom/update/sd/uImage"), at("card/uImage")) == 0);
    CHECK(update_resume(at("card"), stage) == 1);
    check_installed("new kernel");
    CHECK(is(at("card/uImage.prev"), "old kernel"));               /* not the new one */

    /* 4. the stage's sd/ removed, INSTALLING not yet */
    make_card();
    mkdir(stage, 0755);
    put(at("card/fathom/update/INSTALLING"), "installing\n");
    CHECK(update_resume(at("card"), stage) == 1);
    CHECK(!exists(stage) && is(at("card/uImage"), "old kernel"));

    /* A download left unfinished (no INSTALLING): removed, nothing installed */
    make_card();
    mkdir(stage, 0755);
    CHECK(update_stage(at("r.zip"), stage, NULL, NULL, NULL) == UPDATE_OK);
    put(at("card/fathom/update/fathom-h2-0.8.0.zip.part"), "half");
    CHECK(update_resume(at("card"), stage) == 0);
    CHECK(!exists(stage) && is(at("card/uImage"), "old kernel"));
}

/* A real release, when one is given: every file of its sd/ checks out. */
static void test_real_release(void)
{
    const char *zip = getenv("FATHOM_TEST_RELEASE_ZIP");
    if (!zip || !*zip) return;
    char stage[1024];
    snprintf(stage, sizeof stage, "%s", at("real"));
    mkdir(stage, 0755);
    long cluster = 32768;
    int64_t need = update_stage_needs(zip, cluster, 4355866);
    int r = update_stage(zip, stage, NULL, NULL, NULL);
    printf("real release %s: needs %.1f MB, staged: %s\n", zip, need / 1048576.0, r ? update_error_text(r) : "ok");
    CHECK(need > 0 && r == UPDATE_OK);
    update_remove_tree(stage);
}

int main(void)
{
    for (size_t i = 0; i < sizeof g_big; i++) g_big[i] = (unsigned char)((i * 2654435761u) >> 13 ^ (i % 251 ? 0 : 7));
    snprintf(g_tmp, sizeof g_tmp, "%s/fathom-update-XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    if (!mkdtemp(g_tmp)) { perror("mkdtemp"); return 1; }
    test_versions();
    test_release();
    test_sha256();
    test_zip();
    test_stage_and_install();
    test_damaged();
    test_resume();
    test_real_release();
    update_remove_tree(g_tmp);
    if (failures) { fprintf(stderr, "%d failures\n", failures); return 1; }
    printf("update: ok\n");
    return 0;
}
