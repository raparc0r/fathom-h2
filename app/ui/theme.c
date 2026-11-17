/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "theme.h"

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const VETTED_FONTS[] = { "atkinson", "arkpixel", "vt323" };

const char *const THEME_ICON_KEYS[THEME_ICON_N] = {"play", "pause", "shuffle", "repeat", "repeat_one", "charging",
                                                   "stop", "bluetooth", "wifi"};

uint32_t theme_rgb565(uint32_t rgb)
{
    uint32_t r = (rgb >> 16) & 0xff, g = (rgb >> 8) & 0xff, b = rgb & 0xff;
    r = ((r >> 3) << 3) | (r >> 5);
    g = ((g >> 2) << 2) | (g >> 6);
    b = ((b >> 3) << 3) | (b >> 5);
    return (r << 16) | (g << 8) | b;
}

static double channel(uint32_t c8)
{
    double c = c8 / 255.0;
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

static double luminance(uint32_t rgb)
{
    rgb = theme_rgb565(rgb);
    return 0.2126 * channel((rgb >> 16) & 0xff) + 0.7152 * channel((rgb >> 8) & 0xff) +
           0.0722 * channel(rgb & 0xff);
}

double theme_contrast(uint32_t a, uint32_t b)
{
    double la = luminance(a), lb = luminance(b);
    if (la < lb) { double t = la; la = lb; lb = t; }
    return (la + 0.05) / (lb + 0.05);
}

static void errf(char *err, size_t cap, const char *fmt, ...)
{
    if (!err || cap == 0) return;
    size_t len = strlen(err);
    if (len + 1 >= cap) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err + len, cap - len, fmt, ap);
    va_end(ap);
}

static char *trim(char *s)
{
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = '\0';
    return s;
}

static int parse_color(const char *v, uint32_t *out)
{
    if (v[0] != '#' || strlen(v) != 7) return -1;
    for (int i = 1; i < 7; i++) if (!isxdigit((unsigned char)v[i])) return -1;
    *out = (uint32_t)strtoul(v + 1, NULL, 16);
    return 0;
}

static int pick(const char *v, const char *const *opts, int n)
{
    for (int i = 0; i < n; i++) if (!strcmp(v, opts[i])) return i;
    return -1;
}

