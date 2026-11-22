/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/*
 * HiBy's update files (app/sys/hibyupt.c, md5.c), on images made here in
 * HiBy's layout (ISO 9660 names "UBOOT.BIN;1", Rock Ridge-like extra bytes, no
 * Joliet), in mkstockuboot's (with a Joliet tree too) and in Fathom's
 * installer's: reading, checking, patching, patching twice, undoing, and
 * refusing what it shouldn't touch.
 *
 * FATHOM_TEST_UPT=H2-Firmware-V2.3.upt also patches a copy of a real one; with
 * FATHOM_TEST_UPT_PATCHED=uninstall-v2.3/update.upt (Rockbox's mkstockuboot on
 * the same file), its update.txt must come out the same. (Never committed:
 * HiBy's files aren't ours to share.)
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "sys/hibyupt.h"
#include "sys/md5.h"
#include "sys/uninstall.h"
#include "tmpdir.h"

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

#define S 2048u

static char g_dir[512];

static void path_of(char *out, size_t cap, const char *name) { snprintf(out, cap, "%s/%s", g_dir, name); }

static void md5_of(const void *data, size_t len, char hex[33])
{
    md5_t m;
    uint8_t d[16];
    md5_init(&m);
    md5_update(&m, data, len);
    md5_final(&m, d);
    md5_hex(d, hex);
}

/* The whole file's MD5, to see whether anything changed. */
static void md5_file(const char *path, char hex[33])
{
    static uint8_t buf[65536];
    md5_t m;
    uint8_t d[16];
    md5_init(&m);
    FILE *f = fopen(path, "rb");
    size_t n;
    if (f) {
        while ((n = fread(buf, 1, sizeof buf, f)) > 0) md5_update(&m, buf, n);
        fclose(f);
    }
    md5_final(&m, d);
    md5_hex(d, hex);
}

static int exists(const char *path)
{
    struct stat st;
    return !stat(path, &st);
}

/* ---------- making images ---------- */

typedef struct {
    const char *iso, *joliet;           /* names in each tree */
    const uint8_t *data;
    uint32_t size;
} tfile_t;

static void both32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++) { p[i] = (uint8_t)(v >> (8 * i)); p[7 - i] = (uint8_t)(v >> (8 * i)); }
}

/* A directory record; rr adds bytes after the name, as Rock Ridge's do. */
static size_t record(uint8_t *p, uint32_t lba, uint32_t size, int dir, const uint8_t *name, size_t nlen, int rr)
{
    size_t len = 33 + nlen;
    if (len & 1) len++;
    size_t extra = rr ? 12 : 0;
    memset(p, 0, len + extra);
    p[0] = (uint8_t)(len + extra);
    both32(p + 2, lba);
    both32(p + 10, size);
    p[25] = dir ? 2 : 0;
    p[28] = 1;
    p[32] = (uint8_t)nlen;
    memcpy(p + 33, name, nlen);
    if (rr) memcpy(p + len, "NM\x0c\x01\x00test.bin", 12);
    return len + extra;
}

static void descriptor(uint8_t *vd, int type, uint32_t blocks, uint32_t root_lba)
{
    memset(vd, 0, S);
    vd[0] = (uint8_t)type;
    memcpy(vd + 1, "CD001", 5);
    vd[6] = 1;
    if (type == 255) return;
    both32(vd + 80, blocks);
    vd[128] = S & 255; vd[129] = S >> 8; vd[130] = S >> 8; vd[131] = S & 255;
    if (type == 2) memcpy(vd + 88, "%/E", 3);
    uint8_t dot = 0;
    record(vd + 156, root_lba, S, 1, &dot, 1, 0);
}

static void root_dir(uint8_t *sec, uint32_t self, const tfile_t *files, const uint32_t *lba, int n, int joliet, int rr)
{
    memset(sec, 0, S);
    uint8_t dot = 0, dotdot = 1;
    size_t at = record(sec, self, S, 1, &dot, 1, 0);
    at += record(sec + at, self, S, 1, &dotdot, 1, 0);
    for (int i = 0; i < n; i++) {
        uint8_t name[64];
        size_t nlen = 0;
        const char *s = joliet ? files[i].joliet : files[i].iso;
        for (; *s; s++) {
            if (joliet) name[nlen++] = 0;
            name[nlen++] = (uint8_t)*s;
        }
        at += record(sec + at, lba[i], files[i].size, 0, name, nlen, rr);
    }
}

