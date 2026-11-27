/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/*
 * Lyrics: a screen over everything, opened from a song's menu (Now Playing's
 * too). Timed lyrics of the song playing follow it: the line being sung in
 * the middle, in the theme's accent, the ones before above it and the ones to
 * come below, all of them sliding up a line as the song goes on; the wheel
 * reads ahead or back, and it follows again a few seconds after the wheel
 * rests. Lyrics without times, and a song that isn't
 * the one playing, are a page the wheel moves a line at a time. A song with
 * none says so, and Play looks them up online (net/lyricfind.h), with Wi-Fi.
 *
 * Play is the music's as on Now Playing while lyrics show; Back puts the
 * screen away. Menu changes how timed lyrics move: with the song, or only
 * with the wheel (the line being sung still marked), and says which. The
 * song changing changes the lyrics with it.
 */
#include <stdio.h>
#include <string.h>

#include "ui_internal.h"

#include "core/lang.h"
#include "net/lyricfind.h"
#include "net/netclock.h"
#include "player/player.h"

#define LYR_SIDE 14            /* px at each side of the words */
#define LYR_TOP 34             /* the song's title and artist above them */
#define LYR_FOOT 26            /* a line of what the keys do, below (a page's, a song's without lyrics) */
#define LYR_END 8              /* the screen's foot, with no such line (timed lyrics) */
#define LYR_ROOM (UI_H - LYR_TOP - LYR_END)   /* the height timed lyrics have */
#define LYR_GAP 8              /* between the line being sung and its neighbours */
#define LYR_LINE_GAP 2         /* between two of its neighbours */
#define LYR_REST_MS 4000       /* the wheel at rest this long: following the song again */
#define LYR_TICK_MS 100
#define LYR_SLIDE_MS 300       /* the lines moving up one, with the song; */
#define LYR_TURN_MS 140        /* and with the wheel */

static void lyrics_tick(lv_timer_t *t);

/* ---------- the album's cover behind the words ----------
 * Over the whole screen, out of focus, and mostly the theme's background (a
 * fifth cover): the words stay the screen's subject, and readable on any
 * cover. Made once a song from the cover Now Playing shows (136 px a side):
 * blurred there, then stretched to the screen's width with its middle kept,
 * mixed with the background and dithered (a soft dark picture in 16-bit
 * colour shows bands otherwise). An album without a cover: the plain
 * background, as before. */
#define BG_COVER 50            /* of 256: how much of the cover shows (a fifth) */
#define BG_BLUR 4              /* px each way at the cover's own size, twice over */

static void blur_line(uint8_t *p, int n, int step, uint8_t *tmp)
{
    for (int i = 0; i < n; i++) {
        int sum = 0, cnt = 0;
        for (int k = i - BG_BLUR; k <= i + BG_BLUR; k++)
            if (k >= 0 && k < n) { sum += p[k * step]; cnt++; }
        tmp[i] = (uint8_t)(sum / cnt);
    }
    for (int i = 0; i < n; i++) p[i * step] = tmp[i];
}

static uint32_t mix_bg(uint32_t bg, int r, int g, int b)
{
    int br = (int)(bg >> 16 & 255), bgn = (int)(bg >> 8 & 255), bb = (int)(bg & 255);
    r = (br * (256 - BG_COVER) + r * BG_COVER) >> 8;
    g = (bgn * (256 - BG_COVER) + g * BG_COVER) >> 8;
    b = (bb * (256 - BG_COVER) + b * BG_COVER) >> 8;
    return (uint32_t)(r << 16 | g << 8 | b);
}

