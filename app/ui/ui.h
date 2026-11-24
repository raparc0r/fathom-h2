/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* Fathom player UI (LVGL). Platform code (simulator or device) feeds keys and ticks. */
#ifndef FATHOM_UI_H
#define FATHOM_UI_H

#include "audio/eq.h"
#include "core/batlog.h"
#include "library/library.h"
#include "lvgl.h"
#include "player/player.h"
#include "ui/theme.h"

/* The screen: the H2's 320x240. */
#define UI_W 320
#define UI_H 240

typedef enum {
    UI_KEY_UP,          /* wheel counter-clockwise */
    UI_KEY_DOWN,        /* wheel clockwise */
    UI_KEY_OK,          /* centre (Play) key: select in lists, play/pause in Now Playing */
    UI_KEY_BACK,
    UI_KEY_BACK_LONG,   /* toggle Now Playing <-> where you were */
    UI_KEY_MENU,        /* short press: context menu for the selection or the playing track */
    UI_KEY_MENU_LONG,   /* held: quick settings */
    UI_KEY_BRIGHTER,    /* wheel turned while Menu is held */
    UI_KEY_DIMMER,
    UI_KEY_PLAY,        /* play/pause from anywhere */
    UI_KEY_PREV,
    UI_KEY_NEXT,
    UI_KEY_SEEK_BACK,   /* Prev / Next held: repeats while held */
    UI_KEY_SEEK_FWD,
} ui_key_t;

/* Fonts for one family at the fixed UI sizes. */
struct ui_fonts {
    const char *family;      /* matches theme_t.font */
    const lv_font_t *meta;   /* 15 px: status bar, metadata */
    const lv_font_t *body;   /* 20 px: list rows */
    const lv_font_t *sub;    /* 18 px: artist line */
    const lv_font_t *title;  /* 22 px medium: Now Playing title */
    const lv_font_t *head;   /* 15-16 px medium: header */
    const lv_font_t *icon;   /* list-size icons (Home): Tabler Icons, NULL = none */
    const lv_font_t *icon_big;   /* the same at 72 px: the radio's sign where a cover would be */
};

/* Screen brightness levels (perceptual steps; the platform maps them to PWM). */
#define UI_BRIGHTNESS_LEVELS  16
#define UI_BRIGHTNESS_DEFAULT 3   /* (with the 50 kHz backlight; was 9) */
#define UI_TIMEOUT_DEFAULT    30   /* seconds; 0 = never */
/* Defaults of settings read in more than one place (settings.ini keeps
 * only what was changed: someone who never changed one gets a new default) */
#define UI_COVERS_DEFAULT          1   /* display.covers: 1 Albums, 2 Songs, 3 both */
#define UI_DIM_DEFAULT             1   /* display.dim: dimmed 5 s before the screen goes off */
#define UI_ON_CHARGING_DEFAULT     0   /* display.on_charging: the screen stays on while plugged in */
#define UI_TRANSPORT_DEFAULT       0   /* sound.transport: the USB DAC's transport mode */
#define UI_PAUSE_ON_UNPLUG_DEFAULT 1   /* playback.pause_on_unplug: headphones out pause */
#define UI_AUTO_OFF_DEFAULT        15  /* power.auto_off: minutes idle and paused; 0 = never */
#define UI_BT_RANGE_DEFAULT        0   /* bluetooth.range: 1 longer range, 0 best sound */
#define UI_SHUF_ALBUMS_DEFAULT     0   /* shuffle.albums: shuffle albums, not songs */
#define UI_SHUF_FRESH_DEFAULT      1   /* shuffle.freshness: the less played first */
#define UI_SHUF_INTERLUDES_DEFAULT 1   /* shuffle.skip_interludes */
#define UI_SHUF_WORKS_DEFAULT      0   /* shuffle.keep_works: a work's movements together */
#define UI_EXCLUDE_SPOKEN_DEFAULT  1   /* shuffle.exclude_spoken: audiobooks out of shuffle */
#define UI_UPDATE_AUTO_DEFAULT     1   /* update.auto: ask GitHub three times a day, never download */