static void make_iso(const char *path, const tfile_t *files, int n, int joliet)
{
    uint32_t next = 16 + 1 + (joliet ? 1 : 0) + 1;       /* the descriptors */
    uint32_t root1 = next++, root2 = joliet ? next++ : 0;
    uint32_t lba[16];
    for (int i = 0; i < n; i++) {
        lba[i] = next;
        next += (files[i].size + S - 1) / S;
        if (!files[i].size) next++;
    }
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    uint8_t sec[S] = {0};
    for (int i = 0; i < 16; i++) fwrite(sec, 1, S, f);
    descriptor(sec, 1, next, root1);
    fwrite(sec, 1, S, f);
    if (joliet) { descriptor(sec, 2, next, root2); fwrite(sec, 1, S, f); }
    descriptor(sec, 255, 0, 0);
    fwrite(sec, 1, S, f);
    root_dir(sec, root1, files, lba, n, 0, !joliet);
    fwrite(sec, 1, S, f);
    if (joliet) { root_dir(sec, root2, files, lba, n, 1, 0); fwrite(sec, 1, S, f); }
    for (int i = 0; i < n; i++) {
        fwrite(files[i].data, 1, files[i].size, f);
        uint32_t pad = (S - files[i].size % S) % S;
        if (!files[i].size) pad = S;
        memset(sec, 0, S);
        fwrite(sec, 1, pad, f);
    }
    fclose(f);
}

/* ---------- a HiBy-like update ---------- */

static uint8_t g_uboot[100 * 1024], g_kernel[70 * 1024], g_rootfs[200 * 1024];
static char g_manifest[S * 2], g_version[256];
static const char BANNER[] = "U-Boot SPL 2013.07 (Nov 11 2025 - 18:57:13)";

static void fill(uint8_t *p, size_t n, uint32_t seed)
{
    for (size_t i = 0; i < n; i++) { seed = seed * 1103515245 + 12345; p[i] = (uint8_t)(seed >> 16); }
}

static void make_images(void)
{
    fill(g_uboot, sizeof g_uboot, 1);
    fill(g_kernel, sizeof g_kernel, 2);
    fill(g_rootfs, sizeof g_rootfs, 3);
    static const uint8_t head[8] = {0x06, 0x05, 0x04, 0x03, 0x02, 0x55, 0xaa, 0x55};
    memcpy(g_uboot, head, 8);
    /* the banner across the first 32 KB read's end */
    memcpy(g_uboot + 32 * 1024 - 20, BANNER, sizeof BANNER);
}

/* HiBy's update.txt (kernel, rootfs); extra pads it, to make it too long to patch. */
static void make_manifest(int with_uboot, int extra)
{
    char k[33], r[33], u[33];
    md5_of(g_kernel, sizeof g_kernel, k);
    md5_of(g_rootfs, sizeof g_rootfs, r);
    md5_of(g_uboot, sizeof g_uboot, u);
    int n = 0;
    if (with_uboot)
        n += snprintf(g_manifest + n, sizeof g_manifest - (size_t)n,
                      "bootloader={\n        name=uboot\n        file_path=autoupdate/uboot.bin\n        md5=%s\n}\n", u);
    n += snprintf(g_manifest + n, sizeof g_manifest - (size_t)n,
                  "kernel={\n        name=kernel\n        file_path=autoupdate/uimage.bin\n        md5=%s\n}\n\n"
                  "rootfs={\n        name=rootfs\n        full_upgrade=yes\n        file_path=autoupdate/system.ubi\n"
                  "        md5=%s\n}\n\n", k, r);
    for (int i = 0; i < extra; i++) g_manifest[n++] = '\n';
    g_manifest[n] = '\0';
}

static void make_version(const char *model)
{
    snprintf(g_version, sizeof g_version, "\n\nversion={\n        name=%s\n        ver=2025-11-11T18:59:20+08:00\n}\n", model);
}

static void make_hiby(const char *path, int joliet, int with_uboot_file)
{
    tfile_t files[] = {
        {"SYSTEM.UBI;1", "system.ubi", g_rootfs, sizeof g_rootfs},
        {"UBOOT.BIN;1", "uboot.bin", g_uboot, sizeof g_uboot},
        {"UIMAGE.BIN;1", "uimage.bin", g_kernel, sizeof g_kernel},
        {"UPDATE.TXT;1", "update.txt", (const uint8_t *)g_manifest, (uint32_t)strlen(g_manifest)},
        {"VERSION.TXT;1", "version.txt", (const uint8_t *)g_version, (uint32_t)strlen(g_version)},
        {"_GITIGNO.;1", "_gitignore", (const uint8_t *)"*.*\n", 4},
    };
    if (!with_uboot_file) {
        files[1] = files[5];
        make_iso(path, files, 5, joliet);
        return;
    }
    make_iso(path, files, 6, joliet);
}

