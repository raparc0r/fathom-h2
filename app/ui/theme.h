/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Theme files (THEMES.md): parsing and legibility validation. */
#ifndef FATHOM_THEME_H
#define FATHOM_THEME_H

#include <stddef.h>
#include <stdint.h>

typedef enum { THEME_CORNER_ROUND, THEME_CORNER_STEPPED, THEME_CORNER_SQUARE } theme_corner_t;

typedef enum { THEME_OK, THEME_WARN, THEME_ERROR } theme_status_t;

/* Pixel sizes of the five text roles. */
enum { THEME_PX_META, THEME_PX_BODY, THEME_PX_SUB, THEME_PX_TITLE, THEME_PX_HEAD, THEME_PX_N };
#define THEME_MIN_PX 15

/* Header icons a theme may replace with its own 16x16 PNGs ([icons] section). */
enum { THEME_ICON_PLAY, THEME_ICON_PAUSE, THEME_ICON_SHUFFLE, THEME_ICON_REPEAT, THEME_ICON_REPEAT_ONE,
       THEME_ICON_CHARGE, THEME_ICON_STOP, THEME_ICON_BLUETOOTH, THEME_ICON_WIFI, THEME_ICON_N };
#define THEME_ICON_PX 16
extern const char *const THEME_ICON_KEYS[THEME_ICON_N];

typedef struct {
    char name[48];
    char author[48];
    char licence[48];
    int  format;            /* [theme] format (1); unknown keys warn, so newer themes still load */
    int  dark;

    /* colours as 0xRRGGBB */
    uint32_t bg, surface, text, text_muted, select_bg, select_fg;
    uint32_t signal, info, mode, battery, warning, danger;
    uint32_t artist, album, chip_bg, chip_fg;   /* Now Playing (optional; defaults from the others) */

    char font[24];          /* built-in family: "atkinson" | "arkpixel" | "vt323" */
    char font_file[96];     /* or the theme's own font, relative to `dir` ([font] file) */
    char font_file_title[96];   /* optional heavier cut for titles and the header ([font] file_title) */
    int  font_px[THEME_PX_N];   /* [font] meta/body/sub/title/head pixel sizes; 0 = the family's default */
    char icon[THEME_ICON_N][96];   /* [icons] PNG files, relative to `dir`; "" = the built-in symbol */

    int  selection_bar;     /* 0 = pill, 1 = full-width bar */
    theme_corner_t corner;
    int  radius;
    int  shadow_hard;
    int  progress_segments;
    int  cursor_arrow;
    int  playing_equalizer;
    int  motion_stepped;
    int  icons_pixel;       /* Home's and the radio's icons drawn on the pixel grid (Pixel Pastel's) */

    /* set by whoever loads it */
    char dir[256];          /* the theme's folder (its fonts are relative to it); "" = built in */
    char file[128];         /* its file, relative to the card's themes folder ("Name.theme",
                             * "Name/theme.ini"); "" = built in */
    theme_status_t status;  /* loads fine / loads with a legibility warning / can't be used */
    char note[160];         /* the first warning or the error, for the theme list */
    int  unknown_keys;      /* keys this firmware doesn't know (ignored) */
} theme_t;

/* Parse theme text. Returns 0 on success; on error writes a message to err. */
int theme_parse(const char *text, theme_t *t, char *err, size_t errcap);
int theme_load_file(const char *path, theme_t *t, char *err, size_t errcap);

/* Legibility rules: contrast measured after RGB565 quantization, text sizes, and
 * (for themes without their own font file) a vetted built-in family.
 * Returns the number of failures; each one is described in err (newline separated). */
int theme_validate(const theme_t *t, char *err, size_t errcap);

/* The theme as a file theme_parse reads back the same (every key written out).
 * Returns the length, or -1 if it doesn't fit in cap. */
int theme_format(const theme_t *t, char *out, size_t cap);

/* Colours as hue (0..359), saturation and lightness (0..100), and back. */
void theme_to_hsl(uint32_t rgb, int *h, int *s, int *l);
uint32_t theme_from_hsl(int h, int s, int l);

/* The colour c, made lighter or darker (away from bg) until it reads at min
 * contrast on bg, keeping its hue; c itself if it already does. */
uint32_t theme_fit(uint32_t c, uint32_t bg, double min);

/* Made from a few choices (the theme editor): dark or light, and an accent hue
 * and saturation. theme_make sets every colour from them; theme_accent only
 * the selection, its text, signal and mode, against the theme's own bg. */
void theme_make(theme_t *t, int dark, int hue, int sat);
void theme_accent(theme_t *t, int hue, int sat);

/* WCAG contrast ratio between two colours after RGB565 quantization. */
double theme_contrast(uint32_t a, uint32_t b);
/* Colour as the panel shows it (RGB565 expanded back to 8 bits per channel). */
uint32_t theme_rgb565(uint32_t rgb);

#endif
