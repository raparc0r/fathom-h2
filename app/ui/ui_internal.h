/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/*
 * The interface's own header: what ui.c and the files beside it share and nothing
 * outside app/ui includes. The screen's layout numbers, the kinds of page, row,
 * setting and menu action, the one state of the interface (S), the small helpers
 * every screen is built with, and the functions one of its files has for the
 * others. The public API is ui.h.
 */
#ifndef FATHOM_UI_INTERNAL_H
#define FATHOM_UI_INTERNAL_H

#include "ui/ui.h"
#include "ui/coverflow.h"
#include "ui/fonts.h"
#include "core/lang.h"
#include "core/settings.h"
#include "core/datetime.h"
#include "core/tz.h"
#include "library/cue.h"
#include "library/playlist.h"
#include "core/rng.h"
#include "net/radiodir.h"
#include "net/radioicon.h"
#include "net/podcast.h"
#include "net/podsync.h"
#include "net/coverfind.h"
#include "net/netclock.h"
#include "core/clock.h"
#include "core/utf8.h"

#include <math.h>
#include <stddef.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#define HEADER_H   28
#define ROW_PITCH  42
#define ROW_H      38
#define ROWS       5
#define ROW_Y0     ((ROW_H - 38) / 2)   /* the playing-bars marker, centred in a row */
#define ROW_MARGIN 6
#define POSBAR_W   3     /* the position bar, in the right margin (clear of the pill) */
#define POSBAR_PAD 4
#define POSBAR_MIN 16
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
/* A row's value at its right, and what follows a name on its line (an album's
 * artist, the song after "Now playing"): 16 px in Atkinson, where meta is 15
 * (the owner, 2026-11-25); meta itself in the pixel families and a theme's own font. */
#define VALUE_FONT (S.f->value ? S.f->value : S.f->meta)
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

/* (Items added later come after Settings in this numbering, which pixel_icons.h
 * follows; on Home they go before Settings.) */
enum { H_NOWPLAYING, H_SHUFFLE, H_SEARCH, H_ALBUMS, H_ARTISTS, H_ALBUM_ARTISTS, H_SONGS, H_PLAYLISTS,
       H_FAVOURITES, H_RECENT_ADDED, H_HISTORY, H_RADIO, H_SETTINGS, H_GENRES, H_COMPOSERS, H_YEARS, H_FOLDERS,
       H_PODCASTS, H_N };
/* Home's "Update available!": first on Home while a newer Fathom is on offer
 * (update.found, from the update check) and not ignored (update.ignored); not
 * an item Settings > Interface > Home screen items shows, hides or moves. Its icon: circle-arrow-up
 * (Tabler), and Pixel Pastel's own after the items' (pixel_icons.h). */
#define H_UPDATE H_N
#define UPDATE_ICON 0xf703
/* Home's "Get language packs": after the update's row and like it, while the
 * library has names a pack would draw and that pack isn't there
 * (ui_pack_t.songs) nor put off (packs.ignored). The update's icon. It opens
 * Settings > Interface > Language packs. */
#define H_PACKS (H_N + 2)
#define PODCAST_ICON 0xe000   /* drawn into the icon font: tools/fonts/add_podcast_icon.py */
/* Playlists: a smart playlist's icon, sparkles (Tabler, copied into the icon
 * font), and Pixel Pastel's own after the update's; yours have Home's playlist */
#define SMART_ICON 0xf6d7
#define PIXEL_SMART (H_N + 1)
#define PIXEL_ICON_SCALE 2
#define ICON_W 22     /* the icon's column; the text starts after it */
#define ICON_GAP 10
#define BT_DEV_ROW 3   /* Settings > Bluetooth: on/off, Find headphones, Audio, then the devices */
#define SWATCH_W 30   /* Settings > Interface > Theme: a little picture of each theme */
#define SWATCH_H 20