/* update.txt as the file now holds it, through a fresh open. */
static int manifest_now(const char *path, char *out, size_t cap, upt_kind_t *kind)
{
    upt_t u;
    upt_err_t e = upt_open(&u, path, 0);
    if (!e) {
        snprintf(out, cap, "%s", u.manifest);
        if (kind) *kind = u.kind;
    }
    upt_close(&u);
    return e;
}

/* ---------- tests ---------- */

static void test_md5(void)
{
    char h[33];
    md5_of("", 0, h);
    CHECK(!strcmp(h, "d41d8cd98f00b204e9800998ecf8427e"));
    md5_of("abc", 3, h);
    CHECK(!strcmp(h, "900150983cd24fb0d6963f7d28e17f72"));
    md5_of("The quick brown fox jumps over the lazy dog", 43, h);
    CHECK(!strcmp(h, "9e107d9d372bb6826bd81d3542a419d6"));
    /* 1-127 bytes, whole and a byte at a time: the padding's edges */
    static uint8_t b[128];
    fill(b, sizeof b, 9);
    for (size_t n = 1; n < sizeof b; n++) {
        char a1[33], a2[33];
        md5_t m;
        uint8_t d[16];
        md5_of(b, n, a1);
        md5_init(&m);
        for (size_t i = 0; i < n; i++) md5_update(&m, b + i, 1);
        md5_final(&m, d);
        md5_hex(d, a2);
        CHECK(!strcmp(a1, a2));
    }
    md5_of("12345678901234567890123456789012345678901234567890123456789012345678901234567890", 80, h);
    CHECK(!strcmp(h, "57edf4a22be3c955ac49da2e2107b67a"));
}

static void test_hiby(int joliet)
{
    char path[600], undo[600], before[33], after[33], text[S + 1];
    path_of(path, sizeof path, joliet ? "joliet.upt" : "hiby.upt");
    path_of(undo, sizeof undo, "undo");
    make_manifest(0, 0);
    make_version("erosq_2024");
    make_hiby(path, joliet, 1);
    md5_file(path, before);

    upt_t u;
    CHECK(upt_open(&u, path, 1) == UPT_OK);
    CHECK(u.kind == UPT_HIBY);
    CHECK(!strcmp(u.model, "erosq_2024"));
    CHECK(!strcmp(u.ver, "2025-11-11T18:59:20+08:00"));
    CHECK(u.nentries == 2);
    CHECK(upt_file(&u, "uboot.bin") && upt_file(&u, "uboot.bin")->nrec == (joliet ? 2 : 1));
    CHECK(upt_patch(&u, undo) == UPT_ERR_NOT_UPDATE);     /* not checked yet */
    CHECK(upt_check(&u, NULL, NULL) == UPT_OK);
    char um[33];
    md5_of(g_uboot, sizeof g_uboot, um);
    CHECK(!strcmp(u.uboot_md5, um));
    CHECK(!strcmp(u.uboot_built, "Nov 11 2025 - 18:57:13"));

    CHECK(upt_patch(&u, undo) == UPT_OK);
    CHECK(u.kind == UPT_HIBY_PATCHED);
    CHECK(exists(undo));
    upt_close(&u);

    make_manifest(1, 0);                                  /* what mkstockuboot makes */
    upt_kind_t kind = UPT_HIBY;
    CHECK(manifest_now(path, text, sizeof text, &kind) == UPT_OK);
    CHECK(!strcmp(text, g_manifest));
    CHECK(kind == UPT_HIBY_PATCHED);

    /* a patched file checks, and patching again changes nothing */
    md5_file(path, after);
    CHECK(strcmp(before, after));
    CHECK(upt_open(&u, path, 1) == UPT_OK);
    CHECK(upt_check(&u, NULL, NULL) == UPT_OK);
    CHECK(upt_patch(&u, undo) == UPT_OK);
    upt_close(&u);
    char again[33];
    md5_file(path, again);
    CHECK(!strcmp(after, again));

    /* undo: the file as it was, the undo file gone */
    CHECK(upt_undo(path, undo) == UPT_OK);
    md5_file(path, after);
    CHECK(!strcmp(before, after));
    CHECK(!exists(undo));
    CHECK(upt_undo(path, undo) == UPT_ERR_UNDO);          /* nothing left to undo */
    unlink(path);
}

