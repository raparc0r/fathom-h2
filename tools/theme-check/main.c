/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/*
 * theme-check: check themes against the format and the legibility rules
 * (THEMES.md), the same way the player does when it lists them.
 *
 *   theme-check [--strict] THEME...
 *
 * THEME is a NAME.theme file or a folder holding theme.ini (and its fonts).
 * Errors (the player can't use the theme) always fail; warnings (it loads,
 * with a note) fail only with --strict, as for the built-in themes.
 * Font and icon files are checked for existence here; glyph coverage and icon
 * sizes are checked by the player (and the simulator) when it lists the theme.
 */
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "../../app/ui/theme.h"

int main(int argc, char **argv)
{
    int strict = 0, errors = 0, warnings = 0, n = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--strict")) { strict = 1; continue; }
        n++;
        char path[1200], dir[1024];
        struct stat st;
        if (stat(argv[i], &st) == 0 && S_ISDIR(st.st_mode)) {
            snprintf(dir, sizeof dir, "%s", argv[i]);
            snprintf(path, sizeof path, "%s/theme.ini", argv[i]);
        } else {
            snprintf(path, sizeof path, "%s", argv[i]);
            snprintf(dir, sizeof dir, "%s", argv[i]);
            char *slash = strrchr(dir, '/');
            if (slash) *slash = '\0';
            else snprintf(dir, sizeof dir, ".");
        }
        theme_t t;
        char err[2048];
        if (theme_load_file(path, &t, err, sizeof err)) {
            printf("ERROR %s\n%s", argv[i], err);
            errors++;
            continue;
        }
        int bad = 0;
        const char *files[2] = {t.font_file, t.font_file_title};
        for (int f = 0; f < 2; f++) {
            if (!files[f][0]) continue;
            char fp[2400];
            snprintf(fp, sizeof fp, "%s/%s", dir, files[f]);
            if (stat(fp, &st) != 0) { printf("ERROR %s: font file %s not found\n", argv[i], files[f]); bad = 1; }
        }
        for (int ic = 0; ic < THEME_ICON_N; ic++) {
            if (!t.icon[ic][0]) continue;
            char fp[2400];
            snprintf(fp, sizeof fp, "%s/%s", dir, t.icon[ic]);
            if (stat(fp, &st) != 0) { printf("ERROR %s: icon %s not found\n", argv[i], t.icon[ic]); bad = 1; }
        }
        if (bad) { errors++; continue; }
        int w = theme_validate(&t, err, sizeof err);
        printf("%s %s (%s): text %.1f, muted %.1f, selection %.1f, signal %.1f%s\n%s",
               w ? "WARN " : "ok   ", argv[i], t.name,
               theme_contrast(t.text, t.bg), theme_contrast(t.text_muted, t.bg),
               theme_contrast(t.select_fg, t.select_bg), theme_contrast(t.signal, t.bg),
               t.unknown_keys ? " (some settings ignored: newer format?)" : "", err);
        warnings += w != 0;
    }
    if (!n) { fprintf(stderr, "usage: %s [--strict] THEME.theme|THEME_DIR...\n", argv[0]); return 2; }
    return errors || (strict && warnings) ? 1 : 0;
}
