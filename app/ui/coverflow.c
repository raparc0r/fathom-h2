/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* Albums as cover flow: the picture of it (coverflow.h). */
#include "ui/coverflow.h"

#include <stdlib.h>

/* The layout, in pixels of the picture. The middle cover; then each side's
 * covers, TURNED wide, the first GAP from the middle one and the rest PITCH
 * apart (they overlap, the nearer over the further), NEAR tall at the edge
 * away from the middle and FAR at the one towards it. */
enum {
    MID_X = (FLOW_W - FLOW_COVER) / 2, MID_Y = 6, MID_CY = MID_Y + FLOW_COVER / 2,
    GAP = 8, TURNED = 36, PITCH = 26, NEAR = 118, FAR = 92,
    KEEP_FIRST = 190, KEEP_LESS = 40,    /* how much of a turned cover's own colour shows (of 256), and less a place */
    RADIUS = 9,
};

typedef struct { int xl, w, hl, hr, keep; } quad_t;   /* its left edge, width, heights at its left and right, colour kept */

/* A turned cover a places out from the first (in FLOW_STEPs), on the right or the left */
static quad_t turned(int a, int right)
{
    quad_t q;
    int out = a * PITCH / FLOW_STEP;
    q.w = TURNED;
    q.xl = right ? MID_X + FLOW_COVER + GAP + out : MID_X - GAP - TURNED - out;
    q.hl = right ? FAR : NEAR;
    q.hr = right ? NEAR : FAR;
    /* three show; the fourth fades in from nothing as it comes */
    int k = KEEP_FIRST - KEEP_LESS * a / FLOW_STEP, last = KEEP_FIRST - KEEP_LESS * (FLOW_REACH - 2);
    if (a > (FLOW_REACH - 2) * FLOW_STEP) k = last - last * (a - (FLOW_REACH - 2) * FLOW_STEP) / FLOW_STEP;
    q.keep = k < 0 ? 0 : k;
    return q;
}

/* The cover at place x (FLOW_STEPs from the middle, either way): the middle
 * one, a turned one, or on its way between the two */
static quad_t quad_at(int x)
{
    int ax = abs(x);
    if (ax >= FLOW_STEP) return turned(ax - FLOW_STEP, x > 0);
    quad_t mid = {MID_X, FLOW_COVER, FLOW_COVER, FLOW_COVER, 256}, side = turned(0, x > 0), q;
    q.xl = mid.xl + (side.xl - mid.xl) * ax / FLOW_STEP;
    q.w = mid.w + (side.w - mid.w) * ax / FLOW_STEP;
    q.hl = mid.hl + (side.hl - mid.hl) * ax / FLOW_STEP;
    q.hr = mid.hr + (side.hr - mid.hr) * ax / FLOW_STEP;
    q.keep = mid.keep + (side.keep - mid.keep) * ax / FLOW_STEP;
    return q;
}

static inline uint16_t dimmed(uint16_t c, uint16_t bg, int keep)
{
    int r = c >> 11, g = (c >> 5) & 63, b = c & 31, br = bg >> 11, bgn = (bg >> 5) & 63, bb = bg & 31;
    r = br + (((r - br) * keep) >> 8);
    g = bgn + (((g - bgn) * keep) >> 8);
    b = bb + (((b - bb) * keep) >> 8);
    return (uint16_t)((r << 11) | (g << 5) | b);
}

/* One cover as its quad: a column of the picture at a time. Across, the
 * cover is spread as a turned square's is (what is nearer takes more of the
 * width); down, each column is the cover's shrunk to its height. */