/* Cut short between the text and its length: it doesn't open; undo mends it. */
static void test_cut_short(void)
{
    char path[600], undo[600], before[33], after[33];
    path_of(path, sizeof path, "cut.upt");
    path_of(undo, sizeof undo, "undo");
    make_manifest(0, 0);
    make_version("erosq_2024");
    make_hiby(path, 0, 1);
    md5_file(path, before);
    upt_t u;
    CHECK(upt_open(&u, path, 1) == UPT_OK && upt_check(&u, NULL, NULL) == UPT_OK);
    const upt_file_t *f = upt_file(&u, "update.txt");
    uint64_t rec = f->rec[0];
    uint32_t old = f->size;
    CHECK(upt_patch(&u, undo) == UPT_OK);
    upt_close(&u);
    /* the length as it was, the new text in place */
    int fd = open(path, O_RDWR);
    uint8_t v[8];
    both32(v, old);
    CHECK(pwrite(fd, v, 8, (off_t)rec + 10) == 8);
    close(fd);
    CHECK(upt_open(&u, path, 0) != UPT_OK);
    upt_close(&u);
    CHECK(upt_undo(path, undo) == UPT_OK);
    md5_file(path, after);
    CHECK(!strcmp(before, after));
    unlink(path);
}

static void test_refusals(void)
{
    char path[600], undo[600], before[33], after[33];
    path_of(path, sizeof path, "bad.upt");
    path_of(undo, sizeof undo, "undo");
    upt_t u;

    CHECK(upt_open(&u, "/nonexistent/update.upt", 0) == UPT_ERR_OPEN);
    upt_close(&u);

    /* not an ISO */
    FILE *f = fopen(path, "wb");
    fwrite(g_kernel, 1, sizeof g_kernel, f);
    fclose(f);
    CHECK(upt_open(&u, path, 0) == UPT_ERR_NOT_UPDATE);
    upt_close(&u);

    /* another player */
    make_manifest(0, 0);
    make_version("erosq");
    make_hiby(path, 0, 1);
    CHECK(upt_open(&u, path, 0) == UPT_ERR_MODEL);
    upt_close(&u);
    make_version("erosq_2024");

    /* cut short */
    make_hiby(path, 0, 1);
    struct stat st;
    stat(path, &st);
    CHECK(!truncate(path, st.st_size - S));
    CHECK(upt_open(&u, path, 0) == UPT_ERR_DAMAGED);
    upt_close(&u);

    /* an image that doesn't match its MD5 */
    g_kernel[1000] ^= 1;
    make_hiby(path, 0, 1);
    g_kernel[1000] ^= 1;
    CHECK(upt_open(&u, path, 0) == UPT_OK);
    CHECK(upt_check(&u, NULL, NULL) == UPT_ERR_DAMAGED);
    upt_close(&u);

    /* no uboot.bin; one without the boot header; one that isn't HiBy's U-Boot */
    make_hiby(path, 0, 0);
    CHECK(upt_open(&u, path, 0) == UPT_OK);
    CHECK(upt_check(&u, NULL, NULL) == UPT_ERR_NO_BOOTLOADER);
    upt_close(&u);
    g_uboot[0] = 0;
    make_hiby(path, 0, 1);
    g_uboot[0] = 6;
    CHECK(upt_open(&u, path, 0) == UPT_OK);
    CHECK(upt_check(&u, NULL, NULL) == UPT_ERR_NO_BOOTLOADER);
    upt_close(&u);
    memcpy(g_uboot + 32 * 1024 - 20, "Rockbox", 7);
    make_hiby(path, 0, 1);
    CHECK(upt_open(&u, path, 0) == UPT_OK);
    CHECK(upt_check(&u, NULL, NULL) == UPT_ERR_NO_BOOTLOADER);
    upt_close(&u);
    memcpy(g_uboot + 32 * 1024 - 20, BANNER, sizeof BANNER);

    /* a list too long for the bootloader's entry to fit its sector: left alone */
    make_manifest(0, 0);
    int base = (int)strlen(g_manifest);
    make_manifest(0, (int)S - 118 - base + 1);
    make_hiby(path, 0, 1);
    md5_file(path, before);
    CHECK(upt_open(&u, path, 1) == UPT_OK);
    CHECK(upt_check(&u, NULL, NULL) == UPT_OK);
    CHECK(upt_patch(&u, undo) == UPT_ERR_LAYOUT);
    upt_close(&u);
    md5_file(path, after);
    CHECK(!strcmp(before, after));
    CHECK(!exists(undo));
    /* one byte less fits exactly */
    make_manifest(0, (int)S - 118 - base);
    make_hiby(path, 0, 1);
    CHECK(upt_open(&u, path, 1) == UPT_OK);
    CHECK(upt_check(&u, NULL, NULL) == UPT_OK);
    CHECK(upt_patch(&u, undo) == UPT_OK);
    CHECK(upt_file(&u, "update.txt")->size == S);
    upt_close(&u);
    CHECK(upt_undo(path, undo) == UPT_OK);

    /* read-only: not written */
    make_manifest(0, 0);
    make_hiby(path, 0, 1);
    CHECK(upt_open(&u, path, 0) == UPT_OK);
    CHECK(upt_check(&u, NULL, NULL) == UPT_OK);
    CHECK(upt_patch(&u, undo) == UPT_ERR_WRITE);
    upt_close(&u);
    CHECK(!exists(undo));

    /* an undo file for another file */
    char other[600];
    path_of(other, sizeof other, "other.upt");
    CHECK(upt_open(&u, path, 1) == UPT_OK && upt_check(&u, NULL, NULL) == UPT_OK);
    CHECK(upt_patch(&u, undo) == UPT_OK);
    upt_close(&u);
    tfile_t lone[] = {{"README.TXT;1", "readme.txt", (const uint8_t *)"hello\n", 6}};
    make_iso(other, lone, 1, 0);
    CHECK(upt_undo(other, undo) == UPT_ERR_UNDO);
    CHECK(exists(undo));
    CHECK(upt_undo(path, undo) == UPT_OK);
    unlink(other);

    /* a list that isn't one */
    snprintf(g_manifest, sizeof g_manifest, "kernel={\n name=kernel\n");
    make_hiby(path, 0, 1);
    CHECK(upt_open(&u, path, 0) == UPT_ERR_NOT_UPDATE);
    upt_close(&u);
    unlink(path);
}