/* What the UI asks of the platform when a setting changes (either may be NULL). */
typedef struct ui_fonts ui_fonts_t;

/* Settings > Bluetooth: one device, as the platform knows it. */
typedef struct { char addr[18]; char name[64]; int paired, connected; } ui_bt_dev_t;
enum { UI_BT_TOGGLE, UI_BT_SCAN, UI_BT_PRESS, UI_BT_FORGET, UI_BT_RANGE };
/* Wi-Fi (Settings > Wi-Fi): the state, a network in range, what to do */
typedef enum { UI_WIFI_OFF, UI_WIFI_ON, UI_WIFI_CONNECTING, UI_WIFI_CONNECTED } ui_wifi_state_t;
typedef struct { int state; char ssid[33]; char ip[16]; int dbm; } ui_wifi_status_t;
typedef struct { char ssid[33]; int dbm, secured, saved, connected; } ui_wifi_net_t;
enum { UI_WIFI_TOGGLE, UI_WIFI_SCAN, UI_WIFI_CONNECT, UI_WIFI_FORGET };
/* The network drive (Settings > Network drive): the music folder over Wi-Fi */
enum { UI_SHARE_OFF, UI_SHARE_WAITING, UI_SHARE_STARTING, UI_SHARE_ON, UI_SHARE_FAILED };
typedef struct { int state; char host[32]; char ip[16]; char user[16]; char password[32]; char cert[24]; } ui_share_t;
enum { UI_SHARE_TOGGLE, UI_SHARE_NEW_PASSWORD };
/* SSH (Settings > SSH): root's login over the USB link and Wi-Fi, and SFTP */
enum { UI_SSH_OFF, UI_SSH_STARTING, UI_SSH_ON, UI_SSH_FAILED };
typedef struct { int state; char usb_ip[16]; char wifi_ip[16]; char password[32]; } ui_ssh_t;

/* How far a scan has got (ui_hooks_t.scan_progress): its step; songs read or
 * albums looked at, done of total; the file it's reading (a song's name) */
enum { UI_SCAN_LOOKING, UI_SCAN_SONGS, UI_SCAN_ALBUMS, UI_SCAN_COVERS };
typedef struct { int step, done, total; char file[96]; } ui_scan_t;
enum { UI_SSH_TOGGLE, UI_SSH_NEW_PASSWORD };
enum { UI_SOUND_BALANCE, UI_SOUND_LIMIT, UI_SOUND_CROSSFEED, UI_SOUND_WIDTH };
/* Updates (Settings > About > Check for updates): only ever when asked.
 * The state, the versions, the release's size and the work's progress
 * (bytes done of total, while downloading, checking and unpacking). */
enum { UI_UPD_IDLE, UI_UPD_CHECKING, UI_UPD_LATEST, UI_UPD_AVAILABLE, UI_UPD_DOWNLOADING, UI_UPD_VERIFYING,
       UI_UPD_UNPACKING, UI_UPD_READY, UI_UPD_INSTALLING, UI_UPD_RESTARTING, UI_UPD_FAILED };
typedef struct {
    int state;
    char current[48], latest[32];
    int64_t size;
    uint64_t done, total;
    char message[96];       /* UI_UPD_FAILED: why (English: the UI translates it) */
    int need_mb;            /* and for want of space: how many MB it needs (0: not that) */
} ui_update_t;
enum { UI_UPD_CHECK, UI_UPD_START, UI_UPD_CANCEL, UI_UPD_INSTALL };
/* Uninstall Fathom (Settings > About > Uninstall Fathom): HiBy's update file
 * found on the card and checked (progress: bytes read of total), then made
 * ready (its bootloader listed, named update.upt); then it waits for the
 * restart into HiBy's recovery, which installs it. */
