/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
#include "ui/assets.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "stb/stb_image.h"
#include "ui/fonts.h"

static const char *const BUILTIN[] = {"studio-mint.theme", "studio-ember.theme", "studio-pastel.theme",
                                      "pixel-pastel.theme"};
#define N_BUILTIN (int)(sizeof BUILTIN / sizeof BUILTIN[0])

static char g_assets[512];

static void *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rbe");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *buf = n > 0 && n < 16 * 1024 * 1024 ? malloc((size_t)n) : NULL;
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); buf = NULL; }
    fclose(f);
    *len = buf ? (size_t)n : 0;
    return buf;
}

/* ---------- font files: read once, shared by every size and theme ---------- */

#define MAX_FILES 12
static struct { char path[640]; void *data; size_t len; } g_files[MAX_FILES];

static int font_file(const char *path, void **data, size_t *len)
{
    for (int i = 0; i < MAX_FILES && g_files[i].path[0]; i++)
        if (!strcmp(g_files[i].path, path)) { *data = g_files[i].data; *len = g_files[i].len; return 0; }
    void *d = slurp(path, len);
    if (!d) return -1;
    for (int i = 0; i < MAX_FILES; i++)
        if (!g_files[i].path[0]) {
            snprintf(g_files[i].path, sizeof g_files[i].path, "%s", path);
            g_files[i].data = d;
            g_files[i].len = *len;
            break;
        }
    *data = d;   /* kept for the program's lifetime: tiny_ttf reads from it */
    return 0;
}

static const lv_font_t *ttf(const char *path, int px, const lv_font_t *fallback)
{
    void *data;
    size_t len;
    if (font_file(path, &data, &len)) { fprintf(stderr, "cannot read %s\n", path); return NULL; }
    lv_font_t *f = ui_font_ttf(data, len, px, fallback);
    if (!f) fprintf(stderr, "cannot load font %s\n", path);
    return f;
}

/* ---------- font sets ---------- */

#define MAX_SETS 12
static struct { char key[900]; ui_fonts_t fonts; } g_sets[MAX_SETS];

/* Built-in families: files and default sizes per role (meta, body, sub, title, head),
 * and how much bigger than meta a row's value is drawn (ui.h: value). The pixel
 * families stay on their grid; a theme's own font is its maker's to size. */
static const struct {
    const char *family, *file, *file_title;
    int px[THEME_PX_N];
    int value_add;
} FAMILIES[] = {
    {"atkinson", "AtkinsonHyperlegibleNext-Regular.ttf", "AtkinsonHyperlegibleNext-Medium.ttf", {15, 20, 18, 22, 16}, 1},
    /* Ark Pixel (10 px proportional) is drawn on a 100-unit grid: 20 px = 2 screen
     * pixels per font pixel, 30 px = 3. Only these sizes stay crisp. */
    {"arkpixel", "ArkPixel-10px-Proportional.ttf", "ArkPixel-10px-Proportional.ttf", {20, 20, 20, 30, 20}, 0},
    /* VT323 is drawn small for its pixel size: these match Atkinson's cap height */
    {"vt323", "VT323-Regular.ttf", "VT323-Regular.ttf", {20, 26, 24, 30, 22}, 0},
};

/* Behind every font: Noto Sans (a subset: Latin Extended-A and B, Greek,
 * Cyrillic) for the letters the others lack. */
static const char *const FALLBACK_FILE = "NotoSans-Subset.ttf";
/* Tabler Icons (MIT), cut down to the icons the UI uses (tools/fonts/subset_ttf.py --keep) */
static const char *const ICON_FILE = "TablerIcons-Home.ttf";