/* Fathom's installer (tools/release/make-upt.sh): a bootloader only. */
static void make_fathom(const char *path)
{
    char m[33];
    md5_of(g_uboot, sizeof g_uboot, m);
    snprintf(g_manifest, sizeof g_manifest,
             "bootloader={\n        name=uboot\n        file_path=autoupdate/uboot.bin\n        md5=%s\n}\n\n", m);
    make_version("erosq_2024");
    tfile_t files[] = {
        {"UBOOT.BIN", "uboot.bin", g_uboot, sizeof g_uboot},
        {"UPDATE.TXT", "update.txt", (const uint8_t *)g_manifest, (uint32_t)strlen(g_manifest)},
        {"VERSION.TXT", "version.txt", (const uint8_t *)g_version, (uint32_t)strlen(g_version)},
    };
    make_iso(path, files, 3, 1);
}

/* Checked, never patched. */
static void test_fathom(void)
{
    char path[600], undo[600], m[33];
    path_of(path, sizeof path, "fathom.upt");
    path_of(undo, sizeof undo, "undo");
    md5_of(g_uboot, sizeof g_uboot, m);
    make_fathom(path);
    upt_t u;
    CHECK(upt_open(&u, path, 1) == UPT_OK);
    CHECK(u.kind == UPT_FATHOM);
    CHECK(upt_check(&u, NULL, NULL) == UPT_OK);
    CHECK(!strcmp(u.uboot_md5, m));
    CHECK(upt_patch(&u, undo) == UPT_ERR_NOT_UPDATE);
    upt_close(&u);
    CHECK(!exists(undo));
    unlink(path);
}

static void progress(void *ctx, uint64_t done, uint64_t total)
{
    uint64_t *p = ctx;
    CHECK(done >= p[0] && done <= total);
    p[0] = done;
    p[1] = total;
}

static void test_progress(void)
{
    char path[600];
    path_of(path, sizeof path, "progress.upt");
    make_manifest(0, 0);
    make_version("erosq_2024");
    make_hiby(path, 0, 1);
    upt_t u;
    uint64_t p[2] = {0, 0};
    CHECK(upt_open(&u, path, 0) == UPT_OK);
    CHECK(upt_check(&u, progress, p) == UPT_OK);
    CHECK(p[1] == sizeof g_kernel + sizeof g_rootfs + sizeof g_uboot);
    CHECK(p[0] == p[1]);
    upt_close(&u);
    unlink(path);
}

/* ---------- on a card (sys/uninstall.c) ---------- */

static void card_file(char *out, size_t cap, const char *card, const char *name)
{
    snprintf(out, cap, "%s/%s", card, name);
}

static void hiby_at(const char *card, const char *name, const char *model)
{
    char p[700];
    card_file(p, sizeof p, card, name);
    make_manifest(0, 0);
    make_version(model);
    make_hiby(p, 0, 1);
}

static void fathom_at(const char *card, const char *name)
{
    char p[700];
    card_file(p, sizeof p, card, name);
    make_fathom(p);
}

static int kind_of(const char *card, const char *name, upt_kind_t *kind)
{
    char p[700], text[S + 1];
    card_file(p, sizeof p, card, name);
    return manifest_now(p, text, sizeof text, kind);
}

