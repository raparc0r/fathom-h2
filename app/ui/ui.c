/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Phase-0 UI skeleton: one list model (28 px header + 5 rows at a 42 px pitch),
 * Now Playing, live theme switching, and motion:
 *   - the selection pill slides between rows and retargets mid-flight,
 *   - rows slide when the list scrolls (one extra row rendered above and below),
 *   - open/back: the new level slides over the old one (snapshot) with parallax,
 *   - Now Playing slides up over the list like a sheet, and back down.
 * Themes with `motion = stepped` move in whole 8 px steps. Animations never
 * block input: every new key retargets from wherever things currently are.
 * Lists come from the music library (row ids only; text per visible row) and
 * Now Playing from the player.
 */
#include "ui/ui.h"
#include "core/lang.h"
#include "core/settings.h"
#include "library/cue.h"
#include "library/playlist.h"

#include <math.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define HEADER_H   28
#define ROW_PITCH  42
#define ROW_H      38
#define ROWS       5
#define ROW_Y0     ((ROW_H - 38) / 2)   /* the playing-bars marker, centred in a row */
#define ROW_MARGIN 6
#define SLOTS      (ROWS + 2)   /* one spare row above and below for scrolling */
#define STEP       4            /* stepped-corner notch and hard-shadow offset (Pixel Pastel) */
#define SEGMENTS   24

#define DUR_SELECT 110          /* ms: pill moves to a neighbouring row */
#define DUR_SCROLL 120          /* ms: rows slide by one */
#define DUR_PUSH   200          /* ms: open a section / go back */
#define DUR_SHEET  240          /* ms: Now Playing slides up / down */
#define UNDER_SHIFT (UI_W / 3)  /* px the covered layer drifts (parallax) */
#define UNDER_DIM  110          /* opacity the covered layer loses (of 255) */
#define MOTION_STEP 8           /* px per step for `motion = stepped` */

/* ---------- menus ---------- */

/* Home: every item, in the default order. Settings > Home shows, hides and
 * reorders them (settings key home.items: ids in order, "-" before a hidden
 * one); Settings always stays. */
static const char *const HOME[] = {"Now playing", "Shuffle all songs", "Search", "Albums", "Artists",
                                   "Album artists", "Songs", "Playlists", "Favourites", "Recently added",
                                   "History", "Radio", "Settings", "Genres", "Composers", "Years", "Folders"};
static const char *const HOME_ID[] = {"nowplaying", "shuffle", "search", "albums", "artists",
                                      "albumartists", "songs", "playlists", "favourites", "recent",
                                      "history", "radio", "settings", "genres", "composers", "years", "folders"};
/* (Items added later come after Settings in this numbering, which pixel_icons.h
 * follows; on Home they go before Settings.) */
enum { H_NOWPLAYING, H_SHUFFLE, H_SEARCH, H_ALBUMS, H_ARTISTS, H_ALBUM_ARTISTS, H_SONGS, H_PLAYLISTS,
       H_FAVOURITES, H_RECENT_ADDED, H_HISTORY, H_RADIO, H_SETTINGS, H_GENRES, H_COMPOSERS, H_YEARS, H_FOLDERS, H_N };
/* Hidden until shown in Settings > Home (fewer people tag them) */
static int home_hidden_by_default(int h) { return h == H_COMPOSERS || h == H_YEARS; }
/* Their icons: Tabler Icons code points (player-play, arrows-shuffle, search,
 * disc, microphone-2, users, music, playlist, heart, clock-plus, history,
 * radio, settings; then category, feather, calendar, folder), and Pixel Pastel's own, drawn on its 10 px grid (pixel_icons.h,
 * from tools/icons/pixel_icons.py), in the same order. */
static const uint32_t HOME_ICON[H_N] = {0xed46, 0xf000, 0xeb1c, 0xea90, 0xef2c, 0xebf2,
                                        0xeafc, 0xeec0, 0xeabe, 0xf7c5, 0xebea, 0xef2d, 0xeb20,
                                        0xf1f6, 0xee8b, 0xea53, 0xeaad};   /* category, feather, calendar, folder */
#include "pixel_icons.h"
#define PIXEL_ICON_SCALE 2
#define ICON_W 22     /* the icon's column; the text starts after it */
#define ICON_GAP 10
#define SWATCH_W 30   /* Settings > Theme: a little picture of each theme */
#define SWATCH_H 20