static const ui_fonts_t *load_set(const char *family, const char *file, const char *file_title,
                                  const int *px, int value_add, const ui_fonts_t *fallback)
{
    char key[900];
    snprintf(key, sizeof key, "%s|%s|%d,%d,%d,%d,%d+%d", file, file_title, px[0], px[1], px[2], px[3], px[4], value_add);
    int free_slot = -1;
    for (int i = 0; i < MAX_SETS; i++) {
        if (!strcmp(g_sets[i].key, key)) return &g_sets[i].fonts;
        if (!g_sets[i].key[0] && free_slot < 0) free_slot = i;
    }
    if (free_slot < 0) return NULL;
    ui_fonts_t f = {
        .family = family,
        .meta  = ttf(file, px[THEME_PX_META], fallback ? fallback->meta : NULL),
        .body  = ttf(file, px[THEME_PX_BODY], fallback ? fallback->body : NULL),
        .sub   = ttf(file, px[THEME_PX_SUB], fallback ? fallback->sub : NULL),
        .title = ttf(file_title, px[THEME_PX_TITLE], fallback ? fallback->title : NULL),
        .head  = ttf(file_title, px[THEME_PX_HEAD], fallback ? fallback->head : NULL),
    };
    f.value = value_add ? ttf(file, px[THEME_PX_META] + value_add, fallback ? fallback->value : NULL) : f.meta;
    if (!f.meta || !f.body || !f.sub || !f.title || !f.head || !f.value) return NULL;
    /* Home's icons, the same for every family (Pixel Pastel draws its own) */
    char icons[600];
    snprintf(icons, sizeof icons, "%s/fonts/%s", g_assets, ICON_FILE);
    f.icon = ttf(icons, px[THEME_PX_BODY], NULL);
    f.icon_big = ttf(icons, 72, NULL);      /* drawn at its size, not a 20 px glyph stretched */
    snprintf(g_sets[free_slot].key, sizeof g_sets[free_slot].key, "%s", key);
    g_sets[free_slot].fonts = f;
    return &g_sets[free_slot].fonts;
}

/* The last-resort fallback at the sizes of a set (NULL if the file is missing). */
static const ui_fonts_t *fallback_set(const int *px, int value_add)
{
    char f[640];
    snprintf(f, sizeof f, "%s/fonts/%s", g_assets, FALLBACK_FILE);
    FILE *probe = fopen(f, "rbe");
    if (!probe) return NULL;
    fclose(probe);
    return load_set("noto", f, f, px, value_add, NULL);
}

/* value_add: the family's own (-1), or the one of the set this is the fallback of. */
static const ui_fonts_t *builtin_family(const char *family, const int *override_px, int value_add)
{
    for (size_t i = 0; i < sizeof FAMILIES / sizeof FAMILIES[0]; i++) {
        if (strcmp(FAMILIES[i].family, family)) continue;
        char f[640], ft[640];
        snprintf(f, sizeof f, "%s/fonts/%s", g_assets, FAMILIES[i].file);
        snprintf(ft, sizeof ft, "%s/fonts/%s", g_assets, FAMILIES[i].file_title);
        int px[THEME_PX_N];
        for (int r = 0; r < THEME_PX_N; r++) px[r] = override_px && override_px[r] ? override_px[r] : FAMILIES[i].px[r];
        /* Atkinson falls back to Noto; the other families to Atkinson first (punctuation
         * like "‹" that pixel fonts lack), then Noto */
        int add = value_add < 0 ? FAMILIES[i].value_add : value_add;
        const ui_fonts_t *fb = i == 0 ? fallback_set(px, add) : builtin_family("atkinson", px, add);
        return load_set(FAMILIES[i].family, f, ft, px, add, fb);
    }
    return NULL;
}

const ui_fonts_t *ui_theme_fonts(const theme_t *t)
{
    if (!t) return NULL;
    if (!t->font_file[0]) return builtin_family(t->font, t->font_px, -1);
    /* the theme's own font, with Atkinson (and Noto behind that) for anything it lacks */
    int px[THEME_PX_N];
    for (int r = 0; r < THEME_PX_N; r++) px[r] = t->font_px[r] ? t->font_px[r] : FAMILIES[0].px[r];
    const ui_fonts_t *fallback = builtin_family("atkinson", px, 0);
    char f[640], ft[640];
    snprintf(f, sizeof f, "%s/%s", t->dir, t->font_file);
    snprintf(ft, sizeof ft, "%s/%s", t->dir, t->font_file_title[0] ? t->font_file_title : t->font_file);
    return load_set("theme", f, ft, px, 0, fallback);
}

