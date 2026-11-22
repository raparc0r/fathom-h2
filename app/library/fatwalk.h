/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/*
 * List a FAT32 folder tree by reading the partition directly: one read per
 * directory cluster, instead of the kernel's many small ones (on the H2's
 * 128 GB card, 32 KB clusters: listing ~500 folders went from ~21 s to a few).
 * Used only to tell quickly that nothing changed (library.c); any difference
 * falls back to an ordinary scan, so a mistake here can't corrupt the library.
 */
#ifndef FATHOM_FATWALK_H
#define FATHOM_FATWALK_H

#include <stdint.h>

/* Called for every file (not folder) under the start folder, with its path as
 * the kernel shows it (mount point + names), date and size. Names starting
 * with '.' are skipped, as walk() does. Return non-zero to stop. */
typedef int (*fatwalk_cb)(const char *path, int64_t mtime, int64_t size, void *ctx);

/* Walk `dev` (e.g. /dev/mmcblk0p1, mounted at `mount`) from the folder
 * `mount`/`rel` (e.g. "Music"; "" for the root). 0 on success, -1 if the
 * device isn't FAT32 or can't be read. */
int fatwalk(const char *dev, const char *mount, const char *rel, fatwalk_cb cb, void *ctx);

#endif