static void backdrop(int64_t album)
{
    enum { N = LIB_ART_LARGE };
    if (S.lyr_bg_px && album == S.lyr_bg_album) return;          /* (the same album's next song) */
    S.lyr_bg_album = album;
    S.lyr_bg_on = 0;
    int64_t t0 = clock_ms();
    uint16_t *src = malloc(N * N * sizeof *src);
    uint8_t *c = malloc(N * N * 3 + N);                            /* its three colours apart, and a line to blur through */
    if (!S.lyr_bg_px) S.lyr_bg_px = malloc(UI_W * UI_H * sizeof *S.lyr_bg_px);
    if (src && c && S.lyr_bg_px && S.lib && album > 0 && library_art(S.lib, album, N, src) == 0) {
        uint32_t bg = T()->bg;
        for (int i = 0; i < N * N; i++) {
            uint16_t px = src[i];
            c[i] = (uint8_t)((px >> 11) * 255 / 31);
            c[N * N + i] = (uint8_t)((px >> 5 & 63) * 255 / 63);
            c[2 * N * N + i] = (uint8_t)((px & 31) * 255 / 31);
        }
        uint8_t *tmp = c + 3 * N * N;
        for (int ch = 0; ch < 3; ch++)
            for (int pass = 0; pass < 2; pass++) {
                for (int y = 0; y < N; y++) blur_line(c + ch * N * N + y * N, N, 1, tmp);
                for (int x = 0; x < N; x++) blur_line(c + ch * N * N + x, N, N, tmp);
            }
        /* the screen's width across the cover, the screen's height from its
         * middle; each point between four of the cover's (16.16) */
        static const uint8_t DITHER[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
        const int32_t step = (int32_t)(((int64_t)(N - 1) << 16) / (UI_W - 1));
        int far = 0;                                               /* the point furthest from the background's own colour */
        uint32_t ink = bg;
        for (int y = 0; y < UI_H; y++) {
            int32_t fy = (y + (UI_W - UI_H) / 2) * step;
            int y0 = fy >> 16, wy = (fy >> 8) & 255, y1 = y0 + 1 < N ? y0 + 1 : y0;
            for (int x = 0; x < UI_W; x++) {
                int32_t fx = x * step;
                int x0 = fx >> 16, wx = (fx >> 8) & 255, x1 = x0 + 1 < N ? x0 + 1 : x0, v[3];
                for (int ch = 0; ch < 3; ch++) {
                    const uint8_t *p = c + ch * N * N;
                    int a = p[y0 * N + x0] * (256 - wx) + p[y0 * N + x1] * wx;
                    int b = p[y1 * N + x0] * (256 - wx) + p[y1 * N + x1] * wx;
                    v[ch] = (a * (256 - wy) + b * wy) >> 16;
                }
                uint32_t m = mix_bg(bg, v[0], v[1], v[2]);
                /* (each colour dithered on its own grid: together they made a pattern of light and dark dots) */
                int r = (int)(m >> 16), g = (int)(m >> 8 & 255), b = (int)(m & 255);
                int dr = DITHER[y & 3][x & 3], dg = DITHER[(y + 2) & 3][(x + 1) & 3], db = DITHER[(y + 1) & 3][(x + 2) & 3];
                int off = abs(r - (int)(bg >> 16)) + abs(g - (int)(bg >> 8 & 255)) + abs(b - (int)(bg & 255));
                if (off > far) { far = off; ink = m; }
                r = r + (dr >> 1) > 255 ? 255 : r + (dr >> 1);     /* (5 bits kept: 8 levels between two) */
                g = g + (dg >> 2) > 255 ? 255 : g + (dg >> 2);     /* (6 bits: 4) */
                b = b + (db >> 1) > 255 ? 255 : b + (db >> 1);
                S.lyr_bg_px[y * UI_W + x] = (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3));
            }
        }
        S.lyr_bg_ink = ink;
        S.lyr_bg_on = 1;
        image_dsc(&S.lyr_bg_dsc, LV_COLOR_FORMAT_RGB565, UI_W, UI_H, S.lyr_bg_px, UI_W * UI_H * sizeof *S.lyr_bg_px);
        lv_image_cache_drop(&S.lyr_bg_dsc);                        /* (the same descriptor, other pixels) */
        lv_image_set_src(S.lyr_bg, &S.lyr_bg_dsc);
        lv_obj_invalidate(S.lyr_bg);
    }
    free(src);
    free(c);
    show(S.lyr_bg, S.lyr_bg_on);
    /* (on the interface's own thread, once a song: said when it is long enough to be felt) */
    if (clock_ms() - t0 >= 100) fprintf(stderr, "lyrics: the cover behind the words took %d ms\n", (int)(clock_ms() - t0));
}

/* A colour of the theme's for words on this screen: as it is on the plain
 * background; over a cover, kept clear of the cover where it comes nearest */
static uint32_t ink(uint32_t colour, double clear)
{
    return S.lyr_bg_on ? theme_fit(colour, S.lyr_bg_ink, clear) : colour;
}

static lv_obj_t *words(lv_obj_t *parent, const lv_font_t *font, uint32_t colour)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    style_label(l, font, colour);
    lv_obj_set_width(l, LV_PCT(100));
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_label_set_text(l, "");
    return l;
}