/* ---------- icons ---------- */

#define MAX_ICONS 24
static struct { char path[640]; lv_image_dsc_t dsc; int ok; } g_icons[MAX_ICONS];

/* Decode a PNG to LVGL's ARGB8888 (B, G, R, A in memory). Size in *w, *h. */
static uint8_t *load_png(const char *path, int *w, int *h)
{
    size_t len;
    unsigned char *file = slurp(path, &len);
    if (!file) return NULL;
    int ch;
    unsigned char *rgba = stbi_load_from_memory(file, (int)len, w, h, &ch, 4);
    free(file);
    if (!rgba) return NULL;
    for (int i = 0; i < *w * *h; i++) {          /* RGBA -> BGRA */
        unsigned char r = rgba[4 * i];
        rgba[4 * i] = rgba[4 * i + 2];
        rgba[4 * i + 2] = r;
    }
    return rgba;
}

static lv_image_dsc_t g_logo;

const lv_image_dsc_t *ui_logo(void)
{
    if (g_logo.data) return &g_logo;
    char path[600];
    snprintf(path, sizeof path, "%s/logo.png", g_assets);
    int w, h;
    uint8_t *px = load_png(path, &w, &h);
    if (!px) return NULL;
    g_logo.header.magic = LV_IMAGE_HEADER_MAGIC;
    g_logo.header.cf = LV_COLOR_FORMAT_ARGB8888;
    g_logo.header.w = (uint32_t)w;
    g_logo.header.h = (uint32_t)h;
    g_logo.header.stride = (uint32_t)w * 4;
    g_logo.data_size = (uint32_t)(w * h * 4);
    g_logo.data = px;
    return &g_logo;
}

void ui_logo_release(void)
{
    if (!g_logo.data) return;
    lv_image_cache_drop(&g_logo);
    free((void *)g_logo.data);
    memset(&g_logo, 0, sizeof g_logo);
}

const lv_image_dsc_t *ui_theme_icon(const theme_t *t, int which)
{
    if (!t || which < 0 || which >= THEME_ICON_N || !t->icon[which][0]) return NULL;
    char path[640];
    snprintf(path, sizeof path, "%s/%s", t->dir, t->icon[which]);
    int slot = -1;
    for (int i = 0; i < MAX_ICONS; i++) {
        if (!strcmp(g_icons[i].path, path)) return g_icons[i].ok ? &g_icons[i].dsc : NULL;
        if (!g_icons[i].path[0] && slot < 0) slot = i;
    }
    if (slot < 0) return NULL;
    snprintf(g_icons[slot].path, sizeof g_icons[slot].path, "%s", path);
    int w, h;
    uint8_t *px = load_png(path, &w, &h);
    if (!px || w != THEME_ICON_PX || h != THEME_ICON_PX) { free(px); return NULL; }   /* remembered as unusable */
    lv_image_dsc_t *d = &g_icons[slot].dsc;
    memset(d, 0, sizeof *d);
    d->header.magic = LV_IMAGE_HEADER_MAGIC;
    d->header.cf = LV_COLOR_FORMAT_ARGB8888;
    d->header.w = (uint32_t)w;
    d->header.h = (uint32_t)h;
    d->header.stride = (uint32_t)w * 4;
    d->data = px;                                /* kept: LVGL draws from it */
    d->data_size = (uint32_t)(w * h * 4);
    g_icons[slot].ok = 1;
    return d;
}

/* ---------- the catalogue ---------- */

static void note(theme_t *t, theme_status_t st, const char *msg)
{
    if (st < t->status) return;           /* an error outranks a warning */
    if (st > t->status || !t->note[0]) {
        snprintf(t->note, sizeof t->note, "%.159s", msg);
        char *nl = strchr(t->note, '\n');
        if (nl) *nl = '\0';
    }
    t->status = st;
}

