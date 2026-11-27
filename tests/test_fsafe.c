/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* file_unreadable (app/core/fsafe.c): a file that is there and can't be read
 * just now, told from one that reads, is empty, or isn't there. */
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "core/fsafe.h"
#include "test_util.h"

int main(void)
{
    char tmpl[600], p[700];
    char *dir = t_mkdtemp(tmpl, sizeof tmpl, "fathom-fsafe");

    /* it reads: no */
    snprintf(p, sizeof p, "%s/song.flac", dir);
    t_write_text(p, "fLaC and then some");
    CHECK(file_unreadable(p) == 0);
    /* shorter than what is asked for: it still reads */
    t_write_text(p, "x");
    CHECK(file_unreadable(p) == 0);
    /* empty: nothing to read, and no error */
    t_write_text(p, "");
    CHECK(file_unreadable(p) == 0);
    unlink(p);
    /* not there: no (gone is not "later") */
    CHECK(file_unreadable(p) == 0);
    /* nor under a name that isn't a folder */
    snprintf(p, sizeof p, "%s/file", dir);
    t_write_text(p, "x");
    snprintf(p, sizeof p, "%s/file/song.flac", dir);
    CHECK(file_unreadable(p) == 0);
    snprintf(p, sizeof p, "%s/file", dir);
    unlink(p);
    /* there, and reading it fails (a folder opens, and can't be read): yes */
    snprintf(p, sizeof p, "%s/folder.flac", dir);
    CHECK(mkdir(p, 0755) == 0);
    CHECK(file_unreadable(p) == 1);
    rmdir(p);
    /* there, and it can't be opened (not as root, who opens anything) */
    snprintf(p, sizeof p, "%s/closed.flac", dir);
    t_write_text(p, "fLaC");
    if (chmod(p, 0) == 0 && geteuid() != 0 && access(p, R_OK) != 0) CHECK(file_unreadable(p) == 1);   /* (FAT keeps no modes) */
    unlink(p);

    rmdir(dir);
    return t_result();
}