/* Settings' pages, in their order (their names: SETTINGS in ui.c) */
enum { SET_SOUND, SET_PLAYBACK, SET_INTERFACE, SET_WIFI, SET_BLUETOOTH, SET_PODCASTS, SET_DISPLAY, SET_LIBRARY, SET_USB,
       SET_SHARE, SET_POWER, SET_SSH, SET_ABOUT };

/* Rows of the quick settings panel and of Settings > Display. */
typedef enum {
    ST_BRIGHTNESS, ST_SHUFFLE, ST_REPEAT, ST_SLEEP, ST_STOP_AFTER, ST_TIMEOUT,
    ST_DIM, ST_ON_CHARGING, ST_COVERS, ST_ALBUM_FLOW, ST_OUTPUT, ST_EQ, ST_LEVELLING, ST_DAC_FILTER, ST_PAUSE_UNPLUG, ST_AUTO_OFF,
    ST_RESCAN, ST_USB_DRIVE, ST_USB_COMPUTER, ST_USB_DAC, ST_N_SONGS, ST_N_ALBUMS, ST_N_ARTISTS, ST_N_PLAYLISTS,
    ST_BT, ST_WIFI,
    ST_SHUF_BY, ST_SHUF_FRESH, ST_SHUF_INTERLUDES, ST_SHUF_WORKS, ST_SHUF_SPOKEN, ST_BALANCE, ST_VOL_LIMIT, ST_PURE, ST_TRANSPORT, ST_BATTERY, ST_LANGUAGE,
    ST_THEME, ST_ALBUM_ARTISTS, ST_POD_AUTO, ST_POD_KEEP, ST_POD_CLEAN, ST_POD_SPEED, ST_FIND_COVERS, ST_CROSSFEED, ST_WIDTH,
    ST_HOME_SCREEN, ST_N_UNREAD, ST_PACKS,
} setting_t;

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
    L_UPDATE, L_BATTERY, L_BAT_SPAN, L_BAT_HISTORY, L_UNINSTALL, L_PACKS, L_UPD_ASK, L_COVER_FIND,
    L_SMART_TRACKS,   /* a smart playlist's songs (parent: LIB_SMART_*) */
    L_UNREAD          /* Settings > Library > Files not read */
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
    int from_np;            /* opened from Now Playing's menu, and closing it goes to the lists (Go to album, Go to artist) */
    int typed;              /* a typing screen with results: the typing was left for them (typing_on) */
} level_t;

typedef enum { R_SNAP, R_ANIMATE, R_CONTENT } render_mode_t;
typedef enum { TR_NONE, TR_FORWARD, TR_BACK, TR_NP_IN, TR_NP_OUT } trans_t;

struct ui_state {
    const theme_t *themes; int n_themes, cur_theme;
    const ui_fonts_t *fonts; int n_fonts;
    const ui_fonts_t *f;

    level_t stack[10]; int depth;   /* (ten: an EQ's band, quick settings over it and a network's password need nine) */
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
    int entry_back;                         /* the cursor: how many letters from the end (<Prev>, <Next>); 0: at the end */
    /* A Wi-Fi password is typed over another typing screen (quick settings,
     * from Search or a name): that screen's text and wheel, kept meanwhile */
    char entry_kept[64];
    int entry_kept_ch, entry_kept_back, entry_kept_on;
    uint32_t ok_tick;                       /* when OK last added a letter (a second OK soon after: double-click) */
    int ok_added;
    lib_search_t found;                     /* Search: the results */
    int64_t rename_id;
    lv_timer_t *lvl_timer;
    int charging;
    ui_hooks_t hooks;
    int editing;            /* a value row is being changed with the wheel */