/* Letters for the wheel: search (results last) and names (done last). */
static const char SEARCH_CHARS[] = "abcdefghijklmnopqrstuvwxyz0123456789 ";
static const char NAME_CHARS[] = "abcdefghijklmnopqrstuvwxyz0123456789 -'&.,!?()";
/* passwords: every printable character, in groups Menu jumps between */
static const char PASS_CHARS[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
                                 " !\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~";
static const int PASS_GROUPS[] = {0, 26, 52, 62};   /* a, A, 0, symbols */
static const char *const HISTORY[] = {"Recently played", "Most played"};
static const char *const SETTINGS[] = {"Theme", "Home", "Display", "Sound", "Bluetooth", "Wi-Fi",
                                       "Network drive", "SSH", "USB connection", "Playback", "Power", "Library",
                                       "About"};
enum { SET_THEME, SET_HOME, SET_DISPLAY, SET_SOUND, SET_BLUETOOTH, SET_WIFI, SET_SHARE, SET_SSH, SET_USB, SET_PLAYBACK,
       SET_POWER, SET_LIBRARY, SET_ABOUT };

/* Rows of the quick settings panel and of Settings > Display. */
typedef enum {
    ST_BRIGHTNESS, ST_SHUFFLE, ST_REPEAT, ST_SLEEP, ST_STOP_AFTER, ST_TIMEOUT,
    ST_DIM, ST_ON_CHARGING, ST_OUTPUT, ST_EQ, ST_EQ_TOGGLE, ST_LEVELLING, ST_DAC_FILTER, ST_PAUSE_UNPLUG, ST_AUTO_OFF,
    ST_RESCAN, ST_USB_DRIVE, ST_USB_COMPUTER, ST_USB_DAC, ST_N_SONGS, ST_N_ALBUMS, ST_N_ARTISTS, ST_N_PLAYLISTS,
    ST_BT, ST_BT_DEVICE, ST_WIFI, ST_WIFI_NET,
    ST_SHUF_BY, ST_SHUF_FRESH, ST_SHUF_INTERLUDES, ST_SHUF_WORKS, ST_SHUF_SPOKEN, ST_BALANCE, ST_VOL_LIMIT, ST_PURE, ST_TRANSPORT, ST_BATTERY, ST_LANGUAGE,
} setting_t;
/* Quick settings (quick_rows(): Bluetooth's and Wi-Fi's rows where they are) */
static const setting_t QUICK[] = {ST_BRIGHTNESS, ST_SHUFFLE, ST_REPEAT, ST_EQ_TOGGLE, ST_BT, ST_BT_DEVICE,
                                  ST_WIFI, ST_WIFI_NET, ST_SLEEP, ST_STOP_AFTER, ST_TIMEOUT};
static const setting_t SOUND_ROWS[] = {ST_OUTPUT, ST_PURE, ST_EQ, ST_BALANCE, ST_VOL_LIMIT, ST_LEVELLING, ST_TRANSPORT,
                                       ST_USB_DAC, ST_DAC_FILTER};   /* sound_rows(): those with their hooks */
static const setting_t PLAYBACK_ROWS[] = {ST_SHUFFLE, ST_SHUF_BY, ST_SHUF_FRESH, ST_SHUF_INTERLUDES, ST_SHUF_WORKS,
                                         ST_SHUF_SPOKEN, ST_REPEAT, ST_PAUSE_UNPLUG};

/* Settings > Playback's shuffle rows, to the player (at start and when changed). */
static void shuffle_prefs_apply(void);
static const setting_t POWER_ROWS[] = {ST_BATTERY, ST_AUTO_OFF, ST_SLEEP};
static const setting_t LIBRARY_ROWS[] = {ST_RESCAN, ST_N_SONGS, ST_N_ALBUMS, ST_N_ARTISTS, ST_N_PLAYLISTS};
static const setting_t USB_ROWS[] = {ST_USB_COMPUTER, ST_USB_DRIVE, ST_USB_DAC};
static const int AUTO_OFFS[] = {5, 15, 30, 0};               /* minutes; 0 = never */
static const int SLEEPS[] = {0, 15, 30, 45, 60, 90, PLAYER_SLEEP_END_ALBUM};   /* minutes; 0 = off */
static const setting_t DISPLAY_ROWS[] = {ST_BRIGHTNESS, ST_TIMEOUT, ST_DIM, ST_ON_CHARGING, ST_LANGUAGE};
static const int TIMEOUTS[] = {15, 30, 60, 120, 300, 0};   /* seconds; 0 = never */

/* Context menu actions. */
typedef enum {
    CTX_PLAY, CTX_PLAY_NEXT, CTX_ADD_QUEUE, CTX_SHOW_QUEUE, CTX_GO_ALBUM, CTX_GO_ARTIST, CTX_INFO,
    CTX_Q_PLAY, CTX_Q_MOVE, CTX_Q_REMOVE, CTX_Q_CLEAR, CTX_FAV_ADD, CTX_FAV_REMOVE,
    CTX_ADD_PLAYLIST, CTX_PL_REMOVE, CTX_PL_MOVE, CTX_PL_DELETE, CTX_PL_RENAME, CTX_HOME_MOVE,
    CTX_BT_FORGET, CTX_WIFI_FORGET,
} ctx_action_t;
static const char *const CTX_LABEL[] = {
    "Play", "Play next", "Add to queue", "Show queue", "Go to album", "Go to artist", "Info",
    "Play now", "Move", "Remove from queue", "Clear queue", "Add to favourites", "Remove from favourites",
    "Add to playlist", "Remove from playlist", "Move", "Delete playlist", "Rename playlist", "Move",
    "Forget this device", "Forget this network",
};

/* Rows of the track info screen. */
enum { INFO_TITLE, INFO_ARTIST, INFO_ALBUM, INFO_TRACK, INFO_FORMAT, INFO_LENGTH, INFO_FILE, INFO_N };

/* ---------- state ---------- */

typedef enum {
    L_HOME, L_ALBUMS, L_ALBUM_ARTISTS, L_ARTIST_ALBUMS, L_ARTISTS, L_CREDIT_ALBUMS, L_CREDIT_TRACKS,
    L_GENRES, L_GENRE_ALBUMS, L_COMPOSERS, L_COMPOSER_ALBUMS, L_COMPOSER_TRACKS, L_YEARS, L_YEAR_ALBUMS, L_FOLDER,
    L_TRACKS, L_SONGS, L_PLAYLISTS, L_PLAYLIST_TRACKS, L_HOME_EDIT,
    L_FAVOURITES, L_RECENT_ADDED, L_RECENT_PLAYED, L_MOST_PLAYED, L_HISTORY,
    L_SETTINGS, L_THEMES, L_QUICK, L_DISPLAY, L_SOUND, L_EQ, L_EQ_EDIT, L_EQ_BAND, L_BLUETOOTH, L_WIFI, L_WIFI_PASS, L_SHARE, L_SSH, L_USB, L_PLAYBACK, L_POWER, L_LIBRARY, L_ABOUT, L_SIGNAL,
    L_CONTEXT, L_INFO, L_QUEUE, L_PICK_PLAYLIST, L_SEARCH, L_RENAME, L_USB_ASK, L_RADIO, L_PLACEHOLDER
} list_kind_t;

typedef struct {
    list_kind_t kind;
    int sel, top;
    int64_t parent;         /* artist, album or playlist id */
    int64_t parent2;        /* L_CREDIT_TRACKS, L_COMPOSER_TRACKS: the album (parent: the artist, composer) */
    lib_list_t items;       /* library-backed levels: row ids */
    char title[128];
    int back_to_np;         /* opened from Now Playing: closing it goes back there */
} level_t;

typedef enum { R_SNAP, R_ANIMATE, R_CONTENT } render_mode_t;
typedef enum { TR_NONE, TR_FORWARD, TR_BACK, TR_NP_IN, TR_NP_OUT } trans_t;

static struct {
    const theme_t *themes; int n_themes, cur_theme;
    const ui_fonts_t *fonts; int n_fonts;
    const ui_fonts_t *f;

    level_t stack[8]; int depth;
    int now_playing_view;   /* 1 when Now Playing is shown */
    /* Now Playing: the wheel moves a marker (scrub_s); OK jumps there, Back or
     * SCRUB_IDLE_MS without turning cancel */
    int scrubbing, scrub_s;
    uint32_t scrub_last, scrub_seq;
    lv_obj_t *np_marker;
    int battery_pct;        /* -1 = unknown (hidden) */
    lv_obj_t *toast, *toast_lbl;
    lv_timer_t *toast_timer;
    lv_obj_t *lvl, *lvl_lbl, *lvl_bar;     /* level bar overlay (brightness) */
    lv_obj_t *letter, *letter_lbl;          /* fast scrolling: the current initial */
    lv_timer_t *letter_timer;
    uint32_t wheel_last; int wheel_streak;  /* wheel acceleration */

    /* typing with the wheel (Search, Rename): the text, and the letter shown */
    char entry[64];
    int entry_ch;                           /* index into the letters; == strlen(letters): results / done */
    uint32_t ok_tick;                       /* when OK last added a letter (a second OK soon after: double-click) */
    int ok_added;
    lib_search_t found;                     /* Search: the results */
    int64_t rename_id;
    lv_timer_t *lvl_timer;
    int charging;
    ui_hooks_t hooks;
    int editing;            /* a value row is being changed with the wheel */

    int moving;             /* Queue: the selected track moves with the wheel */
    int home_order[H_N];    /* every Home item, in the order set in Settings > Home */
    int home_hidden[H_N];   /* by item */
    int home[H_N], home_n;  /* the items shown on Home */
    lv_timer_t *marquee_timer;  /* starts scrolling long text after a pause */
    lv_obj_t *np_next, *np_out, *np_pause, *empty;
    int lib_counts[4];      /* songs, albums, artists, playlists (Library screen) */
    uint32_t reset_armed;   /* About > Reset: first press time; a second within 3 s resets */
    char output[80];
    int pure, bitperfect;   /* Pure mode for this output; ...and nothing changes the samples */
    uint32_t seek_last; int seek_streak;

    /* the context menu's subject and actions */
    ctx_action_t ctx[12]; int ctx_n;
    int64_t ctx_track, ctx_album, ctx_artist, ctx_playlist;
    int ctx_qidx;           /* queue row the menu is for, -1 if none */
    int ctx_plpos;          /* row in your own playlist the menu is for, -1 if none */
    lib_list_t pick_ids;    /* "Add to playlist": the tracks to add */
    lib_list_t pick_pls;    /* ...and your playlists to choose from */
    uint32_t delete_armed;  /* "Delete playlist": first press time */

    library_t *lib;
    player_t *player;
    player_now_t now;       /* refreshed on every render */
    uint32_t np_seq;        /* player seq the Now Playing art belongs to */
    uint16_t art_px[LIB_ART_LARGE * LIB_ART_LARGE];
    lv_image_dsc_t art_dsc;

    /* what the list showed last time, to decide which animation applies */
    int last_depth, last_top, last_np;
    list_kind_t last_kind; int64_t last_parent;

    /* animation state (current values, so new input can retarget smoothly) */
    int pill_y, pill_target, rows_off, colored_slot;

    /* screen transitions: a snapshot ("ghost") of the old screen moves with the new one */
    lv_obj_t *ghost;
    lv_draw_buf_t *ghost_buf, *pending;
    trans_t tr;
    int text_x0;            /* left text indent for rows (room for the cursor) */

    /* widgets */
    lv_obj_t *scr, *header, *h_left, *h_right;
    lv_obj_t *list, *rows_box, *pill, *pill_notch[4], *pill_cursor;
    lv_obj_t *row[SLOTS], *row_lbl[SLOTS], *row_meta[SLOTS], *row_dot[SLOTS], *row_eq[SLOTS][3];
    lv_obj_t *row_icon[SLOTS], *row_picon[SLOTS];   /* Home: an icon glyph, or a pixel icon */
    lv_obj_t *row_sw[SLOTS], *row_sw_part[SLOTS][3];  /* Theme: a little picture of each (bar, text, accent) */
    lv_obj_t *row_wheel[SLOTS], *row_ch[SLOTS][3];   /* the typing row: letter before, the wheel's (larger), after */
    int slot_item[SLOTS], slot_playing[SLOTS];
    lv_obj_t *np_radio, *np_radio_px;   /* the radio's sign where a cover would be */
    lv_obj_t *np, *np_shadow, *np_art, *np_title, *np_artist, *np_album, *np_chip,
             *np_track, *np_fill, *np_seg[SEGMENTS], *np_t0, *np_t1, *np_img, *h_mode;
    /* header indicators: each slot shows the theme's icon or the built-in symbol */
#define HDR_SLOTS 6
    lv_obj_t *h_slot_img[HDR_SLOTS], *h_slot_sym[HDR_SLOTS];
    lv_obj_t *h_pulse;      /* the charging icon, pulsing (NULL: none) */
    int asleep;             /* the screen is off: no animations, nothing drawn */
    int theme_gen;          /* +1 at each apply_theme() (the header lays itself out again) */
    render_mode_t wake_mode;   /* the render put off while asleep: R_SNAP if a level or view changed */
    int ticking;            /* rendering for ui_tick_1s (nothing asked for it) */
    unsigned lib_gen;       /* +1 when the library changed (rows read again) */
    struct fill_sig {       /* a library list's rows as last filled */
        const level_t *l; list_kind_t kind; int64_t parent, parent2, track, album;
        const int64_t *ids; int n, top, sel, active, moving, theme_gen; unsigned lib_gen;
    } fill_sig;
    int64_t next_id; unsigned next_gen;   /* Now Playing's "Next:", as last read */
    char next_text[256];
    /* Settings > Bluetooth, as of the last look */
    ui_bt_dev_t bt[16];
    int bt_n, bt_on, bt_link, wifi_link;
    char bt_busy[32], ctx_bt[18];
    /* Settings > Wi-Fi, as of the last look */
    ui_wifi_status_t wifi_st;
    ui_wifi_net_t wifi[24];
    int wifi_n, wifi_on;
    char wifi_busy[32], wifi_pass_ssid[33], ctx_wifi[33];
    /* Settings > Network drive, as of the last look */
    ui_share_t share;
    int share_on;
    ui_ssh_t ssh;
    int ssh_on;
    /* the EQ: the output playing now, its profile ("" = off), the profiles on the card */
    char eq_out[64], eq_name[64];
    char eq_names[64][64];
    int eq_n;
    /* the EQ editor: the profile being edited, its name, a delete awaiting confirmation */
    eq_profile_t eqe;
    char eqe_name[64];
    uint32_t eqe_del_armed;
    /* Radio: the stations on the card (-1: no fathom/radio.txt) */
    radio_station_t radio[RADIO_MAX];
    int radio_n;
    int radio_art;          /* art_px holds the radio's cover */
} S;

/* ---------- helpers ---------- */

static lv_color_t C(uint32_t rgb) { return lv_color_hex(rgb); }
static const theme_t *T(void) { return &S.themes[S.cur_theme]; }

static lv_obj_t *box(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static lv_obj_t *label(lv_obj_t *parent)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    return l;
}

/* fill() and show() change an object only when it differs: LVGL redraws an
 * object on every style or flag call, even one that changes nothing, and the
 * screens are rendered again every second (a whole screen redrawn and sent
 * to the panel once a second, at rest). */
static void fill(lv_obj_t *o, uint32_t rgb)
{
    if (!lv_color_eq(lv_obj_get_style_bg_color(o, 0), C(rgb))) lv_obj_set_style_bg_color(o, C(rgb), 0);
    if (lv_obj_get_style_bg_opa(o, 0) != LV_OPA_COVER) lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
}

static void show(lv_obj_t *o, int on)
{
    if (!on == lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return;
    if (on) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

/* Change a label only when something really differs: any text or style update
 * makes LVGL lay the label out again, which restarts a scrolling label from
 * its start (the screens re-render every second). */
static void text_copy_free(lv_event_t *e) { free(lv_obj_get_user_data(lv_event_get_target(e))); }

/* The comparison is with a copy of what was set (the label's user data): a
 * label cut short with "..." holds its text with the dots in, so it never
 * compares equal, and was laid out and redrawn every second. (Labels set
 * here are set only here, once they have been.) */
static void set_text(lv_obj_t *l, const char *s)
{
    char *had = lv_obj_get_user_data(l);
    if (had ? !strcmp(had, s) : !strcmp(lv_label_get_text(l) ? lv_label_get_text(l) : "", s)) return;
    lv_label_set_text(l, s);
    size_t n = strlen(s) + 1;
    char *copy = realloc(had, n);
    if (!copy) { free(had); lv_obj_set_user_data(l, NULL); return; }
    memcpy(copy, s, n);
    if (!had) lv_obj_add_event_cb(l, text_copy_free, LV_EVENT_DELETE, NULL);
    lv_obj_set_user_data(l, copy);
}

static void set_pos_width(lv_obj_t *o, int x, int y, int w)
{
    if (lv_obj_get_style_x(o, 0) != x || lv_obj_get_style_y(o, 0) != y) lv_obj_set_pos(o, x, y);
    if (lv_obj_get_style_width(o, 0) != w) lv_obj_set_width(o, w);
}

static void set_text_color(lv_obj_t *o, lv_color_t c)
{
    if (!lv_color_eq(lv_obj_get_style_text_color(o, 0), c)) lv_obj_set_style_text_color(o, c, 0);
}

static void fmt_time(char *b, size_t n, int secs, int neg)
{
    snprintf(b, n, "%s%d:%02d", neg ? "-" : "", secs / 60, secs % 60);
}

/* For `motion = stepped`: snap the remaining distance to whole steps. */
static int quantize(int v, int target)
{
    if (!T()->motion_stepped) return v;
    return target + (v - target) / MOTION_STEP * MOTION_STEP;
}

static int library_backed(list_kind_t k)
{
    return k == L_ALBUMS || k == L_ALBUM_ARTISTS || k == L_ARTIST_ALBUMS || k == L_ARTISTS ||
           k == L_CREDIT_ALBUMS || k == L_CREDIT_TRACKS || k == L_TRACKS || k == L_SONGS ||
           k == L_PLAYLISTS || k == L_PLAYLIST_TRACKS || k == L_FAVOURITES || k == L_RECENT_ADDED ||
           k == L_RECENT_PLAYED || k == L_MOST_PLAYED || k == L_GENRES || k == L_GENRE_ALBUMS ||
           k == L_COMPOSERS || k == L_COMPOSER_ALBUMS || k == L_COMPOSER_TRACKS || k == L_YEARS ||
           k == L_YEAR_ALBUMS || k == L_FOLDER;
}

/* Lists of songs (play from the chosen one) and lists of albums (open one). */
static int is_track_list(list_kind_t k)
{
    return k == L_TRACKS || k == L_SONGS || k == L_PLAYLIST_TRACKS || k == L_FAVOURITES ||
           k == L_RECENT_PLAYED || k == L_MOST_PLAYED || k == L_CREDIT_TRACKS || k == L_COMPOSER_TRACKS;
}

static int is_album_list(list_kind_t k)
{
    return k == L_ALBUMS || k == L_ARTIST_ALBUMS || k == L_RECENT_ADDED || k == L_CREDIT_ALBUMS ||
           k == L_GENRE_ALBUMS || k == L_COMPOSER_ALBUMS || k == L_YEAR_ALBUMS;
}

static lib_view_t view_of(list_kind_t k)
{
    switch (k) {
    case L_ALBUMS: return LIB_ALBUMS;
    case L_ALBUM_ARTISTS: return LIB_ALBUM_ARTISTS;
    case L_ARTIST_ALBUMS: return LIB_ARTIST_ALBUMS;
    case L_ARTISTS: return LIB_ARTISTS;
    case L_CREDIT_ALBUMS: return LIB_CREDIT_ALBUMS;
    case L_CREDIT_TRACKS: return LIB_CREDIT_TRACKS;
    case L_TRACKS: return LIB_ALBUM_TRACKS;
    case L_PLAYLISTS: return LIB_PLAYLISTS;
    case L_PLAYLIST_TRACKS: return LIB_PLAYLIST_TRACKS;
    case L_FAVOURITES: return LIB_FAVOURITES;
    case L_RECENT_ADDED: return LIB_RECENT_ADDED;
    case L_RECENT_PLAYED: return LIB_RECENT_PLAYED;
    case L_MOST_PLAYED: return LIB_MOST_PLAYED;
    case L_GENRES: return LIB_GENRES;
    case L_GENRE_ALBUMS: return LIB_GENRE_ALBUMS;
    case L_COMPOSERS: return LIB_COMPOSERS;
    case L_COMPOSER_ALBUMS: return LIB_COMPOSER_ALBUMS;
    case L_COMPOSER_TRACKS: return LIB_COMPOSER_TRACKS;
    case L_YEARS: return LIB_YEARS;
    case L_YEAR_ALBUMS: return LIB_YEAR_ALBUMS;
    case L_FOLDER: return LIB_FOLDER;
    default: return LIB_SONGS;
    }
}

/* ---------- Home items ---------- */

static void home_rebuild(void)
{
    S.home_n = 0;
    for (int i = 0; i < H_N; i++)
        if (!S.home_hidden[S.home_order[i]] || S.home_order[i] == H_SETTINGS) S.home[S.home_n++] = S.home_order[i];
}

/* home.items: ids in order, "-" before a hidden one; unknown ids are ignored
 * and items missing from it (new ones) are added, shown, before Settings. */
static void home_load(void)
{
    int n = 0, seen[H_N] = {0};
    memset(S.home_hidden, 0, sizeof S.home_hidden);
    char buf[512];
    snprintf(buf, sizeof buf, "%s", settings_get("home.items", ""));
    for (char *save = NULL, *t = strtok_r(buf, ", ", &save); t; t = strtok_r(NULL, ", ", &save)) {
        int hidden = *t == '-';
        if (hidden) t++;
        for (int h = 0; h < H_N; h++)
            if (!strcmp(t, HOME_ID[h]) && !seen[h]) {
                seen[h] = 1;
                S.home_order[n++] = h;
                S.home_hidden[h] = hidden && h != H_SETTINGS;
            }
    }
    for (int h = 0; h < H_N; h++) {
        if (seen[h]) continue;
        int at = n;
        for (int i = 0; i < n; i++)
            if (S.home_order[i] == H_SETTINGS) at = i;
        memmove(&S.home_order[at + 1], &S.home_order[at], (size_t)(n - at) * sizeof S.home_order[0]);
        S.home_order[at] = h;
        S.home_hidden[h] = home_hidden_by_default(h);
        n++;
    }
    home_rebuild();
}

/* Settings > Home changed something: keep it, and keep Home's selection valid. */
static void home_changed(void)
{
    char buf[512] = "";
    size_t w = 0;
    for (int i = 0; i < H_N && w < sizeof buf; i++) {
        int h = S.home_order[i];
        w += (size_t)snprintf(buf + w, sizeof buf - w, "%s%s%s", i ? "," : "", S.home_hidden[h] ? "-" : "", HOME_ID[h]);
    }
    int was = S.stack[0].sel < S.home_n ? S.home[S.stack[0].sel] : H_SETTINGS;
    settings_set("home.items", buf);
    home_rebuild();
    int sel = 0;
    for (int i = 0; i < S.home_n; i++)
        if (S.home[i] == was) sel = i;
    S.stack[0].sel = sel;
    if (S.stack[0].top > sel) S.stack[0].top = sel;
}

/* Where item h is on Home (0 if it's hidden). */
static int home_index(int h)
{
    for (int i = 0; i < S.home_n; i++)
        if (S.home[i] == h) return i;
    return 0;
}

static int quick_n(void);

/* Settings > Bluetooth: what the platform says now. */
static void bt_look(void)
{
    S.bt_n = 0;
    S.bt_on = 0;
    S.bt_busy[0] = '\0';
    if (!S.hooks.bt_state) return;
    S.bt_on = S.hooks.bt_state(S.bt_busy, sizeof S.bt_busy);
    if (S.bt_on && S.hooks.bt_devices) S.bt_n = S.hooks.bt_devices(S.bt, (int)(sizeof S.bt / sizeof S.bt[0]));
}

/* Settings > Wi-Fi: what the platform says now. */
static void wifi_look(void)
{
    S.wifi_n = 0;
    S.wifi_on = 0;
    S.wifi_busy[0] = '\0';
    memset(&S.wifi_st, 0, sizeof S.wifi_st);
    if (!S.hooks.wifi_state) return;
    S.wifi_on = S.hooks.wifi_state(S.wifi_busy, sizeof S.wifi_busy, &S.wifi_st);
    if (S.wifi_on && S.hooks.wifi_networks)
        S.wifi_n = S.hooks.wifi_networks(S.wifi, (int)(sizeof S.wifi / sizeof S.wifi[0]));
}

static void share_look(void)
{
    memset(&S.share, 0, sizeof S.share);
    S.share_on = S.hooks.share_state ? S.hooks.share_state(&S.share) : 0;
}

static void ssh_look(void)
{
    memset(&S.ssh, 0, sizeof S.ssh);
    S.ssh_on = S.hooks.ssh_state ? S.hooks.ssh_state(&S.ssh) : 0;
}

static int sound_rows(setting_t *r);
static int list_count(const level_t *l)
{
    if (library_backed(l->kind)) return l->items.n;
    switch (l->kind) {
    case L_HOME: return S.home_n;
    case L_HOME_EDIT: return H_N;
    case L_USB_ASK: return 3;
    case L_SETTINGS: return (int)(sizeof SETTINGS / sizeof SETTINGS[0]);
    case L_BLUETOOTH: bt_look(); return S.bt_on ? 2 + S.bt_n : 1;
    case L_WIFI: wifi_look(); return S.wifi_on ? 3 + S.wifi_n : 1;   /* on/off, the connection, search, networks */
    case L_WIFI_PASS: return 1;
    case L_SHARE: share_look(); return !S.share_on ? 1 : S.share.state == UI_SHARE_ON ? 7 : 2;   /* on/off, then how to reach it */
    case L_SSH: ssh_look(); return !S.ssh_on ? 1 : S.ssh.state == UI_SSH_ON ? 6 : 2;
    case L_HISTORY: return (int)(sizeof HISTORY / sizeof HISTORY[0]);
    case L_RADIO: return S.radio_n > 0 ? S.radio_n : 0;
    case L_THEMES: return S.n_themes;
    case L_EQ: return 1 + S.eq_n + 1;              /* Off, the profiles, New EQ */
    case L_EQ_EDIT: return 1 + S.eqe.n + (S.eqe.n < EQ_MAX_BANDS) + 1;   /* preamp, bands, add, delete */
    case L_EQ_BAND: return 5;                      /* type, frequency, gain, Q, remove */
    case L_QUICK: bt_look(); wifi_look(); return quick_n();
    case L_USB: return (int)(sizeof USB_ROWS / sizeof USB_ROWS[0]);
    case L_DISPLAY: return (int)(sizeof DISPLAY_ROWS / sizeof DISPLAY_ROWS[0]);
    case L_SOUND: {
        setting_t r[sizeof SOUND_ROWS / sizeof SOUND_ROWS[0]];
        return sound_rows(r);
    }
    case L_PLAYBACK: return (int)(sizeof PLAYBACK_ROWS / sizeof PLAYBACK_ROWS[0]);
    case L_POWER: return (int)(sizeof POWER_ROWS / sizeof POWER_ROWS[0]);
    case L_LIBRARY: return (int)(sizeof LIBRARY_ROWS / sizeof LIBRARY_ROWS[0]);
    case L_SIGNAL: {
        int n = 0;
        char a[8], b[8];
        while (S.hooks.signal_path && n < 16 && S.hooks.signal_path(n, a, sizeof a, b, sizeof b) == 0) n++;
        return n;
    }
    case L_ABOUT: {
        int n = 0;
        char a[8], b[8];
        while (S.hooks.about && n < 32 && S.hooks.about(n, a, sizeof a, b, sizeof b) == 0) n++;
        return n + 1;   /* + Reset settings */
    }
    case L_CONTEXT: return S.ctx_n;
    case L_INFO: return INFO_N;
    case L_QUEUE: return S.player ? player_queue_len(S.player) : 0;
    case L_PICK_PLAYLIST: return 1 + S.pick_pls.n;   /* "New playlist" + yours */
    case L_SEARCH: return 1 + S.found.artists.n + S.found.albums.n + S.found.songs.n;
    case L_RENAME: return 1;
    default: return 1;
    }
}

/* ---------- settings the UI owns ---------- */

static int brightness(void)
{
    int v = settings_get_int("display.brightness", UI_BRIGHTNESS_DEFAULT);
    return v < 1 ? 1 : v > UI_BRIGHTNESS_LEVELS ? UI_BRIGHTNESS_LEVELS : v;
}

static void level_bar(const char *text, int value, int max) { ui_level(text, value, max, NULL); }

static void set_brightness(int v)
{
    v = v < 1 ? 1 : v > UI_BRIGHTNESS_LEVELS ? UI_BRIGHTNESS_LEVELS : v;
    settings_set_int("display.brightness", v);
    if (S.hooks.brightness) S.hooks.brightness(v);
    level_bar("Brightness", v, UI_BRIGHTNESS_LEVELS);
}

static int screen_timeout(void) { return settings_get_int("display.timeout", UI_TIMEOUT_DEFAULT); }

static void next_timeout(void)
{
    int cur = screen_timeout(), n = (int)(sizeof TIMEOUTS / sizeof TIMEOUTS[0]), i = 0;
    while (i < n && TIMEOUTS[i] != cur) i++;
    int v = TIMEOUTS[(i + 1) % n];
    settings_set_int("display.timeout", v);
    if (S.hooks.screen_timeout) S.hooks.screen_timeout(v);
}

/* Quick settings' rows: Bluetooth's and Wi-Fi's only where there are. */
static int quick_rows(setting_t *out)
{
    int n = 0;
    for (size_t i = 0; i < sizeof QUICK / sizeof QUICK[0]; i++) {
        if ((QUICK[i] == ST_BT || QUICK[i] == ST_BT_DEVICE) && !S.hooks.bt_state) continue;
        if ((QUICK[i] == ST_WIFI || QUICK[i] == ST_WIFI_NET) && !S.hooks.wifi_state) continue;
        out[n++] = QUICK[i];
    }
    return n;
}

static int quick_n(void)
{
    setting_t r[sizeof QUICK / sizeof QUICK[0]];
    return quick_rows(r);
}

/* Settings > Sound's rows: those whose hooks are there. */
static int sound_rows(setting_t *r)
{
    int n = 0;
    for (size_t i = 0; i < sizeof SOUND_ROWS / sizeof SOUND_ROWS[0]; i++) {
        setting_t st = SOUND_ROWS[i];
        if (st == ST_DAC_FILTER && !S.hooks.dac_filter_name) continue;
        if ((st == ST_BALANCE || st == ST_VOL_LIMIT) && !S.hooks.sound_value) continue;
        if (st == ST_PURE && !S.hooks.pure_toggle) continue;
        if (st == ST_TRANSPORT && !S.hooks.transport_toggle) continue;
        r[n++] = st;
    }
    return n;
}

static setting_t setting_at(const level_t *l, int i)
{
    switch (l->kind) {
    case L_QUICK: {
        setting_t r[sizeof QUICK / sizeof QUICK[0]];
        int n = quick_rows(r);
        return i < n ? r[i] : QUICK[0];
    }
    case L_USB: return USB_ROWS[i];
    case L_SOUND: {
        setting_t r[sizeof SOUND_ROWS / sizeof SOUND_ROWS[0]];
        int n = sound_rows(r);
        return i < n ? r[i] : SOUND_ROWS[0];
    }
    case L_PLAYBACK: return PLAYBACK_ROWS[i];
    case L_POWER: return POWER_ROWS[i];
    case L_LIBRARY: return LIBRARY_ROWS[i];
    default: return DISPLAY_ROWS[i];
    }
}

static int is_setting_level(list_kind_t k)
{
    return k == L_QUICK || k == L_DISPLAY || k == L_SOUND || k == L_PLAYBACK || k == L_POWER || k == L_LIBRARY ||
           k == L_USB;
}

static const char *on_off(int v) { return v ? "On" : "Off"; }

/* ---------- the EQ editor ---------- */

static const char *const EQ_TYPE_NAME[] = {"Peak", "Low shelf", "High shelf"};

static void fmt_hz(char *out, size_t cap, double f)
{
    if (f < 1000) snprintf(out, cap, "%.0f Hz", f);
    else if (f < 10000) snprintf(out, cap, "%.1f kHz", f / 1000);
    else snprintf(out, cap, "%.0f kHz", f / 1000);
}

static void eqe_row(int i, char *main, size_t mn, char *meta, size_t tn)
{
    int n = S.eqe.n;
    if (i == 0) {
        snprintf(main, mn, "Preamp");
        snprintf(meta, tn, S.editing && S.stack[S.depth - 1].sel == 0 ? "< %+.1f dB >" : "%+.1f dB", S.eqe.preamp_db);
    } else if (i <= n) {
        const eq_band_t *b = &S.eqe.band[i - 1];
        char hz[24];
        fmt_hz(hz, sizeof hz, b->fc);
        snprintf(main, mn, "%s %s", EQ_TYPE_NAME[b->type], hz);
        snprintf(meta, tn, "%+.1f dB", b->gain_db);
    } else if (i == n + 1 && n < EQ_MAX_BANDS) {
        snprintf(main, mn, "Add a band");
    } else {
        snprintf(main, mn, "Delete this EQ");
    }
}

static void eqe_band_row(int b, int i, char *main, size_t mn, char *meta, size_t tn)
{
    if (b < 0 || b >= S.eqe.n) return;
    const eq_band_t *bd = &S.eqe.band[b];
    int ed = S.editing && S.stack[S.depth - 1].sel == i;
    char v[32] = "";
    switch (i) {
    case 0: snprintf(main, mn, "Type"); snprintf(v, sizeof v, "%s", EQ_TYPE_NAME[bd->type]); break;
    case 1: snprintf(main, mn, "Frequency"); fmt_hz(v, sizeof v, bd->fc); break;
    case 2: snprintf(main, mn, "Gain"); snprintf(v, sizeof v, "%+.1f dB", bd->gain_db); break;
    case 3: snprintf(main, mn, "Q"); snprintf(v, sizeof v, "%.2f", bd->q); break;
    default: snprintf(main, mn, "Remove this band"); return;
    }
    snprintf(meta, tn, ed ? "< %s >" : "%s", v);
}

static int band_by_freq(const void *a, const void *b)
{
    double x = ((const eq_band_t *)a)->fc, y = ((const eq_band_t *)b)->fc;
    return x < y ? -1 : x > y;
}

/* Hear the profile being edited (and save it: on the card at once). */
static void eqe_changed(int save)
{
    if (S.hooks.eq_preview) S.hooks.eq_preview(&S.eqe);
    if (save && S.hooks.eq_put && S.hooks.eq_put(S.eqe_name, &S.eqe)) ui_toast("Couldn't save the EQ", 2500);
}

static double clampd(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }

/* The next frequency on a 1/12-octave grid through 1 kHz (so twelve steps
 * from 1 kHz are 2 kHz exactly), to three figures: 1.06 kHz, 530 Hz. */
static double freq_step(double f, int d)
{
    double idx = round(12 * log2(f / 1000)) + d, v = 1000 * pow(2, idx / 12);
    double unit = v >= 10000 ? 100 : v >= 1000 ? 10 : 1;
    return clampd(round(v / unit) * unit, 20, 20000);
}

/* The wheel on a value being edited: d = +1 (clockwise) or -1. */
static void eqe_adjust(const level_t *l, int d)
{
    if (l->kind == L_EQ_EDIT) {
        if (l->sel == 0) S.eqe.preamp_db = clampd(round(S.eqe.preamp_db * 2 + d) / 2, -24, 6);
    } else if (l->parent >= 0 && l->parent < S.eqe.n) {
        eq_band_t *b = &S.eqe.band[l->parent];
        if (l->sel == 1) b->fc = freq_step(b->fc, d);                                        /* 1/12 octave */
        else if (l->sel == 2) b->gain_db = clampd(round(b->gain_db * 2 + d) / 2, -18, 18);  /* 0.5 dB */
        else if (l->sel == 3) b->q = clampd(round(b->q * pow(2, d / 8.0) * 100) / 100, 0.1, 10);
    }
    eqe_changed(0);
}

static void push(list_kind_t kind, const char *title, int64_t parent);

/* Opens the editor on a profile; it plays while it's being edited. */
static void eqe_open(const char *name)
{
    if (!S.hooks.eq_get || S.hooks.eq_get(name, &S.eqe)) { ui_toast("Couldn't read this EQ", 2500); return; }
    snprintf(S.eqe_name, sizeof S.eqe_name, "%s", name);
    qsort(S.eqe.band, (size_t)S.eqe.n, sizeof S.eqe.band[0], band_by_freq);
    S.eqe_del_armed = 0;
    push(L_EQ_EDIT, name, 0);
    if (S.hooks.eq_preview) S.hooks.eq_preview(&S.eqe);
}

/* New EQ: "My EQ" (2, 3...), five flat bands to shape, chosen for this output. */
static void eqe_new(void)
{
    char name[64] = "My EQ";
    for (int k = 2; k < 100; k++) {
        int taken = 0;
        for (int i = 0; i < S.eq_n; i++) taken |= !strcasecmp(S.eq_names[i], name);
        if (!taken) break;
        snprintf(name, sizeof name, "My EQ %d", k);
    }
    eq_profile_t p = {.preamp_db = 0, .n = 5, .band = {
        {EQ_LOW_SHELF, 105, 0, 0.7}, {EQ_PEAK, 250, 0, 1}, {EQ_PEAK, 1000, 0, 1},
        {EQ_PEAK, 4000, 0, 1}, {EQ_HIGH_SHELF, 10000, 0, 0.7}}};
    if (!S.hooks.eq_put || S.hooks.eq_put(name, &p)) { ui_toast("Couldn't save the EQ", 2500); return; }
    if (S.hooks.eq_set) S.hooks.eq_set(name);
    S.eq_n = S.hooks.eq_list ? S.hooks.eq_list(S.eq_names, 64) : 0;
    eqe_open(name);
}

static void setting_row(setting_t st, int editing, char *main, size_t mn, char *meta, size_t tn)
{
    switch (st) {
    case ST_BRIGHTNESS:
        snprintf(main, mn, "Brightness");
        snprintf(meta, tn, editing ? "< %d / %d >" : "%d / %d", brightness(), UI_BRIGHTNESS_LEVELS);
        break;
    case ST_SHUFFLE:
        snprintf(main, mn, "Shuffle");
        snprintf(meta, tn, "%s", S.player && player_shuffle(S.player) ? "On" : "Off");
        break;
    case ST_REPEAT: {
        static const char *const R[] = {"Off", "All", "One"};
        snprintf(main, mn, "Repeat");
        snprintf(meta, tn, "%s", R[S.player ? player_repeat(S.player) : 0]);
        break;
    }
    case ST_SLEEP:
        snprintf(main, mn, "Sleep timer");
        if (S.player && player_sleep_minutes(S.player) == PLAYER_SLEEP_END_ALBUM) snprintf(meta, tn, "End of album");
        else if (S.now.sleep_s > 0) snprintf(meta, tn, "%d min left", (S.now.sleep_s + 59) / 60);
        else snprintf(meta, tn, "Off");
        break;
    case ST_STOP_AFTER:
        snprintf(main, mn, "Stop after this track");
        snprintf(meta, tn, "%s", S.player && player_stop_after(S.player) ? "On" : "Off");
        break;
    case ST_DIM:
        snprintf(main, mn, "Dim before turning off");
        snprintf(meta, tn, "%s", on_off(settings_get_int("display.dim", 1)));
        break;
    case ST_ON_CHARGING:
        snprintf(main, mn, "Stay on while charging");
        snprintf(meta, tn, "%s", on_off(settings_get_int("display.on_charging", 0)));
        break;
    case ST_OUTPUT:
        snprintf(main, mn, "Output");
        snprintf(meta, tn, "%.*s", (int)tn - 1, S.output[0] ? S.output : "-");   /* long names are cut */
        break;
    case ST_LEVELLING: {
        static const char *const how[] = {"Off", "Track", "Album"};
        int v = settings_get_int("sound.levelling", LEVEL_OFF);
        snprintf(main, mn, "Volume levelling");
        snprintf(meta, tn, "%s", S.pure ? "Bypassed" : how[v >= 0 && v <= 2 ? v : 0]);
        break;
    }
    case ST_EQ:
    case ST_EQ_TOGGLE:   /* (quick settings: Play turns it off and back on, to compare) */
        snprintf(main, mn, "EQ");
        snprintf(meta, tn, "%.*s", (int)tn - 1, S.pure ? "Bypassed" : S.eq_name[0] ? S.eq_name : "Off");
        break;
    case ST_LANGUAGE: {
        const char *lang = settings_get("display.language", "");
        snprintf(main, mn, "Language");
        snprintf(meta, tn, "%.*s", (int)tn - 1, lang[0] ? lang : "English");
        break;
    }
    case ST_BATTERY:
        snprintf(main, mn, "Battery");
        if (S.hooks.battery_info) S.hooks.battery_info(meta, tn);
        else snprintf(meta, tn, "-");
        break;
    case ST_PURE:
        snprintf(main, mn, "Pure mode");
        snprintf(meta, tn, "%s", on_off(S.pure));
        break;
    case ST_TRANSPORT:
        snprintf(main, mn, "Transport mode");
        snprintf(meta, tn, "%s", on_off(settings_get_int("sound.transport", 0)));
        break;
    case ST_BALANCE:
    case ST_VOL_LIMIT: {
        char v[48] = "";
        snprintf(main, mn, "%s", st == ST_BALANCE ? "Balance" : "Volume limit");
        if (st == ST_BALANCE && S.pure) { snprintf(meta, tn, "Bypassed"); break; }
        if (S.hooks.sound_value) S.hooks.sound_value(st == ST_BALANCE ? UI_SOUND_BALANCE : UI_SOUND_LIMIT, v, sizeof v);
        snprintf(meta, tn, editing ? "< %s >" : "%s", v);
        break;
    }
    case ST_DAC_FILTER:
        snprintf(main, mn, "DAC filter");
        if (S.hooks.dac_filter_name) S.hooks.dac_filter_name(meta, tn);
        break;
    case ST_SHUF_BY:
        snprintf(main, mn, "Shuffle by");
        snprintf(meta, tn, "%s", settings_get_int("shuffle.albums", 0) ? "Albums" : "Songs");
        break;
    case ST_SHUF_FRESH:
        snprintf(main, mn, "Favour unheard songs");
        snprintf(meta, tn, "%s", on_off(settings_get_int("shuffle.freshness", 1)));
        break;
    case ST_SHUF_INTERLUDES:
        snprintf(main, mn, "Skip short interludes");
        snprintf(meta, tn, "%s", on_off(settings_get_int("shuffle.skip_interludes", 1)));
        break;
    case ST_SHUF_WORKS:
        snprintf(main, mn, "Keep works together");
        snprintf(meta, tn, "%s", on_off(settings_get_int("shuffle.keep_works", 0)));
        break;
    case ST_SHUF_SPOKEN:
        snprintf(main, mn, "Leave out audiobooks");
        snprintf(meta, tn, "%s", on_off(settings_get_int("shuffle.exclude_spoken", 0)));
        break;
    case ST_PAUSE_UNPLUG:
        snprintf(main, mn, "Pause on unplug");
        snprintf(meta, tn, "%s", on_off(settings_get_int("playback.pause_on_unplug", 1)));
        break;
    case ST_AUTO_OFF: {
        int m = settings_get_int("power.auto_off", 15);
        snprintf(main, mn, "Power off when idle");
        if (m <= 0) snprintf(meta, tn, "Never");
        else snprintf(meta, tn, "%d min", m);
        break;
    }
    case ST_RESCAN: snprintf(main, mn, "Rescan the music folder"); break;
    case ST_USB_DRIVE: snprintf(main, mn, "USB drive mode"); break;
    case ST_USB_COMPUTER: {
        static const char *const what[] = {"Ask", "USB drive", "Just charge", "USB DAC"};
        int v = settings_get_int("usb.computer", UI_USB_ASK);
        snprintf(main, mn, "On a computer");
        snprintf(meta, tn, "%s", what[v >= 0 && v <= 3 ? v : 0]);
        break;
    }
    case ST_USB_DAC: snprintf(main, mn, "USB DAC mode"); break;
    case ST_BT:
        snprintf(main, mn, "Bluetooth");
        snprintf(meta, tn, "%s", S.bt_busy[0] && strcmp(S.bt_busy, "Searching") ? S.bt_busy : on_off(S.bt_on));
        break;
    case ST_WIFI:
        snprintf(main, mn, "Wi-Fi");
        snprintf(meta, tn, "%s", S.wifi_busy[0] && strcmp(S.wifi_busy, "Searching") ? S.wifi_busy : on_off(S.wifi_on));
        break;
    case ST_WIFI_NET:
        snprintf(main, mn, "Wi-Fi network");
        snprintf(meta, tn, "%.*s", (int)tn - 1, S.wifi_st.state == UI_WIFI_CONNECTED ? S.wifi_st.ssid
                                                : S.wifi_on ? "None" : "");
        break;
    case ST_BT_DEVICE: {
        snprintf(main, mn, "Bluetooth device");
        const char *c = NULL;
        for (int d = 0; d < S.bt_n; d++)
            if (S.bt[d].connected) c = S.bt[d].name[0] ? S.bt[d].name : S.bt[d].addr;
        snprintf(meta, tn, "%s", c ? c : S.bt_on ? "None" : "");
        break;
    }
    case ST_N_SONGS: snprintf(main, mn, "Songs"); snprintf(meta, tn, "%d", S.lib_counts[0]); break;
    case ST_N_ALBUMS: snprintf(main, mn, "Albums"); snprintf(meta, tn, "%d", S.lib_counts[1]); break;
    case ST_N_ARTISTS: snprintf(main, mn, "Artists"); snprintf(meta, tn, "%d", S.lib_counts[2]); break;
    case ST_N_PLAYLISTS: snprintf(main, mn, "Playlists"); snprintf(meta, tn, "%d", S.lib_counts[3]); break;
    case ST_TIMEOUT: {
        int t = screen_timeout();
        snprintf(main, mn, "Screen off after");
        if (t <= 0) snprintf(meta, tn, "Never");
        else if (t < 60) snprintf(meta, tn, "%d s", t);
        else snprintf(meta, tn, "%d min", t / 60);
        break;
    }
    }
}

static void info_row(int64_t track, int i, char *main, size_t mn, char *meta, size_t tn)
{
    static const char *const LABEL[INFO_N] = {"Title", "Artist", "Album", "Track", "Format", "Length", "File"};
    snprintf(meta, tn, "%s", LABEL[i]);
    lib_track_t t;
    if (!S.lib || library_track(S.lib, track, &t)) { snprintf(main, mn, "-"); return; }
    switch (i) {
    case INFO_TITLE: snprintf(main, mn, "%s", t.title); break;
    case INFO_ARTIST: snprintf(main, mn, "%s", t.artist); break;
    case INFO_ALBUM: snprintf(main, mn, "%s", t.album); break;
    case INFO_TRACK:
        if (t.disc > 1) snprintf(main, mn, "Disc %d, track %d", t.disc, t.track);
        else snprintf(main, mn, "%d", t.track);
        break;
    case INFO_FORMAT:
        if (t.bits == 1) snprintf(main, mn, "%s %.4g MHz", t.codec, t.sample_rate / 1e6);   /* DSD */
        else if (t.bits) snprintf(main, mn, "%s %d-bit %.1f kHz", t.codec, t.bits, t.sample_rate / 1000.0);
        else snprintf(main, mn, "%s %.1f kHz", t.codec, t.sample_rate / 1000.0);
        break;
    case INFO_LENGTH: snprintf(main, mn, "%u:%02u", t.duration_ms / 60000, (t.duration_ms / 1000) % 60); break;
    case INFO_FILE: {
        const char *b = strrchr(t.path, '/'), *sfx = cue_part_suffix(t.path);   /* a CUE track: its file */
        b = b ? b + 1 : t.path;
        snprintf(main, mn, "%.*s", sfx ? (int)(sfx - b) : 255, b);
        break;
    }
    }
}

/* ---------- typing with the wheel ---------- */

#define WHEEL_W 84             /* the typing row's letters, at its right */
#define DOUBLE_CLICK_MS 350   /* OK twice within this: Results (Search) or Done (Rename) */

static const char *entry_letters(list_kind_t k)
{
    return k == L_SEARCH ? SEARCH_CHARS : k == L_WIFI_PASS ? PASS_CHARS : NAME_CHARS;
}

static void entry_glyph(list_kind_t k, int i, char *out, size_t cap)
{
    const char *letters = entry_letters(k);
    int n = (int)strlen(letters);
    i = ((i % (n + 1)) + n + 1) % (n + 1);
    if (i == n) snprintf(out, cap, "%s", k == L_SEARCH ? "Results" : k == L_WIFI_PASS ? "Join" : "Done");
    else if (letters[i] == ' ') snprintf(out, cap, "space");
    else if (k == L_WIFI_PASS) snprintf(out, cap, "%c", letters[i]);   /* exactly what's typed */
    else snprintf(out, cap, "%c", letters[i] >= 'a' && letters[i] <= 'z' ? letters[i] - 32 : letters[i]);
}

/* The typing row: what's typed so far, and the wheel's letter with its
 * neighbours ("d E f", the wheel's letter drawn larger). A word (Results,
 * Done, space) is shown alone, with no neighbours. */
static void entry_row(list_kind_t k, char *main, size_t mn, char wheel[3][16])
{
    if (k == L_SEARCH && !S.entry[0]) snprintf(main, mn, "Type with the wheel");
    else if (k == L_WIFI_PASS && !S.entry[0]) snprintf(main, mn, "Password");
    else snprintf(main, mn, "%s_", S.entry);
    entry_glyph(k, S.entry_ch - 1, wheel[0], 16);
    entry_glyph(k, S.entry_ch, wheel[1], 16);
    entry_glyph(k, S.entry_ch + 1, wheel[2], 16);
    /* the neighbours in lower case, so the letter OK adds stands out */
    for (int w = 0; w < 3; w += 2) {
        if (strlen(wheel[1]) > 1 || strlen(wheel[w]) > 1) wheel[w][0] = '\0';
        else if (k != L_WIFI_PASS && wheel[w][0] >= 'A' && wheel[w][0] <= 'Z') wheel[w][0] += 32;
    }
}

static int is_typing_row(const level_t *l, int i)
{
    return (l->kind == L_SEARCH || l->kind == L_RENAME || l->kind == L_WIFI_PASS) && i == 0;
}

/* Search result r: artists, then albums, then songs. */
static int search_kind(int r, int64_t *id)
{
    if (r < S.found.artists.n) { *id = S.found.artists.ids[r]; return 0; }
    r -= S.found.artists.n;
    if (r < S.found.albums.n) { *id = S.found.albums.ids[r]; return 1; }
    r -= S.found.albums.n;
    *id = r < S.found.songs.n ? S.found.songs.ids[r] : -1;
    return 2;
}

static void search_row(int r, char *main, size_t mn, char *meta, size_t tn)
{
    int64_t id;
    int kind = search_kind(r, &id);
    if (!S.lib || id < 0) return;
    if (kind == 0) { library_row(S.lib, LIB_ARTISTS, id, main, mn, meta, tn); snprintf(meta, tn, "Artist"); }
    else if (kind == 1) { library_row(S.lib, LIB_ALBUMS, id, main, mn, meta, tn); snprintf(meta, tn, "Album"); }
    else library_row(S.lib, LIB_PLAYLIST_TRACKS, id, main, mn, meta, tn);
}

static void search_run(void)
{
    library_search_free(&S.found);
    if (S.lib && S.entry[0]) library_search(S.lib, S.entry, &S.found);
}

static void list_item(const level_t *l, int i, char *main, size_t mn, char *meta, size_t tn, int *playing)
{
    *playing = 0;
    main[0] = meta[0] = '\0';
    if (library_backed(l->kind)) {
        int64_t id = l->items.ids[i];
        if (S.lib) library_row(S.lib, view_of(l->kind), id, main, mn, meta, tn);
        if (S.moving && l->kind == L_PLAYLIST_TRACKS && i == l->sel) snprintf(meta, tn, "Moving");
        if (S.now.active) {
            if (is_track_list(l->kind) || l->kind == L_FOLDER) *playing = id == S.now.track.id;
            else if (is_album_list(l->kind)) *playing = id == S.now.track.album_id;
        }
        return;
    }
    switch (l->kind) {
    case L_HOME:
        snprintf(main, mn, "%s", HOME[S.home[i]]);
        if (S.home[i] == H_NOWPLAYING) *playing = S.now.playing;
        break;
    case L_USB_ASK:
        snprintf(main, mn, "%s", i == 0 ? "Use as a USB drive" : i == 1 ? "Use as a USB DAC" : "Keep playing");
        break;
    case L_HOME_EDIT: {
        int h = S.home_order[i];
        snprintf(main, mn, "%s", HOME[h]);
        snprintf(meta, tn, "%s", S.moving && i == l->sel ? "Moving" : h == H_SETTINGS ? "Always"
                                 : S.home_hidden[h] ? "Hidden" : "Shown");
        break;
    }
    case L_SETTINGS: snprintf(main, mn, "%s", SETTINGS[i]); break;
    case L_BLUETOOTH:
        if (!S.hooks.bt_state) { snprintf(main, mn, "Not available"); break; }
        if (i == 0) {
            snprintf(main, mn, "Bluetooth");
            snprintf(meta, tn, "%s", S.bt_busy[0] && strcmp(S.bt_busy, "Searching") ? S.bt_busy : S.bt_on ? "On" : "Off");
        } else if (i == 1) {
            snprintf(main, mn, "Find headphones");
            if (!strcmp(S.bt_busy, "Searching")) snprintf(meta, tn, "Searching");
        } else if (i - 2 < S.bt_n) {
            const ui_bt_dev_t *d = &S.bt[i - 2];
            snprintf(main, mn, "%s", d->name[0] ? d->name : d->addr);
            snprintf(meta, tn, "%s", d->connected ? "Connected" : d->paired ? "Paired" : "Pair");
            *playing = d->connected;
        }
        break;
    case L_WIFI:
        if (!S.hooks.wifi_state) { snprintf(main, mn, "Not available"); break; }
        if (i == 0) {
            snprintf(main, mn, "Wi-Fi");
            snprintf(meta, tn, "%s", S.wifi_busy[0] && strcmp(S.wifi_busy, "Searching") ? S.wifi_busy : S.wifi_on ? "On" : "Off");
        } else if (i == 1) {
            const ui_wifi_status_t *st = &S.wifi_st;
            if (st->state == UI_WIFI_CONNECTED) {
                snprintf(main, mn, "%s", st->ssid);
                snprintf(meta, tn, "%s", st->ip);
                *playing = 1;
            } else {
                snprintf(main, mn, "%s", st->state == UI_WIFI_CONNECTING ? "Connecting" : "Not connected");
                if (st->state == UI_WIFI_CONNECTING) snprintf(meta, tn, "%.*s", (int)tn - 1, st->ssid);
            }
        } else if (i == 2) {
            snprintf(main, mn, "Search again");
            if (!strcmp(S.wifi_busy, "Searching")) snprintf(meta, tn, "Searching");
        } else if (i - 3 < S.wifi_n) {
            const ui_wifi_net_t *w = &S.wifi[i - 3];
            snprintf(main, mn, "%s", w->ssid);
            const char *sig = w->dbm >= -55 ? "Strong" : w->dbm >= -67 ? "Good" : w->dbm >= -78 ? "Fair" : "Weak";
            if (w->connected) snprintf(meta, tn, "Connected");
            else if (w->saved) snprintf(meta, tn, "Saved");
            else snprintf(meta, tn, "%s%s", sig, w->secured ? "" : ", open");
            *playing = w->connected;
        }
        break;
    case L_WIFI_PASS: { char wheel[3][16]; entry_row(l->kind, main, mn, wheel); break; }
    case L_SHARE: {
        const ui_share_t *st = &S.share;
        if (!S.hooks.share_state) { snprintf(main, mn, "Not available"); break; }
        if (i == 0) {
            snprintf(main, mn, "Network drive");
            snprintf(meta, tn, "%s", S.share_on && st->state == UI_SHARE_STARTING ? "Starting" : S.share_on ? "On" : "Off");
        } else if (st->state != UI_SHARE_ON) {
            snprintf(main, mn, "%s", st->state == UI_SHARE_FAILED ? "Couldn't start it" : "Waiting for Wi-Fi");
        } else if (i == 1) {
            snprintf(main, mn, "Address");
            snprintf(meta, tn, "%s", st->host);
        } else if (i == 2) {
            snprintf(main, mn, "IP address");
            snprintf(meta, tn, "%s", st->ip);
        } else if (i == 3) {
            snprintf(main, mn, "User");
            snprintf(meta, tn, "%s", st->user);
        } else if (i == 4) {
            snprintf(main, mn, "Password");
            snprintf(meta, tn, "%s", st->password);
        } else if (i == 5) {
            snprintf(main, mn, "Certificate");
            snprintf(meta, tn, "%s", st->cert);
        } else {
            snprintf(main, mn, "New password");
        }
        break;
    }
    case L_SSH: {
        const ui_ssh_t *st = &S.ssh;
        if (!S.hooks.ssh_state) { snprintf(main, mn, "Not available"); break; }
        if (i == 0) {
            snprintf(main, mn, "SSH");
            snprintf(meta, tn, "%s", S.ssh_on && st->state == UI_SSH_STARTING ? "Starting" : S.ssh_on ? "On" : "Off");
        } else if (st->state != UI_SSH_ON) {
            snprintf(main, mn, "%s", st->state == UI_SSH_FAILED ? "Couldn't start it" : "Starting");
        } else if (i == 1) {
            snprintf(main, mn, "Over USB");
            snprintf(meta, tn, "%s", st->usb_ip[0] ? st->usb_ip : "Not connected");
        } else if (i == 2) {
            snprintf(main, mn, "Over Wi-Fi");
            snprintf(meta, tn, "%s", st->wifi_ip[0] ? st->wifi_ip : "Not connected");
        } else if (i == 3) {
            snprintf(main, mn, "User");
            snprintf(meta, tn, "root");
        } else if (i == 4) {
            snprintf(main, mn, "Password");
            snprintf(meta, tn, "%s", st->password);
        } else {
            snprintf(main, mn, "New password");
        }
        break;
    }
    case L_HISTORY: snprintf(main, mn, "%s", HISTORY[i]); break;
    case L_RADIO:
        snprintf(main, mn, "%s", S.radio[i].name);
        if (S.now.radio && S.now.index == i && S.now.playing) {
            *playing = 1;
            snprintf(meta, tn, "%s", S.now.connecting ? "Connecting" : "Playing");
        }
        break;
    case L_QUICK: case L_DISPLAY: case L_SOUND: case L_PLAYBACK: case L_POWER: case L_LIBRARY: case L_USB:
        setting_row(setting_at(l, i), S.editing && i == l->sel, main, mn, meta, tn);
        break;
    case L_SIGNAL: if (S.hooks.signal_path) S.hooks.signal_path(i, main, mn, meta, tn); break;
    case L_ABOUT:
        if (i == list_count(l) - 1) snprintf(main, mn, "Reset all settings");
        else if (S.hooks.about) S.hooks.about(i, main, mn, meta, tn);
        break;
    case L_CONTEXT: snprintf(main, mn, "%s", CTX_LABEL[S.ctx[i]]); break;
    case L_INFO: info_row(l->parent, i, main, mn, meta, tn); break;
    case L_SEARCH:
    case L_RENAME:
        if (i == 0) { char wheel[3][16]; entry_row(l->kind, main, mn, wheel); }
        else search_row(i - 1, main, mn, meta, tn);
        break;
    case L_PICK_PLAYLIST:
        if (i == 0) snprintf(main, mn, "New playlist");
        else if (S.lib) library_row(S.lib, LIB_PLAYLISTS, S.pick_pls.ids[i - 1], main, mn, meta, tn);
        break;
    case L_QUEUE: {
        int64_t id = S.player ? player_queue_at(S.player, i) : -1;
        if (S.lib && id >= 0) library_row(S.lib, LIB_PLAYLIST_TRACKS, id, main, mn, meta, tn);
        if (S.moving && i == l->sel) snprintf(meta, tn, "Moving");
        break;
    }
    case L_EQ:
        if (i == 0) {
            snprintf(main, mn, "Off");
            snprintf(meta, tn, "bit-perfect");
            *playing = !S.eq_name[0];
        } else if (i - 1 < S.eq_n) {
            snprintf(main, mn, "%s", S.eq_names[i - 1]);
            *playing = !strcmp(S.eq_names[i - 1], S.eq_name);
        } else {
            snprintf(main, mn, "New EQ");
        }
        break;
    case L_EQ_EDIT: eqe_row(i, main, mn, meta, tn); break;
    case L_EQ_BAND: eqe_band_row((int)l->parent, i, main, mn, meta, tn); break;
    case L_THEMES:
        snprintf(main, mn, "%s", S.themes[i].name);
        if (S.themes[i].status == THEME_ERROR) snprintf(meta, tn, "can't load");
        else if (S.themes[i].status == THEME_WARN) snprintf(meta, tn, "warning");
        else if (i == 0) snprintf(meta, tn, "default");
        *playing = i == S.cur_theme;
        break;
    default: snprintf(main, mn, "-"); break;
    }
}

/* ---------- styling ---------- */

/* A one-line label in a box of height h, its capitals centred: fonts' line
 * heights differ a lot (Ark Pixel's has room above and below the letters), so
 * the box follows the letters, not the line, and fits between the lines
 * around it whatever the font. */
#define CHIP_H 22
#define NP_ALBUM_Y 98      /* Now Playing: the album's line, and "Next:" */
#define NP_NEXT_Y 148
/* Where a font's capitals start below the top of its line, and how tall they are. */
static void cap_metrics(const lv_font_t *f, int *cap_top, int *cap_h)
{
    lv_font_glyph_dsc_t g;
    int lh = lv_font_get_line_height(f);
    *cap_h = lh * 2 / 3;
    *cap_top = (lh - *cap_h) / 2;
    if (lv_font_get_glyph_dsc(f, &g, 'F', 0) && g.box_h > 0) {
        *cap_h = g.box_h;
        *cap_top = lh - f->base_line - (g.ofs_y + g.box_h);
    }
}

static void chip_fit(lv_obj_t *o, int h)
{
    int cap_top, cap_h;
    cap_metrics(lv_obj_get_style_text_font(o, 0), &cap_top, &cap_h);
    int top = h / 2 - (cap_top + cap_h / 2);
    lv_obj_set_height(o, h);
    lv_obj_set_style_pad_top(o, top, 0);
    lv_obj_set_style_pad_bottom(o, 0, 0);
}

static void style_label(lv_obj_t *l, const lv_font_t *font, uint32_t color)
{
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, C(color), 0);
}

static void apply_theme(void)
{
    const theme_t *t = T();
    S.f = &S.fonts[0];
    for (int i = 0; i < S.n_fonts; i++)
        if (!strcmp(S.fonts[i].family, t->font)) S.f = &S.fonts[i];
    if (S.hooks.fonts) {
        const ui_fonts_t *own = S.hooks.fonts(t);
        if (own) S.f = own;
    }

    S.theme_gen++;
    fill(S.scr, t->bg);
    fill(S.list, t->bg);   /* opaque: a snapshot may sit underneath during transitions */
    fill(S.np, t->bg);
    style_label(S.h_left, S.f->head, t->text);
    style_label(S.h_right, S.f->meta, t->battery);
    style_label(S.h_mode, &lv_font_montserrat_14, t->mode);
    for (int i = 0; i < HDR_SLOTS; i++) style_label(S.h_slot_sym[i], &lv_font_montserrat_14, t->mode);

    int radius = t->corner == THEME_CORNER_ROUND ? t->radius : 0;
    lv_obj_set_style_radius(S.pill, radius, 0);
    fill(S.pill, t->select_bg);
    for (int k = 0; k < 4; k++) {
        fill(S.pill_notch[k], t->bg);
        show(S.pill_notch[k], t->corner == THEME_CORNER_STEPPED);
    }
    show(S.pill_cursor, t->cursor_arrow);
    style_label(S.pill_cursor, &lv_font_montserrat_14, t->select_fg);
    S.text_x0 = t->cursor_arrow ? 24 : 10;

    for (int r = 0; r < SLOTS; r++) {
        lv_obj_set_style_radius(S.row_dot[r], LV_RADIUS_CIRCLE, 0);
        fill(S.row_dot[r], t->signal);
        style_label(S.row_lbl[r], S.f->body, t->text);
        style_label(S.row_meta[r], S.f->meta, t->text_muted);
        style_label(S.row_icon[r], S.f->icon ? S.f->icon : S.f->body, t->text_muted);
        for (int w = 0; w < 3; w++) style_label(S.row_ch[r][w], w == 1 ? S.f->title : S.f->meta, t->text_muted);
    }
    S.colored_slot = -1;

    /* Now Playing */
    lv_obj_set_style_radius(S.np_art, radius, 0);
    fill(S.np_art, 0x2B3F63);
    style_label(S.np_radio, S.f->icon_big ? S.f->icon_big : S.f->icon ? S.f->icon : S.f->title, 0xC9D4E8);
    lv_obj_set_style_transform_scale(S.np_radio, S.f->icon_big ? 256 : 256 * 3, 0);
    lv_obj_set_style_transform_pivot_x(S.np_radio, LV_PCT(50), 0);
    lv_obj_set_style_transform_pivot_y(S.np_radio, LV_PCT(50), 0);
    lv_obj_set_style_image_recolor(S.np_radio_px, C(0xC9D4E8), 0);
    fill(S.np_shadow, 0x0B0918);
    show(S.np_shadow, t->shadow_hard);
    style_label(S.np_title, S.f->title, t->text);
    style_label(S.np_artist, S.f->sub, t->artist);
    style_label(S.np_album, S.f->meta, t->album);
    style_label(S.np_next, S.f->meta, t->text_muted);
    style_label(S.np_out, S.f->meta, t->info);
    style_label(S.empty, S.f->body, t->text_muted);
    style_label(S.np_chip, S.f->meta, t->chip_fg);
    chip_fit(S.np_chip, CHIP_H);
    {   /* ...centred between the album's baseline and the capitals of "Next:" */
        int ct, ch;
        const lv_font_t *af = S.f->meta;
        int album_base = NP_ALBUM_Y + lv_font_get_line_height(af) - af->base_line;
        cap_metrics(S.f->meta, &ct, &ch);
        int next_top = NP_NEXT_Y + ct;
        lv_obj_set_y(S.np_chip, (album_base + next_top) / 2 - CHIP_H / 2 + 1);
    }
    fill(S.np_chip, t->chip_bg);
    lv_obj_set_style_radius(S.np_chip, t->corner == THEME_CORNER_ROUND ? LV_RADIUS_CIRCLE : 0, 0);
    fill(S.np_track, t->surface);
    fill(S.np_fill, t->signal);
    lv_obj_set_style_radius(S.np_track, t->corner == THEME_CORNER_ROUND ? 3 : 0, 0);
    lv_obj_set_style_radius(S.np_fill, t->corner == THEME_CORNER_ROUND ? 3 : 0, 0);
    show(S.np_track, !t->progress_segments);
    for (int i = 0; i < SEGMENTS; i++) show(S.np_seg[i], t->progress_segments);
    style_label(S.np_t0, S.f->meta, t->text_muted);
    style_label(S.np_t1, S.f->meta, t->text_muted);
}

/* Colour every row; `sel_slot` is the row the pill is over (text flips as the pill arrives). */
static void color_slots(int sel_slot)
{
    if (sel_slot == S.colored_slot) return;
    const theme_t *t = T();
    for (int j = 0; j < SLOTS; j++) {
        int sel = j == sel_slot;
        set_text_color(S.row_lbl[j], C(sel ? t->select_fg : t->text));
        set_text_color(S.row_meta[j], C(sel ? t->select_fg : t->text_muted));
        set_text_color(S.row_icon[j], C(sel ? t->select_fg : t->text_muted));
        lv_color_t ic = C(sel ? t->select_fg : t->text_muted);
        if (!lv_color_eq(lv_obj_get_style_image_recolor(S.row_picon[j], 0), ic))
            lv_obj_set_style_image_recolor(S.row_picon[j], ic, 0);
        for (int w = 0; w < 3; w++) set_text_color(S.row_ch[j][w], C(sel ? t->select_fg : w == 1 ? t->text : t->text_muted));
        for (int k = 0; k < 3; k++) fill(S.row_eq[j][k], sel ? t->select_fg : t->signal);
        show(S.row_dot[j], S.slot_playing[j] && !t->playing_equalizer && !sel);
    }
    S.colored_slot = sel_slot;
}

/* ---------- animation callbacks ---------- */

static int floor_div(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

/* Slot whose row is under the pill's centre right now (slot 0 is the spare row above). */
static int slot_under_pill(void)
{
    return floor_div(S.pill_y - S.rows_off + ROW_PITCH / 2, ROW_PITCH) + 1;
}

static void rows_box_place(void) { lv_obj_set_y(S.rows_box, -ROW_PITCH + S.rows_off); }

static void anim_pill(void *var, int32_t v)
{
    (void)var;
    S.pill_y = quantize(v, S.pill_target);
    lv_obj_set_y(S.pill, S.pill_y);
    color_slots(slot_under_pill());
}

static void anim_rows(void *var, int32_t v)
{
    (void)var;
    S.rows_off = quantize(v, 0);
    rows_box_place();
    color_slots(slot_under_pill());  /* text under the pill is always the highlight colour */
}

static void run(lv_obj_t *var, lv_anim_exec_xcb_t cb, int from, int to, int ms)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, var);
    lv_anim_set_exec_cb(&a, cb);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, ms);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

static void stop_list_anims(void)
{
    lv_anim_delete(S.pill, anim_pill);
    lv_anim_delete(S.rows_box, anim_rows);
}

/* ---------- screen transitions ---------- */

/* What the ghost image shows between transitions. LVGL warns on a NULL source,
 * and the image must not keep pointing at a freed snapshot. */
static const uint16_t ghost_empty_px[1];
static const lv_image_dsc_t ghost_empty = {
    .header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565, .w = 1, .h = 1, .stride = 2},
    .data = (const uint8_t *)ghost_empty_px,
    .data_size = sizeof ghost_empty_px,
};