/* Accented letters U+00C0..U+017F (without x and division signs) the font lacks. */
static int missing_accents(const char *path)
{
    size_t len;
    void *data = slurp(path, &len);
    if (!data) return -1;
    font_cov_t *c = font_cov_new(data, len);
    int missing = 0;
    for (uint32_t cp = 0xC0; c && cp <= 0x17F; cp++)
        if (cp != 0xD7 && cp != 0xF7 && !font_cov_has(c, cp)) missing++;
    font_cov_free(c);
    free(data);
    return c ? missing : -1;
}

/* How many accented letters the default font (Atkinson) lacks: the bar a theme's font is held to. */
static int default_missing(void)
{
    static int m = -2;
    if (m == -2) {
        char path[640];
        snprintf(path, sizeof path, "%s/fonts/%s", g_assets, "AtkinsonHyperlegibleNext-Regular.ttf");
        m = missing_accents(path);
        if (m < 0) m = 0;
    }
    return m;
}

static void check_theme(theme_t *t)
{
    char msg[1024];
    if (theme_validate(t, msg, sizeof msg)) note(t, THEME_WARN, msg);
    if (t->font_file[0]) {
        const char *files[2] = {t->font_file, t->font_file_title};
        for (int i = 0; i < 2; i++) {
            if (!files[i][0]) continue;
            char path[640];
            snprintf(path, sizeof path, "%s/%s", t->dir, files[i]);
            int m = missing_accents(path);
            if (m < 0) { snprintf(msg, sizeof msg, "font %s can't be read", files[i]); note(t, THEME_ERROR, msg); }
            else if (m > default_missing()) {
                snprintf(msg, sizeof msg, "font lacks %d accented letters (the default font, %d)", m, default_missing());
                note(t, THEME_WARN, msg);
            }
        }
    }
    for (int i = 0; i < THEME_ICON_N; i++) {
        if (!t->icon[i][0]) continue;
        char path[640];
        snprintf(path, sizeof path, "%s/%s", t->dir, t->icon[i]);
        int w, h;
        uint8_t *px = load_png(path, &w, &h);
        int readable = px != NULL;
        free(px);
        if (!readable) { snprintf(msg, sizeof msg, "icon %s can't be read", t->icon[i]); note(t, THEME_ERROR, msg); }
        else if (w != THEME_ICON_PX || h != THEME_ICON_PX) {
            snprintf(msg, sizeof msg, "icon %s is %dx%d, not %dx%d", t->icon[i], w, h, THEME_ICON_PX, THEME_ICON_PX);
            note(t, THEME_WARN, msg);
        }
    }
    if (t->unknown_keys && t->status == THEME_OK)
        snprintf(t->note, sizeof t->note, "%d setting(s) ignored", t->unknown_keys);
}

static int load_one(const char *path, const char *dir, const char *file, const char *fallback_name, theme_t *t)
{
    char msg[1024];
    if (theme_load_file(path, t, msg, sizeof msg)) {
        /* keep a name so the list can say what's wrong with it */
        if (!t->name[0]) snprintf(t->name, sizeof t->name, "%s", fallback_name);
        t->status = THEME_OK;
        note(t, THEME_ERROR, msg[0] ? msg : "can't be read");
    }
    snprintf(t->dir, sizeof t->dir, "%s", dir);
    snprintf(t->file, sizeof t->file, "%s", file);
    if (t->status != THEME_ERROR) check_theme(t);
    return 0;
}