int theme_parse(const char *text, theme_t *t, char *err, size_t errcap)
{
    if (err && errcap) err[0] = '\0';
    memset(t, 0, sizeof *t);
    t->dark = 1;
    t->radius = 10;
    strcpy(t->font, "atkinson");
    uint32_t have = 0;   /* bitmask of required colours seen */
    enum { R_BG = 1, R_SURF = 2, R_TEXT = 4, R_MUTED = 8, R_SELBG = 16, R_SELFG = 32, R_SIG = 64 };
    int set_info = 0, set_mode = 0, set_batt = 0, set_warn = 0, set_danger = 0;
    int set_artist = 0, set_album = 0, set_chip_bg = 0, set_chip_fg = 0;

    char section[32] = "";
    char *copy = strdup(text ? text : "");
    if (!copy) { errf(err, errcap, "out of memory\n"); return -1; }
    int lineno = 0, bad = 0;
    for (char *save = NULL, *line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        lineno++;
        char *semi = strchr(line, ';');
        if (semi) *semi = '\0';
        char *l = trim(line);
        if (!*l) continue;
        if (*l == '[') {
            char *end = strchr(l, ']');
            if (!end) { errf(err, errcap, "line %d: bad section\n", lineno); bad++; continue; }
            *end = '\0';
            snprintf(section, sizeof section, "%s", l + 1);
            continue;
        }
        char *eq = strchr(l, '=');
        if (!eq) { errf(err, errcap, "line %d: expected key = value\n", lineno); bad++; continue; }
        *eq = '\0';
        char *k = trim(l), *v = trim(eq + 1);

        if (!strcmp(section, "theme")) {
            if (!strcmp(k, "name")) snprintf(t->name, sizeof t->name, "%s", v);
            else if (!strcmp(k, "author")) snprintf(t->author, sizeof t->author, "%s", v);
            else if (!strcmp(k, "licence") || !strcmp(k, "license")) snprintf(t->licence, sizeof t->licence, "%s", v);
            else if (!strcmp(k, "format")) t->format = atoi(v);
            else if (!strcmp(k, "base")) t->dark = strcmp(v, "light") != 0;
            else t->unknown_keys++;
            continue;
        }
        if (!strcmp(section, "color")) {
            struct { const char *key; uint32_t *dst; uint32_t bit; int *flag; } map[] = {
                {"bg", &t->bg, R_BG, NULL}, {"surface", &t->surface, R_SURF, NULL},
                {"text", &t->text, R_TEXT, NULL}, {"text_muted", &t->text_muted, R_MUTED, NULL},
                {"select_bg", &t->select_bg, R_SELBG, NULL}, {"select_fg", &t->select_fg, R_SELFG, NULL},
                {"signal", &t->signal, R_SIG, NULL}, {"info", &t->info, 0, &set_info},
                {"mode", &t->mode, 0, &set_mode}, {"battery", &t->battery, 0, &set_batt},
                {"warning", &t->warning, 0, &set_warn}, {"danger", &t->danger, 0, &set_danger},
                {"artist", &t->artist, 0, &set_artist}, {"album", &t->album, 0, &set_album},
                {"chip_bg", &t->chip_bg, 0, &set_chip_bg}, {"chip_fg", &t->chip_fg, 0, &set_chip_fg},
            };
            size_t i;
            for (i = 0; i < sizeof map / sizeof map[0]; i++) {
                if (strcmp(k, map[i].key)) continue;
                if (parse_color(v, map[i].dst)) {
                    errf(err, errcap, "line %d: %s: colour must look like #RRGGBB\n", lineno, k);
                    bad++;
                } else {
                    have |= map[i].bit;
                    if (map[i].flag) *map[i].flag = 1;
                }
                break;
            }
            if (i == sizeof map / sizeof map[0]) t->unknown_keys++;   /* a newer colour role: ignored */
            continue;
        }
        if (!strcmp(section, "font")) {
            static const char *const roles[THEME_PX_N] = {"meta", "body", "sub", "title", "head"};
            int role = -1;
            for (int r = 0; r < THEME_PX_N; r++)
                if (!strcmp(k, roles[r])) role = r;
            if (!strcmp(k, "family")) snprintf(t->font, sizeof t->font, "%s", v);
            else if (!strcmp(k, "file")) snprintf(t->font_file, sizeof t->font_file, "%s", v);
            else if (!strcmp(k, "file_title")) snprintf(t->font_file_title, sizeof t->font_file_title, "%s", v);
            else if (role >= 0) {
                int px = atoi(v);
                if (px < 6 || px > 64) { errf(err, errcap, "line %d: [font] %s = %s: a size from 6 to 64 px\n", lineno, k, v); bad++; }
                else t->font_px[role] = px;
            } else {
                t->unknown_keys++;
            }
            if (strstr(t->font_file, "..") || strstr(t->font_file_title, "..") || t->font_file[0] == '/' || t->font_file_title[0] == '/') {
                errf(err, errcap, "line %d: font files must be inside the theme's folder\n", lineno);
                bad++;
            }
            continue;
        }
        if (!strcmp(section, "icons")) {
            int which = -1;
            for (int ic = 0; ic < THEME_ICON_N; ic++)
                if (!strcmp(k, THEME_ICON_KEYS[ic])) which = ic;
            if (which < 0) { t->unknown_keys++; continue; }
            if (strstr(v, "..") || v[0] == '/') {
                errf(err, errcap, "line %d: icon files must be inside the theme's folder\n", lineno);
                bad++;
            } else {
                snprintf(t->icon[which], sizeof t->icon[which], "%s", v);
            }
            continue;
        }
        int ok = 1;
        if (!strcmp(section, "shape")) {
            static const char *const sel[] = {"pill", "bar"}, *const cor[] = {"round", "stepped", "square"},
                                     *const sh[] = {"none", "hard"}, *const pr[] = {"bar", "segments"},
                                     *const cu[] = {"none", "arrow"}, *const pl[] = {"dot", "equalizer"},
                                     *const mo[] = {"smooth", "stepped"};
            int r = -1;
            if (!strcmp(k, "selection")) { r = pick(v, sel, 2); t->selection_bar = r; }
            else if (!strcmp(k, "corner")) { r = pick(v, cor, 3); t->corner = (theme_corner_t)r; }
            else if (!strcmp(k, "radius")) { r = atoi(v); t->radius = r; if (r < 0 || r > 20) r = -1; }
            else if (!strcmp(k, "shadow")) { r = pick(v, sh, 2); t->shadow_hard = r; }
            else if (!strcmp(k, "progress")) { r = pick(v, pr, 2); t->progress_segments = r; }
            else if (!strcmp(k, "cursor")) { r = pick(v, cu, 2); t->cursor_arrow = r; }
            else if (!strcmp(k, "playing")) { r = pick(v, pl, 2); t->playing_equalizer = r; }
            else if (!strcmp(k, "motion")) { r = pick(v, mo, 2); t->motion_stepped = r; }
            else { t->unknown_keys++; continue; }   /* a newer key: ignored */
            ok = r >= 0;
        } else if (!strcmp(section, "art") && !strcmp(k, "style")) {
            static const char *const st[] = {"normal", "pixelate"};
            int r = pick(v, st, 2); t->art_pixelate = r; ok = r >= 0;
        } else if (!strcmp(section, "decor")) {
            static const char *const hp[] = {"none", "dots"}, *const ic[] = {"line", "pixel"};
            int r = -1;
            if (!strcmp(k, "header_pattern")) { r = pick(v, hp, 2); t->header_dots = r; }
            else if (!strcmp(k, "icons")) { r = pick(v, ic, 2); t->icons_pixel = r; }
            else { t->unknown_keys++; continue; }
            ok = r >= 0;
        } else {
            t->unknown_keys++;      /* a newer format's section or key: ignored */
            continue;
        }
        if (!ok) { errf(err, errcap, "line %d: [%s] %s = %s is not valid\n", lineno, section, k, v); bad++; }
    }
    free(copy);

    static const char *const req[] = {"bg", "surface", "text", "text_muted", "select_bg", "select_fg", "signal"};
    for (int i = 0; i < 7; i++)
        if (!(have & (1u << i))) { errf(err, errcap, "missing colour: %s\n", req[i]); bad++; }
    if (!t->name[0]) { errf(err, errcap, "missing [theme] name\n"); bad++; }
    if (!set_info) t->info = t->signal;
    if (!set_mode) t->mode = t->signal;
    if (!set_batt) t->battery = t->text_muted;
    if (!set_warn) t->warning = t->dark ? 0xFFC845 : 0x8A5A00;
    if (!set_danger) t->danger = t->dark ? 0xFF6B6B : 0xB3261E;
    if (!set_artist) t->artist = t->text_muted;
    if (!set_album) t->album = t->text_muted;
    /* the format chip: a pale surface, or (stepped corners) a block of info colour */
    if (!set_chip_bg) t->chip_bg = t->corner == THEME_CORNER_STEPPED ? t->info : t->surface;
    if (!set_chip_fg) t->chip_fg = t->corner == THEME_CORNER_STEPPED ? t->select_fg : t->text;
    return bad ? -1 : 0;
}

