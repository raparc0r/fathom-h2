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

#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>

/* rename(tmp, path), then the entry and the folder written to the card: 0, or
 * -1 if the rename failed (tmp is still there: the caller's to remove). tmp's
 * contents are the caller's to fsync before. */
int file_replace(const char *tmp, const char *path);

/* A whole file from memory, safely: written beside it (path + ".tmp"), on
 * the card (fsync), then put in its place with file_replace. a, then b if
 * there is one (two pieces without joining them first; b NULL or bn 0: one).
 * 0; or -1 with the file that was there untouched and nothing left beside it.
 * The folder must exist. (Leftovers of a cut are swept by their ".tmp".) */
int file_write_whole(const char *path, const void *a, size_t an, const void *b, size_t bn);

/* The end of the same for a file written a line at a time: f, open on tmp, is
 * flushed, put on the card and closed, then tmp takes path's place. ok: what
 * the caller's own writing came to (0: nothing is replaced). 0; or -1 with
 * tmp removed and path as it was. f is closed either way. */
int file_commit(FILE *f, int ok, const char *tmp, const char *path);

/* A small text (a number or two, under 60 bytes) kept in a file of its own,
 * written over itself in place: no cluster or entry changes after the first
 * time, so nothing a cut can leave dangling. Read it back with fscanf: the
 * rest of its 64 bytes are spaces. sync: on the card before returning (one
 * sector), else within the kernel's half minute. 0, or -1. */
int file_put_small(const char *path, const char *text, int sync);

/* A file that is there and can't be read just now: it can't be opened for a
 * reason other than not being there, or its first bytes give an error (the
 * card). 1 then; 0 if it reads, is empty, or isn't there. For telling "try
 * again later" from "this file is no good". */
int file_unreadable(const char *path);

/* There is something at path (a file, a folder: whatever stat() finds). */
static inline int file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

/* What was read or written of fd leaves the page cache (len bytes from off;
 * 0, 0: all of it): tens of megabytes of an update or a download would
 * otherwise push out the music's cached pages and the player's own code, in
 * the H2's little RAM. Where the system can't be told, nothing. */
static inline void file_drop_cache(int fd, off_t off, off_t len)
{
#ifdef POSIX_FADV_DONTNEED
    posix_fadvise(fd, off, len, POSIX_FADV_DONTNEED);
#else
    (void)fd; (void)off; (void)len;
#endif
}

#endif