enum { UI_UNI_IDLE, UI_UNI_CHECKING, UI_UNI_FAILED, UI_UNI_READY, UI_UNI_APPLYING, UI_UNI_PENDING };
typedef struct {
    int state;
    char file[64];          /* the file's name on the card */
    char ver[16];           /* HiBy's release, by date: "2025-11-11" */
    uint64_t done, total;
    char message[96];       /* UI_UNI_FAILED: why (English: the UI translates it) */
    int need_mb;            /* and for want of space: how many MB it needs (0: not that) */
    int no_file;            /* and for want of a file: none on the card (where to get one is shown) */
} ui_uninstall_t;
enum { UI_UNI_CHECK, UI_UNI_APPLY, UI_UNI_CANCEL, UI_UNI_RESTART };

typedef struct {
    void (*brightness)(int level);        /* 1..UI_BRIGHTNESS_LEVELS: apply now */
    void (*screen_timeout)(int seconds);  /* 0 = never */
    /* The fonts for a theme, loaded on demand (ui/assets.h: ui_theme_fonts).
     * NULL: pick from the fonts given to ui_init by family name. */
    const ui_fonts_t *(*fonts)(const theme_t *t);
    /* A theme's own header icon (THEME_ICON_*), or NULL for the built-in symbol. */
    const lv_image_dsc_t *(*icon)(const theme_t *t, int which);
    /* Sound > DAC filter (the H2's own DAC); NULL hides the row. */
    void (*dac_filter_name)(char *out, size_t cap);
    void (*dac_filter_next)(void);
    /* Sound > Balance and Volume limit, for the output playing now (NULL hides
     * them): the value as shown, and a step along it (dir -1 or +1; 0 only
     * asks). sound_adjust returns -1 when the output has no such setting (line
     * out's fixed level), having said why. */
    void (*sound_value)(int what, char *out, size_t cap);
    int  (*sound_adjust)(int what, int dir);
    /* Sound > Pure mode, for the output playing now (NULL hides it): the EQ,
     * balance and levelling bypassed. The device says when it's on with
     * ui_set_pure(). */
    void (*pure_toggle)(void);
    /* Sound > Transport mode (NULL hides it): switch "sound.transport". With a
     * USB DAC playing, the H2 then only feeds it (Bluetooth off, the screen off
     * soon). */
    void (*transport_toggle)(void);
    /* Settings > Power > Battery: the charge and, on battery, the time left
     * ("62 %, about 9 h left"). NULL hides the row. */
    void (*battery_info)(char *out, size_t cap);
    /* Settings > Display > Language: the translations on the card (core/lang.h),
     * and using one ("" = English). The UI keeps the choice in display.language. */
    int  (*languages)(char names[][64], int max);
    void (*language_set)(const char *name);
    /* Sound > Output: the signal path, row i's label and value; -1 past the last. */
    int  (*signal_path)(int i, char *label, size_t lc, char *value, size_t vc);
    /* Settings > Sound > EQ (NULL: none): the profiles' names, and choosing one
     * for the output playing now ("" = off). */
    int  (*eq_list)(char names[][64], int max);
    void (*eq_set)(const char *name);
    /* The editor: read a profile (0 = loaded), save one (0 = saved; it plays
     * if it's the output's choice), play one now while it's edited (NULL =
     * back to the output's choice), delete one (0 = deleted). */
    int  (*eq_get)(const char *name, eq_profile_t *p);
    int  (*eq_put)(const char *name, const eq_profile_t *p);
    void (*eq_preview)(const eq_profile_t *p);
    int  (*eq_remove)(const char *name);
    /* The theme editor (Settings > Interface > Theme, Menu on a theme; NULL:
     * none). theme_save writes t to the card's themes folder, over `file`
     * (relative to it, as theme_t.file) or, with file "", to a new file named
     * after the theme; the file it wrote goes in saved (0 = saved).
     * themes_reload reads the catalogue again into the array ui_init was
     * given and returns how many there are; theme_remove deletes a theme's
     * file (0 = deleted). */
    /* Home > Podcasts (net/podsync.h, started by the caller): an OPML file to
     * offer to import ("Import NAME.opml"): 1 and its path, or 0 (none). */
    int  (*podcast_opml)(char *path, size_t cap);
    int  (*theme_save)(const theme_t *t, const char *file, char *saved, size_t cap);
    int  (*themes_reload)(void);
    int  (*theme_remove)(const char *file);
    /* Library > Rescan: look for new music now (in the background). */
    void (*rescan)(void);
    /* Start the player again (after Reset all settings: everything from the
     * defaults, the language, the EQ and levelling included). NULL: no restart. */
    void (*restart)(void);
    /* USB drive mode: lend the card to the computer (the player exits for it). */
    void (*usb_drive)(void);
    /* USB DAC mode: play the computer's audio through the H2 (Sound > USB DAC mode). */
    void (*usb_dac)(void);
    /* Settings > Bluetooth (NULL: not available). bt_state: 1 on, 0 off; busy gets
     * what it's doing ("Starting", "Searching", ...) or "". bt_devices: paired
     * first, then those found nearby. bt_do: UI_BT_TOGGLE, UI_BT_SCAN, or for
     * a device UI_BT_PRESS (pair, connect or disconnect) and UI_BT_FORGET. */
    int  (*bt_state)(char *busy, size_t cap);
    int  (*bt_devices)(ui_bt_dev_t *out, int max);
    void (*bt_do)(int what, const char *addr);
    /* Settings > Wi-Fi (NULL: not available). wifi_state: 1 on; busy gets what
     * it's doing ("Starting", "Searching", "Connecting") or ""; st the
     * connection. wifi_networks: those in range, strongest first. wifi_do:
     * UI_WIFI_TOGGLE, UI_WIFI_SCAN, UI_WIFI_CONNECT (ssid, password: "" for a
     * saved or open network), UI_WIFI_FORGET (ssid). wifi_wake (may be NULL):
     * something is waiting for a connection, so with nothing joined look for
     * a network now, not at the next slow look. */
    int  (*wifi_state)(char *busy, size_t cap, ui_wifi_status_t *st);
    int  (*wifi_networks)(ui_wifi_net_t *out, int max);
    void (*wifi_do)(int what, const char *ssid, const char *password);
    void (*wifi_wake)(void);
    /* Settings > Network drive (NULL: not available). share_state: 1 when it's
     * on (wanted; st says whether it's serving, waiting for Wi-Fi...). share_do:
     * UI_SHARE_TOGGLE, UI_SHARE_NEW_PASSWORD. */
    int  (*share_state)(ui_share_t *st);
    void (*share_do)(int what);
    /* Settings > SSH (NULL: not available). ssh_state: 1 when it's on; ssh_do:
     * UI_SSH_TOGGLE, UI_SSH_NEW_PASSWORD. */
    int  (*ssh_state)(ui_ssh_t *st);
    void (*ssh_do)(int what);
    /* Home > Radio (NULL: no stations): read the stations (fathom/radio.txt),
     * how many, -1 without the file. radio_ready: 0 when a station can be
     * played now, else why not (e.g. Wi-Fi is off), shown as it is. */
    int  (*radio_stations)(radio_station_t *out, int max);
    int  (*radio_ready)(char *why, size_t cap);
    /* Find stations: add a station to fathom/radio.txt or remove one (by its
     * stream), the file made from the example first if there's none yet
     * (player/radio.h: radio_add, radio_remove; their results). NULL: no
     * Add / Remove in the menus. */
    int  (*radio_add)(const radio_station_t *st);
    int  (*radio_remove)(const char *url);
    /* About: row i's label and value; returns -1 past the last row. Row 0 is
     * UI_ABOUT_CREDIT, alone (row 1 the version): with its help (UI_ABOUT_NOTICE) it is
     * the "Appropriate Legal Notices" of GPLv3 section 0, which the attribution
     * term in LICENSE (section 7(b)) asks a modified version to keep. */
#define UI_ABOUT_CREDIT "Fathom by m11n/raparc0r"
#define UI_ABOUT_NOTICE "Copyright 2026 m11n/raparc0r. GPL 3 or later with " \
                        "added terms, no warranty. " \
                        "Licence: fathom/LICENSE."
    int  (*about)(int i, char *label, size_t lc, char *value, size_t vc);
    /* A background scan running (after USB drive mode, Rescan...): 1, with
     * how far (ui_scan_t); 0 none */
    int  (*scan_progress)(ui_scan_t *p);
    /* About > Clock, set by hand (Date and time): t is seconds since 1970, UTC.
     * 0 once it's set (and kept for the next start). NULL: not settable here. */
    int  (*clock_set)(int64_t t);
    /* About > Check for updates (NULL hides it). update_do: UI_UPD_CHECK (ask
     * GitHub), UI_UPD_START (download, check and unpack the release offered),
     * UI_UPD_CANCEL, UI_UPD_INSTALL (then the H2 restarts). The device says
     * when the state changes with ui_update_changed(). */
    void (*update_state)(ui_update_t *st);
    void (*update_do)(int what);
    /* About > Uninstall Fathom (NULL hides it). uninstall_do: UI_UNI_CHECK
     * (find HiBy's update file and check it), UI_UNI_APPLY (make it ready),
     * UI_UNI_CANCEL (put it back as it was), UI_UNI_RESTART (into HiBy's recovery). The device
     * says when the state changes with ui_uninstall_changed(). */
    void (*uninstall_state)(ui_uninstall_t *st);
    void (*uninstall_do)(int what);
    /* Settings > Power > Battery: the history for a graph of `days` (1 or 7),
     * from core/batlog.c. 0, or -1 (none: Battery shows only the charge). */
    int  (*battery_report)(batrep_t *rep, int days);
} ui_hooks_t;