static void clear_card(const char *card)
{
    static const char *const names[] = {"update.upt", "fathom-install.upt", "H2-Firmware-V2.3.upt",
                                        "H2-Firmware-V2.2.upt", "._H2-Firmware-V2.3.upt", "other.upt"};
    char p[700];
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        card_file(p, sizeof p, card, names[i]);
        unlink(p);
    }
    uninstall_cancel(card);
}

static void test_card(void)
{
    char card[600], name[256], p[700], before[33], after[33];
    path_of(card, sizeof card, "card");
    mkdir(card, 0755);
    card_file(p, sizeof p, card, "fathom");
    mkdir(p, 0755);
    upt_err_t why;
    upt_t u;

    CHECK(uninstall_find(card, name, sizeof name, &why) == UNI_NONE);
    CHECK(!uninstall_pending(card));

    /* Fathom's installer and macOS's "._" file aren't it */
    fathom_at(card, "update.upt");
    card_file(p, sizeof p, card, "._H2-Firmware-V2.3.upt");
    FILE *f = fopen(p, "wb");
    fputs("Mac OS X resource fork", f);
    fclose(f);
    CHECK(uninstall_find(card, name, sizeof name, &why) == UNI_NONE);

    /* HiBy's as downloaded: found; applying sets Fathom's aside and names HiBy's update.upt */
    hiby_at(card, "H2-Firmware-V2.3.upt", "erosq_2024");
    card_file(p, sizeof p, card, "H2-Firmware-V2.3.upt");
    md5_file(p, before);
    CHECK(uninstall_find(card, name, sizeof name, &why) == UNI_OK);
    CHECK(!strcmp(name, "H2-Firmware-V2.3.upt"));
    CHECK(upt_open(&u, p, 1) == UPT_OK && upt_check(&u, NULL, NULL) == UPT_OK);
    CHECK(uninstall_apply(card, name, &u, &why) == UNI_OK);
    upt_close(&u);
    CHECK(uninstall_pending(card));
    upt_kind_t kind = UPT_HIBY;
    CHECK(kind_of(card, "update.upt", &kind) == UPT_OK && kind == UPT_HIBY_PATCHED);
    CHECK(kind_of(card, "fathom-install.upt", &kind) == UPT_OK && kind == UPT_FATHOM);
    CHECK(!exists(p));
    /* found again while pending: update.upt, HiBy's */
    CHECK(uninstall_find(card, name, sizeof name, &why) == UNI_OK && !strcmp(name, "update.upt"));

    /* cancelled: everything as it was */
    CHECK(uninstall_cancel(card) == 0);
    CHECK(!uninstall_pending(card));
    md5_file(p, after);
    CHECK(!strcmp(before, after));
    CHECK(kind_of(card, "update.upt", &kind) == UPT_OK && kind == UPT_FATHOM);
    card_file(p, sizeof p, card, "fathom-install.upt");
    CHECK(!exists(p));
    card_file(p, sizeof p, card, "fathom/uninstall");
    CHECK(!exists(p));

    /* made ready, then HiBy's recovery installed it and Fathom's installer went
     * back to update.upt by hand: the record is forgotten at start, the files
     * left as they are (a cancel would have renamed Fathom's installer) */
    clear_card(card);
    fathom_at(card, "update.upt");
    hiby_at(card, "H2-Firmware-V2.3.upt", "erosq_2024");
    card_file(p, sizeof p, card, "H2-Firmware-V2.3.upt");
    CHECK(uninstall_find(card, name, sizeof name, &why) == UNI_OK);
    CHECK(upt_open(&u, p, 1) == UPT_OK && upt_check(&u, NULL, NULL) == UPT_OK);
    CHECK(uninstall_apply(card, name, &u, &why) == UNI_OK);
    upt_close(&u);
    CHECK(uninstall_forget_stale(card) == 0);            /* still waiting: kept */
    CHECK(uninstall_pending(card));
    {
        char a[700], b[700], c[700];
        card_file(a, sizeof a, card, "update.upt");
        card_file(b, sizeof b, card, "H2-Firmware-V2.3.upt");
        card_file(c, sizeof c, card, "fathom-install.upt");
        CHECK(!rename(a, b) && !rename(c, a));
        char m1[33], m2[33];
        md5_file(a, m1);
        md5_file(b, m2);
        CHECK(uninstall_forget_stale(card) == 1);
        CHECK(!uninstall_pending(card));
        md5_file(a, before);
        md5_file(b, after);
        CHECK(!strcmp(m1, before) && !strcmp(m2, after));
        CHECK(kind_of(card, "update.upt", &kind) == UPT_OK && kind == UPT_FATHOM);
    }
    /* no update.upt at all (HiBy's recovery took it away): forgotten too */
    clear_card(card);
    hiby_at(card, "update.upt", "erosq_2024");
    card_file(p, sizeof p, card, "update.upt");
    CHECK(upt_open(&u, p, 1) == UPT_OK && upt_check(&u, NULL, NULL) == UPT_OK);
    CHECK(uninstall_apply(card, "update.upt", &u, &why) == UNI_OK);
    upt_close(&u);
    unlink(p);
    CHECK(uninstall_forget_stale(card) == 1 && !uninstall_pending(card));

    /* put in the Music folder through the network drive: found there, moved to the
     * card's top as update.upt, and back into Music on cancel */
    clear_card(card);
    {
        char music[700], m1[33];
        card_file(music, sizeof music, card, "Music");
        mkdir(music, 0755);
        hiby_at(card, "Music/H2-Firmware-V2.3.upt", "erosq_2024");
        card_file(p, sizeof p, card, "Music/H2-Firmware-V2.3.upt");
        md5_file(p, m1);
        CHECK(uninstall_find(card, name, sizeof name, &why) == UNI_OK);
        CHECK(!strcmp(name, "Music/H2-Firmware-V2.3.upt"));
        CHECK(upt_open(&u, p, 1) == UPT_OK && upt_check(&u, NULL, NULL) == UPT_OK);
        CHECK(uninstall_apply(card, name, &u, &why) == UNI_OK);
        upt_close(&u);
        CHECK(!exists(p));
        CHECK(kind_of(card, "update.upt", &kind) == UPT_OK && kind == UPT_HIBY_PATCHED);
        CHECK(uninstall_cancel(card) == 0);
        md5_file(p, before);
        CHECK(!strcmp(m1, before));
        card_file(p, sizeof p, card, "update.upt");
        CHECK(!exists(p));
        /* one at the top and one in Music: which? */
        hiby_at(card, "H2-Firmware-V2.2.upt", "erosq_2024");
        CHECK(uninstall_find(card, name, sizeof name, &why) == UNI_SEVERAL);
        card_file(p, sizeof p, card, "Music/H2-Firmware-V2.3.upt");
        unlink(p);
        rmdir(music);
    }

    /* HiBy's already named update.upt: patched where it is */
    clear_card(card);
    hiby_at(card, "update.upt", "erosq_2024");
    card_file(p, sizeof p, card, "update.upt");
    md5_file(p, before);
    CHECK(uninstall_find(card, name, sizeof name, &why) == UNI_OK && !strcmp(name, "update.upt"));
    CHECK(upt_open(&u, p, 1) == UPT_OK && upt_check(&u, NULL, NULL) == UPT_OK);
    CHECK(uninstall_apply(card, name, &u, &why) == UNI_OK);
    upt_close(&u);
    CHECK(kind_of(card, "update.upt", &kind) == UPT_OK && kind == UPT_HIBY_PATCHED);
    CHECK(uninstall_cancel(card) == 0);
    md5_file(p, after);
    CHECK(!strcmp(before, after));

    /* update.upt HiBy's and another: update.upt; two others: which? */
    hiby_at(card, "H2-Firmware-V2.2.upt", "erosq_2024");
    CHECK(uninstall_find(card, name, sizeof name, &why) == UNI_OK && !strcmp(name, "update.upt"));
    card_file(p, sizeof p, card, "update.upt");
    unlink(p);
    hiby_at(card, "H2-Firmware-V2.3.upt", "erosq_2024");
    CHECK(uninstall_find(card, name, sizeof name, &why) == UNI_SEVERAL);

    /* the only one for another player: why */
    clear_card(card);
    hiby_at(card, "other.upt", "erosq");
    CHECK(uninstall_find(card, name, sizeof name, &why) == UNI_BAD && why == UPT_ERR_MODEL);

    /* update.upt and fathom-install.upt both in the way */
    clear_card(card);
    fathom_at(card, "update.upt");
    fathom_at(card, "fathom-install.upt");
    hiby_at(card, "H2-Firmware-V2.3.upt", "erosq_2024");
    card_file(p, sizeof p, card, "H2-Firmware-V2.3.upt");
    md5_file(p, before);
    CHECK(uninstall_find(card, name, sizeof name, &why) == UNI_OK);
    CHECK(upt_open(&u, p, 1) == UPT_OK && upt_check(&u, NULL, NULL) == UPT_OK);
    CHECK(uninstall_apply(card, name, &u, &why) == UNI_IN_THE_WAY);
    upt_close(&u);
    CHECK(!uninstall_pending(card));
    md5_file(p, after);
    CHECK(!strcmp(before, after));

    /* not older than the stock firmware in the flash (h2boot's note) */
    CHECK(uninstall_ver_time("2025-11-11T18:59:20+08:00") == 1762858760);   /* 10:59:20 UTC */
    CHECK(uninstall_ver_time("2025-01-15T15:53:29+08:00") == 1736927609);
    CHECK(uninstall_ver_time("2026-11-19T04:03:54+00:00") == 1795061034);
    CHECK(uninstall_ver_time("someday") == -1);
    CHECK(uninstall_stock_time(card) == 0);                                 /* no note */
    CHECK(uninstall_version_ok(card, "2025-01-15T15:53:29+08:00") == UNI_OK);
    card_file(p, sizeof p, card, "fathom/stock-version");
    f = fopen(p, "w");
    fputs("name=Linux-3.10.14\ntime=1762858730\n", f);                     /* V2.3's kernel */
    fclose(f);
    CHECK(uninstall_stock_time(card) == 1762858730);
    CHECK(uninstall_version_ok(card, "2025-11-11T18:59:20+08:00") == UNI_OK);   /* V2.3 */
    CHECK(uninstall_version_ok(card, "2026-03-01T10:00:00+08:00") == UNI_OK);   /* newer */
    CHECK(uninstall_version_ok(card, "2025-01-15T15:53:29+08:00") == UNI_OLDER);   /* V2.2 */
    CHECK(uninstall_version_ok(card, "2025-11-10T20:00:00+08:00") == UNI_OK);   /* 23 h before: within a day */
    CHECK(uninstall_version_ok(card, "2025-11-10T12:00:00+08:00") == UNI_OLDER);   /* 31 h before */
    CHECK(uninstall_version_ok(card, "garbled") == UNI_OK);
    unlink(p);

    /* space */
    int need = 0;
    CHECK(uninstall_space(card, 1024 * 1024, &need) == UNI_OK && need == 1 + UNINSTALL_SPARE_MB);
    CHECK(uninstall_space(card, (uint64_t)1 << 50, &need) == UNI_NO_SPACE && need > 1000000);

    clear_card(card);
    card_file(p, sizeof p, card, "fathom");
    rmdir(p);
    rmdir(card);
}

