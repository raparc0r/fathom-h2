/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The bootloader's copy of files out of the stock system (bootloader/fathom/fwcopy.c),
 * on a made-up flash: the newest entry, inode and block win, a node with a bad
 * CRC is ignored, blocks in another eraseblock are found, uncompressed and LZO
 * blocks both read, and missing files and blocks are reported. (Against the real
 * stock system: tools/fwcopy, byte for byte as extract-firmware.py.) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fwcopy.h"
#include "test_util.h"

/* The stock partition (64 MiB): its first MiB held, the rest erased (0xFF).
 * (All of it in RAM was more than the H2 has: the test crashed there.) */
#define HELD (1u << 20)
static uint8_t *g_flash;

static int read_flash(void *ctx, uint32_t addr, void *buf, uint32_t len)
{
    (void)ctx;
    if (addr < FWC_ROOTFS_ADDR || addr - FWC_ROOTFS_ADDR + len > FWC_ROOTFS_SIZE) return -1;
    uint32_t off = addr - FWC_ROOTFS_ADDR, held = off < HELD ? (HELD - off < len ? HELD - off : len) : 0;
    memcpy(buf, g_flash + off, held);
    memset((uint8_t *)buf + held, 0xFF, len - held);
    return 0;
}

static void put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> 8 * i); }
static void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32)); }

static uint32_t crc(const uint8_t *p, uint32_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    }
    return c;
}

/* A node at *at (in the partition), its common header filled in; returns its body. */
static uint32_t g_at;
static uint8_t *node(uint8_t type, uint64_t sqnum, uint32_t len)
{
    uint8_t *p = g_flash + g_at;
    memset(p, 0, len);
    put32(p, 0x06101831u);
    put64(p + 8, sqnum);
    put32(p + 16, len);
    p[20] = type;
    g_at += (len + 7) & ~7u;
    return p;
}
static void seal(uint8_t *p) { uint32_t len = p[16] | p[17] << 8 | p[18] << 16 | (uint32_t)p[19] << 24; put32(p + 4, crc(p + 8, len - 8)); }

static void dent(const char *name, uint32_t inum, uint64_t sqnum, int good)
{
    uint32_t n = (uint32_t)strlen(name);
    uint8_t *p = node(2, sqnum, 56 + n + 1);
    put64(p + 40, inum);
    p[50] = (uint8_t)n;
    memcpy(p + 56, name, n);
    seal(p);
    if (!good) p[60] ^= 1;                  /* damaged after its CRC */
}

static void inode(uint32_t ino, uint32_t size, uint64_t sqnum)
{
    uint8_t *p = node(0, sqnum, 160);
    put32(p + 24, ino);
    put64(p + 48, size);
    seal(p);
}

static void data_raw(uint32_t ino, uint32_t block, uint64_t sqnum, uint8_t fill, uint32_t usize)
{
    uint8_t *p = node(1, sqnum, 48 + usize);
    put32(p + 24, ino);
    put32(p + 28, block | 1u << 29);        /* the key type (data) in the top bits */
    put32(p + 40, usize);
    memset(p + 48, fill, usize);
    seal(p);
}

/* LZO1X with only literals: a first byte of 17 + n, the bytes, then the end marker. */
static void data_lzo(uint32_t ino, uint32_t block, uint64_t sqnum, const uint8_t *bytes, uint32_t n)
{
    uint8_t *p = node(1, sqnum, 48 + 1 + n + 3);
    put32(p + 24, ino);
    put32(p + 28, block | 1u << 29);
    put32(p + 40, n);
    p[44] = 1;                               /* LZO */
    p[48] = (uint8_t)(17 + n);
    memcpy(p + 49, bytes, n);
    p[49 + n] = 0x11;
    seal(p);
}

/* The stock kernel's uImage header as h2boot's note: a header made here (V2.3's
 * name and time), a wrong magic, a wrong CRC, too little room; and with
 * FATHOM_TEST_FLASH=backups/flash.img, the real one in a flash backup. */
static void be(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (24 - 8 * i)); }

