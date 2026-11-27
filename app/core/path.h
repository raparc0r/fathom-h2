/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* A path's extension, found in one place (the tags' reader, the playlists',
 * the CUE sheets' and the library each looked for it their own way). In a
 * header: some of those files are built on their own (a test, a tool). */
#ifndef FATHOM_PATH_H
#define FATHOM_PATH_H

#include <string.h>

/* What follows the last '.' of the path's last part ("flac" of
 * "/music/a.b/song.flac"): "" if that part has no '.'. */
static inline const char *path_ext(const char *path)
{
    const char *dot = strrchr(path, '.');
    const char *slash = strrchr(path, '/');
    return (dot && (!slash || dot > slash)) ? dot + 1 : "";
}

#endif