/* lib and player may be NULL (empty library, no playback). Settings
 * (core/settings.h) must be loaded first: the UI reads and writes them. */
void ui_init(lv_obj_t *screen, const theme_t *themes, int n_themes,
             const ui_fonts_t *fonts, int n_fonts, library_t *lib, player_t *player);
void ui_key(ui_key_t k);
int ui_tick(void);              /* ~10x per second: follows the player; 0 when it needn't until a key */
void ui_tick_1s(void);
/* What's new in this version: the release notes' summary, one point per line
 * (fathom/assets/whatsnew.txt). Shown at once if `show` (the first start of a
 * new version), and from then on in Settings > About > What's new. */
/* Before ui_init: a fresh install (no library before this start), whose Home
 * starts shorter (Genres and Folders hidden, one switch away in Settings >
 * Home); an existing owner's stays as it was. */
void ui_set_new_install(int on);
/* A fresh install's welcome: the keys nobody would guess (also Settings >
 * About > Tips), shown over everything until Play or Back */
void ui_show_welcome(void);
/* The card has no Music folder (an empty library then says so, not just
 * that there's no music). */
void ui_set_music_missing(int missing);
void ui_set_whats_new(const char *version, const char *text, int show);          /* refreshes position and progress */
void ui_set_theme(int index);
/* Before the player exits: what the UI keeps on the card now and then (where
 * a podcast episode was left), kept now. */
