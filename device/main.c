/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/*
 * fathom-player: the Fathom player on the HiFi Walker H2.
 *
 *   fathom-player [--music DIR] [--data DIR] [--assets DIR] [--scan] [--theme N]
 *
 * Defaults match the SD card layout: music in /mnt/sd/Music (only ever read),
 * database and cover cache in /mnt/sd/fathom, themes and fonts in
 * /mnt/sd/fathom/assets. The library is scanned on first start, or with --scan.
 *
 * Display: DRM/KMS (disp_drm.c). Keys: gpio-keys and the wheel (input.c).
 * Audio: ALSA (out_alsa.c), bit-perfect; volume is the DAC's own (mixer_alsa.c),
 * saved in DATA/volume. Without a sound card, a silent real-time clock.
 * Output: headphones when plugged in, otherwise line out at a fixed 0 dB once
 * a line-out device is detected (muted until then); see route_apply(). A USB
 * DAC takes over while it is plugged in (usb_dac_poll()), at its own volume.
 */
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
#include "radiometa.h"
#include "out_router.h"
#include "audio/out_bt.h"
#include "audio/output.h"
#include "disp_drm.h"
#include "hw_paths.h"
#define DISP(f) disp_drm_##f
#define DISP_DEV HW_DRM
#define CARD_DIR "/mnt/sd"
#define USB_ONLINE HW_USB_ONLINE
#include "input.h"
#include "library/library.h"
#include "library/tags.h"
#include "net/update.h"
#include "net/podsync.h"
#include "net/coverfind.h"
#include "audio/crossfeed.h"
#include "audio/decoder.h"
#include "audio/ff_util.h"
#include "audio/eq.h"
#include "audio/loudness.h"
#include "lvgl.h"
#include "player/player.h"
#include "ui/assets.h"
#include "ui/ui.h"

static volatile sig_atomic_t g_quit;
static void on_signal(int s) { (void)s; g_quit = 1; }

/* The boot logo as the player's last frame: on a crash too, so the screen shows
 * the logo (not a frozen or black screen) until S90fathom has it running again;
 * the display driver never sends the console's all-black frames. */
static uint16_t *g_logo565;
static int g_powering_off;    /* its message stays on screen instead */

static void on_fatal(int sig)
{
    DISP(show_last_frame)();
    signal(sig, SIG_DFL);
    raise(sig);
}

static void keep_logo_for_exit(const lv_image_dsc_t *logo)
{
    if (!logo || logo->header.w != UI_W || logo->header.h != UI_H) return;
    g_logo565 = malloc((size_t)UI_W * UI_H * 2);
    if (!g_logo565) return;
    for (int i = 0; i < UI_W * UI_H; i++) {   /* ARGB8888 in memory: B, G, R, A */
        const uint8_t *p = logo->data + 4 * i;
        g_logo565[i] = (uint16_t)((p[2] >> 3) << 11 | (p[1] >> 2) << 5 | p[0] >> 3);
    }
    DISP(set_last_frame)(g_logo565);
    static const int fatal[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};
    for (size_t i = 0; i < sizeof fatal / sizeof fatal[0]; i++) signal(fatal[i], on_fatal);
}

/* The themes (the card's fathom/themes after the built-in ones), and the theme editor's hooks */
static theme_t g_themes[UI_MAX_THEMES];
static char g_user_themes[600], g_assets_dir[600];
static int hook_theme_save(const theme_t *t, const char *file, char *saved, size_t cap)
{
    int r = ui_theme_write(g_user_themes, t, file, saved, cap);
    sync();
    return r;
}
static int hook_themes_reload(void) { return ui_load_themes(g_assets_dir, g_user_themes, g_themes, UI_MAX_THEMES); }
static int hook_theme_remove(const char *file)
{
    int r = ui_theme_delete(g_user_themes, file);
    sync();
    return r;
}

/* kill -USR1 $(pidof fathom-player): the screen as it is now, to /tmp/fathom-screen.bmp
 * (with what shows over it: messages, help, the level bar) */
static volatile sig_atomic_t g_shot, g_usb_req;
static void on_usr1(int s) { (void)s; g_shot = 1; }
/* kill -USR2 $(pidof fathom-player): USB drive mode, as if chosen (development, tests) */
static void on_usr2(int s) { (void)s; g_usb_req = 1; }

static void screen_shot(void)
{
    /* a full-screen panel on the top layer (charging, USB drive) covers the screen */
    lv_obj_t *what = lv_screen_active(), *top = lv_layer_top();
    for (int i = (int)lv_obj_get_child_count(top) - 1; i >= 0; i--) {
        lv_obj_t *c = lv_obj_get_child(top, i);
        if (lv_obj_get_width(c) >= LV_HOR_RES && lv_obj_get_height(c) >= LV_VER_RES) { what = c; break; }
    }
    lv_draw_buf_t *img = lv_snapshot_take(what, LV_COLOR_FORMAT_RGB888);
    /* the top layer's other things shown, drawn over it (blended: round corners) */
    for (uint32_t i = 0; img && what == lv_screen_active() && i < lv_obj_get_child_count(top); i++) {
        lv_obj_t *c = lv_obj_get_child(top, (int32_t)i);
        if (lv_obj_has_flag(c, LV_OBJ_FLAG_HIDDEN)) continue;
        lv_draw_buf_t *o = lv_snapshot_take(c, LV_COLOR_FORMAT_ARGB8888);
        if (!o) continue;
        lv_area_t a;
        lv_obj_get_coords(c, &a);
        /* (the snapshot is the object with its shadow's margin all round) */
        int32_t ext = ((int32_t)o->header.w - lv_area_get_width(&a)) / 2, x0 = a.x1 - ext, y0 = a.y1 - ext;
        for (int32_t y = 0; y < (int32_t)o->header.h; y++) {
            if (y0 + y < 0 || y0 + y >= (int32_t)img->header.h) continue;
            const uint8_t *s = o->data + (size_t)y * o->header.stride;
            uint8_t *d = img->data + (size_t)(y0 + y) * img->header.stride;
            for (int32_t x = 0; x < (int32_t)o->header.w; x++) {
                if (x0 + x < 0 || x0 + x >= (int32_t)img->header.w) continue;
                const uint8_t *p = s + 4 * x;            /* B, G, R, A */
                uint8_t *q = d + 3 * (x0 + x);
                for (int k = 0; k < 3; k++) q[k] = (uint8_t)((p[k] * p[3] + q[k] * (255 - p[3])) / 255);
            }
        }
        lv_draw_buf_destroy(o);
    }
    FILE *f = img ? fopen("/tmp/fathom-screen.bmp", "wbe") : NULL;
    if (f) {
        int w = img->header.w, h = img->header.h, row = (w * 3 + 3) & ~3;
        uint32_t v[] = {54u + (uint32_t)row * h, 0, 54, 40, (uint32_t)w, (uint32_t)h};
        uint8_t hdr[54] = {'B', 'M'};
        for (int i = 0; i < 6; i++)
            for (int b = 0; b < 4; b++) hdr[2 + 4 * i + b] = (uint8_t)(v[i] >> (8 * b));
        hdr[26] = 1; hdr[28] = 24;
        fwrite(hdr, 1, sizeof hdr, f);
        static const uint8_t pad[3];
        for (int y = h - 1; y >= 0; y--) {   /* LVGL's RGB888 is B,G,R, as BMP wants */
            fwrite(img->data + (size_t)y * img->header.stride, 1, (size_t)w * 3, f);
            fwrite(pad, 1, (size_t)(row - w * 3), f);
        }
        fclose(f);
    }
    if (img) lv_draw_buf_destroy(img);
}

static uint32_t ms_now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)(t.tv_sec * 1000 + t.tv_nsec / 1000000);
}

static void every_second(lv_timer_t *t) { (void)t; ui_tick_1s(); }

/* Battery level and charging state from the AXP2101 driver (power_supply class). */
static int read_int_file(const char *path, char *buf, size_t cap)
{
    FILE *f = fopen(path, "re");
    if (!f) return -1;
    int ok = fgets(buf, (int)cap, f) != NULL;
    fclose(f);
    return ok ? 0 : -1;
}

/* Clean power off: init stops the player (S90fathom), unmounts the card, and the
 * battery driver sets the PMIC's power-off bit at the end. */
static void screen_set(int on);
static library_t *g_lib;
static void queue_save(void);

/* ---- a clock that never goes back ----
 * The H2 has no running real-time clock: every boot starts in 1970. History
 * ("recently played") and the shuffle's freshness only need time to move
 * forward, so the clock is saved now and then and, at start-up, set to the
 * saved value if it is behind. Time spent switched off isn't counted. */

static void clock_restore(void)
{
    long saved = settings_get_int("clock.last", 0);
    if (saved > 0 && time(NULL) < saved) {
        struct timeval tv = {saved + 1, 0};
        if (settimeofday(&tv, NULL)) perror("settimeofday");
    }
}

static void clock_save(void) { settings_set_int("clock.last", (int)time(NULL)); }

/* ---- battery history (core/batlog.c): Settings > Power > Battery ----
 * fathom/battery.log on the card: the charge and the state, noted every 10 s
 * (battery_log_poll), written every 10 minutes, at charger changes and when
 * the player stops. */
static batlog_t g_batlog;
static int g_batlog_on;

static int64_t mono_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec;
}

static void batlog_end(void) { if (g_batlog_on) batlog_off(&g_batlog, mono_s()); }   /* switching off, restarting */

static int hook_battery_report(batrep_t *rep, int days)
{
    if (!g_batlog_on) return -1;
    batrec_t *r = NULL;
    int n = batlog_load(g_batlog.path, &g_batlog, mono_s(), &r);
    if (n > 0) batlog_report(r, n, days, rep);
    free(r);
    return n > 0 ? 0 : -1;
}


static void power_off(const char *why)
{
    if (g_powering_off) return;        /* (charging mode asks every second until init stops us) */
    fprintf(stderr, "power off: %s\n", why);
    g_powering_off = 1;
    batlog_end();
    clock_save();
    settings_save();
    queue_save();
    screen_set(1);                     /* the message must be visible */
    ui_toast(why, 5000);
    lv_refr_now(NULL);
    if (system("/sbin/poweroff") != 0) {
        fprintf(stderr, "poweroff failed\n");
        g_powering_off = 0;            /* (asked again later, as before) */
    }
}

/* The stock firmware cuts power at 3.40 V. Power off a little above that, on
 * three readings in a row, so the player shuts down cleanly first. */
#define LOW_BATTERY_UV 3450000

static int g_charging;       /* on power: plugged in (poll_battery) */

/* ---- battery: time left and charge cycles ----
 * The PMIC (AXP2101) gives the charge in percent and the voltage, not the
 * current. Time left is the rate the percentage has fallen at over the last
 * 90 minutes on battery; charge cycles are the percentage used up, counted by
 * Fathom ("battery.used": 100 = one cycle). The battery's health (its capacity
 * now) can't be measured without the current. */
#define BAT_HIST 32
static struct { uint32_t s; int pct; } g_bat_hist[BAT_HIST];   /* when each percentage was first seen */
static int g_bat_n, g_bat_last = -1, g_bat_pct = -1;

static void battery_track(int pct, int on_power)
{
    g_bat_pct = pct;
    if (on_power) { g_bat_n = 0; g_bat_last = -1; return; }
    if (g_bat_last >= 0 && pct < g_bat_last)
        settings_set_int("battery.used", settings_get_int("battery.used", 0) + (g_bat_last - pct));
    g_bat_last = pct;
    if (g_bat_n && g_bat_hist[(g_bat_n - 1) % BAT_HIST].pct == pct) return;
    g_bat_hist[g_bat_n % BAT_HIST].s = ms_now() / 1000;
    g_bat_hist[g_bat_n % BAT_HIST].pct = pct;
    g_bat_n++;
}

/* Seconds left on battery, or -1 while there's too little to go on. */
static long battery_left_s(void)
{
    if (g_bat_n < 3) return -1;                       /* two drops at least (the first sample is mid-step) */
    uint32_t now = ms_now() / 1000;
    int last = (g_bat_n - 1) % BAT_HIST, first = -1;
    for (int k = g_bat_n - 1; k >= 1 && k > g_bat_n - BAT_HIST; k--) {   /* the oldest drop within 90 min */
        int i = k % BAT_HIST;
        if (now - g_bat_hist[i].s > 90 * 60) break;
        first = i;
    }
    if (first < 0 || first == last) return -1;
    int drop = g_bat_hist[first].pct - g_bat_hist[last].pct;
    uint32_t dt = g_bat_hist[last].s - g_bat_hist[first].s;
    if (drop < 2 || dt < 600) return -1;
    return (long)((double)g_bat_pct * dt / drop);
}

static void hook_battery_info(char *out, size_t cap)
{
    char pct[16], n[16];
    if (g_bat_pct < 0) { snprintf(out, cap, "-"); return; }
    snprintf(pct, sizeof pct, "%d %%", g_bat_pct);
    if (g_charging) { tr_fill(out, cap, g_bat_pct >= 100 ? "%s, full" : "%s, charging", pct, NULL); return; }
    long s = battery_left_s();
    if (s < 0) { snprintf(out, cap, "%s", pct); return; }
    snprintf(n, sizeof n, "%ld", s >= 2 * 3600 ? (s + 1800) / 3600 : (s + 300) / 600 * 10 > 10 ? (s + 300) / 600 * 10 : 10);
    tr_fill(out, cap, s >= 2 * 3600 ? "%s, about %s h left" : "%s, about %s min left", pct, n);
}

static void poll_battery(lv_timer_t *t)
{
    (void)t;
    static int low_count;
    char cap[16], status[32], volt[24];
    if (read_int_file(HW_BATTERY "capacity", cap, sizeof cap)) { ui_set_battery(-1, 0); return; }
    int charging = !read_int_file(HW_BATTERY "status", status, sizeof status) &&
                   !strncmp(status, "Charging", 8);
    int pct = atoi(cap);
    ui_set_battery(pct, charging);
    /* on power: charging, or full (or held) while plugged in */
    g_charging = !read_int_file(HW_BATTERY "status", status, sizeof status) &&
                 strncmp(status, "Discharging", 11) != 0;
    battery_track(pct, g_charging);

    /* low-battery warnings, once each per discharge */
    static int warned10, warned5;
    if (charging) warned10 = warned5 = 0;
    else if (pct <= 5 && !warned5) { warned5 = warned10 = 1; ui_toast("Battery at 5 %: charge soon", 4000); }
    else if (pct <= 10 && !warned10) { warned10 = 1; ui_toast("Battery at 10 %", 3000); }

    long uv = read_int_file(HW_BATTERY "voltage_now", volt, sizeof volt) ? 0 : atol(volt);
    if (!charging && uv > 0 && uv < LOW_BATTERY_UV) {
        if (++low_count == 3) power_off("Battery empty: powering off");
    } else {
        low_count = 0;
    }
}
/* The player followed 10 times a second while it has something to follow
 * (playing, starting, the sleep timer, a stream reconnecting); otherwise once a
 * second, until a key (fast_tick_now()). Paused, that's 9 wakeups a second fewer. */
static lv_timer_t *g_fast;
static uint32_t g_fast_ms = 100;
static void every_100ms(lv_timer_t *t)
{
    uint32_t ms = ui_tick() ? 100 : 1000;
    if (ms != g_fast_ms) lv_timer_set_period(t, g_fast_ms = ms);
}
static void fast_tick_now(void)
{
    if (g_fast && g_fast_ms != 100) {
        lv_timer_set_period(g_fast, g_fast_ms = 100);
        lv_timer_ready(g_fast);
    }
}

/* ---- volume and output routing ----
 *
 * Volume is the DAC's hardware attenuator (0.5 dB steps), 1 dB per key press,
 * on headphones only. Line out is fixed at 0 dB, the standard line level, and
 * only once the line-out detect reports a device: the detect works only while
 * that path is selected and the amplifier is on, so with no headphones the
 * switch goes to line out but the DAC stays muted until something answers.
 */

static mixer_t g_mix = {.fd = -1};
static char g_vol_path[512];
static int g_user_vol = -1;            /* headphone volume (raw mixer value) */
static int g_have_route;
static audio_route_t g_route = ROUTE_HEADPHONES;
static int g_jack_hp, g_jack_lo;
static player_t *g_player;

static int g_usb_card = -1;             /* card number while a USB DAC is the output */

static int vol_limit(void);
static int hp_cap(void);
static int bt_limit_pct(void);
static int bt_cap(void);

static void volume_show(int raw)
{
    char t[32];
    int tenths = (raw - g_mix.max) * 5;                 /* 0.5 dB per step below max */
    if (raw <= g_mix.min) snprintf(t, sizeof t, "%s", tr("muted"));
    else snprintf(t, sizeof t, "%s%d.%d dB%s", tenths < 0 ? "-" : "", abs(tenths) / 10, abs(tenths) % 10,
                  vol_limit() && raw >= hp_cap() ? tr(" (limit)") : "");
    ui_level("Volume", raw - g_mix.min, g_mix.max - g_mix.min, t);
}

