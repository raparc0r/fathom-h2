/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/*
 * fathom-nocache.so: file data read by the network drive (lighttpd) and SFTP
 * (gesftpserver) leaves the page cache as soon as it's been read, loaded
 * with LD_PRELOAD by fathom-share and sftp-server.
 *
 * Streaming a 65 MB file to a computer otherwise fills the 23 MB H2's page
 * cache with data read once, and the kernel makes room by evicting the
 * player's code (it runs from the card): re-read on the next fault, which
 * the audio hears. Both servers read files with pread() or preadv2() (never
 * mmap or sendfile over TLS); after each read the whole pages behind its end
 * are dropped (POSIX_FADV_DONTNEED). The kernel's readahead beyond the read
 * stays, so the next read still finds its data waiting.
 *
 * Only clean pages go (a file being written is only asked to start writeback),
 * and a file the player is also reading is just read again from the card.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

#define BEHIND (256 * 1024)        /* how far back each read drops (the last read's tail) */
#define PAGE   4096

/* the C library's own, found once */
#define REAL(name) \
    static __typeof__(&name) real; \
    if (!real && !(real = (__typeof__(&name))dlsym(RTLD_NEXT, #name))) { errno = ENOSYS; return -1; }

static void drop(int fd, off_t start, ssize_t n)
{
    if (n <= 0 || start < 0) return;
    off_t end = (start + n) & ~(off_t)(PAGE - 1);   /* the page under the end stays: read on */
    off_t from = start > BEHIND ? (start - BEHIND) & ~(off_t)(PAGE - 1) : 0;
    if (end > from) posix_fadvise(fd, from, end - from, POSIX_FADV_DONTNEED);
}

ssize_t pread(int fd, void *buf, size_t count, off_t offset)
{
    REAL(pread)
    ssize_t n = real(fd, buf, count, offset);
    drop(fd, offset, n);
    return n;
}

ssize_t preadv(int fd, const struct iovec *iov, int cnt, off_t offset)
{
    REAL(preadv)
    ssize_t n = real(fd, iov, cnt, offset);
    if (offset >= 0) drop(fd, offset, n);     /* (-1: the file position; not used here) */
    return n;
}

ssize_t preadv2(int fd, const struct iovec *iov, int cnt, off_t offset, int flags)
{
    REAL(preadv2)
    ssize_t n = real(fd, iov, cnt, offset, flags);
    if (offset >= 0) drop(fd, offset, n);
    return n;
}
