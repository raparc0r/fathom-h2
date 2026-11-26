/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/*
 * Albums as cover flow: the picture of it. The album that's chosen in the
 * middle at its cover's own size, three at each side turned towards it (drawn
 * in perspective: taller at the edge away from the middle), dimmer the further
 * out, and the slide from one album to the next. Drawn here, a pixel at a
 * time, into one RGB565 picture the UI shows (LVGL has no perspective, and
 * its scaled images cost more than this does): whole numbers only, the H2's
 * build has no floating point to spare.
 *
 * Nothing of LVGL or the library: the UI gives the covers' pixels.
 */
#ifndef FATHOM_UI_COVERFLOW_H
#define FATHOM_UI_COVERFLOW_H

#include <stdint.h>

#define FLOW_W      320     /* the picture: the screen's width */
#define FLOW_H      148     /* ...and the covers' band */
#define FLOW_COVER  136     /* a cover's side, as the library keeps it (LIB_ART_LARGE) */
#define FLOW_NARROW 48      /* a cover shrunk across, for the turned ones (flow_narrow) */
#define FLOW_REACH  4       /* covers each side of the middle one that can show: 3 at rest, a 4th on its way in */
#define FLOW_N      (2 * FLOW_REACH + 1)
#define FLOW_STEP   256     /* a slide's unit: one album's place */
#define FLOW_SHIFT_MAX (FLOW_STEP * 3 / 2)   /* how far from rest a slide may be: a turn made before the last slide was over */

/* The album at one place: there (0: past the list's end, nothing drawn), and
 * its cover, FLOW_COVER square (NULL: none, a plain tile) with the same
 * shrunk to FLOW_NARROW across (NULL: taken from the full one, coarser). */
typedef struct {
    int there;
    const uint16_t *full, *narrow;
} flow_cover_t;

/* full (FLOW_COVER x FLOW_COVER) shrunk across into narrow (FLOW_NARROW wide,
 * FLOW_COVER tall): each pixel the average of those it stands for. */
void flow_narrow(const uint16_t *full, uint16_t *narrow);

/* The picture into out (FLOW_W x FLOW_H). cov[FLOW_REACH] is the chosen
 * album, cov[FLOW_REACH + k] the one k places after it. shift: where the row
 * is on its way there, in FLOW_STEPs (0 at rest; FLOW_STEP: one place to the
 * right of rest, as when the next album has just been chosen; between
 * -FLOW_SHIFT_MAX and FLOW_SHIFT_MAX). bg: the ground; blank: an album with no cover;
 * round: the middle cover's corners rounded off, at rest. */
void flow_draw(uint16_t *out, const flow_cover_t cov[FLOW_N], int shift, uint16_t bg, uint16_t blank, int round);

#endif