static void ghost_free(void)
{
    if (S.ghost_buf) {
        lv_image_set_src(S.ghost, &ghost_empty);
        lv_image_cache_drop(S.ghost_buf);
        lv_draw_buf_destroy(S.ghost_buf);
        S.ghost_buf = NULL;
    }
    show(S.ghost, 0);
}

static void pending_free(void)
{
    if (S.pending) { lv_draw_buf_destroy(S.pending); S.pending = NULL; }
}

static void layers_at_rest(void)
{
    lv_obj_set_pos(S.list, 0, HEADER_H);
    lv_obj_set_style_opa(S.list, LV_OPA_COVER, 0);
    lv_obj_set_pos(S.np, 0, HEADER_H);
    lv_obj_set_style_opa(S.np, LV_OPA_COVER, 0);
}

static void anim_transition(void *var, int32_t p);

/* Jump any running transition to its end state. */
static void finish_transition(void)
{
    if (S.tr != TR_NONE) lv_anim_delete(S.ghost, anim_transition);
    S.tr = TR_NONE;
    ghost_free();
    layers_at_rest();
}

static void transition_done(lv_anim_t *a)
{
    (void)a;
    S.tr = TR_NONE;
    ghost_free();
    layers_at_rest();
}

static int dim_at(int p)   /* covered layer opacity for progress p (0..1024) towards covered */
{
    int opa = 255 - UNDER_DIM * p / 1024;
    return T()->motion_stepped ? 255 - (255 - opa) / 32 * 32 : opa;
}