    int moving;             /* Queue: the selected track moves with the wheel */
    int64_t move_track;     /* ...which one (its row changes when the song does) */
    int home_order[H_N];    /* every Home item, in the order set in Settings > Interface > Home screen items */
    int home_hidden[H_N];   /* by item */
    int home[H_N + 2], home_n;  /* the items shown on Home (+ H_UPDATE and H_PACKS, first, when there are) */
    lv_timer_t *marquee_timer;  /* starts scrolling long text after a pause */
    lv_obj_t *np_next, *np_out, *np_pause, *empty;
    int lib_counts[4];      /* songs, albums, artists, playlists (Library screen) */
    int lib_unread;         /* music files the scans couldn't read (its last row, and that list) */
    uint32_t reset_armed;   /* About > Reset: first press time; a second within 3 s resets */
    uint32_t uni_armed;     /* About > Uninstall Fathom > Uninstall: the same */
    ui_pack_t packs[UI_MAX_PACKS];   /* Language packs: the packs, as the device last gave them */
    int packs_all;          /* ...every pack the library needs is being fetched, one after another */
    int npacks;
    uint32_t pack_armed;    /* ...Remove: the first press's time */
    int pack_armed_i;
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
    int pick_gen;           /* ...+1 each time they're chosen: an older list of playlists left open isn't for them */
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
    lv_obj_t *posbar; int posbar_y, posbar_h;   /* where the selection is in a list longer than the screen */
    lv_obj_t *row[SLOTS], *row_lbl[SLOTS], *row_meta[SLOTS], *row_dot[SLOTS], *row_eq[SLOTS][3];
    lv_obj_t *row_line[SLOTS], *row_lt[SLOTS], *row_la[SLOTS];   /* album rows: title and artist, one line */
    lv_obj_t *row_icon[SLOTS], *row_picon[SLOTS];   /* Home: an icon glyph, or a pixel icon */
    lv_obj_t *row_mark[SLOTS];                      /* a podcast episode on the card: an arrow onto a line */
    lv_obj_t *row_sw[SLOTS], *row_sw_part[SLOTS][3];  /* Theme: a little picture of each (bar, text, accent) */
    lv_obj_t *row_cov[SLOTS];       /* Albums: each album's cover, the library's 34 px thumbnail */
    lv_image_dsc_t cov_dsc[SLOTS];  /* (their pixels: g_cov, only while Albums is shown) */
    int64_t cov_id[SLOTS];          /* the album each slot shows (-1: to be read) */
    unsigned char cov_has[SLOTS];   /* ...and whether it has a picture (Podcasts: a show without one shows an icon) */
    lv_obj_t *row_wheel[SLOTS], *row_ch[SLOTS][3];   /* the typing row: letter before, the wheel's (larger), after */
    int slot_playing[SLOTS];
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
    dt_t clk;               /* About > Clock: the date and time being set */
    lv_obj_t *clk_ov;       /* ...on its own screen, over everything */
    /* "Turn on Wi-Fi?" answered Turn on, or something asked for while Wi-Fi
     * was still joining: what to do once it connects, if the screen it was
     * asked from is still there (wifi_then_run) */
    struct {
        int what, armed, depth, sel, q_null;
        list_kind_t kind;
        uint32_t at;
        rdir_query_t q;
    } wifi_then;
    lv_obj_t *np_radio, *np_radio_px;   /* the radio's sign where a cover would be */
    /* Albums as cover flow (Settings > Interface > Cover flow): a panel over the
     * rows of a list of albums. The picture (ui/coverflow.c) and what is under it. */
    lv_obj_t *flow, *flow_img, *flow_title, *flow_by, *flow_track, *flow_thumb;
    lv_obj_t *flow_az[27];              /* "#", A to Z along the foot: where in the alphabet (a list by name) */
    lv_obj_t *flow_az_pill;             /* round themes: a small pill behind the letter that's on */
    int flow_az_every;                  /* 1: every letter drawn; 2: every other (a wide font), and the one that's on */
    int flow_az_on;                     /* the letter lit (0 "#", 1..26; -1: none) */
    lv_image_dsc_t flow_dsc;
    int flow_shift;                     /* the slide under way (FLOW_STEPs: 0 at rest) */
    int flow_sel;                       /* the album the picture is of, of flow_ids (-1: none yet) */
    const int64_t *flow_ids;            /* ...the list it belongs to, as last drawn */
    unsigned flow_gen;                  /* ...and the library then */
    int flow_theme;                     /* ...and the theme */
    int64_t flow_named;                 /* the album the two lines under it name */
    lv_timer_t *flow_more;              /* covers still to read: the picture is drawn again */
    lv_obj_t *np, *np_shadow, *np_art, *np_title, *np_artist, *np_album, *np_chip,
             *np_track, *np_fill, *np_seg[SEGMENTS], *np_t0, *np_t1, *np_img, *h_mode;
    /* header indicators: each slot shows the theme's icon or the built-in symbol */
#define HDR_SLOTS 6
    lv_obj_t *h_slot_img[HDR_SLOTS], *h_slot_sym[HDR_SLOTS];
    lv_obj_t *h_pulse;      /* the charging icon, pulsing (NULL: none) */
    int asleep;             /* the screen is off: no animations, nothing drawn */
    int media_key;          /* the key being handled is from the headphones' buttons (ui_media_key) */
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
    int bt_back_to, bt_back_gone;   /* Settings > Bluetooth: the row to select again once Bluetooth is back on (0: none), and
                                     * that its rows have been seen to go (the H2 takes a moment to stop it) */
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
    int64_t radio_key[RADIO_MAX];   /* each one's picture in the list's cache (COV_RADIO | its icon's id; -1: no icon) */
    int radio_art;          /* art_px holds the radio's cover */
    int radio_logo;         /* ...or, the song having none, the station's logo: */
    char logo_url[520];     /* the station whose icon was read for it, or tried ("?": to be read again) */
    unsigned logo_seq, ricon_seq;   /* the icons on the card as last looked at (ricon_seq), for it and for the list */
    /* Find stations (net/radiodir.h): the request running (RD_JOB_*) for the
     * level at rd_job_depth; the open list's stations (Results, Search) and its
     * query; the countries; how the last request for the list went; the
     * station a context menu is for */
    int rd_job, rd_job_depth;
    /* Where the station on air was started from: a list of the directory still
     * open (its level's depth: Next and Previous go through that list), such
     * a list since left with the station not one of the user's (-1: it plays
     * alone, no Next), or anything else (0: the user's stations, Random). */
    int radio_from;
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
    lv_obj_t *scan_ov, *scan_step, *scan_count, *scan_left, *scan_file;   /* Rescan's progress (ui_show_scan) */
    lv_timer_t *scan_timer;
    int scan_was;           /* the step shown (-1: none yet) */
    uint32_t scan_t0, scan_left_at;
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
};
/* The one there is (ui.c). The interface's files have always called it S: the
 * name the linker sees is longer, so that it can meet no other. */
