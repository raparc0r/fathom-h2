/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * h2boot (Fathom): put the Bluetooth and Wi-Fi chip's firmware on the card,
 * copied from the stock system still in the H2's flash (fwcopy.c does the
 * reading). Fathom can't ship these files (they're Cypress's and HiBy's), but
 * every H2 has them, so a fresh install gets Bluetooth and Wi-Fi without a
 * computer:
 *
 *   - whenever it boots Linux (by default, or from the menu's Boot select),
 *     once: when the card is Fathom's (it has /fathom)
 *     and any of the three files is missing. If the copy can't be made (no
 *     stock system in the flash, a read error), a marker file stops it from
 *     being tried at every start;
 *   - from the recovery menu (Advanced > Copy BT/Wi-Fi firmware), any time.
 *
 * It only reads the flash. The files are written next to the card's other
 * firmware, each to a new file renamed into place.
 */
#include "x1000bootloader.h"
#include "ingenic/gui-ingenic.h"
#include "core_alloc.h"
#include "file.h"
#include "dir.h"
#include "kernel.h"
#include "nand-ingenic.h"
#include "fwcopy.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define FW_DIR  "/fathom/firmware/brcm"
#define TRIED   FW_DIR "/.not-copied"

static const char *const stock_names[3] = {
    "BCM4343A1_001.002.009.0122.0538.hcd",     /* Bluetooth */
    "cyw43438-7.46.58.35.bin",                 /* Wi-Fi */
    "nvram_azw372.txt",                        /* Wi-Fi: the board's calibration */
};
static const char *const card_names[3] = {
    FW_DIR "/BCM43430A1.hcd",
    FW_DIR "/brcmfmac43430-sdio.bin",
    FW_DIR "/brcmfmac43430-sdio.txt",
};

static bool has_file(const char* path)
{
    int fd = open(path, O_RDONLY);
    if(fd < 0)
        return false;
    close(fd);
    return true;
}

static bool has_dir(const char* path)
{
    DIR* d = opendir(path);
    if(!d)
        return false;
    closedir(d);
    return true;
}

static int read_flash(void* ctx, uint32_t addr, void* buf, uint32_t len)
{
    return nand_read_bytes((struct nand_drv*)ctx, addr, len, buf) == NAND_SUCCESS ? 0 : -1;
}

static int write_file(const char* path, const uint8_t* data, uint32_t len)
{
    char tmp[64];
    snprintf(tmp, sizeof(tmp), "%s.new", path);
    int fd = open(tmp, O_WRONLY|O_CREAT|O_TRUNC);
    if(fd < 0)
        return -1;
    ssize_t w = write(fd, data, len);
    if(close(fd) < 0 || w != (ssize_t)len) {
        remove(tmp);
        return -1;
    }
    remove(path);
    return rename(tmp, path) < 0 ? -1 : 0;
}

static void mark_tried(void)
{
    int fd = open(TRIED, O_WRONLY|O_CREAT|O_TRUNC);
    if(fd >= 0)
        close(fd);
}

/* 0: copied; <0: why not (shown when asked) */
static int copy_firmware(const char** why)
{
    int handles[5] = {0, 0, 0, 0, 0}, ret = -1;
    struct fwc f;
    memset(&f, 0, sizeof(f));
    struct fwc_file files[3];
    for(int i = 0; i < 3; i++)
        files[i].name = stock_names[i];

    size_t sizes[4] = {FWC_PEB_SIZE, FWC_DATA_MAX * sizeof(struct fwc_data),
                       FWC_INODES_MAX * sizeof(struct fwc_inode), FWC_NODE_MAX};
    void* mem[4];
    for(int i = 0; i < 4; i++) {
        handles[i] = core_alloc_ex(sizes[i], &buflib_ops_locked);
        if(handles[i] <= 0) {
            *why = "Out of memory";
            goto out_free;
        }
        mem[i] = core_get_data(handles[i]);
    }
    f.peb = mem[0];
    f.data = mem[1];
    f.data_max = FWC_DATA_MAX;
    f.inodes = mem[2];
    f.inodes_max = FWC_INODES_MAX;

    struct nand_drv* ndrv = nand_init();
    nand_lock(ndrv);
    if(nand_open(ndrv) != NAND_SUCCESS) {
        *why = "Can't open the flash";
        nand_unlock(ndrv);
        goto out_free;
    }
    f.read = read_flash;
    f.ctx = ndrv;

    if(fwc_scan(&f, files, 3) < 0) {
        *why = "No stock firmware in the flash";
        goto out_nand;
    }
    uint32_t largest = 0;
    for(int i = 0; i < 3; i++) {
        if(!files[i].inum) {
            *why = "Not in the stock firmware";
            goto out_nand;
        }
        if(files[i].size > largest)
            largest = files[i].size;
    }
    if(largest > 2*1024*1024) {
        *why = "Too big";
        goto out_nand;
    }
    handles[4] = core_alloc_ex(largest, &buflib_ops_locked);
    if(handles[4] <= 0) {
        *why = "Out of memory";
        goto out_nand;
    }
    uint8_t* out = core_get_data(handles[4]);

    mkdir("/fathom/firmware");
    mkdir(FW_DIR);
    for(int i = 0; i < 3; i++) {
        if(fwc_extract(&f, &files[i], out, mem[3]) < 0) {
            *why = "Read error";
            goto out_nand;
        }
        bool ok = i == 0 ? fwc_check_hcd(out, files[i].size)
                : i == 1 ? fwc_check_wifi_fw(out, files[i].size)
                         : fwc_check_nvram(out, files[i].size);
        if(!ok) {
            *why = "It doesn't look right";
            goto out_nand;
        }
        if(write_file(card_names[i], out, files[i].size) < 0) {
            *why = "Can't write to the card";
            goto out_nand;
        }
    }
    remove(TRIED);
    ret = 0;

  out_nand:
    nand_close(ndrv);
    nand_unlock(ndrv);
  out_free:
    for(int i = 0; i < 5; i++)
        if(handles[i] > 0)
            core_free(handles[i]);
    return ret;
}

void fathom_copy_firmware(bool asked)
{
    if(check_disk(asked) != DISK_PRESENT)
        return;
    if(!asked) {
        if(!has_dir("/fathom"))             /* not a Fathom card */
            return;
        if(has_file(card_names[0]) && has_file(card_names[1]) && has_file(card_names[2]))
            return;
        if(has_file(TRIED))
            return;
    }

    splashf(0, "Copying the Bluetooth and\nWi-Fi firmware from the\nstock firmware...");
    long start = current_tick;
    const char* why = "";
    if(copy_firmware(&why) == 0) {
        splashf(asked ? 3*HZ : HZ, "Bluetooth and Wi-Fi\nfirmware copied\n(%ld s)",
                (current_tick - start + HZ/2) / HZ);
    } else {
        if(!asked) {
            mark_tried();
            splashf(3*HZ, "Couldn't copy the Bluetooth\nand Wi-Fi firmware:\n%s", why);
        } else {
            splashf(5*HZ, "Couldn't copy the Bluetooth\nand Wi-Fi firmware:\n%s", why);
        }
    }
}

void fathom_copy_firmware_menu(void)
{
    fathom_copy_firmware(true);
}