/* Lines a..b (those there are) one under another, an empty one as a line of
 * its own height */
static void lines_text(char *out, size_t cap, int a, int b)
{
    size_t w = 0;
    out[0] = '\0';
    for (int i = a < 0 ? 0 : a; i <= b && i < S.lyr.n && w + 2 < cap; i++) {
        const char *s = lyrics_line(&S.lyr, i);
        w += (size_t)snprintf(out + w, cap - w, "%s%s", w ? "\n" : "", s[0] ? s : " ");
        if (w >= cap) w = cap - 1;
    }
}

static int text_h(const char *s, const lv_font_t *font, int width)
{
    lv_point_t sz;
    if (!s[0]) return 0;
    lv_text_get_size(&sz, s, font, 0, lv_obj_get_style_text_line_space(S.lyr_page, 0), width, LV_TEXT_FLAG_NONE);
    return sz.y;
}

/* The lines are moving with the song now: its lyrics, timed, not set to the
 * wheel alone (Menu), and the wheel at rest */
static int with_song(void) { return S.lyr_follow && S.lyr_auto && !S.lyr_manual; }

/* The line the song is at, or -1: before the first, or not a song being followed */
static int sung_line(void)
{
    if (!S.lyr_follow || !S.lyr.synced || !S.player) return -1;
    player_now_t n;
    player_now(S.player, &n);
    if (!n.active || n.radio || n.podcast || n.track.id != S.lyr_track) return -1;
    return lyrics_at(&S.lyr, (int32_t)n.pos_ms);
}

/* ---------- timed lyrics: a strip of lines ---------- */

/* The strip is three times the height that shows, the part that shows in its
 * middle: the lines a slide brings in or takes out are inside it, a screen's
 * worth each way. (It was the height that shows, its lines allowed past its
 * edges: moved, only its own box was drawn again, and the part of the screen
 * the lines came from showed none while the wheel turned fast. On the H2.) */
/* The window the strip shows through: rows top to bottom of the height timed
 * lyrics have (all of it while they slide), the strip kept where it was */
static void win_set(int top, int bottom)
{
    lv_obj_set_y(S.lyr_win, top);
    lv_obj_set_height(S.lyr_win, bottom > top ? bottom - top : 0);
    lv_obj_set_y(S.lyr_roll, S.lyr_shift - LYR_ROOM - top);
}

static void anim_roll(void *var, int32_t v)
{
    (void)var;
    S.lyr_shift = quantize(v, 0);
    win_set(0, LYR_ROOM);
}

/* The strip at rest: every row of words that fits whole is shown, a long
 * line's first or last rows too when the rest of it is past the edge; the
 * window ends between two rows, so no letter is cut. (A line that didn't fit
 * whole was not shown at all: one that takes two or three rows slid into
 * view and was gone when the slide ended. The owner, on the H2.) While they
 * slide they come and go past the edges. */
static void roll_rest(void)
{
    int room = LYR_ROOM, top = 0, bottom = room;
    int space = lv_obj_get_style_text_line_space(S.lyr_page, 0);
    S.lyr_shift = 0;
    for (int k = 0; k < LYR_SLOTS; k++) {
        show(S.lyr_slot[k], S.lyr_slot_line[k] >= 0);
        if (S.lyr_slot_line[k] < 0) continue;
        int y = S.lyr_slot_y[k], h = S.lyr_slot_h[k];
        int row = lv_font_get_line_height(lv_obj_get_style_text_font(S.lyr_slot[k], 0)) + space;
        if (row <= 0) continue;
        if (y < 0 && y + h > 0) {                  /* over the top edge: from its first row that starts on the screen */
            int e = y + (-y + row - 1) / row * row;
            if (e > top) top = e > y + h ? y + h : e;
        }
        if (y < room && y + h > room) {            /* over the bottom edge: to its last row that ends on it */
            int e = y + (room - y) / row * row;
            if (e < bottom) bottom = e;
        }
    }
    win_set(top, bottom);
}

static void roll_done(lv_anim_t *a) { (void)a; if (S.lyr_roll) roll_rest(); }