void ui_save_state(void);
void ui_set_hooks(const ui_hooks_t *h);
void ui_bt_changed(void);       /* Bluetooth state or devices changed: refresh its screen */
void ui_wifi_changed(void);     /* Wi-Fi state or networks changed: refresh its screens and the header */
void ui_share_changed(void);    /* the network drive's state changed: refresh its screen */
void ui_ssh_changed(void);      /* SSH's state changed: refresh its screen */
void ui_update_changed(void);   /* the update's state or progress changed: refresh its screens */
void ui_uninstall_changed(void);   /* Uninstall Fathom's state or progress changed */
/* The library changed (a rescan): the open lists are read again, in place. */
void ui_library_changed(void);
/* Covers were made (a scan in the background): shown where they were missing. */
void ui_covers_changed(void);
/* Connected to a computer: ask what to be (USB drive, USB DAC, or keep playing). */
void ui_usb_prompt(void);
void ui_usb_prompt_close(void);   /* the cable came out: the question no longer stands */
/* A full-screen message (title and a line or two), drawn over everything:
 * the screen left up while the player is away (USB drive mode). */
lv_obj_t *ui_message_screen(const char *title, const char *body);
/* USB DAC mode's screen: the stream's format (lavender) and whether it reaches
 * the DAC untouched (mint when it does), over a hint; format "" = nothing playing. */
