/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
#include "core/fsafe.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int file_replace(const char *tmp, const char *path)
{
    if (rename(tmp, path)) return -1;
    int fd = open(path, O_RDONLY | O_CLOEXEC);       /* its entry, where it is now, and the FAT */
    if (fd >= 0) { fsync(fd); close(fd); }
    char dir[1024];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash && slash != dir) *slash = '\0';
    else snprintf(dir, sizeof dir, "%s", slash ? "/" : ".");
    fd = open(dir, O_RDONLY | O_CLOEXEC);            /* (tmp's entry gone) */
    if (fd >= 0) { fsync(fd); close(fd); }
    return 0;
}

int file_put_small(const char *path, const char *text, int sync)
{
    char rec[64];
    size_t n = strlen(text);
    if (n > sizeof rec - 2) return -1;
    memset(rec, ' ', sizeof rec);
    memcpy(rec, text, n);
    rec[sizeof rec - 1] = '\n';
    int fd = open(path, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) return -1;
    int ok = pwrite(fd, rec, sizeof rec, 0) == (ssize_t)sizeof rec;
    if (ok && sync) fdatasync(fd);
    if (close(fd)) ok = 0;
    return ok ? 0 : -1;
}