/* Slot k: line `line` at y, h high (its text's height in that font) */
static void slot_set(int k, int line, int y, int h, const lv_font_t *font, uint32_t colour)
{
    lv_obj_t *l = S.lyr_slot[k];
    const char *s = lyrics_line(&S.lyr, line);
    set_text(l, s[0] ? s : font == S.f->title ? "..." : " ");      /* (a pause in the words) */
    if (lv_obj_get_style_text_font(l, 0) != font) lv_obj_set_style_text_font(l, font, 0);
    set_text_color(l, C(colour));
    lv_obj_set_y(l, y + LYR_ROOM);
    show(l, 1);
    S.lyr_slot_line[k] = line;
    S.lyr_slot_y[k] = (int16_t)y;
    S.lyr_slot_h[k] = (int16_t)h;
}

static int line_h(int line, const lv_font_t *font, int width)
{
    const char *s = lyrics_line(&S.lyr, line);
    return text_h(s[0] ? s : font == S.f->title ? "..." : " ", font, width);
}

/* The lines around line c (-1: before the song's first), c a little above
 * the middle; the line being sung at the title's size and weight and in the
 * accent, the others smaller: told apart in any theme, and more of them fit.
 * slide_ms over 0: they slide to
 * their places from where line c was (it was among those shown), else they
 * are drawn there. */
static void timed_draw(int slide_ms)
{
    const theme_t *t = T();
    int room = LYR_ROOM, width = UI_W - 2 * LYR_SIDE;   /* (to the screen's foot: nothing is said there) */
    int sung = sung_line();
    int c = with_song() ? sung : S.lyr_line;
    int was = INT32_MIN, moved = c != S.lyr_drawn;   /* where line c is now, if it is among those shown */
    for (int k = 0; k < LYR_SLOTS && c >= 0; k++)
        if (S.lyr_slot_line[k] == c) was = S.lyr_slot_y[k] + S.lyr_shift;
    S.lyr_drawn = c;
    S.lyr_drawn_sung = sung;
    lv_anim_delete(S.lyr_roll, anim_roll);
    /* The large letters are the line being sung's, wherever it is; every other
     * line is the smaller size, the one in the middle too when the wheel put
     * it there. (The middle line was the large one: read with the wheel,
     * each line grew as it came to the middle and shrank as it left.) */
    const lv_font_t *big = S.f->title, *small = S.f->sub;
#define LYR_FONT(i) ((i) == sung ? big : small)
#define LYR_BETWEEN(i, j) ((i) == sung || (j) == sung ? LYR_GAP : LYR_LINE_GAP)   /* (more room round the large one) */
    int hn = c >= 0 ? line_h(c, LYR_FONT(c), width) : 0, y = room * 2 / 5 - hn / 2, n = 0;
    if (y < 0 || hn > room) y = 0;
    uint32_t accent = S.lyr_bg_on ? theme_fit(t->select_bg, S.lyr_bg_ink, 3.0) : theme_fit(t->select_bg, t->bg, 3.0);
    uint32_t plain = ink(t->text, 4.5), muted = ink(t->text_muted, 3.0);
    if (c >= 0) slot_set(n++, c, y, hn, LYR_FONT(c), c == sung ? accent : plain);
    /* above it, as far as a screen's height up (those sliding away), and
     * below it as far as a screen's height down (those sliding in) */
    int top = y;
    for (int i = c - 1; i >= 0 && n < LYR_SLOTS / 2 && top > -room; i--) {
        int h = line_h(i, LYR_FONT(i), width);
        top -= LYR_BETWEEN(i, i + 1) + h;
        slot_set(n++, i, top, h, LYR_FONT(i), i == sung ? accent : muted);
    }
    int bottom = y + hn;
    for (int i = c + 1; i < S.lyr.n && n < LYR_SLOTS && bottom < 2 * room; i++) {
        int h = line_h(i, LYR_FONT(i), width);
        if (i > 0 && (c >= 0 || i > c + 1)) bottom += LYR_BETWEEN(i - 1, i);
        slot_set(n++, i, bottom, h, LYR_FONT(i), i == sung ? accent : plain);   /* (read back or ahead: where the song is) */
        bottom += h;
    }
#undef LYR_FONT
#undef LYR_BETWEEN
    for (int k = n; k < LYR_SLOTS; k++) { S.lyr_slot_line[k] = -1; show(S.lyr_slot[k], 0); }
    if (slide_ms > 0 && moved && was != INT32_MIN && was != y && !S.asleep) {
        /* (the wheel turned faster than the lines slide: they are a screen behind at most, which is what the strip holds) */
        int from = was - y > room ? room : was - y < -room ? -room : was - y;
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, S.lyr_roll);
        lv_anim_set_exec_cb(&a, anim_roll);
        lv_anim_set_values(&a, from, 0);
        lv_anim_set_duration(&a, (uint32_t)slide_ms);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_set_completed_cb(&a, roll_done);
        anim_roll(S.lyr_roll, from);
        lv_anim_start(&a);
    } else {
        roll_rest();
    }
}