extern struct ui_state g_ui;
#define S g_ui

enum { RD_JOB_NONE, RD_JOB_LIST, RD_JOB_RANDOM };
enum { RD_IDLE, RD_FAILED };
enum { RD_MENU_SEARCH, RD_MENU_RANDOM, RD_MENU_POPULAR, RD_MENU_GENRES, RD_MENU_COUNTRIES, RD_MENU_N };

/* ---------- helpers ---------- */

static inline lv_color_t C(uint32_t rgb) { return lv_color_hex(rgb); }
static inline const theme_t *T(void) { return &S.themes[S.cur_theme]; }

static inline lv_obj_t *box(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static inline lv_obj_t *label(lv_obj_t *parent)
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
static inline void fill(lv_obj_t *o, uint32_t rgb)
{
    if (!lv_color_eq(lv_obj_get_style_bg_color(o, 0), C(rgb))) lv_obj_set_style_bg_color(o, C(rgb), 0);
    if (lv_obj_get_style_bg_opa(o, 0) != LV_OPA_COVER) lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
}

/* An edge's colour and a corner's radius, set only when they differ (setting
 * one again, the same, has LVGL draw the object again: the screens are
 * rendered every second) */
static inline void set_border_color(lv_obj_t *o, uint32_t rgb)
{
    if (!lv_color_eq(lv_obj_get_style_border_color(o, 0), C(rgb))) lv_obj_set_style_border_color(o, C(rgb), 0);
}

static inline void set_radius(lv_obj_t *o, int r)
{
    if (lv_obj_get_style_radius(o, 0) != r) lv_obj_set_style_radius(o, r, 0);
}

/* Pixels the UI makes itself, described for LVGL to draw: w by h, a byte
 * each of coverage (LV_COLOR_FORMAT_A8) or two of colour (RGB565) */
static inline void image_dsc(lv_image_dsc_t *d, lv_color_format_t cf, int w, int h, const void *px, size_t size)
{
    d->header.magic = LV_IMAGE_HEADER_MAGIC;
    d->header.cf = cf;
    d->header.w = w;
    d->header.h = h;
    d->header.stride = w * (cf == LV_COLOR_FORMAT_A8 ? 1 : 2);
    d->data_size = size;
    d->data = px;
}

static inline void style_label(lv_obj_t *l, const lv_font_t *font, uint32_t color)
{
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, C(color), 0);
}

