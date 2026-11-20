/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Just enough of Linux's <sys/inotify.h> to build device/input.c on any host
 * (tests/test_input.c): no watch is ever made there. */
#ifndef FATHOM_FAKE_SYS_INOTIFY_H
#define FATHOM_FAKE_SYS_INOTIFY_H

#include <errno.h>
#include <stdint.h>

struct inotify_event {
    int      wd;
    uint32_t mask, cookie, len;
    char     name[];
};

#define IN_ATTRIB   0x00000004
#define IN_CREATE   0x00000100
#define IN_NONBLOCK 00004000
#define IN_CLOEXEC  02000000

static inline int inotify_init1(int flags) { (void)flags; errno = ENOSYS; return -1; }
static inline int inotify_add_watch(int fd, const char *path, uint32_t mask)
{
    (void)fd; (void)path; (void)mask;
    errno = ENOSYS;
    return -1;
}

#endif