static void lyrics_draw_as(int slide_ms);
static void lyrics_draw(void) { lyrics_draw_as(0); }

static void lyrics_draw_as(int slide_ms)
{
    if (!S.lyr_ov) return;
    char buf[1536];
    int found = S.lyr_state == LF_FOUND && S.lyr.n > 0;
    int timed = found && S.lyr.synced;
    show(S.lyr_win, timed);
    show(S.lyr_page, found && !timed);
    show(S.lyr_msg, !found);
    const char *hint = "";
    if (!found) {
        const char *msg = "";
        switch (S.lyr_state) {
        case LF_LOOKING: msg = S.lyr_online ? "Looking for lyrics" : ""; break;
        case LF_NONE:
            msg = S.lyr_tried ? "No lyrics with this song, and none were found online last time" : "No lyrics with this song";
            hint = S.lyr_tried ? "<Play>: look again" : "<Play>: look online";
            break;
        case LF_NOT_FOUND: msg = "No lyrics found for this song"; break;
        case LF_INSTRUMENTAL: msg = "A song without words"; break;
        case LF_FAILED:
            msg = netclock_why("Couldn't reach the lyrics site");
            hint = "<Play>: try again";
            break;
        default: break;
        }
        set_text(S.lyr_msg, tr(msg));
    } else if (timed) {
        timed_draw(slide_ms);
    } else {
        /* a page: from the first line shown, as many whole lines as fit */
        int room = UI_H - LYR_TOP - LYR_FOOT, width = UI_W - 2 * LYR_SIDE;   /* (the body's: not laid out yet at the first draw) */
        int last = S.lyr_line;
        lines_text(buf, sizeof buf, S.lyr_line, last);
        for (int i = S.lyr_line + 1; i < S.lyr.n; i++) {
            char more[sizeof buf];
            lines_text(more, sizeof more, S.lyr_line, i);
            if (text_h(more, S.f->body, width) > room || strlen(more) + 1 >= sizeof more) break;
            memcpy(buf, more, sizeof buf);
            last = i;
        }
        S.lyr_last = last;
        S.lyr_drawn = S.lyr_line;
        set_text(S.lyr_page, buf);
        if (last < S.lyr.n - 1) hint = "Turn for more";
    }
    set_text(S.lyr_hint, tr(hint));
}

/* The screen's top line: the song's title in the header's heavier letters and,
 * after a dot, its artist in the small ones, the two centred as one; too long
 * for the line, the artist keeps two fifths of it at most and each is cut
 * with its dots */
static void lyrics_heading(const lib_track_t *t)
{
    char artist[sizeof t->artist + 8];
    int w = UI_W - 2 * LYR_SIDE;
    snprintf(artist, sizeof artist, t->artist[0] ? " \xc2\xb7 %s" : "%s", t->artist);
    int wt = text_w(t->title, S.f->head) + 1, wa = artist[0] ? text_w(artist, S.f->meta) + 1 : 0;
    if (wt + wa > w) {
        int least = wa < w * 2 / 5 ? wa : w * 2 / 5;
        if (wt > w - least) wt = w - least;
        wa = w - wt;
    }
    int x = LYR_SIDE + (w - wt - wa) / 2;
    /* (on one line of letters: the smaller ones sit lower by the difference) */
    int down = (lv_font_get_line_height(S.f->head) - S.f->head->base_line) - (lv_font_get_line_height(S.f->meta) - S.f->meta->base_line);
    /* (each a line high: a label only ends in dots when it can't grow, and a
     * long title went on to a second row, over the words. Seen on the H2.) */
    set_text(S.lyr_title, t->title);
    set_pos_width(S.lyr_title, x, 6, wt);
    lv_obj_set_height(S.lyr_title, lv_font_get_line_height(S.f->head));
    set_text(S.lyr_artist, artist);
    set_pos_width(S.lyr_artist, x + wt, 6 + down, wa > 0 ? wa : 1);
    lv_obj_set_height(S.lyr_artist, lv_font_get_line_height(S.f->meta));
    show(S.lyr_artist, wa > 0);
}