static inline void show(lv_obj_t *o, int on)
{
    if (!on == lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return;
    if (on) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

/* Change a label only when something really differs: any text or style update
 * makes LVGL lay the label out again, which restarts a scrolling label from
 * its start (the screens re-render every second). */
static inline void text_copy_free(lv_event_t *e) { free(lv_obj_get_user_data(lv_event_get_target(e))); }

/* The comparison is with a copy of what was set (the label's user data): a
 * label cut short with "..." holds its text with the dots in, so it never
 * compares equal, and was laid out and redrawn every second. (Labels set
 * here are set only here, once they have been.) */
static inline void set_text(lv_obj_t *l, const char *s)
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

/* How wide a text is on one line in a font, in px */
static inline int text_w(const char *s, const lv_font_t *font)
{
    lv_point_t sz;
    lv_text_get_size(&sz, s, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return sz.x;
}

/* A translated text with a number in it: tr_num(b, n, "%s songs", 12) */
static inline void tr_num(char *out, size_t cap, const char *fmt_en, long v)
{
    char num[24];
    snprintf(num, sizeof num, "%ld", v);
    tr_fill(out, cap, fmt_en, num, NULL);
}

static inline void set_pos_width(lv_obj_t *o, int x, int y, int w)
{
    if (lv_obj_get_style_x(o, 0) != x || lv_obj_get_style_y(o, 0) != y) lv_obj_set_pos(o, x, y);
    if (lv_obj_get_style_width(o, 0) != w) lv_obj_set_width(o, w);
}

static inline void set_text_color(lv_obj_t *o, lv_color_t c)
{
    if (!lv_color_eq(lv_obj_get_style_text_color(o, 0), c)) lv_obj_set_style_text_color(o, c, 0);
}

static inline void fmt_time(char *b, size_t n, int secs, int neg)
{
    snprintf(b, n, "%s%d:%02d", neg ? "-" : "", secs / 60, secs % 60);
}

/* For `motion = stepped`: snap the remaining distance to whole steps. */
static inline int quantize(int v, int target)
{
    if (!T()->motion_stepped) return v;
    return target + (v - target) / MOTION_STEP * MOTION_STEP;
}

/* ---------- ui_graphs.c: Battery's page and the EQ editor's curve ---------- */

#define BAT_GRAPH_H 90          /* the graph above the rows (three fit below it) */
int ui_zone(void);                       /* the time zone times are shown in (clock.tz) */
const char *ui_month(int mon);           /* a month's short name (1-12), translated */
void bat_day(char *b, size_t n, int64_t t);
void battery_row(list_kind_t k, int i, char *main, size_t mn, char *meta, size_t tn);
void battery_labels(void);
int battery_place(list_kind_t k);
void eqg_update(void);
int eq_place(list_kind_t k);

#endif
