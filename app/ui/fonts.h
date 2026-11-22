/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/*
 * TrueType fonts for the UI, with coverage-aware glyph lookup.
 *
 * Tag text can contain any character. Each font knows which code points its
 * file covers (from the cmap table), so a missing character goes quietly to
 * the fallback font. Hyphen and dash variants are drawn with a glyph the font
 * has. A character that no font in the chain covers is drawn as an outlined
 * box, never left blank.
 */
#ifndef FATHOM_UI_FONTS_H
#define FATHOM_UI_FONTS_H

#include <stddef.h>
#include <stdint.h>

#include "lvgl.h"

/* `ttf` must stay valid for the font's lifetime. `fallback` may be NULL. */
lv_font_t *ui_font_ttf(const void *ttf, size_t len, int px, const lv_font_t *fallback);

/* Code-point coverage of a TrueType/OpenType file (exposed for tests and tools). */
typedef struct font_cov font_cov_t;
font_cov_t *font_cov_new(const void *ttf, size_t len);   /* NULL if no usable cmap */
int         font_cov_has(const font_cov_t *c, uint32_t cp);
void        font_cov_free(font_cov_t *c);

#endif