static void test_kernel_note(void)
{
    uint8_t h[FWC_UIMAGE_HDR] = {0};
    be(h, 0x27051956u);
    be(h + 8, 1762858730u);                  /* 2025-11-11 10:58:50 UTC */
    memcpy(h + 32, "Linux-3.10.14", 13);
    uint32_t c = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < sizeof h; i++) {
        c ^= h[i];
        for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    }
    be(h + 4, ~c);
    char out[96];
    int n = fwc_kernel_note(h, out, sizeof out);
    CHECK(n > 0 && !strcmp(out, "name=Linux-3.10.14\ntime=1762858730\n"));
    CHECK(fwc_kernel_note(h, out, 20) < 0);
    h[40] ^= 1;                              /* the CRC no longer matches */
    CHECK(fwc_kernel_note(h, out, sizeof out) < 0);
    h[40] ^= 1;
    h[0] = 0;
    CHECK(fwc_kernel_note(h, out, sizeof out) < 0);

    const char *flash = getenv("FATHOM_TEST_FLASH");
    FILE *f = flash && *flash ? fopen(flash, "rb") : NULL;
    if (f) {
        uint8_t r[FWC_UIMAGE_HDR];
        CHECK(!fseek(f, FWC_KERNEL_ADDR, SEEK_SET) && fread(r, 1, sizeof r, f) == sizeof r);
        fclose(f);
        n = fwc_kernel_note(r, out, sizeof out);
        CHECK(n > 0);
        if (n > 0) printf("%s: %s", flash, out);
    }
}

int main(void)
{
    g_flash = malloc(HELD);
    if (!g_flash) return 1;
    memset(g_flash, 0xFF, HELD);               /* erased flash */
    memcpy(g_flash, "UBI#", 4);

    g_at = 0x1000;                             /* eraseblock 0, after UBI's headers */
    dent("fw.bin", 10, 5, 1);                  /* an old entry: replaced below */
    inode(11, 1, 2);                           /* an old inode for 11 */
    data_raw(11, 0, 3, 0x11, 4096);            /* an old block 0 */
    dent("broken.txt", 12, 8, 0);              /* bad CRC: not there */
    inode(12, 10, 8);
    data_raw(12, 0, 8, 0x22, 10);
    dent("holey.bin", 13, 10, 1);              /* its block 1 is missing */
    inode(13, 5000, 10);
    data_raw(13, 0, 10, 0x33, 4096);

    g_at = 3 * FWC_PEB_SIZE + 0x1000;          /* eraseblock 3 */
    dent("fw.bin", 11, 9, 1);
    inode(11, 4096 + 200, 6);
    data_raw(11, 0, 7, 0xAB, 4096);
    uint8_t tail[200];
    for (int i = 0; i < 200; i++) tail[i] = (uint8_t)i;
    data_lzo(11, 1, 7, tail, sizeof tail);

    struct fwc f = {.read = read_flash};
    f.peb = malloc(FWC_PEB_SIZE);
    f.data = malloc(FWC_DATA_MAX * sizeof *f.data);
    f.data_max = FWC_DATA_MAX;
    f.inodes = malloc(FWC_INODES_MAX * sizeof *f.inodes);
    f.inodes_max = FWC_INODES_MAX;
    struct fwc_file files[] = {{.name = "fw.bin"}, {.name = "broken.txt"}, {.name = "holey.bin"}, {.name = "absent"}};
    CHECK(fwc_scan(&f, files, 4) == 0);
    CHECK(files[0].inum == 11 && files[0].size == 4296);
    CHECK(files[1].inum == 0);
    CHECK(files[2].inum == 13 && files[2].size == 5000);
    CHECK(files[3].inum == 0);

    uint8_t *node_buf = malloc(FWC_NODE_MAX), out[8192];
    CHECK(fwc_extract(&f, &files[0], out, node_buf) == 0);
    int ok = 1;
    for (int i = 0; i < 4096; i++) ok &= out[i] == 0xAB;   /* the newer block 0 */
    CHECK(ok);
    CHECK(!memcmp(out + 4096, tail, sizeof tail));
    CHECK(fwc_extract(&f, &files[2], out, node_buf) < 0);   /* a block missing */
    CHECK(fwc_extract(&f, &files[3], out, node_buf) < 0);

    /* No stock system: no UBI header at the start */
    g_flash[0] = 0;
    CHECK(fwc_scan(&f, files, 4) < 0);

    static const uint8_t hcd[] = {0x2e, 0xfc, 0x00, 0x4c, 0xfc, 0x02, 0x01, 0x02};
    CHECK(fwc_check_hcd(hcd, sizeof hcd));
    CHECK(!fwc_check_hcd(hcd, sizeof hcd - 1));

    test_kernel_note();

    return t_result();
}
