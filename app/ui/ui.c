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
#include "core/rng.h"
#include "net/radiodir.h"
#include "net/podcast.h"
#include "net/podsync.h"
#include "net/coverfind.h"

#include <math.h>
#include <stddef.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define HEADER_H   28
#define ROW_PITCH  42
#define ROW_H      38
#define ROWS       5
#define ROW_Y0     ((ROW_H - 38) / 2)   /* the playing-bars marker, centred in a row */
#define ROW_MARGIN 6
#define SLOTS      (ROWS + 2)   /* one spare row above and below for scrolling */
/* The EQ editor's curve, over the top of the list as Battery's graph is: 20 Hz
 * to 20 kHz on a log scale across the plot, +-12 dB top to bottom, worked out
 * at EQG_PTS points */
#define EQG_X0 10
#define EQG_X1 (UI_W - 10)
#define EQG_Y0 4
#define EQG_Y1 72
#define EQG_RANGE 12.0
#define EQG_PTS 100
#define ABOUT_MAX  16           /* Settings > About: the device's rows at most */
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
                                   "History", "Radio", "Settings", "Genres", "Composers", "Years", "Folders",
                                   "Podcasts"};
static const char *const HOME_ID[] = {"nowplaying", "shuffle", "search", "albums", "artists",
                                      "albumartists", "songs", "playlists", "favourites", "recent",
                                      "history", "radio", "settings", "genres", "composers", "years", "folders",
                                      "podcasts"};
/* (Items added later come after Settings in this numbering, which pixel_icons.h
 * follows; on Home they go before Settings.) */
enum { H_NOWPLAYING, H_SHUFFLE, H_SEARCH, H_ALBUMS, H_ARTISTS, H_ALBUM_ARTISTS, H_SONGS, H_PLAYLISTS,
       H_FAVOURITES, H_RECENT_ADDED, H_HISTORY, H_RADIO, H_SETTINGS, H_GENRES, H_COMPOSERS, H_YEARS, H_FOLDERS,
       H_PODCASTS, H_N };
/* Hidden until shown in Settings > Home (fewer people tag them) */
static int home_hidden_by_default(int h) { return h == H_COMPOSERS || h == H_YEARS; }
/* Their icons: Tabler Icons code points (player-play, arrows-shuffle, search,
 * disc, microphone-2, users, music, playlist, heart, clock-plus, history,
 * radio, settings; then category, feather, calendar, folder), and Pixel Pastel's own, drawn on its 10 px grid (pixel_icons.h,
 * from tools/icons/pixel_icons.py), in the same order. */
/* Home's "Update available!": first on Home while a newer Fathom is on offer
 * (update.found, from the update check) and not ignored (update.ignored); not
 * an item Settings > Home shows, hides or moves. Its icon: circle-arrow-up
 * (Tabler), and Pixel Pastel's own after the items' (pixel_icons.h). */
#define H_UPDATE H_N
#define UPDATE_ICON 0xf703
#define PODCAST_ICON 0xe000   /* drawn into the icon font: tools/fonts/add_podcast_icon.py */
/* Playlists: a smart playlist's icon, sparkles (Tabler, copied into the icon
 * font), and Pixel Pastel's own after the update's; yours have Home's playlist */
#define SMART_ICON 0xf6d7
#define PIXEL_SMART (H_N + 1)
static const uint32_t HOME_ICON[H_N] = {0xed46, 0xf000, 0xeb1c, 0xea90, 0xef2c, 0xebf2,
                                        0xeafc, 0xeec0, 0xeabe, 0xf7c5, 0xebea, 0xef2d, 0xeb20,
                                        0xf1f6, 0xee8b, 0xea53, 0xeaad,    /* category, feather, calendar, folder */
                                        PODCAST_ICON};
#include "pixel_icons.h"
#define PIXEL_ICON_SCALE 2
#define ICON_W 22     /* the icon's column; the text starts after it */
#define ICON_GAP 10
#define BT_DEV_ROW 3   /* Settings > Bluetooth: on/off, Find headphones, Audio, then the devices */
#define SWATCH_W 30   /* Settings > Interface > Theme: a little picture of each theme */
#define SWATCH_H 20

/* Letters for the wheel: search (results last) and names (done last). */
static const char SEARCH_CHARS[] = "abcdefghijklmnopqrstuvwxyz0123456789 ";
static const char NAME_CHARS[] = "abcdefghijklmnopqrstuvwxyz0123456789 -'&.,!?()";
/* passwords: every printable character, in groups Menu jumps between */
static const char PASS_CHARS[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
                                 " !\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~";
static const int PASS_GROUPS[] = {0, 26, 52, 62};   /* a, A, 0, symbols */
static const char *const HISTORY[] = {"Recently played", "Most played"};
static const char *const SETTINGS[] = {"Interface", "Home", "Display", "Sound", "Bluetooth", "Wi-Fi",
                                       "Network drive", "SSH", "USB connection", "Playback", "Podcasts", "Power",
                                       "Library", "About"};
enum { SET_INTERFACE, SET_HOME, SET_DISPLAY, SET_SOUND, SET_BLUETOOTH, SET_WIFI, SET_SHARE, SET_SSH, SET_USB, SET_PLAYBACK,
       SET_PODCASTS, SET_POWER, SET_LIBRARY, SET_ABOUT };

/* Rows of the quick settings panel and of Settings > Display. */
typedef enum {
    ST_BRIGHTNESS, ST_SHUFFLE, ST_REPEAT, ST_SLEEP, ST_STOP_AFTER, ST_TIMEOUT,
    ST_DIM, ST_ON_CHARGING, ST_COVERS, ST_OUTPUT, ST_EQ, ST_LEVELLING, ST_DAC_FILTER, ST_PAUSE_UNPLUG, ST_AUTO_OFF,
    ST_RESCAN, ST_USB_DRIVE, ST_USB_COMPUTER, ST_USB_DAC, ST_N_SONGS, ST_N_ALBUMS, ST_N_ARTISTS, ST_N_PLAYLISTS,
    ST_BT, ST_WIFI,
    ST_SHUF_BY, ST_SHUF_FRESH, ST_SHUF_INTERLUDES, ST_SHUF_WORKS, ST_SHUF_SPOKEN, ST_BALANCE, ST_VOL_LIMIT, ST_PURE, ST_TRANSPORT, ST_BATTERY, ST_LANGUAGE,
    ST_THEME, ST_ALBUM_ARTISTS, ST_POD_AUTO, ST_POD_KEEP, ST_POD_CLEAN, ST_POD_SPEED, ST_FIND_COVERS, ST_CROSSFEED,
} setting_t;
/* Quick settings (quick_rows(): Bluetooth's and Wi-Fi's rows where they are) */
static const setting_t QUICK[] = {ST_BRIGHTNESS, ST_SHUFFLE, ST_REPEAT, ST_EQ, ST_BT, ST_WIFI,
                                  ST_SLEEP, ST_STOP_AFTER, ST_TIMEOUT};
static const setting_t SOUND_ROWS[] = {ST_OUTPUT, ST_PURE, ST_EQ, ST_CROSSFEED, ST_BALANCE, ST_VOL_LIMIT, ST_LEVELLING, ST_TRANSPORT,
                                       ST_USB_DAC, ST_DAC_FILTER};   /* sound_rows(): those with their hooks */
static const setting_t PLAYBACK_ROWS[] = {ST_SHUFFLE, ST_SHUF_BY, ST_SHUF_FRESH, ST_SHUF_INTERLUDES, ST_SHUF_WORKS,
                                         ST_SHUF_SPOKEN, ST_REPEAT, ST_PAUSE_UNPLUG};

/* Settings > Playback's shuffle rows, to the player (at start and when changed). */
static void shuffle_prefs_apply(void);
static const setting_t POWER_ROWS[] = {ST_BATTERY, ST_AUTO_OFF, ST_SLEEP};
static const setting_t POD_ROWS[] = {ST_POD_AUTO, ST_POD_KEEP, ST_POD_CLEAN, ST_POD_SPEED};
static const int POD_KEEPS[] = {1, 2, 3, 5};
static const int POD_CLEANS[] = {1, 3, 7, 30, 0};            /* days; 0 = never */
static const setting_t LIBRARY_ROWS[] = {ST_RESCAN, ST_FIND_COVERS, ST_N_SONGS, ST_N_ALBUMS, ST_N_ARTISTS, ST_N_PLAYLISTS};
static const setting_t USB_ROWS[] = {ST_USB_COMPUTER, ST_USB_DRIVE, ST_USB_DAC};
static const int AUTO_OFFS[] = {5, 15, 30, 0};               /* minutes; 0 = never */
static const int SLEEPS[] = {0, 15, 30, 45, 60, 90, PLAYER_SLEEP_END_ALBUM};   /* minutes; 0 = off */
static const setting_t DISPLAY_ROWS[] = {ST_BRIGHTNESS, ST_TIMEOUT, ST_DIM, ST_ON_CHARGING};
static const setting_t INTERFACE_ROWS[] = {ST_THEME, ST_COVERS, ST_ALBUM_ARTISTS, ST_LANGUAGE};
static const int TIMEOUTS[] = {15, 30, 60, 120, 300, 0};   /* seconds; 0 = never */

/* The theme editor: its colours, as it names them, and its rows */
enum { TC_BG, TC_SURFACE, TC_TEXT, TC_MUTED, TC_SELBG, TC_SELFG, TC_SIGNAL, TC_MODE, TC_INFO, TC_BATTERY,
       TC_ARTIST, TC_ALBUM, TC_CHIPBG, TC_CHIPFG, TC_WARNING, TC_DANGER, TC_N };
enum { TE_NAME, TE_BASE, TE_ACCENT, TE_COLOURS, TE_FONT, TE_SIZE, TE_SELECTION, TE_CORNERS, TE_RADIUS,
       TE_PROGRESS, TE_PLAYING, TE_CURSOR, TE_SHADOW, TE_MOTION, TE_SAVE, TE_DELETE, TE_N };

/* Context menu actions. */
typedef enum {
    CTX_PLAY, CTX_PLAY_NEXT, CTX_ADD_QUEUE, CTX_SHOW_QUEUE, CTX_GO_ALBUM, CTX_GO_ARTIST, CTX_INFO,
    CTX_Q_PLAY, CTX_Q_MOVE, CTX_Q_REMOVE, CTX_Q_CLEAR, CTX_FAV_ADD, CTX_FAV_REMOVE,
    CTX_ADD_PLAYLIST, CTX_PL_REMOVE, CTX_PL_MOVE, CTX_PL_DELETE, CTX_PL_RENAME, CTX_HOME_MOVE,
    CTX_BT_FORGET, CTX_WIFI_FORGET,
    CTX_RADIO_PLAY, CTX_RADIO_ADD, CTX_RADIO_REMOVE,
    CTX_POD_REFRESH, CTX_POD_UNSUB, CTX_POD_DOWNLOAD, CTX_POD_CANCEL, CTX_POD_DELETE, CTX_POD_PLAYED, CTX_POD_UNPLAYED,
    CTX_POD_CHAPTERS, CTX_POD_SPEED, CTX_FIND_COVER, CTX_REMOVE_COVER, CTX_FIND_ANOTHER,
    CTX_SHUFFLE,
} ctx_action_t;
static const char *const CTX_LABEL[] = {
    "Play", "Play next", "Add to queue", "Show queue", "Go to album", "Go to artist", "Info",
    "Play now", "Move", "Remove from queue", "Clear queue", "Add to favourites", "Remove from favourites",
    "Add to playlist", "Remove from playlist", "Move", "Delete playlist", "Rename playlist", "Move",
    "Forget this device", "Forget this network",
    "Play", "Add to your stations", "Remove from your stations",
    "Check for new episodes", "Unsubscribe", "Download", "Cancel download", "Delete download", "Mark as played",
    "Mark as not played", "Chapters", "Speed", "Find cover", "Remove cover", "Find another cover",
    "Shuffle",
};

/* Rows of the track info screen. */
enum { INFO_TITLE, INFO_ARTIST, INFO_ALBUM, INFO_TRACK, INFO_FORMAT, INFO_LENGTH, INFO_FILE, INFO_N };

/* ---------- state ---------- */

typedef enum {
    L_HOME, L_ALBUMS, L_ALBUM_ARTISTS, L_ARTIST_ALBUMS, L_ARTISTS, L_CREDIT_ALBUMS, L_CREDIT_TRACKS,
    L_GENRES, L_GENRE_ALBUMS, L_COMPOSERS, L_COMPOSER_ALBUMS, L_COMPOSER_TRACKS, L_YEARS, L_YEAR_ALBUMS, L_FOLDER,
    L_TRACKS, L_SONGS, L_PLAYLISTS, L_PLAYLIST_TRACKS, L_HOME_EDIT,
    L_FAVOURITES, L_RECENT_ADDED, L_RECENT_PLAYED, L_MOST_PLAYED, L_HISTORY,
    L_SETTINGS, L_INTERFACE, L_THEMES, L_QUICK, L_DISPLAY, L_SOUND, L_EQ, L_EQ_EDIT, L_EQ_BAND, L_THEME_EDIT, L_THEME_COLOURS, L_BLUETOOTH, L_WIFI, L_WIFI_PASS, L_SHARE, L_SSH, L_USB, L_PLAYBACK, L_POD_SETTINGS, L_POWER, L_LIBRARY, L_ABOUT, L_SIGNAL,
    L_CONTEXT, L_INFO, L_QUEUE, L_PICK_PLAYLIST, L_SEARCH, L_RENAME, L_USB_ASK, L_WIFI_ASK, L_RADIO,
    /* Home > Radio > Find stations: its menu, genres, countries, a list of stations, search by name */
    L_RDIR, L_RDIR_GENRES, L_RDIR_COUNTRIES, L_RDIR_RESULTS, L_RDIR_SEARCH, L_PODCASTS, L_POD_SHOW, L_POD_SEARCH, L_POD_CHAPTERS,
    L_UPDATE, L_BATTERY, L_BAT_SPAN, L_BAT_HISTORY, L_UNINSTALL, L_UPD_ASK, L_COVER_FIND,
    L_SMART_TRACKS,   /* a smart playlist's songs (parent: LIB_SMART_*) */
    L_PLACEHOLDER
} list_kind_t;

typedef struct {
    list_kind_t kind;
    int sel, top;
    int64_t parent;         /* artist, album or playlist id */
    int64_t parent2;        /* L_CREDIT_TRACKS, L_COMPOSER_TRACKS: the album (parent: the artist, composer) */
    lib_list_t items;       /* library-backed levels: row ids */
    char title[128];
    int title_ui;           /* the title is the UI's own words (translated), not a name */
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
    int home[H_N + 1], home_n;  /* the items shown on Home (+ H_UPDATE, first, when there's one) */
    lv_timer_t *marquee_timer;  /* starts scrolling long text after a pause */
    lv_obj_t *np_next, *np_out, *np_pause, *empty;
    int lib_counts[4];      /* songs, albums, artists, playlists (Library screen) */
    uint32_t reset_armed;   /* About > Reset: first press time; a second within 3 s resets */
    uint32_t uni_armed;     /* About > Uninstall Fathom > Uninstall: the same */
    ui_uninstall_t uni;     /* its state, as the device last gave it */
    lv_obj_t *uni_ov;       /* the screen while it waits for the H2 to be switched off */
    char output[80];
    int pure, bitperfect;   /* Pure mode for this output; ...and nothing changes the samples */
    uint32_t seek_last; int seek_streak;

    /* the context menu's subject and actions */
    ctx_action_t ctx[12]; int ctx_n;
    int64_t ctx_track, ctx_album, ctx_artist, ctx_playlist;
    int ctx_credited;                         /* ctx_artist from Artists (credited), not Album artists */
    int smart_n[LIB_SMART_N];                 /* Playlists: each smart playlist's songs, as last opened */
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
    /* Find cover (net/coverfind.h): the album, the match shown, its picture */
    lv_obj_t *cf, *cf_art, *cf_img, *cf_title, *cf_artist, *cf_meta, *cf_note, *cf_hint;
    uint16_t cf_px[LIB_ART_LARGE * LIB_ART_LARGE];
    lv_image_dsc_t cf_dsc;
    int64_t cf_album;
    int cf_k;                 /* the match shown */
    int cf_drawn;             /* what's drawn: (find state, matches, picture state, k) */
    unsigned cf_batch_seq;    /* Find missing covers: the last batch said */

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
    lv_obj_t *row_line[SLOTS], *row_lt[SLOTS], *row_la[SLOTS];   /* album rows: title and artist, one line */
    lv_obj_t *row_icon[SLOTS], *row_picon[SLOTS];   /* Home: an icon glyph, or a pixel icon */
    lv_obj_t *row_mark[SLOTS];                      /* a podcast episode on the card: an arrow onto a line */
    lv_obj_t *row_sw[SLOTS], *row_sw_part[SLOTS][3];  /* Theme: a little picture of each (bar, text, accent) */
    lv_obj_t *row_cov[SLOTS];       /* Albums: each album's cover, the library's 34 px thumbnail */
    lv_image_dsc_t cov_dsc[SLOTS];  /* (their pixels: g_cov, only while Albums is shown) */
    int64_t cov_id[SLOTS];          /* the album each slot shows (-1: to be read) */
    lv_obj_t *row_wheel[SLOTS], *row_ch[SLOTS][3];   /* the typing row: letter before, the wheel's (larger), after */
    int slot_item[SLOTS], slot_playing[SLOTS];
    int slot_accent[SLOTS];   /* drawn in the theme's accent colour (Home's Update available!) */
    int slot_mark[SLOTS];     /* the on-the-card mark before the meta (a downloaded podcast episode) */
    /* album rows (line_layout): shown so, and how wide the title and the
     * artist are in full */
    int line_on[SLOTS], line_tw[SLOTS], line_aw[SLOTS];
    /* Settings > About's rows from the device (about_look): read at most once
     * a second, not for every count and row (the battery's are read from the
     * PMIC: scrolling there lagged) */
    int about_n, help_shown;
    uint32_t about_at;
    char about_lbl[ABOUT_MAX][32], about_val[ABOUT_MAX][48];
    /* "Turn on Wi-Fi?" answered Turn on: what to do once it connects, if
     * the screen it was asked from is still there (wifi_then_run) */
    struct {
        int what, armed, depth, sel, q_null;
        list_kind_t kind;
        uint32_t at;
        rdir_query_t q;
    } wifi_then;
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
    /* Find stations (net/radiodir.h): the request running (RD_JOB_*) for the
     * level at rd_job_depth; the open list's stations (Results, Search) and its
     * query; the countries; how the last request for the list went; the
     * station a context menu is for */
    int rd_job, rd_job_depth;
    int rd_state;           /* RD_IDLE, RD_FAILED (rd_msg says why) */
    char rd_msg[96];
    rdir_query_t rd_q;
    radio_station_t *rd_st;
    rdir_meta_t *rd_meta;
    int rd_n;
    rdir_country_t *rd_cty;
    int rd_cty_n;
    radio_station_t rd_ctx;
    int rd_ctx_i;           /* its row in rd_st, -1 if it isn't one */
    uint32_t rd_del_armed;  /* "Remove from your stations": first press time */
    /* Updates: as of the last look; the progress screen over everything while
     * one downloads, unpacks or installs (NULL: not shown) */
    ui_update_t upd;
    lv_obj_t *upd_ov, *upd_title, *upd_bar, *upd_detail, *upd_hint;
    lv_obj_t *wn_ov, *wn_body, *wn_hint;   /* What's new: the panel, its points, the hint */
    lv_obj_t *wn_row[16];     /* ...each point (the wheel moves through them a whole point at a time) */
    int wn_h[16], wn_n, wn_first, wn_last;
    char wn_ver[24], wn_text[1024];
    /* Settings > Power > Battery: the history as of bat_at (ms), its graph
     * (over the list, which moves down below it: list_y0), the span shown */
    batrep_t bat;
    int bat_days, bat_span;   /* bat_span: -1 since full charge, else one of bat.span */
    uint32_t bat_at;
    lv_obj_t *bat_graph, *bat_lbl[9];
    lv_obj_t *eq_graph, *eq_lbl[3], *eq_warn;   /* the EQ editor's curve (eq_place) */
    eq_profile_t eqg_for;                     /* the profile eqg_db[] was worked out for */
    int eqg_have;
    float eqg_db[EQG_PTS];                    /* its bands' response, preamp left out */
    /* the theme editor (Settings > Interface > Theme, Menu on a theme) */
    theme_t the;                              /* the theme being made or changed */
    char the_file[128];                       /* its file in the themes folder ("" = not saved yet) */
    int the_dirty;                            /* changed since it was opened or saved */
    int the_acc;                              /* the accent picked (ACCENTS), -1 = its own colours */
    int the_pal;                              /* the colour being picked (the_pal_at), -1 = its own */
    uint32_t the_back_armed, the_del_armed;
    int rename_theme;                         /* the name entry names the theme, not a playlist */
    /* Podcasts (Home > Podcasts; net/podsync.h): read from the card when the
     * pages open, and again when the worker changed something */
    pod_show_t *pod_shows; int pod_n;
    char pod_cur[13]; int pod_ci;             /* the show open (its id; its index in pod_shows, -1 none) */
    pod_episode_t *pod_eps; int pod_ne;
    pod_state_t *pod_st; int pod_nst;
    unsigned char pod_on_card[POD_MAX_EPISODES];
    int64_t pod_seen;                         /* the open show: episodes newer than this are new */
    unsigned pod_changed, pod_failed, pod_imported;
    pod_show_t *pod_found; int pod_nf, pod_search;   /* Find podcasts: the shows found, how it went */
    char pod_opml[300];                       /* an OPML file to import ("" none) */
    int pod_ctx;                              /* the episode (or show) a menu is for */
    char pod_ctx_key[13], pod_ctx_url[512];   /* ...which (found again when the lists are read again) */
    uint32_t pod_del_armed;
    struct { char id[13], key[13]; uint32_t pos, len, saved; int marked; } pod_play;   /* the episode playing: kept on the card
                                                                                 (marked: Mark as played on it, which its saves keep) */
    uint32_t pod_auto_at;
    pod_chapter_t pod_ch[64]; int pod_nch;    /* the episode playing's chapters */
    char pod_ch_key[13];
    int pod_cover;                            /* Now Playing shows the show's cover */
    lv_obj_t *thp, *thp_in, *thp_hd, *thp_bat, *thp_mode[2], *thp_sel, *thp_notch[4], *thp_cur, *thp_sel_t,
             *thp_sel_m, *thp_dot, *thp_eq[3], *thp_row_t, *thp_row_m, *thp_track, *thp_fill, *thp_seg[10],
             *thp_warn;                       /* its preview over the list (the_place) */
    int list_y0;
} S;

enum { RD_JOB_NONE, RD_JOB_LIST, RD_JOB_RANDOM };
enum { RD_IDLE, RD_FAILED };
enum { RD_MENU_SEARCH, RD_MENU_RANDOM, RD_MENU_POPULAR, RD_MENU_GENRES, RD_MENU_COUNTRIES, RD_MENU_N };
static const char *const RD_MENU[] = {"Search by name", "Random station", "Popular", "Genres", "Countries"};

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

/* fmt with each %s filled in, a then b (a translated text's own order of
 * words: only %s, never a printf format from a language file) */
static void subst(char *out, size_t cap, const char *fmt, const char *a, const char *b)
{
    lang_fill(out, cap, fmt, a, b);
}

/* A translated text with a number in it: tr_num(b, n, "%s songs", 12) */
static void tr_num(char *out, size_t cap, const char *fmt_en, long v)
{
    char num[24];
    snprintf(num, sizeof num, "%ld", v);
    subst(out, cap, tr(fmt_en), num, NULL);
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
           k == L_YEAR_ALBUMS || k == L_FOLDER || k == L_SMART_TRACKS;
}

/* Lists of songs (play from the chosen one) and lists of albums (open one). */
static int is_track_list(list_kind_t k)
{
    return k == L_TRACKS || k == L_SONGS || k == L_PLAYLIST_TRACKS || k == L_FAVOURITES ||
           k == L_RECENT_PLAYED || k == L_MOST_PLAYED || k == L_CREDIT_TRACKS || k == L_COMPOSER_TRACKS ||
           k == L_SMART_TRACKS;
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
    case L_SMART_TRACKS: return LIB_SMART;
    default: return LIB_SONGS;
    }
}

/* ---------- Home items ---------- */

/* A newer Fathom the update check found, not ignored: its tag ("v0.11.0"), or NULL. */
static const char *update_offered(void)
{
    if (!S.hooks.update_do) return NULL;
    const char *found = settings_get("update.found", "");
    return found[0] && strcmp(found, settings_get("update.ignored", "")) ? found : NULL;
}