int theme_load_file(const char *path, theme_t *t, char *err, size_t errcap)
{
    FILE *f = fopen(path, "rb");
    if (!f) { if (err && errcap) snprintf(err, errcap, "cannot open %s\n", path); return -1; }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len < 0 || len > 64 * 1024) { fclose(f); if (err && errcap) snprintf(err, errcap, "%s: too large\n", path); return -1; }
    char *buf = malloc((size_t)len + 1);
    if (!buf) { fclose(f); return -1; }
    size_t got = fread(buf, 1, (size_t)len, f);
    fclose(f);
    buf[got] = '\0';
    int r = theme_parse(buf, t, err, errcap);
    free(buf);
    return r;
}

int theme_validate(const theme_t *t, char *err, size_t errcap)
{
    if (err && errcap) err[0] = '\0';
    int fails = 0;
    struct { const char *fg_name; uint32_t fg; const char *bg_name; uint32_t bg; double min; } pairs[] = {
        {"text", t->text, "bg", t->bg, 7.0},
        {"text_muted", t->text_muted, "bg", t->bg, 4.5},
        {"select_fg", t->select_fg, "select_bg", t->select_bg, 7.0},
        {"text", t->text, "surface", t->surface, 7.0},
        {"signal", t->signal, "bg", t->bg, 4.5},
        {"info", t->info, "bg", t->bg, 4.5},
        {"mode", t->mode, "bg", t->bg, 4.5},
        {"battery", t->battery, "bg", t->bg, 4.5},
        {"warning", t->warning, "bg", t->bg, 4.5},
        {"danger", t->danger, "bg", t->bg, 4.5},
        {"artist", t->artist, "bg", t->bg, 4.5},
        {"album", t->album, "bg", t->bg, 4.5},
        {"chip_fg", t->chip_fg, "chip_bg", t->chip_bg, 4.5},
    };
    for (size_t i = 0; i < sizeof pairs / sizeof pairs[0]; i++) {
        double c = theme_contrast(pairs[i].fg, pairs[i].bg);
        if (c < pairs[i].min) {
            errf(err, errcap, "%s on %s: %.1f < %.1f\n", pairs[i].fg_name, pairs[i].bg_name, c, pairs[i].min);
            fails++;
        }
    }
    if (!t->font_file[0]) {
        int vetted = 0;
        for (size_t i = 0; i < sizeof VETTED_FONTS / sizeof VETTED_FONTS[0]; i++)
            if (!strcmp(t->font, VETTED_FONTS[i])) vetted = 1;
        if (!vetted) { errf(err, errcap, "font %s is not a built-in family\n", t->font); fails++; }
    }
    static const char *const roles[THEME_PX_N] = {"meta", "body", "sub", "title", "head"};
    for (int r = 0; r < THEME_PX_N; r++)
        if (t->font_px[r] && t->font_px[r] < THEME_MIN_PX) {
            errf(err, errcap, "%s text is %d px, under %d px\n", roles[r], t->font_px[r], THEME_MIN_PX);
            fails++;
        }
    return fails;
}
