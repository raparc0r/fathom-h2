/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* Albums as cover flow: the picture (app/ui/coverflow.c). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui/coverflow.h"
#include "test_util.h"

enum { GUARD = 4096, MID_X = (FLOW_W - FLOW_COVER) / 2, MID_Y = 6 };
static uint16_t store[GUARD + FLOW_W * FLOW_H + GUARD];
static uint16_t *const out = store + GUARD;
static uint16_t full[FLOW_N][FLOW_COVER * FLOW_COVER], narrow[FLOW_N][FLOW_NARROW * FLOW_COVER];
static const uint16_t BG = 0x0841, BLANK = 0x39E7, MARK = 0xA5A5;

/* each album's cover one plain colour of its own (its place in the row in the red and the blue) */
static uint16_t colour_of(int i) { return (uint16_t)(((8 + 2 * i) << 11) | (40 << 5) | (28 - 2 * i)); }

static void guards_set(void)
{
    for (int i = 0; i < GUARD; i++) store[i] = store[GUARD + FLOW_W * FLOW_H + i] = MARK;
}

static int guards_whole(void)
{
    for (int i = 0; i < GUARD; i++)
        if (store[i] != MARK || store[GUARD + FLOW_W * FLOW_H + i] != MARK) return 0;
    return 1;
}

static int count(uint16_t c)
{
    int n = 0;
    for (int i = 0; i < FLOW_W * FLOW_H; i++) n += out[i] == c;
    return n;
}

