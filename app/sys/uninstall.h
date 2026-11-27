/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/*
 * Uninstall Fathom, on the card (Settings > About > Uninstall Fathom): find
 * HiBy's update file at the card's top, list its bootloader (sys/hibyupt.h)
 * and name it update.upt, where HiBy's recovery looks; or put everything back.
 *
 * The file: update.upt if that's HiBy's, else the one other *.upt that is
 * (as downloaded: "H2-Firmware-V2.3.upt"), at the card's top or at the top of
 * the Music folder (all a phone reaches through the network drive), moved to
 * the card's top. An update.upt in the way that isn't (Fathom's own
 * installer) is renamed fathom-install.upt. Its name is given relative to the
 * card ("Music/H2-Firmware-V2.3.upt").
 *
 * What's changed is kept in fathom/uninstall/ until it's done or cancelled:
 *   undo      what hibyupt's patch overwrote
 *   renames   "from to" lines, each written before the rename it describes
 * so cancelling (or a start after the H2 was switched off and turned on into
 * Fathom again) can put it all back; while it's there, Uninstall is pending.
 */
#ifndef FATHOM_UNINSTALL_H
#define FATHOM_UNINSTALL_H

#include <stddef.h>
#include <stdint.h>

#include "sys/hibyupt.h"

#define UNINSTALL_MIN_BATTERY 50        /* %, unless on USB power */
#define UNINSTALL_MUSIC "Music"         /* also looked in: the network drive's folder */
#define UNINSTALL_SPARE_MB 16           /* beyond the file's size: HiBy unpacks it to the card */

/* What uninstall_find() and the rest say beyond hibyupt's errors. */
typedef enum {
    UNI_OK,
    UNI_NONE,           /* no HiBy update file at the card's top */
    UNI_SEVERAL,        /* more than one */
    UNI_BAD,            /* the one there can't be used: see *why */
    UNI_IN_THE_WAY,     /* update.upt and fathom-install.upt both there, neither HiBy's */
    UNI_NO_SPACE,       /* *need_mb */
    UNI_WRITE,          /* couldn't rename or write the record */
    UNI_OLDER,          /* older than the stock firmware in the flash */
} uni_err_t;

/* HiBy's file at the card's top: its name (no folder). UNI_BAD: *why says
 * what's wrong with the one .upt that isn't Fathom's installer. */
uni_err_t uninstall_find(const char *card, char *name, size_t cap, upt_err_t *why);

/* 1 while an uninstall is ready and waiting (fathom/uninstall/undo or
 * renames there). */
int uninstall_pending(const char *card);

/* At start: a record whose update.upt is no longer the HiBy file it made
 * ready (HiBy's recovery installed it and Fathom was installed again, or the
 * files were moved by hand) is forgotten, the files left as they are, so a
 * later cancel can't rename what's there now. One left by an Apply that was
 * cut short (HiBy's file half changed, or not yet named update.upt) is
 * cancelled instead: the file and the names put back. 1 if either. */
int uninstall_forget_stale(const char *card);

/* The space it needs on the card, in MB (the file's size and the spare), and
 * 0 if there's that much, else UNI_NO_SPACE. */
uni_err_t uninstall_space(const char *card, uint64_t file_size, int *need_mb);

/* Lists the bootloader in the file u (opened writable from card/name and
 * checked) and moves it to update.upt. */
uni_err_t uninstall_apply(const char *card, const char *name, upt_t *u, upt_err_t *why);

/* Puts back what uninstall_apply() changed and forgets it: 0, or -1 if some of
 * it couldn't be (the record is cleared all the same: there's nothing more
 * to try). */
int uninstall_cancel(const char *card);

/* The stock firmware in the flash, from h2boot's note (fathom/stock-version,
 * bootloader patch 0010): its kernel's build time, UTC seconds; 0 if there's
 * no note (an older bootloader). */
int64_t uninstall_stock_time(const char *card);

/* version.txt's ver ("2025-11-11T18:59:20+08:00") in UTC seconds; -1 if it
 * can't be read. */
int64_t uninstall_ver_time(const char *ver);

/* UNI_OLDER if the release ver is older than the stock firmware in the flash
 * (by more than a day: a release's kernel is built minutes before its date),
 * else UNI_OK (also with no note, or a date it can't read). An older U-Boot
 * can leave a newer H2's screen blank. */
uni_err_t uninstall_version_ok(const char *card, const char *ver);

/* In English; marked for translation. */
const char *uninstall_error_text(uni_err_t e);

#endif