/* The song whose lyrics are shown: asked for (what it has by itself) */
static void lyrics_song(const lib_track_t *t)
{
    lf_song_t *s = &S.lyr_song;
    memset(s, 0, sizeof *s);
    s->track = t->id;
    snprintf(s->path, sizeof s->path, "%s", t->path);
    snprintf(s->artist, sizeof s->artist, "%s", t->artist);
    snprintf(s->title, sizeof s->title, "%s", t->title);
    snprintf(s->album, sizeof s->album, "%s", t->album);
    s->duration_s = (int)((t->duration_ms + 500) / 1000);
    lyrics_free(&S.lyr);
    S.lyr_track = t->id;
    S.lyr_line = S.lyr_last = 0;
    S.lyr_drawn = S.lyr_drawn_sung = -2;
    for (int k = 0; k < LYR_SLOTS; k++) S.lyr_slot_line[k] = -1;   /* (another song's lines: nothing to slide from) */
    S.lyr_manual = 0;
    S.lyr_online = S.lyr_tried = 0;
    S.lyr_state = lf_ask(s, 0) == 0 ? LF_LOOKING : LF_NONE;
    backdrop(t->album_id);
    const theme_t *th = T();
    set_text_color(S.lyr_title, C(ink(th->text, 4.5)));
    set_text_color(S.lyr_artist, C(ink(th->text_muted, 3.0)));
    set_text_color(S.lyr_hint, C(ink(th->text_muted, 3.0)));
    set_text_color(S.lyr_msg, C(ink(th->text_muted, 3.0)));
    set_text_color(S.lyr_page, C(ink(th->text, 4.5)));
    lyrics_heading(t);
    lyrics_draw();
}

void lyrics_close(void)
{
    if (!S.lyr_ov) return;
    if (S.lyr_timer) lv_timer_delete(S.lyr_timer);
    S.lyr_timer = NULL;
    lv_obj_delete(S.lyr_ov);                       /* (and a slide under way with it) */
    S.lyr_ov = S.lyr_roll = S.lyr_win = S.lyr_bg = NULL;
    free(S.lyr_bg_px);
    S.lyr_bg_px = NULL;
    S.lyr_bg_on = 0;
    lyrics_free(&S.lyr);
    S.lyr_state = LF_IDLE;
}