/* p goes 0..1024. The "live" layer is the real new screen; the ghost is the old one. */
static void anim_transition(void *var, int32_t p)
{
    (void)var;
    const int H = UI_H - HEADER_H;
    switch (S.tr) {
    case TR_FORWARD:   /* new level on top, sliding in from the right */
        lv_obj_set_x(S.list, quantize(UI_W * (1024 - p) / 1024, 0));
        lv_obj_set_x(S.ghost, quantize(-UNDER_SHIFT * p / 1024, -UNDER_SHIFT));
        lv_obj_set_style_opa(S.ghost, dim_at(p), 0);
        break;
    case TR_BACK:      /* old level on top, sliding out to the right */
        lv_obj_set_x(S.ghost, quantize(UI_W * p / 1024, UI_W));
        lv_obj_set_x(S.list, quantize(-UNDER_SHIFT * (1024 - p) / 1024, 0));
        lv_obj_set_style_opa(S.list, dim_at(1024 - p), 0);
        break;
    case TR_NP_IN:     /* Now Playing slides up over the list */
        lv_obj_set_y(S.np, HEADER_H + quantize(H * (1024 - p) / 1024, 0));
        lv_obj_set_style_opa(S.ghost, dim_at(p), 0);
        break;
    case TR_NP_OUT:    /* Now Playing slides down, revealing the list */
        lv_obj_set_y(S.ghost, HEADER_H + quantize(H * p / 1024, H));
        lv_obj_set_style_opa(S.list, dim_at(1024 - p), 0);
        break;
    default: break;
    }
}

/* Snapshot what is on screen now, before a key changes it. */
static void capture(void)
{
    finish_transition();
    pending_free();
    lv_obj_update_layout(S.scr);
    S.pending = lv_snapshot_take(S.now_playing_view ? S.np : S.list, LV_COLOR_FORMAT_RGB565);
}

static void start_transition(trans_t tr)
{
    if (!S.pending) return;               /* no snapshot: just show the new screen */
    S.ghost_buf = S.pending;
    S.pending = NULL;
    lv_image_set_src(S.ghost, S.ghost_buf);
    lv_obj_set_pos(S.ghost, 0, HEADER_H);
    lv_obj_set_style_opa(S.ghost, LV_OPA_COVER, 0);
    show(S.ghost, 1);
    S.tr = tr;

    /* stacking: whichever layer moves over the other goes on top */
    if (tr == TR_FORWARD) { lv_obj_move_background(S.ghost); lv_obj_move_foreground(S.list); }
    else if (tr == TR_NP_IN) { lv_obj_move_background(S.ghost); lv_obj_move_foreground(S.np); }
    else lv_obj_move_foreground(S.ghost);
    lv_obj_move_foreground(S.header);

    anim_transition(NULL, 0);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, S.ghost);
    lv_anim_set_exec_cb(&a, anim_transition);
    lv_anim_set_values(&a, 0, 1024);
    lv_anim_set_duration(&a, (tr == TR_NP_IN || tr == TR_NP_OUT) ? DUR_SHEET : DUR_PUSH);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_completed_cb(&a, transition_done);
    lv_anim_start(&a);
}

/* ---------- rendering ---------- */

/* A Home item's icon in Pixel Pastel: its 10 px drawing at 2x, as an alpha
 * image the row tints like text (image recolor). Built on first use. */
/* Now Playing's radio sign for pixel themes: the icon drawn at a whole
 * number of pixels per cell, so its edges stay sharp (scaling the small one
 * smears them). */
#define PIXEL_BIG_CELL 8
static const lv_image_dsc_t *pixel_icon_big(int k)
{
    enum { PX = PIXEL_ICON_GRID * PIXEL_BIG_CELL };
    static uint8_t px[PX * PX];
    static lv_image_dsc_t dsc;
    static int made = -1;
    if (k < 0 || k >= PIXEL_ICON_N) return NULL;
    if (made != k) {
        for (int y = 0; y < PX; y++)
            for (int x = 0; x < PX; x++)
                px[y * PX + x] = (PIXEL_ICONS[k][y / PIXEL_BIG_CELL] >> (9 - x / PIXEL_BIG_CELL)) & 1 ? 255 : 0;
        dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        dsc.header.cf = LV_COLOR_FORMAT_A8;
        dsc.header.w = PX;
        dsc.header.h = PX;
        dsc.header.stride = PX;
        dsc.data_size = PX * PX;
        dsc.data = px;
        made = k;
        lv_image_cache_drop(&dsc);
    }
    return &dsc;
}

static const lv_image_dsc_t *pixel_icon(int k)
{
    enum { PX = PIXEL_ICON_GRID * PIXEL_ICON_SCALE };
    static uint8_t px[PIXEL_ICON_N][PX * PX];
    static lv_image_dsc_t dsc[PIXEL_ICON_N];
    if (k < 0 || k >= PIXEL_ICON_N) return NULL;
    if (!dsc[k].data) {
        for (int y = 0; y < PX; y++)
            for (int x = 0; x < PX; x++)
                px[k][y * PX + x] = (PIXEL_ICONS[k][y / PIXEL_ICON_SCALE] >> (9 - x / PIXEL_ICON_SCALE)) & 1 ? 255 : 0;
        dsc[k].header.magic = LV_IMAGE_HEADER_MAGIC;
        dsc[k].header.cf = LV_COLOR_FORMAT_A8;
        dsc[k].header.w = PX;
        dsc[k].header.h = PX;
        dsc[k].header.stride = PX;
        dsc[k].data_size = PX * PX;
        dsc[k].data = px[k];
    }
    return &dsc[k];
}

static void utf8_of(uint32_t c, char out[5])
{
    if (c < 0x80) { out[0] = (char)c; out[1] = 0; }
    else if (c < 0x800) { out[0] = (char)(0xc0 | c >> 6); out[1] = (char)(0x80 | (c & 0x3f)); out[2] = 0; }
    else { out[0] = (char)(0xe0 | c >> 12); out[1] = (char)(0x80 | ((c >> 6) & 0x3f));
           out[2] = (char)(0x80 | (c & 0x3f)); out[3] = 0; }
}

/* Lists of the UI's own words (menus, settings), translated; not the music's
 * (albums, artists, songs...), radio stations, devices or networks. */
static int ui_text_list(list_kind_t k)
{
    switch (k) {
    case L_HOME: case L_HOME_EDIT: case L_HISTORY: case L_SETTINGS: case L_QUICK: case L_DISPLAY: case L_SOUND:
    case L_EQ_EDIT: case L_EQ_BAND: case L_SHARE: case L_SSH: case L_USB: case L_PLAYBACK: case L_POWER:
    case L_LIBRARY: case L_ABOUT: case L_SIGNAL: case L_CONTEXT: case L_INFO: case L_USB_ASK:
        return 1;
    default:
        return 0;
    }
}

static void fill_slots(const level_t *l, int n)
{
    for (int j = 0; j < SLOTS; j++) {
        int i = l->top + j - 1;
        int visible = i >= 0 && i < n;
        S.slot_item[j] = visible ? i : -1;
        S.slot_playing[j] = 0;
        show(S.row[j], visible);
        if (!visible) continue;
        char main[256], meta[24];
        int playing;
        list_item(l, i, main, sizeof main, meta, sizeof meta, &playing);
        S.slot_playing[j] = playing;
        if (ui_text_list(l->kind)) {               /* the UI's own words: translated */
            /* (only when there is a translation: copying a string onto itself is
             * undefined, and the H2's C library leaves it empty) */
            const char *tm = tr(main), *tt = l->kind != L_INFO ? tr(meta) : meta;   /* (Info: the song's own) */
            if (tm != main) snprintf(main, sizeof main, "%s", tm);
            if (tt != meta) snprintf(meta, sizeof meta, "%s", tt);
        }

        int x = S.text_x0;
        int eq = playing && T()->playing_equalizer;
        for (int k = 0; k < 3; k++) {
            show(S.row_eq[j][k], eq);
            lv_obj_set_x(S.row_eq[j][k], x + k * 4);
        }
        lv_obj_set_x(S.row_dot[j], x);

        /* Home (and Settings > Home): an icon before each item; on Now playing,
         * while music plays, the equalizer (or dot) above takes its place */
        int hid = l->kind == L_HOME ? S.home[i] : l->kind == L_HOME_EDIT ? S.home_order[i] : -1;
        int pix = hid >= 0 && S.f->family && !strcmp(S.f->family, "arkpixel");
        int glyph = hid >= 0 && !pix && S.f->icon;
        show(S.row_icon[j], glyph && !playing);
        show(S.row_picon[j], pix && !playing);
        if (glyph && !playing) {
            char u[5];
            utf8_of(HOME_ICON[hid], u);
            set_text(S.row_icon[j], u);
            if (lv_obj_get_style_x(S.row_icon[j], 0) != x) lv_obj_set_x(S.row_icon[j], x);
        }
        if (pix && !playing) {
            const lv_image_dsc_t *img = pixel_icon(hid);
            if (lv_image_get_src(S.row_picon[j]) != img) lv_image_set_src(S.row_picon[j], img);
            int px = x + (ICON_W - PIXEL_ICON_GRID * PIXEL_ICON_SCALE) / 2;
            if (lv_obj_get_style_x(S.row_picon[j], 0) != px) lv_obj_set_x(S.row_picon[j], px);
        }
        if (pix || glyph) x += ICON_W + ICON_GAP;
        else if (eq) x += 18;
        else if (playing) x += 14;
        int swatch = l->kind == L_THEMES && i < S.n_themes;   /* the theme's colours, in miniature */
        show(S.row_sw[j], swatch);
        if (swatch) {
            const theme_t *th = &S.themes[i];
            fill(S.row_sw[j], th->bg);
            lv_obj_set_style_border_color(S.row_sw[j], C(T()->text_muted), 0);
            lv_obj_set_style_radius(S.row_sw[j], th->corner == THEME_CORNER_ROUND ? 4 : 0, 0);
            fill(S.row_sw_part[j][0], th->select_bg);
            lv_obj_set_style_radius(S.row_sw_part[j][0], th->corner == THEME_CORNER_ROUND ? 2 : 0, 0);
            fill(S.row_sw_part[j][1], th->text);
            fill(S.row_sw_part[j][2], th->signal);
            if (lv_obj_get_style_x(S.row_sw[j], 0) != x) lv_obj_set_x(S.row_sw[j], x);
            x += SWATCH_W + ICON_GAP;
        }

        int typing = is_typing_row(l, i);
        show(S.row_wheel[j], typing);
        if (typing) {
            char wheel[3][16];
            entry_row(l->kind, main, sizeof main, wheel);
            for (int w = 0; w < 3; w++) {
                set_text(S.row_ch[j][w], wheel[w]);
                /* a letter in a fixed-width cell, so the row doesn't shift as the wheel turns */
                int word = strlen(wheel[1]) > 1;   /* Results, Done, space: alone, small */
                const lv_font_t *f = w == 1 && !word ? S.f->title : S.f->meta;
                if (lv_obj_get_style_text_font(S.row_ch[j][w], 0) != f) lv_obj_set_style_text_font(S.row_ch[j][w], f, 0);
                int cw = word ? (w == 1 ? LV_SIZE_CONTENT : 0) : lv_font_get_line_height(f) * 3 / 4;
                if (lv_obj_get_style_width(S.row_ch[j][w], 0) != cw) lv_obj_set_width(S.row_ch[j][w], cw);
            }
            meta[0] = '\0';
        }
        set_pos_width(S.row_lbl[j], x, 0, UI_W - 2 * ROW_MARGIN - x - (typing ? WHEEL_W + 10 : meta[0] ? 70 : 10));
        set_text(S.row_lbl[j], main);
        set_text(S.row_meta[j], meta);
    }
    S.colored_slot = -1;  /* force recolouring: contents changed */
}

static void render_list(render_mode_t mode)
{
    level_t *l = &S.stack[S.depth - 1];
    int n = list_count(l);
    if (l->sel < l->top) l->top = l->sel;
    if (l->sel >= l->top + ROWS) l->top = l->sel - ROWS + 1;

    char head[96];
    const char *title = ui_text_list(l->kind) || S.depth == 1 ? tr(l->title) : l->title;
    if (S.depth > 1) snprintf(head, sizeof head, "\xe2\x80\xb9 %s", title);  /* "‹ Title" */
    else snprintf(head, sizeof head, "%s", title);
    set_text(S.h_left, head);

    int new_level = S.depth != S.last_depth || l->kind != S.last_kind || l->parent != S.last_parent;
    int top_delta = l->top - S.last_top;
    int target = (l->sel - l->top) * ROW_PITCH;

    /* Once a second a library list that shows the same rows needs none read
     * again: that's SQLite's code and the card, every second. (What a key or a
     * change asks for is always read.) */
    struct fill_sig sig;
    memset(&sig, 0, sizeof sig);
    sig.l = l;                      /* (field by field: memcmp sees the padding too) */
    sig.kind = l->kind;
    sig.parent = l->parent;
    sig.parent2 = l->parent2;
    sig.track = S.now.track.id;
    sig.album = S.now.track.album_id;
    sig.ids = l->items.ids;
    sig.n = n;
    sig.top = l->top;
    sig.sel = l->sel;
    sig.active = S.now.active;
    sig.moving = S.moving;
    sig.theme_gen = S.theme_gen;
    sig.lib_gen = S.lib_gen;
    int same = S.ticking && library_backed(l->kind) && !memcmp(&sig, &S.fill_sig, sizeof sig);
    if (!same) fill_slots(l, n);
    S.fill_sig = sig;
    show(S.pill, n > 0);
    show(S.empty, n == 0);
    if (n == 0) {
        const char *msg = l->kind == L_QUEUE ? "Nothing queued after this track"
                        : l->kind == L_PLAYLISTS ? "No playlists in the music folder"
                        : l->kind == L_PLAYLIST_TRACKS ? "None of these songs are on the card"
                        : l->kind == L_FAVOURITES ? "No favourites yet: Menu on a song"
                        : l->kind == L_RECENT_PLAYED || l->kind == L_MOST_PLAYED ? "Nothing played yet"
                        : l->kind == L_RADIO ? (S.radio_n < 0 ? "No stations: put radio.txt in the card's fathom folder"
                                                              : "No stations in fathom/radio.txt")
                        : library_backed(l->kind) ? "No music found" : "Nothing here";
        set_text(S.empty, tr(msg));
    }

    if (mode == R_CONTENT && !new_level) {
        color_slots(slot_under_pill());
    } else if (mode == R_SNAP || new_level) {
        stop_list_anims();
        S.pill_y = S.pill_target = target;
        S.rows_off = 0;
        lv_obj_set_y(S.pill, target);
        rows_box_place();
        color_slots(l->sel - l->top + 1);
        if (mode == R_ANIMATE && new_level)
            start_transition(S.depth >= S.last_depth ? TR_FORWARD : TR_BACK);
    } else if (top_delta != 0) {
        /* The list scrolled: the pill stays put and the rows slide under it. */
        lv_anim_delete(S.pill, anim_pill);
        S.pill_y = S.pill_target = target;
        lv_obj_set_y(S.pill, target);
        int from = S.rows_off + top_delta * ROW_PITCH;
        if (from > 2 * ROW_PITCH) from = 2 * ROW_PITCH;
        if (from < -2 * ROW_PITCH) from = -2 * ROW_PITCH;
        lv_anim_delete(S.rows_box, anim_rows);
        S.rows_off = from;
        rows_box_place();
        color_slots(slot_under_pill());
        run(S.rows_box, anim_rows, from, 0, DUR_SCROLL);
    } else if (target != S.pill_target) {
        /* Same page: slide the pill from wherever it is now. */
        lv_anim_delete(S.pill, anim_pill);
        S.pill_target = target;
        run(S.pill, anim_pill, S.pill_y, target, DUR_SELECT);
    } else {
        color_slots(slot_under_pill());
    }

    S.last_depth = S.depth;
    S.last_top = l->top;
    S.last_kind = l->kind;
    S.last_parent = l->parent;
}

static void fmt_rate(char *b, size_t n, uint32_t rate)
{
    if (rate % 1000 == 0) snprintf(b, n, "%u", rate / 1000);
    else snprintf(b, n, "%u.%u", rate / 1000, (rate % 1000) / 100);
}

/* Cover art for the current track, from the library's RGB565 cache. */
static void load_art(void)
{
    S.np_seq = S.player ? player_seq(S.player) : 0;
    if (S.now.radio) return;           /* the radio's cover comes by ui_set_radio_art */
    S.radio_art = 0;
    int ok = S.lib && S.now.active &&
             library_art(S.lib, S.now.track.album_id, LIB_ART_LARGE, S.art_px) == 0;
    if (ok) {
        lv_image_cache_drop(&S.art_dsc);   /* same descriptor, new pixels */
        lv_image_set_src(S.np_img, &S.art_dsc);
        lv_obj_invalidate(S.np_img);
    }
    show(S.np_img, ok);
}

/* Now Playing for a radio station: what it says is playing, the station, the
 * stream's format; time listened and "Live" under the bar, which stays empty. */
static void render_np_radio(void)
{
    const lib_track_t *t = &S.now.track;
    int art = S.radio_art;
    int pix = !art && S.f->family && !strcmp(S.f->family, "arkpixel");
    if (art && lv_image_get_src(S.np_img) != &S.art_dsc) lv_image_set_src(S.np_img, &S.art_dsc);
    show(S.np_img, art);
    show(S.np_radio_px, pix);
    show(S.np_radio, !art && !pix && S.f->icon);
    if (pix && lv_image_get_src(S.np_radio_px) != pixel_icon_big(H_RADIO)) {
        lv_image_set_src(S.np_radio_px, pixel_icon_big(H_RADIO));
        lv_obj_center(S.np_radio_px);
    }
    if (!art && !pix && S.f->icon) {
        char g[5];
        utf8_of(HOME_ICON[H_RADIO], g);
        set_text(S.np_radio, g);
        if (lv_obj_get_style_align(S.np_radio, 0) != LV_ALIGN_CENTER) lv_obj_center(S.np_radio);   /* (once: it redraws) */
    }
    set_text(S.h_left, tr("Radio"));
    set_text(S.np_title, t->title);
    set_text(S.np_artist, t->artist);
    set_text(S.np_album, t->album);
    char b[32], r[16];
    if (t->sample_rate && t->codec[0]) {
        fmt_rate(r, sizeof r, t->sample_rate);
        if (t->bits) snprintf(b, sizeof b, "%s %u/%s", t->codec, t->bits, r);
        else snprintf(b, sizeof b, "%s %s", t->codec, r);
        if (S.eq_name[0] && !S.pure) snprintf(b + strlen(b), sizeof b - strlen(b), " \xC2\xB7 EQ");
        set_text(S.np_chip, b);
    }
    show(S.np_chip, t->sample_rate && t->codec[0] && S.now.playing && !S.now.connecting);
    const char *state = tr(S.now.radio_failed ? "Can't reach the station"
                         : !S.now.playing ? "Stopped"
                         : S.now.connecting ? "Connecting\xE2\x80\xA6" : "Live");
    if (S.now.playing && !S.now.connecting) fmt_time(b, sizeof b, (int)(S.now.pos_ms / 1000), 0);
    else b[0] = '\0';
    set_text(S.np_t0, b);
    set_text(S.np_t1, state);
    set_text_color(S.np_t0, C(T()->text_muted));
    set_text_color(S.np_t1, C(S.now.playing && !S.now.connecting ? T()->signal : T()->text_muted));
    show(S.np_marker, 0);
    show(S.np_pause, 0);
    set_text(S.np_out, tr(S.output));
    set_text_color(S.np_out, C(T()->info));
    char nx[300] = "";
    if (S.now.count > 1 && S.radio_n == S.now.count)
        snprintf(nx, sizeof nx, "%s %s", tr("Next:"), S.radio[(S.now.index + 1) % S.now.count].name);
    set_text(S.np_next, nx);
    lv_obj_set_width(S.np_fill, 0);
    for (int i = 0; i < SEGMENTS; i++) fill(S.np_seg[i], T()->surface);
}

static void render_np(void)
{
    /* (the header's title is set once per render: setting it to one text and
     * then another redraws it, every second) */
    if (!S.now.active || S.now.radio || !(S.now.count > 1 && S.now.index >= 0 && S.now.index < S.now.count))
        set_text(S.h_left, tr(S.now.active && S.now.radio ? "Radio" : "Now playing"));
    if (!S.now.active) {
        set_text(S.np_title, tr("Nothing playing"));
        set_text(S.np_artist, tr("Choose an album, or Shuffle all songs"));
        set_text(S.np_album, "");
        show(S.np_chip, 0);
        show(S.np_img, 0);
        show(S.np_radio, 0);
        show(S.np_radio_px, 0);
        set_text(S.np_t0, "");
        set_text(S.np_t1, "");
        set_text(S.np_next, "");
        show(S.np_pause, 0);
        lv_obj_set_width(S.np_fill, 0);
        for (int i = 0; i < SEGMENTS; i++) fill(S.np_seg[i], T()->surface);
        return;
    }
    if (S.player && player_seq(S.player) != S.np_seq) load_art();
    if (S.now.radio) { render_np_radio(); return; }
    if (S.now.count > 1 && S.now.index >= 0 && S.now.index < S.now.count) {   /* where in the queue */
        char h[48];
        snprintf(h, sizeof h, "%s \xC2\xB7 %d/%d", tr("Now playing"), S.now.index + 1, S.now.count);
        lv_point_t sz;     /* too wide beside the status icons (a big queue, a wide font): the numbers alone */
        lv_text_get_size(&sz, h, S.f->head, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        if (sz.x > UI_W / 2 + 30) snprintf(h, sizeof h, "%d/%d", S.now.index + 1, S.now.count);
        set_text(S.h_left, h);
    }
    show(S.np_radio, 0);
    show(S.np_radio_px, 0);
    const lib_track_t *t = &S.now.track;
    set_text(S.np_title, t->title);
    set_text(S.np_artist, t->artist);
    set_text(S.np_album, t->album);

    char b[32], r[16];
    fmt_rate(r, sizeof r, t->sample_rate);
    if (t->bits == 1) snprintf(b, sizeof b, "%s", t->codec);          /* DSD: "DSD64" says it all */
    else if (t->bits) snprintf(b, sizeof b, "%s %u/%s", t->codec, t->bits, r);
    else snprintf(b, sizeof b, "%s %s", t->codec, r);
    if (S.eq_name[0] && !S.pure) snprintf(b + strlen(b), sizeof b - strlen(b), " \xC2\xB7 EQ");   /* not bit-perfect */
    set_text(S.np_chip, b);
    show(S.np_chip, 1);

    int pos = (int)(S.now.pos_ms / 1000), len = (int)(S.now.len_ms / 1000);
    if (pos > len) pos = len;
    int shown = S.scrubbing ? S.scrub_s : pos;     /* scrubbing: the times are the marker's */
    fmt_time(b, sizeof b, shown, 0);
    set_text(S.np_t0, b);
    fmt_time(b, sizeof b, len - shown, 1);
    set_text(S.np_t1, b);
    set_text_color(S.np_t1, C(T()->text_muted));
    set_text_color(S.np_t0, C(S.scrubbing ? T()->signal : T()->text_muted));
    show(S.np_marker, S.scrubbing && len > 0);
    if (S.scrubbing && len > 0) {
        fill(S.np_marker, T()->text);
        lv_obj_set_x(S.np_marker, 12 + (UI_W - 24) * S.scrub_s / len - 1);
    }

    show(S.np_pause, !S.now.playing && !S.now.stopped);
    /* Pure mode, and nothing changes the samples: "Bit+" after the output, in the accent colour */
    if (S.bitperfect) {
        char o[112];
        snprintf(o, sizeof o, "%s%s%s", tr(S.output), S.output[0] ? " \xC2\xB7 " : "", tr("Bit+"));
        set_text(S.np_out, o);
    } else {
        set_text(S.np_out, tr(S.output));
    }
    set_text_color(S.np_out, C(S.bitperfect ? T()->signal : T()->info));
    char nx[300] = "";
    if (S.now.stop_after) snprintf(nx, sizeof nx, "%s", tr("Stops after this track"));
    else if (S.player && player_queue_len(S.player) > 0 && S.lib) {
        char meta[32];
        int64_t id = player_queue_at(S.player, 0);   /* read once per track, not every second */
        if (id != S.next_id || S.next_gen != S.lib_gen) {
            S.next_id = id;
            S.next_gen = S.lib_gen;
            if (library_row(S.lib, LIB_PLAYLIST_TRACKS, id, S.next_text, sizeof S.next_text, meta, sizeof meta))
                S.next_text[0] = '\0';
        }
        if (S.next_text[0]) snprintf(nx, sizeof nx, "%s %s", tr("Next:"), S.next_text);
    }
    set_text(S.np_next, nx);

    int w = UI_W - 24;
    lv_obj_set_width(S.np_fill, len ? w * pos / len : 0);
    int lit = len ? SEGMENTS * pos / len : 0;
    for (int i = 0; i < SEGMENTS; i++) fill(S.np_seg[i], i < lit ? T()->signal : T()->surface);
}

#include "bt_rune.h"
#include "wifi_icon.h"
/* The header's Bluetooth icon: the plain rune (tools/icons/bt_rune.py), tinted
 * like the symbols; Montserrat's own is the rune cut out of a filled box. */
static const lv_image_dsc_t BT_RUNE_DSC = {
    .header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_A8, .w = BT_RUNE_W, .h = BT_RUNE_H,
               .stride = BT_RUNE_W},
    .data_size = sizeof BT_RUNE,
    .data = BT_RUNE,
};
/* ...and Wi-Fi's: a dot and three arcs (tools/icons/wifi_icon.py) */
static const lv_image_dsc_t WIFI_ICON_DSC = {
    .header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_A8, .w = WIFI_ICON_W, .h = WIFI_ICON_H,
               .stride = WIFI_ICON_W},
    .data_size = sizeof WIFI_ICON,
    .data = WIFI_ICON,
};

