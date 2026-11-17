/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Themes and fonts. Built-in themes come from the assets folder (assets/ in the
 * repo, fathom/assets on the H2's card); contributed ones from a themes folder
 * on the card (fathom/themes), each as NAME.theme or a folder NAME/ holding
 * theme.ini and its fonts. Fonts are loaded only when a theme that uses them is
 * shown, and kept for next time (32 MiB of RAM).
 */
#ifndef FATHOM_UI_ASSETS_H
#define FATHOM_UI_ASSETS_H

#include "ui/theme.h"
#include "ui/ui.h"

#define UI_MAX_THEMES 32

/* The theme catalogue: the built-in themes first (Studio Mint, the default,
 * at index 0), then the contributed ones by name. Each has its status and a
 * note (the first warning or the error). user_dir may be NULL. Returns the
 * count, or -1 if not even a built-in theme loads. */
int ui_load_themes(const char *assets_dir, const char *user_dir, theme_t *themes, int max);

/* The fonts for a theme (its own files, or a built-in family). NULL if they
 * can't be loaded. Suitable as ui_hooks_t.fonts. */
const ui_fonts_t *ui_theme_fonts(const theme_t *t);

/* A theme's own header icon (THEME_ICON_*), decoded once; NULL if it has none
 * (or it can't be used): the built-in symbol is drawn instead. Suitable as
 * ui_hooks_t.icon. */
const lv_image_dsc_t *ui_theme_icon(const theme_t *t, int which);

/* The boot logo (logo.png in the assets folder, 320x240), decoded; NULL if
 * it's missing. ui_logo_release() frees it once nothing draws it. */
const lv_image_dsc_t *ui_logo(void);
void ui_logo_release(void);

#endif
