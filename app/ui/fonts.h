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

/* Character packs (ui/glyphpack.h): the characters no built-in font has, drawn
 * beforehand. ui_packs_dir: the folder whose glyphs-*.bin are used (NULL:
 * none); packs open are closed, and the folder is looked in again when a
 * character next needs one. Nothing is opened before that: a library with no
 * such character costs nothing.
 * ui_font_pack: the font to put last in a chain, for text of about px pixels:
 * it has what the packs have, in the cell size nearest to px. Always the same
 * font for the same px; NULL when there is no room for another. */
void             ui_packs_dir(const char *dir);
/* The pack whose characters are used where several packs have the same one
 * (Chinese and Japanese share thousands, each drawing them its own way):
 * "ja", "zh-hans"... as in its file's name, glyphs-ID.bin. "" or NULL: none
 * said, and the packs are asked in their names' order. */
void             ui_packs_first(const char *id);
const lv_font_t *ui_font_pack(int px);
/* Tests: the packs as if nothing of them were in memory and the card took
 * page_delay_ms over each page (ui/glyphpack.h, glyphpack_test); 0, 0: as they are. */
void             ui_packs_test(int forget, int page_delay_ms);
/* ...and what has happened since the start: characters drawn empty (their page
 * not in memory), characters drawn from their cell, and the times the screen
 * was asked to draw again because pages had come. */
void             ui_packs_counts(unsigned *empty, unsigned *drawn, unsigned *redraws);
/* 1 once a character a pack would draw (kana, kanji, Hangul) has been met with no pack
 * that has it: the user may want to be told there are packs. clear: asked
 * again only after the next such character. */
int              ui_packs_wanted(int clear);

/* Code-point coverage of a TrueType/OpenType file (exposed for tests and tools). */
typedef struct font_cov font_cov_t;
font_cov_t *font_cov_new(const void *ttf, size_t len);   /* NULL if no usable cmap */
int         font_cov_has(const font_cov_t *c, uint32_t cp);
void        font_cov_free(font_cov_t *c);

#endif