/* While charging (below 100 %, with the screen lit), the charging icon pulses. */
static void anim_opa(void *o, int32_t v) { lv_obj_set_style_opa(o, (lv_opa_t)v, 0); }

static void charge_pulse(lv_obj_t *o)
{
    if (o == S.h_pulse) return;
    if (S.h_pulse) {
        lv_anim_delete(S.h_pulse, anim_opa);
        lv_obj_set_style_opa(S.h_pulse, LV_OPA_COVER, 0);
    }
    S.h_pulse = o;
    if (!o) return;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, o);
    lv_anim_set_exec_cb(&a, anim_opa);
    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_30);
    lv_anim_set_duration(&a, 1100);
    lv_anim_set_playback_duration(&a, 1100);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
}

/* Right side of the header: [play/pause/stop] [shuffle] [repeat] [Bluetooth] [charging]
 * battery %. The Bluetooth icon is dimmed while it connects. */
typedef struct { int icon; const char *sym; const lv_image_dsc_t *own; int dim; } hdr_want_t;
typedef struct {
    hdr_want_t want[HDR_SLOTS];
    int n, pct, charging, asleep, theme_gen;
} hdr_state_t;

static void render_header_status(void)
{
    hdr_want_t want[HDR_SLOTS];
    memset(want, 0, sizeof want);
    int n = 0;
    /* always one of: playing, paused, stopped (nothing loaded or the queue ended) */
    if (S.now.playing) { want[n].icon = THEME_ICON_PLAY; want[n++].sym = LV_SYMBOL_PLAY; }
    else if (S.now.active && !S.now.stopped) { want[n].icon = THEME_ICON_PAUSE; want[n++].sym = LV_SYMBOL_PAUSE; }
    else { want[n].icon = THEME_ICON_STOP; want[n++].sym = LV_SYMBOL_STOP; }
    if (S.now.shuffle) { want[n].icon = THEME_ICON_SHUFFLE; want[n++].sym = LV_SYMBOL_SHUFFLE; }
    if (S.now.repeat == REPEAT_ALL) { want[n].icon = THEME_ICON_REPEAT; want[n++].sym = LV_SYMBOL_LOOP; }
    else if (S.now.repeat == REPEAT_ONE) { want[n].icon = THEME_ICON_REPEAT_ONE; want[n++].sym = LV_SYMBOL_LOOP "1"; }
    if (S.bt_link) {
        want[n].icon = THEME_ICON_BLUETOOTH;
        want[n].dim = S.bt_link == UI_BT_LINK_CONNECTING;
        want[n].own = &BT_RUNE_DSC;
        want[n++].sym = LV_SYMBOL_BLUETOOTH;
    }
    if (S.wifi_link) {
        want[n].icon = THEME_ICON_WIFI;
        want[n].dim = S.wifi_link == 1;
        want[n].own = &WIFI_ICON_DSC;
        want[n++].sym = LV_SYMBOL_WIFI;
    }
    if (S.charging) { want[n].icon = THEME_ICON_CHARGE; want[n++].sym = LV_SYMBOL_CHARGE; }

    /* As it was: nothing to do (every render comes here, once a second at
     * least; laying the header out again redraws it). A theme counts as a change. */
    static hdr_state_t last;
    static int drawn;
    hdr_state_t now;
    memset(&now, 0, sizeof now);
    memcpy(now.want, want, sizeof want);
    now.n = n;
    now.pct = S.battery_pct;
    now.charging = S.charging;
    now.asleep = S.asleep;
    now.theme_gen = S.theme_gen;
    if (drawn && !memcmp(&now, &last, sizeof now)) return;
    last = now;
    drawn = 1;

    for (int i = 0; i < HDR_SLOTS; i++) {
        const lv_image_dsc_t *img = i < n && S.hooks.icon ? S.hooks.icon(T(), want[i].icon) : NULL;
        int tint = 0;                          /* our own A8 icon, in the symbols' colour */
        if (!img && i < n && want[i].own) { img = want[i].own; tint = 1; }
        if (img) lv_image_set_src(S.h_slot_img[i], img);
        show(S.h_slot_img[i], i < n && img);
        int dim = i < n && want[i].dim;
        lv_obj_set_style_image_recolor_opa(S.h_slot_img[i], tint ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_image_recolor(S.h_slot_img[i], C(dim ? T()->text_muted : T()->mode), 0);
        lv_obj_set_style_image_opa(S.h_slot_img[i], dim && !tint ? LV_OPA_40 : LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(S.h_slot_sym[i], C(dim ? T()->text_muted : T()->mode), 0);
        if (i < n && !img) lv_label_set_text(S.h_slot_sym[i], want[i].sym);
        show(S.h_slot_sym[i], i < n && !img);
    }
    lv_obj_t *pulse = NULL;                /* the charging icon is the last one */
    if (S.charging && S.battery_pct >= 0 && S.battery_pct < 100 && !S.asleep && n > 0)
        pulse = lv_obj_has_flag(S.h_slot_img[n - 1], LV_OBJ_FLAG_HIDDEN) ? S.h_slot_sym[n - 1] : S.h_slot_img[n - 1];
    charge_pulse(pulse);
    if (S.battery_pct >= 0) lv_label_set_text_fmt(S.h_right, "%d%%", S.battery_pct);
    else lv_label_set_text(S.h_right, "");
    lv_obj_update_layout(S.header);
    lv_obj_align_to(S.h_mode, S.h_right, LV_ALIGN_OUT_LEFT_MID, S.battery_pct >= 0 ? -6 : 0, 0);
    /* the title ends before the icons, so a long one scrolls until its end is clear of them */
    lv_obj_update_layout(S.header);
    int title_end = lv_obj_get_x(S.h_right);
    if (n) {   /* the first icon drawn */
        lv_obj_t *o = lv_obj_has_flag(S.h_slot_img[0], LV_OBJ_FLAG_HIDDEN) ? S.h_slot_sym[0] : S.h_slot_img[0];
        title_end = lv_obj_get_x(S.h_mode) + lv_obj_get_x(o);
    }
    set_pos_width(S.h_left, lv_obj_get_style_x(S.h_left, 0), lv_obj_get_style_y(S.h_left, 0),
                  title_end - 8 - lv_obj_get_x(S.h_left));
}

/* ---------- toast: a short message over everything (volume, etc.) ---------- */

static void toast_hide(lv_timer_t *t)
{
    (void)t;
    if (S.toast) lv_obj_add_flag(S.toast, LV_OBJ_FLAG_HIDDEN);
    S.toast_timer = NULL;
}

lv_obj_t *ui_message_screen(const char *title, const char *body)
{
    const theme_t *t = T();
    lv_obj_t *o = box(lv_layer_top(), 0, 0, UI_W, UI_H);
    fill(o, t->bg);
    lv_obj_t *a = lv_label_create(o);
    style_label(a, S.f->title, t->text);
    lv_label_set_text(a, tr(title));
    lv_obj_align(a, LV_ALIGN_TOP_MID, 0, 62);
    lv_obj_t *b = lv_label_create(o);
    style_label(b, S.f->body, t->text_muted);
    lv_obj_set_width(b, UI_W - 40);
    lv_obj_set_style_text_align(b, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(b, LV_LABEL_LONG_WRAP);
    lv_label_set_text(b, tr(body));
    lv_obj_align(b, LV_ALIGN_TOP_MID, 0, 112);
    return o;
}

lv_obj_t *ui_dac_screen(void)
{
    const theme_t *t = T();
    lv_obj_t *o = ui_message_screen("USB DAC", "");          /* children 0 title, 1 hint */
    lv_obj_t *f = lv_label_create(o);                         /* 2: format */
    style_label(f, S.f->title, t->dark ? 0xCBB8FF : 0x5B3FB0);   /* lavender */
    lv_obj_align(f, LV_ALIGN_TOP_MID, 0, 108);
    lv_obj_t *n = lv_label_create(o);                         /* 3: note */
    style_label(n, S.f->body, t->text_muted);
    lv_obj_align(n, LV_ALIGN_TOP_MID, 0, 140);
    return o;
}

void ui_dac_screen_set(lv_obj_t *scr, const char *format, const char *note, int exact, const char *hint)
{
    const theme_t *t = T();
    lv_obj_t *h = lv_obj_get_child(scr, 1), *f = lv_obj_get_child(scr, 2), *n = lv_obj_get_child(scr, 3);
    lv_label_set_text(f, format);
    lv_label_set_text(n, note);
    lv_obj_set_style_text_color(n, C(exact ? (t->dark ? 0xA8F0D1 : 0x1E7A55) : t->text_muted), 0);   /* mint */
    show(f, format[0] != '\0');
    show(n, note[0] != '\0');
    lv_label_set_text(h, hint);
    lv_obj_align(h, LV_ALIGN_TOP_MID, 0, format[0] ? 186 : 112);
}

static void render(render_mode_t mode);

void ui_set_radio_art(const uint16_t *px)
{
    if (px) {
        memcpy(S.art_px, px, sizeof S.art_px);
        lv_image_cache_drop(&S.art_dsc);   /* same descriptor, new pixels */
        lv_obj_invalidate(S.np_img);
    }
    S.radio_art = px != NULL;
    if (S.now_playing_view && S.now.radio) render(R_CONTENT);
}

void ui_toast(const char *text, int ms)
{
    text = tr(text);
    if (!S.toast) {
        S.toast = box(lv_layer_top(), 0, 0, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_hor(S.toast, 14, 0);
        lv_obj_set_style_pad_ver(S.toast, 8, 0);
        S.toast_lbl = lv_label_create(S.toast);
    }
    const theme_t *t = T();
    lv_obj_set_style_bg_color(S.toast, C(t->surface), 0);
    lv_obj_set_style_bg_opa(S.toast, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(S.toast, t->radius, 0);
    lv_obj_set_style_border_color(S.toast, C(t->signal), 0);
    lv_obj_set_style_border_width(S.toast, 2, 0);
    lv_obj_set_style_text_font(S.toast_lbl, S.f->body, 0);
    lv_obj_set_style_text_color(S.toast_lbl, C(t->text), 0);
    /* One line when it fits the screen (with its margins), else wrapped and
     * centred on as many lines as it takes. */
    int32_t maxw = lv_obj_get_width(lv_layer_top()) - 2 * 12 - 2 * 14;
    lv_point_t sz;
    lv_text_get_size(&sz, text, S.f->body, lv_obj_get_style_text_letter_space(S.toast_lbl, 0), 0, LV_COORD_MAX,
                     LV_TEXT_FLAG_NONE);
    lv_label_set_long_mode(S.toast_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(S.toast_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(S.toast_lbl, sz.x > maxw ? maxw : LV_SIZE_CONTENT);
    lv_label_set_text(S.toast_lbl, text);
    lv_obj_remove_flag(S.toast, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(S.toast, LV_ALIGN_BOTTOM_MID, 0, -16);
    if (S.toast_timer) lv_timer_reset(S.toast_timer);
    else {
        S.toast_timer = lv_timer_create(toast_hide, (uint32_t)ms, NULL);
        lv_timer_set_repeat_count(S.toast_timer, 1);
    }
    if (S.toast_timer) lv_timer_set_period(S.toast_timer, (uint32_t)ms);
}

/* ---------- level bar: a labelled bar over everything (brightness) ---------- */

static void level_hide(lv_timer_t *t)
{
    (void)t;
    if (S.lvl) lv_obj_add_flag(S.lvl, LV_OBJ_FLAG_HIDDEN);
    S.lvl_timer = NULL;
}

void ui_level(const char *text, int value, int max, const char *value_text)
{
    const theme_t *t = T();
    if (!S.lvl) {
        S.lvl = box(lv_layer_top(), 0, 0, 220, 64);
        S.lvl_lbl = lv_label_create(S.lvl);
        S.lvl_bar = lv_bar_create(S.lvl);
        lv_obj_set_size(S.lvl_bar, 188, 12);
        lv_obj_align(S.lvl_lbl, LV_ALIGN_TOP_LEFT, 16, 8);
        lv_obj_align(S.lvl_bar, LV_ALIGN_BOTTOM_LEFT, 16, -12);
    }
    lv_obj_set_style_bg_color(S.lvl, C(t->surface), 0);
    lv_obj_set_style_bg_opa(S.lvl, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(S.lvl, t->radius, 0);
    lv_obj_set_style_border_color(S.lvl, C(t->signal), 0);
    lv_obj_set_style_border_width(S.lvl, 2, 0);
    lv_obj_set_style_text_font(S.lvl_lbl, S.f->meta, 0);
    lv_obj_set_style_text_color(S.lvl_lbl, C(t->text), 0);
    int r = t->corner == THEME_CORNER_ROUND ? 6 : 0;
    lv_obj_set_style_radius(S.lvl_bar, r, LV_PART_MAIN);
    lv_obj_set_style_radius(S.lvl_bar, r, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(S.lvl_bar, C(t->bg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(S.lvl_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(S.lvl_bar, C(t->signal), LV_PART_INDICATOR);
    lv_bar_set_range(S.lvl_bar, 0, max);
    lv_bar_set_value(S.lvl_bar, value, LV_ANIM_OFF);
    if (value_text) lv_label_set_text_fmt(S.lvl_lbl, "%s  %s", text, value_text);
    else lv_label_set_text_fmt(S.lvl_lbl, "%s  %d / %d", text, value, max);
    lv_obj_remove_flag(S.lvl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(S.lvl, LV_ALIGN_BOTTOM_MID, 0, -16);
    if (S.toast) lv_obj_add_flag(S.toast, LV_OBJ_FLAG_HIDDEN);   /* one overlay at a time */
    if (S.lvl_timer) lv_timer_reset(S.lvl_timer);
    else {
        S.lvl_timer = lv_timer_create(level_hide, 1200, NULL);
        lv_timer_set_repeat_count(S.lvl_timer, 1);
    }
}

void ui_set_bt_link(int state)
{
    if (state == S.bt_link) return;
    S.bt_link = state;
    if (S.h_right) render_header_status();
}

void ui_set_battery(int percent, int charging)
{
    if (percent == S.battery_pct && !!charging == S.charging) return;
    S.battery_pct = percent > 100 ? 100 : percent;
    S.charging = !!charging;
    if (S.h_right) render_header_status();
}

static void render(render_mode_t mode)
{
    int np_changed = S.now_playing_view != S.last_np;
    if (S.player) player_now(S.player, &S.now);
    if (S.asleep) {                 /* drawn when the screen comes on (no animations meanwhile) */
        if (mode != R_CONTENT) S.wake_mode = R_SNAP;
        pending_free();
        return;
    }
    show(S.list, !S.now_playing_view);
    show(S.np, S.now_playing_view);
    render_header_status();
    if (S.now_playing_view) {
        render_np();
        if (np_changed && mode == R_ANIMATE) start_transition(TR_NP_IN);
    } else if (np_changed && mode == R_ANIMATE) {
        render_list(R_SNAP);
        start_transition(TR_NP_OUT);
    } else {
        render_list(mode);
    }
    S.last_np = S.now_playing_view;
    pending_free();   /* the key did not change screens: drop the unused snapshot */
}

/* ---------- navigation ---------- */

static void push(list_kind_t kind, const char *title, int64_t parent)
{
    if (S.depth >= (int)(sizeof S.stack / sizeof S.stack[0])) return;
    level_t *l = &S.stack[S.depth++];
    memset(l, 0, sizeof *l);
    l->kind = kind;
    snprintf(l->title, sizeof l->title, "%s", title);
    l->parent = parent;
    if (kind == L_CREDIT_TRACKS || kind == L_COMPOSER_TRACKS) return;   /* push_*_tracks() fills it */
    if (library_backed(kind) && S.lib) library_list(S.lib, view_of(kind), parent, &l->items);
    if (kind == L_THEMES) l->sel = S.cur_theme;
    if (kind == L_EQ) {                    /* the profiles on the card, now; the current one selected */
        S.eq_n = S.hooks.eq_list ? S.hooks.eq_list(S.eq_names, 64) : 0;
        for (int i = 0; i < S.eq_n; i++)
            if (!strcmp(S.eq_names[i], S.eq_name)) l->sel = i + 1;
        ui_toast(S.eq_n ? "Menu on an EQ: edit it" : "AutoEQ files go in fathom/eq. Or: New EQ", 2500);
    }
    if (kind == L_LIBRARY && S.lib) {
        static const lib_view_t v[4] = {LIB_SONGS, LIB_ALBUMS, LIB_ARTISTS, LIB_PLAYLISTS};
        for (int i = 0; i < 4; i++) {
            lib_list_t tmp;
            S.lib_counts[i] = library_list(S.lib, v[i], 0, &tmp);
            library_list_free(&tmp);
        }
    }
}

/* An artist's songs on one album (Artists > artist > album). */
static void push_credit_tracks(const char *title, int64_t artist, int64_t album)
{
    push(L_CREDIT_TRACKS, title, artist);
    level_t *l = &S.stack[S.depth - 1];
    if (l->kind != L_CREDIT_TRACKS) return;
    l->parent2 = album;
    if (S.lib) library_list_credit_tracks(S.lib, artist, album, &l->items);
}

static void push_composer_tracks(const char *title, int64_t composer, int64_t album)
{
    push(L_COMPOSER_TRACKS, title, composer);
    level_t *l = &S.stack[S.depth - 1];
    if (l->kind != L_COMPOSER_TRACKS) return;
    l->parent2 = album;
    if (S.lib) library_list_composer_tracks(S.lib, composer, album, &l->items);
}

/* Open an artist found in Search: their Artists page if they're credited on
 * songs, else (an album artist only, like "Various Artists") their albums. */
static void push_artist(int64_t id, const char *name)
{
    int nalbums = 0, ncredited = 0;
    if (S.lib) library_artist_counts(S.lib, id, &nalbums, &ncredited);
    push(ncredited > 0 ? L_CREDIT_ALBUMS : L_ARTIST_ALBUMS, name, id);
}

/* Close the top level. If it was opened from Now Playing, go back there
 * (unless `stay` asks to remain in the lists, e.g. after "Go to album"). */
static void pop_level(int stay)
{
    if (S.depth <= 1) return;
    level_t *l = &S.stack[S.depth - 1];
    int to_np = l->back_to_np && !stay;
    if (S.editing && (l->kind == L_EQ_EDIT || l->kind == L_EQ_BAND)) eqe_changed(1);   /* mid-edit: keep it */
    if (l->kind == L_EQ_BAND) qsort(S.eqe.band, (size_t)S.eqe.n, sizeof S.eqe.band[0], band_by_freq);
    if (l->kind == L_EQ_EDIT && S.hooks.eq_preview) S.hooks.eq_preview(NULL);         /* the output's own choice */
    library_list_free(&l->items);
    S.depth--;
    S.editing = 0;
    S.moving = 0;
    if (to_np) S.now_playing_view = 1;
}

static void pop(void) { pop_level(0); }

/* Open an overlay-like level (quick settings, context menu) from wherever we are. */
static void push_from_here(list_kind_t kind, const char *title, int64_t parent)
{
    int from_np = S.now_playing_view;
    S.now_playing_view = 0;
    push(kind, title, parent);
    S.stack[S.depth - 1].back_to_np = from_np;
}

void ui_usb_prompt(void)
{
    if (S.stack[S.depth - 1].kind == L_USB_ASK) return;
    if (S.stack[S.depth - 1].kind == L_CONTEXT) pop_level(1);
    push_from_here(L_USB_ASK, "Connected to a computer", 0);
    render(R_ANIMATE);
}

void ui_usb_prompt_close(void)
{
    if (S.stack[S.depth - 1].kind != L_USB_ASK) return;
    pop();
    render(R_ANIMATE);
}

static void open_quick(void)
{
    if (S.stack[S.depth - 1].kind == L_QUICK) return;
    if (S.stack[S.depth - 1].kind == L_CONTEXT) pop_level(1);
    push_from_here(L_QUICK, "Quick settings", 0);
}

/* The tracks a context menu acts on (one track, an album, a playlist). */
static int subject_tracks(lib_list_t *out)
{
    memset(out, 0, sizeof *out);
    if (S.ctx_playlist) return library_list(S.lib, LIB_PLAYLIST_TRACKS, S.ctx_playlist, out);
    if (S.ctx_track) {
        out->ids = malloc(sizeof *out->ids);
        if (!out->ids) return -1;
        out->ids[0] = S.ctx_track;
        out->n = 1;
        return 1;
    }
    if (S.ctx_album) return library_list(S.lib, LIB_ALBUM_TRACKS, S.ctx_album, out);
    return -1;
}

#define CTX_ADD(a) (S.ctx[S.ctx_n++] = (a))

/* Context menu for the playing track (Now Playing), a queue row, or the selected row. */
static int open_context(void)
{
    level_t *l = &S.stack[S.depth - 1];
    S.ctx_n = 0;
    S.ctx_track = S.ctx_album = S.ctx_artist = S.ctx_playlist = 0;
    S.ctx_qidx = -1;
    S.ctx_plpos = -1;
    char title[128] = "";
    int on_np = S.now_playing_view;
    if (l->kind == L_BLUETOOTH && !on_np) {   /* Settings > Bluetooth: forget a paired device */
        int d = l->sel - 2;
        if (d < 0 || d >= S.bt_n || !S.bt[d].paired) return -1;
        snprintf(S.ctx_bt, sizeof S.ctx_bt, "%s", S.bt[d].addr);
        CTX_ADD(CTX_BT_FORGET);
        push_from_here(L_CONTEXT, S.bt[d].name[0] ? S.bt[d].name : S.bt[d].addr, 0);
        return 0;
    }
    if (l->kind == L_WIFI && !on_np) {        /* Settings > Wi-Fi: forget a saved network */
        int w = l->sel - 3;
        if (w < 0 || w >= S.wifi_n || !S.wifi[w].saved) return -1;
        snprintf(S.ctx_wifi, sizeof S.ctx_wifi, "%s", S.wifi[w].ssid);
        CTX_ADD(CTX_WIFI_FORGET);
        push_from_here(L_CONTEXT, S.wifi[w].ssid, 0);
        return 0;
    }
    if (l->kind == L_EQ && !on_np) {          /* Settings > Sound > EQ: edit one */
        if (l->sel < 1 || l->sel - 1 >= S.eq_n) return -1;
        eqe_open(S.eq_names[l->sel - 1]);
        return 0;
    }
    if (l->kind == L_HOME_EDIT && !on_np) {   /* Settings > Home: move an item */
        CTX_ADD(CTX_HOME_MOVE);
        push_from_here(L_CONTEXT, HOME[S.home_order[l->sel]], 0);
        return 0;
    }
    if (!S.lib) return -1;
    if (on_np) {
        if (!S.now.active) return -1;
        S.ctx_track = S.now.track.id;
    } else if (l->kind == L_QUEUE && l->sel < list_count(l)) {
        S.ctx_qidx = l->sel;
        S.ctx_track = player_queue_at(S.player, l->sel);
    } else if (is_track_list(l->kind) && l->sel < l->items.n) {
        S.ctx_track = l->items.ids[l->sel];
        if (l->kind == L_PLAYLIST_TRACKS && library_playlist_writable(S.lib, l->parent)) S.ctx_plpos = l->sel;
    } else if (is_album_list(l->kind) && l->sel < l->items.n) {
        S.ctx_album = l->items.ids[l->sel];
    } else if (l->kind == L_PLAYLISTS && l->sel < l->items.n) {
        S.ctx_playlist = l->items.ids[l->sel];
    } else {
        return -1;
    }

    if (S.ctx_playlist) {
        library_playlist_name(S.lib, S.ctx_playlist, title, sizeof title);
        CTX_ADD(CTX_PLAY); CTX_ADD(CTX_PLAY_NEXT); CTX_ADD(CTX_ADD_QUEUE); CTX_ADD(CTX_ADD_PLAYLIST);
        if (library_playlist_writable(S.lib, S.ctx_playlist)) { CTX_ADD(CTX_PL_RENAME); CTX_ADD(CTX_PL_DELETE); }
    } else if (S.ctx_track) {
        lib_track_t t;
        if (library_track(S.lib, S.ctx_track, &t)) return -1;
        S.ctx_album = t.album_id;
        S.ctx_artist = t.artist_id;
        snprintf(title, sizeof title, "%.127s", t.title);
        if (S.ctx_qidx >= 0) {
            CTX_ADD(CTX_Q_PLAY); CTX_ADD(CTX_Q_MOVE); CTX_ADD(CTX_Q_REMOVE); CTX_ADD(CTX_Q_CLEAR);
        } else if (on_np) {
            CTX_ADD(CTX_SHOW_QUEUE);
        } else {
            CTX_ADD(CTX_PLAY_NEXT); CTX_ADD(CTX_ADD_QUEUE);
        }
        if (S.ctx_plpos >= 0) { CTX_ADD(CTX_PL_REMOVE); CTX_ADD(CTX_PL_MOVE); }
        else CTX_ADD(CTX_ADD_PLAYLIST);
        if (S.ctx_album && (on_np || !(l->kind == L_TRACKS && l->parent == S.ctx_album)))
            CTX_ADD(CTX_GO_ALBUM);
        if (S.ctx_artist) CTX_ADD(CTX_GO_ARTIST);
        CTX_ADD(library_favourite(S.lib, S.ctx_track) ? CTX_FAV_REMOVE : CTX_FAV_ADD);
        CTX_ADD(CTX_INFO);
    } else {
        S.ctx_artist = library_album_artist(S.lib, S.ctx_album);
        library_album_name(S.lib, S.ctx_album, title, sizeof title);
        CTX_ADD(CTX_PLAY); CTX_ADD(CTX_PLAY_NEXT); CTX_ADD(CTX_ADD_QUEUE); CTX_ADD(CTX_ADD_PLAYLIST);
        if (S.ctx_artist && !(l->kind == L_ARTIST_ALBUMS && l->parent == S.ctx_artist)) CTX_ADD(CTX_GO_ARTIST);
    }
    push_from_here(L_CONTEXT, title, 0);
    return 0;
}

static void run_context(ctx_action_t a)
{
    char name[128], msg[160];
    int64_t track = S.ctx_track, album = S.ctx_album, artist = S.ctx_artist;
    int qidx = S.ctx_qidx;
    int from_np = S.stack[S.depth - 1].back_to_np;
    lib_list_t tr;
    switch (a) {
    case CTX_PLAY:
    case CTX_PLAY_NEXT:
    case CTX_ADD_QUEUE: {
        int n = subject_tracks(&tr);
        pop_level(a == CTX_PLAY);
        if (n > 0 && S.player) {
            if (a == CTX_PLAY) { player_play(S.player, tr.ids, tr.n, 0); S.now_playing_view = 1; }
            else {
                if (a == CTX_PLAY_NEXT) player_play_next(S.player, tr.ids, tr.n);
                else player_add_to_queue(S.player, tr.ids, tr.n);
                if (n == 1) snprintf(msg, sizeof msg, a == CTX_PLAY_NEXT ? "Playing next" : "Added to queue");
                else snprintf(msg, sizeof msg, a == CTX_PLAY_NEXT ? "%d songs play next" : "%d songs added to queue", n);
                ui_toast(msg, 1500);
            }
        }
        library_list_free(&tr);
        break;
    }
    case CTX_SHOW_QUEUE:
        pop_level(1);
        push(L_QUEUE, "Queue", 0);
        S.stack[S.depth - 1].back_to_np = from_np;
        break;
    case CTX_GO_ALBUM:
        pop_level(1);
        if (library_album_name(S.lib, album, name, sizeof name) == 0) {
            push(L_TRACKS, name, album);
            level_t *l = &S.stack[S.depth - 1];
            for (int i = 0; i < l->items.n; i++)
                if (l->items.ids[i] == track) l->sel = i;
        }
        break;
    case CTX_GO_ARTIST:
        pop_level(1);
        if (library_artist_name(S.lib, artist, name, sizeof name) == 0) {
            push(L_ARTIST_ALBUMS, name, artist);
            level_t *l = &S.stack[S.depth - 1];
            for (int i = 0; i < l->items.n; i++)
                if (l->items.ids[i] == album) l->sel = i;
        }
        break;
    case CTX_INFO:
        pop_level(1);
        push(L_INFO, "Info", track);
        S.stack[S.depth - 1].back_to_np = from_np;
        break;
    case CTX_Q_PLAY:
        pop_level(1);
        if (S.player) { player_queue_jump(S.player, qidx); S.now_playing_view = 1; }
        break;
    case CTX_Q_MOVE:
        pop_level(1);
        S.moving = 1;
        break;
    case CTX_Q_REMOVE:
        pop_level(1);
        if (S.player) player_queue_remove(S.player, qidx);
        break;
    case CTX_Q_CLEAR:
        pop_level(1);
        if (S.player) player_queue_clear(S.player);
        break;
    case CTX_ADD_PLAYLIST: {
        library_list_free(&S.pick_ids);
        library_list_free(&S.pick_pls);
        subject_tracks(&S.pick_ids);
        pop_level(1);
        lib_list_t all;
        library_list(S.lib, LIB_PLAYLISTS, 0, &all);           /* yours only */
        S.pick_pls.ids = malloc((size_t)(all.n ? all.n : 1) * sizeof *S.pick_pls.ids);
        S.pick_pls.n = 0;
        for (int i = 0; S.pick_pls.ids && i < all.n; i++)
            if (library_playlist_writable(S.lib, all.ids[i])) S.pick_pls.ids[S.pick_pls.n++] = all.ids[i];
        library_list_free(&all);
        push(L_PICK_PLAYLIST, "Add to playlist", 0);
        S.stack[S.depth - 1].back_to_np = from_np;
        break;
    }
    case CTX_PL_REMOVE: {
        pop_level(1);
        level_t *l = &S.stack[S.depth - 1];
        if (l->kind == L_PLAYLIST_TRACKS && library_playlist_remove(S.lib, l->parent, S.ctx_plpos) == 0) {
            library_list_free(&l->items);
            library_list(S.lib, LIB_PLAYLIST_TRACKS, l->parent, &l->items);
            if (l->sel >= l->items.n) l->sel = l->items.n ? l->items.n - 1 : 0;
        }
        break;
    }
    case CTX_PL_MOVE:
    case CTX_HOME_MOVE:
        pop_level(1);
        S.moving = 1;
        break;
    case CTX_BT_FORGET:
        pop_level(1);
        if (S.hooks.bt_do) S.hooks.bt_do(UI_BT_FORGET, S.ctx_bt);
        break;
    case CTX_WIFI_FORGET:
        pop_level(1);
        if (S.hooks.wifi_do) S.hooks.wifi_do(UI_WIFI_FORGET, S.ctx_wifi, NULL);
        break;
    case CTX_PL_RENAME:
        S.rename_id = S.ctx_playlist;
        pop_level(1);
        library_playlist_name(S.lib, S.rename_id, S.entry, sizeof S.entry);
        S.entry_ch = 0;
        push(L_RENAME, "Rename playlist", 0);
        break;
    case CTX_PL_DELETE: {
        uint32_t now = lv_tick_get();
        if (!(S.delete_armed && now - S.delete_armed < 3000)) {
            S.delete_armed = now;
            ui_toast("Press again to delete the playlist", 3000);
            break;
        }
        S.delete_armed = 0;
        int64_t victim = S.ctx_playlist;
        pop_level(1);
        if (library_playlist_delete(S.lib, victim) == 0) {
            level_t *l = &S.stack[S.depth - 1];
            if (l->kind == L_PLAYLISTS) {
                library_list_free(&l->items);
                library_list(S.lib, LIB_PLAYLISTS, 0, &l->items);
                if (l->sel >= l->items.n) l->sel = l->items.n ? l->items.n - 1 : 0;
            }
            ui_toast("Playlist deleted", 1500);
        }
        break;
    }
    case CTX_FAV_ADD:
    case CTX_FAV_REMOVE: {
        pop_level(0);
        library_set_favourite(S.lib, track, a == CTX_FAV_ADD);
        ui_toast(a == CTX_FAV_ADD ? "Added to favourites" : "Removed from favourites", 1500);
        level_t *l = &S.stack[S.depth - 1];
        if (l->kind == L_FAVOURITES) {      /* the list shows the change straight away */
            library_list_free(&l->items);
            library_list(S.lib, LIB_FAVOURITES, 0, &l->items);
            if (l->sel >= l->items.n) l->sel = l->items.n ? l->items.n - 1 : 0;
        }
        break;
    }
    }
}

/* OK on a quick settings / Display row. */
static void shuffle_prefs_apply(void)
{
    if (!S.player) return;
    player_shuffle_prefs_t sp = {
        .albums = settings_get_int("shuffle.albums", 0),
        .freshness = settings_get_int("shuffle.freshness", 1),
        .skip_interludes = settings_get_int("shuffle.skip_interludes", 1),
        .exclude_spoken = settings_get_int("shuffle.exclude_spoken", 0),
        .keep_works = settings_get_int("shuffle.keep_works", 0),
    };
    player_set_shuffle_prefs(S.player, &sp);
}

static void setting_ok(setting_t st)
{
    switch (st) {
    case ST_BRIGHTNESS: S.editing = !S.editing; break;
    case ST_PURE: if (S.hooks.pure_toggle) S.hooks.pure_toggle(); break;
    case ST_LANGUAGE: {                    /* English, then the card's translations, round */
        static char names[16][64];
        int n = S.hooks.languages ? S.hooks.languages(names, 16) : 0;
        if (!n) { ui_toast("No translations on the card: see fathom/lang", 2500); break; }
        const char *cur = settings_get("display.language", "");
        int i = 0;
        while (i < n && strcmp(names[i], cur)) i++;            /* n: English now */
        const char *next = i >= n ? names[0] : i + 1 < n ? names[i + 1] : "";
        settings_set("display.language", next);
        if (S.hooks.language_set) S.hooks.language_set(next);
        break;
    }
    case ST_TRANSPORT: if (S.hooks.transport_toggle) S.hooks.transport_toggle(); break;
    case ST_OUTPUT: if (S.hooks.signal_path) push(L_SIGNAL, "Signal path", 0); break;
    case ST_BALANCE: case ST_VOL_LIMIT:
        if (st == ST_BALANCE && S.pure) { ui_toast("Bypassed in Pure mode", 1500); break; }
        if (S.hooks.sound_adjust && S.hooks.sound_adjust(st == ST_BALANCE ? UI_SOUND_BALANCE : UI_SOUND_LIMIT, 0) == 0)
            S.editing = !S.editing;
        break;
    case ST_SHUFFLE: if (S.player) player_set_shuffle(S.player, !player_shuffle(S.player)); break;
    case ST_REPEAT: if (S.player) player_set_repeat(S.player, (repeat_t)((player_repeat(S.player) + 1) % 3)); break;
    case ST_SLEEP: {
        if (!S.player) break;
        int cur = player_sleep_minutes(S.player), n = (int)(sizeof SLEEPS / sizeof SLEEPS[0]), i = 0;
        while (i < n && SLEEPS[i] != cur) i++;
        player_set_sleep(S.player, i < n ? SLEEPS[(i + 1) % n] : 0);
        break;
    }
    case ST_STOP_AFTER: if (S.player) player_set_stop_after(S.player, !player_stop_after(S.player)); break;
    case ST_DIM: settings_set_int("display.dim", !settings_get_int("display.dim", 1)); break;
    case ST_ON_CHARGING: settings_set_int("display.on_charging", !settings_get_int("display.on_charging", 0)); break;
    case ST_DAC_FILTER: if (S.hooks.dac_filter_next) S.hooks.dac_filter_next(); break;
    case ST_EQ: {                          /* the profiles, for the output playing now */
        if (S.pure) { ui_toast("Bypassed in Pure mode", 1500); break; }
        char t[80];
        snprintf(t, sizeof t, "EQ: %s", S.eq_out[0] ? S.eq_out : "this output");
        push(L_EQ, t, 0);
        break;
    }
    case ST_LEVELLING: {
        if (S.pure) { ui_toast("Bypassed in Pure mode", 1500); break; }
        int v = (settings_get_int("sound.levelling", LEVEL_OFF) + 1) % 3;
        settings_set_int("sound.levelling", v);
        if (S.player) player_set_levelling(S.player, (levelling_t)v);
        int done = 0, total = 0;
        if (S.lib) library_rg_progress(S.lib, &done, &total);
        if (v != LEVEL_OFF && done < total) {
            char msg[96];
            snprintf(msg, sizeof msg, "%d of %d tracks measured: the rest while charging", done, total);
            ui_toast(msg, 3000);
        }
        break;
    }
    case ST_PAUSE_UNPLUG:
        settings_set_int("playback.pause_on_unplug", !settings_get_int("playback.pause_on_unplug", 1));
        break;
    case ST_SHUF_BY: case ST_SHUF_FRESH: case ST_SHUF_INTERLUDES: case ST_SHUF_WORKS: case ST_SHUF_SPOKEN: {
        static const char *const key[] = {"shuffle.albums", "shuffle.freshness", "shuffle.skip_interludes",
                                          "shuffle.keep_works", "shuffle.exclude_spoken"};
        static const int def[] = {0, 1, 1, 0, 0};
        int k = (int)(st - ST_SHUF_BY);
        settings_set_int(key[k], !settings_get_int(key[k], def[k]));
        shuffle_prefs_apply();
        if (S.player && player_shuffle(S.player)) ui_toast("From the next shuffle", 1500);
        break;
    }
    case ST_AUTO_OFF: {
        int cur = settings_get_int("power.auto_off", 15), n = (int)(sizeof AUTO_OFFS / sizeof AUTO_OFFS[0]), i = 0;
        while (i < n && AUTO_OFFS[i] != cur) i++;
        settings_set_int("power.auto_off", AUTO_OFFS[i < n ? (i + 1) % n : 1]);
        break;
    }
    case ST_RESCAN: if (S.hooks.rescan) S.hooks.rescan(); break;
    case ST_USB_DRIVE: if (S.hooks.usb_drive) S.hooks.usb_drive(); break;
    case ST_USB_COMPUTER: {      /* Ask, USB drive, USB DAC, Just charge */
        static const int order[] = {UI_USB_ASK, UI_USB_DRIVE, UI_USB_DAC, UI_USB_CHARGE};
        int v = settings_get_int("usb.computer", UI_USB_ASK), i = 0;
        while (i < 4 && order[i] != v) i++;
        settings_set_int("usb.computer", order[(i + 1) % 4]);
        break;
    }
    case ST_USB_DAC: if (S.hooks.usb_dac) S.hooks.usb_dac(); break;
    case ST_BT: if (S.hooks.bt_do) S.hooks.bt_do(UI_BT_TOGGLE, NULL); break;
    case ST_EQ_TOGGLE:
        if (S.pure) ui_toast("Bypassed in Pure mode", 1500);
        else if (S.hooks.eq_toggle) S.hooks.eq_toggle();
        break;
    case ST_BT_DEVICE: push(L_BLUETOOTH, "Bluetooth", 0); break;   /* the devices, and on/off */
    case ST_WIFI:
        if (S.hooks.wifi_do) {
            S.hooks.wifi_do(UI_WIFI_TOGGLE, NULL, NULL);
            settings_set_int("wifi.on", !S.wifi_on);
        }
        break;
    case ST_WIFI_NET:
        push(L_WIFI, "Wi-Fi", 0);
        if (S.hooks.wifi_do) S.hooks.wifi_do(UI_WIFI_SCAN, NULL, NULL);
        break;
    default: break;
    case ST_TIMEOUT: next_timeout(); break;
    }
}

static void open_selected(void)
{
    level_t *l = &S.stack[S.depth - 1];
    int64_t id = (library_backed(l->kind) && l->sel < l->items.n) ? l->items.ids[l->sel] : -1;
    char name[128];
    switch (l->kind) {
    case L_HOME:
        switch (l->sel < S.home_n ? S.home[l->sel] : -1) {
        case H_NOWPLAYING: S.now_playing_view = 1; break;
        case H_SHUFFLE:
            if (S.player) { player_shuffle_all(S.player); S.now_playing_view = 1; }
            break;
        case H_ALBUMS: push(L_ALBUMS, "Albums", 0); break;
        case H_ARTISTS: push(L_ARTISTS, "Artists", 0); break;
        case H_ALBUM_ARTISTS: push(L_ALBUM_ARTISTS, "Album artists", 0); break;
        case H_SONGS: push(L_SONGS, "Songs", 0); break;
        case H_PLAYLISTS: push(L_PLAYLISTS, "Playlists", 0); break;
        case H_GENRES: push(L_GENRES, "Genres", 0); break;
        case H_COMPOSERS: push(L_COMPOSERS, "Composers", 0); break;
        case H_YEARS: push(L_YEARS, "Years", 0); break;
        case H_FOLDERS: push(L_FOLDER, "Folders", 0); break;
        case H_SEARCH:
            S.entry[0] = '\0';
            S.entry_ch = 0;
            search_run();
            push(L_SEARCH, "Search", 0);
            break;
        case H_FAVOURITES: push(L_FAVOURITES, "Favourites", 0); break;
        case H_RECENT_ADDED: push(L_RECENT_ADDED, "Recently added", 0); break;
        case H_HISTORY: push(L_HISTORY, "History", 0); break;
        case H_RADIO:
            S.radio_n = S.hooks.radio_stations ? S.hooks.radio_stations(S.radio, RADIO_MAX) : -1;
            push(L_RADIO, "Radio", 0);
            if (S.now.radio && S.now.index < S.radio_n) S.stack[S.depth - 1].sel = S.now.index;
            break;
        case H_SETTINGS: push(L_SETTINGS, "Settings", 0); break;
        }
        break;
    case L_ALBUMS:
    case L_ARTIST_ALBUMS:
    case L_RECENT_ADDED:
    case L_GENRE_ALBUMS:
    case L_YEAR_ALBUMS:
        if (id >= 0 && library_album_name(S.lib, id, name, sizeof name) == 0) push(L_TRACKS, name, id);
        break;
    case L_GENRES:
        if (id >= 0 && library_genre_name(S.lib, id, name, sizeof name) == 0) push(L_GENRE_ALBUMS, name, id);
        break;
    case L_COMPOSERS:
        if (id >= 0 && library_composer_name(S.lib, id, name, sizeof name) == 0) push(L_COMPOSER_ALBUMS, name, id);
        break;
    case L_COMPOSER_ALBUMS:   /* their works on this album */
        if (id >= 0 && library_album_name(S.lib, id, name, sizeof name) == 0) push_composer_tracks(name, l->parent, id);
        break;
    case L_YEARS:
        if (id > 0) { snprintf(name, sizeof name, "%lld", (long long)id); push(L_YEAR_ALBUMS, name, id); }
        break;
    case L_FOLDER:
        if (l->sel >= l->items.n) break;
        if (id < 0) {                        /* a folder: open it */
            if (library_folder_name(S.lib, -id, name, sizeof name) == 0) push(L_FOLDER, name, -id);
        } else if (S.player) {               /* a song: the folder's songs, from this one */
            int64_t *songs = malloc((size_t)l->items.n * sizeof *songs);
            int ns = 0, at = 0;
            for (int i = 0; songs && i < l->items.n; i++)
                if (l->items.ids[i] > 0) { if (i == l->sel) at = ns; songs[ns++] = l->items.ids[i]; }
            if (ns) { player_play(S.player, songs, ns, at); S.now_playing_view = 1; }
            free(songs);
        }
        break;
    case L_HISTORY: push(l->sel == 0 ? L_RECENT_PLAYED : L_MOST_PLAYED, HISTORY[l->sel], 0); break;
    case L_RADIO: {
        if (!S.player || l->sel >= S.radio_n) break;
        char why[96] = "";
        if (S.hooks.radio_ready && S.hooks.radio_ready(why, sizeof why) != 0) { ui_toast(why, 2500); break; }
        if (S.now.radio && S.now.index == l->sel && S.now.playing) { S.now_playing_view = 1; break; }
        player_radio_play(S.player, S.radio, S.radio_n, l->sel);
        S.now_playing_view = 1;
        break;
    }
    case L_ALBUM_ARTISTS:
        if (id >= 0 && library_artist_name(S.lib, id, name, sizeof name) == 0) push(L_ARTIST_ALBUMS, name, id);
        break;
    case L_ARTISTS:
        if (id >= 0 && library_artist_name(S.lib, id, name, sizeof name) == 0) push(L_CREDIT_ALBUMS, name, id);
        break;
    case L_CREDIT_ALBUMS:   /* their songs on this album */
        if (id >= 0 && library_album_name(S.lib, id, name, sizeof name) == 0) push_credit_tracks(name, l->parent, id);
        break;
    case L_USB_ASK:
        pop();
        if (l->sel == 0 && S.hooks.usb_drive) S.hooks.usb_drive();
        if (l->sel == 1 && S.hooks.usb_dac) S.hooks.usb_dac();
        break;
    case L_HOME_EDIT: {
        int h = S.home_order[l->sel];
        if (h == H_SETTINGS) { ui_toast("Settings always stays on Home", 1500); break; }
        S.home_hidden[h] = !S.home_hidden[h];
        home_changed();
        break;
    }
    case L_PLAYLISTS:
        if (id >= 0 && library_playlist_name(S.lib, id, name, sizeof name) == 0) push(L_PLAYLIST_TRACKS, name, id);
        break;
    case L_TRACKS:
    case L_CREDIT_TRACKS:
    case L_COMPOSER_TRACKS:
    case L_SONGS:
    case L_PLAYLIST_TRACKS:
    case L_FAVOURITES:
    case L_RECENT_PLAYED:
    case L_MOST_PLAYED:
        if (id >= 0 && S.player) {
            player_play(S.player, l->items.ids, l->items.n, l->sel);
            S.now_playing_view = 1;
        }
        break;
    case L_WIFI:
        if (!S.hooks.wifi_do) break;
        if (l->sel == 0) {
            S.hooks.wifi_do(UI_WIFI_TOGGLE, NULL, NULL);
            settings_set_int("wifi.on", !S.wifi_on);
        } else if (l->sel == 1) {
            if (S.wifi_st.state == UI_WIFI_CONNECTED) {
                char t[80];
                snprintf(t, sizeof t, "%s, signal %d dBm", S.wifi_st.ip, S.wifi_st.dbm);
                ui_toast(t, 2500);
            }
        } else if (l->sel == 2) {
            S.hooks.wifi_do(UI_WIFI_SCAN, NULL, NULL);
        } else if (l->sel - 3 < S.wifi_n) {
            const ui_wifi_net_t *w = &S.wifi[l->sel - 3];
            if (w->connected) ui_toast("Connected", 1000);
            else if (w->saved || !w->secured) S.hooks.wifi_do(UI_WIFI_CONNECT, w->ssid, "");
            else {                                         /* a new network: its password */
                snprintf(S.wifi_pass_ssid, sizeof S.wifi_pass_ssid, "%s", w->ssid);
                memset(S.entry, 0, sizeof S.entry);
                S.entry_ch = 0;
                char t[80];
                snprintf(t, sizeof t, "Password for %s", w->ssid);
                push(L_WIFI_PASS, t, 0);
                ui_toast("Play adds a letter, Menu: a A 0 !", 2500);
            }
        }
        break;
    case L_SHARE:
        if (!S.hooks.share_do) break;
        if (l->sel == 0) {
            S.hooks.share_do(UI_SHARE_TOGGLE);
            if (!S.share_on) ui_toast("On while Wi-Fi is connected", 2000);
        } else if (S.share.state != UI_SHARE_ON) {
            if (S.share.state == UI_SHARE_WAITING) ui_toast("Settings > Wi-Fi: join a network", 2000);
        } else if (l->sel == 1) {
            ui_toast("Finder: https://fathom-h2.local", 3500);
        } else if (l->sel == 2) {
            ui_toast("Windows: Map network drive", 3000);
        } else if (l->sel == 5) {
            ui_toast("Compare it when the computer asks", 3000);
        } else if (l->sel == 6) {
            S.hooks.share_do(UI_SHARE_NEW_PASSWORD);
            ui_toast("New password: computers ask for it again", 2500);
        }
        share_look();
        render(R_CONTENT);
        break;
    case L_SSH:
        if (!S.hooks.ssh_do) break;
        if (l->sel == 0) {
            S.hooks.ssh_do(UI_SSH_TOGGLE);
            if (!S.ssh_on) ui_toast("On, over USB and Wi-Fi", 2500);
        } else if (S.ssh.state != UI_SSH_ON) {
        } else if (l->sel == 1 || l->sel == 2) {
            ui_toast("SFTP too: the music is in /mnt/sd/Music", 3000);
        } else if (l->sel == 5) {
            S.hooks.ssh_do(UI_SSH_NEW_PASSWORD);
            ui_toast("New password: the old one stops working", 2500);
        }
        ssh_look();
        render(R_CONTENT);
        break;
    case L_BLUETOOTH:
        if (!S.hooks.bt_do) break;
        if (l->sel == 0) S.hooks.bt_do(UI_BT_TOGGLE, NULL);
        else if (l->sel == 1) S.hooks.bt_do(UI_BT_SCAN, NULL);
        else if (l->sel - 2 < S.bt_n) S.hooks.bt_do(UI_BT_PRESS, S.bt[l->sel - 2].addr);
        break;
    case L_SETTINGS: {
        static const list_kind_t sub[] = {L_THEMES, L_HOME_EDIT, L_DISPLAY, L_SOUND, L_BLUETOOTH, L_WIFI, L_SHARE, L_SSH,
                                          L_USB, L_PLAYBACK, L_POWER, L_LIBRARY, L_ABOUT};
        push(sub[l->sel], SETTINGS[l->sel], 0);
        if (sub[l->sel] == L_HOME_EDIT) ui_toast("OK shows or hides. Menu: Move", 2500);
        if (sub[l->sel] == L_WIFI && S.hooks.wifi_do) S.hooks.wifi_do(UI_WIFI_SCAN, NULL, NULL);   /* fresh networks */
        break;
    }
    case L_ABOUT:
        if (l->sel == list_count(l) - 1) {   /* Reset all settings: press twice */
            uint32_t now = lv_tick_get();
            if (S.reset_armed && now - S.reset_armed < 3000) {
                S.reset_armed = 0;
                settings_reset();
                home_load();
                S.stack[0].sel = home_index(H_SETTINGS);
                if (S.hooks.brightness) S.hooks.brightness(brightness());
                if (S.hooks.screen_timeout) S.hooks.screen_timeout(screen_timeout());
                ui_set_theme(0);
                ui_toast("Settings reset", 1500);
            } else {
                S.reset_armed = now;
                ui_toast("Press again to reset all settings", 3000);
            }
        }
        break;
    case L_EQ:
        if (!S.hooks.eq_set) break;
        if (l->sel == 0) S.hooks.eq_set("");
        else if (l->sel - 1 < S.eq_n) S.hooks.eq_set(S.eq_names[l->sel - 1]);
        else eqe_new();
        break;
    case L_EQ_EDIT: {
        int n = S.eqe.n;
        if (l->sel == 0) S.editing = 1;                          /* the preamp, with the wheel */
        else if (l->sel <= n) {
            char t[80], hz[24];
            fmt_hz(hz, sizeof hz, S.eqe.band[l->sel - 1].fc);
            snprintf(t, sizeof t, "%s %s", EQ_TYPE_NAME[S.eqe.band[l->sel - 1].type], hz);
            push(L_EQ_BAND, t, l->sel - 1);
        } else if (l->sel == n + 1 && n < EQ_MAX_BANDS) {        /* a flat peak at 1 kHz, to shape */
            S.eqe.band[n] = (eq_band_t){EQ_PEAK, 1000, 0, 1};
            S.eqe.n++;
            eqe_changed(1);
            push(L_EQ_BAND, "Peak 1.0 kHz", n);
        } else {                                                 /* delete: press twice */
            uint32_t now = lv_tick_get();
            if (S.eqe_del_armed && now - S.eqe_del_armed < 3000) {
                S.eqe_del_armed = 0;
                if (S.hooks.eq_remove && S.hooks.eq_remove(S.eqe_name) == 0) {
                    pop();                                       /* back to the list, without it */
                    S.eq_n = S.hooks.eq_list ? S.hooks.eq_list(S.eq_names, 64) : 0;
                    level_t *up = &S.stack[S.depth - 1];
                    if (up->sel > S.eq_n + 1) up->sel = S.eq_n + 1;
                    ui_toast("EQ deleted", 1500);
                } else {
                    ui_toast("Couldn't delete it", 2000);
                }
            } else {
                S.eqe_del_armed = now;
                ui_toast("Press again to delete this EQ", 3000);
            }
        }
        break;
    }
    case L_EQ_BAND: {
        int b = (int)l->parent;
        if (b < 0 || b >= S.eqe.n) break;
        if (l->sel == 0) {                                       /* the type: the next one */
            S.eqe.band[b].type = (eq_type_t)((S.eqe.band[b].type + 1) % 3);
            eqe_changed(1);
        } else if (l->sel <= 3) {
            S.editing = 1;
        } else {                                                 /* remove */
            memmove(&S.eqe.band[b], &S.eqe.band[b + 1], (size_t)(S.eqe.n - b - 1) * sizeof S.eqe.band[0]);
            S.eqe.n--;
            eqe_changed(1);
            pop();
        }
        break;
    }
    case L_THEMES: {
        const theme_t *th = &S.themes[l->sel];
        if (th->status == THEME_ERROR) { ui_toast(th->note, 4000); break; }
        ui_set_theme(l->sel);
        if (th->status == THEME_WARN) ui_toast(th->note, 4000);
        break;
    }
    case L_QUICK: case L_DISPLAY: case L_SOUND: case L_PLAYBACK: case L_POWER: case L_LIBRARY: case L_USB:
        setting_ok(setting_at(l, l->sel));
        break;
    case L_CONTEXT: if (l->sel < S.ctx_n) run_context(S.ctx[l->sel]); break;
    case L_SEARCH:
        if (l->sel >= 1) {
            int64_t rid;
            int kind = search_kind(l->sel - 1, &rid);
            if (rid < 0) break;
            if (kind == 0 && library_artist_name(S.lib, rid, name, sizeof name) == 0) push_artist(rid, name);
            else if (kind == 1 && library_album_name(S.lib, rid, name, sizeof name) == 0) push(L_TRACKS, name, rid);
            else if (kind == 2 && S.player) {    /* the matching songs, from this one */
                player_play(S.player, S.found.songs.ids, S.found.songs.n,
                            l->sel - 1 - S.found.artists.n - S.found.albums.n);
                S.now_playing_view = 1;
            }
        }
        break;
    case L_PICK_PLAYLIST: {
        int64_t pl;
        if (l->sel == 0) {                     /* a new one: "Playlist N" (names can't be typed yet) */
            char nm[48];
            snprintf(nm, sizeof nm, "Playlist %d", library_playlist_count_writable(S.lib) + 1);
            pl = library_playlist_create(S.lib, nm);
        } else {
            pl = S.pick_pls.ids[l->sel - 1];
        }
        char msg[192] = "Can't write the playlist";
        if (pl > 0 && library_playlist_add(S.lib, pl, S.pick_ids.ids, S.pick_ids.n) == 0) {
            char nm[128] = "";
            library_playlist_name(S.lib, pl, nm, sizeof nm);
            snprintf(msg, sizeof msg, "Added to %s", nm);
        }
        pop();
        ui_toast(msg, 1500);
        break;
    }
    case L_QUEUE:
        if (S.player && l->sel < list_count(l)) { player_queue_jump(S.player, l->sel); S.now_playing_view = 1; l->sel = 0; }
        break;
    default: break;
    }
}

/* ---------- fast scrolling ----------
 * Wheel steps less than 80 ms apart build up speed in long lists: 1 row, then
 * 3, 8 and 20 per step. While it goes fast through an alphabetical list, the
 * initial of where you are shows in the middle of the screen. */

static int wheel_step(int n)
{
    uint32_t now = lv_tick_get();
    S.wheel_streak = now - S.wheel_last < 80 ? S.wheel_streak + 1 : 0;
    S.wheel_last = now;
    if (n <= 40) return 1;
    return S.wheel_streak < 6 ? 1 : S.wheel_streak < 16 ? 3 : S.wheel_streak < 30 ? 8 : 20;
}

static int alphabetical(list_kind_t k)
{
    return k == L_ALBUMS || k == L_ARTISTS || k == L_ALBUM_ARTISTS || k == L_SONGS || k == L_PLAYLISTS ||
           k == L_FAVOURITES || k == L_GENRES || k == L_COMPOSERS;
}

static void initial_hide(lv_timer_t *t)
{
    (void)t;
    if (S.letter) lv_obj_add_flag(S.letter, LV_OBJ_FLAG_HIDDEN);
    S.letter_timer = NULL;
}

/* The letter the selected row sorts under ("The Beatles" under B, digits under #). */
static void show_initial(const level_t *l)
{
    if (!alphabetical(l->kind) || l->sel >= list_count(l)) return;
    char main[256], meta[32];
    int playing;
    list_item(l, l->sel, main, sizeof main, meta, sizeof meta, &playing);
    const char *p = main;     /* as the library sorts: leading punctuation, then "the ", skipped */
    while (*p && (unsigned char)*p < 0x80 && !isalnum((unsigned char)*p)) p++;
    if (!*p) p = main;
    if (l->kind != L_SONGS && l->kind != L_FAVOURITES && !strncasecmp(p, "the ", 4)) p += 4;
    char fold[8];
    path_loose_key(p, 1 << 30, fold, sizeof fold);     /* first letter, accents folded */
    char letter[4] = "#";
    if (fold[0] >= 'a' && fold[0] <= 'z') { letter[0] = (char)(fold[0] - 32); letter[1] = 0; }

    const theme_t *t = T();
    if (!S.letter) {
        S.letter = box(lv_layer_top(), 0, 0, 72, 72);
        S.letter_lbl = lv_label_create(S.letter);
        lv_obj_center(S.letter_lbl);
    }
    lv_obj_set_style_bg_color(S.letter, C(t->surface), 0);
    lv_obj_set_style_bg_opa(S.letter, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(S.letter, t->radius, 0);
    lv_obj_set_style_border_color(S.letter, C(t->signal), 0);
    lv_obj_set_style_border_width(S.letter, 2, 0);
    lv_obj_set_style_text_font(S.letter_lbl, S.f->title, 0);
    lv_obj_set_style_text_color(S.letter_lbl, C(t->text), 0);
    lv_label_set_text(S.letter_lbl, letter);
    lv_obj_center(S.letter_lbl);
    lv_obj_align(S.letter, LV_ALIGN_CENTER, 0, 10);
    lv_obj_remove_flag(S.letter, LV_OBJ_FLAG_HIDDEN);
    if (S.letter_timer) lv_timer_reset(S.letter_timer);
    else {
        S.letter_timer = lv_timer_create(initial_hide, 700, NULL);
        lv_timer_set_repeat_count(S.letter_timer, 1);
    }
}

/* ---------- scrolling long text ----------
 * After a 1 s pause, the selected row, the header title and Now Playing's
 * artist and album lines scroll back and forth if they don't fit, resting
 * 2 s at each end. Any key puts them back (ellipsis) and restarts the wait. */

#define MARQUEE_REST_MS 2000   /* the rest at each end of a scroll (LVGL's own: 0.3 s) */

/* LVGL takes the delays at each end of a long-mode scroll from the label's
 * "anim" style: a template animation. */
static void marquee_calm(lv_obj_t *l)
{
    static lv_anim_t tmpl;
    static int ready;
    if (!ready) {
        lv_anim_init(&tmpl);
        lv_anim_set_repeat_count(&tmpl, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_playback_delay(&tmpl, MARQUEE_REST_MS);   /* at the end of the text */
        lv_anim_set_repeat_delay(&tmpl, MARQUEE_REST_MS);     /* back at the start */
        ready = 1;
    }
    lv_obj_set_style_anim(l, &tmpl, 0);
}

static void marquee_set(int on)
{
    lv_label_long_mode_t m = on ? LV_LABEL_LONG_SCROLL : LV_LABEL_LONG_DOT;
    for (int r = 0; r < SLOTS; r++)
        if (lv_label_get_long_mode(S.row_lbl[r]) != LV_LABEL_LONG_DOT) lv_label_set_long_mode(S.row_lbl[r], LV_LABEL_LONG_DOT);
    if (lv_label_get_long_mode(S.h_left) != m) lv_label_set_long_mode(S.h_left, m);
    /* Now Playing's lines only while it shows: hidden, they'd scroll on unseen, 60 times a second */
    lv_label_long_mode_t nm = S.now_playing_view ? m : LV_LABEL_LONG_DOT;
    if (lv_label_get_long_mode(S.np_artist) != nm) lv_label_set_long_mode(S.np_artist, nm);
    if (lv_label_get_long_mode(S.np_album) != nm) lv_label_set_long_mode(S.np_album, nm);
    if (on && !S.now_playing_view && list_count(&S.stack[S.depth - 1]) > 0) {
        int slot = slot_under_pill();
        if (slot >= 0 && slot < SLOTS) lv_label_set_long_mode(S.row_lbl[slot], LV_LABEL_LONG_SCROLL);
    }
}

static void marquee_fire(lv_timer_t *t)
{
    marquee_set(1);
    lv_timer_pause(t);      /* once per pause: re-armed by the next key */
}

static void marquee_arm(void)
{
    marquee_set(0);
    if (!S.marquee_timer) {
        S.marquee_timer = lv_timer_create(marquee_fire, 1000, NULL);
        lv_timer_set_repeat_count(S.marquee_timer, -1);
    }
    lv_timer_reset(S.marquee_timer);
    lv_timer_resume(S.marquee_timer);
}

void ui_library_changed(void)
{
    /* Refreshed in place (a background scan may finish while you listen or
     * browse): every open list is read again, keeping its place where it can. */
    S.lib_gen++;
    finish_transition();
    pending_free();
    for (int d = 0; d < S.depth; d++) {
        level_t *l = &S.stack[d];
        if (!library_backed(l->kind) || !S.lib) continue;
        int64_t was = l->sel < l->items.n ? l->items.ids[l->sel] : -1;
        library_list_free(&l->items);
        if (l->kind == L_CREDIT_TRACKS) library_list_credit_tracks(S.lib, l->parent, l->parent2, &l->items);
        else if (l->kind == L_COMPOSER_TRACKS) library_list_composer_tracks(S.lib, l->parent, l->parent2, &l->items);
        else library_list(S.lib, view_of(l->kind), l->parent, &l->items);
        int sel = l->sel < l->items.n ? l->sel : (l->items.n ? l->items.n - 1 : 0);
        for (int i = 0; i < l->items.n; i++)
            if (l->items.ids[i] == was) { sel = i; break; }
        l->sel = sel;
        if (l->top > sel) l->top = sel;
    }
    if (S.stack[S.depth - 1].kind == L_SEARCH) search_run();
    render(R_SNAP);
}

void ui_wifi_changed(void)
{
    ui_wifi_status_t st = {0};
    char busy[32];
    int on = S.hooks.wifi_state ? S.hooks.wifi_state(busy, sizeof busy, &st) : 0;
    int link = !on ? 0 : st.state == UI_WIFI_CONNECTED ? 2 : busy[0] || st.state == UI_WIFI_CONNECTING ? 1 : 0;
    if (link != S.wifi_link) {
        S.wifi_link = link;
        if (S.h_right) render_header_status();
    }
    level_t *l = &S.stack[S.depth - 1];
    if ((l->kind != L_WIFI && l->kind != L_QUICK) || S.now_playing_view) return;
    int n = list_count(l);
    if (l->sel >= n) l->sel = n - 1;
    render(R_CONTENT);
}

void ui_share_changed(void)
{
    level_t *l = &S.stack[S.depth - 1];
    if (l->kind != L_SHARE || S.now_playing_view) return;
    int n = list_count(l);
    if (l->sel >= n) l->sel = n - 1;
    render(R_CONTENT);
}

void ui_ssh_changed(void)
{
    level_t *l = &S.stack[S.depth - 1];
    if (l->kind != L_SSH || S.now_playing_view) return;
    int n = list_count(l);
    if (l->sel >= n) l->sel = n - 1;
    render(R_CONTENT);
}

void ui_bt_changed(void)
{
    level_t *l = &S.stack[S.depth - 1];
    if ((l->kind != L_BLUETOOTH && l->kind != L_QUICK) || S.now_playing_view) return;
    int n = list_count(l);
    if (l->sel >= n) l->sel = n - 1;
    render(R_CONTENT);
}

void ui_set_eq(const char *profile, const char *output)
{
    snprintf(S.eq_name, sizeof S.eq_name, "%s", profile ? profile : "");
    snprintf(S.eq_out, sizeof S.eq_out, "%s", output ? output : "");
    if (S.depth > 0) render(R_CONTENT);
}

/* With the screen off nothing is drawn or sent to the panel (LVGL takes no
 * invalidations, renders are put off, scrolling text stops): keys and the
 * player carry on as usual. Woken, the screen is drawn whole, as it is now. */
void ui_set_awake(int on)
{
    if (S.asleep == !on) return;
    S.asleep = !on;
    if (!S.h_right) return;
    if (!on) {
        render_header_status();               /* (the charging pulse stops) */
        marquee_set(0);
        if (S.marquee_timer) lv_timer_pause(S.marquee_timer);
        lv_display_enable_invalidation(NULL, false);
        return;
    }
    lv_display_enable_invalidation(NULL, true);
    render_mode_t m = S.wake_mode;
    S.wake_mode = R_CONTENT;
    render(m);
    lv_obj_invalidate(lv_screen_active());
    lv_obj_invalidate(lv_layer_top());
    marquee_arm();
}

void ui_set_pure(int pure, int bitperfect)
{
    if (S.pure == !!pure && S.bitperfect == !!bitperfect) return;
    S.pure = !!pure;
    S.bitperfect = !!bitperfect;
    if (S.depth > 0) render(R_CONTENT);
}

void ui_set_output(const char *name)
{
    snprintf(S.output, sizeof S.output, "%s", name ? name : "");
    if (S.now_playing_view) render(R_CONTENT);
}

void ui_key(ui_key_t k)
{
    marquee_arm();
    if (k == UI_KEY_OK || k == UI_KEY_BACK || k == UI_KEY_BACK_LONG) capture();
    level_t *l = &S.stack[S.depth - 1];
    player_t *pl = S.player;
    /* keys that do the same thing everywhere */
    switch (k) {
    case UI_KEY_PLAY: if (pl) player_toggle_pause(pl); render(R_CONTENT); return;
    case UI_KEY_PREV: if (pl) player_prev(pl); render(R_CONTENT); return;
    case UI_KEY_NEXT: if (pl) player_next(pl); render(R_CONTENT); return;
    case UI_KEY_MENU:   /* context menu; pressing again closes it (or quick settings) */
        if (!S.now_playing_view && l->kind == L_WIFI_PASS) {   /* a password: the next group (a, A, 0, !) */
            int g = 0, ng = (int)(sizeof PASS_GROUPS / sizeof PASS_GROUPS[0]);
            while (g < ng && PASS_GROUPS[g] <= S.entry_ch) g++;
            S.entry_ch = PASS_GROUPS[g % ng];
            S.ok_added = 0;
            render(R_CONTENT);
            return;
        }
        if (!S.now_playing_view && (l->kind == L_CONTEXT || l->kind == L_QUICK)) pop();
        else if (open_context()) ui_toast("No actions here", 1000);
        render(R_ANIMATE);
        return;
    case UI_KEY_MENU_LONG: open_quick(); render(R_ANIMATE); return;
    case UI_KEY_SEEK_BACK:
    case UI_KEY_SEEK_FWD: {
        /* held: 2 s steps, then 5 s after ~2 s, then 15 s after ~5 s (repeats every ~200 ms) */
        uint32_t now = lv_tick_get();
        S.seek_streak = now - S.seek_last < 400 ? S.seek_streak + 1 : 0;
        S.seek_last = now;
        int step = S.seek_streak < 10 ? 2000 : S.seek_streak < 25 ? 5000 : 15000;
        if (pl) player_seek_by(pl, k == UI_KEY_SEEK_FWD ? step : -step);
        if (pl && !S.now_playing_view) {
            player_now_t n;
            player_now(pl, &n);
            char a[16], b[16], t[48];
            fmt_time(a, sizeof a, (int)(n.pos_ms / 1000), 0);
            fmt_time(b, sizeof b, (int)(n.len_ms / 1000), 0);
            snprintf(t, sizeof t, "%s / %s", a, b);
            ui_toast(t, 800);
        }
        render(R_CONTENT);
        return;
    }
    case UI_KEY_BRIGHTER: set_brightness(brightness() + 1); render(R_CONTENT); return;
    case UI_KEY_DIMMER: set_brightness(brightness() - 1); render(R_CONTENT); return;
    default: break;
    }
    if (S.now_playing_view) {
        int len = (int)(S.now.len_ms / 1000);
        switch (k) {
        case UI_KEY_UP: case UI_KEY_DOWN:      /* scrub: move the marker; faster the faster you turn */
            if (!pl || !S.now.active || len <= 0) break;
            if (!S.scrubbing) {
                S.scrubbing = 1;
                S.scrub_s = (int)(S.now.pos_ms / 1000);
                S.scrub_seq = player_seq(pl);
            }
            S.scrub_s += (k == UI_KEY_UP ? -5 : 5) * wheel_step(1000);
            if (S.scrub_s < 0) S.scrub_s = 0;
            if (S.scrub_s > len - 1) S.scrub_s = len - 1;
            S.scrub_last = lv_tick_get();
            break;
        case UI_KEY_OK:
            if (S.scrubbing && pl) {
                S.scrubbing = 0;
                player_seek_by(pl, S.scrub_s * 1000 - (int)S.now.pos_ms);
            } else if (pl) {
                player_toggle_pause(pl);
            }
            break;
        case UI_KEY_BACK: case UI_KEY_BACK_LONG:
            if (S.scrubbing && k == UI_KEY_BACK) S.scrubbing = 0;   /* cancel */
            else { S.scrubbing = 0; S.now_playing_view = 0; }
            break;
        default: break;
        }
        render(R_ANIMATE);
        return;
    }
    int n = list_count(l);
    if ((l->kind == L_SEARCH || l->kind == L_RENAME || l->kind == L_WIFI_PASS) && l->sel == 0) {   /* typing with the wheel */
        const char *letters = entry_letters(l->kind);
        int nl = (int)strlen(letters);
        size_t len = strlen(S.entry);
        switch (k) {
        case UI_KEY_UP: S.entry_ch = (S.entry_ch + nl) % (nl + 1); S.ok_added = 0; break;
        case UI_KEY_DOWN: S.entry_ch = (S.entry_ch + 1) % (nl + 1); S.ok_added = 0; break;
        case UI_KEY_OK: {
            /* a double-click: undo the letter the first click added, then Results / Done */
            int dbl = S.ok_added && len && lv_tick_elaps(S.ok_tick) < DOUBLE_CLICK_MS;
            S.ok_added = 0;
            if (dbl) {
                S.entry[--len] = '\0';
                if (l->kind == L_SEARCH) search_run();
                n = list_count(l);
            }
            if (dbl || S.entry_ch == nl) {               /* Results / Done / Join */
                if (l->kind == L_SEARCH) { if (n > 1) l->sel = 1; }
                else if (l->kind == L_WIFI_PASS) {
                    if (strlen(S.entry) < 8) { ui_toast("At least 8 characters", 1500); break; }
                    if (S.hooks.wifi_do) S.hooks.wifi_do(UI_WIFI_CONNECT, S.wifi_pass_ssid, S.entry);
                    memset(S.entry, 0, sizeof S.entry);
                    pop();
                    ui_toast("Joining...", 1500);
                }
                else if (S.entry[0] && library_playlist_rename(S.lib, S.rename_id, S.entry) == 0) {
                    pop();
                    level_t *up = &S.stack[S.depth - 1];   /* show the new name */
                    if (up->kind == L_PLAYLISTS) {
                        library_list_free(&up->items);
                        library_list(S.lib, LIB_PLAYLISTS, 0, &up->items);
                    }
                    ui_toast("Playlist renamed", 1500);
                } else {
                    ui_toast(S.entry[0] ? "Can't rename the playlist" : "Type a name first", 1500);
                }
            } else if (len + 1 < sizeof S.entry) {
                char c = letters[S.entry_ch];
                /* names: a capital at the start of each word */
                if (l->kind == L_RENAME && c >= 'a' && c <= 'z' && (len == 0 || S.entry[len - 1] == ' ')) c -= 32;
                if (l->kind == L_WIFI_PASS && len + 1 > 63) { ui_toast("63 characters at most", 1500); break; }
                S.entry[len] = c;
                S.entry[len + 1] = '\0';
                if (l->kind == L_SEARCH) search_run();
                S.ok_added = 1;
                S.ok_tick = lv_tick_get();
            }
            break;
        }
        case UI_KEY_BACK:                                  /* delete a letter; nothing left: leave */
            S.ok_added = 0;
            if (len) {
                S.entry[len - 1] = '\0';
                if (l->kind == L_SEARCH) search_run();
            } else {
                pop();
            }
            break;
        case UI_KEY_BACK_LONG: S.now_playing_view = 1; break;
        default: break;
        }
        render(R_ANIMATE);
        return;
    }
    if (l->kind == L_SEARCH && k == UI_KEY_BACK) {           /* in the results: back to the letters */
        l->sel = 0;
        render(R_ANIMATE);
        return;
    }
    if (S.moving && l->kind == L_PLAYLIST_TRACKS) {   /* moving a song in your playlist */
        int d = k == UI_KEY_UP ? -1 : k == UI_KEY_DOWN ? 1 : 0;
        if (d && library_playlist_move(S.lib, l->parent, l->sel, d) == 0) {
            int64_t t = l->items.ids[l->sel];
            l->items.ids[l->sel] = l->items.ids[l->sel + d];
            l->items.ids[l->sel + d] = t;
            l->sel += d;
        } else if (k == UI_KEY_OK || k == UI_KEY_BACK) {
            S.moving = 0;
        }
        render(R_CONTENT);
        return;
    }
    if (S.moving && l->kind == L_HOME_EDIT) {   /* moving a Home item with the wheel */
        int d = k == UI_KEY_UP ? -1 : k == UI_KEY_DOWN ? 1 : 0;
        if (d && l->sel + d >= 0 && l->sel + d < H_N) {
            int t = S.home_order[l->sel];
            S.home_order[l->sel] = S.home_order[l->sel + d];
            S.home_order[l->sel + d] = t;
            l->sel += d;
            home_changed();
        } else if (k == UI_KEY_OK || k == UI_KEY_BACK) {
            S.moving = 0;
        }
        render(R_CONTENT);
        return;
    }
    if (S.moving && l->kind == L_QUEUE) {   /* moving a queued track with the wheel */
        switch (k) {
        case UI_KEY_UP: if (l->sel > 0) { player_queue_move(pl, l->sel, -1); l->sel--; } break;
        case UI_KEY_DOWN: if (l->sel < n - 1) { player_queue_move(pl, l->sel, +1); l->sel++; } break;
        case UI_KEY_OK: case UI_KEY_BACK: S.moving = 0; break;
        default: break;
        }
        render(R_CONTENT);
        return;
    }
    if (S.editing && (l->kind == L_EQ_EDIT || l->kind == L_EQ_BAND)) {   /* an EQ value, with the wheel */
        switch (k) {
        case UI_KEY_UP: eqe_adjust(l, -1); break;
        case UI_KEY_DOWN: eqe_adjust(l, +1); break;
        case UI_KEY_OK: case UI_KEY_BACK: S.editing = 0; eqe_changed(1); break;   /* done: saved */
        default: break;
        }
        render(R_CONTENT);
        return;
    }
    if (S.editing && is_setting_level(l->kind)) {   /* changing a value */
        setting_t st = setting_at(l, l->sel);
        int dir = k == UI_KEY_UP ? -1 : k == UI_KEY_DOWN ? 1 : 0;
        switch (k) {
        case UI_KEY_UP:
        case UI_KEY_DOWN:
            if (st == ST_BRIGHTNESS) set_brightness(brightness() + dir);
            else if (S.hooks.sound_adjust)
                S.hooks.sound_adjust(st == ST_BALANCE ? UI_SOUND_BALANCE : UI_SOUND_LIMIT, dir);
            break;
        case UI_KEY_OK: case UI_KEY_BACK: S.editing = 0; break;
        default: break;
        }
        render(R_CONTENT);
        return;
    }
    switch (k) {
    case UI_KEY_UP:
    case UI_KEY_DOWN: {
        int step = wheel_step(n);
        int sel = l->sel + (k == UI_KEY_DOWN ? step : -step);
        l->sel = sel < 0 ? 0 : sel > n - 1 ? (n ? n - 1 : 0) : sel;
        if (step > 1) show_initial(l);
        break;
    }
    case UI_KEY_OK: open_selected(); break;
    case UI_KEY_BACK: pop(); break;
    case UI_KEY_BACK_LONG: S.now_playing_view = 1; break;
    default: break;
    }
    render(R_ANIMATE);
}

#define SCRUB_IDLE_MS 3000

int ui_tick(void)
{
    if (!S.player) return 0;
    uint32_t seq = player_seq(S.player);
    player_tick(S.player);
    /* scrubbing: cancelled after a pause in turning, or when the track changes */
    if (S.scrubbing && (lv_tick_elaps(S.scrub_last) > SCRUB_IDLE_MS || player_seq(S.player) != S.scrub_seq)) {
        S.scrubbing = 0;
        render(R_CONTENT);
    }
    if (player_seq(S.player) != seq) render(R_CONTENT);   /* gapless track change */
    return S.scrubbing || !player_idle(S.player);
}

void ui_tick_1s(void)
{
    S.ticking = 1;
    render(R_CONTENT);
    S.ticking = 0;
    settings_autosave(lv_tick_get(), 1500);
}

void ui_set_hooks(const ui_hooks_t *h)
{
    if (h) S.hooks = *h;
    else memset(&S.hooks, 0, sizeof S.hooks);
}

void ui_set_theme(int index)
{
    if (index < 0 || index >= S.n_themes || S.themes[index].status == THEME_ERROR) return;
    finish_transition();
    S.cur_theme = index;
    settings_set("ui.theme", S.themes[index].name);
    apply_theme();
    render(R_SNAP);
}

int ui_theme_count(void) { return S.n_themes; }

void ui_show(const char *what)
{
    S.scrubbing = 0;
    finish_transition();
    pending_free();
    while (S.depth > 1) pop();
    S.stack[0].sel = home_index(H_ALBUMS);
    S.now_playing_view = 0;
    if (!strcmp(what, "albums") || !strcmp(what, "tracks")) {
        push(L_ALBUMS, "Albums", 0);
        level_t *l = &S.stack[S.depth - 1];
        l->sel = l->items.n > 1 ? 1 : 0;
        char name[128];
        if (!strcmp(what, "tracks") && l->items.n && S.lib &&
            library_album_name(S.lib, l->items.ids[l->sel], name, sizeof name) == 0)
            push(L_TRACKS, name, l->items.ids[l->sel]);
    } else if (!strcmp(what, "playlists") || !strcmp(what, "playlist")) {
        S.stack[0].sel = home_index(H_PLAYLISTS);
        push(L_PLAYLISTS, "Playlists", 0);
        level_t *l = &S.stack[S.depth - 1];
        l->sel = l->items.n > 2 ? 2 : 0;
        char name[128];
        if (!strcmp(what, "playlist") && l->items.n && S.lib &&
            library_playlist_name(S.lib, l->items.ids[l->sel], name, sizeof name) == 0)
            push(L_PLAYLIST_TRACKS, name, l->items.ids[l->sel]);
    } else if (!strcmp(what, "share")) {
        S.stack[0].sel = home_index(H_SETTINGS);
        push(L_SETTINGS, "Settings", 0);
        S.stack[S.depth - 1].sel = SET_SHARE;
        push(L_SHARE, "Network drive", 0);
        S.stack[S.depth - 1].sel = 1;
    } else if (!strcmp(what, "ssh")) {
        S.stack[0].sel = home_index(H_SETTINGS);
        push(L_SETTINGS, "Settings", 0);
        S.stack[S.depth - 1].sel = SET_SSH;
        push(L_SSH, "SSH", 0);
        S.stack[S.depth - 1].sel = 1;
    } else if (!strcmp(what, "radio")) {
        S.stack[0].sel = home_index(H_RADIO);
        S.radio_n = S.hooks.radio_stations ? S.hooks.radio_stations(S.radio, RADIO_MAX) : -1;
        push(L_RADIO, "Radio", 0);
        if (S.radio_n > 1) S.stack[S.depth - 1].sel = 1;
    } else if (!strcmp(what, "search")) {
        S.stack[0].sel = home_index(H_SEARCH);
        snprintf(S.entry, sizeof S.entry, "bu");
        S.entry_ch = 'e' - 'a';
        search_run();
        push(L_SEARCH, "Search", 0);
    } else if (!strcmp(what, "artists") || !strcmp(what, "artist") || !strcmp(what, "artistsongs")) {
        S.stack[0].sel = home_index(H_ARTISTS);
        push(L_ARTISTS, "Artists", 0);
        level_t *l = &S.stack[S.depth - 1];
        const char *want = getenv("FATHOM_SIM_ARTIST");   /* a name to open, else the third */
        char name[128];
        l->sel = l->items.n > 2 ? 2 : 0;
        for (int i = 0; want && S.lib && i < l->items.n; i++)
            if (library_artist_name(S.lib, l->items.ids[i], name, sizeof name) == 0 && !strcmp(name, want)) l->sel = i;
        if (strcmp(what, "artists") && l->items.n && S.lib &&
            library_artist_name(S.lib, l->items.ids[l->sel], name, sizeof name) == 0) {
            push(L_CREDIT_ALBUMS, name, l->items.ids[l->sel]);
            level_t *a = &S.stack[S.depth - 1];
            if (!strcmp(what, "artistsongs") && a->items.n &&
                library_album_name(S.lib, a->items.ids[0], name, sizeof name) == 0)
                push_credit_tracks(name, a->parent, a->items.ids[0]);
        }
    } else if (!strcmp(what, "albumartists")) {
        S.stack[0].sel = home_index(H_ALBUM_ARTISTS);
        push(L_ALBUM_ARTISTS, "Album artists", 0);
    } else if (!strcmp(what, "homeend")) {    /* Home, at its last items */
        S.stack[0].sel = S.home_n > 0 ? S.home_n - 2 : 0;
    } else if (!strcmp(what, "homeall")) {    /* Settings > Home: every item, shown or not */
        S.stack[0].sel = home_index(H_SETTINGS);
        push(L_SETTINGS, "Settings", 0);
        push(L_HOME_EDIT, "Home", 0);
        S.stack[S.depth - 1].sel = H_N - 2;
    } else if (!strcmp(what, "genres")) {
        S.stack[0].sel = home_index(H_GENRES);
        push(L_GENRES, "Genres", 0);
    } else if (!strcmp(what, "folders")) {
        S.stack[0].sel = home_index(H_FOLDERS);
        push(L_FOLDER, "Folders", 0);
    } else if (!strcmp(what, "folder")) {     /* the first folder in Folders */
        S.stack[0].sel = home_index(H_FOLDERS);
        push(L_FOLDER, "Folders", 0);
        level_t *l = &S.stack[S.depth - 1];
        char name[128];
        if (l->items.n && l->items.ids[0] < 0 && S.lib &&
            library_folder_name(S.lib, -l->items.ids[0], name, sizeof name) == 0)
            push(L_FOLDER, name, -l->items.ids[0]);
    } else if (!strcmp(what, "years")) {
        S.stack[0].sel = home_index(H_YEARS);
        push(L_YEARS, "Years", 0);
    } else if (!strcmp(what, "scrub")) {       /* Now Playing with the marker moved on */
        S.now_playing_view = 1;
        render(R_SNAP);
        for (int i = 0; i < 12; i++) ui_key(UI_KEY_DOWN);
        finish_transition();
    } else if (!strcmp(what, "usbask")) {
        ui_usb_prompt();
    } else if (!strcmp(what, "sound")) {
        S.stack[0].sel = home_index(H_SETTINGS);
        push(L_SETTINGS, "Settings", 0);
        S.stack[S.depth - 1].sel = SET_SOUND;
        push(L_SOUND, "Sound", 0);
        S.stack[S.depth - 1].sel = 2;
    } else if (!strcmp(what, "signal")) {
        S.stack[0].sel = home_index(H_SETTINGS);
        push(L_SETTINGS, "Settings", 0);
        S.stack[S.depth - 1].sel = SET_SOUND;
        push(L_SOUND, "Sound", 0);
        push(L_SIGNAL, "Signal path", 0);
    } else if (!strcmp(what, "homeedit")) {
        S.stack[0].sel = home_index(H_SETTINGS);
        push(L_SETTINGS, "Settings", 0);
        S.stack[S.depth - 1].sel = SET_HOME;
        push(L_HOME_EDIT, "Home", 0);
        S.stack[S.depth - 1].sel = 5;
    } else if (!strcmp(what, "songs")) {
        S.stack[0].sel = home_index(H_SONGS);
        push(L_SONGS, "Songs", 0);
    } else if (!strcmp(what, "quick")) {
        open_quick();
    } else if (!strcmp(what, "queue")) {
        S.now_playing_view = 1;
        if (open_context() == 0) run_context(CTX_SHOW_QUEUE);
    } else if (!strcmp(what, "context") || !strcmp(what, "info")) {
        push(L_ALBUMS, "Albums", 0);
        level_t *l = &S.stack[S.depth - 1];
        l->sel = l->items.n > 1 ? 1 : 0;
        char name[128];
        if (l->items.n && S.lib && library_album_name(S.lib, l->items.ids[l->sel], name, sizeof name) == 0) {
            push(L_TRACKS, name, l->items.ids[l->sel]);
            if (open_context() == 0 && !strcmp(what, "info")) run_context(CTX_INFO);
        }
    } else if (!strcmp(what, "nowplaying")) {
        S.now_playing_view = 1;
    } else if (!strcmp(what, "themes")) {
        push(L_SETTINGS, "Settings", 0);
        push(L_THEMES, "Theme", 0);
    }
    render(R_SNAP);
}

/* ---------- construction ---------- */

void ui_init(lv_obj_t *screen, const theme_t *themes, int n_themes,
             const ui_fonts_t *fonts, int n_fonts, library_t *lib, player_t *player)
{
    memset(&S, 0, sizeof S);
    S.lib = lib;
    S.player = player;
    shuffle_prefs_apply();
    S.np_seq = (uint32_t)-1;
    S.art_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    S.art_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    S.art_dsc.header.w = LIB_ART_LARGE;
    S.art_dsc.header.h = LIB_ART_LARGE;
    S.art_dsc.header.stride = LIB_ART_LARGE * 2;
    S.art_dsc.data = (const uint8_t *)S.art_px;
    S.art_dsc.data_size = sizeof S.art_px;
    S.themes = themes; S.n_themes = n_themes;
    S.fonts = fonts; S.n_fonts = n_fonts;
    S.battery_pct = -1;
    S.scr = screen;
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    S.header = box(screen, 0, 0, UI_W, HEADER_H);
    S.h_left = label(S.header);
    lv_obj_set_width(S.h_left, UI_W - 80);
    lv_obj_align(S.h_left, LV_ALIGN_LEFT_MID, 12, 0);
    S.h_right = label(S.header);
    lv_obj_set_width(S.h_right, LV_SIZE_CONTENT);
    lv_obj_align(S.h_right, LV_ALIGN_RIGHT_MID, -12, 0);
    /* the indicator slots, in a row from the right; a fixed width (sized to its
     * content it came out too narrow and clipped the first icon) */
    S.h_mode = box(S.header, 0, 0, HDR_SLOTS * 22, 20);
    lv_obj_set_flex_flow(S.h_mode, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(S.h_mode, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(S.h_mode, 6, 0);
    lv_obj_align(S.h_mode, LV_ALIGN_RIGHT_MID, -56, 0);
    for (int i = 0; i < HDR_SLOTS; i++) {
        S.h_slot_img[i] = lv_image_create(S.h_mode);
        S.h_slot_sym[i] = label(S.h_mode);
        lv_obj_set_width(S.h_slot_sym[i], LV_SIZE_CONTENT);
        show(S.h_slot_img[i], 0);
        show(S.h_slot_sym[i], 0);
    }

    /* The list clips its children, so the spare rows and sliding content stay inside. */
    S.list = box(screen, 0, HEADER_H, UI_W, UI_H - HEADER_H);

    int pw = UI_W - 2 * ROW_MARGIN;
    S.pill = box(S.list, ROW_MARGIN, 0, pw, ROW_H);   /* created first: drawn behind the rows */
    S.pill_notch[0] = box(S.pill, 0, 0, STEP, STEP);
    S.pill_notch[1] = box(S.pill, pw - STEP, 0, STEP, STEP);
    S.pill_notch[2] = box(S.pill, 0, ROW_H - STEP, STEP, STEP);
    S.pill_notch[3] = box(S.pill, pw - STEP, ROW_H - STEP, STEP, STEP);
    S.pill_cursor = label(S.pill);
    lv_label_set_text(S.pill_cursor, LV_SYMBOL_PLAY);
    lv_obj_align(S.pill_cursor, LV_ALIGN_LEFT_MID, 8, 0);

    S.rows_box = box(S.list, 0, -ROW_PITCH, UI_W, SLOTS * ROW_PITCH);
    for (int j = 0; j < SLOTS; j++) {
        lv_obj_t *row = box(S.rows_box, ROW_MARGIN, j * ROW_PITCH, pw, ROW_H);
        S.row[j] = row;
        S.row_dot[j] = box(row, 10, (ROW_H - 8) / 2, 8, 8);
        S.row_eq[j][0] = box(row, 10, ROW_Y0 + 22, 3, 6);
        S.row_eq[j][1] = box(row, 14, ROW_Y0 + 16, 3, 12);
        S.row_eq[j][2] = box(row, 18, ROW_Y0 + 20, 3, 8);
        S.row_icon[j] = lv_label_create(row);
        lv_label_set_text(S.row_icon[j], "");   /* not LVGL's "Text": the icon font has only icons */
        lv_obj_remove_flag(S.row_icon[j], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_width(S.row_icon[j], ICON_W);
        lv_obj_set_style_text_align(S.row_icon[j], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(S.row_icon[j], LV_ALIGN_LEFT_MID, 10, 0);
        show(S.row_icon[j], 0);
        S.row_sw[j] = box(row, 10, (ROW_H - SWATCH_H) / 2, SWATCH_W, SWATCH_H);
        lv_obj_set_style_border_width(S.row_sw[j], 1, 0);
        lv_obj_set_style_border_opa(S.row_sw[j], LV_OPA_50, 0);
        S.row_sw_part[j][0] = box(S.row_sw[j], 3, 3, SWATCH_W - 8, 7);       /* the selection bar */
        S.row_sw_part[j][1] = box(S.row_sw[j], 3, SWATCH_H - 8, 14, 3);     /* a line of text */
        S.row_sw_part[j][2] = box(S.row_sw[j], 20, SWATCH_H - 8, SWATCH_W - 25, 3);   /* the accent */
        show(S.row_sw[j], 0);
        S.row_picon[j] = lv_image_create(row);
        lv_obj_remove_flag(S.row_picon[j], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_align(S.row_picon[j], LV_ALIGN_LEFT_MID, 10, 0);
        lv_obj_set_style_image_recolor_opa(S.row_picon[j], LV_OPA_COVER, 0);
        show(S.row_picon[j], 0);
        S.row_lbl[j] = label(row);
        lv_obj_align(S.row_lbl[j], LV_ALIGN_LEFT_MID, 10, 0);
        S.row_meta[j] = label(row);
        lv_obj_set_width(S.row_meta[j], LV_SIZE_CONTENT);
        lv_obj_align(S.row_meta[j], LV_ALIGN_RIGHT_MID, -10, 0);
        S.row_wheel[j] = box(row, 0, 0, WHEEL_W, ROW_H);
        lv_obj_set_flex_flow(S.row_wheel[j], LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(S.row_wheel[j], LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(S.row_wheel[j], 2, 0);
        lv_obj_align(S.row_wheel[j], LV_ALIGN_RIGHT_MID, -8, 0);
        for (int w = 0; w < 3; w++) {
            S.row_ch[j][w] = lv_label_create(S.row_wheel[j]);
            lv_obj_set_style_text_align(S.row_ch[j][w], LV_TEXT_ALIGN_CENTER, 0);
        }
        show(S.row_wheel[j], 0);
    }

    S.ghost = lv_image_create(screen);
    lv_obj_remove_flag(S.ghost, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    show(S.ghost, 0);

    S.np = box(screen, 0, HEADER_H, UI_W, UI_H - HEADER_H);
    const int art_x = 12, tx = 160, tw = 150, ty = 8;
    const int title_h = 64, artist_y = 74, album_y = NP_ALBUM_Y, chip_y = 122;
    S.np_shadow = box(S.np, art_x + STEP, 6 + STEP, 136, 136);
    S.np_art = box(S.np, art_x, 6, 136, 136);
    lv_obj_set_style_clip_corner(S.np_art, true, 0);
    S.np_img = lv_image_create(S.np_art);
    lv_obj_set_pos(S.np_img, 0, 0);
    show(S.np_img, 0);
    S.np_radio = label(S.np_art);
    lv_label_set_text(S.np_radio, "");   /* (the icon font has no letters) */
    lv_obj_center(S.np_radio);
    show(S.np_radio, 0);
    S.np_radio_px = lv_image_create(S.np_art);
    lv_obj_set_style_image_recolor_opa(S.np_radio_px, LV_OPA_COVER, 0);
    lv_obj_center(S.np_radio_px);
    show(S.np_radio_px, 0);
    S.np_title = label(S.np);   /* two lines at most, then "..." */
    lv_obj_set_pos(S.np_title, tx, ty);
    lv_obj_set_size(S.np_title, tw, title_h);   /* two title lines (22-30 px) with their line spacing */
    lv_label_set_long_mode(S.np_title, LV_LABEL_LONG_DOT);   /* after sizing: wraps to 2 lines */
    S.np_artist = label(S.np);
    lv_obj_set_pos(S.np_artist, tx, artist_y);
    lv_obj_set_width(S.np_artist, tw);
    S.np_album = label(S.np);
    lv_obj_set_pos(S.np_album, tx, album_y);
    lv_obj_set_width(S.np_album, tw);
    S.np_chip = label(S.np);
    lv_obj_set_pos(S.np_chip, tx, chip_y);
    lv_obj_set_style_pad_hor(S.np_chip, 8, 0);
    lv_obj_set_style_pad_ver(S.np_chip, 2, 0);
    lv_label_set_long_mode(S.np_chip, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(S.np_chip, LV_SIZE_CONTENT);
    int py = UI_H - HEADER_H - 38;
    S.np_track = box(S.np, 12, py, UI_W - 24, 6);
    S.np_fill = box(S.np_track, 0, 0, 0, 6);
    int seg_w = (UI_W - 24 - (SEGMENTS - 1) * 2) / SEGMENTS;
    for (int i = 0; i < SEGMENTS; i++) S.np_seg[i] = box(S.np, 12 + i * (seg_w + 2), py - 1, seg_w, 8);
    S.np_t0 = label(S.np);
    lv_obj_set_width(S.np_t0, LV_SIZE_CONTENT);
    lv_obj_set_pos(S.np_t0, 12, py + 12);
    S.np_marker = box(S.np, 12, py - 5, 3, 16);   /* scrubbing: where OK would jump to */
    show(S.np_marker, 0);
    S.np_t1 = label(S.np);
    lv_obj_set_width(S.np_t1, LV_SIZE_CONTENT);
    lv_obj_align(S.np_t1, LV_ALIGN_TOP_RIGHT, -12, py + 12);
    S.np_out = label(S.np);               /* output, between the times */
    lv_obj_set_pos(S.np_out, 70, py + 12);
    lv_obj_set_width(S.np_out, UI_W - 140);
    lv_obj_set_style_text_align(S.np_out, LV_TEXT_ALIGN_CENTER, 0);
    S.np_next = label(S.np);              /* "Next: ..." */
    lv_obj_set_pos(S.np_next, 12, NP_NEXT_Y);
    lv_obj_set_width(S.np_next, UI_W - 24);
    /* paused: the cover dims and shows two bars */
    S.np_pause = box(S.np_art, 0, 0, 136, 136);
    lv_obj_set_style_bg_color(S.np_pause, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(S.np_pause, LV_OPA_50, 0);
    for (int i = 0; i < 2; i++) {
        lv_obj_t *bar = box(S.np_pause, 50 + i * 24, 44, 12, 48);
        lv_obj_set_style_bg_color(bar, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_90, 0);
    }
    show(S.np_pause, 0);
    /* empty lists: one centred line */
    S.empty = label(S.list);
    lv_obj_set_width(S.empty, UI_W - 40);
    lv_obj_set_style_text_align(S.empty, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(S.empty, LV_ALIGN_CENTER, 0, -10);
    show(S.empty, 0);

    S.stack[0].kind = L_HOME;
    home_load();
    snprintf(S.stack[0].title, sizeof S.stack[0].title, "Fathom");
    S.depth = 1;
    S.last_depth = -1;          /* first render is a "new level" (snapped) */

    for (int r = 0; r < SLOTS; r++) marquee_calm(S.row_lbl[r]);
    marquee_calm(S.h_left);
    marquee_calm(S.np_artist);
    marquee_calm(S.np_album);
    apply_theme();
    render(R_SNAP);
    marquee_arm();
}