static void draw_quad(uint16_t *out, const flow_cover_t *c, quad_t q, uint16_t bg, uint16_t blank)
{
    if (q.w < 1 || q.keep <= 0) return;
    const uint16_t *src = c->full;
    int sw = FLOW_COVER;
    if (c->full && c->narrow && q.w <= FLOW_NARROW + FLOW_NARROW / 2) { src = c->narrow; sw = FLOW_NARROW; }
    uint16_t plain = q.keep >= 256 ? blank : dimmed(blank, bg, q.keep);
    for (int col = 0; col < q.w; col++) {
        int x = q.xl + col;
        if (x < 0 || x >= FLOW_W) continue;
        int h = q.hl, sx = 0;
        if (q.w > 1) {
            long den = (long)q.hl * (q.w - 1 - col) + (long)q.hr * col;      /* (its height here, times w - 1) */
            h = (int)((den + (q.w - 1) / 2) / (q.w - 1));
            sx = (int)(((long)col * q.hr * (sw - 1) + den / 2) / den);
        }
        if (h < 1) continue;
        if (sx > sw - 1) sx = sw - 1;
        int y0 = MID_CY - h / 2;
        uint32_t step = ((uint32_t)FLOW_COVER << 16) / (uint32_t)h, pos = step / 2;
        for (int r = 0; r < h; r++, pos += step) {
            int y = y0 + r;
            if (y < 0 || y >= FLOW_H) continue;
            uint16_t *o = out + (size_t)y * FLOW_W + x;
            if (!src) { *o = plain; continue; }
            uint32_t sy = pos >> 16;
            if (sy > FLOW_COVER - 1) sy = FLOW_COVER - 1;
            uint16_t px = src[sy * (uint32_t)sw + (uint32_t)sx];
            *o = q.keep >= 256 ? px : dimmed(px, bg, q.keep);
        }
    }
}

void flow_narrow(const uint16_t *full, uint16_t *narrow)
{
    for (int y = 0; y < FLOW_COVER; y++)
        for (int x = 0; x < FLOW_NARROW; x++) {
            int x0 = x * FLOW_COVER / FLOW_NARROW, x1 = (x + 1) * FLOW_COVER / FLOW_NARROW;
            unsigned r = 0, g = 0, b = 0, n = (unsigned)(x1 - x0);
            for (int sx = x0; sx < x1; sx++) {
                uint16_t c = full[y * FLOW_COVER + sx];
                r += c >> 11, g += (c >> 5) & 63, b += c & 31;
            }
            narrow[y * FLOW_NARROW + x] = (uint16_t)(((r / n) << 11) | ((g / n) << 5) | (b / n));
        }
}

void flow_draw(uint16_t *out, const flow_cover_t cov[FLOW_N], int shift, uint16_t bg, uint16_t blank, int round)
{
    if (shift > FLOW_SHIFT_MAX) shift = FLOW_SHIFT_MAX;
    if (shift < -FLOW_SHIFT_MAX) shift = -FLOW_SHIFT_MAX;
    for (int i = 0; i < FLOW_W * FLOW_H; i++) out[i] = bg;
    /* from the furthest out to the middle: the nearer goes over the further.
     * (Nine places a FLOW_STEP apart: the furthest are at the two ends, and
     * the nearest is whichever the shift has brought closest.) */
    int order[FLOW_N], x[FLOW_N];
    for (int i = 0; i < FLOW_N; i++) {
        order[i] = i;
        x[i] = (i - FLOW_REACH) * FLOW_STEP + shift;
    }
    for (int i = 1; i < FLOW_N; i++)
        for (int j = i; j > 0 && abs(x[order[j]]) > abs(x[order[j - 1]]); j--) {
            int t = order[j];
            order[j] = order[j - 1];
            order[j - 1] = t;
        }
    for (int i = 0; i < FLOW_N; i++) {
        int k = order[i];
        if (!cov[k].there || abs(x[k]) >= FLOW_REACH * FLOW_STEP) continue;   /* (that far out: faded to nothing) */
        draw_quad(out, &cov[k], quad_at(x[k]), bg, blank);
    }
    if (round && shift == 0 && cov[FLOW_REACH].there)
        for (int cy = 0; cy < RADIUS; cy++)
            for (int cx = 0; cx < RADIUS; cx++) {
                int dx = RADIUS - cx, dy = RADIUS - cy;
                if (dx * dx + dy * dy <= RADIUS * RADIUS) continue;
                int x0 = MID_X + cx, x1 = MID_X + FLOW_COVER - 1 - cx, y0 = MID_Y + cy, y1 = MID_Y + FLOW_COVER - 1 - cy;
                out[y0 * FLOW_W + x0] = out[y0 * FLOW_W + x1] = out[y1 * FLOW_W + x0] = out[y1 * FLOW_W + x1] = bg;
            }
}
