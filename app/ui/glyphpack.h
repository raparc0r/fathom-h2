/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/*
 * A character pack: the characters of a language the built-in fonts lack
 * (Japanese to begin with), drawn beforehand in square cells of a few sizes
 * (tools/fonts/mkpack.py; the file's layout is in tools/fonts/packdraw.c).
 * The player works nothing out: it maps the file and copies cells.
 *
 * The file is mapped read-only, never read whole: only the pages of the
 * characters shown are in memory, and the kernel may drop them. It can come
 * from anywhere (a download, a copy from a computer), so everything in it is
 * checked when it is opened and nothing in it is trusted after that.
 */
#ifndef FATHOM_GLYPHPACK_H
#define FATHOM_GLYPHPACK_H

#include <stddef.h>
#include <stdint.h>

#define GLYPHPACK_MAX_SIZES 8

typedef struct glyphpack glyphpack_t;

/* NULL: no such file, or not a pack this reads (why, to stderr, unless it is absent) */
glyphpack_t *glyphpack_open(const char *path);
void         glyphpack_close(glyphpack_t *p);

int glyphpack_count(const glyphpack_t *p);
/* The cell sizes held, smallest first: px[] filled (GLYPHPACK_MAX_SIZES at most); how many */
int glyphpack_sizes(const glyphpack_t *p, int *px);
/* The size to draw text of `want` pixels with: the nearest, the smaller of two
 * as near, and never more than a pixel over. Its place among glyphpack_sizes. */
int glyphpack_size_for(const glyphpack_t *p, int want);
/* How far under the baseline a cell's foot is, at that size (0 or less) */
int glyphpack_foot(const glyphpack_t *p, int size);

/* The character's number (its cell's place), or -1: not in the pack. *half: it is
 * half a cell wide (the half-width katakana). Touches the index only. */
int glyphpack_find(const glyphpack_t *p, uint32_t cp, int *half);

/* The cell: rows from the top, two pixels a byte, the left one in the high half
 * (0: nothing, 15: full). *row_bytes: the bytes of a row. NULL: no such cell. */
const uint8_t *glyphpack_cell(const glyphpack_t *p, int size, int number, int *row_bytes);

/* ---- never waiting for the card ----
 * A cell's page may not be in memory (the first time, or after the kernel let
 * it go), and reading it takes the card about a millisecond: not on the
 * thread that draws. glyphpack_cell_ready gives the cell only if its pages
 * are in memory now; if not it returns NULL at once, and the pack's own thread
 * fetches them. glyphpack_arrivals counts the pages that thread has brought
 * in: when it has changed, draw again. glyphpack_want asks ahead, the same
 * way, without giving anything (when a text is laid out, before it is drawn).
 * The first use of a size also brings in its commonest cells (kana, symbols).
 * glyphpack_pending: pages asked for and not there yet. */
const uint8_t *glyphpack_cell_ready(glyphpack_t *p, int size, int number, int *row_bytes);
void           glyphpack_want(glyphpack_t *p, int size, int number);
unsigned       glyphpack_arrivals(const glyphpack_t *p);
int            glyphpack_pending(const glyphpack_t *p);

/* Tests. forget: nothing counts as in memory until this pack's thread has
 * fetched it (the kernel isn't asked); page_delay_ms: that thread takes this
 * long over each page (a slow card). */
void glyphpack_test(glyphpack_t *p, int forget, int page_delay_ms);

#endif