/* Route by the jacks and set the level for that route; pop-free order. */
static void route_apply(int announce)
{
    if (g_mix.fd < 0) return;
    if (g_usb_card >= 0 || out_router_on_bt()) { mixer_set(&g_mix, g_mix.min); return; }   /* a USB DAC or Bluetooth plays */
    audio_route_t want = g_jack_hp || !g_have_route ? ROUTE_HEADPHONES : ROUTE_LINE_OUT;
    if (want != g_route) {
        mixer_set(&g_mix, g_mix.min);                   /* mute while switching */
        audio_route_set(want);
        g_route = want;
        struct timespec ts = {0, 20 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
    if (g_route == ROUTE_HEADPHONES) mixer_set(&g_mix, g_user_vol > hp_cap() ? hp_cap() : g_user_vol);
    else mixer_set(&g_mix, g_jack_lo ? g_mix.max : g_mix.min);
    ui_set_output(g_route == ROUTE_HEADPHONES ? "Headphones" : "Line out");
    if (announce)
        ui_toast(g_route == ROUTE_HEADPHONES ? "Headphones" : "Line out", 1200);
}

static void volume_init(const char *data)
{
    snprintf(g_vol_path, sizeof g_vol_path, "%s/volume", data);
    if (mixer_open(&g_mix, NULL, "Master Playback Volume")) { fprintf(stderr, "volume: no mixer control\n"); return; }
    g_user_vol = mixer_get(&g_mix);                     /* the driver's safe default */
    FILE *f = fopen(g_vol_path, "re");
    int v;
    if (f && fscanf(f, "%d", &v) == 1 && v >= g_mix.min && v <= g_mix.max) g_user_vol = v;
    if (f) fclose(f);
    input_jacks(&g_jack_hp, &g_jack_lo);
    g_have_route = audio_route_open(g_jack_hp ? ROUTE_HEADPHONES : ROUTE_LINE_OUT) == 0;
    if (!g_have_route) fprintf(stderr, "audio route: no PB5, headphones only\n");
    g_route = g_jack_hp || !g_have_route ? ROUTE_HEADPHONES : ROUTE_LINE_OUT;
    if (g_route == ROUTE_LINE_OUT) mixer_set(&g_mix, g_mix.min);
    route_apply(0);
}

static void usb_volume_step(int dir);

/* The headphones' top level under the volume limit (the mixer: 0.5 dB steps). */
static int hp_cap(void)
{
    int c = g_mix.max + vol_limit() * 2;
    return c < g_mix.min ? g_mix.min : c;
}

static void volume_step(int dir)
{
    if (out_router_on_bt()) {               /* the headphones' own volume (AVRCP), or bluez-alsa's */
        int v = bt_volume_step(dir);
        char t[48];
        snprintf(t, sizeof t, "%d %%%s", (v * 100 + 63) / 127, bt_limit_pct() < 100 && v >= bt_cap() ? tr(" (limit)") : "");
        ui_level("Volume", v, 127, t);
        return;
    }
    if (g_usb_card >= 0) { usb_volume_step(dir); return; }
    if (g_mix.fd < 0) { ui_toast("No volume control", 1200); return; }
    if (g_route == ROUTE_LINE_OUT) { ui_toast("Line out: fixed at 0 dB", 1500); return; }
    int cap = hp_cap(), cur = g_user_vol > cap ? cap : g_user_vol;
    int want = cur + dir * 2;
    int v = mixer_set(&g_mix, want > cap ? cap : want);
    if (v < 0) return;
    g_user_vol = v;
    volume_show(v);
    FILE *f = fopen(g_vol_path, "we");
    if (f) { fprintf(f, "%d\n", v); fclose(f); }
}

static void jacks_changed(int hp, int lo)
{
    int hp_changed = hp != g_jack_hp;
    if (g_jack_hp && !hp && g_usb_card < 0 && !out_router_on_bt() && g_player &&
        settings_get_int("playback.pause_on_unplug", UI_PAUSE_ON_UNPLUG_DEFAULT))
        player_pause(g_player, 1);   /* headphones out: pause */
    g_jack_hp = hp;
    g_jack_lo = lo;
    route_apply(hp_changed);    /* line-out detect flips with the amp; only announce plugging */
}


/* ---- USB DACs ----
 *
 * While a USB audio card is present, playback goes to it and the H2's own DAC
 * stays muted. Volume is the DAC's own control (bit-perfect), 1 dB per key
 * press, remembered per DAC (DATA/volume-usb-VVVV-PPPP, in 1/100 dB). A DAC
 * seen for the first time starts at -40 dB. A DAC with no volume control gets
 * the engine's gain instead (no longer bit-perfect, but never full scale by
 * surprise). Unplugging it pauses playback and goes back to the H2's output.
 */

#define USB_FIRST_CDB (-4000L)

static engine_t *g_eng;                /* NULL unless the output is ALSA (switchable) */
static mixer_t g_usb_mix = {.fd = -1};
static char g_data_dir[512], g_usb_vol_path[600], g_usb_name[80], g_usb_dev[32];
static long g_usb_cdb, g_usb_lo, g_usb_hi;
static int g_usb_soft;                 /* no volume control: engine gain */

/* ---- translations: fathom/lang/NAME.txt (core/lang.h) ---- */
static int hook_languages(char names[][64], int max)
{
    char dir[600];
    snprintf(dir, sizeof dir, "%s/lang", g_data_dir);
    return lang_list(dir, names, max);
}

static void hook_language_set(const char *name)
{
    char path[700];
    snprintf(path, sizeof path, "%s/lang/%s.txt", g_data_dir, name ? name : "");
    if (lang_load(name && name[0] ? path : NULL)) fprintf(stderr, "lang: can't read %s\n", path);
    else fprintf(stderr, "lang: %s, %d texts\n", name && name[0] ? name : "English", lang_count());
}


/* The first USB audio card with a playback device: its number, and VID:PID. */
static int usb_card_find(char *usbid, size_t cap)
{
    for (int n = 1; n < 8; n++) {
        char path[64], pcm[32];
        snprintf(path, sizeof path, HW_USBID_N, n);
        snprintf(pcm, sizeof pcm, HW_PCM_N, n);
        if (access(pcm, W_OK) || read_int_file(path, usbid, cap)) continue;
        usbid[strcspn(usbid, "\r\n")] = 0;
        return n;
    }
    return -1;
}

static void usb_card_name(int n, char *out, size_t cap)
{
    char ctl[32];
    snprintf(ctl, sizeof ctl, HW_CTL_N, n);
    snprintf(out, cap, "USB DAC");
    int fd = open(ctl, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;
    struct snd_ctl_card_info ci;
    memset(&ci, 0, sizeof ci);
    if (ioctl(fd, SNDRV_CTL_IOCTL_CARD_INFO, &ci) == 0 && ci.name[0]) snprintf(out, cap, "%s", (const char *)ci.name);
    close(fd);
}

static void usb_volume_apply(void)
{
    if (g_usb_soft) engine_set_gain(g_eng, (float)pow(10.0, g_usb_cdb / 2000.0));
    else mixer_set(&g_usb_mix, mixer_value_for_db(&g_usb_mix, g_usb_cdb));
}

/* A USB DAC's top level under the volume limit, in 1/100 dB. */
static long usb_cap(void)
{
    long c = g_usb_hi + vol_limit() * 100L;
    return c < g_usb_lo ? g_usb_lo : c;
}

static void usb_volume_step(int dir)
{
    long v = g_usb_cdb + dir * 100L, cap = usb_cap();
    g_usb_cdb = v < g_usb_lo ? g_usb_lo : v > cap ? cap : v;
    usb_volume_apply();
    char t[64];
    long a = labs(g_usb_cdb);
    snprintf(t, sizeof t, "%s%ld.%ld dB%s", g_usb_cdb < 0 ? "-" : "", a / 100, (a % 100) / 10,
             vol_limit() && g_usb_cdb >= cap ? tr(" (limit)") : "");
    ui_level("Volume", (int)(g_usb_cdb - g_usb_lo), (int)(g_usb_hi - g_usb_lo), t);
    FILE *f = fopen(g_usb_vol_path, "we");
    if (f) { fprintf(f, "%ld\n", g_usb_cdb); fclose(f); }
}

/* ---- Transport mode ----
 * With a USB DAC as the output and Settings > Sound > Transport mode on, the
 * H2 only feeds the DAC: Bluetooth stops (and starts again when the DAC goes,
 * if it was on) and the screen turns off 10 s after the last key. (The CPU
 * stays at its one speed: the kernel has no frequency scaling for the X1000.) */
#define TRANSPORT_TIMEOUT_S 10
static int g_transport;              /* on now */
static int g_transport_bt;           /* Bluetooth was on: on again after */

static void transport_update(void)
{
    int want = g_usb_card >= 0 && settings_get_int("sound.transport", UI_TRANSPORT_DEFAULT);
    if (want == g_transport) return;
    g_transport = want;
    if (want) {
        g_transport_bt = bt_available() && bt_on();
        if (g_transport_bt) bt_set(0);
        fprintf(stderr, "transport mode: on%s\n", g_transport_bt ? ", Bluetooth off" : "");
    } else {
        if (g_transport_bt) bt_set(1);
        g_transport_bt = 0;
        fprintf(stderr, "transport mode: off\n");
    }
}

static void hook_transport_toggle(void)
{
    int on = !settings_get_int("sound.transport", UI_TRANSPORT_DEFAULT);
    settings_set_int("sound.transport", on);
    transport_update();
    if (on) ui_toast(g_transport ? "Transport mode: Bluetooth off, screen off after 10 s"
                                 : "With a USB DAC: Bluetooth off, screen off after 10 s", 2500);
}

static void usb_dac_attach(int n, const char *usbid)
{
    char vid[8] = "", pid[8] = "";
    sscanf(usbid, "%7[0-9a-fA-F]:%7[0-9a-fA-F]", vid, pid);
    usb_card_name(n, g_usb_name, sizeof g_usb_name);
    snprintf(g_usb_vol_path, sizeof g_usb_vol_path, "%s/volume-usb-%s-%s", g_data_dir, vid, pid);

    char ctl[32], name[44];
    snprintf(ctl, sizeof ctl, HW_CTL_N, n);
    g_usb_soft = mixer_find_playback_volume(ctl, name) || mixer_open(&g_usb_mix, ctl, name) ||
                 mixer_db_range(&g_usb_mix, &g_usb_lo, &g_usb_hi);
    if (g_usb_soft) { mixer_close(&g_usb_mix); g_usb_lo = -6000; g_usb_hi = 0; }
    g_usb_cdb = USB_FIRST_CDB;
    FILE *f = fopen(g_usb_vol_path, "re");
    long v;
    if (f && fscanf(f, "%ld", &v) == 1) g_usb_cdb = v;
    if (f) fclose(f);
    if (g_usb_cdb < g_usb_lo) g_usb_cdb = g_usb_lo;
    if (g_usb_cdb > g_usb_hi) g_usb_cdb = g_usb_hi;

    g_usb_card = n;
    if (g_usb_cdb > usb_cap()) g_usb_cdb = usb_cap();   /* (the limit is per DAC: once it's the output) */
    route_apply(0);                    /* mutes the H2's own DAC */
    usb_volume_apply();                /* the DAC's level before any audio reaches it */
    transport_update();
    snprintf(g_usb_dev, sizeof g_usb_dev, HW_PCM_N, n);
    engine_switch_output(g_eng, out_router_alsa, g_usb_dev);
    fprintf(stderr, "usb dac: card %d %s (%s), %s, %ld cdB\n", n, g_usb_name, usbid,
            g_usb_soft ? "software volume" : "hardware volume", g_usb_cdb);
    char t[112];
    snprintf(t, sizeof t, "%s", g_usb_name);
    ui_toast(t, 2500);
    ui_set_output(g_usb_name);
}

static char g_bt_routed[200];        /* the Bluetooth PCM playback went to */

/* Back from a USB DAC or USB DAC mode: to Bluetooth headphones if they're
 * connected, else to the H2's own DAC. */
static void output_home(void)
{
    char pcm[200], name[64];
    if (bt_pcm(pcm, sizeof pcm, name, sizeof name)) {
        engine_switch_output(g_eng, out_router_bt, pcm);
        snprintf(g_bt_routed, sizeof g_bt_routed, "%s", pcm);
        route_apply(0);
        ui_set_output(name);
    } else {
        engine_switch_output(g_eng, out_router_alsa, (void *)HW_PCM);
        route_apply(0);
    }
}

static void usb_dac_detach(void)
{
    fprintf(stderr, "usb dac: %s gone\n", g_usb_name);
    engine_pause(g_eng, 1);
    mixer_close(&g_usb_mix);
    if (g_usb_soft) engine_set_gain(g_eng, 1.0f);
    g_usb_card = -1;
    transport_update();                /* (Bluetooth back first, if it was on) */
    output_home();                     /* the H2's own output at its saved level, or Bluetooth */
    ui_toast("USB DAC unplugged: paused", 2500);
}

static void usb_dac_poll(lv_timer_t *t)
{
    (void)t;
    if (!g_eng) return;
    char usbid[16];
    if (g_usb_card < 0) {
        int n = usb_card_find(usbid, sizeof usbid);
        if (n >= 0) usb_dac_attach(n, usbid);
        return;
    }
    char path[64];
    snprintf(path, sizeof path, HW_USBID_N, g_usb_card);
    eng_status_t st;
    engine_status(g_eng, &st);
    if (access(path, F_OK) || st.output_lost) usb_dac_detach();
}

/* ---- Bluetooth ----
 * bt.c does the work on its own thread; here playback follows it: to the
 * headphones when they connect (unless a USB DAC or USB DAC mode has the
 * audio), paused and back to the H2 when they drop out. */

static void bt_ui_toggle_or_do(int what, const char *addr)
{
    if (what == UI_BT_TOGGLE) settings_set_int("bluetooth.on", !bt_on());
    if (what == UI_BT_RANGE) {                   /* fathom-bt reads it as it starts bluez-alsa */
        settings_set_int("bluetooth.range", !settings_get_int("bluetooth.range", UI_BT_RANGE_DEFAULT));
        settings_save();
    }
    bt_ui_do(what, addr);
}

static uac_relay_t *g_relay;          /* USB DAC mode is on */

/* ---- EQ ----
 * Profiles are AutoEQ / Equalizer APO "ParametricEQ.txt" files in
 * DATA/eq/ (the file name, without ".txt" and " ParametricEQ", is the name
 * shown). The choice is remembered per output: the headphone jack, the line
 * out, each USB DAC (by its USB ID) and each pair of Bluetooth headphones, so
 * the EQ follows the headphones. Off by default: bit-perfect. */
#define EQ_MAX_PROFILES 64
static char g_eq_key[64];               /* the output the EQ was last set for */

/* The output playing now: its settings key and its name. */
static void eq_output(char *key, size_t kc, char *label, size_t lc)
{
    char pcm[200], name[64];
    if (g_usb_card >= 0) {
        const char *id = strstr(g_usb_vol_path, "volume-usb-");   /* volume-usb-VID-PID */
        snprintf(key, kc, "eq.usb.%s", id ? id + 11 : "dac");
        snprintf(label, lc, "%.63s", g_usb_name);
    } else if (out_router_on_bt() && g_bt_routed[0]) {
        const char *d = strstr(g_bt_routed, "/dev_");                  /* .../dev_F4_33_.../a2dpsrc/sink */
        char addr[24] = "";
        if (d) sscanf(d + 5, "%23[0-9A-Fa-f_]", addr);
        snprintf(key, kc, "eq.bt.%s", addr);
        if (bt_pcm(pcm, sizeof pcm, name, sizeof name)) snprintf(label, lc, "%s", name);
        else snprintf(label, lc, "Bluetooth");
    } else {
        snprintf(key, kc, "%s", g_route == ROUTE_HEADPHONES ? "eq.headphones" : "eq.line_out");
        snprintf(label, lc, "%s", g_route == ROUTE_HEADPHONES ? "Headphones" : "Line out");
    }
}

/* The profiles in DATA/eq: their names, sorted. */
static int eq_names(char names[][64], int max)
{
    char dir[600];
    snprintf(dir, sizeof dir, "%s/eq", g_data_dir);
    DIR *d = opendir(dir);
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < max) {
        size_t len = strlen(e->d_name);
        if (e->d_name[0] == '.' || len < 5 || strcasecmp(e->d_name + len - 4, ".txt")) continue;
        size_t keep = len - 4;
        static const char sfx[] = " ParametricEQ";               /* AutoEQ's file names */
        if (keep > sizeof sfx - 1 && !strncasecmp(e->d_name + keep - (sizeof sfx - 1), sfx, sizeof sfx - 1))
            keep -= sizeof sfx - 1;
        snprintf(names[n], 64, "%.*s", (int)(keep > 63 ? 63 : keep), e->d_name);
        n++;
    }
    closedir(d);
    qsort(names, (size_t)n, 64, (int (*)(const void *, const void *))strcasecmp);
    return n;
}

/* A profile's file: AutoEQ's "NAME ParametricEQ.txt" if there is one, else "NAME.txt". */
static void eq_path(const char *name, char *out, size_t cap)
{
    snprintf(out, cap, "%s/eq/%s ParametricEQ.txt", g_data_dir, name);
    if (access(out, R_OK)) snprintf(out, cap, "%s/eq/%s.txt", g_data_dir, name);
}

/* Sets the engine's EQ for the output playing now, from its saved choice. */
static void balance_apply(void);
static void crossfeed_apply(void);
static void limit_apply(void);
static int pure_on(void);
static void levelling_apply(void);
static void pure_report(void);
static void dop_apply(void);

static void eq_apply(void)
{
    char key[64], label[64];
    eq_output(key, sizeof key, label, sizeof label);
    snprintf(g_eq_key, sizeof g_eq_key, "%s", key);
    const char *name = settings_get(key, "");
    eq_profile_t p;
    char path[700], err[128] = "";
    eq_path(name, path, sizeof path);
    int on = name[0] && eq_load(path, &p, err, sizeof err) == 0;
    if (name[0] && !on) {
        char t[200];
        tr_fill(t, sizeof t, "EQ \"%s\": %s", name, err);
        ui_toast(t, 4000);
        fprintf(stderr, "eq: %s: %s\n", path, err);
    }
    int pure = pure_on();
    if (g_eng) engine_set_eq(g_eng, on && !pure ? &p : NULL);
    ui_set_eq(on ? name : "", label);
    fprintf(stderr, "eq: %s: %s%s\n", key, on ? name : "off", pure ? " (Pure mode: bypassed)" : "");
    balance_apply();                     /* (a new output: its balance, crossfeed, limit and Pure mode too) */
    crossfeed_apply();
    limit_apply();
    levelling_apply();
    dop_apply();
    pure_report();
}

/* ---- Balance and volume limit ----
 * Per output, like the EQ: "balance.<output>" and "vollimit.<output>".
 * Balance: -12..+12 dB, below 0 toward the left (the right channel turned
 * down), above 0 toward the right; centred, the samples stay untouched.
 * Volume limit: on the headphone jack and USB DACs, dB below the top of the
 * volume (0 = none, down to -30 in 3 dB steps); on Bluetooth headphones, a
 * percentage of theirs (100 = none). Line out is fixed: no limit. */
#define BALANCE_MAX 12
#define LIMIT_MIN_DB (-30)

static void out_setting_key(const char *what, char *key, size_t cap)
{
    char k[64], label[64];
    eq_output(k, sizeof k, label, sizeof label);
    snprintf(key, cap, "%s.%s", what, k + 3);        /* "eq.headphones" -> "balance.headphones" */
}

static int on_bt_now(void) { return g_usb_card < 0 && out_router_on_bt() && g_bt_routed[0]; }
static int on_line_out_now(void) { return g_usb_card < 0 && !out_router_on_bt() && g_route == ROUTE_LINE_OUT; }

static int balance_db(void)
{
    char key[80];
    out_setting_key("balance", key, sizeof key);
    int b = settings_get_int(key, 0);
    return b < -BALANCE_MAX ? -BALANCE_MAX : b > BALANCE_MAX ? BALANCE_MAX : b;
}

/* Crossfeed, per output like the balance ("crossfeed.<output>": XF_OFF..),
 * off by default; Pure mode bypasses it */
static int crossfeed_now(void)
{
    char key[80];
    out_setting_key("crossfeed", key, sizeof key);
    int l = settings_get_int(key, XF_OFF);
    return l > XF_OFF && l < XF_LEVELS ? l : XF_OFF;
}

static void crossfeed_apply(void)
{
    if (g_eng) engine_set_crossfeed(g_eng, pure_on() ? XF_OFF : crossfeed_now());
}

static void balance_apply(void)
{
    int b = pure_on() ? 0 : balance_db();
    float g = (float)pow(10.0, -abs(b) / 20.0);
    if (g_eng) engine_set_balance(g_eng, b > 0 ? g : 1.0f, b < 0 ? g : 1.0f);
}

static int vol_limit(void)                          /* dB, wired outputs; 0 = none */
{
    if (on_bt_now() || on_line_out_now()) return 0;
    char key[80];
    out_setting_key("vollimit", key, sizeof key);
    int l = settings_get_int(key, 0);
    return l > 0 ? 0 : l < LIMIT_MIN_DB ? LIMIT_MIN_DB : l;
}

static int bt_limit_pct(void)                       /* Bluetooth: percent; 100 = none */
{
    if (!on_bt_now()) return 100;
    char key[80];
    out_setting_key("vollimit", key, sizeof key);
    int l = settings_get_int(key, 100);
    return l < 10 ? 10 : l > 100 ? 100 : l;
}

static int bt_cap(void) { return (bt_limit_pct() * 127 + 50) / 100; }

/* The limit for the output playing now: its volume down to it if it's above. */
static void limit_apply(void)
{
    if (on_bt_now()) {
        bt_volume_limit(bt_cap());
    } else if (g_usb_card >= 0) {
        if (g_usb_cdb > usb_cap()) {
            g_usb_cdb = usb_cap();
            usb_volume_apply();
        }
    } else if (g_mix.fd >= 0 && g_route == ROUTE_HEADPHONES) {
        int cur = mixer_get(&g_mix);
        if (cur > hp_cap()) mixer_set(&g_mix, hp_cap());
    }
}

/* UI: Sound > Balance and Volume limit, for the output playing now. */
static void hook_sound_value(int what, char *out, size_t cap)
{
    if (what == UI_SOUND_CROSSFEED) {
        snprintf(out, cap, "%s", tr(crossfeed_name(crossfeed_now())));
        return;
    }
    if (what == UI_SOUND_BALANCE) {
        int b = balance_db();
        char n[16];
        snprintf(n, sizeof n, "%d", abs(b));
        if (!b) snprintf(out, cap, "%s", tr("Centre"));
        else tr_fill(out, cap, b < 0 ? "%s dB left" : "%s dB right", n, NULL);
    } else if (on_line_out_now()) {
        snprintf(out, cap, "%s", tr("Fixed level"));
    } else if (on_bt_now()) {
        int p = bt_limit_pct();
        if (p >= 100) snprintf(out, cap, "%s", tr("Off"));
        else snprintf(out, cap, "%d %%", p);
    } else {
        int l = vol_limit();
        if (!l) snprintf(out, cap, "%s", tr("Off"));
        else snprintf(out, cap, "%d dB", l);
    }
}

static int hook_sound_adjust(int what, int dir)
{
    char key[80];
    if (!dir) {
        if (what == UI_SOUND_LIMIT && on_line_out_now()) { ui_toast("Line out: fixed at 0 dB", 1500); return -1; }
        return 0;
    }
    if (what == UI_SOUND_CROSSFEED) {        /* Off, Light, Medium, Strong, Off... */
        out_setting_key("crossfeed", key, sizeof key);
        settings_set_int(key, (crossfeed_now() + (dir < 0 ? XF_LEVELS - 1 : 1)) % XF_LEVELS);
        crossfeed_apply();
        return 0;
    }
    if (what == UI_SOUND_BALANCE) {
        int b = balance_db() + dir;
        out_setting_key("balance", key, sizeof key);
        settings_set_int(key, b < -BALANCE_MAX ? -BALANCE_MAX : b > BALANCE_MAX ? BALANCE_MAX : b);
        balance_apply();
        return 0;
    }
    if (on_line_out_now()) { ui_toast("Line out: fixed at 0 dB", 1500); return -1; }
    out_setting_key("vollimit", key, sizeof key);
    if (on_bt_now()) {
        int p = bt_limit_pct() + dir * 10;
        settings_set_int(key, p < 10 ? 10 : p > 100 ? 100 : p);
    } else {
        int l = vol_limit() + dir * 3;
        settings_set_int(key, l < LIMIT_MIN_DB ? LIMIT_MIN_DB : l > 0 ? 0 : l);
    }
    limit_apply();
    return 0;
}

/* DSD as DoP for a USB DAC that takes it: "dop.<output>" = 1 in the settings
 * (settings.ini), and only with the DAC's own volume. Otherwise DSD is made PCM. */
static void dop_apply(void)
{
    char key[80];
    out_setting_key("dop", key, sizeof key);
    if (g_eng) engine_set_dop(g_eng, g_usb_card >= 0 && !g_usb_soft && settings_get_int(key, 0));
}

/* ---- Pure mode and the signal path ----
 * Pure mode ("pure.<output>", per output like the EQ) bypasses the EQ, the
 * balance and the levelling, keeping their choices for when it's off. With it
 * on, Now Playing says "Bit+" while nothing changes the samples between
 * the file and the DAC. Settings > Sound > Output shows each stage. */
static int pure_on(void)
{
    char key[80];
    out_setting_key("pure", key, sizeof key);
    return settings_get_int(key, 0) != 0;
}

static void levelling_apply(void)
{
    if (g_player) player_set_levelling(g_player, pure_on() ? LEVEL_OFF
                                                           : (levelling_t)settings_get_int("sound.levelling", LEVEL_OFF));
}

/* The samples reach the DAC exactly as decoded from a lossless file. */
static int path_bitperfect(const eng_status_t *st)
{
    return st->fmt.rate && st->fmt.bits && !st->stream && st->untouched && !on_bt_now() &&
           st->out_fmt.rate == st->fmt.rate && st->out_fmt.channels == st->fmt.channels &&
           st->out_fmt.bits >= st->fmt.bits;
}

static void pure_report(void)
{
    int pure = pure_on(), bp = 0;
    if (pure && g_eng) {
        eng_status_t st;
        engine_status(g_eng, &st);
        bp = path_bitperfect(&st);
    }
    ui_set_pure(pure, bp);
}

static void hook_pure_toggle(void)
{
    char key[80];
    out_setting_key("pure", key, sizeof key);
    settings_set_int(key, !pure_on());
    eq_apply();                                      /* the EQ, balance and levelling, bypassed or back */
    ui_toast(pure_on() ? "Pure mode: EQ, balance and levelling bypassed" : "Pure mode off", 2000);
}

static void fmt_khz(char *out, size_t cap, unsigned hz)
{
    if (hz % 1000) snprintf(out, cap, "%.1f kHz", hz / 1000.0);
    else snprintf(out, cap, "%u kHz", hz / 1000);
}

static void db_text(char *out, size_t cap, double db)
{
    snprintf(out, cap, "%s%.1f dB", db > 0.05 ? "+" : "", db);
}

/* Settings > Sound > Output: the file, each stage, the output, and the verdict. */
static int hook_signal_path(int i, char *label, size_t lc, char *value, size_t vc)
{
    static const char *const L[] = {"File", "Levelling", "EQ", "Balance", "Volume", "Output", "Sent as", "Result", "Because"};
    if (i < 0 || i > (int)(sizeof L / sizeof L[0]) || !g_eng) return -1;
    if (i == 4) {                                    /* Crossfeed, between Balance and Volume */
        snprintf(label, lc, "%s", tr("Crossfeed"));
        if (pure_on()) snprintf(value, vc, "%s", tr("Bypassed"));
        else hook_sound_value(UI_SOUND_CROSSFEED, value, vc);
        return 0;
    }
    if (i > 4) i--;
    eng_status_t st;
    engine_status(g_eng, &st);
    int pure = pure_on(), playing = st.fmt.rate && st.state != ENG_STOPPED;
    int bp = playing && path_bitperfect(&st);
    char k[16];
    if (i == 8) {                                    /* why not bit-perfect (only then) */
        if (!playing || bp) return -1;
        value[0] = '\0';
        size_t n = 0;
#define WHY(c, s) do { if (c) n += (size_t)snprintf(value + n, n < vc ? vc - n : 0, "%s%s", n ? ", " : "", s); } while (0)
        WHY(on_bt_now(), tr("Bluetooth"));
        WHY(st.eq, tr("EQ"));
        WHY(st.track_gain != 1.0f, tr("levelling"));
        WHY(st.balance[0] != 1.0f || st.balance[1] != 1.0f, tr("balance"));
        WHY(st.crossfeed != XF_OFF, tr("crossfeed"));
        WHY(st.gain != 1.0f, tr("software volume"));
        WHY(st.out_fmt.rate && st.out_fmt.rate != st.fmt.rate, tr("resampled"));
        WHY(!st.fmt.bits && strncmp(st.fmt.codec, "DSD", 3), tr("lossy file"));
        WHY(!st.fmt.bits && !strncmp(st.fmt.codec, "DSD", 3), tr("DSD made PCM"));
        WHY(st.stream, tr("a stream"));
#undef WHY
        if (!value[0]) snprintf(value, vc, "-");
        snprintf(label, lc, "%s", tr(L[i]));
        return 0;
    }
    snprintf(label, lc, "%s", tr(L[i]));   /* (translated here: the values hold names too) */
    switch (i) {
    case 0:
        if (!playing) { snprintf(value, vc, "%s", tr("Nothing playing")); break; }
        fmt_khz(k, sizeof k, st.fmt.rate);
        if (!strncmp(st.fmt.codec, "DSD", 3))       /* the rate is the PCM's (or DoP's) */
            snprintf(value, vc, "%s as %s %s%s", st.fmt.codec, st.fmt.dop ? "DoP" : "PCM", k, st.fmt.channels == 1 ? tr(" mono") : "");
        else if (st.fmt.bits) snprintf(value, vc, "%s %u-bit %s%s", st.fmt.codec, st.fmt.bits, k, st.fmt.channels == 1 ? tr(" mono") : "");
        else snprintf(value, vc, "%s %s%s", st.fmt.codec, k, st.fmt.channels == 1 ? tr(" mono") : "");
        break;
    case 1:
        if (pure) snprintf(value, vc, "%s", tr("Bypassed"));
        else if (settings_get_int("sound.levelling", LEVEL_OFF) == LEVEL_OFF) snprintf(value, vc, "%s", tr("Off"));
        else db_text(value, vc, 20.0 * log10(st.track_gain > 0 ? st.track_gain : 1e-6));
        break;
    case 2: {
        char key[64], lab[64];
        eq_output(key, sizeof key, lab, sizeof lab);
        const char *name = settings_get(key, "");
        if (pure) snprintf(value, vc, "%s", tr("Bypassed"));
        else if (!st.eq || !name[0]) snprintf(value, vc, "%s", tr("Off"));
        else snprintf(value, vc, "%s", name);
        break;
    }
    case 3:
        if (pure) snprintf(value, vc, "%s", tr("Bypassed"));
        else hook_sound_value(UI_SOUND_BALANCE, value, vc);
        break;
    case 4:
        if (on_bt_now()) snprintf(value, vc, "%s", tr("The headphones'"));
        else if (g_usb_card >= 0) {
            char d[24];
            db_text(d, sizeof d, g_usb_cdb / 100.0);
            tr_fill(value, vc, g_usb_soft ? "Software %s" : "In the DAC %s", d, NULL);
        } else if (on_line_out_now()) snprintf(value, vc, "%s", tr("Fixed 0 dB"));
        else {
            char d[24];
            int raw = mixer_get(&g_mix);
            db_text(d, sizeof d, (raw - g_mix.max) * 0.5);
            tr_fill(value, vc, "In the DAC %s", d, NULL);
        }
        break;
    case 5: {
        char pcm[200], name[64];
        if (g_usb_card >= 0) snprintf(value, vc, "%s", g_usb_name);
        else if (on_bt_now() && bt_pcm(pcm, sizeof pcm, name, sizeof name)) snprintf(value, vc, "%s", name);
        else snprintf(value, vc, "ES9018K2M %s", tr(on_line_out_now() ? "line out" : "headphones"));
        break;
    }
    case 6: {                                        /* what the hardware took */
        char d[160] = "", f[16] = "", codec[16] = "";
        unsigned rate = 0;
        float khz = 0;
        engine_describe_output(g_eng, d, sizeof d);
        if (!d[0]) snprintf(value, vc, "-");
        else if (sscanf(d, "Bluetooth %15s %f kHz", codec, &khz) == 2) snprintf(value, vc, "%s 16-bit %.1f kHz", codec, khz);
        else if (sscanf(d, "%*s %15s %u Hz", f, &rate) == 2) {
            int bits = !strcmp(f, "S16_LE") ? 16 : !strcmp(f, "S32_LE") ? 32 : 24;
            fmt_khz(k, sizeof k, rate);
            snprintf(value, vc, "%d-bit %s", bits, k);
        } else snprintf(value, vc, "%s", d);
        break;
    }
    case 7:
        snprintf(value, vc, "%s", !playing ? "-" : tr(bp ? "Bit-perfect" : "Not bit-perfect"));
        break;
    }
    return 0;
}

/* Every 300 ms: the output changed (headphones in, a USB DAC, Bluetooth): its EQ. */
static void eq_poll(lv_timer_t *t)
{
    (void)t;
    char key[64], label[64];
    eq_output(key, sizeof key, label, sizeof label);
    if (strcmp(key, g_eq_key)) eq_apply();
    else pure_report();
}

/* UI: the profiles, and a choice for the output playing now ("" = off). */
static int hook_eq_list(char names[][64], int max) { return eq_names(names, max); }
static void hook_eq_set(const char *name)
{
    char key[64], label[64];
    eq_output(key, sizeof key, label, sizeof label);
    settings_set(key, name ? name : "");
    eq_apply();
}

/* The editor (Settings > Sound > EQ, Menu on a profile). Profiles are saved
 * in Equalizer APO's format, so AutoEQ files edited here still load elsewhere. */
static int hook_eq_get(const char *name, eq_profile_t *p)
{
    char path[700], err[128];
    eq_path(name, path, sizeof path);
    return eq_load(path, p, err, sizeof err);
}

static int hook_eq_put(const char *name, const eq_profile_t *p)
{
    static const char *const type[] = {"PK", "LSC", "HSC"};
    char dir[600], path[700], tmp[720];
    snprintf(dir, sizeof dir, "%s/eq", g_data_dir);
    mkdir(dir, 0755);
    eq_path(name, path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.new", path);
    FILE *f = fopen(tmp, "we");
    if (!f) return -1;
    fprintf(f, "Preamp: %.1f dB\n", p->preamp_db);
    for (int i = 0; i < p->n; i++)
        fprintf(f, "Filter %d: ON %s Fc %.0f Hz Gain %.1f dB Q %.2f\n", i + 1, type[p->band[i].type],
                p->band[i].fc, p->band[i].gain_db, p->band[i].q);
    int bad = ferror(f);
    if (fclose(f) || bad || rename(tmp, path)) { unlink(tmp); return -1; }
    char key[64], label[64];
    eq_output(key, sizeof key, label, sizeof label);
    if (!strcmp(settings_get(key, ""), name)) eq_apply();   /* this output's choice: from the saved file */
    return 0;
}

/* Edited, not saved yet: heard on the output playing now. NULL: its own choice again. */
static void hook_eq_preview(const eq_profile_t *p)
{
    if (!p) { eq_apply(); return; }
    if (g_eng) engine_set_eq(g_eng, p);
}

static int hook_eq_remove(const char *name)
{
    char path[700], key[64], label[64];
    eq_path(name, path, sizeof path);
    if (unlink(path)) return -1;
    eq_output(key, sizeof key, label, sizeof label);
    if (!strcmp(settings_get(key, ""), name)) settings_set(key, "");
    eq_apply();
    return 0;
}

/* Every 250 ms: Wi-Fi's news (a network joined, an error) and its screens. */
static void wifi_poll(lv_timer_t *t)
{
    (void)t;
    static unsigned seen;
    unsigned s = wifi_seq();
    if (s == seen) return;
    seen = s;
    char msg[96];
    if (wifi_take_message(msg, sizeof msg)) ui_toast(msg, 2500);
    ui_wifi_changed();
}

/* ---- the network drive (Settings > Network drive) ---- */

/* Its password, and SSH's: made up the first time, kept in the settings (on
 * the card, like the Wi-Fi passwords: readable by anyone holding it). Letters
 * and digits that can't be mistaken for one another on the screen. */
static const char *made_up_password(const char *key, int fresh)
{
    static char pw[2][16];
    char *p = pw[!strcmp(key, "ssh.password")];
    snprintf(p, sizeof pw[0], "%s", settings_get(key, ""));
    if (fresh || strlen(p) < 8) {
        static const char set[] = "abcdefghjkmnpqrstuvwxyz23456789";
        rng_t r;
        rng_seed(&r, rng_os_seed(), (uint64_t)ms_now());
        for (int i = 0; i < 8; i++) p[i] = set[rng_below(&r, (uint32_t)(sizeof set - 1))];
        p[8] = '\0';
        settings_set(key, p);
        settings_save();                /* (fathom-ssh reads it at the next start) */
    }
    return p;
}
static const char *share_password(int fresh) { return made_up_password("share.password", fresh); }

static void hook_share_do(int what)
{
    int on = settings_get_int("share.on", 0);
    if (what == UI_SHARE_TOGGLE) {
        on = !on;
        settings_set_int("share.on", on);
        share_set(on, share_password(0));
    } else if (what == UI_SHARE_NEW_PASSWORD) {
        share_set(on, share_password(1));
    }
}

/* ---- SSH (Settings > SSH) ---- */

static void hook_ssh_do(int what)
{
    int on = settings_get_int("ssh.on", 0);
    if (what == UI_SSH_TOGGLE) {
        on = !on;
        settings_set_int("ssh.on", on);
        settings_save();                /* (and fathom-ssh's start-up, before the player's) */
        ssh_set(on, made_up_password("ssh.password", 0));
    } else if (what == UI_SSH_NEW_PASSWORD) {
        ssh_set(on, made_up_password("ssh.password", 1));
    }
}

/* ---- the clock from HTTPS, where NTP gets no answer ----
 * fathom-ntp (the udhcpc hook) keeps trying NTP while Wi-Fi is up, and counts
 * the tries that got no answer in /var/run/fathom-ntp-fails. While the clock
 * is plainly unset (before 2025) one is enough, else three: then the time is
 * taken from an HTTPS answer of the update check's host instead (netclock.c,
 * which says why its certificate's dates aren't checked), on a worker thread.
 * Once for each new count, so no more often than NTP is tried (1, 5, 15, then
 * every 30 minutes), until it has worked once. And at once when a secure
 * connection has just failed for want of the time (netclock_want: Find
 * stations, covers, the update check), at most every 30 s. */
#define CLOCK_HOST "api.github.com"          /* (UPDATE_API's) */
#define NTP_FAILS "/var/run/fathom-ntp-fails"
static volatile int g_https_clock_busy;     /* a worker is asking */
static volatile int g_https_clock_set;      /* it set the clock: for net_poll to keep */
static volatile int g_https_clock_done;     /* it had an answer: not asked again */
static volatile int g_clock_by_hand;        /* set in Settings since (it may be a little ahead) */

/* The time from one HTTPS answer of CLOCK_HOST, set if later (netclock_take):
 * the player's netclock fetcher, so one at a time (netclock_fetch, _ensure).
 * Blocking, seconds: worker threads only. 0 if the clock is set after it. */
static int clock_from_https(void)
{
    int64_t t = 0;
    const char *ca = ff_ca_file();
    if (!ca || netclock_https_date(CLOCK_HOST, 443, ca, &t) != 0) {
        fprintf(stderr, "clock: no answer from %s\n", CLOCK_HOST);
        return -1;
    }
    int64_t now = (int64_t)time(NULL);
    if (netclock_take(t, now, g_clock_by_hand ? NETCLOCK_HAND_BACK : 0)) {
        struct timeval tv = {(time_t)t, 0};
        if (settimeofday(&tv, NULL)) { perror("settimeofday"); return -1; }
        g_https_clock_set = g_https_clock_done = 1;
        g_clock_by_hand = 0;
    } else {
        fprintf(stderr, "clock: %s's time (%lld) isn't later: kept\n", CLOCK_HOST, (long long)t);
        g_https_clock_done = 1;              /* (the clock is fine as it is) */
    }
    return netclock_unset() ? -1 : 0;
}

static void *https_clock_thread(void *arg)
{
    (void)arg;
    netclock_fetch();
    g_https_clock_busy = 0;
    return NULL;
}

/* Before every network connection (ff_set_before_net, on its thread): with
 * the clock unset, the time first, then the connection, which then succeeds:
 * Find stations, covers, podcasts, the update check, a station over https. */
static void before_net(void)
{
    if (!netclock_unset() || !wifi_connected()) return;
    fprintf(stderr, "clock: unset before a secure connection: asking %s first\n", CLOCK_HOST);
    netclock_ensure();
}

static void https_clock_poll(void)
{
    static int last, ticks, asked_up;
    static uint32_t asked_at, up_since;      /* (ms_now; 0: never, not up) */
    if (g_https_clock_done || g_https_clock_busy || ++ticks % 10) return;   /* (every 5 s) */
    /* a fresh install (no clock.last), say: still unset 10 s after Wi-Fi joined,
     * NTP's first try not answered yet: asked then, once for each joining */
    int up = wifi_connected();
    if (!up) up_since = asked_up = 0;
    else if (!up_since) up_since = ms_now() | 1;
    int start = up && !asked_up && netclock_unset() && ms_now() - up_since >= 10000;
    int want = netclock_wanted() && netclock_unset() &&
               (!asked_at || ms_now() - asked_at >= 30000);   /* (a network that blocks it too: not hammered) */
    int fails = 0;
    FILE *f = fopen(NTP_FAILS, "re");
    if (f) {
        if (fscanf(f, "%d", &fails) != 1) fails = 0;
        fclose(f);
    }
    if (fails < last) last = 0;              /* counting again (a new address) */
    int ntp = fails != last && (fails >= 3 || (fails >= 1 && netclock_unset()));
    if (!up || (!ntp && !want && !start)) return;
    if (ntp) last = fails;
    if (start) asked_up = 1;
    asked_at = ms_now() | 1;
    if (ntp) fprintf(stderr, "clock: no answer from NTP (%d): asking %s\n", fails, CLOCK_HOST);
    else if (start) fprintf(stderr, "clock: unset 10 s after Wi-Fi joined: asking %s\n", CLOCK_HOST);
    else fprintf(stderr, "clock: wanted for a secure connection: asking %s\n", CLOCK_HOST);
    pthread_attr_t a;
    pthread_t t;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&a, 192 * 1024);
    g_https_clock_busy = 1;
    if (pthread_create(&t, &a, https_clock_thread, NULL)) g_https_clock_busy = 0;
    pthread_attr_destroy(&a);
}

/* About > Clock, set by hand (UTC): kept for the next start at once. NTP, or
 * an HTTPS answer (which may then move it back a day at most), sets it again
 * when Wi-Fi can. */
static int clock_set_by_hand(int64_t t)
{
    struct timeval tv = {(time_t)t, 0};
    if (netclock_unset_at(t) || settimeofday(&tv, NULL)) { perror("settimeofday"); return -1; }
    fprintf(stderr, "clock: set by hand\n");
    clock_save();
    settings_save();
    if (g_batlog_on) batlog_clock_set(&g_batlog, time(NULL), mono_s());
    g_clock_by_hand = 1;
    g_https_clock_done = 0;                  /* (an HTTPS answer may set it right again) */
    return 0;
}

/* Twice a second: the drive follows Wi-Fi; music copied to it (or moved,
 * deleted) is picked up once the copying has stopped for a while; and the
 * clock, when NTP (fathom-ntp) or an HTTPS answer has just set it, is kept
 * for the next start. */
static int g_share_rescan;
static volatile int g_scan_state;          /* (background scans, below) */
static int g_usb_drive;
static void scan_start(void);

static void net_poll(lv_timer_t *t)
{
    (void)t;
    static unsigned seen;
    share_set_wifi(wifi_connected());
    unsigned s = share_seq();
    if (s != seen) { seen = s; ui_share_changed(); }
    static unsigned ssh_seen;
    unsigned ss = ssh_seq();
    if (ss != ssh_seen) { ssh_seen = ss; ui_ssh_changed(); }
    if (share_take_changes()) g_share_rescan = 1;
    if (g_share_rescan && !g_scan_state && !g_usb_drive) {
        g_share_rescan = 0;
        fprintf(stderr, "share: music changed over the network: rescanning\n");
        sync();                         /* the scan reads the FAT directly */
        scan_start();
    }
    int by_https = g_https_clock_set;
    g_https_clock_set = 0;
    if (!access("/var/run/fathom-clock", F_OK) || by_https) {
        unlink("/var/run/fathom-clock");
        fprintf(stderr, "clock: set by %s\n", by_https ? CLOCK_HOST "'s answer" : "NTP");
        clock_save();
        if (g_batlog_on) batlog_clock_set(&g_batlog, time(NULL), mono_s());   /* (how long it was off, known now) */
    }
    https_clock_poll();
}

/* ---- updates (Settings > About > Check for updates) ----
 * When asked, or a quiet check by itself once a day with Check automatically
 * on (auto_update_poll); downloads and installs only ever when asked.
 * settings.ini keeps: update.auto (0/1), update.checked (the last answer's
 * time), update.found (a newer release seen, "" if none) and update.told (the
 * one the wake-up toast has announced: each is said once). */

#define UPDATE_STAGE CARD_DIR "/fathom/update"

static int g_update_asked;          /* a check from the screen: what it finds is seen there */

static void hook_update_do(int what)
{
    if (what == UI_UPD_CHECK && !wifi_connected() && !getenv("http_proxy")) {
        updater_refuse(!wifi_on() ? "Wi-Fi is off: Settings > Wi-Fi" : "Not connected yet: Settings > Wi-Fi");
        return;
    }
    if (what == UI_UPD_CHECK) g_update_asked = 1;
    updater_do(what);
}

/* What a check found (tag: newer, or "" for up to date), and whether its owner has seen it. */
static void update_found(const char *tag, int seen)
{
    settings_set_int("update.checked", (int)time(NULL));
    settings_set("update.found", tag);
    if (seen && tag[0]) settings_set("update.told", tag);
    settings_save();
}

/* The screen turned on by Power: a newer release found by the automatic
 * check, once (on two lines: a long version number may not leave room for
 * the second half on the first in the wider themes). */
static void update_announce(void)
{
    const char *tag = settings_get("update.found", "");
    if (!tag[0] || !strcmp(tag, settings_get("update.told", ""))) return;
    char t[160];
    tr_fill(t, sizeof t, "Fathom %s is available\nSettings > About", tag, NULL);
    ui_toast(t, 4000);
    fprintf(stderr, "update: told %s is available\n", tag);
    settings_set("update.told", tag);
    settings_save();
}

/* The player is up: an update's new kernel started (h2boot, bootloader/
 * fathom/bootpick.c, gives it two starts to get here before going back to the
 * one before); or it didn't, and the H2 went back: said once. */
static void boot_markers(void)
{
    static const char *const tried[] = {"boot-new", "boot-try1", "boot-try2"};
    char p[300];
    for (size_t i = 0; i < sizeof tried / sizeof tried[0]; i++) {
        snprintf(p, sizeof p, CARD_DIR "/fathom/%s", tried[i]);
        if (!unlink(p)) fprintf(stderr, "update: the new system started (%s)\n", tried[i]);
    }
    snprintf(p, sizeof p, CARD_DIR "/fathom/boot-fell-back");
    if (!access(p, F_OK)) {
        fprintf(stderr, "update: the new system didn't start twice: h2boot went back to the one before\n");
        ui_toast("The update didn't start, so the H2 went back to the previous version", 6000);
        unlink(p);
    }
}

/* Installed: the new kernel needs a restart, as power-off does it (init's
 * scripts stop the player, which closes the library). */
static void update_poll(lv_timer_t *t)
{
    (void)t;
    static unsigned seen;
    unsigned s = updater_seq();
    if (s != seen) {
        seen = s;
        ui_update_changed();
        ui_update_t st;
        updater_ui_state(&st);
        if (g_update_asked && st.state == UI_UPD_AVAILABLE) { g_update_asked = 0; update_found(st.latest, 1); }
        else if (g_update_asked && st.state == UI_UPD_LATEST) { g_update_asked = 0; update_found("", 1); }
        else if (st.state == UI_UPD_FAILED || st.state == UI_UPD_IDLE) g_update_asked = 0;   /* (or cancelled) */
    }
    if (!updater_installed()) return;
    fprintf(stderr, "update: installed, restarting\n");
    g_powering_off = 1;                /* "Restarting" stays on the screen */
    batlog_end();
    clock_save();
    settings_save();
    queue_save();
    screen_set(1);
    lv_refr_now(NULL);
    sync();
    if (system("/sbin/reboot") != 0) fprintf(stderr, "reboot failed\n");
}

/* ---- Uninstall Fathom (Settings > About > Uninstall Fathom) ----
 * The battery is checked here, before the file is made ready and again before
 * the restart: HiBy's updater then rewrites the flash, and must not run out
 * half way. The restart goes straight into HiBy's recovery (h2boot honours
 * HiBy's own signal, uninstaller_ask_recovery); if the signal can't be set, the
 * H2 switches off and the toast says how to get there by hand. */

static void hook_uninstall_do(int what)
{
    if ((what == UI_UNI_APPLY || what == UI_UNI_RESTART) && !g_charging && g_bat_pct >= 0 &&
        g_bat_pct < UNINSTALL_MIN_BATTERY) {
        ui_toast("Charge the H2 to 50 % or plug it in first", 3000);
        return;
    }
    if (what != UI_UNI_RESTART) {
        uninstaller_do(what);
        return;
    }
    if (uninstaller_ask_recovery()) {
        power_off("Hold Vol+ as you turn the H2 on, then choose Boot select > HibyOS Recovery");
        return;
    }
    fprintf(stderr, "uninstall: restarting into HiBy's recovery\n");
    g_powering_off = 1;
    batlog_end();
    clock_save();
    settings_save();
    queue_save();
    screen_set(1);
    ui_message_screen("Restarting", "HiBy's recovery puts its firmware back. Don't turn the H2 off.");
    lv_refr_now(NULL);
    sync();
    if (system("/sbin/reboot") != 0) fprintf(stderr, "reboot failed\n");
}

static void uninstall_poll(void)
{
    static unsigned seen;
    unsigned s = uninstaller_seq();
    if (s != seen) {
        seen = s;
        ui_uninstall_changed();
    }
}

/* ---- internet radio ---- */

/* Home > Radio: the stations in fathom/radio.txt, or until there is one, the
 * example that comes with Fathom (fathom/assets/radio.txt, replaced by updates). */
static int hook_radio_stations(radio_station_t *out, int max)
{
    char path[600];
    snprintf(path, sizeof path, "%s/radio.txt", g_data_dir);
    int n = radio_load(path, out, max);
    if (n < 0) {
        snprintf(path, sizeof path, "%s/assets/radio.txt", g_data_dir);
        n = radio_load(path, out, max);
    }
    /* Your stations shown: their names looked up meanwhile, so the one chosen
     * doesn't wait for DNS (a few hundred ms on the H2, sometimes seconds) */
    if (wifi_connected()) {
        for (int i = 0; i < n && i < 8; i++) dnscache_prefetch_url(out[i].url);
#ifdef FATHOM_TLS_WRAP
        tls_wrap_warm();             /* and the certificates parsed */
#endif
    }
    return n;
}

/* Find stations: your list changed from the H2 (made from the example the first time). */
static int hook_radio_add(const radio_station_t *st)
{
    char path[600], example[600];
    snprintf(path, sizeof path, "%s/radio.txt", g_data_dir);
    snprintf(example, sizeof example, "%s/assets/radio.txt", g_data_dir);
    return radio_add(path, example, st);
}

static int hook_radio_remove(const char *url)
{
    char path[600], example[600];
    snprintf(path, sizeof path, "%s/radio.txt", g_data_dir);
    snprintf(example, sizeof example, "%s/assets/radio.txt", g_data_dir);
    return radio_remove(path, example, url);
}

/* Home > Podcasts: an OPML file (subscriptions exported from another app) to
 * offer, the first by name at the top of the card, in Music or in
 * fathom/podcasts. */
static const char *g_music;       /* the music folder (--music) */
static int hook_podcast_opml(char *path, size_t cap)
{
    char dirs[3][600];
    snprintf(dirs[0], sizeof dirs[0], "%s", CARD_DIR);
    snprintf(dirs[1], sizeof dirs[1], "%s", g_music ? g_music : CARD_DIR "/Music");
    snprintf(dirs[2], sizeof dirs[2], "%s", podsync_dir());
    for (int d = 0; d < 3; d++) {
        DIR *dir = opendir(dirs[d]);
        struct dirent *e;
        char best[256] = "";
        while (dir && (e = readdir(dir))) {
            size_t n = strlen(e->d_name);
            if (e->d_name[0] == '.' || n < 6 || strcasecmp(e->d_name + n - 5, ".opml")) continue;
            if (!best[0] || strcmp(e->d_name, best) < 0) snprintf(best, sizeof best, "%s", e->d_name);
        }
        if (dir) closedir(dir);
        if (best[0]) { snprintf(path, cap, "%s/%s", dirs[d], best); return 1; }
    }
    return 0;
}

/* A station can play once Wi-Fi has an address (or, for testing over USB, a
 * proxy is set: http_proxy=http://10.42.0.2:PORT). */
static int hook_radio_ready(char *why, size_t cap)
{
    if (wifi_connected() || getenv("http_proxy")) return 0;
    if (!wifi_available()) snprintf(why, cap, "The radio needs Wi-Fi");
    else if (!wifi_on()) snprintf(why, cap, "Wi-Fi is off: Settings > Wi-Fi");
    else snprintf(why, cap, "Not connected yet: Settings > Wi-Fi");
    return -1;
}

/* Twice a second: the radio's live info (stations whose streams don't say);
 * Wi-Fi switched off stops the radio (rather than it retrying for a minute). */
static void radio_poll(lv_timer_t *t)
{
    (void)t;
    if (!g_player) return;
    const radio_station_t *s = player_radio_station(g_player);
    player_now_t n;
    player_now(g_player, &n);
    if (s && n.playing && !wifi_on() && !getenv("http_proxy")) {
        player_pause(g_player, 1);
        n.playing = 0;
    }
    radiometa_want(s && n.playing ? s->info : NULL);
    /* a station playing: the ones Next and Previous go to, looked up ahead */
    static const radio_station_t *ahead_of;
    if (s != ahead_of && wifi_connected()) {   /* (not connected yet: once it is) */
        ahead_of = s;
        const radio_station_t *first = player_radio_at(g_player, 0);
        int i = s && first ? (int)(s - first) : -1;
        const radio_station_t *nx = i >= 0 ? player_radio_at(g_player, i + 1) : NULL;
        const radio_station_t *pv = i > 0 ? player_radio_at(g_player, i - 1) : NULL;
        if (nx) dnscache_prefetch_url(nx->url);
        if (pv) dnscache_prefetch_url(pv->url);
    }
    char a[256], ti[256], v[512];
    if (s && radiometa_take(a, sizeof a, ti, sizeof ti, v, sizeof v)) player_radio_set_info(g_player, a, ti, v);
    /* the song's cover, when the station names one */
    static uint16_t art[LIB_ART_LARGE * LIB_ART_LARGE];
    radiometa_want_art(s && n.playing ? n.art_url : NULL);
    int r = radiometa_take_art(art, LIB_ART_LARGE);
    if (r >= 0) ui_set_radio_art(r ? art : NULL);
}

/* Once a second while Bluetooth is on: what's playing, for fathom-avrcp (what
 * the headphones see of the player). Rewritten only when it changed. */
#define NOW_FILE "/tmp/fathom-now"
static void now_publish(lv_timer_t *t)
{
    (void)t;
    static char last[1100];
    if (!g_player || !bt_on()) {
        if (last[0]) { unlink(NOW_FILE); last[0] = '\0'; }
        return;
    }
    player_now_t n;
    player_now(g_player, &n);
    char buf[sizeof last];
    snprintf(buf, sizeof buf, "status %s\nid %lld\nlen %u\npos %u\ntitle %s\nartist %s\nalbum %s\n",
             n.playing ? "Playing" : n.active && !n.stopped ? "Paused" : "Stopped",
             n.radio ? -2 - (long long)player_seq(g_player) : (long long)n.track.id,   /* radio: new song, new id */
             (unsigned)n.len_ms, (unsigned)n.pos_ms, n.track.title, n.track.artist, n.track.album);
    if (!strcmp(buf, last)) return;
    FILE *f = fopen(NOW_FILE ".new", "we");
    if (!f) return;
    fputs(buf, f);
    if (fclose(f) == 0 && rename(NOW_FILE ".new", NOW_FILE) == 0) memcpy(last, buf, sizeof last);
}

static void bt_poll(lv_timer_t *t)
{
    (void)t;
    static unsigned seen;
    static int lost_shown;
    if (!g_eng) return;
    if (out_router_on_bt()) {                          /* the link stopped taking audio: say why it paused */
        eng_status_t st;
        engine_status(g_eng, &st);
        if (st.output_lost && !lost_shown) ui_toast("Headphones stopped: paused", 3000);
        lost_shown = st.output_lost;
    }
    unsigned s = bt_seq();
    if (s == seen) return;
    seen = s;
    char msg[96];
    if (bt_take_message(msg, sizeof msg)) ui_toast(msg, 2500);
    ui_bt_changed();
    char pcm[200], name[64], last[18], busy[32];
    int have = bt_pcm(pcm, sizeof pcm, name, sizeof name);
    int on = bt_ui_state(busy, sizeof busy);
    ui_set_bt_link(have ? UI_BT_LINK_CONNECTED
                   : on && (!strcmp(busy, "Connecting") || !strcmp(busy, "Pairing"))
                   ? UI_BT_LINK_CONNECTING : UI_BT_LINK_NONE);
    if (have && strcmp(pcm, g_bt_routed)) {            /* headphones connected */
        snprintf(g_bt_routed, sizeof g_bt_routed, "%s", pcm);
        if (bt_last(last, sizeof last) && strcmp(last, settings_get("bluetooth.last", "")))
            settings_set("bluetooth.last", last);
        if (g_usb_card >= 0 || g_relay) return;        /* the USB DAC keeps the audio */
        engine_switch_output(g_eng, out_router_bt, pcm);
        route_apply(0);                                 /* the H2's own output goes quiet */
        ui_set_output(name);
        ui_toast(name, 2000);
        fprintf(stderr, "bluetooth: playing to %s (%s)\n", name, pcm);
    } else if (!have && g_bt_routed[0]) {              /* gone */
        g_bt_routed[0] = '\0';
        if (!out_router_on_bt()) return;
        engine_pause(g_eng, 1);
        engine_switch_output(g_eng, out_router_alsa, (void *)HW_PCM);
        route_apply(0);
        ui_toast("Bluetooth disconnected: paused", 2500);
        fprintf(stderr, "bluetooth: disconnected\n");
    }
}

/* ---- screen: brightness levels, timeout and dimming ----
 *
 * Brightness is one of UI_BRIGHTNESS_LEVELS steps onto the PWM (level_pwm),
 * never fully dark while the screen is on. With a timeout set, the screen
 * dims to 30 % five seconds before it turns off; any key undims it. While the
 * screen is off, play/pause, Prev/Next and volume still work, and only Power
 * turns it back on (no hold switch on the H2: nothing happens in a pocket).
 */

static int g_screen_on = 1, g_dimmed;
static int g_level = UI_BRIGHTNESS_DEFAULT, g_timeout = UI_TIMEOUT_DEFAULT;
static uint32_t g_last_input;

/* The H2's backlight goes dark below about 49/255 rather than dimming, so
 * level 1 is 50 and the rest follow a gentle curve (exponent 1.6) up to 255.
 * Dimming before the screen turns off never goes below that floor either. */
#define BRIGHTNESS_FLOOR 50

static int level_pwm(int level)
{
    int max = DISP(backlight_max)();
    double f = (double)(level - 1) / (UI_BRIGHTNESS_LEVELS - 1);
    int v = BRIGHTNESS_FLOOR + (int)lround((max - BRIGHTNESS_FLOOR) * pow(f, 1.6));
    return v < BRIGHTNESS_FLOOR ? BRIGHTNESS_FLOOR : v > max ? max : v;
}

static void screen_apply(void)
{
    int pwm = level_pwm(g_level);
    if (!g_screen_on) pwm = 0;
    else if (g_dimmed) pwm = pwm * 3 / 10 > BRIGHTNESS_FLOOR ? pwm * 3 / 10 : BRIGHTNESS_FLOOR;
    DISP(set_backlight_raw)(pwm);
}

static void screen_set(int on)
{
    if (on != g_screen_on) ui_set_awake(on);
    if (on && !g_screen_on) wifi_wake();                     /* (out and back: a saved network may be in range) */
    if (!on && g_screen_on) library_release_memory(g_lib);   /* the database's cache: nothing browses meanwhile */
    g_screen_on = on;
    g_dimmed = 0;
    g_last_input = ms_now();
    screen_apply();
}

static void screen_activity(void)
{
    g_last_input = ms_now();
    if (g_dimmed) { g_dimmed = 0; screen_apply(); }
}

static void hook_brightness(int level) { g_level = level; g_dimmed = 0; screen_apply(); }
static void hook_timeout(int seconds) { g_timeout = seconds; g_last_input = ms_now(); }

static int usb_powered(void)
{
    char s[8];
    return read_int_file(USB_ONLINE, s, sizeof s) == 0 && s[0] == '1';
}

static void screen_timer(lv_timer_t *t)
{
    (void)t;
    int timeout = g_transport && (g_timeout <= 0 || g_timeout > TRANSPORT_TIMEOUT_S) ? TRANSPORT_TIMEOUT_S : g_timeout;
    if (!g_screen_on || timeout <= 0) return;
    if (!g_transport && settings_get_int("display.on_charging", UI_ON_CHARGING_DEFAULT) && usb_powered()) { g_last_input = ms_now(); return; }
    uint32_t idle = ms_now() - g_last_input, limit = (uint32_t)timeout * 1000;
    if (idle >= limit) screen_set(0);
    else if (!g_dimmed && limit > 5000 && idle >= limit - 5000 && settings_get_int("display.dim", UI_DIM_DEFAULT)) {
        g_dimmed = 1;
        screen_apply();
    }
}

/* ---- charging mode ----
 * Powered on by plugging in USB power (not the Power key), the H2 shows that
 * it's charging, then turns its screen off; nothing plays. Power starts it as
 * usual; other keys show the charge again; unplugged, it turns itself off.
 * The reason comes from the PMIC (power_on_reason, axp2101 driver), unless
 * the SoC was restarted (/tmp/fathom-reset, from S01bootlog: the PMIC still
 * says what powered it on originally); /tmp/fathom-power-on-reason overrides
 * both (tests). */
static int g_charge_mode;
static lv_obj_t *g_charge_screen;
static uint32_t g_charge_shown;     /* when the screen was last shown */
#define CHARGE_SHOW_MS 6000

static void power_on_reason(char *out, size_t cap)
{
    char reset[16] = "";
    snprintf(out, cap, "other");
    if (read_int_file("/tmp/fathom-power-on-reason", out, cap) == 0) {
        /* tests */
    } else if (read_int_file("/tmp/fathom-reset", reset, sizeof reset) == 0 && !strncmp(reset, "restart", 7)) {
        snprintf(out, cap, "restart");
    } else {
        DIR *d = opendir("/sys/bus/i2c/devices");
        struct dirent *e;
        char path[300];
        while (d && (e = readdir(d))) {
            snprintf(path, sizeof path, "/sys/bus/i2c/devices/%s/power_on_reason", e->d_name);
            if (read_int_file(path, out, cap) == 0) break;
        }
        if (d) closedir(d);
    }
    out[strcspn(out, "\r\n")] = '\0';
}

static void charge_screen_show(void)
{
    char pct[32] = "", body[160];
    read_int_file(HW_BATTERY "capacity", pct, sizeof pct);
    pct[strcspn(pct, "\r\n")] = '\0';
    char pc[40];
    snprintf(pc, sizeof pc, "%s%%", pct[0] ? pct : "-");
    tr_fill(body, sizeof body, "%s\n\nPress Power to start", pc, NULL);
    if (g_charge_screen) lv_obj_delete(g_charge_screen);
    g_charge_screen = ui_message_screen("Charging", body);
    screen_set(1);
    g_charge_shown = ms_now();
}

/* Started for good in this boot (not in charging mode, or left it): a
 * restart of the player (after USB drive mode, the card back) mustn't go
 * back into charging mode because the PMIC still says USB powered it on. */
#define STARTED_MARK "/tmp/fathom-started"

static void started_mark(void)
{
    int fd = open(STARTED_MARK, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
    if (fd >= 0) close(fd);
}

static void charge_mode_end(void)
{
    started_mark();
    g_charge_mode = 0;
    if (g_charge_screen) lv_obj_delete(g_charge_screen);
    g_charge_screen = NULL;
    screen_set(1);
}

/* Every second: the screen off after a while; off altogether when unplugged. */
static void charge_poll(lv_timer_t *t)
{
    (void)t;
    if (!g_charge_mode) return;
    if (!g_charging) { power_off("Unplugged"); return; }
    if (g_screen_on && ms_now() - g_charge_shown > CHARGE_SHOW_MS) screen_set(0);
}

/* ---- Settings hooks: DAC filter, rescan, About ---- */

static const char *const DAC_FILTER = "DAC Filter";

static void dac_filter_name(char *out, size_t cap)
{
    if (mixer_enum_get(HW_CTL, DAC_FILTER, out, (int)cap) < 0) snprintf(out, cap, "-");
}

static void dac_filter_next(void)
{
    int n = mixer_enum_count(HW_CTL, DAC_FILTER);
    int cur = mixer_enum_get(HW_CTL, DAC_FILTER, NULL, 0);
    if (n <= 0 || cur < 0) return;
    int next = (cur + 1) % n;
    if (mixer_enum_set(HW_CTL, DAC_FILTER, next) == 0) settings_set_int("sound.dac_filter", next);
}

static int scan_with_progress(library_t *lib, const char *music, const theme_t *t, const ui_fonts_t *f);

/* ---------- USB drive mode ----------
 * The player can't run without the card, so it hands over: it leaves a "USB
 * drive" screen up (the display keeps it) and exits with code 42; S90fathom
 * runs fathom-usbdisk, which lends the card to the computer until it's
 * ejected or unplugged, and then starts the player again, which picks up the
 * new music (/tmp/fathom-rescan). */
#define USB_DRIVE_EXIT 42
static int g_usb_drive;      /* exiting for USB drive mode */
static int g_usb_was;        /* connected to a computer at the last poll */

static int usb_on_computer(void)
{
    DIR *d = opendir("/sys/class/udc");
    struct dirent *e;
    char path[300], state[32] = "";
    while (d && (e = readdir(d)))
        if (e->d_name[0] != '.') {
            snprintf(path, sizeof path, "/sys/class/udc/%s/state", e->d_name);
            read_int_file(path, state, sizeof state);
            break;
        }
    if (d) closedir(d);
    return !strncmp(state, "configured", 10);
}

/* After Reset all settings: out, and S90fathom starts the player again
 * (2 s), which sets everything from the defaults. */
static void restart_player(void)
{
    fprintf(stderr, "settings reset: starting again\n");
    g_quit = 1;
}

static void usb_drive(void)
{
    if (!usb_on_computer()) { ui_toast("Connect the H2 to a computer first", 2500); return; }
    g_usb_drive = 1;
    g_quit = 1;
}

static uint32_t g_usb_quiet_until;    /* the gadget was just switched: not a new connection */
static void usb_dac_mode(void);

static void usb_poll(lv_timer_t *t)
{
    (void)t;
    int on = usb_on_computer();
    if (g_relay || (int32_t)(ms_now() - g_usb_quiet_until) < 0) on = g_usb_was = on || g_usb_was;
    if (on && !g_usb_was) {        /* just plugged into a computer */
        fprintf(stderr, "usb: on a computer [%u.%03u]\n", ms_now() / 1000, ms_now() % 1000);
        int what = settings_get_int("usb.computer", UI_USB_ASK);
        if (what == UI_USB_DRIVE) {
            usb_drive();
        } else if (what == UI_USB_DAC) {
            usb_dac_mode();
        } else if (what == UI_USB_ASK) {
            screen_set(1);
            screen_activity();
            ui_usb_prompt();
        }
    }
    if (!on && g_usb_was) ui_usb_prompt_close();   /* unplugged before answering */
    g_usb_was = on;
}

/* ---------- USB DAC mode ----------
 * The H2 becomes the computer's sound card: fathom-usbgadget puts a USB
 * audio input in the USB drive's place, and the relay (uac_relay.c) copies
 * what the computer plays to the DAC, bit for bit, at the H2's own volume
 * (headphones or line out as usual). The music pauses and the player lets go
 * of the DAC. Back ends it, and so does unplugging the cable. */
static lv_obj_t *g_dac_screen;
static int g_dac_gone;                /* seconds the cable has been out */
static char g_dac_text[200];

static int usb_gadget(const char *mode)
{
    char cmd[64];
    snprintf(cmd, sizeof cmd, "fathom-usbgadget %s", mode);
    int rc = system(cmd);
    return rc == 0 ? 0 : -1;
}

static void usb_dac_mode(void)
{
    if (g_relay || !g_eng) return;
    if (!usb_on_computer()) { ui_toast("Connect the H2 to a computer first", 2500); return; }
    if (g_player) player_pause(g_player, 1);
    /* the engine lets go of the DAC (it can't open this) until the mode ends */
    engine_switch_output(g_eng, out_router_alsa, (void *)"/dev/snd/usb-dac-mode");
    route_apply(0);                    /* (from Bluetooth the H2's DAC was left muted) */
    g_usb_quiet_until = ms_now() + 15000;
    if (usb_gadget("dac")) {
        output_home();
        ui_toast("USB DAC mode isn't available", 2500);
        return;
    }
    g_relay = uac_relay_start(HW_PCM);
    if (!g_relay) {                    /* (out of memory): all the way back */
        usb_gadget("normal");
        output_home();
        ui_toast("USB DAC mode couldn't start", 2500);
        return;
    }
    g_dac_text[0] = '\0';
    g_dac_screen = ui_dac_screen();
    g_dac_gone = 0;
    screen_set(1);
    fprintf(stderr, "usb dac mode: on\n");
}

/* Media keys for the computer (fathom-usbgadget's HID function): one report
 * with the key's bit, then one with none, as a keyboard sends them. */
enum { HID_PLAY = 1, HID_NEXT = 2, HID_PREV = 4 };
static int g_hid = -1;

static int hid_send(unsigned char r)
{
    struct pollfd pf = {.fd = g_hid, .events = POLLOUT};
    if (poll(&pf, 1, 50) <= 0) return -1;     /* the host collects a report every ms */
    return write(g_hid, &r, 1) == 1 ? 0 : -1;
}

static void usb_dac_key(int key)
{
    if (g_hid < 0) g_hid = open("/dev/hidg0", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (g_hid < 0) { ui_toast("Media keys aren't available", 1500); return; }
    if (hid_send((unsigned char)key) || hid_send(0)) {
        ui_toast("The computer didn't take the key", 1500);
        return;
    }
    ui_toast(key == HID_PLAY ? "Play / pause" : key == HID_NEXT ? "Next" : "Previous", 800);
}

/* The Bluetooth headphones' buttons (IN_MEDIA_*): playback, wherever the UI is
 * and with the screen off; in USB DAC mode, the computer's media keys. */
static void media_key(int k)
{
    g_last_input = ms_now();                           /* use, for the idle power-off */
    if (g_relay) {
        usb_dac_key(k == IN_MEDIA_NEXT ? HID_NEXT : k == IN_MEDIA_PREV ? HID_PREV : HID_PLAY);
        return;
    }
    player_now_t n = {0};
    if (g_player) player_now(g_player, &n);
    fprintf(stderr, "bluetooth: headphone button %s\n", k == IN_MEDIA_TOGGLE ? "play/pause" : k == IN_MEDIA_PLAY ? "play" :
            k == IN_MEDIA_PAUSE ? "pause" : k == IN_MEDIA_NEXT ? "next" : "previous");
    switch (k) {
    case IN_MEDIA_TOGGLE: ui_key(UI_KEY_PLAY); break;
    case IN_MEDIA_PLAY: if (!n.playing) ui_key(UI_KEY_PLAY); break;
    case IN_MEDIA_PAUSE: if (n.playing) ui_key(UI_KEY_PLAY); break;
    case IN_MEDIA_NEXT: ui_key(UI_KEY_NEXT); break;
    case IN_MEDIA_PREV: ui_key(UI_KEY_PREV); break;
    default: break;
    }
}

static void usb_dac_mode_end(const char *why)
{
    if (!g_relay) return;
    uac_relay_status_t st;
    uac_relay_status(g_relay, &st);
    uac_relay_stop(g_relay);
    g_relay = NULL;
    if (g_hid >= 0) close(g_hid);
    g_hid = -1;
    fprintf(stderr, "usb dac mode: off (%s); %llu frames, %u overruns\n", why, st.frames, st.overruns);
    g_usb_quiet_until = ms_now() + 15000;
    usb_gadget("normal");
    output_home();
    if (g_dac_screen) lv_obj_delete(g_dac_screen);
    g_dac_screen = NULL;
    screen_set(1);
    ui_toast(why, 2500);
}

/* Twice a second: what's playing, and whether the cable is still in. */
static void usb_dac_mode_poll(lv_timer_t *t)
{
    (void)t;
    if (!g_relay) return;
    uac_relay_status_t st;
    uac_relay_status(g_relay, &st);
    char fmt[32] = "", note[24] = "", text[200];
    int exact = 0;
    if (st.playing) {
        /* The DAC takes 24 bits: a 16- or 24-bit stream reaches it untouched; a
         * 32-bit one (an app's float output) loses its lowest 8 bits. */
        char depth[16] = "";
        if (st.bits) snprintf(depth, sizeof depth, "%d-bit ", st.bits);
        snprintf(fmt, sizeof fmt, "%s%u.%u kHz", depth, st.rate / 1000, st.rate % 1000 / 100);
        exact = st.bits && st.bits <= 24;
        snprintf(note, sizeof note, "%s", !st.bits ? "" : exact ? "Bit-perfect" : "Played at 24-bit");
    }
    snprintf(text, sizeof text, "%s|%s|%s", fmt, note,
             st.playing ? "Keys control the computer\nBack to stop" : "On the computer, choose \"Fathom H2\" as the sound output.\n\nBack to stop");
    if (strcmp(text, g_dac_text) && g_dac_screen) {
        snprintf(g_dac_text, sizeof g_dac_text, "%s", text);
        ui_dac_screen_set(g_dac_screen, fmt, note, exact, strrchr(text, '|') + 1);
    }
    /* No USB power for 3 s: unplugged. (Not the link's state: it isn't
     * "configured" until the computer takes the new device, and a Mac first
     * asks whether to allow it; it's also suspended while the computer sleeps.) */
    static uint32_t last;
    uint32_t now = ms_now();
    if (now - last < 1000) return;
    last = now;
    g_dac_gone = !usb_powered() ? g_dac_gone + 1 : 0;
    if (g_dac_gone >= 3) usb_dac_mode_end("Unplugged: USB DAC mode ended");
}

/* A file in /tmp left by fathom-usbdisk, read and removed; 0 if there was one. */
static int take_tmp_file(const char *path, char *out, size_t cap)
{
    FILE *f = fopen(path, "re");
    if (!f) return -1;
    if (out && cap) {
        out[0] = '\0';
        if (fgets(out, (int)cap, f)) out[strcspn(out, "\n")] = '\0';
    }
    fclose(f);
    unlink(path);
    return 0;
}

/* What fathom-usbdisk says when it can't lend the card, here for the
 * translations' list (the toast translates them); one it adds later shows
 * as it wrote it. */
static const char *usbdisk_text(const char *msg)
{
    static const char *const known[] = {N_("USB drive mode isn't available"), N_("The card is in use: try again"),
                                        N_("Couldn't start USB drive mode")};
    for (size_t i = 0; i < sizeof known / sizeof known[0]; i++)
        if (!strcmp(msg, known[i])) return known[i];
    return msg;
}

/* ---------- background scans ----------
 * Checking the card for new music takes about a second when nothing changed
 * (the FAT read directly), minutes after adding music (tags, covers), so after
 * USB drive mode, a card put back, a network drive's changes and from
 * Settings > Library > Rescan it runs on a thread with its own connection to
 * the library while the music plays; the lists are refreshed when it's done,
 * if anything changed. */
static char g_db_path[512], g_art_path[512], g_pl_path[600];
static pthread_t g_scan_thread;
static library_t *g_scan_lib;              /* the scan's own connection */
static volatile int g_scan_state;          /* 0 idle, 1 running, 2 finished */
static int g_scan_result, g_scan_changes;

/* Scans read the music folder's FAT directly when it's on a vfat partition
 * (library_set_fast_walk): find it in /proc/mounts. */
static void fast_walk_setup(library_t *lib, const char *music)
{
    FILE *f = fopen("/proc/mounts", "re");
    if (!f || !music) { if (f) fclose(f); return; }
    char dev[128], mnt[256], type[32], best_dev[128] = "", best_mnt[256] = "";
    while (fscanf(f, "%127s %255s %31s %*[^\n]", dev, mnt, type) == 3) {
        size_t l = strlen(mnt);
        if (!strcmp(type, "vfat") && !strncmp(music, mnt, l) && (music[l] == '/' || !music[l]) &&
            l > strlen(best_mnt)) {
            snprintf(best_dev, sizeof best_dev, "%s", dev);
            snprintf(best_mnt, sizeof best_mnt, "%s", mnt);
        }
    }
    fclose(f);
    if (best_dev[0]) library_set_fast_walk(lib, best_dev, best_mnt);
}

/* The card's free space, asked once in the background soon after the start: the
 * FAT driver doesn't trust the count kept on the card, so the first statvfs()
 * after a mount reads the whole FAT (15 MB on a 128 GB card: 1.9 s at start-up),
 * and Settings > About, usually the first to ask, stalled for it. Counted, the
 * kernel keeps it up to date; About's later asks take a millisecond. (The player
 * starts again after USB drive mode and when the card comes back: asked again.) */
static void *free_space_thread(void *arg)
{
    (void)arg;
    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 19);   /* below the music and the UI */
    sleep(15);                                                    /* (after the start's own reading) */
    struct statvfs st;
    statvfs(g_music ? g_music : CARD_DIR, &st);
    return NULL;
}

static void free_space_warm(void)
{
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&a, 64 * 1024);
    pthread_create(&t, &a, free_space_thread, NULL);
    pthread_attr_destroy(&a);
}

static void *scan_thread(void *arg)
{
    (void)arg;
    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 19);   /* below the music and the UI, as levelling */
    g_scan_result = g_scan_lib ? library_scan(g_scan_lib, g_music, NULL, NULL) : -1;
    g_scan_changes = g_scan_lib && g_scan_result >= 0 ? library_scan_changes(g_scan_lib) : 0;
    g_scan_state = 2;
    return NULL;
}

static void scan_start(void)
{
    if (g_scan_state) return;
    g_scan_lib = library_open(g_db_path, g_art_path);
    if (!g_scan_lib) { ui_toast("Can't open the library", 2000); return; }
    library_set_playlist_dir(g_scan_lib, g_pl_path);
    fast_walk_setup(g_scan_lib, g_music);
    g_scan_state = 1;
    if (pthread_create(&g_scan_thread, NULL, scan_thread, NULL)) {
        g_scan_state = 0;
        library_close(g_scan_lib);
        g_scan_lib = NULL;
    }
}

/* Called often: picks up a finished scan. */
static void scan_poll(lv_timer_t *t)
{
    (void)t;
    if (g_scan_state != 2) return;
    pthread_join(g_scan_thread, NULL);
    library_close(g_scan_lib);
    g_scan_lib = NULL;
    g_scan_state = 0;
    fprintf(stderr, "library: background scan: %d tracks, %d changes\n", g_scan_result, g_scan_changes);
    if (g_scan_result >= 0 && g_scan_changes > 0) {
        ui_library_changed();
        ui_toast("Library updated", 1500);
    }
}

/* On the way out (and before lending the card): stop a scan and wait for it. */
static void scan_stop(void)
{
    if (!g_scan_state) return;
    library_scan_cancel(g_scan_lib);
    pthread_join(g_scan_thread, NULL);
    library_close(g_scan_lib);
    g_scan_lib = NULL;
    g_scan_state = 0;
}

/* ---------- volume levelling: measuring loudness ----------
 * Tracks without ReplayGain tags are measured (EBU R128) on a low-priority
 * thread with its own connection to the library, only while the H2 is charging
 * and not playing (a first pass over a big library takes most of a night).
 * Each track is stored as it's done; stopping loses at most the one under way. */
static pthread_t g_rg_thread;
static library_t *g_rg_lib;
static volatile int g_rg_running, g_rg_stop;

static int rg_measure(const char *path, lib_rg_t *rg)
{
    audio_format_t f;
    decoder_t *d = decoder_open(path, &f);
    if (!d) return -1;
    loudness_t *l = loudness_new(f.rate, f.channels);
    if (!l) { decoder_close(d); return -1; }
    static int32_t buf[4096 * 2];
    long n;
    while (!g_rg_stop && (n = decoder_read(d, buf, 4096)) > 0) loudness_add(l, buf, n);
    double lufs = loudness_lufs(l);
    rg->track_peak = (float)loudness_peak(l);
    loudness_free(l);
    decoder_close(d);
    if (g_rg_stop) return 1;                        /* not finished: done again later */
    rg->state = 2;
    rg->track_gain = isinf(lufs) ? 0.0f : (float)(LOUDNESS_REFERENCE - lufs);
    return 0;
}

static void *rg_thread(void *arg)
{
    (void)arg;
    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 19);   /* below the music and the UI */
    int64_t id;
    char path[1024];
    int measured = 0, tagged = 0;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    while (!g_rg_stop && library_rg_next(g_rg_lib, &id, path, sizeof path) == 0) {
        lib_rg_t rg = {0};
        tags_t t;
        if (tags_read(path, &t) == 0 && t.has_rg_track) {    /* tagged since the last scan */
            rg = (lib_rg_t){1, t.rg_track_gain, t.rg_track_peak, t.has_rg_album, t.rg_album_gain, t.rg_album_peak};
            tagged++;
        } else {
            int r = rg_measure(path, &rg);
            if (r > 0) break;                            /* stopped half way */
            if (r < 0) rg.state = 3;
            else measured++;
        }
        library_rg_store(g_rg_lib, id, &rg);
        if ((measured + tagged) % 50 == 0) {
            clock_gettime(CLOCK_MONOTONIC, &t1);
            fprintf(stderr, "levelling: %d measured, %d from tags, %.0f s\n", measured, tagged,
                    (double)(t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9);
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    fprintf(stderr, "levelling: %d measured, %d from tags in %.0f s\n", measured, tagged,
            (double)(t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9);
    g_rg_running = 0;
    return NULL;
}

static void rg_stop(void)
{
    if (!g_rg_lib) return;
    g_rg_stop = 1;
    pthread_join(g_rg_thread, NULL);
    library_close(g_rg_lib);
    g_rg_lib = NULL;
}

/* Every second: start measuring while charging and idle, with levelling on
 * (off, the default: no use for it; measured songs are kept), stop otherwise. */
static void rg_poll(lv_timer_t *t)
{
    (void)t;
    int playing = 0;
    if (g_player) {
        player_now_t now;
        player_now(g_player, &now);
        playing = now.playing;
    }
    int want = g_charging && !playing && !g_scan_state && !g_usb_drive && !g_relay && g_db_path[0] &&
               settings_get_int("sound.levelling", LEVEL_OFF) != LEVEL_OFF;
    if (g_rg_lib && (!want || !g_rg_running)) rg_stop();   /* stopped, or out of tracks */
    else if (want && !g_rg_lib) {
        static int64_t done_at;             /* nothing left: look again in a while (new music) */
        int64_t now_s = (int64_t)time(NULL);
        if (done_at && now_s - done_at < 600) return;
        int64_t id;
        char path[8];
        g_rg_lib = library_open(g_db_path, g_art_path);
        if (!g_rg_lib) return;
        if (library_rg_next(g_rg_lib, &id, path, sizeof path) != 0) {
            library_close(g_rg_lib);
            g_rg_lib = NULL;
            done_at = now_s;
            return;
        }
        done_at = 0;
        g_rg_stop = 0;
        g_rg_running = 1;
        if (pthread_create(&g_rg_thread, NULL, rg_thread, NULL)) {
            g_rg_running = 0;
            library_close(g_rg_lib);
            g_rg_lib = NULL;
        }
    }
}

static void rescan(void)
{
    if (!g_lib) return;
    if (g_scan_state) { ui_toast("Already checking for new music", 1500); return; }
    scan_start();
    ui_toast("Checking for new music", 1500);
}

static int about(int i, char *label, size_t lc, char *value, size_t vc)
{
    char a[32], b[32];
    if (i == 0) {                       /* the credit (ui.h), a row of its own: never cut short */
        snprintf(label, lc, "%s", UI_ABOUT_CREDIT);
        value[0] = '\0';
        return 0;
    }
    switch (i - 1) {
    case 0: snprintf(label, lc, "Version"); snprintf(value, vc, "%s", FATHOM_VERSION); return 0;
    case 1: {
        struct utsname u;
        snprintf(label, lc, "Linux");
        if (uname(&u) == 0) snprintf(value, vc, "%s", u.release); else snprintf(value, vc, "-");
        return 0;
    }
    case 2: {
        char c[16];
        snprintf(label, lc, "Battery");
        if (!read_int_file(HW_BATTERY "capacity", a, sizeof a) &&
            !read_int_file(HW_BATTERY "voltage_now", b, sizeof b)) {
            if (!read_int_file(HW_BATTERY "temp", c, sizeof c))   /* tenths of a degree */
                snprintf(value, vc, "%d %%, %.2f V, %d \xC2\xB0" "C", atoi(a), atol(b) / 1e6, (atoi(c) + 5) / 10);
            else snprintf(value, vc, "%d %%, %.2f V", atoi(a), atol(b) / 1e6);
        } else snprintf(value, vc, "-");
        return 0;
    }
    case 3: {                    /* the percentage used up, counted by Fathom */
        int used = settings_get_int("battery.used", 0);
        snprintf(label, lc, "Charge cycles");
        snprintf(value, vc, "%d.%d", used / 100, used % 100 / 10);
        return 0;
    }
    case 4: {
        struct statvfs st;
        snprintf(label, lc, "SD card");
        if (statvfs(g_music ? g_music : CARD_DIR, &st) == 0) {
            double gb = 1024.0 * 1024 * 1024;
            char fr[16], all[16];
            snprintf(fr, sizeof fr, "%.1f", (double)st.f_bavail * st.f_frsize / gb);
            snprintf(all, sizeof all, "%.1f", (double)st.f_blocks * st.f_frsize / gb);
            tr_fill(value, vc, "%s of %s GB free", fr, all);
        } else {
            snprintf(value, vc, "-");
        }
        return 0;
    }
    case 5: {   /* set by the network (NTP, or an HTTPS answer) once Wi-Fi has connected, or by hand;
                 * shown in the time zone chosen there (clock.tz), UTC by default */
        int64_t now = (int64_t)time(NULL);
        snprintf(label, lc, "Clock");
        if (!netclock_unset_at(now)) {
            static const char *const MON[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
            int z = tz_find(settings_get("clock.tz", "UTC"));
            dt_t t;
            dt_split(tz_to_local(z, now), &t);
            char d[8], date[48];
            snprintf(d, sizeof d, "%d", t.day);
            snprintf(date, sizeof date, "%s %s", d, tr(MON[(t.mon + 11) % 12]));   /* (as the battery's days) */
            snprintf(value, vc, "%s %d, %02d:%02d%s", date, t.year, t.hour, t.min, z ? "" : " UTC");
        }
        else snprintf(value, vc, "Not set: Play sets it");   /* (or Wi-Fi) */
        return 0;
    }
    case 6: snprintf(label, lc, "Licence"); snprintf(value, vc, "GPL; fonts OFL"); return 0;
    }
    return -1;
}

/* ---- auto power-off: paused or stopped, untouched, on battery ---- */

static void idle_timer(lv_timer_t *t)
{
    (void)t;
    static uint32_t idle_since;
    int minutes = settings_get_int("power.auto_off", UI_AUTO_OFF_DEFAULT);
    player_now_t n = {0};
    if (g_player) player_now(g_player, &n);
    uint32_t now = ms_now();
    if (minutes <= 0 || n.playing || usb_powered() || share_busy() || updater_busy() || uninstaller_busy()) { idle_since = 0; return; }
    uint32_t since = g_last_input > idle_since ? g_last_input : idle_since;
    if (!idle_since) { idle_since = now; return; }
    if (now - since >= (uint32_t)minutes * 60000) power_off("Idle: powering off");
}

/* ---- the queue, kept across restarts (DATA/queue.txt) ---- */

static char g_queue_path[600];
static uint32_t g_queue_saved_seq;

static void queue_save(void)
{
    if (g_player && g_queue_path[0] && player_save(g_player, g_queue_path) == 0)
        g_queue_saved_seq = player_seq(g_player);
}

/* Every 10 s while playing (the position), and whenever the track changed. */
static void queue_timer(lv_timer_t *t)
{
    (void)t;
    static int ticks;
    if (++ticks % 30 == 0) clock_save();   /* every 5 min; written with the other settings */
    if (!g_player) return;
    player_now_t n;
    player_now(g_player, &n);
    if (n.playing || player_seq(g_player) != g_queue_saved_seq) queue_save();
}

/* ---- automatic update checks (Settings > About > Check automatically) ----
 * On unless the owner turns it off (UI_UPDATE_AUTO_DEFAULT). At most once a
 * day (a failed try again after an hour: update_auto_due), only on Wi-Fi
 * already joined and while the H2 rests: the screen off for a minute,
 * nothing playing unless it's charging, no download, network-drive transfer,
 * USB drive or DAC mode. It only asks GitHub: a newer release is said by a
 * toast the next time Power turns the screen on, and offered in Settings >
 * About. */

#define AUTO_UPDATE_REST_MS 60000

static void auto_update_poll(void)
{
    static long tried;               /* (since this start: a restart tries again at once) */
    char tag[32];
    int r = updater_quiet_result(tag, sizeof tag);
    if (r) {
        fprintf(stderr, "update: checked by itself: %s\n", r < 0 ? "no answer" : tag[0] ? tag : "up to date");
        if (r > 0) update_found(tag, 0);
    }
    if (!settings_get_int("update.auto", UI_UPDATE_AUTO_DEFAULT) || g_screen_on || ms_now() - g_last_input < AUTO_UPDATE_REST_MS) return;
    if (!wifi_connected() || share_busy() || updater_busy() || g_usb_drive || g_relay || g_charge_mode) return;
    player_now_t n = {0};
    if (g_player) player_now(g_player, &n);
    if (n.playing && !usb_powered()) return;
    long now = (long)time(NULL);
    if (!update_auto_due(now, settings_get_int("update.checked", 0), tried)) return;
    if (updater_check_quiet()) tried = now;
}

/* The battery history's note: the charge, and what drains it */
static void battery_log_poll(void)
{
    if (!g_batlog_on || g_bat_pct < 0) return;
    player_now_t n = {0};
    if (g_player) player_now(g_player, &n);
    int out = g_usb_card >= 0 ? BL_OUT_USB : on_bt_now() ? BL_OUT_BT : g_jack_lo ? BL_OUT_LINE : BL_OUT_WIRED;
    int f = (g_charging ? BL_CHARGING : 0) | (g_screen_on ? BL_SCREEN : 0) | (n.playing ? BL_PLAYING : 0) |
            (wifi_available() && wifi_on() ? BL_WIFI : 0) | (bt_available() && bt_on() ? BL_BT : 0) | out << BL_OUT_SHIFT;
    batlog_note(&g_batlog, mono_s(), g_bat_pct, f);
}

/* ---- the polls: one timer ----
 * Everything the player polls, on one 250 ms timer: LVGL's timers drift apart,
 * so a dozen of them (250, 300, 500 ms, 1, 10, 30 s) woke the CPU 30 to 40 times
 * a second at rest. Each poll keeps its period (the EQ's: 250 ms, was 300). */
static void polls(lv_timer_t *t)
{
    (void)t;
    static unsigned k;
    k++;
    screen_timer(NULL);
    usb_poll(NULL);            /* the prompt soon after plugging in */
    bt_poll(NULL);
    wifi_poll(NULL);
    update_poll(NULL);
    uninstall_poll();
    eq_poll(NULL);
    if (k % 2 == 0) {          /* 500 ms */
        usb_dac_poll(NULL);
        usb_dac_mode_poll(NULL);
        radio_poll(NULL);
        net_poll(NULL);
        scan_poll(NULL);
    }
    if (k % 40 == 0) auto_update_poll();   /* 10 s */
    if (k % 4 == 0) {          /* 1 s */
        every_second(NULL);
        now_publish(NULL);
        charge_poll(NULL);
        rg_poll(NULL);
    }
    if (k % 40 == 0) { poll_battery(NULL); battery_log_poll(); }   /* 10 s */
    if (k % 40 == 20) queue_timer(NULL);   /* 10 s */
    if (k % 120 == 0) idle_timer(NULL);    /* 30 s */
}

/* ---- first-run scan, with progress on screen ---- */

typedef struct { lv_obj_t *count, *path; uint32_t last; } scan_ui_t;

static void scan_progress(int done, int total, const char *path, void *ctx)
{
    scan_ui_t *s = ctx;
    uint32_t now = ms_now();
    if (now - s->last < 250 && done != total) return;   /* redraw at most 4x per second */
    s->last = now;
    char a[16], b[16], t[64];
    snprintf(a, sizeof a, "%d", done);
    snprintf(b, sizeof b, "%d", total);
    tr_fill(t, sizeof t, "%s of %s", a, b);
    if (total > 0) lv_label_set_text(s->count, t);
    else lv_label_set_text_fmt(s->count, "%d", done);
    const char *slash = path ? strrchr(path, '/') : NULL;
    lv_label_set_text(s->path, slash ? slash + 1 : path ? path : "");
    lv_refr_now(NULL);
}

static int scan_with_progress(library_t *lib, const char *music, const theme_t *t, const ui_fonts_t *f)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(t->bg), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);   /* (no theme to set it) */
    lv_obj_t *title = lv_label_create(scr);
    lv_obj_set_style_text_font(title, f->title, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(t->text), 0);
    lv_label_set_text(title, tr("Reading your library"));
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 16, 60);
    scan_ui_t s = {.count = lv_label_create(scr), .path = lv_label_create(scr)};
    lv_obj_set_style_text_font(s.count, f->body, 0);
    lv_obj_set_style_text_color(s.count, lv_color_hex(t->signal), 0);
    lv_obj_align(s.count, LV_ALIGN_TOP_LEFT, 16, 100);
    lv_obj_set_style_text_font(s.path, f->meta, 0);
    lv_obj_set_style_text_color(s.path, lv_color_hex(t->text_muted), 0);
    lv_obj_set_width(s.path, 288);
    lv_label_set_long_mode(s.path, LV_LABEL_LONG_DOT);
    lv_obj_align(s.path, LV_ALIGN_TOP_LEFT, 16, 136);
    lv_label_set_text(s.count, "");
    lv_label_set_text(s.path, music);
    lv_obj_t *prev = lv_screen_active();
    lv_screen_load(scr);
    lv_refr_now(NULL);

    uint32_t t0 = ms_now();
    int n = library_scan(lib, music, scan_progress, &s);
    fprintf(stderr, "library: %d tracks in %.1f s\n", n, (ms_now() - t0) / 1000.0);

    lv_screen_load(prev);
    lv_obj_delete(scr);
    return n;
}

/* What's new (fathom/assets/whatsnew.txt, from the release notes' summary by
 * make-release.sh: the version, then one point per line): shown once, at the
 * first start of a new version over a library that was already there (an
 * update, not a fresh install), and kept for Settings > About. A file left by
 * another version (an update by hand that missed it) is ignored. */
static void whats_new_setup(int had_music)
{
    char p[700], ver[32] = "", text[1024];
    snprintf(p, sizeof p, "%s/whatsnew.txt", g_assets_dir);
    FILE *f = fopen(p, "re");
    if (!f) return;
    if (fgets(ver, sizeof ver, f)) ver[strcspn(ver, "\r\n")] = '\0';
    size_t n = fread(text, 1, sizeof text - 1, f);
    text[n] = '\0';
    fclose(f);
    if (strcmp(ver, FATHOM_VERSION) || !text[0]) return;
    char seen[32];
    snprintf(seen, sizeof seen, "%s", settings_get("whatsnew.seen", ""));
    ui_set_whats_new(ver, text, had_music && strcmp(seen, ver) != 0);
    if (strcmp(seen, ver)) {
        settings_set("whatsnew.seen", ver);
        settings_save();
    }
    fprintf(stderr, "whats new: %s%s\n", ver, had_music && strcmp(seen, ver) ? " (shown)" : "");
}

int main(int argc, char **argv)
{
    const char *music = "/mnt/sd/Music", *data = "/mnt/sd/fathom", *assets = "/mnt/sd/fathom/assets";
    int force_scan = 0, theme = 0, theme_given = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--music") && i + 1 < argc) music = argv[++i];
        else if (!strcmp(argv[i], "--data") && i + 1 < argc) data = argv[++i];
        else if (!strcmp(argv[i], "--assets") && i + 1 < argc) assets = argv[++i];
        else if (!strcmp(argv[i], "--theme") && i + 1 < argc) { theme = atoi(argv[++i]); theme_given = 1; }
        else if (!strcmp(argv[i], "--scan")) force_scan = 1;
        else { fprintf(stderr, "usage: %s [--music DIR] [--data DIR] [--assets DIR] [--scan] [--theme N]\n", argv[0]); return 2; }
    }
    /* SQLite's temporary files (a scan's groups are made aside in temp
     * tables) on the card, not in /tmp: that's RAM. (Before SQLite starts.) */
    LAT("main: start");
    setenv("SQLITE_TMPDIR", data, 0);
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    signal(SIGUSR1, on_usr1);
    signal(SIGUSR2, on_usr2);

    char settings_path[600];
    snprintf(settings_path, sizeof settings_path, "%s/settings.ini", data);
    settings_load(settings_path);
    snprintf(g_data_dir, sizeof g_data_dir, "%s", data);
    hook_language_set(settings_get("display.language", ""));   /* before anything is drawn (the first scan's screen) */
    /* An update's install cut short (power lost): finished before Wi-Fi and
     * Bluetooth start from the card, then a restart into its kernel, so the kernel and
     * its Wi-Fi modules are never from two releases. An unfinished download
     * is removed. (Nothing here goes near the network.) */
    if (update_resume(CARD_DIR, UPDATE_STAGE) == 1) {
        sync();
        if (system("/sbin/reboot") == 0) return 0;
        fprintf(stderr, "reboot failed\n");
    }
    clock_restore();
    /* HTTPS (radio): certificates checked against the bundle on the card */
    ff_set_ca_file("/mnt/sd/fathom/share/ca-certificates.crt");
    netclock_set_fetcher(clock_from_https);   /* (the time before a secure connection, when unset) */
    ff_set_before_net(before_net);

    lv_init();
    lv_tick_set_cb(ms_now);
    theme_t *themes = g_themes;
    snprintf(g_user_themes, sizeof g_user_themes, "%s/themes", data);   /* contributed themes on the card */
    snprintf(g_assets_dir, sizeof g_assets_dir, "%s", assets);
    int n_themes = ui_load_themes(assets, g_user_themes, themes, UI_MAX_THEMES);
    const ui_fonts_t *base_fonts = n_themes > 0 ? ui_theme_fonts(&themes[0]) : NULL;
    if (!base_fonts) { fprintf(stderr, "no usable theme or font in %s\n", assets); return 1; }
    if (!theme_given) {                 /* the last theme picked, by name */
        const char *name = settings_get("ui.theme", "");
        for (int i = 0; i < n_themes; i++)
            if (!strcmp(themes[i].name, name)) theme = i;
    }
    if (theme < 0 || theme >= n_themes || themes[theme].status == THEME_ERROR) theme = 0;
    const ui_fonts_t *theme_fonts = ui_theme_fonts(&themes[theme]);
    if (!theme_fonts) { theme = 0; theme_fonts = base_fonts; }
    if (!DISP(create)(DISP_DEV)) return 1;
    if (!input_open()) fprintf(stderr, "input: no devices\n");

    /* The boot logo while the library opens (a second or two on the card): the
     * same picture the kernel showed, in the same place, so the screen holds
     * still from power-on. Without the file, "Fathom" in the theme's colours. */
    lv_obj_t *splash;
    {
        lv_obj_t *scr = splash = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(scr, lv_color_hex(themes[theme].bg), 0);
        lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
        const lv_image_dsc_t *logo = ui_logo();
        keep_logo_for_exit(logo);
        if (logo && logo->header.w <= UI_W && logo->header.h <= UI_H) {
            lv_obj_t *img = lv_image_create(scr);
            lv_image_set_src(img, logo);
            lv_obj_center(img);
        } else {
            lv_obj_t *l = lv_label_create(scr);
            lv_label_set_text(l, "Fathom");
            lv_obj_set_style_text_font(l, theme_fonts->title, 0);
            lv_obj_set_style_text_color(l, lv_color_hex(themes[theme].text), 0);
            lv_obj_center(l);
        }
        lv_screen_load(scr);
        lv_refr_now(NULL);
        LAT("main: splash");
    }

    /* Library: database and cover cache live next to the music, on the card. */
    char db[512], art[512];
    mkdir(data, 0755);
    snprintf(db, sizeof db, "%s/library.db", data);
    snprintf(art, sizeof art, "%s/art", data);
    mkdir(art, 0755);
    tags_set_exact_mp3_length(0);   /* SD card: don't read whole MP3s for their length */
    tags_set_max_picture(FATHOM_ART_MAX_FILE);   /* embedded covers held in RAM */
    library_t *lib = library_open(db, art);
    LAT("main: library open");
    if (!lib) { fprintf(stderr, "cannot open %s\n", db); return 1; }
    char pl_dir[600];
    snprintf(pl_dir, sizeof pl_dir, "%s/playlists", data);   /* your own playlists: never in the music folder */
    library_set_playlist_dir(lib, pl_dir);
    char found[600];
    snprintf(found, sizeof found, "%s/covers", data);   /* (every library handle: scans use them too) */
    library_set_found_covers(found);
    fast_walk_setup(lib, music);
    lib_list_t songs = {0};
    int have = library_list(lib, LIB_SONGS, 0, &songs);
    library_list_free(&songs);
    char usb_msg[128];
    /* back from USB drive mode, or the card taken out and put back (fathom-sdcard) */
    int back_from_usb = take_tmp_file("/tmp/fathom-rescan", usb_msg, sizeof usb_msg) == 0;
    snprintf(g_db_path, sizeof g_db_path, "%s", db);
    snprintf(g_art_path, sizeof g_art_path, "%s", art);
    snprintf(g_pl_path, sizeof g_pl_path, "%s", pl_dir);
    /* Nothing to show yet: scan first, with progress. (After USB drive mode,
     * new music is looked for in the background, once the UI is up.) */
    if (force_scan || have <= 0)
        scan_with_progress(lib, music, &themes[theme], theme_fonts);

    /* The engine's output: a router to the ALSA output (the H2's DAC, USB DACs)
     * or Bluetooth (bluez-alsa, when it's on the card). */
    static audio_output_t out, alsa_out, bt_out;
    int out_is_alsa = out_alsa_create(&alsa_out, NULL) == 0;
    if (out_is_alsa) {
        int have_bt = bt_available() && out_bt_create(&bt_out, "/mnt/sd/fathom/bt") == 0;
        out_router_create(&out, &alsa_out, have_bt ? &bt_out : NULL);
    } else {
        fprintf(stderr, "audio: no sound card, using a silent clock\n");
        out_clock_create(&out);
    }
    volume_init(data);
    engine_t *eng = engine_new(&out);
    g_eng = out_is_alsa ? eng : NULL;   /* USB DACs need the ALSA output to switch */
    player_t *player = eng ? player_new(lib, eng) : NULL;
    g_player = player;
    snprintf(g_queue_path, sizeof g_queue_path, "%s/queue.txt", data);
    int resumed = player && player_restore(player, g_queue_path) == 0;   /* paused where it was */
    if (player) g_queue_saved_seq = player_seq(player);

    lv_obj_t *main_scr = lv_obj_create(NULL);   /* the UI gets a fresh screen; the splash goes */
    lv_screen_load(main_scr);
    lv_obj_delete(splash);
    ui_logo_release();
    LAT("main: player ready");
    ui_init(main_scr, themes, n_themes, base_fonts, 1, lib, player);
    LAT("main: ui_init");
    g_level = settings_get_int("display.brightness", UI_BRIGHTNESS_DEFAULT);
    if (g_level < 1 || g_level > UI_BRIGHTNESS_LEVELS) g_level = UI_BRIGHTNESS_DEFAULT;
    g_timeout = settings_get_int("display.timeout", UI_TIMEOUT_DEFAULT);
    static ui_hooks_t hooks = {
        .brightness = hook_brightness, .screen_timeout = hook_timeout, .fonts = ui_theme_fonts,
        .icon = ui_theme_icon,
        .dac_filter_name = dac_filter_name, .dac_filter_next = dac_filter_next,
        .sound_value = hook_sound_value, .sound_adjust = hook_sound_adjust,
        .pure_toggle = hook_pure_toggle, .signal_path = hook_signal_path,
        .transport_toggle = hook_transport_toggle, .battery_info = hook_battery_info,
        .battery_report = hook_battery_report,
        .languages = hook_languages, .language_set = hook_language_set,
        .rescan = rescan, .restart = restart_player, .usb_drive = usb_drive, .usb_dac = usb_dac_mode, .about = about,
        .clock_set = clock_set_by_hand,
        .eq_list = hook_eq_list, .eq_set = hook_eq_set, .eq_get = hook_eq_get, .eq_put = hook_eq_put,
        .eq_preview = hook_eq_preview, .eq_remove = hook_eq_remove,
        .theme_save = hook_theme_save, .themes_reload = hook_themes_reload, .theme_remove = hook_theme_remove,
    };
    if (bt_available() && out_is_alsa) {
        hooks.bt_state = bt_ui_state;
        hooks.bt_devices = bt_ui_devices;
        hooks.bt_do = bt_ui_toggle_or_do;
        bt_init(settings_get_int("bluetooth.on", 0), settings_get("bluetooth.last", ""));
    }
    if (wifi_available()) {
        hooks.wifi_state = wifi_ui_state;
        hooks.wifi_networks = wifi_ui_networks;
        hooks.wifi_do = wifi_ui_do;
        wifi_init(settings_get_int("wifi.on", 0));
        http_set_version(FATHOM_VERSION);
        updater_init(CARD_DIR, FATHOM_VERSION, settings_get("update.api", ""));   /* (a test server's) */
        if (!update_is_newer(settings_get("update.found", ""), FATHOM_VERSION) && settings_get("update.found", "")[0])
            settings_set("update.found", "");    /* installed since */
        hooks.update_state = updater_ui_state;
        hooks.update_do = hook_update_do;
        if (share_available()) {
            hooks.share_state = share_ui_state;
            hooks.share_do = hook_share_do;
            int on = settings_get_int("share.on", 0);
            share_init(music, on, share_password(0));
        }
    }
    if (ssh_available()) {           /* root's password is set whether SSH is on or off */
        hooks.ssh_state = ssh_ui_state;
        hooks.ssh_do = hook_ssh_do;
        ssh_init(settings_get_int("ssh.on", 0), made_up_password("ssh.password", 0));
    }
    LAT("main: net, ssh");
    uninstaller_init(CARD_DIR);
    hooks.uninstall_state = uninstaller_ui_state;
    hooks.uninstall_do = hook_uninstall_do;
    if (player) {
        hooks.radio_stations = hook_radio_stations;
        hooks.radio_ready = hook_radio_ready;
        hooks.radio_add = hook_radio_add;
        hooks.radio_remove = hook_radio_remove;
        char pods[600];
        snprintf(pods, sizeof pods, "%s/podcasts", data);
        if (podsync_start(pods) == 0) hooks.podcast_opml = hook_podcast_opml;
        char covers[600];                /* covers found online (Find cover): on the card, never beside the music */
        snprintf(covers, sizeof covers, "%s/covers", data);
        cf_start(covers);
    }
    LAT("main: podcasts, covers");
    g_lib = lib;
    g_music = music;
    int filter = settings_get_int("sound.dac_filter", -1);   /* the DAC filter chosen last time */
    if (filter >= 0) mixer_enum_set(HW_CTL, DAC_FILTER, filter);
    ui_set_hooks(&hooks);
    route_apply(0);                    /* the output's name: the first route_apply() came before the UI */
    LAT("main: hooks");
    ui_set_theme(theme);
    LAT("main: theme");
    whats_new_setup(have > 0);
    screen_set(1);
    g_fast = lv_timer_create(every_100ms, 100, NULL);
    free_space_warm();
    batlog_open(&g_batlog, CARD_DIR "/fathom/battery.log", time(NULL), mono_s());   /* (the clock restored) */
    g_batlog_on = 1;
    poll_battery(NULL);
    usb_dac_poll(NULL);
    g_usb_was = usb_on_computer();   /* already plugged in at start: don't ask */
    LAT("main: screen, battery");
    eq_apply();
    LAT("main: eq");
    if (resumed) ui_show("nowplaying");
    LAT("main: shown");
    {
        char why[32];
        power_on_reason(why, sizeof why);
        poll_battery(NULL);                  /* g_charging, now */
        g_charge_mode = !strncmp(why, "vbus", 4) && g_charging && access(STARTED_MARK, F_OK) != 0;
        fprintf(stderr, "power on: %s%s\n", why, g_charge_mode ? " (charging mode)" : "");
        if (g_charge_mode) charge_screen_show();
        else started_mark();
    }
    lv_timer_create(polls, 250, NULL);
    levelling_apply();                   /* (off in Pure mode) */
    if (take_tmp_file("/tmp/fathom-usbdisk.msg", usb_msg, sizeof usb_msg) == 0) ui_toast(usbdisk_text(usb_msg), 3000);
    else if (access(music, F_OK) != 0) {
        fprintf(stderr, "library: no music folder (%s)\n", music);
        ui_toast("No Music folder on the card", 5000);
    }
    boot_markers();                      /* (up: an update's kernel worked) */
    if (uninstaller_pending()) ui_toast("Uninstall Fathom is ready: Settings > About", 5000);
    if (back_from_usb && have > 0 && !force_scan) scan_start();
    /* a library from before some tags (composers): read them in the background, once */
    else if (have > 0 && !force_scan && library_needs_tags(lib)) {
        fprintf(stderr, "library: reading tags added since the last scan\n");
        scan_start();
    }
    /* albums left without a cover by a version that looked in fewer places: once */
    else if (have > 0 && !force_scan && library_needs_art(lib)) {
        fprintf(stderr, "library: looking again for missing covers\n");
        scan_start();
    }

    LAT("main: loop");
    struct pollfd pfd[INPUT_MAX_POLL];
    while (!g_quit) {
        input_event_t ev;
        while (input_next(&ev, ms_now())) {
            fast_tick_now();                         /* a key may start something to follow */
            if (ev.kind == IN_JACKS) {
                jacks_changed(ev.hp, ev.lo);
            } else if (ev.kind == IN_VOLUME) {
                volume_step(ev.ui_key);              /* works with the screen off too */
                if (g_screen_on) screen_activity();
            } else if (ev.kind == IN_POWER_OFF) {
                power_off("Powering off");
            } else if (ev.kind == IN_POWER && g_charge_mode) {
                charge_mode_end();                   /* start as usual */
            } else if (g_charge_mode && ev.kind == IN_UI) {
                charge_screen_show();                /* nothing plays in charging mode */
            } else if (g_charge_mode && ev.kind == IN_MEDIA) {
                ;                                    /* nor from headphones: Bluetooth ones send Play as they connect */
            } else if (ev.kind == IN_MEDIA) {
                media_key(ev.ui_key);                /* the headphones' buttons; the screen stays as it is */
            } else if (ev.kind == IN_POWER) {
                int was_on = g_screen_on;
                LAT("ui: power, screen %s", was_on ? "off" : "on");
                screen_set(!g_screen_on);
                LAT("ui: power handled");
                if (!was_on && wifi_available()) update_announce();
            } else if (g_relay && ev.kind == IN_UI) {
                /* USB DAC mode: Back ends it; Play, Prev and Next are the
                 * computer's media keys (also with the screen off) */
                if (g_screen_on) screen_activity();
                if (g_screen_on && ev.ui_key == UI_KEY_BACK) usb_dac_mode_end("USB DAC mode ended");
                else if (ev.ui_key == UI_KEY_OK || ev.ui_key == UI_KEY_PLAY) usb_dac_key(HID_PLAY);
                else if (ev.ui_key == UI_KEY_NEXT) usb_dac_key(HID_NEXT);
                else if (ev.ui_key == UI_KEY_PREV) usb_dac_key(HID_PREV);
            } else if (ev.kind == IN_UI && g_screen_on) {
                screen_activity();
                LAT("ui: key %d", ev.ui_key);
                ui_key((ui_key_t)ev.ui_key);
                LAT("ui: key %d handled", ev.ui_key);
            } else if (ev.kind == IN_UI) {
                /* screen off: only the media keys act; Power wakes the screen */
                if (ev.ui_key == UI_KEY_OK) ui_key(UI_KEY_PLAY);
                else if (ev.ui_key == UI_KEY_PREV || ev.ui_key == UI_KEY_NEXT ||
                         ev.ui_key == UI_KEY_SEEK_BACK || ev.ui_key == UI_KEY_SEEK_FWD) ui_key((ui_key_t)ev.ui_key);
            }
        }
        uint32_t wait = lv_timer_handler();
        if (g_shot) { g_shot = 0; screen_shot(); }
        if (g_usb_req) {             /* tests: kill -USR2; /tmp/fathom-usb-mode "dac" or "normal" */
            char m[16] = "";
            g_usb_req = 0;
            take_tmp_file("/tmp/fathom-usb-mode", m, sizeof m);
            if (!strcmp(m, "dac")) usb_dac_mode();
            else if (!strcmp(m, "normal")) usb_dac_mode_end("USB DAC mode ended");
            else usb_drive();
        }
        /* Asleep until the next timer or a key (input_timeout(): long presses,
         * repeats); other threads' news is polled by the timers. */
        int lp = input_timeout(ms_now());
        int timeout = wait > 1000 ? 1000 : (int)wait;
        if (lp >= 0 && lp < timeout) timeout = lp;
        int npfd = input_pollfds(pfd, INPUT_MAX_POLL);   /* devices come and go (headphones) */
        poll(pfd, (nfds_t)npfd, timeout);
    }

    fprintf(stderr, "fathom-player: exiting%s\n", g_usb_drive ? " for USB drive mode" : "");
    scan_stop();
    rg_stop();
    share_shutdown();
    if (g_relay) usb_dac_mode_end("Stopping");
    if (g_usb_drive) {       /* this screen stays up while the card is lent */
        screen_set(1);
        ui_message_screen("USB drive", "Eject the H2 on your computer, or unplug the cable, to go back to your music.");
        lv_refr_now(NULL);
    } else if (!g_powering_off) {
        DISP(show_last_frame)();
    }
    if (g_batlog_on) batlog_flush(&g_batlog);   /* (a restart or USB drive mode: the H2 stays on) */
    clock_save();
    settings_save();
    ui_save_state();       /* (where a podcast episode was left) */
    podsync_stop();        /* (a download under way: dropped, its .part removed) */
    cf_stop();
    queue_save();
    if (player) player_free(player);
    if (eng) engine_free(eng);
    library_close(lib);
    return g_usb_drive ? USB_DRIVE_EXIT : 0;
}
