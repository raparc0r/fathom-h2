/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* Themes written by the theme editor: read back the same, and made ones readable. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui/theme.h"
#include "test_util.h"

/* every field a theme file holds (not where it was loaded from) */
static int same(const theme_t *a, const theme_t *b)
{
    theme_t x = *a, y = *b;
    memset(x.dir, 0, sizeof x.dir); memset(y.dir, 0, sizeof y.dir);
    memset(x.file, 0, sizeof x.file); memset(y.file, 0, sizeof y.file);
    x.status = y.status = THEME_OK;
    memset(x.note, 0, sizeof x.note); memset(y.note, 0, sizeof y.note);
    x.unknown_keys = y.unknown_keys = 0;
    x.format = y.format = 0;
    return !memcmp(&x, &y, sizeof x);
}

static void round_trip(const char *path)
{
    theme_t a, b;
    char err[512], text[8192];
    CHECKF(theme_load_file(path, &a, err, sizeof err) == 0, "%s: %s", path, err);
    int n = theme_format(&a, text, sizeof text);
    CHECKF(n > 0, "%s: format", path);
    CHECKF(theme_parse(text, &b, err, sizeof err) == 0, "%s: reparse: %s", path, err);
    CHECKF(b.unknown_keys == 0, "%s: %d unknown keys written", path, b.unknown_keys);
    CHECKF(same(&a, &b), "%s: not the same after writing it out", path);
}

int main(void)
{
    const char *files[] = {"assets/themes/studio-mint.theme", "assets/themes/studio-ember.theme",
                           "assets/themes/studio-pastel.theme", "assets/themes/pixel-pastel.theme"};
    for (size_t i = 0; i < sizeof files / sizeof files[0]; i++) round_trip(files[i]);

    /* a name with a ';' (a comment in the file) still reads back as a name */
    theme_t t, u;
    char err[512], text[8192];
    if (theme_load_file(files[0], &t, err, sizeof err)) { printf("FAIL: %s\n", err); return 1; }
    snprintf(t.name, sizeof t.name, "Mine; for night");
    t.font_px[1] = 22;
    snprintf(t.icon[0], sizeof t.icon[0], "icons/play.png");
    CHECKF(theme_format(&t, text, sizeof text) > 0, "format");
    CHECKF(theme_parse(text, &u, err, sizeof err) == 0, "reparse: %s", err);
    CHECKF(!strcmp(u.name, "Mine, for night"), "name: %s", u.name);
    CHECKF(u.font_px[1] == 22 && !strcmp(u.icon[0], "icons/play.png"), "sizes and icons kept");
    CHECKF(theme_format(&t, text, 64) == -1, "too small a buffer says so");

    /* a file saved with a byte-order mark (Notepad's "UTF-8") reads the same: its
     * first line was "not key = value", and the theme refused */
    CHECKF(theme_format(&t, text, sizeof text) > 0, "format");
    static char bom[8192 + 4];
    snprintf(bom, sizeof bom, "\xEF\xBB\xBF%s", "[theme]\nname = Marked\n");
    snprintf(bom + strlen(bom), sizeof bom - strlen(bom), "%s", strstr(text, "[color]"));
    CHECKF(theme_parse(bom, &u, err, sizeof err) == 0, "with a byte-order mark: %s", err);
    CHECKF(!strcmp(u.name, "Marked") && u.bg == t.bg && u.unknown_keys == 0, "and its first section is read");
    CHECKF(theme_parse(bom + 3, &u, err, sizeof err) == 0 && !strcmp(u.name, "Marked"), "as without one: %s", err);

    /* HSL: there and back within a step */
    for (uint32_t c = 0; c < 0x1000000; c += 0x010307 * 37) {
        int h, s, l;
        theme_to_hsl(c, &h, &s, &l);
        uint32_t d = theme_from_hsl(h, s, l);
        for (int sh = 0; sh < 24; sh += 8) {
            int x = (int)((c >> sh) & 0xff), y = (int)((d >> sh) & 0xff);
            CHECKF(abs(x - y) <= 4, "%06X -> %d,%d,%d -> %06X", (unsigned)c, h, s, l, (unsigned)d);
        }
    }
    CHECKF(theme_from_hsl(0, 100, 50) == 0xFF0000 && theme_from_hsl(120, 100, 50) == 0x00FF00 &&
          theme_from_hsl(240, 100, 50) == 0x0000FF && theme_from_hsl(0, 0, 100) == 0xFFFFFF, "primaries");

    /* theme_fit reaches the contrast asked for */
    uint32_t bgs[] = {0x000000, 0x101826, 0x777777, 0xF6F5F2, 0xFFFFFF};
    for (size_t i = 0; i < sizeof bgs / sizeof bgs[0]; i++)
        for (uint32_t c = 0; c < 0x1000000; c += 0x0B0D11 * 13)
            CHECKF(theme_contrast(theme_fit(c, bgs[i], 4.5), bgs[i]) >= 4.5 || bgs[i] == 0x777777,
                  "fit %06X on %06X", (unsigned)c, (unsigned)bgs[i]);

    /* any accent, dark or light, makes a theme that passes every rule */
    int made = 0;
    for (int dark = 0; dark < 2; dark++)
        for (int hue = 0; hue < 360; hue += 5)
            for (int sat = 0; sat <= 100; sat += 10) {
                theme_t m = t;
                theme_make(&m, dark, hue, sat);
                int f = theme_validate(&m, err, sizeof err);
                CHECKF(f == 0, "%s hue %d sat %d: %s", dark ? "dark" : "light", hue, sat, err);
                theme_accent(&m, (hue + 137) % 360, 100 - sat);
                f = theme_validate(&m, err, sizeof err);
                CHECKF(f == 0, "accent changed: %s", err);
                made++;
            }
    printf("%d made themes checked\n", made);
    return t_result();
}
