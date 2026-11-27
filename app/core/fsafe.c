/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
#include "core/fsafe.h"

#include <errno.h>
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

int file_unreadable(const char *path)
{
    FILE *f = fopen(path, "rbe");
    if (!f) return errno != ENOENT && errno != ENOTDIR;
    char b[16];
    int bad = fread(b, 1, sizeof b, f) == 0 && ferror(f);
    fclose(f);
    return bad;
}

int file_commit(FILE *f, int ok, const char *tmp, const char *path)
{
    ok = !ferror(f) && ok;
    ok = fflush(f) == 0 && ok;
    ok = fsync(fileno(f)) == 0 && ok;
    ok = fclose(f) == 0 && ok;
    if (!ok || file_replace(tmp, path)) { unlink(tmp); return -1; }
    return 0;
}

int file_write_whole(const char *path, const void *a, size_t an, const void *b, size_t bn)
{
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wbe");
    if (!f) return -1;
    int ok = (!an || fwrite(a, 1, an, f) == an) && (!b || !bn || fwrite(b, 1, bn, f) == bn);
    return file_commit(f, ok, tmp, path);
}
