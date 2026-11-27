/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* fathom-player's own pieces (device/main.c and the files cut out of it):
 * what they include and what they share. Not for the rest of device/. */
#ifndef FATHOM_MAIN_INT_H
#define FATHOM_MAIN_INT_H

#include <dirent.h>
#include <fcntl.h>
#include <strings.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#include <sound/asound.h>

#include "audio/engine.h"
#include "core/rng.h"
#include "core/lang.h"
#include "core/batlog.h"
#include "core/settings.h"
#include "core/datetime.h"
#include "core/tz.h"
#include "core/lat.h"
#include "net/dnscache.h"
#include "core/fsafe.h"
#include "net/netclock.h"
#include "net/tls_wrap.h"
#include "audio/mixer_alsa.h"
#include "audio_route.h"
#include "uac_relay.h"
#include "bt.h"
#include "share.h"
#include "ssh.h"
#include "updater.h"
#include "uninstaller.h"
#include "sys/uninstall.h"
#include "wifi.h"
#include "shell.h"
#include "radiometa.h"
#include "out_router.h"
#include "audio/out_bt.h"
#include "audio/output.h"
#include "disp_drm.h"
#include "hw_paths.h"
#include "core/clock.h"
#include "core/thread.h"
#include "sysfile.h"
#define DISP(f) disp_drm_##f
#define DISP_DEV HW_DRM
#define CARD_DIR "/mnt/sd"
#define USB_ONLINE HW_USB_ONLINE
#include "input.h"
#include "library/library.h"
#include "library/cue.h"
#include "library/reader.h"
#include "library/tags.h"
#include "net/update.h"
#include "net/packfetch.h"
#include "library/scripts.h"
#include "net/podsync.h"
#include "net/coverfind.h"
#include "net/lyricfind.h"
#include "net/radioicon.h"
#include "audio/crossfeed.h"
#include "audio/width.h"
#include "audio/decoder.h"
#include "audio/ff_util.h"
#include "audio/eq.h"
#include "audio/loudness.h"
#include "lvgl.h"
#include "player/player.h"
#include "ui/assets.h"
#include "ui/fonts.h"
#include "ui/ui.h"

/* ---- shared state (defined in main.c) ---- */
extern library_t *g_lib;
extern player_t *g_player;
extern uac_relay_t *g_relay;            /* USB DAC mode is on */
extern int g_charging;                  /* on power: plugged in */
extern int g_screen_on;
extern int g_usb_drive;                 /* exiting for USB drive mode */
extern volatile int g_scan_state;       /* the background scan: 0 idle, 1 running, 2 finished */
extern char g_db_path[512], g_art_path[512];

extern char g_data_dir[512];             /* DATA (--data) */
extern volatile sig_atomic_t g_quit;     /* asked to stop */
extern int g_quit_pipe[2];
extern volatile sig_atomic_t g_shot;     /* a screenshot was asked for (SIGUSR1) */

extern const char *g_music;              /* the music folder (--music) */

extern int g_powering_off;               /* its message stays on the screen */

/* ---- main.c ---- */
extern uint32_t g_last_input;            /* when a key was last pressed (clock_ms32) */
void screen_set(int on);
void screen_activity(void);              /* a key: the screen's timeout starts again */
int usb_powered(void);                   /* USB power is on the port */
void queue_save(void);
#define LOW_BATTERY_UV 3450000
void screen_shot(void);
void mark_clean_stop(void);
int net_up(void);                        /* the network can be used */
const char *net_why(void);               /* why it can't, for the screen */

/* ---- output.c: where the sound goes and how it is shaped ---- */
#define TRANSPORT_TIMEOUT_S 10           /* Transport mode: the screen off this long after the last key */
extern engine_t *g_eng;                  /* NULL unless the output is ALSA (switchable) */
extern int g_jack_lo;                    /* a line-out plug is in */
extern int g_usb_card;                   /* the card's number while a USB DAC is the output; -1 */
extern int g_transport;                  /* Transport mode is on now */
extern char g_bt_routed[200];            /* the Bluetooth PCM playback went to */
int on_bt_now(void);                     /* the sound goes to Bluetooth headphones */
void volume_init(const char *data);
void volume_step(int dir);
void jacks_changed(int hp, int lo);
void route_apply(int announce);
void output_home(void);
void usb_dac_poll(lv_timer_t *t);
void bt_ui_toggle_or_do(int what, const char *addr);
void bt_poll(lv_timer_t *t);
void now_publish(lv_timer_t *t);          /* what is playing, for the headphones' buttons and screen (fathom-avrcp) */
void eq_apply(void);
void eq_poll(lv_timer_t *t);
void levelling_apply(void);
void hook_transport_toggle(void);
void hook_sound_value(int what, char *out, size_t cap);
int hook_sound_adjust(int what, int dir);
void hook_pure_toggle(void);
int hook_signal_path(int i, char *label, size_t lc, char *value, size_t vc);
int hook_eq_list(char names[][64], int max);
void hook_eq_set(const char *name);
int hook_eq_get(const char *name, eq_profile_t *p);
int hook_eq_put(const char *name, const eq_profile_t *p);
void hook_eq_preview(const eq_profile_t *p);
int hook_eq_remove(const char *name);

