/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The bootloader's firmware copy (bootloader/fathom/fwcopy.c), run on a flash
 * backup on a computer, to check it against tools/bluetooth/extract-firmware.py:
 *
 *   fwcopy flash.img DIR     writes DIR/BCM43430A1.hcd, brcmfmac43430-sdio.bin, .txt
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fwcopy.h"

static FILE *g_img;

static int read_img(void *ctx, uint32_t addr, void *buf, uint32_t len)
{
    (void)ctx;
    if (fseek(g_img, (long)addr, SEEK_SET) || fread(buf, 1, len, g_img) != len) return -1;
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 3) { fprintf(stderr, "usage: fwcopy flash.img DIR\n"); return 2; }
    g_img = fopen(argv[1], "rb");
    if (!g_img) { perror(argv[1]); return 1; }
    struct fwc f = {.read = read_img};
    f.peb = malloc(FWC_PEB_SIZE);
    f.data = malloc(FWC_DATA_MAX * sizeof *f.data);
    f.data_max = FWC_DATA_MAX;
    f.inodes = malloc(FWC_INODES_MAX * sizeof *f.inodes);
    f.inodes_max = FWC_INODES_MAX;
    struct fwc_file files[] = {
        {.name = "BCM4343A1_001.002.009.0122.0538.hcd"},
        {.name = "cyw43438-7.46.58.35.bin"},
        {.name = "nvram_azw372.txt"},
    };
    static const char *const out[] = {"BCM43430A1.hcd", "brcmfmac43430-sdio.bin", "brcmfmac43430-sdio.txt"};
    if (fwc_scan(&f, files, 3)) { fprintf(stderr, "no stock system\n"); return 1; }
    printf("%u data nodes, %u inode nodes, %u unreadable eraseblocks\n", f.data_n, f.inodes_n, f.bad_pebs);
    uint8_t *node = malloc(FWC_NODE_MAX);
    for (int i = 0; i < 3; i++) {
        if (!files[i].inum) { fprintf(stderr, "%s: not found\n", files[i].name); return 1; }
        uint8_t *data = malloc(files[i].size);
        int r = fwc_extract(&f, &files[i], data, node);
        if (r) { fprintf(stderr, "%s: extract failed (%d)\n", files[i].name, r); return 1; }
        int ok = i == 0 ? fwc_check_hcd(data, files[i].size)
               : i == 1 ? fwc_check_wifi_fw(data, files[i].size) : fwc_check_nvram(data, files[i].size);
        if (!ok) { fprintf(stderr, "%s: doesn't look right\n", files[i].name); return 1; }
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", argv[2], out[i]);
        FILE *o = fopen(path, "wb");
        if (!o || fwrite(data, 1, files[i].size, o) != files[i].size || fclose(o)) { perror(path); return 1; }
        printf("%s: inode %u, %u bytes -> %s\n", files[i].name, files[i].inum, files[i].size, path);
        free(data);
    }
    return 0;
}