lv_obj_t *ui_dac_screen(void);
void ui_dac_screen_set(lv_obj_t *scr, const char *format, const char *note, int exact, const char *hint);
/* What to do on a computer (settings key usb.computer). */
enum { UI_USB_ASK, UI_USB_DRIVE, UI_USB_CHARGE, UI_USB_DAC };
/* A short message over the current screen for `ms` milliseconds. */
void ui_toast(const char *text, int ms);
/* A labelled bar over the current screen (volume, brightness): `value` of `max`,
 * with `text` (e.g. "-32.0 dB") or, if NULL, "value / max". */
void ui_level(const char *label, int value, int max, const char *text);
/* The radio's cover for the song playing (LIB_ART_LARGE square, RGB565,
 * copied), or NULL: the radio sign. */
void ui_set_radio_art(const uint16_t *px);
/* Where the sound goes, shown on Now Playing ("Headphones", a USB DAC's name). */
void ui_set_output(const char *name);
/* Pure mode is on for the output playing now; bitperfect: and the samples
 * reach the DAC untouched (Now Playing's "Bit+", under the progress bar). */
void ui_set_pure(int pure, int bitperfect);
/* The screen is lit (1) or off (0): animations run only while it's lit. */
void ui_set_awake(int on);
/* Header battery indicator. percent < 0 hides it. */
void ui_set_battery(int percent, int charging);
/* The header's Bluetooth icon: UI_BT_LINK_NONE (hidden), _CONNECTING (dimmed),
 * _CONNECTED (headphones connected). */
enum { UI_BT_LINK_NONE, UI_BT_LINK_CONNECTING, UI_BT_LINK_CONNECTED };
void ui_set_bt_link(int state);
/* The EQ on the output playing now: its profile's name ("" = off) and the
 * output's name (Settings > Sound > EQ; Now Playing's format chip says EQ). */
void ui_set_eq(const char *profile, const char *output);
int  ui_theme_count(void);
/* Jump to a named screen for screenshots: "home", "albums", "tracks", "playlists",
 * "playlist", "radio", "share", "ssh", "nowplaying", "themes", "quick", "context", "info",
 * "about", "update" (Settings > About > Check for updates, in the state update_state gives);
 * Radio > Find stations: "rdir", "rdirgenres", "rdircountries", "rdirresults", "rdirsearch",
 * "rdirctx", "rdirbusy" (still asking), "rdirerror" (when the directory can't be reached). */
/* The scan's progress over everything (Settings > Library > Rescan), while
 * one runs (scan_progress); it goes when the scan ends, or on Back. */
void ui_show_scan(void);
void ui_show(const char *what);
/* For the simulator's checks: how deep the lists go (1: Home; 0: Now Playing
 * shown), the open list's selected row and its number of rows; home_edit: 1
 * when it is Settings > Home (OK there changes Home's rows). */
int ui_list_state(int *sel, int *n, int *home_edit);
/* What's typed on a typing screen, and where the cursor is in it (tests). */
const char *ui_test_entry(int *cursor);
/* Whether the wheel is typing there (0: the typing row was left for the results). */
int ui_test_typing(void);

#endif
