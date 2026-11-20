/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copy files out of the H2's stock system (UBIFS on UBI, in the flash), with
 * nothing but a way to read the flash: the bootloader uses it to put the
 * Bluetooth and Wi-Fi chip's firmware on the card (fwcopy-h2boot.c), and
 * tests/fwcopy runs it on a flash backup. The same method as
 * tools/bluetooth/extract-firmware.py.
 */
#ifndef FATHOM_FWCOPY_H
#define FATHOM_FWCOPY_H

#include <stddef.h>
#include <stdint.h>

#define FWC_ROOTFS_ADDR 0x1000000u   /* the stock system: partition 4 (mtd4) */
#define FWC_ROOTFS_SIZE 0x4000000u
#define FWC_PEB_SIZE    0x20000u     /* a UBI eraseblock: 128 KiB */
#define FWC_NODE_MAX    (48 + 4096 + 64)   /* the largest data node, and then some */

/* Read len bytes of the flash at addr into buf: 0, or <0 on a read error. */
typedef int (*fwc_read_fn)(void *ctx, uint32_t addr, void *buf, uint32_t len);

struct fwc_data { uint64_t sqnum; uint32_t ino, block, addr; };   /* a data node, where it is */
struct fwc_inode { uint64_t sqnum; uint32_t ino; uint32_t size; };

struct fwc_file {
    const char *name;               /* in the stock system */
    uint64_t sqnum;                 /* (its newest directory entry) */
    uint32_t inum, size;            /* found: inode number and size; inum 0: not found */
};

struct fwc {
    fwc_read_fn read;
    void *ctx;
    /* Work memory, the caller's: */
    uint8_t *peb;                   /* FWC_PEB_SIZE bytes */
    struct fwc_data *data;          /* every data node: ~21000 in the H2's stock system */
    uint32_t data_max, data_n;
    struct fwc_inode *inodes;       /* every inode node: ~2400 */
    uint32_t inodes_max, inodes_n;
    uint32_t bad_pebs;              /* eraseblocks that couldn't be read (skipped) */
};

/* Size of the work memory for the H2's stock system, with room to spare. */
#define FWC_DATA_MAX   49152u
#define FWC_INODES_MAX 8192u

/* Scan the stock system once: finds files[i] (name -> inum, size) and indexes
 * the data nodes. 0 when it's there (files not found have inum 0), <0 when
 * there's no stock system at all. */
int fwc_scan(struct fwc *f, struct fwc_file *files, int n);

/* File file's bytes into out (file->size bytes); node: FWC_NODE_MAX bytes of
 * scratch. 0, or <0 if a block is missing or doesn't decompress. */
int fwc_extract(struct fwc *f, const struct fwc_file *file, uint8_t *out, uint8_t *node);

/* What the files must look like before they're written. */
int fwc_check_hcd(const uint8_t *d, uint32_t n);          /* a chain of HCI vendor commands */
int fwc_check_wifi_fw(const uint8_t *d, uint32_t n);
int fwc_check_nvram(const uint8_t *d, uint32_t n);

/* The stock kernel (partition 2, mtd2): its uImage header is the stock
 * firmware's own record of its build. Each HiBy release builds its kernel
 * within a minute of its version.txt date (V2.2: 2025-01-15, V2.3:
 * 2025-11-11), so the header says which release is in the flash. */
#define FWC_KERNEL_ADDR 0x300000u
#define FWC_UIMAGE_HDR  64u

/* The header h (FWC_UIMAGE_HDR bytes) as a note for the card,
 * "name=Linux-3.10.14\ntime=1762858730\n" (its build time, seconds since
 * 1970, UTC): its length, or <0 if h isn't a uImage header (magic and CRC). */
int fwc_kernel_note(const uint8_t *h, char *out, uint32_t cap);

#endif
