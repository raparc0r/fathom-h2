/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* A value the kernel gives as a small text file (/sys, /proc): its first
 * line. The player and the USB-C role's program each had this function. A
 * header only: usbrole.c is built alone. */
#ifndef FATHOM_SYSFILE_H
#define FATHOM_SYSFILE_H

#include <stddef.h>
#include <stdio.h>

/* path's first line into buf, its newline kept: 0, or -1 (no file, nothing in it). */
static inline int sysfile_line(const char *path, char *buf, size_t cap)
{
    FILE *f = fopen(path, "re");
    if (!f) return -1;
    int ok = fgets(buf, (int)cap, f) != NULL;
    fclose(f);
    return ok ? 0 : -1;
}

#endif
