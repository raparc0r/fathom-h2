/* SPDX-License-Identifier: GPL-3.0-or-later */
/* A test's scratch folder template, for mkdtemp(): under $TMPDIR, or /tmp.
 * (On the H2, /tmp is RAM: tools/h2-tests.sh points TMPDIR at the card.) */
#ifndef FATHOM_TEST_TMPDIR_H
#define FATHOM_TEST_TMPDIR_H

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

/* (its real path: macOS's own TMPDIR is under /var, a link to /private/var,
 * and the library compares the paths it's given with those it finds) */
static inline char *test_tmp(char *out, size_t cap, const char *name)
{
    const char *t = getenv("TMPDIR");
    char real[PATH_MAX];
    if (!t || !*t) t = "/tmp";
    snprintf(out, cap, "%s/%s-XXXXXX", realpath(t, real) ? real : t, name);
    return out;
}

#endif