void lyrics_open(int64_t track, int from_np)
{
    lib_track_t t;
    lyrics_close();
    if (!S.lib || library_track(S.lib, track, &t)) return;
    const theme_t *th = T();
    S.lyr_ov = box(lv_layer_top(), 0, 0, UI_W, UI_H);
    fill(S.lyr_ov, th->bg);
    S.lyr_bg = lv_image_create(S.lyr_ov);          /* (first: behind the rest) */
    lv_obj_remove_flag(S.lyr_bg, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(S.lyr_bg, 0, 0);
    show(S.lyr_bg, 0);
    S.lyr_bg_album = 0;
    S.lyr_bg_on = 0;
    S.lyr_title = label(S.lyr_ov);                 /* (placed, with the artist after it, by lyrics_heading) */
    style_label(S.lyr_title, S.f->head, th->text);
    S.lyr_artist = label(S.lyr_ov);
    style_label(S.lyr_artist, S.f->meta, th->text_muted);
    S.lyr_body = box(S.lyr_ov, LYR_SIDE, LYR_TOP, UI_W - 2 * LYR_SIDE, UI_H - LYR_TOP - LYR_END);
    S.lyr_win = box(S.lyr_body, 0, 0, UI_W - 2 * LYR_SIDE, LYR_ROOM);
    S.lyr_roll = box(S.lyr_win, 0, -LYR_ROOM, UI_W - 2 * LYR_SIDE, 3 * LYR_ROOM);   /* (anim_roll) */
    for (int k = 0; k < LYR_SLOTS; k++) {
        S.lyr_slot[k] = words(S.lyr_roll, S.f->sub, th->text);
        S.lyr_slot_line[k] = -1;
    }
    S.lyr_shift = 0;
    S.lyr_page = words(S.lyr_body, S.f->body, th->text);
    S.lyr_msg = words(S.lyr_body, S.f->body, th->text_muted);
    lv_obj_align(S.lyr_msg, LV_ALIGN_CENTER, 0, -(LYR_FOOT - LYR_END) / 2);
    S.lyr_hint = label(S.lyr_ov);
    style_label(S.lyr_hint, S.f->meta, th->text_muted);
    lv_obj_set_style_text_align(S.lyr_hint, LV_TEXT_ALIGN_CENTER, 0);
    set_pos_width(S.lyr_hint, LYR_SIDE, UI_H - LYR_FOOT + 3, UI_W - 2 * LYR_SIDE);
    lv_label_set_text(S.lyr_hint, "");
    lv_obj_move_background(S.lyr_ov);            /* (toasts and the volume bar stay over it) */
    S.lyr_from_np = from_np;
    S.lyr_depth = S.depth;
    /* the song playing is followed (the lyrics change with the song); another
     * one's are read with the wheel */
    S.lyr_follow = S.now.active && !S.now.radio && !S.now.podcast && S.now.track.id == track;
    S.lyr_auto = 1;
    S.lyr_timer = lv_timer_create(lyrics_tick, LYR_TICK_MS, NULL);
    if (S.asleep && S.lyr_timer) lv_timer_pause(S.lyr_timer);
    lyrics_song(&t);
}

/* Five times a second while it shows (not with the screen off): the lookup's
 * answer, the song's place, the song itself */
static void lyrics_tick(lv_timer_t *timer)
{
    (void)timer;
    if (!S.lyr_ov || S.asleep) return;
    if (S.depth != S.lyr_depth) { lyrics_close(); return; }   /* (a question opened under it: that is what to see) */
    if (S.lyr_follow && S.player) {
        player_now_t n;
        player_now(S.player, &n);
        if (n.radio || n.podcast) { lyrics_close(); return; }
        if (n.active && n.track.id != S.lyr_track) { lyrics_song(&n.track); return; }
    }
    if (S.lyr_state == LF_LOOKING) {
        int64_t track = 0;
        int online = 0, tried = 0, st = lf_state(&track, &online, &tried);
        if (track != S.lyr_track || st == LF_IDLE) st = LF_NONE;   /* (asked by another since: none of ours) */
        if (st != LF_LOOKING) {
            S.lyr_online = online;
            S.lyr_tried = tried;
            if (st == LF_FOUND && lf_take(&S.lyr, NULL)) st = LF_NONE;
            S.lyr_state = st;
            S.lyr_line = 0;
            lyrics_draw();
        }
        return;
    }
    if (S.lyr_state != LF_FOUND || !S.lyr.synced) return;
    if (S.lyr_manual && lv_tick_elaps(S.lyr_manual_at) >= LYR_REST_MS && S.lyr_follow && S.lyr_auto) S.lyr_manual = 0;
    int sung = sung_line();
    if (with_song() ? sung != S.lyr_drawn : sung != S.lyr_drawn_sung) lyrics_draw_as(LYR_SLIDE_MS);
}

int lyrics_key(ui_key_t k)
{
    int found = S.lyr_state == LF_FOUND && S.lyr.n > 0;
    switch (k) {
    case UI_KEY_UP: case UI_KEY_DOWN: {
        if (!found) return 1;
        int d = k == UI_KEY_DOWN ? 1 : -1;
        if (S.lyr.synced) {
            if (with_song()) S.lyr_line = S.lyr_drawn;   /* from where the song is (-1: before its first line) */
            S.lyr_line += d;
            if (S.lyr_line < 0) S.lyr_line = 0;
            if (S.lyr_line > S.lyr.n - 1) S.lyr_line = S.lyr.n - 1;
            S.lyr_manual = 1;
            S.lyr_manual_at = lv_tick_get();
        } else {
            if (d > 0 && S.lyr_last < S.lyr.n - 1) S.lyr_line++;
            if (d < 0 && S.lyr_line > 0) S.lyr_line--;
        }
        lyrics_draw_as(LYR_TURN_MS);
        return 1;
    }
    case UI_KEY_OK:
        if (found) {
            if (S.lyr_follow && S.player) player_toggle_pause(S.player);   /* as on Now Playing */
            return 1;
        }
        if (S.lyr_state == LF_NONE || S.lyr_state == LF_FAILED) {
            char why[96] = "";
            if (S.hooks.radio_ready && S.hooks.radio_ready(why, sizeof why)) {
                /* no network: this goes, and Wi-Fi is asked for or why not said, where the menu was opened */
                lyrics_close();
                ui_net_or_ask("Finding lyrics needs Wi-Fi");
                return 1;
            }
            S.lyr_online = 1;
            S.lyr_state = lf_ask(&S.lyr_song, 1) == 0 ? LF_LOOKING : LF_FAILED;
            lyrics_draw();
        }
        return 1;
    case UI_KEY_BACK: case UI_KEY_BACK_LONG:
        lyrics_close();
        return 1;
    case UI_KEY_MENU:
        if (!found) return 1;
        if (!S.lyr.synced) { ui_toast("These lyrics have no times to follow", 2000); return 1; }
        if (!S.lyr_follow) { ui_toast("Lyrics follow a song while it plays", 2000); return 1; }
        if (S.lyr_auto) S.lyr_line = S.lyr_drawn < 0 ? 0 : S.lyr_drawn;   /* (they stay where they are) */
        S.lyr_auto = !S.lyr_auto;
        S.lyr_manual = 0;
        ui_toast(S.lyr_auto ? "Lyrics follow the song" : "Lyrics move with the wheel", 1500);
        lyrics_draw_as(LYR_SLIDE_MS);
        return 1;
    case UI_KEY_MENU_LONG:
        return 1;                                   /* (quick settings would open under it, unseen) */
    default:
        return 0;                                   /* the music's and the screen's, as anywhere */
    }
}

void lyrics_awake(int on)
{
    if (!S.lyr_timer) return;
    if (on) { lv_timer_resume(S.lyr_timer); lyrics_draw(); }
    else lv_timer_pause(S.lyr_timer);
}

int ui_test_lyrics(char *now, size_t cap)
{
    if (cap) now[0] = '\0';
    if (!S.lyr_ov) return -2;
    if (S.lyr_state == LF_FOUND && cap && !S.lyr.synced) snprintf(now, cap, "%s", lv_label_get_text(S.lyr_page));
    else if (S.lyr_state == LF_FOUND && cap && S.lyr_drawn >= 0 && S.lyr_drawn < S.lyr.n)
        snprintf(now, cap, "%s", lyrics_line(&S.lyr, S.lyr_drawn));
    return S.lyr_state;
}

/* The heading's height in px (tests: one line, however long the title) */
int ui_test_lyrics_heading(void)
{
    if (!S.lyr_ov) return -1;
    lv_obj_update_layout(S.lyr_ov);
    int a = lv_obj_get_height(S.lyr_title), b = lv_obj_has_flag(S.lyr_artist, LV_OBJ_FLAG_HIDDEN) ? 0 : lv_obj_get_height(S.lyr_artist);
    return a > b ? a : b;
}

/* Timed lyrics as they are drawn this instant, a slide under way included:
 * the tallest stretch of the screen's height with no line in it, in px. */
int ui_test_lyrics_gap(void)
{
    if (!S.lyr_ov || S.lyr_state != LF_FOUND || !S.lyr.synced) return -1;
    int gap = 0, at = 0;
    for (;;) {                                     /* the line that starts first at or after `at`, or covers it */
        int next = LYR_ROOM, end = -1;
        for (int k = 0; k < LYR_SLOTS; k++) {
            if (S.lyr_slot_line[k] < 0) continue;
            int a = S.lyr_slot_y[k] + S.lyr_shift, b = a + S.lyr_slot_h[k];
            if (b <= at || a >= LYR_ROOM) continue;
            if (a < next) { next = a; end = b; }
        }
        if (next - at > gap) gap = next - at;
        if (end < 0) break;
        at = end;
    }
    return gap;
}

#ifdef FATHOM_UI_SHOW
#include <time.h>
/* The simulator's pictures: the lookup's answer (it reads saved ones) before the picture */
void lyrics_settle(void)
{
    for (int i = 0; i < 400 && S.lyr_ov && S.lyr_state == LF_LOOKING; i++) {
        lyrics_tick(NULL);
        struct timespec t = {0, 5 * 1000 * 1000L};
        nanosleep(&t, NULL);
    }
    lyrics_tick(NULL);
    lyrics_still();
}

/* ...and the lines where they belong, a slide under way finished */
void lyrics_still(void)
{
    if (!S.lyr_roll) return;
    lv_anim_delete(S.lyr_roll, anim_roll);
    if (S.lyr_state == LF_FOUND && S.lyr.synced) roll_rest();
}
#endif