/* ---------- a real one ---------- */

static void copy(const char *from, const char *to)
{
    static uint8_t buf[1 << 16];
    FILE *a = fopen(from, "rb"), *b = fopen(to, "wb");
    size_t n;
    if (!a || !b) { perror(from); exit(1); }
    while ((n = fread(buf, 1, sizeof buf, a)) > 0) fwrite(buf, 1, n, b);
    fclose(a);
    fclose(b);
}

static void test_real(const char *real, const char *patched)
{
    char path[600], undo[600], before[33], after[33], text[S + 1];
    path_of(path, sizeof path, "real.upt");
    path_of(undo, sizeof undo, "undo");
    copy(real, path);
    md5_file(path, before);
    upt_t u;
    CHECK(upt_open(&u, path, 1) == UPT_OK);
    CHECK(u.kind == UPT_HIBY);
    CHECK(upt_check(&u, NULL, NULL) == UPT_OK);
    printf("real: %s, ver %s, U-Boot built %s, uboot.bin md5 %s\n", u.model, u.ver, u.uboot_built, u.uboot_md5);
    CHECK(upt_patch(&u, undo) == UPT_OK);
    upt_close(&u);
    CHECK(upt_open(&u, path, 0) == UPT_OK && upt_check(&u, NULL, NULL) == UPT_OK);
    CHECK(u.kind == UPT_HIBY_PATCHED);
    snprintf(text, sizeof text, "%s", u.manifest);
    upt_close(&u);
    if (patched) {
        char want[S + 1];
        CHECK(manifest_now(patched, want, sizeof want, NULL) == UPT_OK);
        CHECK(!strcmp(text, want));
    }
    CHECK(upt_undo(path, undo) == UPT_OK);
    md5_file(path, after);
    CHECK(!strcmp(before, after));
    unlink(path);
}

int main(void)
{
    char tmpl[512];
    test_tmp(tmpl, sizeof tmpl, "hibyupt");
    if (!mkdtemp(tmpl)) { perror(tmpl); return 1; }
    snprintf(g_dir, sizeof g_dir, "%s", tmpl);
    make_images();

    test_md5();
    test_hiby(0);
    test_hiby(1);
    test_cut_short();
    test_refusals();
    test_fathom();
    test_progress();
    test_card();
    const char *real = getenv("FATHOM_TEST_UPT");
    if (real && *real) test_real(real, getenv("FATHOM_TEST_UPT_PATCHED"));

    rmdir(g_dir);
    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("hibyupt: all passed\n");
    return 0;
}