int main(void)
{
    flow_cover_t cov[FLOW_N];
    for (int i = 0; i < FLOW_N; i++) {
        for (int p = 0; p < FLOW_COVER * FLOW_COVER; p++) full[i][p] = colour_of(i);
        flow_narrow(full[i], narrow[i]);
        cov[i] = (flow_cover_t){1, full[i], narrow[i]};
    }
    /* a plain cover shrunk across is the same colour */
    CHECK(narrow[2][0] == colour_of(2) && narrow[2][FLOW_NARROW * FLOW_COVER - 1] == colour_of(2));
    /* ...and a real one's columns are averaged: black and white stripes a pixel wide come out grey */
    {
        static uint16_t stripes[FLOW_COVER * FLOW_COVER], thin[FLOW_NARROW * FLOW_COVER];
        for (int p = 0; p < FLOW_COVER * FLOW_COVER; p++) stripes[p] = p % FLOW_COVER % 2 ? 0xFFFF : 0;
        flow_narrow(stripes, thin);
        int r = thin[10] >> 11;
        CHECK(r >= 8 && r <= 23);
    }

    /* At rest: the chosen album's cover as it is, pixel for pixel, in the middle */
    guards_set();
    for (int p = 0; p < FLOW_COVER * FLOW_COVER; p++) full[FLOW_REACH][p] = (uint16_t)(p * 7 + 3);   /* (every pixel its own) */
    flow_draw(out, cov, 0, BG, BLANK, 0);
    CHECK(guards_whole());
    int same = 1;
    for (int y = 0; y < FLOW_COVER; y++)
        for (int x = 0; x < FLOW_COVER; x++) same &= out[(MID_Y + y) * FLOW_W + MID_X + x] == full[FLOW_REACH][y * FLOW_COVER + x];
    CHECK(same);
    for (int p = 0; p < FLOW_COVER * FLOW_COVER; p++) full[FLOW_REACH][p] = colour_of(FLOW_REACH);
    flow_draw(out, cov, 0, BG, BLANK, 0);
    /* the ground above and below it, and in the gap at each side */
    CHECK(out[2 * FLOW_W + 160] == BG && out[(FLOW_H - 2) * FLOW_W + 160] == BG);
    CHECK(out[74 * FLOW_W + MID_X - 3] == BG && out[74 * FLOW_W + MID_X + FLOW_COVER + 2] == BG);
    /* three at each side and no more: the fourth is there only on its way in */
    CHECK(count(colour_of(FLOW_REACH)) == FLOW_COVER * FLOW_COVER);
    CHECK(count(colour_of(0)) == 0 && count(colour_of(FLOW_N - 1)) == 0);
    /* the turned ones are dimmed towards the ground (none of them its own colour), each further one more */
    CHECK(count(colour_of(FLOW_REACH - 1)) == 0 && count(colour_of(FLOW_REACH + 1)) == 0);
    {
        /* (the first at the left: just left of the gap; the third: at the screen's edge) */
        uint16_t first = out[74 * FLOW_W + MID_X - 8 - 4], third = out[74 * FLOW_W + MID_X - 8 - 36 - 2 * 26 + 2];
        CHECK(first != BG && third != BG && (first & 0x07E0) > (third & 0x07E0) && (third & 0x07E0) > (BG & 0x07E0));
        /* taller at the edge away from the middle */
        int inner = 0, outer = 0;
        for (int y = 0; y < FLOW_H; y++) {
            inner += out[y * FLOW_W + MID_X - 8 - 1] != BG;
            outer += out[y * FLOW_W + MID_X - 8 - 36] != BG;
        }
        CHECK(inner >= 90 && inner <= 94 && outer >= 116 && outer <= 120);
    }
    /* the two sides mirror each other's shapes */
    {
        int mirrored = 1;
        for (int y = 0; y < FLOW_H; y++)
            for (int x = 0; x < FLOW_W / 2; x++)
                mirrored &= (out[y * FLOW_W + x] == BG) == (out[y * FLOW_W + FLOW_W - 1 - x] == BG);
        CHECK(mirrored);
    }
    /* round: the middle cover's four corners are the ground's; not while it moves */
    flow_draw(out, cov, 0, BG, BLANK, 1);
    CHECK(out[MID_Y * FLOW_W + MID_X] == BG && out[MID_Y * FLOW_W + MID_X + FLOW_COVER - 1] == BG &&
          out[(MID_Y + FLOW_COVER - 1) * FLOW_W + MID_X] == BG && out[(MID_Y + 20) * FLOW_W + MID_X] == colour_of(FLOW_REACH));

    /* The end of a slide is the next album at rest: one place along is the same picture as the row moved by one */
    {
        static uint16_t a[FLOW_W * FLOW_H];
        flow_cover_t moved[FLOW_N];
        for (int i = 0; i < FLOW_N; i++) moved[i] = i + 1 < FLOW_N ? cov[i + 1] : (flow_cover_t){0, NULL, NULL};
        flow_draw(out, moved, 0, BG, BLANK, 0);
        memcpy(a, out, sizeof a);
        flow_draw(out, cov, -FLOW_STEP, BG, BLANK, 0);
        CHECK(!memcmp(a, out, sizeof a));
    }
    /* Every step of a slide, both ways, with and without the shrunk covers, a cover missing and
     * a place past the list's end: nothing written outside the picture, and at least a cover's
     * worth of it is covers (half way, the two that cross the middle stand at each side of it) */
    for (int pass = 0; pass < 2; pass++) {
        flow_cover_t c2[FLOW_N];
        memcpy(c2, cov, sizeof c2);
        if (pass) {
            for (int i = 0; i < FLOW_N; i++) c2[i].narrow = NULL;
            c2[FLOW_REACH + 1].full = NULL;          /* no cover: a plain tile */
            c2[0].there = 0;
        }
        for (int s = -FLOW_SHIFT_MAX - 40; s <= FLOW_SHIFT_MAX + 40; s += 7) {
            guards_set();
            flow_draw(out, c2, s, BG, BLANK, s % 2 == 0);
            CHECK(guards_whole());
            CHECK(FLOW_W * FLOW_H - count(BG) > FLOW_COVER * FLOW_COVER);
        }
    }
    /* an album with no cover: the tile's colour in the middle, dimmed at the side */
    {
        flow_cover_t c3[FLOW_N];
        memcpy(c3, cov, sizeof c3);
        c3[FLOW_REACH].full = c3[FLOW_REACH].narrow = NULL;
        c3[FLOW_REACH - 1].full = c3[FLOW_REACH - 1].narrow = NULL;
        flow_draw(out, c3, 0, BG, BLANK, 0);
        CHECK(out[74 * FLOW_W + 160] == BLANK && out[74 * FLOW_W + MID_X - 8 - 4] != BLANK && out[74 * FLOW_W + MID_X - 8 - 4] != BG);
    }
    /* nothing there at all (an empty list's neighbours): the ground alone */
    {
        flow_cover_t none[FLOW_N];
        memset(none, 0, sizeof none);
        flow_draw(out, none, 0, BG, BLANK, 1);
        CHECK(count(BG) == FLOW_W * FLOW_H);
    }
    return t_result();
}
