/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/*
 * Files replaced so that a power cut leaves the old one or the new one, on
 * the card's FAT too.
 *
 * rename() over a file there only changes the entry in memory: on the card it
 * goes on naming the old file's clusters, which the next fsync of anything
 * frees (the kernel writes the entry with the renamed file's inode, up to 30 s
 * later; replaced again within that, as the queue was every 10 s, never). A
 * cut then leaves an entry naming freed clusters: the next save frees them
 * again, the kernel calls the card damaged ("deleting FAT entry beyond EOF")
 * and, mounted the usual way, makes it read-only: nothing is saved that
 * session, nor in any after it, until the card is repaired on a computer.
 * (The card is mounted dirsync as well, which makes rename itself write the
 * entry: usr/lib/fathom/card.sh. This is for a kernel that doesn't.)
 */
#ifndef FATHOM_FSAFE_H
#define FATHOM_FSAFE_H

/* rename(tmp, path), then the entry and the folder written to the card: 0, or
 * -1 if the rename failed (tmp is still there: the caller's to remove). tmp's
 * contents are the caller's to fsync before. */
int file_replace(const char *tmp, const char *path);

/* A small text (a number or two, under 60 bytes) kept in a file of its own,
 * written over itself in place: no cluster or entry changes after the first
 * time, so nothing a cut can leave dangling. Read it back with fscanf: the
 * rest of its 64 bytes are spaces. sync: on the card before returning (one
 * sector), else within the kernel's half minute. 0, or -1. */
int file_put_small(const char *path, const char *text, int sync);

#endif