static void home_rebuild(void)
{
    S.home_n = 0;
    if (update_offered()) S.home[S.home_n++] = H_UPDATE;
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

/* The update entry came or went (a check found one, it was installed or
 * ignored): Home again, its selection on the same item. */
static void home_sync(void)
{
    int has = S.home_n > 0 && S.home[0] == H_UPDATE;
    if (has == (update_offered() != NULL)) return;
    level_t *h = &S.stack[0];
    int was = h->sel < S.home_n ? S.home[h->sel] : -1;
    home_rebuild();
    int sel = 0;
    for (int i = 0; i < S.home_n; i++)
        if (S.home[i] == was) sel = i;
    h->sel = sel;
    if (h->top > sel) h->top = sel;
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
static void about_look(void)
{
    if (S.about_at && lv_tick_elaps(S.about_at) < 1000) return;
    S.about_at = lv_tick_get() | 1;
    S.about_n = 0;
    while (S.hooks.about && S.about_n < ABOUT_MAX &&
           S.hooks.about(S.about_n, S.about_lbl[S.about_n], sizeof S.about_lbl[0],
                         S.about_val[S.about_n], sizeof S.about_val[0]) == 0)
        S.about_n++;
}

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

static void update_look(void)
{
    memset(&S.upd, 0, sizeof S.upd);
    if (S.hooks.update_state) S.hooks.update_state(&S.upd);
}

static void uninstall_look(void)
{
    memset(&S.uni, 0, sizeof S.uni);
    if (S.hooks.uninstall_state) S.hooks.uninstall_state(&S.uni);
}

/* About's rows: the device's, then Check for updates and Check automatically
 * (with updates), Uninstall Fathom (when it can), Reset all settings. */
enum { AB_INFO, AB_WHATSNEW, AB_CHECK, AB_AUTO, AB_UNINSTALL, AB_RESET };
static int about_row(int i)
{
    if (i < S.about_n) return AB_INFO;
    i -= S.about_n;
    if (S.wn_text[0] && i-- == 0) return AB_WHATSNEW;   /* (a release's: not in builds of my own) */
    if (S.hooks.update_do) {
        if (i < 2) return i == 0 ? AB_CHECK : AB_AUTO;
        i -= 2;
    }
    if (S.hooks.uninstall_do && i-- == 0) return AB_UNINSTALL;
    return AB_RESET;
}

/* Uninstall Fathom's rows, by its state: what it found, then what to do. */
enum { UR_STATUS, UR_LOOK, UR_VERSION, UR_GO };
static int uninstall_rows(void)
{
    switch (S.uni.state) {
    case UI_UNI_FAILED: return 2;               /* why, look again */
    case UI_UNI_READY: return 2;                /* HiBy's release, uninstall */
    default: return 1;                          /* checking, getting ready... */
    }
}
static int uninstall_row(int i)
{
    if (S.uni.state == UI_UNI_FAILED) return i == 0 ? UR_STATUS : UR_LOOK;
    if (S.uni.state == UI_UNI_READY) return i == 0 ? UR_VERSION : UR_GO;
    return S.uni.state == UI_UNI_IDLE ? UR_LOOK : UR_STATUS;
}

static int sound_rows(setting_t *r);
/* Settings > Wi-Fi's rows: on/off, then (while on) the connection, search
 * and the networks in range (*net: which). */
enum { WR_TOGGLE, WR_LINK, WR_SEARCH, WR_NET };
static int wifi_row(int i, int *net)
{
    *net = -1;
    if (i == 0) return WR_TOGGLE;
    if (i <= 2) return i == 1 ? WR_LINK : WR_SEARCH;
    *net = i - 3;
    return WR_NET;
}

static int the_rows(void);
static int list_count(const level_t *l)
{
    if (library_backed(l->kind)) return l->items.n;
    switch (l->kind) {
    case L_HOME: home_sync(); return S.home_n;
    case L_UPD_ASK: return 2;                      /* install, ignore */
    case L_HOME_EDIT: return H_N;
    case L_USB_ASK: return 3;
    case L_WIFI_ASK: return 2;
    case L_SETTINGS: return (int)(sizeof SETTINGS / sizeof SETTINGS[0]);
    case L_BLUETOOTH: bt_look(); return S.bt_on ? BT_DEV_ROW + S.bt_n : 1;
    case L_WIFI: wifi_look(); return S.wifi_on ? 3 + S.wifi_n : 1;   /* see wifi_row */
    case L_WIFI_PASS: return 1;
    case L_SHARE: share_look(); return !S.share_on ? 1 : S.share.state == UI_SHARE_ON ? 7 : 2;   /* on/off, then how to reach it */
    case L_SSH: ssh_look(); return !S.ssh_on ? 1 : S.ssh.state == UI_SSH_ON ? 6 : 2;
    case L_HISTORY: return (int)(sizeof HISTORY / sizeof HISTORY[0]);
    case L_RADIO: return 1 + (S.radio_n > 0 ? S.radio_n : 0);   /* Find stations, yours */
    case L_RDIR: return RD_MENU_N;
    case L_RDIR_GENRES: return RDIR_N_GENRES;
    case L_RDIR_COUNTRIES: return S.rd_cty_n;
    case L_RDIR_RESULTS: return S.rd_n > 0 ? 1 + S.rd_n : 0;   /* Random station, the stations */
    case L_RDIR_SEARCH:   /* typing; the stations, or a line saying how the search goes */
        return 1 + (S.rd_n > 0 ? S.rd_n : S.rd_job == RD_JOB_LIST || S.rd_state == RD_FAILED || S.rd_n == 0 ? 1 : 0);
    case L_THEMES: return S.n_themes + (S.hooks.theme_save != NULL);   /* the themes, New theme */
    case L_PODCASTS: return 1 + (S.pod_n ? S.pod_n : !S.pod_opml[0]) + (S.pod_opml[0] != '\0');   /* Find podcasts, the shows (or how to add some), Import */
    case L_POD_SHOW: return S.pod_ne > 0 ? S.pod_ne : 1;                /* (or a line saying why there are none) */
    case L_POD_SEARCH: return 1 + (S.pod_nf > 0 ? S.pod_nf : S.pod_search != -1 ? 1 : 0);
    case L_POD_CHAPTERS: return S.pod_nch;
    case L_THEME_EDIT: return the_rows();
    case L_THEME_COLOURS: return TC_N;
    case L_EQ: return 1 + S.eq_n + 1;              /* Off, the profiles, New EQ */
    case L_EQ_EDIT: return 1 + S.eqe.n + (S.eqe.n < EQ_MAX_BANDS) + 1;   /* preamp, bands, add, delete */
    case L_EQ_BAND: return 5;                      /* type, frequency, gain, Q, remove */
    case L_QUICK: bt_look(); wifi_look(); return quick_n();
    case L_USB: return (int)(sizeof USB_ROWS / sizeof USB_ROWS[0]);
    case L_DISPLAY: return (int)(sizeof DISPLAY_ROWS / sizeof DISPLAY_ROWS[0]);
    case L_INTERFACE: return (int)(sizeof INTERFACE_ROWS / sizeof INTERFACE_ROWS[0]);
    case L_SOUND: {
        setting_t r[sizeof SOUND_ROWS / sizeof SOUND_ROWS[0]];
        return sound_rows(r);
    }
    case L_PLAYBACK: return (int)(sizeof PLAYBACK_ROWS / sizeof PLAYBACK_ROWS[0]);
    case L_POWER: return (int)(sizeof POWER_ROWS / sizeof POWER_ROWS[0]);
    case L_POD_SETTINGS: return (int)(sizeof POD_ROWS / sizeof POD_ROWS[0]);
    case L_LIBRARY: return (int)(sizeof LIBRARY_ROWS / sizeof LIBRARY_ROWS[0]);
    case L_SIGNAL: {
        int n = 0;
        char a[8], b[8];
        while (S.hooks.signal_path && n < 16 && S.hooks.signal_path(n, a, sizeof a, b, sizeof b) == 0) n++;
        return n;
    }
    case L_ABOUT:
        about_look();
        return S.about_n + (S.wn_text[0] != 0) + 2 * (S.hooks.update_do != NULL) + (S.hooks.uninstall_do != NULL) + 1;   /* about_row */
    case L_UNINSTALL: uninstall_look(); return uninstall_rows();
    case L_UPDATE: update_look(); return S.upd.state == UI_UPD_AVAILABLE || S.upd.state == UI_UPD_FAILED ? 2 : 1;
    case L_BATTERY: return 3;                      /* the graph's range, since full charge, history */
    case L_BAT_SPAN: return 2 + BL_ACTS;            /* on battery, at this rate, each activity */
    case L_BAT_HISTORY: return S.bat.nspans ? S.bat.nspans + 1 : 0;   /* + the average */
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
        if (QUICK[i] == ST_BT && !S.hooks.bt_state) continue;
        if (QUICK[i] == ST_WIFI && !S.hooks.wifi_state) continue;
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
        if ((st == ST_BALANCE || st == ST_VOL_LIMIT || st == ST_CROSSFEED) && !S.hooks.sound_value) continue;
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
    case L_POD_SETTINGS: return POD_ROWS[i];
    case L_LIBRARY: return LIBRARY_ROWS[i];
    case L_INTERFACE: return INTERFACE_ROWS[i];
    default: return DISPLAY_ROWS[i];
    }
}

static int is_setting_level(list_kind_t k)
{
    return k == L_QUICK || k == L_DISPLAY || k == L_SOUND || k == L_PLAYBACK || k == L_POWER || k == L_LIBRARY || k == L_POD_SETTINGS ||
           k == L_USB || k == L_INTERFACE;
}

static const char *on_off(int v) { return tr(v ? "On" : "Off"); }

/* Settings > Interface > Artist names in Albums: each album's artist after its
 * name (on), or its year on the right (off). */
static int album_artists_on(void) { return settings_get_int("display.album_artists", 1) != 0; }

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
        snprintf(main, mn, "%s %s", tr(EQ_TYPE_NAME[b->type]), hz);
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
    case 0: snprintf(main, mn, "Type"); snprintf(v, sizeof v, "%s", tr(EQ_TYPE_NAME[bd->type])); break;
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

static void push_as(list_kind_t kind, const char *title, int64_t parent, int ui);
/* A level titled with a name (an album, an artist, a device) */
static void push(list_kind_t kind, const char *title, int64_t parent) { push_as(kind, title, parent, 0); }
/* A level titled with the UI's own words ("Albums", "Settings"): translated */
static void push_ui(list_kind_t kind, const char *title, int64_t parent) { push_as(kind, title, parent, 1); }
static void about_look(void);
enum { WT_NONE, WT_OK, WT_RDIR };   /* once Wi-Fi connects: nothing, OK again, ask the directory again */
static int wifi_ask_if_off(int then);
static void open_selected(void);

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

/* A setting's row, translated here (its list isn't translated whole: a
 * network's, a device's or an EQ's name stays as it is) */
static void pod_speed_text(int pct, char *out, size_t cap);

static void setting_row(setting_t st, int editing, char *main, size_t mn, char *meta, size_t tn)
{
    switch (st) {
    case ST_BRIGHTNESS:
        snprintf(main, mn, "%s", tr("Brightness"));
        snprintf(meta, tn, editing ? "< %d / %d >" : "%d / %d", brightness(), UI_BRIGHTNESS_LEVELS);
        break;
    case ST_SHUFFLE:
        snprintf(main, mn, "%s", tr("Shuffle"));
        snprintf(meta, tn, "%s", on_off(S.player && player_shuffle(S.player)));
        break;
    case ST_REPEAT: {
        static const char *const R[] = {"Off", "All", "One"};
        snprintf(main, mn, "%s", tr("Repeat"));
        snprintf(meta, tn, "%s", tr(R[S.player ? player_repeat(S.player) : 0]));
        break;
    }
    case ST_SLEEP:
        snprintf(main, mn, "%s", tr("Sleep timer"));
        if (S.player && player_sleep_minutes(S.player) == PLAYER_SLEEP_END_ALBUM) snprintf(meta, tn, "%s", tr("End of album"));
        else if (S.now.sleep_s > 0) tr_num(meta, tn, "%s min left", (S.now.sleep_s + 59) / 60);
        else snprintf(meta, tn, "%s", tr("Off"));
        break;
    case ST_STOP_AFTER:
        snprintf(main, mn, "%s", tr("Stop after this track"));
        snprintf(meta, tn, "%s", on_off(S.player && player_stop_after(S.player)));
        break;
    case ST_DIM:
        snprintf(main, mn, "%s", tr("Dim before turning off"));
        snprintf(meta, tn, "%s", on_off(settings_get_int("display.dim", UI_DIM_DEFAULT)));
        break;
    case ST_ON_CHARGING:
        snprintf(main, mn, "%s", tr("Stay on while charging"));
        snprintf(meta, tn, "%s", on_off(settings_get_int("display.on_charging", UI_ON_CHARGING_DEFAULT)));
        break;
    case ST_THEME:
        snprintf(main, mn, "%s", tr("Theme"));
        snprintf(meta, tn, "%.*s", (int)tn - 1, S.cur_theme < S.n_themes ? S.themes[S.cur_theme].name : "");
        break;
    case ST_ALBUM_ARTISTS:
        snprintf(main, mn, "%s", tr("Artist names in Albums"));
        snprintf(meta, tn, "%s", on_off(album_artists_on()));
        break;
    case ST_COVERS: {
        static const char *const W[] = {"Off", "Albums", "Songs", "Albums, songs"};
        snprintf(main, mn, "%s", tr("Covers in lists"));
        snprintf(meta, tn, "%s", tr(W[settings_get_int("display.covers", UI_COVERS_DEFAULT) & 3]));
        break;
    }
    case ST_OUTPUT:
        snprintf(main, mn, "%s", tr("Output"));
        snprintf(meta, tn, "%.*s", (int)tn - 1, S.output[0] ? tr(S.output) : "-");   /* long names are cut; (Headphones, Line out: translated) */
        break;
    case ST_LEVELLING: {
        static const char *const how[] = {"Off", "Track", "Album"};
        int v = settings_get_int("sound.levelling", LEVEL_OFF);
        snprintf(main, mn, "%s", tr("Volume levelling"));
        snprintf(meta, tn, "%s", tr(S.pure ? "Bypassed" : how[v >= 0 && v <= 2 ? v : 0]));
        break;
    }
    case ST_EQ:
        snprintf(main, mn, "%s", tr("EQ"));
        snprintf(meta, tn, "%.*s", (int)tn - 1, S.pure ? tr("Bypassed") : S.eq_name[0] ? S.eq_name : tr("Off"));
        break;
    case ST_LANGUAGE: {
        const char *lang = settings_get("display.language", "");
        snprintf(main, mn, "%s", tr("Language"));
        snprintf(meta, tn, "%.*s", (int)tn - 1, lang[0] ? lang : "English");
        break;
    }
    case ST_BATTERY:
        snprintf(main, mn, "%s", tr("Battery"));
        if (S.hooks.battery_info) S.hooks.battery_info(meta, tn);
        else snprintf(meta, tn, "-");
        break;
    case ST_PURE:
        snprintf(main, mn, "%s", tr("Pure mode"));
        snprintf(meta, tn, "%s", on_off(S.pure));
        break;
    case ST_TRANSPORT:
        snprintf(main, mn, "%s", tr("Transport mode"));
        snprintf(meta, tn, "%s", on_off(settings_get_int("sound.transport", UI_TRANSPORT_DEFAULT)));
        break;
    case ST_CROSSFEED: {
        char v[48] = "";
        snprintf(main, mn, "%s", tr("Crossfeed"));
        if (S.pure) { snprintf(meta, tn, "%s", tr("Bypassed")); break; }
        if (S.hooks.sound_value) S.hooks.sound_value(UI_SOUND_CROSSFEED, v, sizeof v);
        snprintf(meta, tn, "%s", v);
        break;
    }
    case ST_BALANCE:
    case ST_VOL_LIMIT: {
        char v[48] = "";
        snprintf(main, mn, "%s", tr(st == ST_BALANCE ? "Balance" : "Volume limit"));
        if (st == ST_BALANCE && S.pure) { snprintf(meta, tn, "%s", tr("Bypassed")); break; }
        if (S.hooks.sound_value) S.hooks.sound_value(st == ST_BALANCE ? UI_SOUND_BALANCE : UI_SOUND_LIMIT, v, sizeof v);
        snprintf(meta, tn, editing ? "< %s >" : "%s", v);
        break;
    }
    case ST_DAC_FILTER:
        snprintf(main, mn, "%s", tr("DAC filter"));
        if (S.hooks.dac_filter_name) S.hooks.dac_filter_name(meta, tn);
        break;
    case ST_SHUF_BY:
        snprintf(main, mn, "%s", tr("Shuffle by"));
        snprintf(meta, tn, "%s", tr(settings_get_int("shuffle.albums", UI_SHUF_ALBUMS_DEFAULT) ? "Albums" : "Songs"));
        break;
    case ST_SHUF_FRESH:
        snprintf(main, mn, "%s", tr("Favour unheard songs"));
        snprintf(meta, tn, "%s", on_off(settings_get_int("shuffle.freshness", UI_SHUF_FRESH_DEFAULT)));
        break;
    case ST_SHUF_INTERLUDES:
        snprintf(main, mn, "%s", tr("Skip short interludes"));
        snprintf(meta, tn, "%s", on_off(settings_get_int("shuffle.skip_interludes", UI_SHUF_INTERLUDES_DEFAULT)));
        break;
    case ST_SHUF_WORKS:
        snprintf(main, mn, "%s", tr("Keep works together"));
        snprintf(meta, tn, "%s", on_off(settings_get_int("shuffle.keep_works", UI_SHUF_WORKS_DEFAULT)));
        break;
    case ST_SHUF_SPOKEN:
        snprintf(main, mn, "%s", tr("Leave out audiobooks"));
        snprintf(meta, tn, "%s", on_off(settings_get_int("shuffle.exclude_spoken", UI_EXCLUDE_SPOKEN_DEFAULT)));
        break;
    case ST_PAUSE_UNPLUG:
        snprintf(main, mn, "%s", tr("Pause on unplug"));
        snprintf(meta, tn, "%s", on_off(settings_get_int("playback.pause_on_unplug", UI_PAUSE_ON_UNPLUG_DEFAULT)));
        break;
    case ST_AUTO_OFF: {
        int m = settings_get_int("power.auto_off", UI_AUTO_OFF_DEFAULT);
        snprintf(main, mn, "%s", tr("Power off when idle"));
        if (m <= 0) snprintf(meta, tn, "%s", tr("Never"));
        else tr_num(meta, tn, "%s min", m);
        break;
    }
    case ST_POD_AUTO:
        snprintf(main, mn, "%s", tr("New episodes"));
        snprintf(meta, tn, "%s", tr(settings_get_int("podcasts.auto", 1) ? "When charging" : "Off"));
        break;
    case ST_POD_KEEP:
        snprintf(main, mn, "%s", tr("Keep per show"));
        snprintf(meta, tn, "%d", settings_get_int("podcasts.keep", 2));
        break;
    case ST_POD_CLEAN: {
        int d = settings_get_int("podcasts.clean_days", 7);
        snprintf(main, mn, "%s", tr("Delete played"));
        if (d <= 0) snprintf(meta, tn, "%s", tr("Never"));
        else if (d == 1) snprintf(meta, tn, "%s", tr("After a day"));
        else tr_num(meta, tn, "After %s days", d);
        break;
    }
    case ST_POD_SPEED:
        snprintf(main, mn, "%s", tr("Speed"));
        pod_speed_text(S.player ? player_speed(S.player) : settings_get_int("podcasts.speed", 100), meta, tn);
        break;
    case ST_RESCAN: snprintf(main, mn, "%s", tr("Rescan the music folder")); break;
    case ST_FIND_COVERS: {
        cf_batch_t b;
        cf_batch_status(&b);
        snprintf(main, mn, "%s", tr(b.running ? "Stop finding covers" : "Find missing covers"));
        if (b.running) snprintf(meta, tn, "%d / %d", b.done, b.total);
        break;
    }
    case ST_USB_DRIVE: snprintf(main, mn, "%s", tr("USB drive mode")); break;
    case ST_USB_COMPUTER: {
        static const char *const what[] = {"Ask", "USB drive", "Just charge", "USB DAC"};
        int v = settings_get_int("usb.computer", UI_USB_ASK);
        snprintf(main, mn, "%s", tr("On a computer"));
        snprintf(meta, tn, "%s", tr(what[v >= 0 && v <= 3 ? v : 0]));
        break;
    }
    case ST_USB_DAC: snprintf(main, mn, "%s", tr("USB DAC mode")); break;
    case ST_BT: {               /* quick settings: what's connected, or on/off; opens the page */
        snprintf(main, mn, "%s", tr("Bluetooth"));
        const char *c = NULL;
        for (int d = 0; d < S.bt_n; d++)
            if (S.bt[d].connected) c = S.bt[d].name[0] ? S.bt[d].name : S.bt[d].addr;
        snprintf(meta, tn, "%s", S.bt_busy[0] && strcmp(S.bt_busy, "Searching") ? tr(S.bt_busy)
                                 : c && S.bt_on ? c : on_off(S.bt_on));
        break;
    }
    case ST_WIFI:               /* the same for Wi-Fi: the network joined, or on/off */
        snprintf(main, mn, "%s", tr("Wi-Fi"));
        if (S.wifi_busy[0] && strcmp(S.wifi_busy, "Searching")) snprintf(meta, tn, "%s", tr(S.wifi_busy));
        else if (S.wifi_on && S.wifi_st.state == UI_WIFI_CONNECTED) snprintf(meta, tn, "%.*s", (int)tn - 1, S.wifi_st.ssid);
        else snprintf(meta, tn, "%s", on_off(S.wifi_on));
        break;
    case ST_N_SONGS: snprintf(main, mn, "%s", tr("Songs")); snprintf(meta, tn, "%d", S.lib_counts[0]); break;
    case ST_N_ALBUMS: snprintf(main, mn, "%s", tr("Albums")); snprintf(meta, tn, "%d", S.lib_counts[1]); break;
    case ST_N_ARTISTS: snprintf(main, mn, "%s", tr("Artists")); snprintf(meta, tn, "%d", S.lib_counts[2]); break;
    case ST_N_PLAYLISTS: snprintf(main, mn, "%s", tr("Playlists")); snprintf(meta, tn, "%d", S.lib_counts[3]); break;
    case ST_TIMEOUT: {
        int t = screen_timeout();
        snprintf(main, mn, "%s", tr("Screen off after"));
        if (t <= 0) snprintf(meta, tn, "%s", tr("Never"));
        else if (t < 60) tr_num(meta, tn, "%s s", t);
        else tr_num(meta, tn, "%s min", t / 60);
        break;
    }
    }
}

static void info_row(int64_t track, int i, char *main, size_t mn, char *meta, size_t tn)
{
    static const char *const LABEL[INFO_N] = {"Title", "Artist", "Album", "Track", "Format", "Length", "File"};
    snprintf(meta, tn, "%s", tr(LABEL[i]));   /* (the song's own words in main: never translated) */
    lib_track_t t;
    if (!S.lib || library_track(S.lib, track, &t)) { snprintf(main, mn, "-"); return; }
    switch (i) {
    case INFO_TITLE: snprintf(main, mn, "%s", t.title); break;
    case INFO_ARTIST: snprintf(main, mn, "%s", t.artist); break;
    case INFO_ALBUM: snprintf(main, mn, "%s", t.album); break;
    case INFO_TRACK:
        if (t.disc > 1) {
            char d[16], n[16];
            snprintf(d, sizeof d, "%d", t.disc);
            snprintf(n, sizeof n, "%d", t.track);
            subst(main, mn, tr("Disc %s, track %s"), d, n);
        }
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
#define WHEEL_CAP 48           /* a wheel word (Results, space), translated */
#define DOUBLE_CLICK_MS 350   /* OK twice within this: Results (Search) or Done (Rename) */

static const char *entry_letters(list_kind_t k)
{
    return k == L_SEARCH || k == L_RDIR_SEARCH || k == L_POD_SEARCH ? SEARCH_CHARS : k == L_WIFI_PASS ? PASS_CHARS : NAME_CHARS;
}

static void entry_glyph(list_kind_t k, int i, char *out, size_t cap)
{
    const char *letters = entry_letters(k);
    int n = (int)strlen(letters);
    i = ((i % (n + 1)) + n + 1) % (n + 1);
    if (i == n) snprintf(out, cap, "%s", tr(k == L_SEARCH ? "Results" : k == L_RDIR_SEARCH || k == L_POD_SEARCH ? "Search"
                                           : k == L_WIFI_PASS ? "Join" : "Done"));
    else if (letters[i] == ' ') snprintf(out, cap, "%s", tr("space"));
    else if (k == L_WIFI_PASS) snprintf(out, cap, "%c", letters[i]);   /* exactly what's typed */
    else snprintf(out, cap, "%c", letters[i] >= 'a' && letters[i] <= 'z' ? letters[i] - 32 : letters[i]);
}

/* The typing row: what's typed so far, and the wheel's letter with its
 * neighbours ("d E f", the wheel's letter drawn larger). A word (Results,
 * Done, space) is shown alone, with no neighbours. */
static void entry_row(list_kind_t k, char *main, size_t mn, char wheel[3][WHEEL_CAP])
{
    if ((k == L_SEARCH || k == L_RDIR_SEARCH || k == L_POD_SEARCH) && !S.entry[0]) snprintf(main, mn, "%s", tr("Type with the wheel"));
    else if (k == L_WIFI_PASS && !S.entry[0]) snprintf(main, mn, "%s", tr("Password"));
    else snprintf(main, mn, "%s_", S.entry);
    entry_glyph(k, S.entry_ch - 1, wheel[0], WHEEL_CAP);
    entry_glyph(k, S.entry_ch, wheel[1], WHEEL_CAP);
    entry_glyph(k, S.entry_ch + 1, wheel[2], WHEEL_CAP);
    /* the neighbours in lower case, so the letter OK adds stands out */
    for (int w = 0; w < 3; w += 2) {
        if (strlen(wheel[1]) > 1 || strlen(wheel[w]) > 1) wheel[w][0] = '\0';
        else if (k != L_WIFI_PASS && wheel[w][0] >= 'A' && wheel[w][0] <= 'Z') wheel[w][0] += 32;
    }
}

static int is_typing_row(const level_t *l, int i)
{
    return (l->kind == L_SEARCH || l->kind == L_RENAME || l->kind == L_WIFI_PASS || l->kind == L_RDIR_SEARCH ||
            l->kind == L_POD_SEARCH) && i == 0;
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
    if (kind == 0) { library_row(S.lib, LIB_ARTISTS, id, main, mn, meta, tn); snprintf(meta, tn, "%s", tr("Artist")); }
    else if (kind == 1) { library_row(S.lib, LIB_ALBUMS, id, main, mn, meta, tn); snprintf(meta, tn, "%s", tr("Album")); }
    else library_row(S.lib, LIB_PLAYLIST_TRACKS, id, main, mn, meta, tn);
}

static void search_run(void)
{
    library_search_free(&S.found);
    if (S.lib && S.entry[0]) library_search(S.lib, S.entry, &S.found);
}

/* ---------- the station directory: what the lists show ---------- */

/* This station is the one playing (or connecting). */
static int rd_on_air(const radio_station_t *st)
{
    const radio_station_t *cur = S.now.radio && S.now.playing && S.player ? player_radio_station(S.player) : NULL;
    return cur && !strcmp(cur->url, st->url);
}

/* What Random station plays from (settings key radio.random: "tag:jazz:Jazz",
 * "country:GB:United Kingdom", or "" for any station): 1 and its name (and
 * *q, may be NULL) for a genre or a country, 0 for any. */
static int rd_random_filter(rdir_query_t *q, char *name, size_t cap)
{
    const char *v = settings_get("radio.random", "");
    const char *a = strchr(v, ':'), *b = a ? strchr(a + 1, ':') : NULL;
    rdir_by_t by = a && a - v == 3 && !strncmp(v, "tag", 3) ? RDIR_TAG
                 : a && a - v == 7 && !strncmp(v, "country", 7) ? RDIR_COUNTRY : RDIR_POPULAR;
    if (q) *q = (rdir_query_t){.by = RDIR_POPULAR, .random = 1};
    if (by == RDIR_POPULAR || !b || b == a + 1 || !b[1]) return 0;
    if (q) {
        q->by = by;
        snprintf(q->arg, sizeof q->arg, "%.*s", (int)(b - a - 1), a + 1);
    }
    snprintf(name, cap, "%s", b + 1);
    return 1;
}

/* ---------- updates (Settings > About > Check for updates) ---------- */

static void fmt_mb(char *out, size_t cap, uint64_t bytes) { snprintf(out, cap, "%.1f MB", bytes / 1048576.0); }

/* The running version, short: a development build's "v0.7.0-11-g177904b" as
 * "v0.7.0" (its full name would push the row's text off the screen). */
static void short_version(char *out, size_t cap, const char *v)
{
    size_t n = strcspn(v, "-");
    snprintf(out, cap, "%.*s", (int)n, v);
}

/* Why an update failed, translated: the text, or the space it needs */
static void update_failure(char *out, size_t cap)
{
    if (S.upd.need_mb > 0) tr_num(out, cap, "Not enough space on the card: %s MB needed", S.upd.need_mb);
    else snprintf(out, cap, "%s", tr(S.upd.message));
}

static void update_row(int i, char *main, size_t mn, char *meta, size_t tn)
{
    const ui_update_t *u = &S.upd;
    switch (u->state) {
    case UI_UPD_IDLE: snprintf(main, mn, "Check for updates"); break;
    case UI_UPD_CHECKING: snprintf(main, mn, "Checking for updates"); break;
    case UI_UPD_LATEST:
        snprintf(main, mn, "Fathom is up to date");
        short_version(meta, tn, u->current);
        break;
    case UI_UPD_AVAILABLE:
        if (i == 0) { subst(main, mn, tr("Update to %s"), u->latest, NULL); fmt_mb(meta, tn, (uint64_t)u->size); }
        else { snprintf(main, mn, "This version"); short_version(meta, tn, u->current); }
        break;
    case UI_UPD_FAILED:
        if (i == 0) update_failure(main, mn);
        else snprintf(main, mn, "Try again");
        break;
    default: subst(main, mn, tr("Updating to %s"), u->latest, NULL); break;   /* (under the progress screen) */
    }
}

/* Why Uninstall can't go on, translated: the text, or the space it needs */
static void uninstall_failure(char *out, size_t cap)
{
    if (S.uni.need_mb > 0) tr_num(out, cap, "Not enough space on the card: %s MB needed", S.uni.need_mb);
    else snprintf(out, cap, "%s", tr(S.uni.message));
}

static void uninstall_row_text(int i, char *main, size_t mn, char *meta, size_t tn)
{
    const ui_uninstall_t *u = &S.uni;
    switch (uninstall_row(i)) {
    case UR_LOOK: snprintf(main, mn, "%s", u->state == UI_UNI_IDLE ? "Look for HiBy's update file" : "Look again"); break;
    case UR_VERSION:
        snprintf(main, mn, "HiBy's update");
        snprintf(meta, tn, "%s", u->ver);
        break;
    case UR_GO: snprintf(main, mn, "Uninstall Fathom"); break;
    default:
        if (u->state == UI_UNI_FAILED) { uninstall_failure(main, mn); break; }
        snprintf(main, mn, "%s", u->state == UI_UNI_CHECKING ? "Checking the update file"
                                 : u->state == UI_UNI_APPLYING ? "Getting the update ready"
                                 : "Ready to uninstall");
        if (u->total && u->state != UI_UNI_PENDING)
            snprintf(meta, tn, "%d %%", (int)(u->done * 100 / u->total));
        break;
    }
}


/* ---------- battery history (Settings > Power > Battery) ---------- */

static void style_label(lv_obj_t *l, const lv_font_t *font, uint32_t color);
static void rows_box_place(void);

#define BAT_GRAPH_H 90          /* the graph above the rows (three fit below it) */
#define BAT_GX0 34              /* the plot: a column a pixel, 0-100 % between these */
#define BAT_GY0 6
#define BAT_GY1 64

static const char *const BAT_ACTS[BL_ACTS] = {"Playing", "Screen on", "Paused"};   /* (playing and paused: the screen off) */

static void fmt_dur(char *b, size_t n, int64_t secs)
{
    long m = (long)((secs + 30) / 60);
    if (m < 60) tr_num(b, n, "%s min", m);
    else if (m % 60 == 0) tr_num(b, n, "%s h", m / 60);
    else {
        char h[16], mm[16];
        snprintf(h, sizeof h, "%ld", m / 60);
        snprintf(mm, sizeof mm, "%02ld", m % 60);
        subst(b, n, tr("%s h %s min"), h, mm);
    }
}

static void fmt_full(char *b, size_t n, int h10)
{
    if (h10 > 0) tr_num(b, n, "about %s h from full", (h10 + 5) / 10);
    else snprintf(b, n, "\xe2\x80\x93");             /* (too little to say) */
}

/* A span's day: Today, Yesterday, 16 Nov (UTC, as About's clock); without a
 * clock ever set, how many days back */
static void bat_day(char *b, size_t n, int64_t t)
{
    int64_t now = S.bat.t1;
    if (!S.bat.clock_ok) {
        long d = (long)((now - t) / 86400);
        if (d <= 0) snprintf(b, n, "%s", tr("Today"));
        else if (d == 1) snprintf(b, n, "%s", tr("Yesterday"));
        else tr_num(b, n, "%s days ago", d);
        return;
    }
    long dn = (long)(now / 86400), dt = (long)(t / 86400);
    if (dt == dn) { snprintf(b, n, "%s", tr("Today")); return; }
    if (dt == dn - 1) { snprintf(b, n, "%s", tr("Yesterday")); return; }
    static const char *const MON[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    time_t tt = (time_t)t;
    struct tm tm;
    gmtime_r(&tt, &tm);
    char day[8];
    snprintf(day, sizeof day, "%d", tm.tm_mday);
    snprintf(b, n, "%s %s", day, tr(MON[tm.tm_mon]));
}

static const batspan_t *bat_cur(void)
{
    if (S.bat_span < 0) return S.bat.have_since ? &S.bat.since : NULL;
    return S.bat_span < S.bat.nspans ? &S.bat.span[S.bat_span] : NULL;
}

/* The first of the metas (longest first, NULL-ended) that fits beside main
 * in this theme's fonts (the pixel one is wide) */
static void bat_meta(const char *main, char *meta, size_t tn, const char *const *alts)
{
    int room = UI_W - 2 * ROW_MARGIN - 2 * 12 - 16;
    lv_point_t a, b;
    lv_text_get_size(&a, tr(main), S.f->body, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    int i = 0;
    for (; alts[i + 1]; i++) {
        lv_text_get_size(&b, alts[i], S.f->meta, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        if (a.x + b.x <= room) break;
    }
    snprintf(meta, tn, "%s", alts[i]);
}

static void battery_row(list_kind_t k, int i, char *main, size_t mn, char *meta, size_t tn)
{
    char d[32], m1[64], m2[64], m3[64];
    const char *dash = "\xe2\x80\x93";
    if (k == L_BATTERY) {
        if (i == 0) {
            snprintf(main, mn, "%s", S.bat_days == 7 ? "Last 7 days" : "Last 24 hours");
            snprintf(meta, tn, "%s", S.bat_days == 7 ? "Play: 24 hours" : "Play: 7 days");
        } else if (i == 1) {
            snprintf(main, mn, "Since full charge");
            fmt_dur(d, sizeof d, S.bat.since.on_s);
            snprintf(m1, sizeof m1, "%s, %d %%", d, S.bat.since.used);   /* (d translated) */
            snprintf(m2, sizeof m2, "%d %%", S.bat.since.used);
            const char *alts[] = {m1, m2, NULL};
            if (S.bat.have_since) bat_meta(main, meta, tn, alts);
            else snprintf(meta, tn, "%s", dash);
        } else {
            snprintf(main, mn, "History");
            if (S.bat.total_spans == 1) snprintf(meta, tn, "%s", tr("1 charge"));
            else if (S.bat.total_spans) tr_num(meta, tn, "%s charges", S.bat.total_spans);
            else snprintf(meta, tn, "%s", dash);
        }
        return;
    }
    if (k == L_BAT_HISTORY) {
        if (i < S.bat.nspans) {
            const batspan_t *x = &S.bat.span[i];
            bat_day(main, mn, x->start);
            if (S.bat.clock_ok) {                  /* (two on one day: when each began) */
                time_t tt = (time_t)x->start;
                struct tm tm;
                gmtime_r(&tt, &tm);
                size_t l = strlen(main);
                snprintf(main + l, mn - l, ", %02d:%02d", tm.tm_hour, tm.tm_min);
            }
            fmt_dur(d, sizeof d, x->on_s);
            snprintf(m1, sizeof m1, "%d%s%d %%, %s", x->pct_start, dash, x->pct_end, d);
            snprintf(m2, sizeof m2, "%d%s%d %%", x->pct_start, dash, x->pct_end);
            const char *alts[] = {m1, m2, NULL};
            bat_meta(main, meta, tn, alts);
        } else {
            snprintf(main, mn, "%s", tr("Average"));
            fmt_full(m1, sizeof m1, S.bat.avg_full_h10);
            tr_num(m2, sizeof m2, "%s h", (S.bat.avg_full_h10 + 5) / 10);
            const char *alts[] = {m1, m2, NULL};
            if (S.bat.avg_full_h10 > 0) bat_meta(main, meta, tn, alts);
            else snprintf(meta, tn, "%s", dash);
        }
        return;
    }
    const batspan_t *x = bat_cur();            /* L_BAT_SPAN */
    if (!x) return;
    if (i == 0) {
        snprintf(main, mn, "On battery");
        fmt_dur(d, sizeof d, x->on_s);
        char pc[16];
        snprintf(pc, sizeof pc, "%d %%", x->used);
        subst(m1, sizeof m1, tr("%s, %s used"), d, pc);
        snprintf(m2, sizeof m2, "%s, %d %%", d, x->used);
        snprintf(m3, sizeof m3, "%s", d);
        const char *alts[] = {m1, m2, m3, NULL};
        bat_meta(main, meta, tn, alts);
    } else if (i == 1) {
        snprintf(main, mn, "At this rate");
        fmt_full(m1, sizeof m1, x->full_h10);
        tr_num(m2, sizeof m2, "%s h from full", (x->full_h10 + 5) / 10);
        tr_num(m3, sizeof m3, "%s h", (x->full_h10 + 5) / 10);
        const char *alts[] = {m1, m2, m3, NULL};
        if (x->full_h10 > 0) bat_meta(main, meta, tn, alts);
        else snprintf(meta, tn, "%s", dash);
    } else {
        int a = i - 2;
        snprintf(main, mn, "%s", BAT_ACTS[a]);
        fmt_dur(d, sizeof d, x->act_s[a]);
        char per[32];
        tr_num(per, sizeof per, "%s min", x->min_per_pct[a]);
        snprintf(m1, sizeof m1, "%s, 1 %%/%s", d, per);
        snprintf(m2, sizeof m2, "1 %%/%s", per);
        const char *alts[] = {m1, m2, NULL};
        if (!x->act_s[a]) snprintf(meta, tn, "%s", dash);
        else if (x->min_per_pct[a]) bat_meta(main, meta, tn, alts);
        else snprintf(meta, tn, "%s", d);
    }
}

/* The graph's labels: 100, 50 and 0 down the side; the hours (or days) under
 * it, and now */
static void battery_labels(void)
{
    if (!S.bat_graph) return;
    const theme_t *t = T();
    for (int i = 0; i < 9; i++) {
        style_label(S.bat_lbl[i], S.f->meta, t->text_muted);
        show(S.bat_lbl[i], 0);
    }
    static const char *const Y[3] = {"100", "50", "0"};
    for (int i = 0; i < 3; i++) {
        set_text(S.bat_lbl[i], Y[i]);
        lv_obj_set_style_text_align(S.bat_lbl[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_width(S.bat_lbl[i], BAT_GX0 - 1);
        lv_obj_set_pos(S.bat_lbl[i], 0, BAT_GY0 + i * (BAT_GY1 - BAT_GY0) / 2 - lv_font_get_line_height(S.f->meta) / 2);
        show(S.bat_lbl[i], 1);
    }
    int64_t t0 = S.bat.t0, t1 = S.bat.t1, span = t1 - t0;
    if (span <= 0) return;
    lv_point_t nw;
    lv_text_get_size(&nw, tr("now"), S.f->meta, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    int n = 3, last_x = BAT_GX0 + BATREP_COLS - nw.x - 6;   /* ("now" at the right end) */
    int lbl_y = BAT_GRAPH_H - lv_font_get_line_height(S.f->meta);   /* (the wider fonts are taller) */
    if (lbl_y < BAT_GY1 + 4) lbl_y = BAT_GY1 + 4;   /* (below the charging strip) */
    int64_t step = S.bat_days == 7 ? 86400 : 6 * 3600;
    int64_t first = (t0 / step + 1) * step;
    char b[32];
    for (int64_t tt = first; tt < t1 && n < 8; tt += step) {
        int x = BAT_GX0 + (int)((tt - t0) * BATREP_COLS / span);
        lv_point_t sz;
        lv_text_get_size(&sz, "00:00", S.f->meta, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        if (x + sz.x / 2 + 4 > last_x || x - sz.x / 2 < 2) continue;
        if (S.bat.clock_ok && S.bat_days == 7) {
            static const char *const WD[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
            time_t ttt = (time_t)tt;
            struct tm tm;
            gmtime_r(&ttt, &tm);
            snprintf(b, sizeof b, "%s", tr(WD[tm.tm_wday]));
        } else if (S.bat.clock_ok) {
            snprintf(b, sizeof b, "%02d:00", (int)((tt / 3600) % 24));
        } else {
            char ago[24];
            tr_num(ago, sizeof ago, S.bat_days == 7 ? "%s d" : "%s h",
                   (t1 - tt + step / 2) / (S.bat_days == 7 ? 86400 : 3600));
            snprintf(b, sizeof b, "-%s", ago);
        }
        set_text(S.bat_lbl[n], b);
        lv_obj_set_style_text_align(S.bat_lbl[n], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(S.bat_lbl[n], 64);
        lv_obj_set_pos(S.bat_lbl[n], x - 32, lbl_y);
        show(S.bat_lbl[n], 1);
        n++;
    }
    set_text(S.bat_lbl[8], tr("now"));
    lv_obj_set_style_text_align(S.bat_lbl[8], LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_width(S.bat_lbl[8], 64);
    lv_obj_set_pos(S.bat_lbl[8], BAT_GX0 + BATREP_COLS - 64, lbl_y);
    show(S.bat_lbl[8], 1);
}

static void bat_rect(lv_layer_t *layer, lv_draw_rect_dsc_t *d, int x1, int y1, int x2, int y2)
{
    lv_area_t a = {x1, y1, x2, y2};
    lv_draw_rect(layer, d, &a);
}

/* The graph, drawn whole: the charge a column a pixel (the theme's signal
 * colour on battery, its muted one charging, with a strip under), filled
 * faintly beneath; a dashed line where the H2 was off for a time not known */
static void bat_graph_draw(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(lv_event_get_target(e), &a);
    const theme_t *t = T();
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.radius = 0;
    d.bg_opa = LV_OPA_COVER;
    int x0 = a.x1 + BAT_GX0, y0 = a.y1 + BAT_GY0, y1 = a.y1 + BAT_GY1;
    d.bg_color = C(t->surface);
    for (int g = 0; g < 3; g++) {
        int y = y0 + g * (y1 - y0) / 2;
        bat_rect(layer, &d, x0, y, x0 + BATREP_COLS - 1, y);
    }
    /* runs of columns at the same charge and state: one rectangle each */
    for (int c = 0; c < BATREP_COLS;) {
        int p = S.bat.pct[c], chg = S.bat.charging[c], e2 = c + 1;
        while (e2 < BATREP_COLS && S.bat.pct[e2] == p && S.bat.charging[e2] == chg) e2++;
        if (p >= 0) {
            int y = y1 - p * (y1 - y0) / 100;
            d.bg_color = C(chg ? t->text_muted : t->signal);
            d.bg_opa = LV_OPA_40;
            if (y + 2 <= y1) bat_rect(layer, &d, x0 + c, y + 2, x0 + e2 - 1, y1);
            d.bg_opa = LV_OPA_COVER;
            bat_rect(layer, &d, x0 + c, y, x0 + e2 - 1, y + 1);
            if (chg) bat_rect(layer, &d, x0 + c, y1 + 2, x0 + e2 - 1, y1 + 3);
        }
        c = e2;
    }
    d.bg_color = C(t->text_muted);
    d.bg_opa = LV_OPA_COVER;
    for (int g = 0; g < S.bat.ngaps; g++)
        for (int y = y0; y < y1; y += 4) bat_rect(layer, &d, x0 + S.bat.gap[g], y, x0 + S.bat.gap[g], y + 1);
}

/* The Battery screen's graph over the top of the list (graphs_place moves the list down) */
static int battery_place(list_kind_t k)
{
    int on = k == L_BATTERY && S.hooks.battery_report;
    if (on && !S.bat_graph) {
        S.bat_graph = box(S.list, 0, 0, UI_W, BAT_GRAPH_H);
        lv_obj_add_event_cb(S.bat_graph, bat_graph_draw, LV_EVENT_DRAW_MAIN, NULL);
        for (int i = 0; i < 9; i++) S.bat_lbl[i] = label(S.bat_graph);
    }
    if (S.bat_graph) {
        show(S.bat_graph, on);
        lv_obj_set_style_bg_color(S.bat_graph, C(T()->bg), 0);
        lv_obj_set_style_bg_opa(S.bat_graph, LV_OPA_COVER, 0);
        if (on) { lv_obj_move_foreground(S.bat_graph); battery_labels(); }
    }
    return on;
}

/* ---------- the EQ editor's curve (Settings > Sound > EQ > a profile, a band) ---------- */

static int eqg_x(double f)
{
    if (f < 20) f = 20;
    if (f > 20000) f = 20000;
    return EQG_X0 + (int)lround((log10(f) - log10(20.0)) / 3.0 * (EQG_X1 - EQG_X0));
}

static int eqg_y(double db)
{
    if (db > EQG_RANGE) db = EQG_RANGE;
    if (db < -EQG_RANGE) db = -EQG_RANGE;
    return EQG_Y0 + (int)lround((EQG_RANGE - db) / (2 * EQG_RANGE) * (EQG_Y1 - EQG_Y0));
}

/* The curve at the plot's column x, between the points either side of it */
static double eqg_at(int x)
{
    double pos = (double)x * (EQG_PTS - 1) / (EQG_X1 - EQG_X0);
    int i = (int)pos;
    if (i < 0) return S.eqg_db[0];
    if (i >= EQG_PTS - 1) return S.eqg_db[EQG_PTS - 1];
    double fr = pos - i;
    return S.eqg_db[i] * (1 - fr) + S.eqg_db[i + 1] * fr;
}

static int same_profile(const eq_profile_t *a, const eq_profile_t *b)
{
    if (a->preamp_db != b->preamp_db || a->n != b->n) return 0;
    for (int i = 0; i < a->n; i++)
        if (a->band[i].type != b->band[i].type || a->band[i].fc != b->band[i].fc ||
            a->band[i].gain_db != b->band[i].gain_db || a->band[i].q != b->band[i].q)
            return 0;
    return 1;
}

/* The curve for the profile being edited (worked out again only when it
 * changed), and the warning when, with the preamp, it goes above 0 dB: the
 * loudest moments can clip there. */
static void eqg_update(void)
{
    if (S.eqg_have && same_profile(&S.eqg_for, &S.eqe)) return;
    double f[EQG_PTS];
    for (int i = 0; i < EQG_PTS; i++) f[i] = 20 * pow(1000, (double)i / (EQG_PTS - 1));
    eq_curve_db(&S.eqe, 48000, f, EQG_PTS, S.eqg_db);
    S.eqg_for = S.eqe;
    S.eqg_have = 1;
    double peak = -1e9;
    int at = 0;
    for (int i = 0; i < EQG_PTS; i++)
        if (S.eqg_db[i] > peak) peak = S.eqg_db[i], at = i;
    peak += S.eqe.preamp_db;
    if (S.eq_warn) {
        char num[16], t[64];
        snprintf(num, sizeof num, "+%.1f dB", peak);
        subst(t, sizeof t, tr("%s over 0 dB"), num, NULL);
        set_text(S.eq_warn, t);
        show(S.eq_warn, peak > 0.05);
        /* on the side away from the peak, so it covers neither it nor its dot */
        lv_obj_align(S.eq_warn, at >= EQG_PTS / 2 ? LV_ALIGN_TOP_LEFT : LV_ALIGN_TOP_RIGHT,
                     at >= EQG_PTS / 2 ? EQG_X0 : -(UI_W - EQG_X1), EQG_Y0);
    }
}

/* The band the page is about: the selected row's on the profile's page, the
 * page's own on a band's; -1 for none (Preamp, Add a band...) */
static int eqg_selected(void)
{
    const level_t *l = &S.stack[S.depth - 1];
    if (l->kind == L_EQ_BAND) return l->parent >= 0 && l->parent < S.eqe.n ? (int)l->parent : -1;
    if (l->kind == L_EQ_EDIT) return l->sel >= 1 && l->sel <= S.eqe.n ? l->sel - 1 : -1;
    return -1;
}

/* Drawn whole: the grid (+-6 dB, 100 Hz, 1 kHz, 10 kHz; 0 dB stronger), on a
 * band's page a faint line at its frequency, the curve a column a pixel (each
 * joined to the one before), the bands as dots on it, the selected one bigger
 * in the theme's signal colour */
static void eq_graph_draw(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(lv_event_get_target(e), &a);
    const theme_t *t = T();
    if (!S.eqg_have) eqg_update();
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.radius = 0;
    d.bg_opa = LV_OPA_COVER;
    int ox = a.x1, oy = a.y1, x0 = ox + EQG_X0, x1 = ox + EQG_X1;
    d.bg_color = C(t->surface);
    for (int g = -1; g <= 1; g += 2) {
        int y = oy + eqg_y(6.0 * g);
        bat_rect(layer, &d, x0, y, x1, y);
    }
    static const double decade[3] = {100, 1000, 10000};
    for (int i = 0; i < 3; i++) {
        int x = ox + eqg_x(decade[i]);
        bat_rect(layer, &d, x, oy + EQG_Y0, x, oy + EQG_Y1);
    }
    d.bg_color = C(t->text_muted);
    d.bg_opa = LV_OPA_50;
    bat_rect(layer, &d, x0, oy + eqg_y(0), x1, oy + eqg_y(0));
    int sel = eqg_selected();
    if (S.stack[S.depth - 1].kind == L_EQ_BAND && sel >= 0) {
        d.bg_color = C(t->signal);
        d.bg_opa = LV_OPA_40;
        int x = ox + eqg_x(S.eqe.band[sel].fc);
        bat_rect(layer, &d, x, oy + EQG_Y0, x, oy + EQG_Y1);
    }
    d.bg_opa = LV_OPA_COVER;
    d.bg_color = C(t->text);
    int prev = -1;
    for (int x = 0; x <= EQG_X1 - EQG_X0; x++) {
        int y = eqg_y(eqg_at(x));
        int lo = prev < 0 || y < prev ? y : prev, hi = prev < 0 || y > prev ? y : prev;
        bat_rect(layer, &d, x0 + x, oy + lo, x0 + x, oy + hi + 1);
        prev = y;
    }
    int round_dots = t->corner == THEME_CORNER_ROUND;
    for (int pass = 0; pass < 2; pass++)          /* the others, then the selected one over them */
        for (int i = 0; i < S.eqe.n; i++) {
            if ((i == sel) != pass) continue;
            int x = eqg_x(S.eqe.band[i].fc), y = eqg_y(eqg_at(x - EQG_X0)), r = pass ? 4 : 2;
            d.bg_color = C(pass ? t->signal : t->text_muted);
            d.radius = round_dots ? r + 1 : 0;
            bat_rect(layer, &d, ox + x - r, oy + y - r, ox + x + r, oy + y + r);
        }
}

/* The curve over the top of the EQ editor's pages (graphs_place moves the list down) */
static int eq_place(list_kind_t k)
{
    int on = k == L_EQ_EDIT || k == L_EQ_BAND;
    if (on && !S.eq_graph) {
        S.eq_graph = box(S.list, 0, 0, UI_W, BAT_GRAPH_H);
        lv_obj_add_event_cb(S.eq_graph, eq_graph_draw, LV_EVENT_DRAW_MAIN, NULL);
        for (int i = 0; i < 3; i++) S.eq_lbl[i] = label(S.eq_graph);
        S.eq_warn = label(S.eq_graph);
    }
    if (S.eq_graph) {
        show(S.eq_graph, on);
        lv_obj_set_style_bg_color(S.eq_graph, C(T()->bg), 0);
        lv_obj_set_style_bg_opa(S.eq_graph, LV_OPA_COVER, 0);
        if (on) {
            static const char *const mark[3] = {"100", "1k", "10k"};
            static const double at[3] = {100, 1000, 10000};
            for (int i = 0; i < 3; i++) {
                style_label(S.eq_lbl[i], S.f->meta, T()->text_muted);
                lv_obj_set_width(S.eq_lbl[i], 40);
                lv_obj_set_style_text_align(S.eq_lbl[i], LV_TEXT_ALIGN_CENTER, 0);
                set_text(S.eq_lbl[i], mark[i]);
                /* above the graph's foot, whatever the font's height (the pixel one is taller) */
                int y = BAT_GRAPH_H - lv_font_get_line_height(S.f->meta);
                lv_obj_set_pos(S.eq_lbl[i], eqg_x(at[i]) - 20, y > EQG_Y1 + 1 ? EQG_Y1 + 1 : y);
            }
            style_label(S.eq_warn, S.f->meta, T()->warning);
            lv_obj_set_style_bg_color(S.eq_warn, C(T()->bg), 0);   /* readable over the grid */
            lv_obj_set_style_bg_opa(S.eq_warn, LV_OPA_COVER, 0);
            lv_obj_set_style_pad_hor(S.eq_warn, 3, 0);
            S.eqg_have = 0;                         /* (a profile opened anew, or another theme) */
            eqg_update();
            lv_obj_move_foreground(S.eq_graph);
            lv_obj_invalidate(S.eq_graph);
        }
    }
    return on;
}


/* ---------- Podcasts: what's on the card, and the rows (Home > Podcasts) ---------- */

#define POD_STATES 400

static const char *pod_dir(void) { return podsync_dir(); }

/* The subscriptions, and an OPML file to offer */
static void pod_read_shows(void)
{
    if (!S.pod_shows) S.pod_shows = calloc(POD_MAX_SHOWS, sizeof *S.pod_shows);
    S.pod_n = S.pod_shows && pod_dir()[0] ? pod_shows_load(pod_dir(), S.pod_shows, POD_MAX_SHOWS) : 0;
    S.pod_ci = -1;
    for (int i = 0; i < S.pod_n; i++)
        if (!strcmp(S.pod_shows[i].id, S.pod_cur)) S.pod_ci = i;
    S.pod_opml[0] = '\0';
    if (S.hooks.podcast_opml && S.hooks.podcast_opml(S.pod_opml, sizeof S.pod_opml) != 1) S.pod_opml[0] = '\0';
}

/* The open show's episodes, where each was left, and which are on the card */
static void pod_read_episodes(void)
{
    /* the episode selected (and a menu's), by key: a refresh puts a new one
     * first, so the places move; found again whoever reads the list again
     * (a refresh, the place kept every 15 s, Mark played) */
    char sel_key[sizeof S.stack / sizeof S.stack[0]][13] = {{0}};
    for (int d = 0; d < S.depth && S.pod_eps; d++) {
        level_t *k = &S.stack[d];
        if (k->kind == L_POD_SHOW && k->sel >= 0 && k->sel < S.pod_ne)
            snprintf(sel_key[d], sizeof sel_key[d], "%s", S.pod_eps[k->sel].key);
    }
    S.pod_ne = S.pod_nst = 0;
    if (S.pod_ci >= 0) {
        if (!S.pod_eps) S.pod_eps = calloc(POD_MAX_EPISODES, sizeof *S.pod_eps);
        if (!S.pod_st) S.pod_st = calloc(POD_STATES, sizeof *S.pod_st);
    }
    if (S.pod_ci >= 0 && S.pod_eps && S.pod_st) {
        const char *id = S.pod_shows[S.pod_ci].id;
        S.pod_ne = pod_episodes_load(pod_dir(), id, S.pod_eps, POD_MAX_EPISODES);
        S.pod_nst = pod_state_load(pod_dir(), id, S.pod_st, POD_STATES);
        for (int i = 0; i < S.pod_ne; i++) S.pod_on_card[i] = (unsigned char)pod_downloaded(pod_dir(), id, &S.pod_eps[i]);
    }
    for (int d = 0; d < S.depth; d++)
        for (int i = 0; sel_key[d][0] && i < S.pod_ne; i++)
            if (!strcmp(S.pod_eps[i].key, sel_key[d])) S.stack[d].sel = i;
    if (S.pod_ctx_key[0]) {                       /* an episode's menu: where it is now, or gone (-1) */
        int at = -1;
        for (int i = 0; i < S.pod_ne; i++)
            if (!strcmp(S.pod_eps[i].key, S.pod_ctx_key)) at = i;
        S.pod_ctx = at;
    }
}

static void pod_free(void)
{
    free(S.pod_shows);
    free(S.pod_eps);
    free(S.pod_st);
    free(S.pod_found);
    S.pod_shows = NULL;
    S.pod_eps = NULL;
    S.pod_st = NULL;
    S.pod_found = NULL;
    S.pod_n = S.pod_ne = S.pod_nst = S.pod_nf = 0;
    S.pod_ci = -1;
    S.pod_cur[0] = '\0';
}

/* "1.25x" */
static void pod_speed_text(int pct, char *out, size_t cap)
{
    if (pct % 100 == 0) snprintf(out, cap, "%d\xC3\x97", pct / 100);
    else if (pct % 10 == 0) snprintf(out, cap, "%d.%d\xC3\x97", pct / 100, pct % 100 / 10);
    else snprintf(out, cap, "%d.%02d\xC3\x97", pct / 100, pct % 100);
}

/* The chapter of the episode playing at ms (-1: none) */
static int pod_chapter_at(uint32_t ms)
{
    int c = -1;
    for (int i = 0; i < S.pod_nch; i++)
        if (S.pod_ch[i].start_ms <= ms) c = i;
    return c;
}

static int pod_subscribed(const char *url)
{
    for (int i = 0; i < S.pod_n; i++)
        if (!strcmp(S.pod_shows[i].url, url)) return 1;
    return 0;
}

static void pod_shows_row(int i, char *main, size_t mn, char *meta, size_t tn, int *playing)
{
    if (i == 0) {
        podsync_status_t st;
        podsync_status(&st);
        snprintf(main, mn, "%s", tr("Find podcasts"));
        if (st.refreshing) snprintf(meta, tn, "%s", tr("Checking"));
        return;
    }
    i--;
    if (!S.pod_n && !S.pod_opml[0]) {            /* none yet: how to add some */
        snprintf(main, mn, "%s", tr("Or an OPML file on the card"));
        return;
    }
    if (i < S.pod_n) {
        const pod_show_t *s = &S.pod_shows[i];
        snprintf(main, mn, "%s", s->title);
        if (s->latest > s->seen) snprintf(meta, tn, "%s", tr("New"));
        *playing = S.now.podcast && !strcmp(S.pod_play.id, s->id);
        return;
    }
    const char *b = strrchr(S.pod_opml, '/');
    subst(main, mn, tr("Import %s"), b ? b + 1 : S.pod_opml, NULL);
}

/* An episode: its title; how it stands (downloading, played, time left, new, its length) */
static void pod_episode_row(int i, char *main, size_t mn, char *meta, size_t tn, int *playing)
{
    if (S.pod_ci < 0) return;
    const pod_show_t *s = &S.pod_shows[S.pod_ci];
    if (!S.pod_ne) {                      /* none: why */
        podsync_status_t st;
        podsync_status(&st);
        snprintf(main, mn, "%s", tr(st.refreshing ? "Reading its episodes\xE2\x80\xA6"
                                    : s->refreshed ? "No episodes" : "Menu: Check for new episodes"));
        return;
    }
    const pod_episode_t *e = &S.pod_eps[i];
    snprintf(main, mn, "%s", e->title);
    pod_state_t st;
    pod_state_find(S.pod_st, S.pod_nst, e->key, &st);
    int dl = podsync_download_state(s->id, e->key);
    uint32_t len = st.len_ms ? st.len_ms : (uint32_t)e->dur_s * 1000;
    if (dl == 0) snprintf(meta, tn, "%s", tr("Waiting"));
    else if (dl > 0) snprintf(meta, tn, "%d%%", dl);
    else if (st.played) snprintf(meta, tn, "%s", tr("Played"));
    else if (st.pos_ms && len > st.pos_ms) tr_num(meta, tn, "%s min left", (long)((len - st.pos_ms + 59999) / 60000));
    else if (e->date > S.pod_seen) snprintf(meta, tn, "%s", tr("New"));
    else if (len) tr_num(meta, tn, "%s min", (long)((len + 59999) / 60000));
    *playing = S.now.podcast && !strcmp(S.pod_play.id, s->id) && !strcmp(S.pod_play.key, e->key);
}

/* Find podcasts: a show found, or a line saying how the search goes */
static void pod_found_row(int i, char *main, size_t mn, char *meta, size_t tn, int *playing)
{
    if (S.pod_nf <= 0) {
        snprintf(main, mn, "%s", tr(S.pod_search == 0 ? "Searching\xE2\x80\xA6"
                                    : S.pod_search == -2 ? "Couldn't reach the directory" : "No podcasts found"));
        return;
    }
    if (i >= S.pod_nf) return;
    snprintf(main, mn, "%s", S.pod_found[i].title);
    if (pod_subscribed(S.pod_found[i].url)) {
        snprintf(meta, tn, "%s", tr("Added"));
        *playing = 1;
    }
}

/* ---------- the theme editor's choices and its preview (Settings > Interface > Theme) ---------- */

/* Accents: the selection, progress, playing mark and icons in one colour */
static const struct { const char *name; int hue, sat; } ACCENTS[] = {
    {"Mint", 158, 72}, {"Teal", 176, 60}, {"Sky", 199, 85}, {"Steel blue", 214, 41}, {"Blue", 217, 85},
    {"Indigo", 235, 70}, {"Violet", 262, 70}, {"Lavender", 270, 55}, {"Purple", 290, 65}, {"Plum", 305, 45},
    {"Pink", 330, 75}, {"Hot pink", 330, 100}, {"Red", 355, 80}, {"Tomato", 4, 100}, {"Coral", 14, 100},
    {"Orange", 28, 95}, {"Wheat", 39, 55}, {"Amber", 42, 95}, {"Lime", 84, 60}, {"Sea green", 120, 25},
    {"Emerald", 140, 60}, {"Grey", 210, 8},   /* (Grey last: the greys) */
};
#define N_ACCENTS ((int)(sizeof ACCENTS / sizeof ACCENTS[0]))

/* The colours as the editor names them (TC_*, at the top) */
static const struct { const char *name; size_t off; } THE_COL[TC_N] = {
    {"Background", offsetof(theme_t, bg)}, {"Panels", offsetof(theme_t, surface)},
    {"Text", offsetof(theme_t, text)}, {"Secondary text", offsetof(theme_t, text_muted)},
    {"Selection", offsetof(theme_t, select_bg)}, {"Text on selection", offsetof(theme_t, select_fg)},
    {"Progress, playing", offsetof(theme_t, signal)}, {"Icons", offsetof(theme_t, mode)},
    {"Format, output", offsetof(theme_t, info)}, {"Battery", offsetof(theme_t, battery)},
    {"Artist", offsetof(theme_t, artist)}, {"Album", offsetof(theme_t, album)},
    {"Format chip", offsetof(theme_t, chip_bg)}, {"Text on chip", offsetof(theme_t, chip_fg)},
    {"Warnings", offsetof(theme_t, warning)}, {"Errors", offsetof(theme_t, danger)},
};
/* theme_validate's rules: a colour on another, and the contrast it needs */
static const struct { int fg, bg; double min; } THE_PAIRS[] = {
    {TC_TEXT, TC_BG, 7}, {TC_MUTED, TC_BG, 4.5}, {TC_SELFG, TC_SELBG, 7}, {TC_TEXT, TC_SURFACE, 7},
    {TC_SIGNAL, TC_BG, 4.5}, {TC_INFO, TC_BG, 4.5}, {TC_MODE, TC_BG, 4.5}, {TC_BATTERY, TC_BG, 4.5},
    {TC_WARNING, TC_BG, 4.5}, {TC_DANGER, TC_BG, 4.5}, {TC_ARTIST, TC_BG, 4.5}, {TC_ALBUM, TC_BG, 4.5},
    {TC_CHIPFG, TC_CHIPBG, 4.5},
};
#define N_PAIRS ((int)(sizeof THE_PAIRS / sizeof THE_PAIRS[0]))

static const struct { const char *family, *name; } FONT_FAMILIES[] = {
    {"atkinson", "Atkinson"}, {"arkpixel", "Ark Pixel"}, {"vt323", "VT323"},
};
/* Text size Large: a step up from each family's sizes (Ark Pixel has only its own) */
static const int SIZE_LARGE_ATKINSON[THEME_PX_N] = {16, 22, 20, 24, 17};
static const int SIZE_LARGE_VT323[THEME_PX_N] = {22, 28, 26, 32, 24};


static uint32_t *the_col(theme_t *t, int r) { return (uint32_t *)((char *)t + THE_COL[r].off); }
static uint32_t the_get(int r) { return *the_col(&S.the, r); }

static const int *the_large(const theme_t *t)
{
    if (t->font_file[0]) return NULL;
    return !strcmp(t->font, "atkinson") ? SIZE_LARGE_ATKINSON : !strcmp(t->font, "vt323") ? SIZE_LARGE_VT323 : NULL;
}

/* 0: the family's own sizes, 1: Large, 2: the theme file's own */
static int the_size(const theme_t *t)
{
    int none = 1;
    for (int r = 0; r < THEME_PX_N; r++) none &= !t->font_px[r];
    if (none) return 0;
    const int *large = the_large(t);
    return large && !memcmp(large, t->font_px, sizeof t->font_px) ? 1 : 2;
}

/* Of the rules colour r is in (r = -1: all of them), the one that reads worst
 * for what it needs; -1 if none. Its contrast goes in *c. */
static int the_worst(int r, double *c)
{
    int w = -1;
    double worst = 1e9;
    for (int i = 0; i < N_PAIRS; i++) {
        if (r >= 0 && THE_PAIRS[i].fg != r && THE_PAIRS[i].bg != r) continue;
        double x = theme_contrast(the_get(THE_PAIRS[i].fg), the_get(THE_PAIRS[i].bg));
        if (x / THE_PAIRS[i].min < worst) { worst = x / THE_PAIRS[i].min; w = i; *c = x; }
    }
    return w;
}

/* "Text: bad contrast" for a rule that fails (r: as for the_worst), else "" */
static void the_problem(int r, char *out, size_t cap)
{
    double c = 0;
    int w = the_worst(r, &c);
    out[0] = '\0';
    if (w < 0 || c >= THE_PAIRS[w].min) return;
    snprintf(out, cap, "%s: %s", tr(THE_COL[THE_PAIRS[w].fg].name), tr("bad contrast"));
}

/* The editor's rows: the theme's choices, then Save (and Delete, for one on the card) */
static int the_rows(void) { return S.the_file[0] ? TE_N : TE_N - 1; }

static void the_row(int i, char *main, size_t mn, char *meta, size_t tn)
{
    const theme_t *t = &S.the;
    int ed = S.editing && S.stack[S.depth - 1].sel == i;
    char v[64] = "";
    static const char *const corner[] = {"Round", "Stepped", "Square"};
    switch (i) {
    case TE_NAME: snprintf(main, mn, "Name"); snprintf(meta, tn, "%s", t->name); return;
    case TE_BASE: snprintf(main, mn, "Dark or light"); snprintf(v, sizeof v, "%s", tr(t->dark ? "Dark" : "Light")); break;
    case TE_ACCENT:
        snprintf(main, mn, "Accent");
        snprintf(v, sizeof v, "%s", tr(S.the_acc >= 0 ? ACCENTS[S.the_acc].name : "Its own"));
        break;
    case TE_COLOURS: {
        char p[96];
        snprintf(main, mn, "Colours");
        the_problem(-1, p, sizeof p);
        if (p[0]) snprintf(v, sizeof v, "%s", tr("bad contrast"));
        break;
    }
    case TE_FONT:
        snprintf(main, mn, "Font");
        snprintf(v, sizeof v, "%s", tr("Its own"));
        for (size_t f = 0; f < sizeof FONT_FAMILIES / sizeof FONT_FAMILIES[0]; f++)
            if (!t->font_file[0] && !strcmp(t->font, FONT_FAMILIES[f].family)) snprintf(v, sizeof v, "%s", FONT_FAMILIES[f].name);
        break;
    case TE_SIZE: {
        int z = the_size(t);
        snprintf(main, mn, "Text size");
        snprintf(v, sizeof v, "%s", tr(z == 0 ? "Normal" : z == 1 ? "Large" : "Its own"));
        break;
    }
    case TE_SELECTION: snprintf(main, mn, "Selection"); snprintf(v, sizeof v, "%s", tr(t->selection_bar ? "Bar" : "Pill")); break;
    case TE_CORNERS: snprintf(main, mn, "Corners"); snprintf(v, sizeof v, "%s", tr(corner[t->corner <= 2 ? t->corner : 0])); break;
    case TE_RADIUS: snprintf(main, mn, "Corner radius"); snprintf(v, sizeof v, "%d px", t->radius); break;
    case TE_PROGRESS: snprintf(main, mn, "Progress bar"); snprintf(v, sizeof v, "%s", tr(t->progress_segments ? "Segments" : "Bar")); break;
    case TE_PLAYING: snprintf(main, mn, "Playing mark"); snprintf(v, sizeof v, "%s", tr(t->playing_equalizer ? "Equalizer" : "Dot")); break;
    case TE_CURSOR: snprintf(main, mn, "Cursor"); snprintf(v, sizeof v, "%s", tr(t->cursor_arrow ? "Arrow" : "None")); break;
    case TE_SHADOW: snprintf(main, mn, "Shadow"); snprintf(v, sizeof v, "%s", tr(t->shadow_hard ? "Hard" : "None")); break;
    case TE_MOTION: snprintf(main, mn, "Motion"); snprintf(v, sizeof v, "%s", tr(t->motion_stepped ? "Stepped" : "Smooth")); break;
    case TE_SAVE: snprintf(main, mn, "Save"); if (S.the_dirty) snprintf(v, sizeof v, "%s", tr("not saved")); break;
    default: snprintf(main, mn, "Delete this theme"); return;
    }
    snprintf(meta, tn, ed ? "< %s >" : "%s", v);
}

/* The colours a colour can be, picked with the wheel on Colours: greys, then
 * each accent's hue in six tones, from pale (for text on dark) to night (for
 * dark backgrounds) */
static const struct { const char *name; int l; } GREYS[] = {
    {"White", 100}, {"Paper", 95}, {"Light grey", 80}, {"Grey", 60}, {"Dark grey", 36}, {"Charcoal", 18},
    {"Ink", 8}, {"Black", 0},
};
static const struct { const char *fmt; int sat, l; } TONES[] = {   /* sat: of the accent's own, in % */
    {"Pale %s", 70, 90}, {"Light %s", 90, 76}, {"%s", 100, 62}, {"Dark %s", 90, 44}, {"Deep %s", 50, 22},
    {"Night %s", 35, 9},
};
#define N_GREYS ((int)(sizeof GREYS / sizeof GREYS[0]))
#define N_TONES ((int)(sizeof TONES / sizeof TONES[0]))
#define N_PAL (N_GREYS + (N_ACCENTS - 1) * N_TONES)   /* (the accents' Grey is the greys) */

static uint32_t the_pal_at(int i, char *name, size_t cap)
{
    if (i < N_GREYS) {
        if (name) snprintf(name, cap, "%s", tr(GREYS[i].name));
        return theme_from_hsl(0, 0, GREYS[i].l);
    }
    i -= N_GREYS;
    int h = i / N_TONES, t = i % N_TONES;
    if (name) subst(name, cap, tr(TONES[t].fmt), tr(ACCENTS[h].name), NULL);
    return theme_from_hsl(ACCENTS[h].hue, ACCENTS[h].sat * TONES[t].sat / 100, TONES[t].l);
}

/* The named colour c is (-1: none), and the nearest one */
static int the_pal_find(uint32_t c)
{
    for (int i = 0; i < N_PAL; i++)
        if (the_pal_at(i, NULL, 0) == (c & 0xFFFFFF)) return i;
    return -1;
}

static int the_pal_nearest(uint32_t c)
{
    int best = 0;
    long bd = -1;
    for (int i = 0; i < N_PAL; i++) {
        uint32_t p = the_pal_at(i, NULL, 0);
        long dr = (long)((p >> 16) & 0xff) - (long)((c >> 16) & 0xff), dg = (long)((p >> 8) & 0xff) - (long)((c >> 8) & 0xff),
             db = (long)(p & 0xff) - (long)(c & 0xff), d = 3 * dr * dr + 4 * dg * dg + 2 * db * db;
        if (bd < 0 || d < bd) { bd = d; best = i; }
    }
    return best;
}

/* Colours: each one, and how its text reads ("bad contrast" under the rule) */
static void the_colour_row(int i, char *main, size_t mn, char *meta, size_t tn)
{
    if (i < 0 || i >= TC_N) return;
    snprintf(main, mn, "%s", THE_COL[i].name);
    if (S.editing && S.stack[S.depth - 1].sel == i) {   /* being picked: its name */
        char name[48];
        if (S.the_pal >= 0) the_pal_at(S.the_pal, name, sizeof name);
        else snprintf(name, sizeof name, "%s", tr("Its own"));
        snprintf(meta, tn, "< %s >", name);
        return;
    }
    double c = 0;
    int w = the_worst(i, &c), fg = 0;
    if (w < 0) return;
    for (int p = 0; p < N_PAIRS; p++) fg |= THE_PAIRS[p].fg == i;
    if (c < THE_PAIRS[w].min) snprintf(meta, tn, "%s", tr("bad contrast"));   /* (the strip says which text) */
    else if (fg) snprintf(meta, tn, "%.1f:1", floor(c * 10) / 10);   /* (a background: its text's, on their rows) */
}

static int is_theme_page(list_kind_t k) { return k == L_THEME_EDIT || k == L_THEME_COLOURS; }

/* The preview: a header, a selected row, a playing one and a progress bar, in
 * the theme being made; the editor's own page stays in the theme in use. */
#define THP_W (UI_W - 12)
static void the_preview(void)
{
    if (!S.thp) return;
    const theme_t *t = &S.the, *u = T();
    int round = t->corner == THEME_CORNER_ROUND, bar = t->selection_bar;
    fill(S.thp, u->bg);
    fill(S.thp_in, t->bg);
    lv_obj_set_style_border_color(S.thp_in, C(u->surface), 0);
    /* (one small font: the theme's own can be too tall for the strip, and the colours are the point) */
    const lv_font_t *pf = &lv_font_montserrat_14;
    style_label(S.thp_hd, pf, t->text);
    style_label(S.thp_bat, pf, t->battery);
    for (int i = 0; i < 2; i++) fill(S.thp_mode[i], t->mode);
    /* the selection: a pill (or stepped) or a full-width bar */
    int sx = bar ? 0 : 4, sw = bar ? THP_W - 2 : THP_W - 10;
    lv_obj_set_pos(S.thp_sel, sx, 22);
    lv_obj_set_width(S.thp_sel, sw);
    lv_obj_set_style_radius(S.thp_sel, round && !bar ? (t->radius < 13 ? t->radius : 13) : 0, 0);
    fill(S.thp_sel, t->select_bg);
    for (int k = 0; k < 4; k++) {
        fill(S.thp_notch[k], t->bg);
        lv_obj_set_x(S.thp_notch[k], k & 1 ? sw - STEP : 0);
        show(S.thp_notch[k], t->corner == THEME_CORNER_STEPPED && !bar);
    }
    show(S.thp_cur, t->cursor_arrow);
    style_label(S.thp_cur, &lv_font_montserrat_14, t->select_fg);
    style_label(S.thp_sel_t, pf, t->select_fg);
    lv_obj_align(S.thp_sel_t, LV_ALIGN_LEFT_MID, t->cursor_arrow ? 24 : 10, 0);
    style_label(S.thp_sel_m, pf, t->select_fg);
    /* the playing row: a dot or an equalizer */
    lv_obj_set_style_radius(S.thp_dot, round ? LV_RADIUS_CIRCLE : 0, 0);
    fill(S.thp_dot, t->signal);
    show(S.thp_dot, !t->playing_equalizer);
    for (int i = 0; i < 3; i++) { fill(S.thp_eq[i], t->signal); show(S.thp_eq[i], t->playing_equalizer); }
    style_label(S.thp_row_t, pf, t->text);
    style_label(S.thp_row_m, pf, t->text_muted);
    /* progress: a bar or segments */
    int seg = t->progress_segments;
    show(S.thp_track, !seg);
    fill(S.thp_track, t->surface);
    fill(S.thp_fill, t->signal);
    lv_obj_set_style_radius(S.thp_track, round ? 3 : 0, 0);
    lv_obj_set_style_radius(S.thp_fill, round ? 3 : 0, 0);
    for (int i = 0; i < 10; i++) { fill(S.thp_seg[i], i < 6 ? t->signal : t->surface); show(S.thp_seg[i], seg); }
    /* a rule that fails: the colour's own on its page, else the first */
    char p[96];
    level_t *l = &S.stack[S.depth - 1];
    the_problem(l->kind == L_THEME_COLOURS ? l->sel : -1, p, sizeof p);
    style_label(S.thp_warn, S.f->meta, u->warning);
    lv_obj_set_style_bg_color(S.thp_warn, C(u->bg), 0);
    set_text(S.thp_warn, p);
    show(S.thp_warn, p[0] != '\0');
    lv_obj_invalidate(S.thp);
}

/* The preview over the top of the theme editor's pages (graphs_place moves the list down) */
static int the_place(list_kind_t k)
{
    int on = is_theme_page(k);
    if (on && !S.thp) {
        S.thp = box(S.list, 0, 0, UI_W, BAT_GRAPH_H);
        S.thp_in = box(S.thp, 6, 2, THP_W, BAT_GRAPH_H - 6);
        lv_obj_set_style_border_width(S.thp_in, 1, 0);
        lv_obj_set_style_radius(S.thp_in, 6, 0);
        lv_obj_set_style_clip_corner(S.thp_in, true, 0);
        S.thp_hd = label(S.thp_in);
        lv_obj_set_pos(S.thp_hd, 8, 2);
        lv_label_set_text(S.thp_hd, LV_SYMBOL_LEFT " Albums");
        S.thp_bat = label(S.thp_in);
        lv_obj_align(S.thp_bat, LV_ALIGN_TOP_RIGHT, -8, 2);
        lv_label_set_text(S.thp_bat, "86%");
        for (int i = 0; i < 2; i++) S.thp_mode[i] = box(S.thp_in, THP_W - 66 + 5 * i, 6, 3, 10);
        S.thp_sel = box(S.thp_in, 4, 22, THP_W - 10, 26);
        for (int q = 0; q < 4; q++) S.thp_notch[q] = box(S.thp_sel, 0, q < 2 ? 0 : 26 - STEP, STEP, STEP);
        S.thp_cur = label(S.thp_sel);
        lv_label_set_text(S.thp_cur, LV_SYMBOL_PLAY);
        lv_obj_align(S.thp_cur, LV_ALIGN_LEFT_MID, 8, 0);
        S.thp_sel_t = label(S.thp_sel);
        lv_label_set_text(S.thp_sel_t, "Kind of Blue");
        S.thp_sel_m = label(S.thp_sel);
        lv_label_set_text(S.thp_sel_m, "1959");
        lv_obj_align(S.thp_sel_m, LV_ALIGN_RIGHT_MID, -10, 0);
        S.thp_dot = box(S.thp_in, 12, 58, 6, 6);
        static const int eq_h[3] = {8, 12, 6};
        for (int i = 0; i < 3; i++) S.thp_eq[i] = box(S.thp_in, 10 + 4 * i, 68 - eq_h[i], 3, eq_h[i]);
        S.thp_row_t = label(S.thp_in);
        lv_obj_set_pos(S.thp_row_t, 26, 53);
        lv_label_set_text(S.thp_row_t, "Blue Train");
        S.thp_row_m = label(S.thp_in);
        lv_obj_align(S.thp_row_m, LV_ALIGN_TOP_RIGHT, -14, 53);
        lv_label_set_text(S.thp_row_m, "1957");
        S.thp_track = box(S.thp_in, 8, 75, THP_W - 18, 5);
        S.thp_fill = box(S.thp_track, 0, 0, (THP_W - 18) * 6 / 10, 5);
        int segw = (THP_W - 18 - 9 * 3) / 10;
        for (int i = 0; i < 10; i++) S.thp_seg[i] = box(S.thp_in, 8 + i * (segw + 3), 75, segw, 5);
        S.thp_warn = label(S.thp);
        lv_obj_set_style_bg_opa(S.thp_warn, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_hor(S.thp_warn, 3, 0);
        lv_obj_align(S.thp_warn, LV_ALIGN_TOP_RIGHT, -10, 3);
    }
    if (S.thp) {
        show(S.thp, on);
        if (on) { lv_obj_move_foreground(S.thp); the_preview(); }
    }
    return on;
}

/* A graph over the top of the list, if the page has one: the list moves down below it */
static void graphs_place(list_kind_t k)
{
    int on = battery_place(k);
    on |= eq_place(k);
    on |= the_place(k);
    S.list_y0 = on ? BAT_GRAPH_H : 0;
    rows_box_place();
}

/* How many rows show below a graph, or without one */
static int rows_shown(list_kind_t k)
{
    return (k == L_BATTERY && S.hooks.battery_report) || k == L_EQ_EDIT || k == L_EQ_BAND || is_theme_page(k) ? 3 : ROWS;
}

static void battery_load(void)
{
    if (!S.bat_days) S.bat_days = 1;
    memset(&S.bat, 0, sizeof S.bat);
    S.bat.days = S.bat_days;
    if (S.hooks.battery_report) S.hooks.battery_report(&S.bat, S.bat_days);
    S.bat_at = lv_tick_get();
    battery_labels();
    if (S.bat_graph) lv_obj_invalidate(S.bat_graph);
}

static int covers_in(list_kind_t k);

static void list_item(const level_t *l, int i, char *main, size_t mn, char *meta, size_t tn, int *playing)
{
    *playing = 0;
    main[0] = meta[0] = '\0';
    if (library_backed(l->kind)) {
        int64_t id = l->items.ids[i];
        if (l->kind == L_PLAYLISTS && id < 0) {          /* a smart playlist: its name, how many songs */
            snprintf(main, mn, "%s", tr(library_smart_name((int)-id)));
            snprintf(meta, tn, "%d", S.smart_n[-id < LIB_SMART_N ? -id : 0]);
            return;
        }
        if (S.lib) library_row(S.lib, view_of(l->kind), id, main, mn, meta, tn);
        if (covers_in(l->kind) && !(album_artists_on() && library_row_shows_artist(view_of(l->kind))))
            meta[0] = '\0';   /* (the cover takes the year's or length's place) */
        if (S.moving && l->kind == L_PLAYLIST_TRACKS && i == l->sel) snprintf(meta, tn, "%s", tr("Moving"));
        if (S.now.active) {
            if (is_track_list(l->kind) || l->kind == L_FOLDER) *playing = id == S.now.track.id;
            else if (is_album_list(l->kind)) *playing = id == S.now.track.album_id;
        }
        return;
    }
    switch (l->kind) {
    case L_HOME:
        if (S.home[i] == H_UPDATE) {
            const char *tag = update_offered();
            snprintf(main, mn, "Update available!");
            snprintf(meta, tn, "%s", tag ? tag : "");
            break;
        }
        snprintf(main, mn, "%s", HOME[S.home[i]]);
        if (S.home[i] == H_NOWPLAYING) *playing = S.now.playing;
        break;
    case L_UPD_ASK:
        if (i == 0) {
            const char *tag = update_offered();
            snprintf(main, mn, "Install new version");
            snprintf(meta, tn, "%s", tag ? tag : "");
        } else {
            snprintf(main, mn, "Ignore");
        }
        break;
    case L_USB_ASK:
        snprintf(main, mn, "%s", i == 0 ? "Use as a USB drive" : i == 1 ? "Use as a USB DAC" : "Keep playing");
        break;
    case L_WIFI_ASK:
        snprintf(main, mn, "%s", i == 0 ? "Turn on Wi-Fi" : "Not now");
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
        if (!S.hooks.bt_state) { snprintf(main, mn, "%s", tr("Not available")); break; }
        if (i == 0) {
            snprintf(main, mn, "%s", tr("Bluetooth"));
            snprintf(meta, tn, "%s", tr(S.bt_busy[0] && strcmp(S.bt_busy, "Searching") ? S.bt_busy : S.bt_on ? "On" : "Off"));
        } else if (i == 1) {
            snprintf(main, mn, "%s", tr("Find headphones"));
            if (!strcmp(S.bt_busy, "Searching")) snprintf(meta, tn, "%s", tr("Searching"));
        } else if (i == 2) {                       /* sound quality or reach: the codec's bitrate */
            snprintf(main, mn, "%s", tr("Audio"));
            snprintf(meta, tn, "%s", tr(settings_get_int("bluetooth.range", UI_BT_RANGE_DEFAULT) ? "Longer range" : "Best sound"));
        } else if (i - BT_DEV_ROW < S.bt_n) {
            const ui_bt_dev_t *d = &S.bt[i - BT_DEV_ROW];
            snprintf(main, mn, "%s", d->name[0] ? d->name : d->addr);
            snprintf(meta, tn, "%s", tr(d->connected ? "Connected" : d->paired ? "Paired" : "Pair"));
            *playing = d->connected;
        }
        break;
    case L_WIFI:
        if (!S.hooks.wifi_state) { snprintf(main, mn, "%s", tr("Not available")); break; }
        int net, row = wifi_row(i, &net);
        if (row == WR_TOGGLE) {
            snprintf(main, mn, "%s", tr("Wi-Fi"));
            snprintf(meta, tn, "%s", tr(S.wifi_busy[0] && strcmp(S.wifi_busy, "Searching") ? S.wifi_busy : S.wifi_on ? "On" : "Off"));
        } else if (row == WR_LINK) {
            const ui_wifi_status_t *st = &S.wifi_st;
            if (st->state == UI_WIFI_CONNECTED) {
                snprintf(main, mn, "%s", st->ssid);
                snprintf(meta, tn, "%s", st->ip);
                *playing = 1;
            } else {
                snprintf(main, mn, "%s", tr(st->state == UI_WIFI_CONNECTING ? "Connecting" : "Not connected"));
                if (st->state == UI_WIFI_CONNECTING) snprintf(meta, tn, "%.*s", (int)tn - 1, st->ssid);
            }
        } else if (row == WR_SEARCH) {
            snprintf(main, mn, "%s", tr("Search again"));
            if (!strcmp(S.wifi_busy, "Searching")) snprintf(meta, tn, "%s", tr("Searching"));
        } else if (net < S.wifi_n) {
            const ui_wifi_net_t *w = &S.wifi[net];
            snprintf(main, mn, "%s", w->ssid);
            const char *sig = w->dbm >= -55 ? "Strong" : w->dbm >= -67 ? "Good" : w->dbm >= -78 ? "Fair" : "Weak";
            if (w->connected) snprintf(meta, tn, "%s", tr("Connected"));
            else if (w->saved) snprintf(meta, tn, "%s", tr("Saved"));
            else if (w->secured) snprintf(meta, tn, "%s", tr(sig));
            else subst(meta, tn, tr("%s, open"), tr(sig), NULL);
            *playing = w->connected;
        }
        break;
    case L_WIFI_PASS: { char wheel[3][WHEEL_CAP]; entry_row(l->kind, main, mn, wheel); break; }
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
        if (i == 0) { snprintf(main, mn, "%s", tr("Find stations")); break; }
        snprintf(main, mn, "%s", S.radio[i - 1].name);
        if (rd_on_air(&S.radio[i - 1])) {
            *playing = 1;
            snprintf(meta, tn, "%s", tr(S.now.connecting ? "Connecting" : "Playing"));
        }
        break;
    case L_RDIR:
        snprintf(main, mn, "%s", RD_MENU[i]);
        if (i == RD_MENU_RANDOM) {
            if (S.rd_job == RD_JOB_RANDOM && S.rd_job_depth == S.depth) snprintf(meta, tn, "Searching");
            else if (!rd_random_filter(NULL, meta, tn)) snprintf(meta, tn, "Any");
        }
        break;
    case L_RDIR_GENRES: snprintf(main, mn, "%s", RDIR_GENRES[i]); break;
    case L_RDIR_COUNTRIES:
        if (i < S.rd_cty_n) {
            snprintf(main, mn, "%s", S.rd_cty[i].name);
            snprintf(meta, tn, "%d", S.rd_cty[i].count);
        }
        break;
    case L_RDIR_RESULTS:
    case L_RDIR_SEARCH: {
        int r = i - 1;
        if (l->kind == L_RDIR_SEARCH && i == 0) { char wheel[3][WHEEL_CAP]; entry_row(l->kind, main, mn, wheel); break; }
        if (l->kind == L_RDIR_RESULTS && i == 0) {
            snprintf(main, mn, "%s", tr("Random station"));
            if (S.rd_job == RD_JOB_RANDOM && S.rd_job_depth == S.depth) snprintf(meta, tn, "%s", tr("Searching"));
            break;
        }
        if (r < 0 || r >= S.rd_n) {   /* Search: how it goes */
            snprintf(main, mn, "%s", tr(S.rd_job == RD_JOB_LIST ? "Searching\xE2\x80\xA6"
                                        : S.rd_state == RD_FAILED ? S.rd_msg : "No stations found"));
            break;
        }
        snprintf(main, mn, "%s", S.rd_st[r].name);
        snprintf(meta, tn, "%s", S.rd_meta[r].detail);
        *playing = rd_on_air(&S.rd_st[r]);
        break;
    }
    case L_QUICK: case L_DISPLAY: case L_SOUND: case L_PLAYBACK: case L_POWER: case L_LIBRARY: case L_USB: case L_POD_SETTINGS:
    case L_INTERFACE:
        setting_row(setting_at(l, i), S.editing && i == l->sel, main, mn, meta, tn);
        break;
    case L_SIGNAL: if (S.hooks.signal_path) S.hooks.signal_path(i, main, mn, meta, tn); break;
    case L_ABOUT:
        switch (about_row(i)) {
        case AB_RESET: snprintf(main, mn, "Reset all settings"); break;
        case AB_WHATSNEW:
            snprintf(main, mn, "%s", tr("What's new"));
            snprintf(meta, tn, "%s", S.wn_ver);
            break;
        case AB_UNINSTALL: snprintf(main, mn, "Uninstall Fathom"); break;
        case AB_AUTO:
            snprintf(main, mn, "Check automatically");
            snprintf(meta, tn, "%s", on_off(settings_get_int("update.auto", UI_UPDATE_AUTO_DEFAULT)));
            break;
        case AB_CHECK: {
            const char *found = settings_get("update.found", "");   /* (the device's: newer than this one) */
            snprintf(main, mn, "%s", found[0] ? "Update available" : "Check for updates");
            snprintf(meta, tn, "%s", found);
            break;
        }
        default:
            snprintf(main, mn, "%s", S.about_lbl[i]);
            snprintf(meta, tn, "%s", S.about_val[i]);
            break;
        }
        break;
    case L_UPDATE: update_row(i, main, mn, meta, tn); break;
    case L_UNINSTALL: uninstall_row_text(i, main, mn, meta, tn); break;
    case L_BATTERY: case L_BAT_SPAN: case L_BAT_HISTORY: battery_row(l->kind, i, main, mn, meta, tn); break;
    case L_CONTEXT:
        snprintf(main, mn, "%s", CTX_LABEL[S.ctx[i]]);
        if (S.ctx[i] == CTX_POD_SPEED && S.player) pod_speed_text(player_speed(S.player), meta, tn);
        break;
    case L_INFO: info_row(l->parent, i, main, mn, meta, tn); break;
    case L_SEARCH:
    case L_RENAME:
        if (i == 0) { char wheel[3][WHEEL_CAP]; entry_row(l->kind, main, mn, wheel); }
        else search_row(i - 1, main, mn, meta, tn);
        break;
    case L_PICK_PLAYLIST:
        if (i == 0) snprintf(main, mn, "%s", tr("New playlist"));
        else if (S.lib) library_row(S.lib, LIB_PLAYLISTS, S.pick_pls.ids[i - 1], main, mn, meta, tn);
        break;
    case L_QUEUE: {
        int64_t id = S.player ? player_queue_at(S.player, i) : -1;
        if (S.lib && id >= 0) library_row(S.lib, LIB_PLAYLIST_TRACKS, id, main, mn, meta, tn);
        if (S.moving && i == l->sel) snprintf(meta, tn, "%s", tr("Moving"));
        break;
    }
    case L_EQ:
        if (i == 0) {
            snprintf(main, mn, "%s", tr("Off"));
            snprintf(meta, tn, "%s", tr("bit-perfect"));
            *playing = !S.eq_name[0];
        } else if (i - 1 < S.eq_n) {
            snprintf(main, mn, "%s", S.eq_names[i - 1]);
            *playing = !strcmp(S.eq_names[i - 1], S.eq_name);
        } else {
            snprintf(main, mn, "%s", tr("New EQ"));
        }
        break;
    case L_EQ_EDIT: eqe_row(i, main, mn, meta, tn); break;
    case L_EQ_BAND: eqe_band_row((int)l->parent, i, main, mn, meta, tn); break;
    case L_PODCASTS: pod_shows_row(i, main, mn, meta, tn, playing); break;
    case L_POD_SHOW: pod_episode_row(i, main, mn, meta, tn, playing); break;
    case L_POD_CHAPTERS:
        if (i < S.pod_nch) {
            if (S.pod_ch[i].title[0]) snprintf(main, mn, "%s", S.pod_ch[i].title);
            else tr_num(main, mn, "Chapter %s", i + 1);
            fmt_time(meta, tn, (int)(S.pod_ch[i].start_ms / 1000), 0);
            *playing = i == pod_chapter_at(S.now.pos_ms);
        }
        break;
    case L_POD_SEARCH:
        if (i == 0) { char wheel[3][WHEEL_CAP]; entry_row(l->kind, main, mn, wheel); }
        else pod_found_row(i - 1, main, mn, meta, tn, playing);
        break;
    case L_THEME_EDIT: the_row(i, main, mn, meta, tn); break;
    case L_THEME_COLOURS: the_colour_row(i, main, mn, meta, tn); break;
    case L_THEMES:
        if (i >= S.n_themes) { snprintf(main, mn, "%s", tr("New theme")); break; }
        snprintf(main, mn, "%s", S.themes[i].name);
        if (S.themes[i].status == THEME_ERROR) snprintf(meta, tn, "%s", tr("can't load"));
        else if (S.themes[i].status == THEME_WARN) snprintf(meta, tn, "%s", tr("warning"));
        else if (i == 0) snprintf(meta, tn, "%s", tr("default"));
        *playing = i == S.cur_theme;
        break;
    default: snprintf(main, mn, "%s", tr("-")); break;
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

static void covers_reset(int forget);

static void apply_theme(void)
{
    const theme_t *t = T();
    covers_reset(0);                       /* (the no-cover square takes the theme's colour) */
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

    /* the selection: a pill inside the margins, or a bar edge to edge (square) */
    int bar = t->selection_bar, pw = bar ? UI_W : UI_W - 2 * ROW_MARGIN;
    lv_obj_set_x(S.pill, bar ? 0 : ROW_MARGIN);
    lv_obj_set_width(S.pill, pw);
    lv_obj_set_x(S.pill_notch[1], pw - STEP);
    lv_obj_set_x(S.pill_notch[3], pw - STEP);
    int radius = t->corner == THEME_CORNER_ROUND && !bar ? t->radius : 0;
    lv_obj_set_style_radius(S.pill, radius, 0);
    fill(S.pill, t->select_bg);
    for (int k = 0; k < 4; k++) {
        fill(S.pill_notch[k], t->bg);
        show(S.pill_notch[k], t->corner == THEME_CORNER_STEPPED && !bar);
    }
    show(S.pill_cursor, t->cursor_arrow);
    style_label(S.pill_cursor, &lv_font_montserrat_14, t->select_fg);
    S.text_x0 = t->cursor_arrow ? 24 : 10;

    for (int r = 0; r < SLOTS; r++) {
        lv_obj_set_style_radius(S.row_dot[r], LV_RADIUS_CIRCLE, 0);
        fill(S.row_dot[r], t->signal);
        style_label(S.row_lbl[r], S.f->body, t->text);
        style_label(S.row_meta[r], S.f->meta, t->text_muted);
        style_label(S.row_lt[r], S.f->body, t->text);
        style_label(S.row_la[r], S.f->meta, t->text_muted);
        /* one line each (cut with "...", never wrapped), on one baseline */
        int lt = lv_font_get_line_height(S.f->body), la = lv_font_get_line_height(S.f->meta);
        int yt = (ROW_H - lt) / 2, base = yt + lt - S.f->body->base_line;
        lv_obj_set_height(S.row_lt[r], lt);
        lv_obj_set_height(S.row_la[r], la);
        lv_obj_set_y(S.row_lt[r], yt);
        lv_obj_set_y(S.row_la[r], base - (la - S.f->meta->base_line));
        S.line_on[r] = 0;          /* (laid out again with the new fonts) */
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

    /* Find cover */
    fill(S.cf, t->bg);
    lv_obj_set_style_radius(S.cf_art, radius, 0);
    fill(S.cf_art, t->surface);
    style_label(S.cf_title, S.f->title, t->text);
    style_label(S.cf_artist, S.f->sub, t->artist);
    style_label(S.cf_meta, S.f->meta, t->album);
    style_label(S.cf_note, S.f->meta, t->info);
    style_label(S.cf_hint, S.f->meta, t->text_muted);
}

/* Colour every row; `sel_slot` is the row the pill is over (text flips as the pill arrives). */
static void color_slots(int sel_slot)
{
    if (sel_slot == S.colored_slot) return;
    const theme_t *t = T();
    for (int j = 0; j < SLOTS; j++) {
        int sel = j == sel_slot, acc = S.slot_accent[j];
        set_text_color(S.row_lbl[j], C(sel ? t->select_fg : acc ? t->signal : t->text));
        set_text_color(S.row_meta[j], C(sel ? t->select_fg : acc ? t->signal : t->text_muted));
        set_text_color(S.row_lt[j], C(sel ? t->select_fg : t->text));
        set_text_color(S.row_la[j], C(sel ? t->select_fg : t->text_muted));
        set_text_color(S.row_icon[j], C(sel ? t->select_fg : acc ? t->signal : t->text_muted));
        lv_color_t ic = C(sel ? t->select_fg : acc ? t->signal : t->text_muted);
        if (!lv_color_eq(lv_obj_get_style_image_recolor(S.row_picon[j], 0), ic))
            lv_obj_set_style_image_recolor(S.row_picon[j], ic, 0);
        if (!lv_color_eq(lv_obj_get_style_image_recolor(S.row_mark[j], 0), ic))
            lv_obj_set_style_image_recolor(S.row_mark[j], ic, 0);
        for (int w = 0; w < 3; w++) set_text_color(S.row_ch[j][w], C(sel ? t->select_fg : w == 1 ? t->text : t->text_muted));
        for (int k = 0; k < 3; k++) fill(S.row_eq[j][k], sel ? t->select_fg : t->signal);
        /* the playing dot: on the selection too, in its text colour (hidden there, Home's
         * "Now playing" row was left with neither its icon nor the dot) */
        fill(S.row_dot[j], sel ? t->select_fg : t->signal);
        show(S.row_dot[j], S.slot_playing[j] && !t->playing_equalizer);
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

static void rows_box_place(void) { lv_obj_set_y(S.rows_box, S.list_y0 - ROW_PITCH + S.rows_off); }

static void anim_pill(void *var, int32_t v)
{
    (void)var;
    S.pill_y = quantize(v, S.pill_target);
    lv_obj_set_y(S.pill, S.list_y0 + S.pill_y);
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

/* A podcast episode on the card: an arrow down onto a line, 12 px, tinted
 * like the meta beside it (drawn: the themes' fonts share no such sign) */
#define MARK_PX 12
#define MARK_GAP 5
static const lv_image_dsc_t *on_card_mark(void)
{
    static const char *const MARK_ROWS[MARK_PX] = {
        ".....XX.....", ".....XX.....", ".....XX.....", ".....XX.....",
        ".XX..XX..XX.", "..XX.XX.XX..", "...XXXXXX...", "....XXXX....",
        ".....XX.....", "............", ".XXXXXXXXXX.", ".XXXXXXXXXX.",
    };
    static uint8_t px[MARK_PX * MARK_PX];
    static lv_image_dsc_t dsc;
    if (!dsc.data) {
        for (int y = 0; y < MARK_PX; y++)
            for (int x = 0; x < MARK_PX; x++) px[y * MARK_PX + x] = MARK_ROWS[y][x] == 'X' ? 255 : 0;
        dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        dsc.header.cf = LV_COLOR_FORMAT_A8;
        dsc.header.w = MARK_PX;
        dsc.header.h = MARK_PX;
        dsc.header.stride = MARK_PX;
        dsc.data_size = sizeof px;
        dsc.data = px;
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
    case L_HOME: case L_HOME_EDIT: case L_HISTORY: case L_SETTINGS: case L_WIFI_ASK: case L_UPD_ASK:
    case L_EQ_EDIT: case L_EQ_BAND: case L_THEME_EDIT: case L_THEME_COLOURS: case L_PODCASTS: case L_SHARE: case L_SSH: case L_ABOUT: case L_CONTEXT: case L_USB_ASK: case L_UPDATE:
    case L_UNINSTALL:
    case L_RDIR: case L_RDIR_GENRES: case L_BATTERY: case L_BAT_SPAN:
        return 1;
    default:
        return 0;
    }
}

/* Settings > Interface > Covers in lists (display.covers: 1 Albums, the default; 2 Songs):
 * each album's cover before it, or each song's album's, in place of the year
 * or the length */
static int covers_in(list_kind_t k)
{
    int v = settings_get_int("display.covers", UI_COVERS_DEFAULT);
    return (k == L_ALBUMS && (v & 1)) || (k == L_SONGS && (v & 2));
}

/* Albums' covers: the last COV_CACHE read, so scrolling a row reads one, not a
 * screenful (2.3 KB each from the card's art cache, no decoding), and each
 * visible row's pixels. Taken when Albums is drawn, given back when it goes
 * or the screen turns off: about 45 KB, and nothing elsewhere. */
#define COV_CACHE 12
static struct covers {
    struct cov { int64_t id; uint32_t used; int ok; uint16_t px[LIB_ART_SMALL * LIB_ART_SMALL]; } cache[COV_CACHE];
    uint16_t slot[SLOTS][LIB_ART_SMALL * LIB_ART_SMALL];
    uint32_t tick;
} *g_cov;

static int covers_take(void)
{
    if (g_cov) return 1;
    if (!(g_cov = calloc(1, sizeof *g_cov))) return 0;
    for (int j = 0; j < SLOTS; j++) {
        S.cov_dsc[j].data = (const uint8_t *)g_cov->slot[j];
        S.cov_dsc[j].data_size = sizeof g_cov->slot[j];
        S.cov_id[j] = -1;
    }
    return 1;
}

static void covers_free(void)
{
    if (!g_cov) return;
    for (int j = 0; j < SLOTS; j++) {
        show(S.row_cov[j], 0);
        lv_image_cache_drop(&S.cov_dsc[j]);
        S.cov_dsc[j].data = NULL;
        S.cov_id[j] = -1;
    }
    free(g_cov);
    g_cov = NULL;
    memset(&S.fill_sig, 0, sizeof S.fill_sig);   /* (the rows filled again when they're shown) */
}

static const uint16_t *cover_px(int64_t id)
{
    struct cov *e = &g_cov->cache[0];
    for (int k = 0; k < COV_CACHE; k++) {
        struct cov *c = &g_cov->cache[k];
        if (c->used && c->id == id) { c->used = ++g_cov->tick; return c->ok ? c->px : NULL; }
        if (c->used < e->used) e = c;              /* (the least recently used, or an empty one) */
    }
    e->id = id;
    e->used = ++g_cov->tick;
    e->ok = library_art(S.lib, id, LIB_ART_SMALL, e->px) == 0;
    return e->ok ? e->px : NULL;
}

/* (the theme changed, or the library: each row's cover drawn again; a new
 * library's covers read again) */
static void covers_reset(int forget)
{
    for (int j = 0; j < SLOTS; j++) S.cov_id[j] = -1;
    if (forget && g_cov) for (int k = 0; k < COV_CACHE; k++) g_cov->cache[k].used = 0;
}

/* ---------- album rows: title and artist on one line ----------
 * The title, then the album artist in the smaller muted font, on one
 * baseline, in a box that cuts them at the row's end ("..." on whichever
 * is cut; an artist with no room left goes). On the selected row, once the
 * marquee starts, the two scroll together as one line, as a long title does. */

#define MARQUEE_REST_MS 2000   /* the rest at each end of a scroll (LVGL's own: 0.3 s) */
#define LINE_GAP 8   /* between the title and the artist */

static int line_width(int j) { return lv_obj_get_style_width(S.row_line[j], 0); }

static void set_width_if(lv_obj_t *o, int w)
{
    if (lv_obj_get_style_width(o, 0) != w) lv_obj_set_width(o, w);
}

static void set_x_if(lv_obj_t *o, int x)
{
    if (lv_obj_get_style_x(o, 0) != x) lv_obj_set_x(o, x);
}

/* Both at rest: as much as fits, from the start. */
static void line_layout(int j)
{
    int bw = line_width(j), tw = S.line_tw[j], aw = S.line_aw[j];
    int min_a = lv_font_get_line_height(S.f->meta) * 2;   /* less room than this: no artist */
    set_x_if(S.row_lt[j], 0);
    if (tw + LINE_GAP + aw <= bw) {                          /* both in full */
        set_width_if(S.row_lt[j], tw);
        set_width_if(S.row_la[j], aw);
        show(S.row_la[j], aw > 0);
    } else if (tw + LINE_GAP + min_a > bw) {                 /* the title alone, cut */
        set_width_if(S.row_lt[j], tw < bw ? tw : bw);
        show(S.row_la[j], 0);
    } else {                                                 /* the title, the artist cut */
        set_width_if(S.row_lt[j], tw);
        set_width_if(S.row_la[j], bw - tw - LINE_GAP);
        show(S.row_la[j], 1);
    }
    set_x_if(S.row_la[j], tw + LINE_GAP);
}

static void line_scroll_cb(void *var, int32_t v)
{
    int j = (int)(intptr_t)lv_obj_get_user_data(var);
    lv_obj_set_x(S.row_lt[j], -v);
    lv_obj_set_x(S.row_la[j], S.line_tw[j] + LINE_GAP - v);
}

/* The selected row's line scrolls (on) or rests (off). */
static void line_scroll(int j, int on)
{
    int bw = line_width(j), all = S.line_tw[j] + LINE_GAP + S.line_aw[j];
    int running = lv_anim_get(S.row_line[j], line_scroll_cb) != NULL;
    if (on && S.line_aw[j] > 0 && all > bw) {
        if (running) return;
        set_width_if(S.row_lt[j], S.line_tw[j]);    /* in full: the box cuts them */
        set_width_if(S.row_la[j], S.line_aw[j]);
        show(S.row_la[j], 1);
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, S.row_line[j]);
        lv_anim_set_exec_cb(&a, line_scroll_cb);
        lv_anim_set_values(&a, 0, all - bw);
        lv_anim_set_duration(&a, lv_anim_speed_clamped(40, 300, 10000));   /* (a label's own speed) */
        lv_anim_set_playback_duration(&a, a.duration);
        lv_anim_set_playback_delay(&a, MARQUEE_REST_MS);
        lv_anim_set_repeat_delay(&a, MARQUEE_REST_MS);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_start(&a);
        return;
    }
    if (running) lv_anim_delete(S.row_line[j], line_scroll_cb);
    line_layout(j);
}

/* The copy set_text() keeps: whether a label already says s. */
static int text_is(lv_obj_t *l, const char *s)
{
    const char *had = lv_obj_get_user_data(l);
    return had && !strcmp(had, s);
}

static void fill_slots(const level_t *l, int n)
{
    for (int j = 0; j < SLOTS; j++) {
        int i = l->top + j - 1;
        int visible = i >= 0 && i < n;
        S.slot_item[j] = visible ? i : -1;
        S.slot_playing[j] = 0;
        S.slot_accent[j] = visible && l->kind == L_HOME && S.home[i] == H_UPDATE;
        S.slot_mark[j] = visible && l->kind == L_POD_SHOW && i < S.pod_ne && S.pod_on_card[i];
        show(S.row[j], visible);
        show(S.row_mark[j], 0);
        if (!visible) continue;
        char main[256], meta[128];
        int playing;
        list_item(l, i, main, sizeof main, meta, sizeof meta, &playing);
        S.slot_playing[j] = playing;
        if (ui_text_list(l->kind)) {               /* the UI's own words: translated */
            /* (only when there is a translation: copying a string onto itself is
             * undefined, and the H2's C library leaves it empty) */
            const char *tm = tr(main), *tt = tr(meta);
            if (tm != main) snprintf(main, sizeof main, "%s", tm);
            if (tt != meta) snprintf(meta, sizeof meta, "%s", tt);
        }

        int x = S.text_x0;
        int eq = playing && T()->playing_equalizer;

        /* Albums: the cover first, in a straight column; the playing marker after it */
        int cover = covers_in(l->kind) && S.lib && covers_take();   /* the album's cover (none: a plain square) */
        show(S.row_cov[j], cover);
        if (cover) {
            int64_t id = l->kind == L_ALBUMS ? l->items.ids[i] : library_track_album(S.lib, l->items.ids[i]);
            if (S.cov_id[j] != id) {
                const uint16_t *px = cover_px(id);
                uint16_t *to = g_cov->slot[j];
                if (px) memcpy(to, px, sizeof g_cov->slot[j]);
                else {
                    uint16_t c = (uint16_t)theme_rgb565(T()->surface);
                    for (size_t k = 0; k < sizeof g_cov->slot[j] / 2; k++) to[k] = c;
                }
                S.cov_id[j] = id;
                lv_image_cache_drop(&S.cov_dsc[j]);
                lv_image_set_src(S.row_cov[j], &S.cov_dsc[j]);
                lv_obj_invalidate(S.row_cov[j]);
            }
            if (lv_obj_get_style_x(S.row_cov[j], 0) != x) lv_obj_set_x(S.row_cov[j], x);
            x += LIB_ART_SMALL + ICON_GAP;
        }

        /* Home (and Settings > Home): an icon before each item; on Now playing,
         * while music plays, the equalizer (or dot) takes its place, centred in
         * the icons' column */
        int hid = l->kind == L_HOME ? S.home[i] : l->kind == L_HOME_EDIT ? S.home_order[i] : -1;
        uint32_t cp = hid < 0 ? 0 : hid == H_UPDATE ? UPDATE_ICON : HOME_ICON[hid];
        int pic = hid;
        if (l->kind == L_PLAYLISTS && i < l->items.n) {      /* Playlists: smart ones apart from yours */
            int smart = l->items.ids[i] < 0;
            cp = smart ? SMART_ICON : HOME_ICON[H_PLAYLISTS];
            pic = smart ? PIXEL_SMART : H_PLAYLISTS;
        }
        int pix = pic >= 0 && T()->icons_pixel;
        int glyph = pic >= 0 && !pix && S.f->icon;
        int col = pix || glyph;
        for (int k = 0; k < 3; k++) {          /* three bars, 11 px across */
            show(S.row_eq[j][k], eq);
            lv_obj_set_x(S.row_eq[j][k], x + (col ? (ICON_W - 11) / 2 : 0) + k * 4);
        }
        lv_obj_set_x(S.row_dot[j], x + (col ? (ICON_W - 8) / 2 : 0));
        show(S.row_icon[j], glyph && !playing);
        show(S.row_picon[j], pix && !playing);
        if (glyph && !playing) {
            char u[5];
            utf8_of(cp, u);
            set_text(S.row_icon[j], u);
            if (lv_obj_get_style_x(S.row_icon[j], 0) != x) lv_obj_set_x(S.row_icon[j], x);
        }
        if (pix && !playing) {
            const lv_image_dsc_t *img = pixel_icon(pic);
            if (lv_image_get_src(S.row_picon[j]) != img) lv_image_set_src(S.row_picon[j], img);
            int px = x + (ICON_W - PIXEL_ICON_GRID * PIXEL_ICON_SCALE) / 2;
            if (lv_obj_get_style_x(S.row_picon[j], 0) != px) lv_obj_set_x(S.row_picon[j], px);
        }
        if (col) x += ICON_W + ICON_GAP;
        else if (eq) x += 18;
        else if (playing) x += 14;
        int swatch = (l->kind == L_THEMES && i < S.n_themes) || l->kind == L_THEME_COLOURS;
        show(S.row_sw[j], swatch);
        if (swatch && l->kind == L_THEME_COLOURS) {   /* a colour of the theme being made */
            uint32_t c = the_get(i);
            fill(S.row_sw[j], c);
            lv_obj_set_style_border_color(S.row_sw[j], C(T()->text_muted), 0);
            lv_obj_set_style_radius(S.row_sw[j], T()->corner == THEME_CORNER_ROUND ? 4 : 0, 0);
            for (int q = 0; q < 3; q++) fill(S.row_sw_part[j][q], c);
            if (lv_obj_get_style_x(S.row_sw[j], 0) != x) lv_obj_set_x(S.row_sw[j], x);
            x += SWATCH_W + ICON_GAP;
        } else if (swatch) {   /* the theme's colours, in miniature */
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
            char wheel[3][WHEEL_CAP];
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
        int meta_w = 70;                           /* room for the meta: a wide one ("MP3 320" in a pixel font) gets more */
        lv_point_t sz = {0, 0};
        if (meta[0] && !typing) {
            lv_text_get_size(&sz, meta, S.f->meta, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
            if (sz.x + 18 > meta_w) meta_w = sz.x + 18;
        }
        if (S.slot_mark[j] && !typing) {          /* on the card: the mark, just before the meta */
            int mx = UI_W - 2 * ROW_MARGIN - 10 - (meta[0] ? sz.x + MARK_GAP : 0) - MARK_PX;
            if (lv_image_get_src(S.row_mark[j]) != on_card_mark()) lv_image_set_src(S.row_mark[j], on_card_mark());
            if (lv_obj_get_style_x(S.row_mark[j], 0) != mx) lv_obj_set_x(S.row_mark[j], mx);
            show(S.row_mark[j], 1);
            if (meta[0]) { if (sz.x + 18 + MARK_PX + MARK_GAP > meta_w) meta_w = sz.x + 18 + MARK_PX + MARK_GAP; }
            else meta_w = MARK_PX + 18;
        }
        int line = !typing && library_backed(l->kind) && library_row_shows_artist(view_of(l->kind)) &&
                   album_artists_on();
        S.line_on[j] = line;
        show(S.row_line[j], line);
        show(S.row_lbl[j], !line);
        show(S.row_meta[j], !line);
        if (line) {   /* an album: its title and artist on one line */
            int same = text_is(S.row_lt[j], main) && text_is(S.row_la[j], meta) &&
                       lv_obj_get_style_x(S.row_line[j], 0) == x;
            if (!same || !lv_anim_get(S.row_line[j], line_scroll_cb)) {   /* (a scrolling one left to scroll) */
                lv_point_t tz, az = {0, 0};
                lv_text_get_size(&tz, main, S.f->body, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
                if (meta[0]) lv_text_get_size(&az, meta, S.f->meta, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
                S.line_tw[j] = tz.x;
                S.line_aw[j] = az.x;
                set_pos_width(S.row_line[j], x, 0, UI_W - 2 * ROW_MARGIN - x - 10);
                set_text(S.row_lt[j], main);
                set_text(S.row_la[j], meta);
                lv_anim_delete(S.row_line[j], line_scroll_cb);
                line_layout(j);
            }
            continue;
        }
        lv_anim_delete(S.row_line[j], line_scroll_cb);   /* (one that scrolled, now hidden) */
        set_pos_width(S.row_lbl[j], x, 0, UI_W - 2 * ROW_MARGIN - x - (typing ? WHEEL_W + 10 : meta[0] ? meta_w : 10));
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
    int rows = rows_shown(l->kind);
    if (l->sel >= l->top + rows) l->top = l->sel - rows + 1;
    if (S.eq_graph && (l->kind == L_EQ_EDIT || l->kind == L_EQ_BAND)) {   /* the curve and the selected band */
        eqg_update();
        lv_obj_invalidate(S.eq_graph);
    }
    if (is_theme_page(l->kind)) the_preview();     /* the theme as it is now */

    char head[136];   /* "‹ " and a title of up to 127 bytes */
    const char *title = l->title_ui ? tr(l->title) : l->title;
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
                        : l->kind == L_SMART_TRACKS ? "Nothing here now"
                        : l->kind == L_FAVOURITES ? "No favourites yet: Menu on a song"
                        : l->kind == L_RECENT_PLAYED || l->kind == L_MOST_PLAYED ? "Nothing played yet"
                        : l->kind == L_BAT_HISTORY ? "No time on battery yet"
                        : l->kind == L_RDIR_RESULTS || l->kind == L_RDIR_COUNTRIES
                            ? (S.rd_job == RD_JOB_LIST ? "Searching\xE2\x80\xA6" : S.rd_state == RD_FAILED ? S.rd_msg
                               : l->kind == L_RDIR_COUNTRIES ? "No countries found" : "No stations found")
                        : library_backed(l->kind) ? "No music found" : "Nothing here";
        set_text(S.empty, tr(msg));
    }

    if (mode == R_CONTENT && !new_level) {
        color_slots(slot_under_pill());
    } else if (mode == R_SNAP || new_level) {
        stop_list_anims();
        S.pill_y = S.pill_target = target;
        S.rows_off = 0;
        graphs_place(l->kind);
        if (!covers_in(l->kind)) covers_free();     /* (a list without covers: their memory back) */
        lv_obj_set_y(S.pill, S.list_y0 + target);
        rows_box_place();
        color_slots(l->sel - l->top + 1);
        if (mode == R_ANIMATE && new_level)
            start_transition(S.depth >= S.last_depth ? TR_FORWARD : TR_BACK);
    } else if (top_delta != 0) {
        /* The list scrolled: the pill stays put and the rows slide under it. */
        lv_anim_delete(S.pill, anim_pill);
        S.pill_y = S.pill_target = target;
        lv_obj_set_y(S.pill, S.list_y0 + target);
        /* At most one row away: there's one spare row above and below (SLOTS),
         * and from further the edge row had nothing behind it, empty for a
         * moment while scrolling fast */
        int from = S.rows_off + top_delta * ROW_PITCH;
        if (from > ROW_PITCH) from = ROW_PITCH;
        if (from < -ROW_PITCH) from = -ROW_PITCH;
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
    if (S.now.podcast) {               /* the show's, kept small on the card */
        const player_episode_t *e = S.player ? player_episode(S.player, NULL, NULL) : NULL;
        char p[800];
        FILE *f = NULL;
        if (e && pod_dir()[0]) {
            pod_cover_path(pod_dir(), e->id, p, sizeof p);
            f = fopen(p, "rbe");
        }
        S.pod_cover = f && fread(S.art_px, 1, sizeof S.art_px, f) == sizeof S.art_px;
        if (f) fclose(f);
        if (S.pod_cover) {
            lv_image_cache_drop(&S.art_dsc);
            lv_image_set_src(S.np_img, &S.art_dsc);
            lv_obj_invalidate(S.np_img);
        }
        return;
    }
    int ok = S.lib && S.now.active &&
             library_art(S.lib, S.now.track.album_id, LIB_ART_LARGE, S.art_px) == 0;
    if (ok) {
        lv_image_cache_drop(&S.art_dsc);   /* same descriptor, new pixels */
        lv_image_set_src(S.np_img, &S.art_dsc);
        lv_obj_invalidate(S.np_img);
    }
    show(S.np_img, ok);
}

/* A Home item's icon, big, in the cover's place (the radio's, a podcast's) */
static void np_icon(int h)
{
    int pix = T()->icons_pixel;
    show(S.np_img, 0);
    show(S.np_radio_px, pix);
    show(S.np_radio, !pix && S.f->icon);
    if (pix && lv_image_get_src(S.np_radio_px) != pixel_icon_big(h)) {
        lv_image_set_src(S.np_radio_px, pixel_icon_big(h));
        lv_obj_center(S.np_radio_px);
    }
    if (!pix && S.f->icon) {
        char g[5];
        utf8_of(HOME_ICON[h], g);
        set_text(S.np_radio, g);
        if (lv_obj_get_style_align(S.np_radio, 0) != LV_ALIGN_CENTER) lv_obj_center(S.np_radio);   /* (once: it redraws) */
    }
}

/* Now Playing for a radio station: what it says is playing, the station, the
 * stream's format; time listened and "Live" under the bar, which stays empty. */
static void render_np_radio(void)
{
    const lib_track_t *t = &S.now.track;
    int art = S.radio_art;
    if (art) {
        if (lv_image_get_src(S.np_img) != &S.art_dsc) lv_image_set_src(S.np_img, &S.art_dsc);
        show(S.np_img, 1);
        show(S.np_radio_px, 0);
        show(S.np_radio, 0);
    } else {
        np_icon(H_RADIO);
    }
    set_text(S.h_left, tr("Radio"));
    /* The station in the big title (what's on is the question when listening to
     * radio), then the song and its artist, when the station says. (The player
     * gives the station as the album, and as the title when there's no song.) */
    int song = t->title[0] && strcmp(t->title, t->album);
    set_text(S.np_title, t->album);
    set_text(S.np_artist, song ? t->title : "");
    set_text(S.np_album, song ? t->artist : "");
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
    const radio_station_t *after = S.now.count > 1 && S.player ? player_radio_at(S.player, (S.now.index + 1) % S.now.count) : NULL;
    if (after) snprintf(nx, sizeof nx, "%s %s", tr("Next:"), after->name);
    set_text(S.np_next, nx);
    lv_obj_set_width(S.np_fill, 0);
    for (int i = 0; i < SEGMENTS; i++) fill(S.np_seg[i], T()->surface);
}

static void render_np(void)
{
    /* (the header's title is set once per render: setting it to one text and
     * then another redraws it, every second) */
    if (!S.now.active || S.now.radio || !(S.now.count > 1 && S.now.index >= 0 && S.now.index < S.now.count))
        set_text(S.h_left, tr(S.now.active && S.now.radio ? "Radio" : S.now.active && S.now.podcast ? "Podcast" : "Now playing"));
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
        set_text(S.np_out, "");          /* (or LVGL's placeholder "Text" shows until something plays) */
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
    const lib_track_t *t = &S.now.track;
    if (S.now.podcast) {               /* an episode: the show's cover (or the podcast icon), the show under the title */
        if (S.pod_cover) {
            if (lv_image_get_src(S.np_img) != &S.art_dsc) lv_image_set_src(S.np_img, &S.art_dsc);
            show(S.np_img, 1);
            show(S.np_radio, 0);
            show(S.np_radio_px, 0);
        } else {
            np_icon(H_PODCASTS);
        }
        set_text(S.np_title, t->title);
        set_text(S.np_artist, t->artist);
        int c = S.player && player_episode(S.player, NULL, NULL) &&
                !strcmp(player_episode(S.player, NULL, NULL)->key, S.pod_ch_key) ? pod_chapter_at(S.now.pos_ms) : -1;
        set_text(S.np_album, c >= 0 ? S.pod_ch[c].title : S.now.podcast_stream ? tr("Streaming") : "");
    } else {
        show(S.np_radio, 0);
        show(S.np_radio_px, 0);
        set_text(S.np_title, t->title);
        set_text(S.np_artist, t->artist);
        set_text(S.np_album, t->album);
    }

    char b[32], r[16];
    fmt_rate(r, sizeof r, t->sample_rate);
    if (t->bits == 1) snprintf(b, sizeof b, "%s", t->codec);          /* DSD: "DSD64" says it all */
    else if (t->bits) snprintf(b, sizeof b, "%s %u/%s", t->codec, t->bits, r);
    else snprintf(b, sizeof b, "%s %s", t->codec, r);
    if (S.eq_name[0] && !S.pure) snprintf(b + strlen(b), sizeof b - strlen(b), " \xC2\xB7 EQ");   /* not bit-perfect */
    set_text(S.np_chip, b);
    show(S.np_chip, !S.now.podcast || t->sample_rate);   /* (an episode not started yet: no format) */

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
    if (S.now.podcast) ;               /* (no queue: Next and Previous skip) */
    else if (S.now.stop_after) snprintf(nx, sizeof nx, "%s", tr("Stops after this track"));
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
    lv_label_set_text(n, tr(note));
    lv_obj_set_style_text_color(n, C(exact ? (t->dark ? 0xA8F0D1 : 0x1E7A55) : t->text_muted), 0);   /* mint */
    show(f, format[0] != '\0');
    show(n, note[0] != '\0');
    lv_label_set_text(h, tr(hint));
    lv_obj_align(h, LV_ALIGN_TOP_MID, 0, format[0] ? 186 : 112);
}

static void render(render_mode_t mode);

/* While an update downloads, unpacks or installs: a screen over everything,
 * with a bar. Back cancels until it installs (the music plays on: Play/Pause,
 * Prev and Next and the volume still work); when it's ready, Play installs. */
static void update_overlay_close(void)
{
    if (S.upd_ov) lv_obj_delete(S.upd_ov);
    S.upd_ov = NULL;
}

/* A translated text's "%s" filled in with a, then b: as text, never as a
 * printf format (a translation file could hold anything). */

static void update_overlay(void)
{
    const ui_update_t *u = &S.upd;
    if (u->state < UI_UPD_DOWNLOADING || u->state > UI_UPD_RESTARTING) { update_overlay_close(); return; }
    const theme_t *t = T();
    if (!S.upd_ov) {
        /* a centred column (a title or text that wraps, translated or in a
         * wide font, pushes the rest down), the hint at the foot */
        S.upd_ov = box(lv_layer_top(), 0, 0, UI_W, UI_H);
        fill(S.upd_ov, t->bg);
        lv_obj_t *col = box(S.upd_ov, 0, 0, UI_W, UI_H - 36);
        lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(col, 16, 0);
        S.upd_title = lv_label_create(col);
        style_label(S.upd_title, S.f->title, t->text);
        lv_obj_set_width(S.upd_title, UI_W - 32);
        lv_obj_set_style_text_align(S.upd_title, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(S.upd_title, LV_LABEL_LONG_WRAP);
        S.upd_bar = lv_bar_create(col);
        lv_obj_set_size(S.upd_bar, UI_W - 64, 12);
        int r = t->corner == THEME_CORNER_ROUND ? 6 : 0;
        lv_obj_set_style_radius(S.upd_bar, r, LV_PART_MAIN);
        lv_obj_set_style_radius(S.upd_bar, r, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(S.upd_bar, C(t->surface), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(S.upd_bar, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(S.upd_bar, C(t->signal), LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(S.upd_bar, LV_OPA_COVER, LV_PART_INDICATOR);   /* (no theme to set it) */
        lv_bar_set_range(S.upd_bar, 0, 1000);
        S.upd_detail = lv_label_create(col);
        style_label(S.upd_detail, S.f->body, t->text);
        lv_obj_set_width(S.upd_detail, UI_W - 32);
        lv_obj_set_style_text_align(S.upd_detail, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(S.upd_detail, LV_LABEL_LONG_WRAP);
        S.upd_hint = lv_label_create(S.upd_ov);
        style_label(S.upd_hint, S.f->body, t->text_muted);   /* (the lists' size: what to press, read at a glance) */
        lv_obj_set_width(S.upd_hint, UI_W - 32);
        lv_obj_set_style_text_align(S.upd_hint, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(S.upd_hint, LV_LABEL_LONG_WRAP);
        lv_obj_align(S.upd_hint, LV_ALIGN_BOTTOM_MID, 0, -12);
        lv_obj_move_background(S.upd_ov);            /* toasts and the volume bar stay over it */
    }
    char title[96], detail[96], a[24], b[24];
    const char *hint = "Back cancels";
    int permille = u->total ? (int)(u->done * 1000 / u->total) : 0;
    detail[0] = '\0';
    fmt_mb(a, sizeof a, u->done);
    fmt_mb(b, sizeof b, u->total);
    switch (u->state) {
    case UI_UPD_DOWNLOADING:
        subst(title, sizeof title, tr("Downloading %s"), u->latest, NULL);
        if (u->total) subst(detail, sizeof detail, tr("%s of %s"), a, b);
        break;
    case UI_UPD_VERIFYING:
        snprintf(title, sizeof title, "%s", tr("Checking the download"));
        break;
    case UI_UPD_UNPACKING:
        subst(title, sizeof title, tr("Unpacking %s"), u->latest, NULL);
        if (u->total) subst(detail, sizeof detail, tr("%s of %s"), a, b);
        break;
    case UI_UPD_READY:
        subst(title, sizeof title, tr("Ready to install %s"), u->latest, NULL);
        snprintf(detail, sizeof detail, "%s", tr("The H2 restarts to finish. Your settings and music stay."));
        hint = "Play installs, Back cancels";
        permille = 1000;
        break;
    case UI_UPD_INSTALLING:
        subst(title, sizeof title, tr("Installing %s"), u->latest, NULL);
        snprintf(detail, sizeof detail, "%s", tr("Don't turn the H2 off"));
        hint = "";
        permille = 1000;
        break;
    default:   /* UI_UPD_RESTARTING */
        snprintf(title, sizeof title, "%s", tr("Restarting"));
        hint = "";
        permille = 1000;
        break;
    }
    lv_label_set_text(S.upd_title, title);
    lv_label_set_text(S.upd_detail, detail);
    show(S.upd_detail, detail[0] != '\0');
    lv_label_set_text(S.upd_hint, tr(hint));
    lv_bar_set_value(S.upd_bar, permille, LV_ANIM_OFF);
}

/* ---------- What's new ----------
 * After an update, once: the release's points (ui_set_whats_new), each with
 * a dot in the theme's accent (drawn: the theme fonts share almost no
 * symbols), under a title. When they don't all fit, the wheel moves through
 * them a whole point at a time, and only points that fit entirely are drawn
 * (no line cut at an edge); Play or Back puts it away. Settings > About >
 * What's new shows it again. */

static void whats_new_close(void)
{
    if (S.wn_ov) lv_obj_delete(S.wn_ov);
    S.wn_ov = S.wn_body = S.wn_hint = NULL;
    S.wn_n = 0;
}

/* The points from wn_first on that fit whole (at least one), and the hint:
 * "Turn for more" while some are left below, then how to put it away */
static void whats_new_show(void)
{
    if (!S.wn_body || !S.wn_n) return;
    int room = lv_obj_get_content_height(S.wn_body), gap = lv_obj_get_style_pad_row(S.wn_body, 0), used = 0;
    S.wn_last = S.wn_first;
    for (int i = 0; i < S.wn_n; i++) {
        int on = i >= S.wn_first && (i == S.wn_first || used + gap + S.wn_h[i] <= room);
        if (on) { used += (i > S.wn_first ? gap : 0) + S.wn_h[i]; S.wn_last = i; }
        else if (i > S.wn_first) used = room + 1;    /* (none after one that doesn't fit) */
        show(S.wn_row[i], on);
    }
    if (S.wn_hint)
        lv_label_set_text(S.wn_hint, tr(S.wn_last < S.wn_n - 1 ? "Turn for more, Play: done" : "Play or Back: done"));
}

static void whats_new_open(void)
{
    if (!S.wn_text[0]) return;
    whats_new_close();
    const theme_t *t = T();
    S.wn_ov = box(lv_layer_top(), 0, 0, UI_W, UI_H);
    fill(S.wn_ov, t->bg);
    lv_obj_t *col = box(S.wn_ov, 0, 0, UI_W, UI_H);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(col, 16, 0);
    lv_obj_set_style_pad_top(col, 14, 0);
    lv_obj_set_style_pad_bottom(col, 10, 0);
    lv_obj_set_style_pad_row(col, 10, 0);
    lv_obj_t *title = lv_label_create(col);       /* (the lists' size, as the points) */
    style_label(title, S.f->body, t->text);
    lv_obj_set_width(title, LV_PCT(100));
    lv_label_set_long_mode(title, LV_LABEL_LONG_WRAP);
    char tt[96];
    subst(tt, sizeof tt, tr("What's new in %s"), S.wn_ver, NULL);
    lv_label_set_text(title, tt);
    S.wn_body = box(col, 0, 0, LV_PCT(100), 10);
    lv_obj_set_flex_grow(S.wn_body, 1);
    lv_obj_set_flex_flow(S.wn_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(S.wn_body, 8, 0);
    S.wn_n = S.wn_first = 0;
    const lv_font_t *pf = S.f->body;              /* (the lists' 20 px: easy to read; the wheel scrolls the rest) */
    int lh = lv_font_get_line_height(pf), dot = 7;
    for (const char *p = S.wn_text; *p;) {
        const char *e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        char line[300];
        snprintf(line, sizeof line, "%.*s", (int)(n < sizeof line ? n : sizeof line - 1), p);
        p += n + (e != NULL);
        if (!line[0] || S.wn_n == (int)(sizeof S.wn_row / sizeof S.wn_row[0])) continue;
        lv_obj_t *row = box(S.wn_body, 0, 0, LV_PCT(100), LV_SIZE_CONTENT);
        S.wn_row[S.wn_n++] = row;
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_column(row, 10, 0);
        lv_obj_t *d = box(row, 0, 0, dot, dot);
        fill(d, t->signal);
        lv_obj_set_style_radius(d, t->corner == THEME_CORNER_ROUND ? LV_RADIUS_CIRCLE : 0, 0);
        lv_obj_set_style_margin_top(d, (lh - dot) / 2, 0);
        lv_obj_t *l = lv_label_create(row);
        style_label(l, pf, t->text);
        lv_obj_set_flex_grow(l, 1);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_label_set_text(l, tr(line));
    }
    S.wn_hint = lv_label_create(col);
    style_label(S.wn_hint, S.f->meta, t->text_muted);
    lv_obj_set_width(S.wn_hint, LV_PCT(100));
    lv_obj_set_style_text_align(S.wn_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_move_background(S.wn_ov);            /* (toasts and the volume bar stay over it) */
    lv_obj_update_layout(S.wn_ov);               /* each point's height, all of them shown */
    for (int i = 0; i < S.wn_n; i++) S.wn_h[i] = lv_obj_get_height(S.wn_row[i]);
    whats_new_show();
}

/* A key while it's up: 1 if it was its (the wheel scrolls; Play, Back, Menu
 * put it away; the media keys and the volume are the player's as ever) */
static int whats_new_key(ui_key_t k)
{
    switch (k) {
    case UI_KEY_UP: case UI_KEY_DOWN:
        if (k == UI_KEY_DOWN && S.wn_last < S.wn_n - 1) S.wn_first++;
        if (k == UI_KEY_UP && S.wn_first > 0) S.wn_first--;
        whats_new_show();
        return 1;
    case UI_KEY_OK: case UI_KEY_BACK: case UI_KEY_BACK_LONG: case UI_KEY_MENU:
        whats_new_close();
        return 1;
    default:
        return 0;
    }
}

void ui_set_whats_new(const char *version, const char *text, int show)
{
    snprintf(S.wn_ver, sizeof S.wn_ver, "%s", version ? version : "");
    snprintf(S.wn_text, sizeof S.wn_text, "%s", text ? text : "");
    if (show) whats_new_open();
}

/* A key while the progress screen is up: 1 if it was the screen's. */
static int update_overlay_key(ui_key_t k)
{
    switch (k) {
    case UI_KEY_PLAY: case UI_KEY_PREV: case UI_KEY_NEXT: case UI_KEY_SEEK_BACK: case UI_KEY_SEEK_FWD:
    case UI_KEY_BRIGHTER: case UI_KEY_DIMMER:
        return 0;                                   /* the music's and the screen's, as anywhere */
    case UI_KEY_BACK:
        if (S.upd.state < UI_UPD_INSTALLING && S.hooks.update_do) {
            S.hooks.update_do(UI_UPD_CANCEL);
            ui_toast("Update cancelled", 1500);
            ui_update_changed();
        }
        return 1;
    case UI_KEY_OK:
        if (S.upd.state == UI_UPD_READY && S.hooks.update_do) {
            S.hooks.update_do(UI_UPD_INSTALL);
            ui_update_changed();
        }
        return 1;
    default:
        return 1;
    }
}

static void update_ok(int sel)
{
    if (!S.hooks.update_do) return;
    update_look();
    int st = S.upd.state;
    if (st == UI_UPD_AVAILABLE && sel == 0) S.hooks.update_do(UI_UPD_START);
    else if (st == UI_UPD_FAILED && sel == 0) {                  /* (whole, if the row cuts it) */
        char why[200];
        update_failure(why, sizeof why);
        ui_toast(why, 3000);
    }
    else if (st == UI_UPD_IDLE || st == UI_UPD_LATEST || st == UI_UPD_FAILED) {
        if (!wifi_ask_if_off(WT_OK)) S.hooks.update_do(UI_UPD_CHECK);
    }
    ui_update_changed();
}

static void pop(void);

/* Uninstall Fathom, over everything:
 *   made ready: what happens next, until the H2 restarts into HiBy's recovery
 *   (OK) or it's cancelled (Back);
 *   no update file on the card: where to get one (OK looks again, Back goes back). */
static int uni_ov_pending;      /* which of the two is up */

static void uninstall_overlay_close(void)
{
    if (S.uni_ov) lv_obj_delete(S.uni_ov);
    S.uni_ov = NULL;
}

static void uninstall_overlay(void)
{
    level_t *l = &S.stack[S.depth - 1];
    int pending = S.uni.state == UI_UNI_PENDING, none = S.uni.state == UI_UNI_FAILED && S.uni.no_file;
    if ((!pending && !none) || l->kind != L_UNINSTALL) { uninstall_overlay_close(); return; }
    if (S.uni_ov && uni_ov_pending == pending) return;
    uninstall_overlay_close();
    uni_ov_pending = pending;
    const theme_t *t = T();
    S.uni_ov = box(lv_layer_top(), 0, 0, UI_W, UI_H);
    fill(S.uni_ov, t->bg);
    lv_obj_t *col = box(S.uni_ov, 0, 0, UI_W, UI_H - 36);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(col, 12, 0);
    lv_obj_t *title = lv_label_create(col);
    style_label(title, S.f->title, t->text);
    lv_obj_set_width(title, UI_W - 32);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(title, LV_LABEL_LONG_WRAP);
    lv_label_set_text(title, tr(pending ? "Ready to uninstall" : "No update file"));
    lv_obj_t *body = lv_label_create(col);
    style_label(body, S.f->body, t->text);
    lv_obj_set_width(body, UI_W - 32);
    lv_obj_set_style_text_align(body, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    /* (each text on one line: tools/lang/strings.py reads them so) */
    static const char *const ready = "The H2 restarts into HiBy's recovery, which puts HiBy's firmware back. Don't turn it off meanwhile.";
    static const char *const where = "Get the H2's firmware from HiFi Walker's website (its version or newer) and put the .upt file at the top of the card or in Music.";
    lv_label_set_text(body, tr(pending ? ready : where));
    lv_obj_t *hint = lv_label_create(S.uni_ov);
    style_label(hint, S.f->meta, t->text_muted);
    lv_obj_set_width(hint, UI_W - 32);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_label_set_text(hint, tr(pending ? "Play restarts, Back cancels" : "Play looks again"));
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -12);
    /* the text centred in the room above the hint (two lines of it in a wide font) */
    lv_obj_update_layout(hint);
    lv_obj_set_height(col, UI_H - 12 - lv_obj_get_height(hint) - 8);
    lv_obj_set_style_pad_row(col, 8, 0);
    lv_obj_move_background(S.uni_ov);                /* toasts and the volume bar stay over it */
}

/* A key while that screen is up: 1 if it was the screen's. */
static int uninstall_overlay_key(ui_key_t k)
{
    switch (k) {
    case UI_KEY_PLAY: case UI_KEY_PREV: case UI_KEY_NEXT: case UI_KEY_SEEK_BACK: case UI_KEY_SEEK_FWD:
    case UI_KEY_BRIGHTER: case UI_KEY_DIMMER:
        return 0;                                   /* the music's and the screen's, as anywhere */
    case UI_KEY_BACK: case UI_KEY_BACK_LONG:
        if (uni_ov_pending) S.hooks.uninstall_do(UI_UNI_CANCEL);
        uninstall_overlay_close();
        pop();
        if (uni_ov_pending) ui_toast("Uninstall cancelled", 2000);
        render(R_ANIMATE);
        return 1;
    case UI_KEY_OK:
        S.hooks.uninstall_do(uni_ov_pending ? UI_UNI_RESTART : UI_UNI_CHECK);
        ui_uninstall_changed();
        return 1;
    default:
        return 1;
    }
}

static void uninstall_ok(int sel)
{
    if (!S.hooks.uninstall_do) return;
    uninstall_look();
    switch (uninstall_row(sel)) {
    case UR_LOOK: S.hooks.uninstall_do(UI_UNI_CHECK); break;
    case UR_GO: {                                   /* press twice */
        uint32_t now = lv_tick_get();
        if (S.uni_armed && now - S.uni_armed < 3000) {
            S.uni_armed = 0;
            S.hooks.uninstall_do(UI_UNI_APPLY);
        } else {
            S.uni_armed = now;
            ui_toast("Press again to uninstall Fathom", 3000);
        }
        break;
    }
    case UR_STATUS:
        if (S.uni.state == UI_UNI_FAILED) {         /* (whole, if the row cuts it) */
            char why[200];
            uninstall_failure(why, sizeof why);
            ui_toast(why, 3000);
        }
        break;
    default: break;
    }
    ui_uninstall_changed();
}

void ui_uninstall_changed(void)
{
    uninstall_look();
    uninstall_overlay();
    level_t *l = &S.stack[S.depth - 1];
    if (l->kind != L_UNINSTALL || S.now_playing_view) return;
    int n = list_count(l), sel = l->sel;
    if (l->sel >= n) l->sel = n - 1;
    if (l->sel < 0) l->sel = 0;
    render(l->sel != sel ? R_SNAP : R_CONTENT);   /* (the list got shorter under the selection: it moves) */
}

void ui_update_changed(void)
{
    update_look();
    update_overlay();
    level_t *l = &S.stack[S.depth - 1];
    if (l->kind != L_UPDATE || S.now_playing_view) return;
    int n = list_count(l), sel = l->sel;
    if (l->sel >= n) l->sel = n - 1;
    render(l->sel != sel ? R_SNAP : R_CONTENT);   /* (Try again: the list got shorter under the selection) */
}

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

static void toast_show(const char *text, int ms, const lv_font_t *font)
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
    lv_obj_set_style_text_font(S.toast_lbl, font, 0);
    lv_obj_set_style_text_color(S.toast_lbl, C(t->text), 0);
    /* One line when it fits the screen (with its margins), else wrapped and
     * centred on as many lines as it takes. */
    int32_t maxw = lv_obj_get_width(lv_layer_top()) - 2 * 12 - 2 * 14;
    lv_point_t sz;
    lv_text_get_size(&sz, text, font, lv_obj_get_style_text_letter_space(S.toast_lbl, 0), 0, LV_COORD_MAX,
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
    S.help_shown = 0;
}

void ui_toast(const char *text, int ms) { toast_show(text, ms, S.f->body); }

/* ---------- Menu in Settings: what the row is for ----------
 * Two or three lines in the artist line's font, over the list, until the next key (or
 * a while: longer texts stay longer). Rows where Menu does something else
 * (Home's Move, forgetting a device or a network) have none. */

static const char *setting_help(setting_t st)
{
    switch (st) {
    case ST_THEME: return "Pick how Fathom looks. More themes can go in fathom/themes.";
    case ST_COVERS: return "Show album covers when browsing Albums/Songs.";
    case ST_ALBUM_ARTISTS: return "Display artist name when browsing albums.";
    case ST_BRIGHTNESS: return "How bright the screen is. Press Play, then turn the wheel.";
    case ST_TIMEOUT: return "How long the screen stays on. Saves battery.";
    case ST_DIM: return "Dims the screen 5 seconds before it turns off.";
    case ST_ON_CHARGING: return "Keeps the screen on while the H2 is plugged in.";
    case ST_LANGUAGE: return "Interface language. More in fathom/lang.";
    case ST_OUTPUT: return "Where the sound goes. Play shows each step in the path.";
    case ST_PURE: return "Skips EQ, balance and levelling, for bit-perfect sound.";
    case ST_EQ: return "Shape the sound with an equaliser. Each output keeps its own.";
    case ST_BALANCE: return "Left/right balance. Press Play, then turn the wheel.";
    case ST_CROSSFEED: return "Each ear hears a little of the other side, as with speakers.";
    case ST_VOL_LIMIT: return "Caps the volume, to protect your ears. Per output.";
    case ST_LEVELLING: return "Plays songs at the same loudness, or albums as mastered.";
    case ST_TRANSPORT: return "With a USB DAC: Bluetooth and screen off for the cleanest sound.";
    case ST_USB_DAC: return "Use the H2 as an external USB DAC.";
    case ST_DAC_FILTER: return "The DAC chip's filter, for headphones and line out. To taste.";
    case ST_SHUFFLE: return "Shuffle current playlist.";
    case ST_SHUF_BY: return "Shuffle songs or shuffle albums.";
    case ST_SHUF_FRESH: return "Songs you haven't heard in a while come up sooner.";
    case ST_SHUF_INTERLUDES: return "Leaves short intros, skits and interludes out of shuffle.";
    case ST_SHUF_WORKS: return "Keeps a work's parts together and in order, like a symphony's movements.";
    case ST_SHUF_SPOKEN: return "Leaves out tracks whose genre is audiobook, podcast or spoken.";
    case ST_REPEAT: return "Repeat all, or one song.";
    case ST_PAUSE_UNPLUG: return "Pauses when wired headphones are unplugged.";
    case ST_BATTERY: return "Charge and time left. Play shows the battery history.";
    case ST_AUTO_OFF: return "Turns the H2 off when it's not in use.";
    case ST_POD_AUTO: return "Your shows' new episodes download by themselves, while charging on Wi-Fi.";
    case ST_POD_KEEP: return "How many of each show's newest unplayed episodes are downloaded.";
    case ST_POD_CLEAN: return "A played episode's download is deleted after this long.";
    case ST_POD_SPEED: return "Episodes faster, without the voices going up. Music always plays as it is.";
    case ST_SLEEP: return "Stops the music after a set time or at the end of the album.";
    case ST_RESCAN: return "Force a library rescan.";
    case ST_FIND_COVERS: return "Finds covers online for albums without one. Keeps only sure matches.";
    case ST_N_SONGS: case ST_N_ALBUMS: case ST_N_ARTISTS: case ST_N_PLAYLISTS: return "Library counts.";
    case ST_USB_COMPUTER: return "What to do when plugged into a computer.";
    case ST_USB_DRIVE: return "Use the H2 as a USB drive, to copy music.";
    default: return NULL;
    }
}

static const char *page_help(int set)
{
    static const char *const H[] = {
        [SET_INTERFACE] = "Themes, list settings and languages.",
        [SET_HOME] = "Show, hide and move the items on the Home screen.",
        [SET_DISPLAY] = "Screen brightness and time-outs.",
        [SET_SOUND] = "Output, EQ, balance, volume and levelling.",
        [SET_BLUETOOTH] = "Pair and connect Bluetooth headphones and speakers.",
        [SET_WIFI] = "Join and manage Wi-Fi networks.",
        [SET_SHARE] = "Copy music to the H2 from a computer, over Wi-Fi.",
        [SET_SSH] = "Connect to the H2 through SSH.",
        [SET_USB] = "What the H2 does when you plug it into a computer.",
        [SET_PLAYBACK] = "Shuffle options, repeat and pause on unplug.",
        [SET_PODCASTS] = "Downloading new episodes, keeping and deleting them, speed.",
        [SET_POWER] = "Battery, sleep timer and auto power off.",
        [SET_LIBRARY] = "Rescan library and library stats.",
        [SET_ABOUT] = "Device info and firmware updates.",
    };
    return set >= 0 && set < (int)(sizeof H / sizeof H[0]) ? H[set] : NULL;
}

static const char *row_help(const level_t *l)
{
    int i = l->sel, n = list_count(l), w;
    switch (l->kind) {
    case L_SETTINGS: return page_help(i);
    case L_BLUETOOTH:
        if (!S.hooks.bt_state) return NULL;
        return i == 0 ? "Turns Bluetooth on or off. Off saves battery."
             : i == 1 ? "Search for available Bluetooth devices."
             : i == 2 ? "Optimize sound quality vs Bluetooth range." : NULL;
    case L_WIFI:
        switch (wifi_row(i, &w)) {
        case WR_TOGGLE: return "Turns Wi-Fi on or off. Off saves battery.";
        case WR_LINK: return "The network you're on. Play shows its address and signal.";
        case WR_SEARCH: return "Refresh the list of networks.";
        default: return NULL;
        }
    case L_SHARE:
        if (!S.hooks.share_state) return NULL;
        if (i == 0) return "Turns the network drive on or off.";
        if (S.share.state != UI_SHARE_ON) return NULL;
        return i == 1 ? "The name to type on your computer to connect."
             : i == 2 ? "Use this IP if you can't connect with the hostname."
             : i == 3 || i == 4 ? "Username and password for the network drive."
             : i == 5 ? "Check it matches when your computer warns about the connection."
             : "Generate a new password.";
    case L_SSH:
        if (!S.hooks.ssh_state) return NULL;
        if (i == 0) return "Turns SSH on or off.";
        if (S.ssh.state != UI_SSH_ON) return NULL;
        return i == 1 || i == 2 ? "The address to connect to, over USB or Wi-Fi."
             : i == 3 || i == 4 ? "Username and password for SSH."
             : "Generate a new password.";
    case L_ABOUT:
        if (i >= n) return NULL;
        switch (about_row(i)) {
        case AB_RESET: return "Reset all settings to defaults. Press Play twice.";
        case AB_UNINSTALL: return "Put the H2 back on HiBy's own firmware.";
        case AB_AUTO: return "Check for updates once a day over Wi-Fi.";
        case AB_CHECK: return "Check for a newer version of Fathom.";
        default:
            if (!strcmp(S.about_lbl[i], "Clock")) return "Set from the internet over Wi-Fi. In UTC.";
            if (!strcmp(S.about_lbl[i], "Charge cycles")) return "Battery wear, in full charges used.";
            return NULL;
        }
    case L_UPD_ASK:
        return i == 0 ? "Download and install it: Settings > About > Updates."
                      : "Hide it from Home until a newer version is out.";
    case L_UNINSTALL:
        if (i >= n) return NULL;
        switch (uninstall_row(i)) {
        case UR_LOOK: return "Put HiBy's update file for the H2 at the top of the card or in Music.";
        case UR_VERSION: return "Use your H2's version of HiBy's firmware, or a newer one.";
        case UR_GO: return "Press Play twice. Your music stays on the card.";
        default: return NULL;
        }
    default:
        return is_setting_level(l->kind) && l->kind != L_QUICK && i < n ? setting_help(setting_at(l, i)) : NULL;
    }
}

/* Menu on a Settings row: its help, if it has one (1). */
static int show_help(const level_t *l)
{
    const char *h = S.now_playing_view ? NULL : row_help(l);
    if (!h) return 0;
    int ms = 2000 + 45 * (int)strlen(h);
    toast_show(h, ms > 7000 ? 7000 : ms, S.f->sub);   /* (between the rows and the meta: three lines at most) */
    S.help_shown = 1;
    return 1;
}

/* Any key puts a help away (and then does what it does). */
static void help_hide(void)
{
    if (!S.help_shown) return;
    S.help_shown = 0;
    if (S.toast_timer) lv_timer_delete(S.toast_timer);
    toast_hide(NULL);
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
    lv_obj_set_style_bg_opa(S.lvl_bar, LV_OPA_COVER, LV_PART_INDICATOR);   /* (no theme to set it) */
    lv_bar_set_range(S.lvl_bar, 0, max);
    lv_bar_set_value(S.lvl_bar, value, LV_ANIM_OFF);
    if (value_text) lv_label_set_text_fmt(S.lvl_lbl, "%s  %s", tr(text), value_text);
    else lv_label_set_text_fmt(S.lvl_lbl, "%s  %d / %d", tr(text), value, max);
    lv_obj_remove_flag(S.lvl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(S.lvl, LV_ALIGN_BOTTOM_MID, 0, -16);
    if (S.toast) lv_obj_add_flag(S.toast, LV_OBJ_FLAG_HIDDEN);   /* one overlay at a time */
    S.help_shown = 0;   /* (a help it hid: the next Menu isn't "put it away") */
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

/* Find cover: the match shown, its picture (or why there's none), and what OK does */
static void render_cf(void)
{
    int n = 0, st = cf_find_state(&n);
    cf_match_t m;
    memset(&m, 0, sizeof m);
    if (S.cf_k >= n) S.cf_k = n ? n - 1 : 0;
    int pic = st == 1 && n ? cf_find_show(S.cf_k, &m, S.cf_px) : -1;
    S.cf_drawn = (st + 2) | (n << 3) | ((pic + 2) << 8) | (S.cf_k << 11);
    char meta[32] = "", hint[96], head[136];
    snprintf(head, sizeof head, "\xe2\x80\xb9 %s", S.stack[S.depth - 1].title);   /* "‹ Album" */
    set_text(S.h_left, head);
    if (pic == 1) {
        lv_image_cache_drop(&S.cf_dsc);            /* same descriptor, new pixels */
        lv_image_set_src(S.cf_img, &S.cf_dsc);
        lv_obj_invalidate(S.cf_img);
    }
    show(S.cf_img, pic == 1);
    if (st == 1 && n) {
        set_text(S.cf_title, m.title);
        set_text(S.cf_artist, m.artist);
        if (m.year) snprintf(meta, sizeof meta, "%d", m.year);
        set_text(S.cf_meta, meta);
        char src[64];
        subst(src, sizeof src, tr("Source: %s"), m.source == CF_APPLE ? "Apple" : "MusicBrainz", NULL);
        set_text(S.cf_note, pic == -2 ? tr("Couldn't fetch it") : src);
        snprintf(hint, sizeof hint, "%d / %d  %s", S.cf_k + 1, n,
                 tr(pic == 1 ? "Play: use this cover" : pic == 0 ? "Fetching its picture\xE2\x80\xA6"
                    : pic == -2 ? "Play: try again" : "No picture"));
        set_text(S.cf_hint, hint);
    } else {
        set_text(S.cf_title, st == 0 ? tr("Looking\xE2\x80\xA6") : st == -2 ? tr("Couldn't look it up") : tr("Nothing found"));
        set_text(S.cf_artist, "");
        set_text(S.cf_meta, "");
        set_text(S.cf_note, st == -2 ? tr("No network, or no answer") : "");
        set_text(S.cf_hint, st == 0 ? "" : tr("Back: done"));
    }
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
    int cfv = !S.now_playing_view && S.stack[S.depth - 1].kind == L_COVER_FIND;
    show(S.list, !S.now_playing_view && !cfv);
    show(S.np, S.now_playing_view);
    show(S.cf, cfv);
    render_header_status();
    if (cfv) {
        render_cf();
    } else if (S.now_playing_view) {
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

/* Playlists: the smart ones that have songs (as -LIB_SMART_*), then yours */
static void playlists_load(level_t *l)
{
    library_list_free(&l->items);
    if (!S.lib) return;
    lib_list_t yours;
    if (library_list(S.lib, LIB_PLAYLISTS, 0, &yours) < 0) memset(&yours, 0, sizeof yours);
    int64_t *ids = malloc((size_t)(yours.n + LIB_SMART_N) * sizeof *ids);
    if (!ids) { l->items = yours; return; }
    int n = 0;
    for (int k = 1; k < LIB_SMART_N; k++) {
        lib_list_t t;
        S.smart_n[k] = library_list(S.lib, LIB_SMART, k, &t);
        library_list_free(&t);
        if (S.smart_n[k] > 0) ids[n++] = -k;
    }
    if (yours.n) memcpy(ids + n, yours.ids, (size_t)yours.n * sizeof *ids);
    n += yours.n;
    library_list_free(&yours);
    l->items.view = LIB_PLAYLISTS;
    l->items.ids = ids;
    l->items.n = n;
}

static void push_as(list_kind_t kind, const char *title, int64_t parent, int ui)
{
    if (S.depth >= (int)(sizeof S.stack / sizeof S.stack[0])) return;
    /* A screen over a value being changed (quick settings, the USB question):
     * the change ends as OK would end it, an EQ's saved. Left on, the wheel
     * went on changing it from the new screen, and backing out of that
     * dropped an EQ change. */
    if (S.editing && S.depth > 0) {
        list_kind_t under = S.stack[S.depth - 1].kind;
        if (under == L_EQ_EDIT || under == L_EQ_BAND) eqe_changed(1);
        S.editing = 0;
    }
    level_t *l = &S.stack[S.depth++];
    memset(l, 0, sizeof *l);
    l->kind = kind;
    snprintf(l->title, sizeof l->title, "%s", title);
    l->title_ui = ui;
    l->parent = parent;
    if (kind == L_CREDIT_TRACKS || kind == L_COMPOSER_TRACKS) return;   /* push_*_tracks() fills it */
    if (kind == L_PLAYLISTS) playlists_load(l);
    else if (library_backed(kind) && S.lib) library_list(S.lib, view_of(kind), parent, &l->items);
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

/* ---------- the station directory: requests ---------- */

/* Asked only when a list is opened, a search is made or Random station is
 * chosen; the answers come in on ui_tick (rd_poll). */

static void rd_free_list(void)
{
    free(S.rd_st);
    free(S.rd_meta);
    S.rd_st = NULL;
    S.rd_meta = NULL;
    S.rd_n = -1;            /* nothing asked yet */
    S.rd_state = RD_IDLE;
}

static void rd_free_countries(void)
{
    free(S.rd_cty);
    S.rd_cty = NULL;
    S.rd_cty_n = 0;
}

static void rd_stop(void)
{
    rdir_cancel();
    S.rd_job = RD_JOB_NONE;
}

/* The directory can be asked now (Wi-Fi is connected); else why not. */
static int rd_ready(char *why, size_t cap)
{
    why[0] = '\0';
    if (!S.hooks.radio_ready || S.hooks.radio_ready(why, cap) == 0) return 1;
    if (!why[0]) snprintf(why, cap, "The radio needs Wi-Fi");
    return 0;
}

#define RD_UNREACHABLE "Couldn't reach the station directory. Play tries again"

/* Ask for the list on top: stations for q, or (NULL) the countries. */
static void rd_ask(const rdir_query_t *q)
{
    rd_stop();
    S.rd_state = RD_IDLE;
    if (!rd_ready(S.rd_msg, sizeof S.rd_msg)) {
        S.rd_state = RD_FAILED;
        if (wifi_ask_if_off(WT_RDIR)) {
            snprintf(S.rd_msg, sizeof S.rd_msg, "%s", tr("Wi-Fi is off"));
            S.wifi_then.q_null = !q;
            if (q) S.wifi_then.q = *q;
        }
        return;
    }
    if (rdir_start(q)) {
        S.rd_state = RD_FAILED;
        snprintf(S.rd_msg, sizeof S.rd_msg, "%s", RD_UNREACHABLE);
        return;
    }
    S.rd_job = RD_JOB_LIST;
    S.rd_job_depth = S.depth;
}

/* Open a list of stations (a genre, a country, Popular); a genre or a country
 * is what Random station plays from next (by: "tag", "country"; NULL: any);
 * ui: the title is the UI's own words (Popular, a genre), not a country. */
static void rd_open_results(const char *title, int ui, const rdir_query_t *q, const char *by)
{
    push_as(L_RDIR_RESULTS, title, 0, ui);   /* (a genre's kept in English: shown translated) */
    rd_free_list();
    S.rd_q = *q;
    char v[160] = "";
    if (by) snprintf(v, sizeof v, "%s:%s:%s", by, q->arg, title);
    settings_set("radio.random", v);
    rd_ask(q);
}

/* Play station i of a list from the directory: Next and Previous then go
 * through the list. (radio-browser counts the play.) */
static void rd_play(const radio_station_t *st, const rdir_meta_t *meta, int n, int i)
{
    char why[96];
    if (!S.player || !st || i < 0 || i >= n) return;
    if (!rd_ready(why, sizeof why)) { if (!wifi_ask_if_off(WT_OK)) ui_toast(why, 2500); return; }
    if (rd_on_air(&st[i])) { S.now_playing_view = 1; return; }
    player_radio_play(S.player, st, n, i);
    if (meta) rdir_count_click(meta[i].uuid);
    S.now_playing_view = 1;
}

/* Random station: the directory's stations for q in random order, played
 * from the first; Next goes on to another. */
static void rd_random(const rdir_query_t *q)
{
    char why[96];
    if (!S.player) return;
    if (!rd_ready(why, sizeof why)) { if (!wifi_ask_if_off(WT_OK)) ui_toast(why, 2500); return; }
    if (S.rd_job == RD_JOB_RANDOM) return;            /* already looking */
    rdir_query_t r = *q;
    r.random = 1;
    rd_stop();
    if (rdir_start(&r)) { ui_toast("Couldn't reach the station directory", 2500); return; }
    S.rd_job = RD_JOB_RANDOM;
    S.rd_job_depth = S.depth;
}

static void rd_to_np(void)
{
    if (S.now_playing_view) return;
    if (!S.asleep) capture();                          /* (Now Playing slides up, as after a key) */
    S.now_playing_view = 1;
}

static void rd_random_done(rdir_result_t *r)
{
    if (r->n > 0) {
        player_radio_play(S.player, r->st, r->n, 0);
        rdir_count_click(r->meta[0].uuid);
        rd_to_np();
        return;
    }
    if (r->n == 0) { ui_toast("No stations found", 2000); return; }
    /* the directory didn't answer (Wi-Fi is on: it was asked): one of your own stations, at random */
    S.radio_n = S.hooks.radio_stations ? S.hooks.radio_stations(S.radio, RADIO_MAX) : -1;
    radio_station_t *mix = S.radio_n > 0 ? malloc((size_t)S.radio_n * sizeof *mix) : NULL;
    if (!mix) { ui_toast("Couldn't reach the station directory", 2500); return; }
    static rng_t rng;
    static int seeded;
    if (!seeded) { rng_seed(&rng, rng_os_seed(), 11); seeded = 1; }
    memcpy(mix, S.radio, (size_t)S.radio_n * sizeof *mix);
    for (int i = S.radio_n - 1; i > 0; i--) {
        int j = (int)rng_below(&rng, (uint32_t)i + 1);
        radio_station_t t = mix[i];
        mix[i] = mix[j];
        mix[j] = t;
    }
    player_radio_play(S.player, mix, S.radio_n, 0);
    free(mix);
    ui_toast("Couldn't reach the directory: one of your stations", 3000);
    rd_to_np();
}

static void rd_list_done(rdir_result_t *r, int depth)
{
    level_t *l = &S.stack[depth - 1];
    if (l->kind == L_RDIR_COUNTRIES) {
        rd_free_countries();
        S.rd_cty = r->countries;
        S.rd_cty_n = r->n > 0 ? r->n : 0;
        r->countries = NULL;
    } else {
        rd_free_list();
        S.rd_st = r->st;
        S.rd_meta = r->meta;
        S.rd_n = r->n;
        r->st = NULL;
        r->meta = NULL;
        /* Search: still on the letters after asking, the selection goes to the first station */
        if (l->kind == L_RDIR_SEARCH && l->sel == 0 && S.rd_n > 0 && depth == S.depth) l->sel = 1;
    }
    if (r->n < 0) {
        S.rd_state = RD_FAILED;
        snprintf(S.rd_msg, sizeof S.rd_msg, "%s", RD_UNREACHABLE);
    }
    int n = list_count(l);
    if (l->sel >= n) l->sel = n ? n - 1 : 0;
}

/* The answer to a request, once it's in: 1 while one is still running. */
static int rd_poll(void)
{
    if (!S.rd_job) return 0;
    rdir_result_t r;
    int p = rdir_poll(&r);
    if (p == 0) return 1;
    int job = S.rd_job, depth = S.rd_job_depth;
    S.rd_job = RD_JOB_NONE;
    if (p == 1) {
        if (job == RD_JOB_RANDOM) rd_random_done(&r);
        else if (depth >= 1 && depth <= S.depth) rd_list_done(&r, depth);
        rdir_result_free(&r);
    }
    render(R_ANIMATE);
    return 0;
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
    if (l->kind == L_UNINSTALL) uninstall_overlay_close();
    if (l->kind == L_POD_SHOW) { S.pod_ci = -1; S.pod_cur[0] = '\0'; S.pod_ne = 0; }
    if (l->kind == L_PODCASTS) pod_free();
    library_list_free(&l->items);
    S.depth--;
    S.editing = 0;
    S.moving = 0;
    /* the directory: a request for a level that's gone is dropped, and a list's stations go with it */
    if (S.rd_job && S.rd_job_depth > S.depth) rd_stop();
    if (l->kind == L_RDIR_RESULTS || l->kind == L_RDIR_SEARCH) rd_free_list();
    if (l->kind == L_RDIR_COUNTRIES) rd_free_countries();
    if (to_np) S.now_playing_view = 1;
}

static void pop(void) { pop_level(0); }

/* Open an overlay-like level (quick settings, context menu) from wherever we are. */
static void push_from_here(list_kind_t kind, const char *title, int64_t parent, int ui)
{
    int from_np = S.now_playing_view;
    S.now_playing_view = 0;
    push_as(kind, title, parent, ui);
    S.stack[S.depth - 1].back_to_np = from_np;
}

/* Something that needs Wi-Fi, with Wi-Fi off: ask to turn it on (1), and
 * once it has connected do `then` (WT_*) where it was asked. On, but not
 * connected yet (or no Wi-Fi at all), the caller says why as before (0). */
static int wifi_ask_if_off(int then)
{
    if (!S.hooks.wifi_state || !S.hooks.wifi_do) return 0;
    char busy[32];
    ui_wifi_status_t st;
    if (S.hooks.wifi_state(busy, sizeof busy, &st) || busy[0]) return 0;
    if (S.stack[S.depth - 1].kind == L_WIFI_ASK) return 1;
    S.wifi_then.what = then;
    S.wifi_then.armed = 0;
    push_from_here(L_WIFI_ASK, "Turn on Wi-Fi?", 0, 1);
    return 1;
}

/* Wi-Fi connected (or gave up): what "Turn on Wi-Fi" was for. */
static void wifi_then_run(int connected)
{
    if (!S.wifi_then.armed) return;
    S.wifi_then.armed = 0;
    level_t *l = &S.stack[S.depth - 1];
    if (!connected) {
        if (S.depth == S.wifi_then.depth && l->kind == S.wifi_then.kind)
            ui_toast("Couldn't connect: Settings > Wi-Fi", 2500);
        return;
    }
    if (S.now_playing_view || S.depth != S.wifi_then.depth || l->kind != S.wifi_then.kind) return;   /* moved on */
    if (S.wifi_then.what == WT_RDIR) rd_ask(S.wifi_then.q_null ? NULL : &S.wifi_then.q);
    else if (S.wifi_then.what == WT_OK && l->sel == S.wifi_then.sel) open_selected();
    render(R_ANIMATE);
}

void ui_usb_prompt(void)
{
    if (S.stack[S.depth - 1].kind == L_USB_ASK) return;
    if (S.stack[S.depth - 1].kind == L_CONTEXT) pop_level(1);
    push_from_here(L_USB_ASK, "Connected to a computer", 0, 1);
    render(R_ANIMATE);
}

void ui_usb_prompt_close(void)
{
    if (S.stack[S.depth - 1].kind != L_USB_ASK) return;
    pop();
    render(R_ANIMATE);
}

static void reset_restart(lv_timer_t *t)
{
    (void)t;
    if (S.hooks.restart) S.hooks.restart();
}

static void open_quick(void)
{
    if (S.stack[S.depth - 1].kind == L_QUICK) {   /* left open under Now Playing: show it */
        S.now_playing_view = 0;
        return;
    }
    if (S.stack[S.depth - 1].kind == L_CONTEXT) pop_level(1);
    push_from_here(L_QUICK, "Quick settings", 0, 1);
}

/* The tracks a context menu acts on (one track, an album, a playlist). */
static int subject_tracks(lib_list_t *out)
{
    memset(out, 0, sizeof *out);
    if (S.ctx_playlist < 0) return library_list(S.lib, LIB_SMART, -S.ctx_playlist, out);
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

/* An album's covers found online: Find cover where it has none; where it has
 * one (its own, or found), Find another cover; and a found one can go */
static void ctx_add_cover(int64_t album)
{
    int cov = library_album_cover(S.lib, album);
    if (cov == LIB_COVER_NONE) CTX_ADD(CTX_FIND_COVER);
    else if (cov != LIB_COVER_UNKNOWN) CTX_ADD(CTX_FIND_ANOTHER);
    if (cov == LIB_COVER_FOUND || cov == LIB_COVER_PICKED) CTX_ADD(CTX_REMOVE_COVER);
}

/* Context menu for the playing track (Now Playing), a queue row, or the selected row. */
/* ---------- the theme editor: opening, saving, the choices ---------- */

static int wheel_step(int n);

/* Another theme already has this name (themes are kept, and picked, by name) */
static int the_taken(const char *name)
{
    for (int i = 0; i < S.n_themes; i++)
        if (!strcasecmp(S.themes[i].name, name) && (!S.the_file[0] || strcmp(S.themes[i].file, S.the_file))) return 1;
    return 0;
}

static void the_unique(const char *base, char *out, size_t cap)
{
    snprintf(out, cap, "%s", base);
    for (int k = 2; k < 100 && the_taken(out); k++) snprintf(out, cap, "%.40s %d", base, k);
}

/* The accent nearest a colour (Grey for one with little colour) */
static int the_nearest_accent(uint32_t c)
{
    int h, s, l, best = 0, bd = 999;
    theme_to_hsl(c, &h, &s, &l);
    if (s < 15) return N_ACCENTS - 1;
    for (int i = 0; i < N_ACCENTS - 1; i++) {   /* by hue, then how strong (Pink or Hot pink) */
        int d = abs(ACCENTS[i].hue - h);
        if (d > 180) d = 360 - d;
        d = 4 * d + abs(ACCENTS[i].sat - s);
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

/* Open the editor: on theme i (a built-in one as a copy), or (i < 0) on a new
 * one with the current theme's shapes and font and colours from an accent */
static void the_open(int i)
{
    if (!S.hooks.theme_save) return;
    const theme_t *src = i >= 0 && i < S.n_themes ? &S.themes[i] : T();
    if (src->status == THEME_ERROR) { ui_toast(src->note, 4000); return; }
    S.the = *src;
    S.the_acc = -1;
    S.the_dirty = 0;
    S.the_back_armed = S.the_del_armed = 0;
    snprintf(S.the_file, sizeof S.the_file, "%s", i >= 0 ? src->file : "");
    if (!S.the_file[0]) {                     /* new, or a copy: a theme of the owner's own */
        char name[48];
        if (i < 0) {
            S.the_acc = the_nearest_accent(src->select_bg);
            theme_make(&S.the, src->dark, ACCENTS[S.the_acc].hue, ACCENTS[S.the_acc].sat);
            the_unique(tr("My theme"), name, sizeof name);
        } else {
            char base[48];
            subst(base, sizeof base, tr("%s copy"), src->name, NULL);
            the_unique(base, name, sizeof name);
            ui_toast("A copy: the built-in themes stay as they are", 2500);
        }
        snprintf(S.the.name, sizeof S.the.name, "%s", name);
        S.the.author[0] = S.the.licence[0] = S.the.file[0] = '\0';
        S.the_dirty = 1;
    }
    push(L_THEME_EDIT, S.the.name, 0);
}

/* The catalogue read again (a theme saved or deleted): use_file is used, or
 * ("") the theme in use stays, by name (the default if it's gone) */
static void the_reload(const char *use_file)
{
    char cur[48];
    snprintf(cur, sizeof cur, "%s", T()->name);
    int n = S.hooks.themes_reload ? S.hooks.themes_reload() : S.n_themes;
    if (n > 0) S.n_themes = n;
    int idx = -1;
    for (int i = 0; i < S.n_themes && use_file[0]; i++)
        if (!strcmp(S.themes[i].file, use_file)) idx = i;
    for (int i = 0; i < S.n_themes && idx < 0; i++)
        if (!strcmp(S.themes[i].name, cur)) idx = i;
    if (idx < 0) idx = 0;
    S.cur_theme = idx;
    for (int d = 0; d < S.depth; d++)
        if (S.stack[d].kind == L_THEMES) S.stack[d].sel = idx;
    ui_set_theme(idx);
}

static void the_save(void)
{
    char p[96], err[512], saved[128];
    if (!S.the.name[0]) { ui_toast("Give it a name first", 2000); return; }
    if (the_taken(S.the.name)) { ui_toast("Another theme has this name", 2500); return; }
    the_problem(-1, p, sizeof p);
    if (!p[0] && theme_validate(&S.the, err, sizeof err)) {   /* (a rule the editor doesn't name) */
        char *nl = strchr(err, '\n');
        if (nl) *nl = '\0';
        snprintf(p, sizeof p, "%s", err);
    }
    if (p[0]) { ui_toast(p, 3500); return; }
    if (S.hooks.theme_save(&S.the, S.the_file, saved, sizeof saved)) { ui_toast("Couldn't save the theme", 2500); return; }
    snprintf(S.the_file, sizeof S.the_file, "%s", saved);
    S.the_dirty = 0;
    the_reload(saved);
    ui_toast("Saved, and in use", 1500);
}

static void the_delete(void)
{
    uint32_t now = lv_tick_get();
    if (!S.hooks.theme_remove || !S.the_file[0]) return;
    if (!(S.the_del_armed && now - S.the_del_armed < 3000)) {
        S.the_del_armed = now;
        ui_toast("Press again to delete this theme", 3000);
        return;
    }
    S.the_del_armed = 0;
    if (S.hooks.theme_remove(S.the_file)) {
        ui_toast(strchr(S.the_file, '/') ? "It has its own folder: delete it on a computer" : "Couldn't delete it", 3000);
        return;
    }
    S.the_dirty = 0;
    pop();
    the_reload("");
    ui_toast("Theme deleted", 1500);
}

/* The wheel on a value: the accent, the radius, a colour */
static void the_adjust(const level_t *l, int d)
{
    if (l->kind == L_THEME_EDIT && l->sel == TE_ACCENT) {
        S.the_acc = ((S.the_acc < 0 ? 0 : S.the_acc) + d + N_ACCENTS) % N_ACCENTS;
        theme_accent(&S.the, ACCENTS[S.the_acc].hue, ACCENTS[S.the_acc].sat);
    } else if (l->kind == L_THEME_EDIT && l->sel == TE_RADIUS) {
        int r = S.the.radius + d;
        S.the.radius = r < 0 ? 0 : r > 20 ? 20 : r;
    } else if (l->kind == L_THEME_COLOURS && l->sel >= 0 && l->sel < TC_N) {
        int r = l->sel, step = wheel_step(1000);   /* faster as it turns faster */
        if (S.the_pal < 0) S.the_pal = the_pal_nearest(the_get(r));   /* its own colour: the nearest first */
        else S.the_pal = ((S.the_pal + d * step) % N_PAL + N_PAL) % N_PAL;
        *the_col(&S.the, r) = the_pal_at(S.the_pal, NULL, 0);
        if (r == TC_SELBG || r == TC_SELFG || r == TC_SIGNAL || r == TC_MODE) S.the_acc = -1;
    } else {
        return;
    }
    S.the_dirty = 1;
}

/* Play on a row of the editor's pages */
static void the_ok(level_t *l)
{
    theme_t *t = &S.the;
    if (l->kind == L_THEME_COLOURS) {             /* pick another with the wheel */
        if (l->sel < 0 || l->sel >= TC_N) return;
        S.the_pal = the_pal_find(the_get(l->sel));
        S.editing = 1;
        return;
    }
    switch (l->sel) {
    case TE_NAME:
        S.rename_theme = 1;
        snprintf(S.entry, sizeof S.entry, "%s", t->name);
        S.entry_ch = 0;
        push_ui(L_RENAME, "Theme name", 0);
        return;
    case TE_BASE: {                               /* every colour made again for it */
        int h, s, lg;
        if (S.the_acc >= 0) { h = ACCENTS[S.the_acc].hue; s = ACCENTS[S.the_acc].sat; }
        else theme_to_hsl(t->select_bg, &h, &s, &lg);
        theme_make(t, !t->dark, h, s);
        break;
    }
    case TE_ACCENT:
        if (S.the_acc < 0) {                      /* from its own colours: the nearest accent */
            S.the_acc = the_nearest_accent(t->select_bg);
            theme_accent(t, ACCENTS[S.the_acc].hue, ACCENTS[S.the_acc].sat);
            S.the_dirty = 1;
        }
        S.editing = 1;
        return;
    case TE_COLOURS: push_ui(L_THEME_COLOURS, "Colours", 0); return;
    case TE_FONT: {
        if (t->font_file[0]) { ui_toast("Its own font: change it on a computer", 2500); return; }
        int n = (int)(sizeof FONT_FAMILIES / sizeof FONT_FAMILIES[0]), f = 0;
        while (f < n && strcmp(t->font, FONT_FAMILIES[f].family)) f++;
        snprintf(t->font, sizeof t->font, "%s", FONT_FAMILIES[(f + 1) % n].family);
        memset(t->font_px, 0, sizeof t->font_px);
        t->icons_pixel = !strcmp(t->font, "arkpixel");
        break;
    }
    case TE_SIZE: {
        const int *large = the_large(t);
        if (!large) {
            ui_toast(t->font_file[0] ? "Its own font: change it on a computer" : "Ark Pixel comes in one size", 2500);
            return;
        }
        if (the_size(t) == 0) memcpy(t->font_px, large, sizeof t->font_px);
        else memset(t->font_px, 0, sizeof t->font_px);
        ui_toast("The new size shows once it's saved", 2000);
        break;
    }
    case TE_SELECTION: t->selection_bar = !t->selection_bar; break;
    case TE_CORNERS: t->corner = (theme_corner_t)((t->corner + 1) % 3); break;
    case TE_RADIUS: S.editing = 1; return;
    case TE_PROGRESS: t->progress_segments = !t->progress_segments; break;
    case TE_PLAYING: t->playing_equalizer = !t->playing_equalizer; break;
    case TE_CURSOR: t->cursor_arrow = !t->cursor_arrow; break;
    case TE_SHADOW: t->shadow_hard = !t->shadow_hard; break;
    case TE_MOTION: t->motion_stepped = !t->motion_stepped; break;
    case TE_SAVE: the_save(); return;
    case TE_DELETE: the_delete(); return;
    default: return;
    }
    S.the_dirty = 1;
}

/* ---------- Podcasts: what Play and Menu do ---------- */

/* The network can be used now; else say why (or offer to turn on Wi-Fi): 0 */
static int pod_net(void)
{
    char why[96] = "";
    if (!S.hooks.radio_ready || S.hooks.radio_ready(why, sizeof why) == 0) return 1;
    if (!wifi_ask_if_off(WT_OK)) ui_toast(why[0] ? why : "Podcasts need Wi-Fi for this", 2500);
    return 0;
}

/* The episode playing: where it is, kept on the card now and then (every 15 s
 * of playing, on a pause, when another one starts or it's left), and played
 * when it ends (or is left within its last 30 s). */
static void pod_save_play(int played)
{
    if (!S.pod_play.key[0] || !pod_dir()[0]) return;
    if (S.pod_play.marked && !played) {          /* marked played as it plays: the card has it so */
        S.pod_play.saved = S.pod_play.pos;
        return;
    }
    pod_state_t st = {0};
    snprintf(st.key, sizeof st.key, "%s", S.pod_play.key);
    st.len_ms = S.pod_play.len;
    if (played || (S.pod_play.len > 60000 && S.pod_play.pos + 30000 > S.pod_play.len)) {
        st.played = 1;
        st.played_at = (int64_t)time(NULL);
        st.pos_ms = 0;
    } else {
        st.pos_ms = S.pod_play.pos;
    }
    pod_state_put(pod_dir(), S.pod_play.id, &st);
    S.pod_play.saved = S.pod_play.pos;   /* (where it was kept from: a played one's 0 isn't, or its last 30 s saved every second) */
    if (S.pod_ci >= 0 && !strcmp(S.pod_shows[S.pod_ci].id, S.pod_play.id)) pod_read_episodes();
}

/* 1 if it has just played to its end (kept as played) */
static int pod_track(void)
{
    if (!S.player) return 0;
    uint32_t pos = 0, len = 0;
    const player_episode_t *e = player_episode(S.player, &pos, &len);
    if (player_episode_ended(S.player)) {       /* played to its end (said once) */
        S.pod_play.pos = 0;
        pod_save_play(1);
        return 1;
    }
    if (S.pod_play.key[0] && (!e || strcmp(e->key, S.pod_play.key) || strcmp(e->id, S.pod_play.id))) {
        pod_save_play(0);                         /* left (or another one started) */
        S.pod_play.key[0] = '\0';
    }
    if (!e) return 0;
    if (!S.pod_play.key[0]) {
        snprintf(S.pod_play.id, sizeof S.pod_play.id, "%s", e->id);
        snprintf(S.pod_play.key, sizeof S.pod_play.key, "%s", e->key);
        S.pod_play.saved = pos;
        S.pod_play.marked = 0;
    }
    S.pod_play.pos = pos;
    if (len) S.pod_play.len = len;
    uint32_t d = pos > S.pod_play.saved ? pos - S.pod_play.saved : S.pod_play.saved - pos;
    if (d >= 15000 || (!S.now.playing && d >= 1000)) pod_save_play(0);
    return 0;
}

static void pod_play_episode(int i)
{
    if (!S.player || S.pod_ci < 0 || i < 0 || i >= S.pod_ne) return;
    const pod_show_t *s = &S.pod_shows[S.pod_ci];
    const pod_episode_t *e = &S.pod_eps[i];
    if (S.now.podcast && !strcmp(S.pod_play.id, s->id) && !strcmp(S.pod_play.key, e->key)) {
        if (!S.now.playing) player_pause(S.player, 0);   /* this one: go on with it */
        S.now_playing_view = 1;
        return;
    }
    pod_state_t st;
    pod_state_find(S.pod_st, S.pod_nst, e->key, &st);
    player_episode_t pe;
    memset(&pe, 0, sizeof pe);
    snprintf(pe.show, sizeof pe.show, "%s", s->title);
    snprintf(pe.title, sizeof pe.title, "%s", e->title);
    snprintf(pe.id, sizeof pe.id, "%s", s->id);
    snprintf(pe.key, sizeof pe.key, "%s", e->key);
    pe.len_ms = st.len_ms ? st.len_ms : (uint32_t)e->dur_s * 1000;
    if (S.pod_on_card[i]) {
        pod_episode_path(pod_dir(), s->id, e, pe.path, sizeof pe.path);
        pe.start_ms = st.played ? 0 : st.pos_ms;
    } else {
        if (!pod_net()) return;
        snprintf(pe.path, sizeof pe.path, "%s", e->url);
        if (st.pos_ms >= 60000) {
            char at[16], msg[96];
            fmt_time(at, sizeof at, (int)(st.pos_ms / 1000), 0);
            subst(msg, sizeof msg, tr("From the start: download it to go on from %s"), at, NULL);
            ui_toast(msg, 3000);
        }
    }
    if (!pod_track() && S.pod_play.key[0])
        pod_save_play(0);                         /* the one playing: where it is now, not 15 s ago */
    S.pod_nch = S.pod_on_card[i] ? pod_chapters(pod_dir(), s->id, e, S.pod_ch, 64) : 0;
    snprintf(S.pod_ch_key, sizeof S.pod_ch_key, "%s", e->key);
    player_episode_play(S.player, &pe);
    snprintf(S.pod_play.id, sizeof S.pod_play.id, "%s", pe.id);
    snprintf(S.pod_play.key, sizeof S.pod_play.key, "%s", pe.key);
    S.pod_play.pos = S.pod_play.saved = pe.start_ms;
    S.pod_play.len = pe.len_ms;
    S.now_playing_view = 1;
}

static void pod_open(void)
{
    pod_read_shows();
    push_ui(L_PODCASTS, "Podcasts", 0);
    S.stack[S.depth - 1].sel = S.pod_n ? 1 : 0;
    for (int i = 0; i < S.pod_n; i++)            /* the show playing selected */
        if (S.now.podcast && !strcmp(S.pod_shows[i].id, S.pod_play.id)) S.stack[S.depth - 1].sel = i + 1;
    /* feeds over an hour old: read again, with the network already up */
    int64_t now = (int64_t)time(NULL), oldest = now;
    for (int i = 0; i < S.pod_n; i++)
        if (S.pod_shows[i].refreshed < oldest) oldest = S.pod_shows[i].refreshed;
    char why[96];
    if (S.pod_n && now - oldest > 3600 && S.hooks.radio_ready && S.hooks.radio_ready(why, sizeof why) == 0)
        podsync_refresh("");
}

static void pod_open_show(int i)
{
    if (i < 0 || i >= S.pod_n) return;
    pod_show_t *s = &S.pod_shows[i];
    snprintf(S.pod_cur, sizeof S.pod_cur, "%s", s->id);
    S.pod_ci = i;
    pod_read_episodes();
    S.pod_seen = s->seen;                         /* new while it's open; seen from now on */
    if (s->latest > s->seen) {
        s->seen = s->latest;
        pod_show_seen(pod_dir(), s->id, s->seen);
    }
    push(L_POD_SHOW, s->title, i);
    level_t *l = &S.stack[S.depth - 1];
    for (int e = 0; e < S.pod_ne; e++)            /* the episode playing selected */
        if (S.now.podcast && !strcmp(S.pod_play.id, s->id) && !strcmp(S.pod_play.key, S.pod_eps[e].key)) l->sel = e;
}

static void pod_search_run(void)
{
    if (!S.entry[0]) { ui_toast("Type a name first", 1500); return; }
    if (!pod_net()) return;
    if (podsync_search(S.entry)) { S.pod_search = -2; return; }
    S.pod_search = 0;
    S.pod_nf = 0;
}

/* Something on the card changed (a feed read, a download): the lists again */
static void pod_reload(void)
{
    level_t *l = &S.stack[S.depth - 1];
    /* the shows selected (and a menu's), by feed: an unsubscribe takes a show
     * out, so the places move (the episodes': pod_read_episodes) */
    char sel_url[sizeof S.stack / sizeof S.stack[0]][512] = {{0}};
    for (int d = 0; d < S.depth; d++) {
        level_t *k = &S.stack[d];
        if (k->kind == L_PODCASTS && k->sel >= 1 && k->sel <= S.pod_n)
            snprintf(sel_url[d], sizeof sel_url[d], "%s", S.pod_shows[k->sel - 1].url);
    }
    pod_read_shows();
    if (S.pod_ci >= 0) pod_read_episodes();
    else if (l->kind == L_POD_SHOW) {             /* (unsubscribed meanwhile) */
        S.pod_ne = 0;
        if (S.pod_ctx_key[0]) S.pod_ctx = -1;
    }
    for (int d = 0; d < S.depth; d++) {
        level_t *k = &S.stack[d];
        if (k->kind != L_PODCASTS && k->kind != L_POD_SHOW) continue;
        for (int i = 0; sel_url[d][0] && i < S.pod_n; i++)
            if (!strcmp(S.pod_shows[i].url, sel_url[d])) k->sel = i + 1;
        int n = list_count(k);
        if (k->sel >= n) k->sel = n - 1;
        if (k->sel < 0) k->sel = 0;
    }
    if (S.pod_ctx_url[0]) {                       /* a show's menu: where it is now, or gone (-1) */
        int at = -1;
        for (int i = 0; i < S.pod_n; i++)
            if (!strcmp(S.pod_shows[i].url, S.pod_ctx_url)) at = i;
        S.pod_ctx = at;
    }
}

/* ui_tick: the search's answer. 1 while waiting for one */
static int pod_poll(void)
{
    if (S.pod_search != 0) return 0;
    if (!S.pod_found) S.pod_found = calloc(POD_SEARCH_MAX, sizeof *S.pod_found);
    if (!S.pod_found) return 0;
    int n = 0, r = podsync_search_result(S.pod_found, POD_SEARCH_MAX, &n);
    if (r == 0) return 1;
    S.pod_search = r;
    S.pod_nf = r == 1 ? n : 0;
    level_t *l = &S.stack[S.depth - 1];
    if (l->kind == L_POD_SEARCH && l->sel == 0 && S.pod_nf > 0) l->sel = 1;
    if (l->kind == L_POD_SEARCH) render(R_ANIMATE);
    return 0;
}

/* Once a second: the episode playing kept; the card's changes shown; now and
 * then, the automatic part (feeds read, new episodes downloaded while charging) */
static void pod_second(void)
{
    if (!pod_dir()[0]) return;
    pod_track();
    podsync_status_t st;
    podsync_status(&st);
    if (st.failed_seq != S.pod_failed) {         /* a feed or a download that failed: said once */
        if (S.pod_failed || st.failed_seq > 1) {
            char msg[128];
            subst(msg, sizeof msg, tr("Couldn't get %s"), st.failed_what, NULL);
            ui_toast(msg, 3000);
        }
        S.pod_failed = st.failed_seq;
    }
    if (st.import_seq != S.pod_imported) {       /* an import finished: how it went */
        if (st.import_seq > 0 && S.pod_imported != st.import_seq) {
            char msg[96];
            if (st.imported < 0) snprintf(msg, sizeof msg, "%s", tr("Couldn't read the OPML file"));
            else if (st.imported == 0) snprintf(msg, sizeof msg, "%s", tr("No new shows in it"));
            else tr_num(msg, sizeof msg, st.imported == 1 ? "%s show added" : "%s shows added", st.imported);
            ui_toast(msg, 2500);
        }
        S.pod_imported = st.import_seq;
    }
    if (st.changed != S.pod_changed) {
        S.pod_changed = st.changed;
        list_kind_t k = S.stack[S.depth - 1].kind;
        if (S.pod_shows && (k == L_PODCASTS || k == L_POD_SHOW || k == L_POD_SEARCH || S.pod_ci >= 0)) pod_reload();
    }
    uint32_t now = lv_tick_get();
    if (!S.pod_auto_at) S.pod_auto_at = now - 7 * 60 * 1000 + 1;   /* first 3 minutes in (the network up by then) */
    if (now - S.pod_auto_at >= 10 * 60 * 1000) {                    /* then every 10 minutes */
        S.pod_auto_at = now;
        char why[96];
        int net = S.hooks.radio_ready && S.hooks.radio_ready(why, sizeof why) == 0;
        if (settings_get_int("podcasts.auto", 1))
            podsync_auto((int64_t)time(NULL), net, S.charging, 6 * 3600, settings_get_int("podcasts.keep", 2),
                         settings_get_int("podcasts.clean_days", 7));
    }
}

static int open_context(void)
{
    level_t *l = &S.stack[S.depth - 1];
    S.ctx_n = 0;
    S.ctx_track = S.ctx_album = S.ctx_artist = S.ctx_playlist = 0;
    S.ctx_credited = 0;
    S.ctx_qidx = -1;
    S.ctx_plpos = -1;
    char title[128] = "";
    int on_np = S.now_playing_view;
    /* A menu or quick settings left open under Now Playing (long Back): put
     * it away first, or a second menu went on top and the first, uncovered
     * later, showed one item's title with the other's actions. */
    while (on_np && S.depth > 1 &&
           (S.stack[S.depth - 1].kind == L_CONTEXT || S.stack[S.depth - 1].kind == L_QUICK))
        pop_level(1);
    l = &S.stack[S.depth - 1];
    if (l->kind == L_BLUETOOTH && !on_np) {   /* Settings > Bluetooth: forget a paired device */
        int d = l->sel - BT_DEV_ROW;
        if (d < 0 || d >= S.bt_n || !S.bt[d].paired) return -1;
        snprintf(S.ctx_bt, sizeof S.ctx_bt, "%s", S.bt[d].addr);
        CTX_ADD(CTX_BT_FORGET);
        push_from_here(L_CONTEXT, S.bt[d].name[0] ? S.bt[d].name : S.bt[d].addr, 0, 0);
        return 0;
    }
    if (l->kind == L_WIFI && !on_np) {        /* Settings > Wi-Fi: forget a saved network */
        int w;
        if (wifi_row(l->sel, &w) != WR_NET || w >= S.wifi_n || !S.wifi[w].saved) return -1;
        snprintf(S.ctx_wifi, sizeof S.ctx_wifi, "%s", S.wifi[w].ssid);
        CTX_ADD(CTX_WIFI_FORGET);
        push_from_here(L_CONTEXT, S.wifi[w].ssid, 0, 0);
        return 0;
    }
    if (on_np && S.now.podcast) {             /* an episode playing: its chapters, its speed */
        const player_episode_t *pe = S.player ? player_episode(S.player, NULL, NULL) : NULL;
        if (!pe) return -1;
        if (S.pod_nch && !strcmp(pe->key, S.pod_ch_key)) CTX_ADD(CTX_POD_CHAPTERS);
        CTX_ADD(CTX_POD_SPEED);
        push_from_here(L_CONTEXT, pe->title, 0, 0);
        return 0;
    }
    if (l->kind == L_PODCASTS && !on_np) {    /* Podcasts: check for new episodes (all, or a show's); unsubscribe */
        if (l->sel > S.pod_n) return -1;
        S.pod_ctx = l->sel - 1;
        snprintf(S.pod_ctx_url, sizeof S.pod_ctx_url, "%s", S.pod_ctx >= 0 ? S.pod_shows[S.pod_ctx].url : "");
        S.pod_ctx_key[0] = '\0';
        S.pod_del_armed = 0;
        CTX_ADD(CTX_POD_REFRESH);
        if (S.pod_ctx >= 0) CTX_ADD(CTX_POD_UNSUB);
        push_from_here(L_CONTEXT, S.pod_ctx >= 0 ? S.pod_shows[S.pod_ctx].title : tr("Podcasts"), 0, 0);
        return 0;
    }
    if (l->kind == L_POD_SHOW && !on_np && S.pod_ci >= 0) {   /* an episode: download it or not, played or not */
        S.pod_ctx = S.pod_ne ? l->sel : -1;
        snprintf(S.pod_ctx_key, sizeof S.pod_ctx_key, "%s", S.pod_ctx >= 0 ? S.pod_eps[S.pod_ctx].key : "");
        S.pod_ctx_url[0] = '\0';
        if (S.pod_ctx >= 0) {
            const pod_episode_t *e = &S.pod_eps[S.pod_ctx];
            pod_state_t st;
            pod_state_find(S.pod_st, S.pod_nst, e->key, &st);
            if (S.pod_on_card[S.pod_ctx]) CTX_ADD(CTX_POD_DELETE);
            else if (podsync_download_state(S.pod_shows[S.pod_ci].id, e->key) >= 0) CTX_ADD(CTX_POD_CANCEL);
            else CTX_ADD(CTX_POD_DOWNLOAD);
            CTX_ADD(st.played ? CTX_POD_UNPLAYED : CTX_POD_PLAYED);
        }
        CTX_ADD(CTX_POD_REFRESH);
        push_from_here(L_CONTEXT, S.pod_ctx >= 0 ? S.pod_eps[S.pod_ctx].title : S.pod_shows[S.pod_ci].title, 0, 0);
        return 0;
    }
    if (l->kind == L_THEMES && !on_np) {      /* Settings > Interface > Theme: edit one (a copy, of a built-in one) */
        if (!S.hooks.theme_save || l->sel >= S.n_themes) return -1;
        the_open(l->sel);
        return 0;
    }
    if (l->kind == L_EQ && !on_np) {          /* Settings > Sound > EQ: edit one */
        if (l->sel < 1 || l->sel - 1 >= S.eq_n) return -1;
        eqe_open(S.eq_names[l->sel - 1]);
        return 0;
    }
    if (l->kind == L_HOME_EDIT && !on_np) {   /* Settings > Home: move an item */
        CTX_ADD(CTX_HOME_MOVE);
        push_from_here(L_CONTEXT, HOME[S.home_order[l->sel]], 0, 1);
        return 0;
    }
    /* the radio: a station of yours, one in the directory, or the one playing */
    const radio_station_t *rs = NULL;
    S.rd_ctx_i = -1;
    if (on_np && S.now.radio) rs = S.player ? player_radio_station(S.player) : NULL;
    else if (!on_np && l->kind == L_RADIO && l->sel >= 1 && l->sel - 1 < S.radio_n) rs = &S.radio[l->sel - 1];
    else if (!on_np && (l->kind == L_RDIR_RESULTS || l->kind == L_RDIR_SEARCH) && l->sel >= 1 && l->sel - 1 < S.rd_n) {
        S.rd_ctx_i = l->sel - 1;
        rs = &S.rd_st[S.rd_ctx_i];
    }
    if (rs) {
        S.rd_ctx = *rs;
        int yours = l->kind == L_RADIO && !on_np;
        if (!yours) {                            /* is it in your list? (as the card has it now) */
            S.radio_n = S.hooks.radio_stations ? S.hooks.radio_stations(S.radio, RADIO_MAX) : -1;
            for (int i = 0; i < S.radio_n && !yours; i++) yours = !strcmp(S.radio[i].url, rs->url);
        }
        if (S.rd_ctx_i >= 0) CTX_ADD(CTX_RADIO_PLAY);
        if (yours && S.hooks.radio_remove) CTX_ADD(CTX_RADIO_REMOVE);
        else if (!yours && S.hooks.radio_add) CTX_ADD(CTX_RADIO_ADD);
        if (!S.ctx_n) return -1;
        S.rd_del_armed = 0;
        push_from_here(L_CONTEXT, S.rd_ctx.name, 0, 0);
        return 0;
    }
    if (on_np && S.now.radio) return -1;
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
    } else if ((l->kind == L_ARTISTS || l->kind == L_ALBUM_ARTISTS) && l->sel < l->items.n) {
        S.ctx_artist = l->items.ids[l->sel];
        S.ctx_credited = l->kind == L_ARTISTS;
    } else {
        return -1;
    }

    if (S.ctx_playlist < 0) {                        /* a smart one: Fathom keeps it */
        snprintf(title, sizeof title, "%s", tr(library_smart_name((int)-S.ctx_playlist)));
        CTX_ADD(CTX_PLAY); CTX_ADD(CTX_SHUFFLE); CTX_ADD(CTX_PLAY_NEXT); CTX_ADD(CTX_ADD_QUEUE); CTX_ADD(CTX_ADD_PLAYLIST);
    } else if (S.ctx_playlist) {
        library_playlist_name(S.lib, S.ctx_playlist, title, sizeof title);
        CTX_ADD(CTX_PLAY); CTX_ADD(CTX_SHUFFLE); CTX_ADD(CTX_PLAY_NEXT); CTX_ADD(CTX_ADD_QUEUE); CTX_ADD(CTX_ADD_PLAYLIST);
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
        if (S.ctx_album) ctx_add_cover(S.ctx_album);
    } else if (!S.ctx_album) {                      /* an artist: their songs */
        if (library_artist_name(S.lib, S.ctx_artist, title, sizeof title)) return -1;
        CTX_ADD(CTX_SHUFFLE);
    } else {
        S.ctx_artist = library_album_artist(S.lib, S.ctx_album);
        library_album_name(S.lib, S.ctx_album, title, sizeof title);
        CTX_ADD(CTX_PLAY); CTX_ADD(CTX_SHUFFLE); CTX_ADD(CTX_PLAY_NEXT); CTX_ADD(CTX_ADD_QUEUE); CTX_ADD(CTX_ADD_PLAYLIST);
        if (S.ctx_artist && !(l->kind == L_ARTIST_ALBUMS && l->parent == S.ctx_artist)) CTX_ADD(CTX_GO_ARTIST);
        ctx_add_cover(S.ctx_album);
    }
    push_from_here(L_CONTEXT, title, 0, 0);
    return 0;
}

/* ---------- covers found online (net/coverfind.h) ---------- */

/* Wi-Fi up (1); else asked to turn it on, or why not said (0) */
static int cover_net(int then)
{
    char why[96] = "";
    if (!S.hooks.radio_ready || S.hooks.radio_ready(why, sizeof why) == 0) return 1;
    if (!wifi_ask_if_off(then)) ui_toast(why[0] ? why : "Finding covers needs Wi-Fi", 2500);
    return 0;
}

/* An album's cover changed: its rows and Now Playing drawn again */
static void cover_changed(int64_t album)
{
    covers_reset(1);
    if (S.now.active && !S.now.radio && !S.now.podcast && S.now.track.album_id == album) load_art();
}

/* Context menu > Find cover: its matches, shown one at a time */
static void cover_find_open(int64_t album, int from_np)
{
    lib_album_t a;
    if (!S.lib || library_album_info(S.lib, album, &a)) return;
    if (!a.title[0]) { ui_toast("It has no album name to look for", 2500); return; }
    if (!cover_net(WT_NONE)) return;
    if (cf_find(&a)) { ui_toast("Can't look covers up here", 2000); return; }
    S.cf_album = album;
    S.cf_k = 0;
    S.cf_drawn = -1;
    push(L_COVER_FIND, a.title, 0);                 /* (titled with the album: what's looked for) */
    S.stack[S.depth - 1].back_to_np = from_np;
}

static void cover_find_key(ui_key_t k)
{
    int n = 0, st = cf_find_state(&n);
    switch (k) {
    case UI_KEY_UP: if (S.cf_k > 0) S.cf_k--; break;
    case UI_KEY_DOWN: if (S.cf_k + 1 < n) S.cf_k++; break;
    case UI_KEY_OK: {
        static uint16_t small[LIB_ART_SMALL * LIB_ART_SMALL];
        if (st != 1 || !n) break;
        int pic = cf_find_show(S.cf_k, NULL, NULL);
        if (pic == -2) { cf_find_retry(S.cf_k); break; }   /* (couldn't be fetched: again) */
        if (pic != 1) break;
        if (cf_find_keep(S.cf_k, S.cf_px, small) || library_store_found_cover(S.lib, S.cf_album, S.cf_px, small)) {
            ui_toast("Couldn't save the cover", 2500);
            break;
        }
        int64_t album = S.cf_album;
        cf_find_close();
        pop();
        cover_changed(album);
        ui_toast("Cover saved", 1500);
        break;
    }
    default: break;
    }
}

/* Settings > Library > Find missing covers (or, under way, stop) */
static void covers_find_all(void)
{
    cf_batch_t b;
    cf_batch_status(&b);
    if (b.running) { cf_batch_stop(); ui_toast("Stopped", 1500); return; }
    if (!S.lib) return;
    lib_album_t *list = NULL;
    int n = library_albums_without_cover(S.lib, &list);
    if (n == 0) ui_toast("Every album has a cover", 2000);
    else if (n > 0 && cover_net(WT_OK)) {
        if (cf_batch(list, n)) ui_toast("Can't look covers up here", 2000);
        else {
            char msg[96];
            tr_num(msg, sizeof msg, n == 1 ? "Looking for %s cover" : "Looking for %s covers", n);
            ui_toast(msg, 2000);
        }
    }
    free(list);
}

/* Every 100 ms: a find's answers and pictures as they come; the batch's
 * covers stored, and how it went said when it's done. 1 while waiting. */
static int cf_poll(void)
{
    static uint16_t large[LIB_ART_LARGE * LIB_ART_LARGE], small[LIB_ART_SMALL * LIB_ART_SMALL];
    int64_t album;
    int stored = 0;
    while (S.lib && cf_batch_take(&album, large, small)) {
        /* (only over no cover: one of its own may have come with a rescan
         * while the batch ran, and a found one would then go ahead of it) */
        if (library_album_cover(S.lib, album) == LIB_COVER_NONE && library_store_found_cover(S.lib, album, large, small) == 0) {
            stored = 1;
            if (S.now.active && S.now.track.album_id == album) cover_changed(album);
        }
    }
    if (stored) covers_reset(1);
    cf_batch_t b;
    cf_batch_status(&b);
    if (b.seq != S.cf_batch_seq) {
        if (S.cf_batch_seq || b.seq > 1 || !b.running) {
            char msg[160];
            if (b.failed && !b.done) snprintf(msg, sizeof msg, "%s", tr("Couldn't reach MusicBrainz or Apple"));
            else if (b.total == 0) snprintf(msg, sizeof msg, "%s", tr("No covers to look for"));
            else {
                char f[48], u[48], z[48];
                tr_num(f, sizeof f, b.found == 1 ? "%s cover found" : "%s covers found", b.found);
                tr_num(u, sizeof u, "%s unsure", b.unsure);
                tr_num(z, sizeof z, "%s not found", b.none);
                snprintf(msg, sizeof msg, "%s, %s, %s", f, u, z);
            }
            ui_toast(msg, 4000);
        }
        S.cf_batch_seq = b.seq;
    }
    level_t *l = &S.stack[S.depth - 1];
    if (l->kind == L_LIBRARY && !S.now_playing_view && b.running) render(R_CONTENT);   /* (its count) */
    if (l->kind != L_COVER_FIND || S.now_playing_view) return b.running;
    int n = 0, st = cf_find_state(&n);
    int pic = st == 1 && n ? cf_find_show(S.cf_k, NULL, NULL) : -1;
    int k = S.cf_k >= n ? (n ? n - 1 : 0) : S.cf_k;
    int now = (st + 2) | (n << 3) | ((pic + 2) << 8) | (k << 11);
    if (now != S.cf_drawn) render(R_CONTENT);
    return st == 0 || pic == 0 || b.running;
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
                else tr_num(msg, sizeof msg, a == CTX_PLAY_NEXT ? "%s songs play next" : "%s songs added to queue", n);
                ui_toast(msg, 1500);
            }
        }
        library_list_free(&tr);
        break;
    }
    case CTX_SHUFFLE: {                           /* a playlist's songs, an album's, or an artist's */
        int pl = S.ctx_playlist != 0;
        int n = pl ? subject_tracks(&tr)
              : album ? library_list(S.lib, LIB_ALBUM_TRACKS, album, &tr)
                      : library_artist_tracks(S.lib, artist, S.ctx_credited, &tr);
        pop_level(1);
        if (n > 0 && S.player && player_shuffle_these(S.player, tr.ids, tr.n, pl || album != 0)) S.now_playing_view = 1;
        else ui_toast("Nothing to shuffle", 1500);
        library_list_free(&tr);
        break;
    }
    case CTX_SHOW_QUEUE:
        pop_level(1);
        push_ui(L_QUEUE, "Queue", 0);
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
        push_ui(L_INFO, "Info", track);
        S.stack[S.depth - 1].back_to_np = from_np;
        break;
    case CTX_FIND_COVER:
    case CTX_FIND_ANOTHER:
        pop_level(1);
        cover_find_open(album, from_np);
        break;
    case CTX_REMOVE_COVER:
        pop_level(0);
        if (library_remove_found_cover(S.lib, album) == 0) {
            cover_changed(album);
            ui_toast(library_album_cover(S.lib, album) == LIB_COVER_OWN ? "Back to its own cover" : "Cover removed", 1500);
        } else {
            ui_toast("Couldn't remove it", 2000);
        }
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
        push_ui(L_PICK_PLAYLIST, "Add to playlist", 0);
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
    case CTX_RADIO_PLAY: {
        int i = S.rd_ctx_i;
        pop_level(1);
        rd_play(S.rd_st, S.rd_meta, S.rd_n, i);
        break;
    }
    case CTX_RADIO_ADD: {
        pop_level(0);
        int r = S.hooks.radio_add ? S.hooks.radio_add(&S.rd_ctx) : -1;
        ui_toast(r == 1 ? "Added to your stations" : r == 0 ? "Already in your stations"
                 : r == -2 ? "Your stations are full: remove one first" : "Couldn't save fathom/radio.txt", 2000);
        S.radio_n = S.hooks.radio_stations ? S.hooks.radio_stations(S.radio, RADIO_MAX) : -1;
        break;
    }
    case CTX_RADIO_REMOVE: {
        uint32_t now = lv_tick_get();
        if (!(S.rd_del_armed && now - S.rd_del_armed < 3000)) {
            S.rd_del_armed = now;
            ui_toast("Press again to remove it", 3000);
            break;
        }
        S.rd_del_armed = 0;
        pop_level(0);
        int r = S.hooks.radio_remove ? S.hooks.radio_remove(S.rd_ctx.url) : -1;
        ui_toast(r == 0 ? "Removed from your stations" : r == 1 ? "Not in your stations" : "Couldn't save fathom/radio.txt", 2000);
        S.radio_n = S.hooks.radio_stations ? S.hooks.radio_stations(S.radio, RADIO_MAX) : -1;
        level_t *l = &S.stack[S.depth - 1];
        if (l->kind == L_RADIO && !S.now_playing_view) {
            int n = list_count(l);
            if (l->sel >= n) l->sel = n - 1;
        }
        break;
    }
    case CTX_POD_SPEED: {                     /* the next speed; the menu stays (Play again: the next) */
        static const int SPEEDS[] = {100, 125, 150, 175, 200};
        int cur = S.player ? player_speed(S.player) : 100, k = 0;
        for (int i = 0; i < 5; i++)
            if (SPEEDS[i] == cur) k = i;
        int to = SPEEDS[(k + 1) % 5];
        if (S.player) player_set_speed(S.player, to);
        settings_set_int("podcasts.speed", to);
        break;
    }
    case CTX_POD_CHAPTERS: {
        int back = S.stack[S.depth - 1].back_to_np;
        pop_level(1);
        push_ui(L_POD_CHAPTERS, "Chapters", 0);
        level_t *c = &S.stack[S.depth - 1];
        c->back_to_np = back;
        int at = pod_chapter_at(S.now.pos_ms);
        c->sel = at >= 0 ? at : 0;
        break;
    }
    case CTX_POD_REFRESH: {
        level_t *under = &S.stack[S.depth - 2];
        const char *url = under->kind == L_POD_SHOW && S.pod_ci >= 0 ? S.pod_shows[S.pod_ci].url
                        : under->kind == L_PODCASTS && S.pod_ctx >= 0 && S.pod_ctx < S.pod_n ? S.pod_shows[S.pod_ctx].url : "";
        char u[512];
        snprintf(u, sizeof u, "%s", url);
        pop_level(1);
        if (!pod_net()) break;
        podsync_refresh(u);
        ui_toast("Checking for new episodes", 1500);
        break;
    }
    case CTX_POD_UNSUB: {
        uint32_t now = lv_tick_get();
        if (!(S.pod_del_armed && now - S.pod_del_armed < 3000)) {
            S.pod_del_armed = now;
            ui_toast("Press again to unsubscribe (its downloads go too)", 3000);
            break;
        }
        S.pod_del_armed = 0;
        pop_level(1);
        if (S.pod_ctx < 0 || S.pod_ctx >= S.pod_n) break;
        if (podsync_unsubscribe(S.pod_shows[S.pod_ctx].url) == 0) {
            memmove(&S.pod_shows[S.pod_ctx], &S.pod_shows[S.pod_ctx + 1], (size_t)(S.pod_n - S.pod_ctx - 1) * sizeof S.pod_shows[0]);
            S.pod_n--;
            level_t *l = &S.stack[S.depth - 1];
            if (l->kind == L_PODCASTS && l->sel >= list_count(l)) l->sel = list_count(l) - 1;
            ui_toast("Unsubscribed", 1500);
        }
        break;
    }
    case CTX_POD_DOWNLOAD: case CTX_POD_CANCEL: case CTX_POD_DELETE: case CTX_POD_PLAYED: case CTX_POD_UNPLAYED: {
        pop_level(1);
        if (S.pod_ci < 0 || S.pod_ctx < 0 || S.pod_ctx >= S.pod_ne) break;
        const char *id = S.pod_shows[S.pod_ci].id;
        const pod_episode_t *e = &S.pod_eps[S.pod_ctx];
        if (a == CTX_POD_DOWNLOAD) {
            if (!pod_net()) break;
            if (podsync_download(id, e) == 0) ui_toast("Downloading", 1500);
        } else if (a == CTX_POD_CANCEL) {
            podsync_cancel_download(id, e->key);
        } else if (a == CTX_POD_DELETE) {
            if (pod_delete_download(pod_dir(), id, e) == 0) ui_toast("Download deleted", 1500);
            S.pod_on_card[S.pod_ctx] = 0;
        } else {
            pod_state_t st;
            pod_state_find(S.pod_st, S.pod_nst, e->key, &st);
            st.played = a == CTX_POD_PLAYED;
            st.played_at = st.played ? (int64_t)time(NULL) : 0;
            st.pos_ms = 0;
            /* the one playing: it plays on, and its position saves keep the mark
             * (else the next, within 15 s, undid it); not played: saved again */
            if (!strcmp(S.pod_play.key, e->key) && !strcmp(S.pod_play.id, id)) {
                S.pod_play.marked = st.played;
                if (!st.played) st.pos_ms = S.pod_play.pos;
                S.pod_play.saved = S.pod_play.pos;
            }
            pod_state_put(pod_dir(), id, &st);
            pod_read_episodes();
        }
        break;
    }
    case CTX_PL_RENAME:
        S.rename_theme = 0;
        S.rename_id = S.ctx_playlist;
        pop_level(1);
        library_playlist_name(S.lib, S.rename_id, S.entry, sizeof S.entry);
        S.entry_ch = 0;
        push_ui(L_RENAME, "Rename playlist", 0);
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
                playlists_load(l);
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
        .albums = settings_get_int("shuffle.albums", UI_SHUF_ALBUMS_DEFAULT),
        .freshness = settings_get_int("shuffle.freshness", UI_SHUF_FRESH_DEFAULT),
        .skip_interludes = settings_get_int("shuffle.skip_interludes", UI_SHUF_INTERLUDES_DEFAULT),
        .exclude_spoken = settings_get_int("shuffle.exclude_spoken", UI_EXCLUDE_SPOKEN_DEFAULT),
        .keep_works = settings_get_int("shuffle.keep_works", UI_SHUF_WORKS_DEFAULT),
    };
    player_set_shuffle_prefs(S.player, &sp);
}

static void setting_ok(setting_t st)
{
    switch (st) {
    case ST_BATTERY:                       /* its history */
        if (!S.hooks.battery_report) break;
        battery_load();
        push_ui(L_BATTERY, "Battery", 0);
        break;
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
    case ST_OUTPUT: if (S.hooks.signal_path) push_ui(L_SIGNAL, "Signal path", 0); break;
    case ST_CROSSFEED:                     /* Off, Light, Medium, Strong */
        if (S.pure) { ui_toast("Bypassed in Pure mode", 1500); break; }
        if (S.hooks.sound_adjust) S.hooks.sound_adjust(UI_SOUND_CROSSFEED, 1);
        break;
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
    case ST_DIM: settings_set_int("display.dim", !settings_get_int("display.dim", UI_DIM_DEFAULT)); break;
    case ST_ON_CHARGING: settings_set_int("display.on_charging", !settings_get_int("display.on_charging", UI_ON_CHARGING_DEFAULT)); break;
    case ST_COVERS: settings_set_int("display.covers", (settings_get_int("display.covers", UI_COVERS_DEFAULT) + 1) & 3); break;
    case ST_THEME: push_ui(L_THEMES, "Theme", 0); break;
    case ST_ALBUM_ARTISTS:
        settings_set_int("display.album_artists", !album_artists_on());
        library_set_album_artists(S.lib, album_artists_on());
        break;
    case ST_DAC_FILTER: if (S.hooks.dac_filter_next) S.hooks.dac_filter_next(); break;
    case ST_EQ: {                          /* the profiles, for the output playing now */
        if (S.pure) { ui_toast("Bypassed in Pure mode", 1500); break; }
        char t[80];
        subst(t, sizeof t, tr("EQ: %s"), S.eq_out[0] ? S.eq_out : tr("this output"), NULL);
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
            char d[16], n[16];
            snprintf(d, sizeof d, "%d", done);
            snprintf(n, sizeof n, "%d", total);
            subst(msg, sizeof msg, tr("%s of %s tracks measured: the rest while charging"), d, n);
            ui_toast(msg, 3000);
        }
        break;
    }
    case ST_PAUSE_UNPLUG:
        settings_set_int("playback.pause_on_unplug", !settings_get_int("playback.pause_on_unplug", UI_PAUSE_ON_UNPLUG_DEFAULT));
        break;
    case ST_SHUF_BY: case ST_SHUF_FRESH: case ST_SHUF_INTERLUDES: case ST_SHUF_WORKS: case ST_SHUF_SPOKEN: {
        static const char *const key[] = {"shuffle.albums", "shuffle.freshness", "shuffle.skip_interludes",
                                          "shuffle.keep_works", "shuffle.exclude_spoken"};
        static const int def[] = {UI_SHUF_ALBUMS_DEFAULT, UI_SHUF_FRESH_DEFAULT, UI_SHUF_INTERLUDES_DEFAULT,
                                  UI_SHUF_WORKS_DEFAULT, UI_EXCLUDE_SPOKEN_DEFAULT};
        int k = (int)(st - ST_SHUF_BY);
        settings_set_int(key[k], !settings_get_int(key[k], def[k]));
        shuffle_prefs_apply();
        if (S.player && player_shuffle(S.player)) ui_toast("From the next shuffle", 1500);
        break;
    }
    case ST_AUTO_OFF: {
        int cur = settings_get_int("power.auto_off", UI_AUTO_OFF_DEFAULT), n = (int)(sizeof AUTO_OFFS / sizeof AUTO_OFFS[0]), i = 0;
        while (i < n && AUTO_OFFS[i] != cur) i++;
        settings_set_int("power.auto_off", AUTO_OFFS[i < n ? (i + 1) % n : 1]);
        break;
    }
    case ST_POD_AUTO: settings_set_int("podcasts.auto", !settings_get_int("podcasts.auto", 1)); break;
    case ST_POD_KEEP: case ST_POD_CLEAN: {
        const int *v = st == ST_POD_KEEP ? POD_KEEPS : POD_CLEANS;
        int n = st == ST_POD_KEEP ? 4 : 5, cur = settings_get_int(st == ST_POD_KEEP ? "podcasts.keep" : "podcasts.clean_days",
                                                                  st == ST_POD_KEEP ? 2 : 7), i = 0;
        while (i < n && v[i] != cur) i++;
        settings_set_int(st == ST_POD_KEEP ? "podcasts.keep" : "podcasts.clean_days", v[i < n ? (i + 1) % n : 0]);
        break;
    }
    case ST_POD_SPEED: {
        static const int SPEEDS[] = {100, 125, 150, 175, 200};
        int cur = S.player ? player_speed(S.player) : 100, k = 0;
        for (int i = 0; i < 5; i++)
            if (SPEEDS[i] == cur) k = i;
        if (S.player) player_set_speed(S.player, SPEEDS[(k + 1) % 5]);
        settings_set_int("podcasts.speed", SPEEDS[(k + 1) % 5]);
        break;
    }
    case ST_RESCAN: if (S.hooks.rescan) S.hooks.rescan(); break;
    case ST_FIND_COVERS: covers_find_all(); break;
    case ST_USB_DRIVE: if (S.hooks.usb_drive) S.hooks.usb_drive(); break;
    case ST_USB_COMPUTER: {      /* Ask, USB drive, USB DAC, Just charge */
        static const int order[] = {UI_USB_ASK, UI_USB_DRIVE, UI_USB_DAC, UI_USB_CHARGE};
        int v = settings_get_int("usb.computer", UI_USB_ASK), i = 0;
        while (i < 4 && order[i] != v) i++;
        settings_set_int("usb.computer", order[(i + 1) % 4]);
        break;
    }
    case ST_USB_DAC: if (S.hooks.usb_dac) S.hooks.usb_dac(); break;
    case ST_BT: push_ui(L_BLUETOOTH, "Bluetooth", 0); break;   /* the devices, and on/off */
    case ST_WIFI:                                              /* the networks, and on/off */
        push_ui(L_WIFI, "Wi-Fi", 0);
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
        case H_UPDATE: push_ui(L_UPD_ASK, "Update available", 0); break;
        case H_NOWPLAYING: S.now_playing_view = 1; break;
        case H_SHUFFLE:
            if (S.player) { player_shuffle_all(S.player); S.now_playing_view = 1; }
            break;
        case H_ALBUMS: push_ui(L_ALBUMS, "Albums", 0); break;
        case H_ARTISTS: push_ui(L_ARTISTS, "Artists", 0); break;
        case H_ALBUM_ARTISTS: push_ui(L_ALBUM_ARTISTS, "Album artists", 0); break;
        case H_SONGS: push_ui(L_SONGS, "Songs", 0); break;
        case H_PLAYLISTS: push_ui(L_PLAYLISTS, "Playlists", 0); break;
        case H_GENRES: push_ui(L_GENRES, "Genres", 0); break;
        case H_COMPOSERS: push_ui(L_COMPOSERS, "Composers", 0); break;
        case H_YEARS: push_ui(L_YEARS, "Years", 0); break;
        case H_FOLDERS: push_ui(L_FOLDER, "Folders", 0); break;
        case H_SEARCH:
            S.entry[0] = '\0';
            S.entry_ch = 0;
            search_run();
            push_ui(L_SEARCH, "Search", 0);
            break;
        case H_FAVOURITES: push_ui(L_FAVOURITES, "Favourites", 0); break;
        case H_RECENT_ADDED: push_ui(L_RECENT_ADDED, "Recently added", 0); break;
        case H_HISTORY: push_ui(L_HISTORY, "History", 0); break;
        case H_RADIO:   /* the station playing selected, else your first */
            S.radio_n = S.hooks.radio_stations ? S.hooks.radio_stations(S.radio, RADIO_MAX) : -1;
            push_ui(L_RADIO, "Radio", 0);
            S.stack[S.depth - 1].sel = S.radio_n > 0 ? 1 : 0;
            for (int i = 0; i < S.radio_n; i++)
                if (rd_on_air(&S.radio[i])) { S.stack[S.depth - 1].sel = i + 1; break; }
            break;
        case H_SETTINGS: push_ui(L_SETTINGS, "Settings", 0); break;
        case H_PODCASTS: pod_open(); break;
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
    case L_HISTORY: push_ui(l->sel == 0 ? L_RECENT_PLAYED : L_MOST_PLAYED, HISTORY[l->sel], 0); break;
    case L_RADIO: {
        if (l->sel == 0) {                       /* Find stations */
            push_ui(L_RDIR, "Find stations", 0);
            break;
        }
        int i = l->sel - 1;
        if (!S.player || i >= S.radio_n) break;
        char why[96] = "";
        if (S.hooks.radio_ready && S.hooks.radio_ready(why, sizeof why) != 0) {
            if (!wifi_ask_if_off(WT_OK)) ui_toast(why, 2500);
            break;
        }
        if (rd_on_air(&S.radio[i])) { S.now_playing_view = 1; break; }
        player_radio_play(S.player, S.radio, S.radio_n, i);
        S.now_playing_view = 1;
        break;
    }
    case L_RDIR:
        switch (l->sel) {
        case RD_MENU_SEARCH:
            S.entry[0] = '\0';
            S.entry_ch = 0;
            S.ok_added = 0;
            push_ui(L_RDIR_SEARCH, "Search by name", 0);
            rd_free_list();
            break;
        case RD_MENU_RANDOM: {
            rdir_query_t q;
            rd_random_filter(&q, name, sizeof name);
            rd_random(&q);
            break;
        }
        case RD_MENU_POPULAR: {
            rdir_query_t q = {.by = RDIR_POPULAR};
            rd_open_results("Popular", 1, &q, NULL);
            break;
        }
        case RD_MENU_GENRES: push_ui(L_RDIR_GENRES, "Genres", 0); break;
        case RD_MENU_COUNTRIES:
            push_ui(L_RDIR_COUNTRIES, "Countries", 0);
            rd_ask(NULL);
            break;
        }
        break;
    case L_RDIR_GENRES:
        if (l->sel < RDIR_N_GENRES) {
            rdir_query_t q = {.by = RDIR_TAG};
            rdir_genre_tag(l->sel, q.arg, sizeof q.arg);
            rd_open_results(RDIR_GENRES[l->sel], 1, &q, "tag");
        }
        break;
    case L_RDIR_COUNTRIES:
        if (l->sel < S.rd_cty_n) {
            rdir_query_t q = {.by = RDIR_COUNTRY};
            snprintf(q.arg, sizeof q.arg, "%s", S.rd_cty[l->sel].code);
            snprintf(name, sizeof name, "%s", S.rd_cty[l->sel].name);
            rd_open_results(name, 0, &q, "country");
        } else if (!S.rd_job && S.rd_state == RD_FAILED) {
            rd_ask(NULL);                        /* try again */
        }
        break;
    case L_RDIR_RESULTS:
        if (S.rd_n <= 0) { if (!S.rd_job && S.rd_state == RD_FAILED) rd_ask(&S.rd_q); }
        else if (l->sel == 0) rd_random(&S.rd_q);
        else rd_play(S.rd_st, S.rd_meta, S.rd_n, l->sel - 1);
        break;
    case L_RDIR_SEARCH:   /* (the letters: ui_key) */
        if (S.rd_n > 0) rd_play(S.rd_st, S.rd_meta, S.rd_n, l->sel - 1);
        else if (!S.rd_job && S.rd_state == RD_FAILED) rd_ask(&S.rd_q);
        break;
    case L_ALBUM_ARTISTS:
        if (id >= 0 && library_artist_name(S.lib, id, name, sizeof name) == 0) push(L_ARTIST_ALBUMS, name, id);
        break;
    case L_ARTISTS:
        if (id >= 0 && library_artist_name(S.lib, id, name, sizeof name) == 0) push(L_CREDIT_ALBUMS, name, id);
        break;
    case L_CREDIT_ALBUMS:   /* their songs on this album */
        if (id >= 0 && library_album_name(S.lib, id, name, sizeof name) == 0) push_credit_tracks(name, l->parent, id);
        break;
    case L_WIFI_ASK:
        pop();
        if (l->sel == 0 && S.hooks.wifi_do) {
            S.hooks.wifi_do(UI_WIFI_TOGGLE, NULL, NULL);
            settings_set_int("wifi.on", 1);
            level_t *b = &S.stack[S.depth - 1];
            S.wifi_then.armed = S.wifi_then.what != WT_NONE;
            S.wifi_then.depth = S.depth;
            S.wifi_then.kind = b->kind;
            S.wifi_then.sel = b->sel;
            S.wifi_then.at = lv_tick_get();
            ui_toast("Turning on Wi-Fi", 1500);
        }
        break;
    case L_UPD_ASK:
        if (l->sel == 0) {
            /* Settings > About > Check for updates, as if chosen there (it
             * offers to turn on Wi-Fi first if it's off) */
            while (S.depth > 1) pop();
            S.stack[0].sel = home_index(H_SETTINGS);
            push_ui(L_SETTINGS, "Settings", 0);
            S.stack[S.depth - 1].sel = SET_ABOUT;
            push_ui(L_ABOUT, "About", 0);
            level_t *a = &S.stack[S.depth - 1];
            int rows = list_count(a);               /* (the device's rows: about_n) */
            for (int r = 0; r < rows; r++)          /* Check for updates (What's new may come first) */
                if (about_row(r) == AB_CHECK) { a->sel = r; break; }
            open_selected();
        } else {
            const char *tag = update_offered();
            if (tag) {
                settings_set("update.ignored", tag);
                settings_save();
            }
            pop();
            home_sync();
            ui_toast("Hidden until a newer version", 2000);
        }
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
        if (id < 0 && -id < LIB_SMART_N) push(L_SMART_TRACKS, tr(library_smart_name((int)-id)), -id);
        else if (id >= 0 && library_playlist_name(S.lib, id, name, sizeof name) == 0) push(L_PLAYLIST_TRACKS, name, id);
        break;
    case L_TRACKS:
    case L_CREDIT_TRACKS:
    case L_COMPOSER_TRACKS:
    case L_SONGS:
    case L_PLAYLIST_TRACKS:
    case L_SMART_TRACKS:
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
        int net, row = wifi_row(l->sel, &net);
        if (row == WR_TOGGLE) {
            S.hooks.wifi_do(UI_WIFI_TOGGLE, NULL, NULL);
            settings_set_int("wifi.on", !S.wifi_on);
        } else if (row == WR_LINK) {
            if (S.wifi_st.state == UI_WIFI_CONNECTED) {
                char t[80];
                char dbm[16];
                snprintf(dbm, sizeof dbm, "%d", S.wifi_st.dbm);
                subst(t, sizeof t, tr("%s, signal %s dBm"), S.wifi_st.ip, dbm);
                ui_toast(t, 2500);
            }
        } else if (row == WR_SEARCH) {
            S.hooks.wifi_do(UI_WIFI_SCAN, NULL, NULL);
        } else if (net < S.wifi_n) {
            const ui_wifi_net_t *w = &S.wifi[net];
            if (w->connected) ui_toast("Connected", 1000);
            else if (w->saved || !w->secured) S.hooks.wifi_do(UI_WIFI_CONNECT, w->ssid, "");
            else {                                         /* a new network: its password */
                snprintf(S.wifi_pass_ssid, sizeof S.wifi_pass_ssid, "%s", w->ssid);
                memset(S.entry, 0, sizeof S.entry);
                S.entry_ch = 0;
                char t[80];
                subst(t, sizeof t, tr("Password for %s"), w->ssid, NULL);
                push(L_WIFI_PASS, t, 0);
                ui_toast("Play adds a letter, Menu: a A 0 !", 2500);
            }
        }
        break;
    case L_SHARE:
        if (!S.hooks.share_do) break;
        if (l->sel == 0) {
            S.hooks.share_do(UI_SHARE_TOGGLE);
            if (!S.share_on && !wifi_ask_if_off(WT_NONE)) ui_toast("On while Wi-Fi is connected", 2000);
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
        else if (l->sel == 2) {
            S.hooks.bt_do(UI_BT_RANGE, NULL);
            ui_toast(settings_get_int("bluetooth.range", UI_BT_RANGE_DEFAULT) ? "Longer range: lower bitrate, no aptX. Reconnecting"
                                                            : "Best sound. Reconnecting", 2500);
        }
        else if (l->sel - BT_DEV_ROW < S.bt_n) S.hooks.bt_do(UI_BT_PRESS, S.bt[l->sel - BT_DEV_ROW].addr);
        break;
    case L_SETTINGS: {
        static const list_kind_t sub[] = {L_INTERFACE, L_HOME_EDIT, L_DISPLAY, L_SOUND, L_BLUETOOTH, L_WIFI, L_SHARE, L_SSH,
                                          L_USB, L_PLAYBACK, L_POD_SETTINGS, L_POWER, L_LIBRARY, L_ABOUT};
        push_ui(sub[l->sel], SETTINGS[l->sel], 0);
        if (sub[l->sel] == L_HOME_EDIT) ui_toast("Play shows or hides. Menu: Move", 2500);
        if (sub[l->sel] == L_WIFI && S.hooks.wifi_do) S.hooks.wifi_do(UI_WIFI_SCAN, NULL, NULL);   /* fresh networks */
        break;
    }
    case L_ABOUT:
        if (about_row(l->sel) == AB_WHATSNEW) {
            whats_new_open();
        } else if (about_row(l->sel) == AB_CHECK) {    /* Check for updates: from here, or */
            if (wifi_ask_if_off(WT_OK)) break;
            S.hooks.update_do(UI_UPD_CHECK);
            push_ui(L_UPDATE, "Updates", 0);
        } else if (about_row(l->sel) == AB_AUTO) {     /* by itself (on by default) */
            int on = !settings_get_int("update.auto", UI_UPDATE_AUTO_DEFAULT);
            settings_set_int("update.auto", on);
            settings_save();
            if (on) ui_toast("Once a day, while the H2 rests on Wi-Fi", 3000);
        } else if (about_row(l->sel) == AB_UNINSTALL) {
            uninstall_look();
            if (S.uni.state != UI_UNI_PENDING) S.hooks.uninstall_do(UI_UNI_CHECK);
            S.uni_armed = 0;
            push_ui(L_UNINSTALL, "Uninstall Fathom", 0);
            ui_uninstall_changed();
        } else if (about_row(l->sel) == AB_RESET) {   /* Reset all settings: press twice */
            uint32_t now = lv_tick_get();
            if (S.reset_armed && now - S.reset_armed < 3000) {
                S.reset_armed = 0;
                int used = settings_get_int("battery.used", 0);   /* (the battery's wear: not a setting) */
                settings_reset();
                if (used) settings_set_int("battery.used", used);
                library_set_album_artists(S.lib, album_artists_on());
                home_load();
                S.stack[0].sel = home_index(H_SETTINGS);
                if (S.hooks.brightness) S.hooks.brightness(brightness());
                if (S.hooks.screen_timeout) S.hooks.screen_timeout(screen_timeout());
                ui_set_theme(0);
                if (S.hooks.restart) {
                    /* the rest (language, sound, levelling, shuffle, Wi-Fi...) is
                     * set at start: start again, once the message has been seen */
                    ui_toast("Settings reset. Starting again", 2000);
                    lv_timer_t *rt = lv_timer_create(reset_restart, 1500, NULL);
                    if (rt) lv_timer_set_repeat_count(rt, 1);
                } else {
                    ui_toast("Settings reset", 1500);
                }
            } else {
                S.reset_armed = now;
                ui_toast("Press again to reset all settings", 3000);
            }
        }
        break;
    case L_UPDATE: update_ok(l->sel); break;
    case L_UNINSTALL: uninstall_ok(l->sel); break;
    case L_BATTERY:
        if (l->sel == 0) {                         /* the range: 24 hours or 7 days */
            S.bat_days = S.bat_days == 7 ? 1 : 7;
            battery_load();
        } else if (l->sel == 1) {
            if (!S.bat.have_since) { ui_toast("Not since a full charge yet", 2000); break; }
            S.bat_span = -1;
            push_ui(L_BAT_SPAN, "Since full charge", 0);
        } else if (S.bat.nspans) {
            push_ui(L_BAT_HISTORY, "History", 0);
        }
        break;
    case L_BAT_HISTORY:
        if (l->sel < S.bat.nspans) {
            char day[32];
            S.bat_span = l->sel;
            bat_day(day, sizeof day, S.bat.span[l->sel].start);
            push(L_BAT_SPAN, day, 0);
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
            snprintf(t, sizeof t, "%s %s", tr(EQ_TYPE_NAME[S.eqe.band[l->sel - 1].type]), hz);
            push(L_EQ_BAND, t, l->sel - 1);
        } else if (l->sel == n + 1 && n < EQ_MAX_BANDS) {        /* a flat peak at 1 kHz, to shape */
            S.eqe.band[n] = (eq_band_t){EQ_PEAK, 1000, 0, 1};
            S.eqe.n++;
            eqe_changed(1);
            char t[80], hz[24];
            fmt_hz(hz, sizeof hz, 1000);
            snprintf(t, sizeof t, "%s %s", tr(EQ_TYPE_NAME[EQ_PEAK]), hz);
            push(L_EQ_BAND, t, n);
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
    case L_THEME_EDIT: case L_THEME_COLOURS: the_ok(l); break;
    case L_PODCASTS:
        if (l->sel == 0) {                        /* Find podcasts */
            S.entry[0] = '\0';
            S.entry_ch = 0;
            S.ok_added = 0;
            if (S.pod_search != 0) { S.pod_search = -1; S.pod_nf = 0; }
            push_ui(L_POD_SEARCH, "Find podcasts", 0);
        } else if (l->sel - 1 < S.pod_n) {
            pod_open_show(l->sel - 1);
        } else if (S.pod_opml[0] && podsync_import_opml(S.pod_opml) == 0) {
            ui_toast("Adding its shows\xE2\x80\xA6", 2000);
        }
        break;
    case L_POD_SHOW: pod_play_episode(l->sel); break;
    case L_POD_CHAPTERS:
        if (S.player && l->sel < S.pod_nch && S.now.podcast) {
            player_seek_by(S.player, (int)S.pod_ch[l->sel].start_ms - (int)S.now.pos_ms);
            if (!S.now.playing) player_pause(S.player, 0);
            pop();
            S.now_playing_view = 1;
        }
        break;
    case L_POD_SEARCH:                            /* a show found: add it */
        if (l->sel >= 1 && l->sel - 1 < S.pod_nf) {
            const pod_show_t *f = &S.pod_found[l->sel - 1];
            if (pod_subscribed(f->url)) ui_toast("Already in your podcasts", 1500);
            else if (podsync_subscribe(f) == 0) {
                if (S.pod_n < POD_MAX_SHOWS) S.pod_shows[S.pod_n++] = *f;   /* (marked Added now; read again later) */
                ui_toast("Added: reading its episodes", 2000);
            }
        }
        break;
    case L_THEMES: {
        if (l->sel >= S.n_themes) { the_open(-1); break; }   /* New theme */
        const theme_t *th = &S.themes[l->sel];
        if (th->status == THEME_ERROR) { ui_toast(th->note, 4000); break; }
        ui_set_theme(l->sel);
        if (th->status == THEME_WARN) ui_toast(th->note, 4000);
        break;
    }
    case L_QUICK: case L_DISPLAY: case L_SOUND: case L_PLAYBACK: case L_POWER: case L_LIBRARY: case L_USB: case L_POD_SETTINGS:
    case L_INTERFACE:
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
            subst(msg, sizeof msg, tr("Added to %s"), nm, NULL);
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
    int slot = on && !S.now_playing_view && list_count(&S.stack[S.depth - 1]) > 0 ? slot_under_pill() : -1;
    if (slot >= 0 && slot < SLOTS && !S.line_on[slot]) lv_label_set_long_mode(S.row_lbl[slot], LV_LABEL_LONG_SCROLL);
    for (int r = 0; r < SLOTS; r++)   /* album rows: title and artist as one line */
        if (S.line_on[r]) line_scroll(r, r == slot);
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
    covers_reset(1);
    finish_transition();
    pending_free();
    for (int d = 0; d < S.depth; d++) {
        level_t *l = &S.stack[d];
        if (!library_backed(l->kind) || !S.lib) continue;
        int64_t was = l->sel < l->items.n ? l->items.ids[l->sel] : -1;
        library_list_free(&l->items);
        if (l->kind == L_CREDIT_TRACKS) library_list_credit_tracks(S.lib, l->parent, l->parent2, &l->items);
        else if (l->kind == L_COMPOSER_TRACKS) library_list_composer_tracks(S.lib, l->parent, l->parent2, &l->items);
        else if (l->kind == L_PLAYLISTS) playlists_load(l);
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
    if (link == 2) wifi_then_run(1);
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
        covers_free();                        /* (read again when the screen's back) */
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
    int had_help = S.help_shown;
    help_hide();
    marquee_arm();
    if (k == UI_KEY_OK || k == UI_KEY_BACK || k == UI_KEY_BACK_LONG) capture();
    level_t *l = &S.stack[S.depth - 1];
    player_t *pl = S.player;
    if (S.upd_ov && update_overlay_key(k)) return;
    if (S.wn_ov && whats_new_key(k)) return;
    if (S.uni_ov && uninstall_overlay_key(k)) return;
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
        else if (had_help) ;                       /* (Menu again: just put it away) */
        else if (show_help(l)) ;
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
    if (!S.now_playing_view && l->kind == L_COVER_FIND) {   /* Find cover: turn through the matches */
        if (k == UI_KEY_UP || k == UI_KEY_DOWN || k == UI_KEY_OK) {
            cover_find_key(k);
            render(R_ANIMATE);
            return;
        }
        if (k == UI_KEY_BACK || k == UI_KEY_BACK_LONG) {
            cf_find_close();
            pop_level(k == UI_KEY_BACK_LONG);
            if (k == UI_KEY_BACK_LONG) S.now_playing_view = 1;
            render(R_ANIMATE);
            return;
        }
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
    if ((l->kind == L_SEARCH || l->kind == L_RENAME || l->kind == L_WIFI_PASS || l->kind == L_RDIR_SEARCH ||
         l->kind == L_POD_SEARCH) && l->sel == 0) {   /* typing with the wheel */
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
            if (dbl || S.entry_ch == nl) {               /* Results / Done / Join / Search */
                if (l->kind == L_SEARCH) { if (n > 1) l->sel = 1; }
                else if (l->kind == L_RDIR_SEARCH) {      /* asks the directory: the stations come in later */
                    if (!S.entry[0]) { ui_toast("Type a name first", 1500); break; }
                    S.rd_q = (rdir_query_t){.by = RDIR_NAME};
                    snprintf(S.rd_q.arg, sizeof S.rd_q.arg, "%s", S.entry);
                    rd_free_list();
                    rd_ask(&S.rd_q);
                    if (S.rd_state == RD_FAILED) l->sel = 1;   /* (why, on the line below) */
                }
                else if (l->kind == L_POD_SEARCH) pod_search_run();
                else if (l->kind == L_WIFI_PASS) {
                    if (strlen(S.entry) < 8) { ui_toast("At least 8 characters", 1500); break; }
                    if (S.hooks.wifi_do) S.hooks.wifi_do(UI_WIFI_CONNECT, S.wifi_pass_ssid, S.entry);
                    memset(S.entry, 0, sizeof S.entry);
                    pop();
                    ui_toast("Joining...", 1500);
                }
                else if (S.rename_theme) {                /* the theme editor's name */
                    if (!S.entry[0]) { ui_toast("Type a name first", 1500); break; }
                    snprintf(S.the.name, sizeof S.the.name, "%s", S.entry);
                    S.the_dirty = 1;
                    pop();
                    level_t *up = &S.stack[S.depth - 1];
                    if (up->kind == L_THEME_EDIT) snprintf(up->title, sizeof up->title, "%s", S.the.name);
                }
                else if (S.entry[0] && library_playlist_rename(S.lib, S.rename_id, S.entry) == 0) {
                    pop();
                    level_t *up = &S.stack[S.depth - 1];   /* show the new name */
                    if (up->kind == L_PLAYLISTS) playlists_load(up);
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
    if ((l->kind == L_SEARCH || l->kind == L_RDIR_SEARCH || l->kind == L_POD_SEARCH) && k == UI_KEY_BACK) {   /* in the results: back to the letters */
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
        render(R_ANIMATE);                 /* (the pill goes with the item) */
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
        render(R_ANIMATE);                 /* (the pill goes with the item) */
        return;
    }
    if (S.moving && l->kind == L_QUEUE) {   /* moving a queued track with the wheel */
        switch (k) {
        case UI_KEY_UP: if (l->sel > 0) { player_queue_move(pl, l->sel, -1); l->sel--; } break;
        case UI_KEY_DOWN: if (l->sel < n - 1) { player_queue_move(pl, l->sel, +1); l->sel++; } break;
        case UI_KEY_OK: case UI_KEY_BACK: S.moving = 0; break;
        default: break;
        }
        render(R_ANIMATE);                 /* (the pill goes with the item) */
        return;
    }
    if (S.editing && (l->kind == L_THEME_EDIT || l->kind == L_THEME_COLOURS)) {   /* a theme's value, with the wheel */
        switch (k) {
        case UI_KEY_UP: the_adjust(l, -1); break;
        case UI_KEY_DOWN: the_adjust(l, +1); break;
        case UI_KEY_OK: case UI_KEY_BACK: S.editing = 0; break;
        default: break;
        }
        render(R_CONTENT);
        return;
    }
    if (l->kind == L_THEME_EDIT && k == UI_KEY_BACK && S.the_dirty) {   /* leaving it unsaved: press twice */
        uint32_t now = lv_tick_get();
        if (!(S.the_back_armed && now - S.the_back_armed < 3000)) {
            S.the_back_armed = now;
            ui_toast("Not saved: press Back again to leave it", 3000);
            render(R_CONTENT);
            return;
        }
        S.the_back_armed = 0;
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
    int asking = rd_poll();      /* the station directory's answer */
    asking |= pod_poll();        /* ...and the podcast directory's */
    asking |= cf_poll();         /* ...and covers' */
    if (!S.player) return asking;
    uint32_t seq = player_seq(S.player);
    player_tick(S.player);
    /* scrubbing: cancelled after a pause in turning, or when the track changes */
    if (S.scrubbing && (lv_tick_elaps(S.scrub_last) > SCRUB_IDLE_MS || player_seq(S.player) != S.scrub_seq)) {
        S.scrubbing = 0;
        render(R_CONTENT);
    }
    if (player_seq(S.player) != seq) render(R_CONTENT);   /* gapless track change */
    return asking || S.scrubbing || !player_idle(S.player);
}

void ui_tick_1s(void)
{
    if (S.wifi_then.armed && lv_tick_elaps(S.wifi_then.at) >= 45000) wifi_then_run(0);   /* (no network joined, say) */
    list_kind_t k = S.stack[S.depth - 1].kind;
    if ((k == L_BATTERY || k == L_BAT_SPAN || k == L_BAT_HISTORY) && lv_tick_elaps(S.bat_at) >= 60000) battery_load();
    pod_second();
    S.ticking = 1;
    render(R_CONTENT);
    S.ticking = 0;
    settings_autosave(lv_tick_get(), 1500);
}

void ui_save_state(void)
{
    if (!pod_dir()[0]) return;
    if (!pod_track() && S.pod_play.key[0]) pod_save_play(0);   /* (one that just ended stays played) */
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

/* Screenshots: the directory's answer (saved ones, in the simulator) before the picture. */
static void rd_wait(void)
{
    for (int i = 0; i < 300 && S.rd_job; i++) {
        if (!rd_poll()) break;
        struct timespec t = {0, 10 * 1000 * 1000L};
        nanosleep(&t, NULL);
    }
}

int ui_list_state(int *sel, int *n, int *home_edit)
{
    const level_t *l = &S.stack[S.depth - 1];
    if (sel) *sel = l->sel;
    if (n) *n = list_count(l);
    if (home_edit) *home_edit = l->kind == L_HOME_EDIT;
    return S.now_playing_view ? 0 : S.depth;
}

void ui_show(const char *what)
{
    S.scrubbing = 0;
    finish_transition();
    pending_free();
    while (S.depth > 1) pop();
    S.stack[0].sel = home_index(H_ALBUMS);
    S.now_playing_view = 0;
    if (!strcmp(what, "albums") || !strcmp(what, "tracks")) {
        push_ui(L_ALBUMS, "Albums", 0);
        level_t *l = &S.stack[S.depth - 1];
        l->sel = l->items.n > 1 ? 1 : 0;
        char name[128];
        if (!strcmp(what, "tracks") && l->items.n && S.lib &&
            library_album_name(S.lib, l->items.ids[l->sel], name, sizeof name) == 0)
            push(L_TRACKS, name, l->items.ids[l->sel]);
    } else if (!strcmp(what, "playlists") || !strcmp(what, "playlist") || !strncmp(what, "smart", 5)) {
        /* Playlists: the third of yours ("playlists"), opened ("playlist"); the
         * first smart one ("smartlists"), opened ("smartopen"), its menu ("smartmenu") */
        S.stack[0].sel = home_index(H_PLAYLISTS);
        push_ui(L_PLAYLISTS, "Playlists", 0);
        level_t *l = &S.stack[S.depth - 1];
        int first = 0;
        while (first < l->items.n && l->items.ids[first] < 0) first++;   /* (the smart ones first) */
        int smart = !strncmp(what, "smart", 5);
        l->sel = smart ? 0 : first + 2 < l->items.n ? first + 2 : first;
        char name[128];
        if (!strcmp(what, "playlist") && l->sel < l->items.n && S.lib &&
            library_playlist_name(S.lib, l->items.ids[l->sel], name, sizeof name) == 0)
            push(L_PLAYLIST_TRACKS, name, l->items.ids[l->sel]);
        if (!strcmp(what, "smartopen") && l->items.n && l->items.ids[0] < 0)
            push(L_SMART_TRACKS, tr(library_smart_name((int)-l->items.ids[0])), -l->items.ids[0]);
        if (!strcmp(what, "smartmenu")) open_context();
    } else if (!strcmp(what, "share")) {
        S.stack[0].sel = home_index(H_SETTINGS);
        push_ui(L_SETTINGS, "Settings", 0);
        S.stack[S.depth - 1].sel = SET_SHARE;
        push_ui(L_SHARE, "Network drive", 0);
        S.stack[S.depth - 1].sel = 1;
    } else if (!strcmp(what, "ssh")) {
        S.stack[0].sel = home_index(H_SETTINGS);
        push_ui(L_SETTINGS, "Settings", 0);
        S.stack[S.depth - 1].sel = SET_SSH;
        push_ui(L_SSH, "SSH", 0);
        S.stack[S.depth - 1].sel = 1;
    } else if (!strcmp(what, "about") || !strcmp(what, "update")) {
        S.stack[0].sel = home_index(H_SETTINGS);
        push_ui(L_SETTINGS, "Settings", 0);
        S.stack[S.depth - 1].sel = SET_ABOUT;
        push_ui(L_ABOUT, "About", 0);
        level_t *l = &S.stack[S.depth - 1];
        l->sel = S.about_n;                         /* Check for updates (or what follows) */
        if (!strcmp(what, "update")) push_ui(L_UPDATE, "Updates", 0);
        ui_update_changed();
    } else if (!strcmp(what, "eqedit") || !strcmp(what, "eqband")) {
        /* the EQ editor on a profile, its curve over it: the 8.9 kHz band selected,
         * or its own page with the gain being turned up */
        eqe_open("AirPods Pro");
        level_t *e = &S.stack[S.depth - 1];
        int b = 0;
        for (int i = 0; i < S.eqe.n; i++)
            if (S.eqe.band[i].fc > 8000 && S.eqe.band[i].fc < 9000) b = i;
        e->sel = b + 1;
        if (!strcmp(what, "eqband")) {
            S.eqe.band[b].gain_db = 8;
            push(L_EQ_BAND, "Peak 8.9 kHz", b);
            S.stack[S.depth - 1].sel = 2;
            S.editing = 1;
        }
    } else if (!strcmp(what, "podsettings")) {      /* Settings > Podcasts */
        S.stack[0].sel = home_index(H_SETTINGS);
        push_ui(L_SETTINGS, "Settings", 0);
        S.stack[S.depth - 1].sel = SET_PODCASTS;
        push_ui(L_POD_SETTINGS, "Podcasts", 0);
    } else if (!strcmp(what, "libsettings")) {      /* Settings > Library, on Find missing covers */
        S.stack[0].sel = home_index(H_SETTINGS);
        push_ui(L_SETTINGS, "Settings", 0);
        S.stack[S.depth - 1].sel = SET_LIBRARY;
        push_ui(L_LIBRARY, "Library", 0);
        S.stack[S.depth - 1].sel = 1;
    } else if (!strcmp(what, "covermenu") || !strcmp(what, "coverfind") || !strcmp(what, "covermenuown")) {
        /* an album without a cover: Menu on it ("covermenu"), Find cover's first match ("coverfind");
         * one with its own: Menu on it ("covermenuown") */
        int want = !strcmp(what, "covermenuown") ? LIB_COVER_OWN : LIB_COVER_NONE;
        S.stack[0].sel = home_index(H_ALBUMS);
        push_ui(L_ALBUMS, "Albums", 0);
        level_t *l = &S.stack[S.depth - 1];
        for (int i = 0; S.lib && i < l->items.n; i++)
            if (library_album_cover(S.lib, l->items.ids[i]) == want) { l->sel = i; break; }
        if (strcmp(what, "coverfind")) {
            open_context();
            for (int i = 0; i < S.ctx_n; i++)
                if (S.ctx[i] == CTX_FIND_COVER || S.ctx[i] == CTX_FIND_ANOTHER) S.stack[S.depth - 1].sel = i;
        } else if (l->items.n) {
            cover_find_open(l->items.ids[l->sel], 0);
            for (int i = 0; i < 300; i++) {          /* (the simulator's saved answers, and the picture) */
                int n = 0;
                cf_poll();
                if (cf_find_state(&n) == 1 && n && cf_find_show(0, NULL, NULL) != 0) break;
                struct timespec ts = {0, 10 * 1000 * 1000L};
                nanosleep(&ts, NULL);
            }
        }
    } else if (!strcmp(what, "albummenu") || !strcmp(what, "artistmenu") || !strcmp(what, "aartistmenu")) {
        /* Menu on an album (Play, Shuffle, ...), an artist in Artists, one in Album artists */
        int art = !strcmp(what, "artistmenu"), aart = !strcmp(what, "aartistmenu");
        S.stack[0].sel = home_index(art ? H_ARTISTS : aart ? H_ALBUM_ARTISTS : H_ALBUMS);
        push_ui(art ? L_ARTISTS : aart ? L_ALBUM_ARTISTS : L_ALBUMS, art ? "Artists" : aart ? "Album artists" : "Albums", 0);
        S.stack[S.depth - 1].sel = S.stack[S.depth - 1].items.n > 2 ? 2 : 0;
        open_context();
    } else if (!strncmp(what, "pod", 3)) {
        /* Podcasts (the simulator's sample shows): "podcasts" the list, "podshow" a
         * show's episodes, "podsearch" Find podcasts' answer, "podnp" an episode playing */
        S.stack[0].sel = home_index(H_PODCASTS);
        pod_open();
        if (!strcmp(what, "podshow") || !strcmp(what, "podnp") || !strcmp(what, "podchapters") || !strcmp(what, "podmenu")) {
            S.stack[S.depth - 1].sel = 1;
            pod_open_show(0);
            S.stack[S.depth - 1].sel = S.pod_ne > 1 ? 1 : 0;
        }
        if ((!strcmp(what, "podnp") || !strcmp(what, "podchapters") || !strcmp(what, "podmenu")) && S.pod_ne > 1 && S.lib) {   /* (a song of the library stands in for its file) */
            lib_list_t songs;
            lib_track_t t;
            if (library_list(S.lib, LIB_SONGS, 0, &songs) > 0 && library_track(S.lib, songs.ids[0], &t) == 0) {
                player_episode_t pe = {0};
                snprintf(pe.show, sizeof pe.show, "%s", S.pod_shows[0].title);
                snprintf(pe.title, sizeof pe.title, "%s", S.pod_eps[1].title);
                snprintf(pe.path, sizeof pe.path, "%s", t.path);
                snprintf(pe.id, sizeof pe.id, "%s", S.pod_shows[0].id);
                snprintf(pe.key, sizeof pe.key, "%s", S.pod_eps[1].key);
                pe.len_ms = (uint32_t)S.pod_eps[1].dur_s * 1000;
                pe.start_ms = 23 * 60 * 1000;
                S.pod_nch = pod_chapters(pod_dir(), pe.id, &S.pod_eps[1], S.pod_ch, 64);
                snprintf(S.pod_ch_key, sizeof S.pod_ch_key, "%s", pe.key);
                player_episode_play(S.player, &pe);
                player_pause(S.player, 1);
                snprintf(S.pod_play.id, sizeof S.pod_play.id, "%s", pe.id);
                snprintf(S.pod_play.key, sizeof S.pod_play.key, "%s", pe.key);
                S.now_playing_view = 1;
            }
            if (songs.ids) library_list_free(&songs);
            if (!strcmp(what, "podmenu")) {             /* Menu on it: Chapters, Speed (at 1.25x) */
                player_set_speed(S.player, 125);
                player_now(S.player, &S.now);
                open_context();
                S.stack[S.depth - 1].sel = 1;
            }
            if (!strcmp(what, "podchapters") && S.pod_nch) {
                S.now_playing_view = 0;
                push_ui(L_POD_CHAPTERS, "Chapters", 0);
                S.stack[S.depth - 1].sel = 1;
            }
        }
        if (!strcmp(what, "podsearch")) {
            S.stack[S.depth - 1].sel = 0;
            snprintf(S.entry, sizeof S.entry, "radiolab");
            S.entry_ch = 0;
            push_ui(L_POD_SEARCH, "Find podcasts", 0);
            pod_search_run();
            for (int i = 0; i < 300 && S.pod_search == 0; i++) {
                pod_poll();
                struct timespec ts = {0, 10 * 1000 * 1000L};
                nanosleep(&ts, NULL);
            }
        }
    } else if (!strncmp(what, "theme", 5) && strcmp(what, "themes")) {
        /* the theme editor, as in its mockups: a new dark theme with a coral accent, "Harbour";
         * "themesnew": the list's New theme row; "themecolours": its colours, Text selected;
         * "themecolour": Text being picked, at Dark grey (bad contrast); "themesave": Save */
        if (!strcmp(what, "themesnew")) {
            push_ui(L_THEMES, "Theme", 0);
            S.stack[S.depth - 1].sel = S.n_themes;
        } else {
            the_open(-1);
            for (S.the_acc = 0; strcmp(ACCENTS[S.the_acc].name, "Coral"); S.the_acc++) {}
            theme_make(&S.the, 1, 215, 30);
            theme_accent(&S.the, ACCENTS[S.the_acc].hue, ACCENTS[S.the_acc].sat);
            snprintf(S.the.name, sizeof S.the.name, "Harbour");
            level_t *e = &S.stack[S.depth - 1];
            snprintf(e->title, sizeof e->title, "%s", S.the.name);
            e->sel = !strcmp(what, "themesave") ? TE_SAVE : TE_ACCENT;
            if (!strcmp(what, "themecolours") || !strcmp(what, "themecolour")) {
                push_ui(L_THEME_COLOURS, "Colours", 0);
                S.stack[S.depth - 1].sel = TC_TEXT;
            }
            if (!strcmp(what, "themecolour")) {
                S.the_pal = 4;                               /* Dark grey */
                S.the.text = the_pal_at(S.the_pal, NULL, 0);
                S.editing = 1;
            }
        }
    } else if (!strcmp(what, "uninstall")) {
        S.stack[0].sel = home_index(H_SETTINGS);
        push_ui(L_SETTINGS, "Settings", 0);
        S.stack[S.depth - 1].sel = SET_ABOUT;
        push_ui(L_ABOUT, "About", 0);
        push_ui(L_UNINSTALL, "Uninstall Fathom", 0);
        ui_uninstall_changed();
    } else if (!strncmp(what, "battery", 7)) {       /* "battery", "battery7", "batterysince", "batteryhistory" */
        S.stack[0].sel = home_index(H_SETTINGS);
        push_ui(L_SETTINGS, "Settings", 0);
        S.stack[S.depth - 1].sel = SET_POWER;
        push_ui(L_POWER, "Power", 0);
        S.bat_days = !strcmp(what, "battery7") ? 7 : 1;
        battery_load();
        push_ui(L_BATTERY, "Battery", 0);
        if (!strcmp(what, "battery7")) S.stack[S.depth - 1].sel = 0;
        if (!strcmp(what, "batterysince")) { S.stack[S.depth - 1].sel = 1; S.bat_span = -1; push_ui(L_BAT_SPAN, "Since full charge", 0); }
        if (!strcmp(what, "batteryhistory")) { S.stack[S.depth - 1].sel = 2; push_ui(L_BAT_HISTORY, "History", 0); }
    } else if (!strcmp(what, "radio")) {
        S.stack[0].sel = home_index(H_RADIO);
        S.radio_n = S.hooks.radio_stations ? S.hooks.radio_stations(S.radio, RADIO_MAX) : -1;
        push_ui(L_RADIO, "Radio", 0);
        if (S.radio_n > 1) S.stack[S.depth - 1].sel = 2;
    } else if (!strncmp(what, "rdir", 4)) {         /* Radio > Find stations, from saved answers */
        S.stack[0].sel = home_index(H_RADIO);
        S.radio_n = S.hooks.radio_stations ? S.hooks.radio_stations(S.radio, RADIO_MAX) : -1;
        push_ui(L_RADIO, "Radio", 0);
        push_ui(L_RDIR, "Find stations", 0);
        level_t *m = &S.stack[S.depth - 1];
        if (!strcmp(what, "rdir")) {
            char v[32];
            snprintf(v, sizeof v, "%s:%s:%s", "tag", "jazz", "Jazz");   /* (as after opening Jazz) */
            settings_set("radio.random", v);
            m->sel = RD_MENU_RANDOM;
        } else if (!strcmp(what, "rdircountries") || !strcmp(what, "rdirerror")) {
            m->sel = RD_MENU_COUNTRIES;
            open_selected();
            rd_wait();
            level_t *c = &S.stack[S.depth - 1];
            for (int i = 0; i < S.rd_cty_n; i++)
                if (!strcmp(S.rd_cty[i].code, "GB")) c->sel = i;
        } else if (!strcmp(what, "rdirsearch")) {
            m->sel = RD_MENU_SEARCH;
            open_selected();
            snprintf(S.entry, sizeof S.entry, "fip");
            S.entry_ch = (int)strlen(SEARCH_CHARS);   /* the wheel on Search */
            S.rd_q = (rdir_query_t){.by = RDIR_NAME, .arg = "fip"};
            rd_ask(&S.rd_q);
            rd_wait();
            S.stack[S.depth - 1].sel = 0;
        } else {                                     /* rdirgenres, rdirresults, rdirbusy, rdirctx */
            m->sel = RD_MENU_GENRES;
            open_selected();
            level_t *g = &S.stack[S.depth - 1];
            for (int i = 0; i < RDIR_N_GENRES; i++)
                if (!strcmp(RDIR_GENRES[i], "Jazz")) g->sel = i;
            if (strcmp(what, "rdirgenres")) {
                open_selected();
                if (strcmp(what, "rdirbusy")) rd_wait();
                S.stack[S.depth - 1].sel = S.rd_n > 1 ? 2 : 0;
                if (!strcmp(what, "rdirctx")) open_context();
            }
        }
    } else if (!strcmp(what, "search")) {
        S.stack[0].sel = home_index(H_SEARCH);
        snprintf(S.entry, sizeof S.entry, "bu");
        S.entry_ch = 'e' - 'a';
        search_run();
        push_ui(L_SEARCH, "Search", 0);
    } else if (!strcmp(what, "artists") || !strcmp(what, "artist") || !strcmp(what, "artistsongs")) {
        S.stack[0].sel = home_index(H_ARTISTS);
        push_ui(L_ARTISTS, "Artists", 0);
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
        push_ui(L_ALBUM_ARTISTS, "Album artists", 0);
    } else if (!strcmp(what, "homeend")) {    /* Home, at its last items */
        S.stack[0].sel = S.home_n > 0 ? S.home_n - 2 : 0;
    } else if (!strcmp(what, "homeall")) {    /* Settings > Home: every item, shown or not */
        S.stack[0].sel = home_index(H_SETTINGS);
        push_ui(L_SETTINGS, "Settings", 0);
        push_ui(L_HOME_EDIT, "Home", 0);
        S.stack[S.depth - 1].sel = H_N - 2;
    } else if (!strcmp(what, "genres")) {
        S.stack[0].sel = home_index(H_GENRES);
        push_ui(L_GENRES, "Genres", 0);
    } else if (!strcmp(what, "folders")) {
        S.stack[0].sel = home_index(H_FOLDERS);
        push_ui(L_FOLDER, "Folders", 0);
    } else if (!strcmp(what, "folder")) {     /* the first folder in Folders */
        S.stack[0].sel = home_index(H_FOLDERS);
        push_ui(L_FOLDER, "Folders", 0);
        level_t *l = &S.stack[S.depth - 1];
        char name[128];
        if (l->items.n && l->items.ids[0] < 0 && S.lib &&
            library_folder_name(S.lib, -l->items.ids[0], name, sizeof name) == 0)
            push(L_FOLDER, name, -l->items.ids[0]);
    } else if (!strcmp(what, "years")) {
        S.stack[0].sel = home_index(H_YEARS);
        push_ui(L_YEARS, "Years", 0);
    } else if (!strcmp(what, "scrub")) {       /* Now Playing with the marker moved on */
        S.now_playing_view = 1;
        render(R_SNAP);
        for (int i = 0; i < 12; i++) ui_key(UI_KEY_DOWN);
        finish_transition();
    } else if (!strcmp(what, "usbask")) {
        ui_usb_prompt();
    } else if (!strcmp(what, "wifiask")) {          /* (as from a station, with Wi-Fi off) */
        push_ui(L_RADIO, "Radio", 0);
        push_ui(L_WIFI_ASK, "Turn on Wi-Fi?", 0);
    } else if (!strcmp(what, "interface")) {
        S.stack[0].sel = home_index(H_SETTINGS);
        push_ui(L_SETTINGS, "Settings", 0);
        push_ui(L_INTERFACE, "Interface", 0);
    } else if (!strcmp(what, "sound")) {
        S.stack[0].sel = home_index(H_SETTINGS);
        push_ui(L_SETTINGS, "Settings", 0);
        S.stack[S.depth - 1].sel = SET_SOUND;
        push_ui(L_SOUND, "Sound", 0);
        S.stack[S.depth - 1].sel = 2;
    } else if (!strcmp(what, "signal")) {
        S.stack[0].sel = home_index(H_SETTINGS);
        push_ui(L_SETTINGS, "Settings", 0);
        S.stack[S.depth - 1].sel = SET_SOUND;
        push_ui(L_SOUND, "Sound", 0);
        push_ui(L_SIGNAL, "Signal path", 0);
    } else if (!strcmp(what, "homeedit")) {
        S.stack[0].sel = home_index(H_SETTINGS);
        push_ui(L_SETTINGS, "Settings", 0);
        S.stack[S.depth - 1].sel = SET_HOME;
        push_ui(L_HOME_EDIT, "Home", 0);
        S.stack[S.depth - 1].sel = 5;
    } else if (!strcmp(what, "songs")) {
        S.stack[0].sel = home_index(H_SONGS);
        push_ui(L_SONGS, "Songs", 0);
    } else if (!strcmp(what, "quick")) {
        open_quick();
    } else if (!strcmp(what, "queue")) {
        S.now_playing_view = 1;
        if (open_context() == 0) run_context(CTX_SHOW_QUEUE);
    } else if (!strcmp(what, "context") || !strcmp(what, "info")) {
        push_ui(L_ALBUMS, "Albums", 0);
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
        push_ui(L_SETTINGS, "Settings", 0);
        push_ui(L_THEMES, "Theme", 0);
    }
    render(R_SNAP);
}

/* ---------- construction ---------- */

void ui_init(lv_obj_t *screen, const theme_t *themes, int n_themes,
             const ui_fonts_t *fonts, int n_fonts, library_t *lib, player_t *player)
{
    memset(&S, 0, sizeof S);
    S.lib = lib;
    library_set_album_artists(lib, album_artists_on());
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
    S.cf_dsc = S.art_dsc;
    S.cf_dsc.data = (const uint8_t *)S.cf_px;
    S.cf_dsc.data_size = sizeof S.cf_px;
    S.themes = themes; S.n_themes = n_themes;
    S.fonts = fonts; S.n_fonts = n_fonts;
    S.battery_pct = -1;
    S.pod_ci = -1;
    S.pod_search = -1;
    if (player) player_set_speed(player, settings_get_int("podcasts.speed", 100));
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
        S.row_cov[j] = lv_image_create(row);
        lv_obj_remove_flag(S.row_cov[j], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_pos(S.row_cov[j], 10, (ROW_H - LIB_ART_SMALL) / 2);
        S.cov_dsc[j].header.magic = LV_IMAGE_HEADER_MAGIC;
        S.cov_dsc[j].header.cf = LV_COLOR_FORMAT_RGB565;
        S.cov_dsc[j].header.w = LIB_ART_SMALL;
        S.cov_dsc[j].header.h = LIB_ART_SMALL;
        S.cov_dsc[j].header.stride = LIB_ART_SMALL * 2;
        S.cov_id[j] = -1;
        show(S.row_cov[j], 0);
        S.row_picon[j] = lv_image_create(row);
        lv_obj_remove_flag(S.row_picon[j], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_align(S.row_picon[j], LV_ALIGN_LEFT_MID, 10, 0);
        lv_obj_set_style_image_recolor_opa(S.row_picon[j], LV_OPA_COVER, 0);
        show(S.row_picon[j], 0);
        S.row_mark[j] = lv_image_create(row);
        lv_obj_remove_flag(S.row_mark[j], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_pos(S.row_mark[j], 0, (ROW_H - MARK_PX) / 2);
        lv_obj_set_style_image_recolor_opa(S.row_mark[j], LV_OPA_COVER, 0);
        show(S.row_mark[j], 0);
        S.row_lbl[j] = label(row);
        lv_obj_align(S.row_lbl[j], LV_ALIGN_LEFT_MID, 10, 0);
        S.row_meta[j] = label(row);
        lv_obj_set_width(S.row_meta[j], LV_SIZE_CONTENT);
        lv_obj_align(S.row_meta[j], LV_ALIGN_RIGHT_MID, -10, 0);
        S.row_line[j] = box(row, 10, 0, 10, ROW_H);   /* (cuts what it holds at its edges) */
        lv_obj_set_user_data(S.row_line[j], (void *)(intptr_t)j);
        S.row_lt[j] = label(S.row_line[j]);
        S.row_la[j] = label(S.row_line[j]);
        show(S.row_line[j], 0);
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

    /* Find cover: a match's picture, as Now Playing's, and what it is */
    S.cf = box(screen, 0, HEADER_H, UI_W, UI_H - HEADER_H);
    S.cf_art = box(S.cf, 12, 6, LIB_ART_LARGE, LIB_ART_LARGE);
    lv_obj_set_style_clip_corner(S.cf_art, true, 0);
    S.cf_img = lv_image_create(S.cf_art);
    lv_obj_set_pos(S.cf_img, 0, 0);
    show(S.cf_img, 0);
    S.cf_title = label(S.cf);
    lv_obj_set_pos(S.cf_title, 160, 8);
    lv_obj_set_size(S.cf_title, 150, 64);
    lv_label_set_long_mode(S.cf_title, LV_LABEL_LONG_DOT);
    S.cf_artist = label(S.cf);
    lv_obj_set_pos(S.cf_artist, 160, 74);
    lv_obj_set_width(S.cf_artist, 150);
    lv_label_set_long_mode(S.cf_artist, LV_LABEL_LONG_DOT);
    S.cf_meta = label(S.cf);
    lv_obj_set_pos(S.cf_meta, 160, NP_ALBUM_Y);
    lv_obj_set_width(S.cf_meta, 150);
    lv_label_set_long_mode(S.cf_meta, LV_LABEL_LONG_DOT);
    S.cf_note = label(S.cf);                /* "Source: MusicBrainz": two lines where the font is wide */
    lv_obj_set_pos(S.cf_note, 160, 122);
    lv_obj_set_width(S.cf_note, 150);
    lv_label_set_long_mode(S.cf_note, LV_LABEL_LONG_WRAP);
    S.cf_hint = label(S.cf);
    lv_obj_set_pos(S.cf_hint, 12, UI_H - HEADER_H - 30);
    lv_obj_set_width(S.cf_hint, UI_W - 24);
    lv_label_set_long_mode(S.cf_hint, LV_LABEL_LONG_DOT);
    show(S.cf, 0);

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
    lv_label_set_long_mode(S.empty, LV_LABEL_LONG_WRAP);   /* a message on two lines rather than cut */
    lv_obj_set_width(S.empty, UI_W - 40);
    lv_obj_set_style_text_align(S.empty, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(S.empty, LV_ALIGN_CENTER, 0, -10);
    show(S.empty, 0);

    S.stack[0].kind = L_HOME;
    home_load();
    snprintf(S.stack[0].title, sizeof S.stack[0].title, "Fathom");
    S.stack[0].title_ui = 1;
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