/* ---- usbmode.c: a computer on the USB port ---- */
#define USB_DRIVE_EXIT 42                /* the player's exit code that asks for USB drive mode (S90fathom) */
enum { HID_PLAY = 1, HID_NEXT = 2, HID_PREV = 4 };   /* the computer's media keys, in USB DAC mode */
extern int g_usb_was;                    /* connected to a computer at the last poll */
int usb_on_computer(void);
void usb_poll(lv_timer_t *t);
void usb_drive(void);
void restart_player(void);
void usb_dac_mode(void);
void usb_dac_mode_end(const char *why);
void usb_dac_mode_poll(lv_timer_t *t);
void usb_dac_key(int key);
int take_tmp_file(const char *path, char *out, size_t cap);
const char *usbdisk_text(const char *msg);

/* ---- updates.c: updates and Uninstall Fathom, the player's side ---- */
#define UPDATE_STAGE CARD_DIR "/fathom/update"
void hook_update_do(int what);
void update_poll(lv_timer_t *t);
void update_announce(void);
void auto_update_poll(void);
void boot_started(void);
void boot_markers(void);
void card_writable_check(void);
void hook_uninstall_do(int what);
void uninstall_poll(void);

/* ---- radiohooks.c: internet radio as the interface asks for it ---- */
int hook_radio_stations(radio_station_t *out, int max);
int hook_radio_add(const radio_station_t *st);
int hook_radio_remove(const char *url);
int hook_radio_ready(char *why, size_t cap);
int hook_podcast_opml(char *path, size_t cap);
void radio_poll(lv_timer_t *t);
void radio_net_feed(void);

/* ---- clockset.c: the clock ---- */
void clock_restore(void);                /* at a start: not behind the last time known */
void clock_save(void);
int clock_from_https(void);              /* (netclock's fetcher) */
void before_net(void);                   /* (before every connection, on its thread) */
int clock_set_by_hand(int64_t t);
void clock_poll(void);

/* ---- power.c: the battery, powering off, charging mode ---- */
#define STARTED_MARK "/tmp/fathom-started"
extern batlog_t g_batlog;                /* Settings > Power > Battery's history */
extern int g_batlog_on;
extern int g_bat_pct;                    /* the charge, percent; -1 not read yet */
extern int g_charge_mode;                /* powered on by the charger, not the Power key */
void poll_battery(lv_timer_t *t);
void battery_log_poll(void);
int hook_battery_report(batrep_t *rep, int days);
void hook_battery_info(char *out, size_t cap);
void save_state(void);                   /* what a stop or a restart leaves on the card */
void restart_now(void);
void power_off(const char *why);
void power_on_reason(char *out, size_t cap);
void started_mark(void);
void charge_screen_show(void);
void charge_mode_end(void);
void charge_poll(lv_timer_t *t);

/* ---- packs.c: the character packs (Settings > Interface > Language packs) ---- */
extern char g_packs_dir[600];
extern volatile int g_pack_songs_known;  /* the library's names were counted once at least */
void pack_songs_count(library_t *lib);   /* (a scan's end: on its thread) */
void pack_songs_count_alone(void);       /* (a start with no scan) */
void pack_songs_count_wait(void);
void pack_songs_load(void);
int hook_packs_state(ui_pack_t *out, int cap);
void hook_packs_do(int i, int what);
void packs_poll(void);

/* ---- bgscan.c: background scans, and the card's free space ---- */
extern char g_pl_path[600];              /* the playlists' folder */
extern int g_scan_check;                 /* the next scan checks the database first (the last stop wasn't clean) */
void fast_walk_setup(library_t *lib, const char *music);
void scan_start(void);
void scan_stop(void);
void scan_poll(lv_timer_t *t);
void rescan(void);
int hook_scan_progress(ui_scan_t *p);
void free_space_warm(void);
void card_full_check(void);

/* ---- early.c: the screens before the player proper ---- */
#define UPGRADE_SCREEN_SONGS 5000        /* a library this big gets a screen while it is brought up to date */
typedef struct { lv_obj_t *scr, *prev, *step, *left; uint32_t t0, estimate_ms, left_at; } upgrade_ui_t;
int scan_with_progress(library_t *lib, const char *music, const theme_t *t, const ui_fonts_t *f);
void upgrade_screen_open(upgrade_ui_t *u, int64_t songs, const theme_t *t, const ui_fonts_t *f);
void upgrade_screen_close(upgrade_ui_t *u, library_t *lib, int64_t songs);
void card_unusable_screen(const theme_t *t, const ui_fonts_t *f);

/* ---- levelling.c: loudness measured while charging and idle ---- */
void rg_poll(lv_timer_t *t);
void rg_stop(void);
int player_reader_do(int op, const char *path, unsigned char **out, size_t *len);

#endif
