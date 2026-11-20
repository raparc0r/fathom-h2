/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * HiBy's update files (update.upt), for Uninstall Fathom: read one, check it,
 * and mark its bootloader for installing, in place.
 *
 * An update file is an ISO 9660 image. Its root holds the images (uboot.bin,
 * uimage.bin, system.ubi), update.txt, the updater's list of what to write
 * with each image's MD5, and version.txt, the model and a date:
 *
 *   kernel={                          version={
 *           name=kernel                       name=erosq_2024
 *           file_path=autoupdate/...          ver=2025-11-11T18:59:20+08:00
 *           md5=...                   }
 *   }
 *
 * HiBy's own files carry their U-Boot but don't list it, so its updater leaves
 * the bootloader alone. upt_patch() adds the same entry as Rockbox's
 * hibyos_nativepatcher.sh mkstockuboot, at the start of update.txt, by
 * rewriting update.txt's sector and its length in the directory records (in
 * each directory tree: ISO 9660's and, if there is one, Joliet's); nothing
 * else in the file changes, so no copy of a 60 MB file is made. What it
 * overwrites is saved to an undo file first; upt_undo() puts it back.
 *
 * Fathom's own installer (tools/release/make-upt.sh) is the same format with
 * only a bootloader entry: upt_open() tells it apart (UPT_FATHOM).
 *
 * Only a sector or two is ever in memory; upt_check() streams each image once
 * (32 KB at a time, dropping it from the page cache as it goes).
 */
#ifndef FATHOM_HIBYUPT_H
#define FATHOM_HIBYUPT_H

#include <stddef.h>
#include <stdint.h>

#define UPT_MODEL "erosq_2024"          /* hardware revision 4: the H2s Fathom runs on */
#define UPT_MAX_FILES   16
#define UPT_MAX_TREES   2               /* ISO 9660's, Joliet's */
#define UPT_MAX_ENTRIES 8

typedef enum {
    UPT_OK,
    UPT_ERR_OPEN,           /* can't open or read the file */
    UPT_ERR_NOT_UPDATE,     /* not an update file (not ISO 9660, files or list missing) */
    UPT_ERR_MODEL,          /* for another player or hardware revision */
    UPT_ERR_DAMAGED,        /* cut short, or an image doesn't match its MD5 */
    UPT_ERR_NO_BOOTLOADER,  /* no uboot.bin, or it isn't HiBy's U-Boot */
    UPT_ERR_LAYOUT,         /* laid out in a way this can't change in place */
    UPT_ERR_WRITE,          /* couldn't write the file or the undo file */
    UPT_ERR_UNDO,           /* no undo file, or it doesn't belong to this file */
} upt_err_t;

typedef enum {
    UPT_HIBY,               /* HiBy's, as HiBy made it: kernel and system, no bootloader */
    UPT_HIBY_PATCHED,       /* HiBy's, its bootloader listed (by upt_patch or mkstockuboot) */
    UPT_FATHOM,             /* Fathom's installer: a bootloader only */
} upt_kind_t;

typedef struct {
    char name[32];          /* lower case, without ";1" */
    uint32_t lba, size;
    int nrec;
    uint64_t rec[UPT_MAX_TREES];    /* byte offset of its directory record in each tree */
} upt_file_t;

typedef struct {
    char key[16], name[16], path[64], md5[33];
} upt_entry_t;

typedef struct {
    int fd, writable;
    uint32_t blocks;                /* the volume's size in 2048-byte sectors */
    int nfiles;
    upt_file_t files[UPT_MAX_FILES];
    char model[32], ver[40];        /* version.txt */
    char manifest[2049];            /* update.txt */
    size_t manifest_len;
    int nentries;
    upt_entry_t entries[UPT_MAX_ENTRIES];
    upt_kind_t kind;
    /* after upt_check(): */
    int checked;
    char uboot_md5[33];
    char uboot_built[48];           /* from the U-Boot SPL banner, "Nov 11 2025 - 18:57:13" */
} upt_t;

/* Reads the file's layout, version.txt and update.txt (no images yet).
 * upt_close() after, whatever it returns. A file upt_patch() was cut short in
 * may not open: upt_undo() first if there's an undo file. */
upt_err_t upt_open(upt_t *u, const char *path, int writable);
void upt_close(upt_t *u);

/* Checks every image listed against its MD5 and that uboot.bin is HiBy's
 * U-Boot (UPT_FATHOM: only the listed bootloader). progress, if given, gets
 * the bytes read so far and the total. */
upt_err_t upt_check(upt_t *u, void (*progress)(void *ctx, uint64_t done, uint64_t total), void *ctx);

/* Lists uboot.bin in update.txt (after upt_check(), on a writable UPT_HIBY);
 * a file already patched is left as it is. Saves what it overwrites to
 * undo_path first. */
upt_err_t upt_patch(upt_t *u, const char *undo_path);

/* Puts back what upt_patch() changed in the file at path, and deletes the undo
 * file. */
upt_err_t upt_undo(const char *path, const char *undo_path);

/* The file, by name (lower case), or NULL. */
const upt_file_t *upt_file(const upt_t *u, const char *name);

/* In English; marked for translation. */
const char *upt_error_text(upt_err_t e);

#endif
