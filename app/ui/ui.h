/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Fathom player UI (LVGL). Platform code (simulator or device) feeds keys and ticks. */
#ifndef FATHOM_UI_H
#define FATHOM_UI_H

#include "audio/eq.h"
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
#define UI_BRIGHTNESS_DEFAULT 9
#define UI_TIMEOUT_DEFAULT    30   /* seconds; 0 = never */

/* What the UI asks of the platform when a setting changes (either may be NULL). */
typedef struct ui_fonts ui_fonts_t;

/* Settings > Bluetooth: one device, as the platform knows it. */
typedef struct { char addr[18]; char name[64]; int paired, connected; } ui_bt_dev_t;
enum { UI_BT_TOGGLE, UI_BT_SCAN, UI_BT_PRESS, UI_BT_FORGET };
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
enum { UI_SSH_TOGGLE, UI_SSH_NEW_PASSWORD };
enum { UI_SOUND_BALANCE, UI_SOUND_LIMIT };

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
    /* Quick settings: the output's EQ off, or back on to the last one it had. */
    void (*eq_toggle)(void);
    /* Library > Rescan: scan the music folder now (blocking, with its own progress screen). */
    void (*rescan)(void);
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
     * saved or open network), UI_WIFI_FORGET (ssid). */
    int  (*wifi_state)(char *busy, size_t cap, ui_wifi_status_t *st);
    int  (*wifi_networks)(ui_wifi_net_t *out, int max);
    void (*wifi_do)(int what, const char *ssid, const char *password);
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
    /* About: row i's label and value; returns -1 past the last row. */
    int  (*about)(int i, char *label, size_t lc, char *value, size_t vc);
} ui_hooks_t;

/* lib and player may be NULL (empty library, no playback). Settings
 * (core/settings.h) must be loaded first: the UI reads and writes them. */
void ui_init(lv_obj_t *screen, const theme_t *themes, int n_themes,
             const ui_fonts_t *fonts, int n_fonts, library_t *lib, player_t *player);
void ui_key(ui_key_t k);
int ui_tick(void);              /* ~10x per second: follows the player; 0 when it needn't until a key */
void ui_tick_1s(void);          /* refreshes position and progress */
void ui_set_theme(int index);
void ui_set_hooks(const ui_hooks_t *h);
void ui_bt_changed(void);       /* Bluetooth state or devices changed: refresh its screen */
void ui_wifi_changed(void);     /* Wi-Fi state or networks changed: refresh its screens and the header */
void ui_share_changed(void);    /* the network drive's state changed: refresh its screen */
void ui_ssh_changed(void);      /* SSH's state changed: refresh its screen */
/* The library changed (a rescan): the open lists are read again, in place. */
void ui_library_changed(void);
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
 * "playlist", "radio", "share", "ssh", "nowplaying", "themes", "quick", "context", "info". */
void ui_show(const char *what);

#endif