static int cmp_name(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

int ui_load_themes(const char *assets_dir, const char *user_dir, theme_t *themes, int max)
{
    snprintf(g_assets, sizeof g_assets, "%s", assets_dir);
    int n = 0, usable = 0;
    for (int i = 0; i < N_BUILTIN && n < max; i++) {
        char path[640], dir[600];
        snprintf(dir, sizeof dir, "%s/themes", assets_dir);
        snprintf(path, sizeof path, "%s/%s", dir, BUILTIN[i]);
        memset(&themes[n], 0, sizeof themes[n]);
        load_one(path, "", "", BUILTIN[i], &themes[n]);
        if (themes[n].status != THEME_OK) fprintf(stderr, "%s: %s\n", path, themes[n].note);
        usable += themes[n].status != THEME_ERROR;
        n++;
    }
    if (!usable) return -1;

    DIR *d = user_dir ? opendir(user_dir) : NULL;
    char *names[UI_MAX_THEMES];
    int nn = 0;
    struct dirent *e;
    while (d && (e = readdir(d)) && nn < UI_MAX_THEMES) {
        if (e->d_name[0] == '.') continue;
        names[nn] = strdup(e->d_name);
        if (names[nn]) nn++;
    }
    if (d) closedir(d);
    qsort(names, (size_t)nn, sizeof *names, cmp_name);
    for (int i = 0; i < nn; i++) {
        char path[1200], dir[1000];
        struct stat st;
        snprintf(path, sizeof path, "%s/%s", user_dir, names[i]);
        const char *dot = strrchr(names[i], '.');
        if (n < max && stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
            snprintf(dir, sizeof dir, "%.999s", path);
            snprintf(path, sizeof path, "%s/theme.ini", dir);
            if (stat(path, &st) == 0) {
                char file[300];
                snprintf(file, sizeof file, "%.250s/theme.ini", names[i]);
                memset(&themes[n], 0, sizeof themes[n]);
                load_one(path, dir, file, names[i], &themes[n]);
                n++;
            }
        } else if (n < max && dot && !strcmp(dot, ".theme")) {
            memset(&themes[n], 0, sizeof themes[n]);
            load_one(path, user_dir, names[i], names[i], &themes[n]);
            n++;
        }
        free(names[i]);
    }
    return n;
}

/* ---------- the theme editor's files ---------- */

int ui_theme_write(const char *user_dir, const theme_t *t, const char *file, char *saved, size_t cap)
{
    static char text[8192];
    char rel[300], path[1200], tmp[1300];
    if (!user_dir || theme_format(t, text, sizeof text) < 0) return -1;
    if (file && file[0]) {
        if (strstr(file, "..") || file[0] == '/') return -1;
        snprintf(rel, sizeof rel, "%s", file);
    } else {                                   /* a new file, named after the theme (as the card allows) */
        char base[64];
        size_t n = 0;
        for (const char *c = t->name; *c && n + 1 < sizeof base; c++) {
            if (n == 0 && (*c == '.' || *c == ' ')) continue;
            base[n++] = (unsigned char)*c < 32 || strchr("/\\:*?\"<>|", *c) ? '-' : *c;
        }
        while (n && base[n - 1] == ' ') n--;
        base[n] = '\0';
        if (!n) snprintf(base, sizeof base, "Theme");
        mkdir(user_dir, 0755);
        for (int k = 1; k < 100; k++) {
            if (k == 1) snprintf(rel, sizeof rel, "%s.theme", base);
            else snprintf(rel, sizeof rel, "%s %d.theme", base, k);
            snprintf(path, sizeof path, "%s/%s", user_dir, rel);
            if (access(path, F_OK) != 0) break;
        }
    }
    snprintf(path, sizeof path, "%s/%s", user_dir, rel);
    snprintf(tmp, sizeof tmp, "%s.new", path);
    FILE *f = fopen(tmp, "we");
    if (!f) return -1;
    int ok = fputs(text, f) >= 0 && fflush(f) == 0 && fsync(fileno(f)) == 0;
    ok &= fclose(f) == 0;
    if (!ok || rename(tmp, path) != 0) { unlink(tmp); return -1; }
    snprintf(saved, cap, "%s", rel);
    return 0;
}

int ui_theme_delete(const char *user_dir, const char *file)
{
    char path[1200];
    if (!user_dir || !file || !file[0] || strchr(file, '/') || strstr(file, "..")) return -1;
    snprintf(path, sizeof path, "%s/%s", user_dir, file);
    return unlink(path);
}
