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
#include "main_int.h"

/* What the player's pieces share (main_int.h) */
library_t *g_lib;
player_t *g_player;
uac_relay_t *g_relay;                   /* USB DAC mode is on */
int g_charging;                         /* on power: plugged in (poll_battery) */
int g_screen_on = 1;
int g_usb_drive;                        /* exiting for USB drive mode */
volatile int g_scan_state;              /* the background scan: 0 idle, 1 running, 2 finished */
char g_db_path[512], g_art_path[512];   /* the library and its covers */
char g_data_dir[512];                    /* DATA (--data) */
const char *g_music;                    /* the music folder (--music) */

volatile sig_atomic_t g_quit;
/* A stop asked for from outside (init's stop script, a restart): the signal
 * may be handed to any of the player's threads, and the main loop, asleep in
 * poll() for up to a second with nothing going on, only saw g_quit at its
 * next timer: a second of every such stop. The handler writes to a pipe the
 * main loop's poll() watches. */
int g_quit_pipe[2] = {-1, -1};
static void on_signal(int s)
{
    (void)s;
    g_quit = 1;
    if (g_quit_pipe[1] >= 0) {
        char c = 1;
        ssize_t w = write(g_quit_pipe[1], &c, 1);
        (void)w;
    }
}

/* The boot logo as the player's last frame: on a crash too, so the screen shows
 * the logo (not a frozen or black screen) until S90fathom has it running again;
 * the display driver never sends the console's all-black frames. */
static uint16_t *g_logo565;
int g_powering_off;           /* its message stays on screen instead */

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
volatile sig_atomic_t g_shot;
static volatile sig_atomic_t g_usb_req;
static void on_usr1(int s) { (void)s; g_shot = 1; }
/* kill -USR2 $(pidof fathom-player): USB drive mode, as if chosen (development, tests) */
static void on_usr2(int s) { (void)s; g_usb_req = 1; }

void screen_shot(void)
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

static void every_second(lv_timer_t *t) { (void)t; ui_tick_1s(); }

/* Clean power off: init stops the player (S90fathom), unmounts the card, and the
 * battery driver sets the PMIC's power-off bit at the end. */

/* The player followed 10 times a second while it has something to follow
 * (playing, starting, the sleep timer, a stream reconnecting); otherwise once a
 * second, until a key (fast_tick_now()). Paused, that's 9 wakeups a second fewer.
 *
 * With the screen off there's nobody to show it to, and a song playing along
 * needs nothing from here for minutes: the engine has what follows lined up
 * (8 tracks or 30 s ahead). Once a second then too (tick_can_wait), and the
 * polls with it (polls_pace: they're 4 a second otherwise), which leaves two
 * wakeups a second of the 14. Not while something that has to come on time
 * is near, when it's 10 times a second as with the screen on:
 *   - for 3 s after a key or a headphone button (what it started: a track,
 *     a seek, one that can't be played and is passed over), and after the
 *     track changed (the next ones lined up; one that fails);
 *   - in a track's last 5 s, or when its length isn't known (the change to
 *     the next, the end of the queue, a new shuffle cycle, "stop after");
 *   - in the sleep timer's last 12 s (it fades over 10);
 *   - the radio and a streamed episode (a stream that drops is reconnected
 *     from here), and anything not plainly playing.
 * Power's wake comes as a key, and screen_set() sets both paces back. */
#define FAST_HOLD_MS 3000
static lv_timer_t *g_fast, *g_polls;
static uint32_t g_fast_ms = 100, g_polls_ms = 250;
static uint32_t g_fast_until;         /* 10 times a second until then... */
static int g_fast_hold;               /* ...while this says so (the clock wraps: not left to g_fast_until alone) */

static void fast_hold(void)
{
    g_fast_hold = 1;
    g_fast_until = clock_ms32() + FAST_HOLD_MS;
}

static int fast_held(void)
{
    if (g_fast_hold && (int32_t)(clock_ms32() - g_fast_until) >= 0) g_fast_hold = 0;
    return g_fast_hold;
}

/* The player needn't be looked at for a second: the screen off, a file
 * playing, nothing near that has to come on time (above). */
static int tick_can_wait(void)
{
    static uint32_t seq;
    if (g_screen_on || !g_player) return 0;
    uint32_t s = player_seq(g_player);
    if (s != seq) { seq = s; fast_hold(); }
    if (fast_held()) return 0;
    player_now_t n;
    player_now(g_player, &n);
    if (!n.playing || n.radio || n.connecting || n.podcast_stream) return 0;
    if (n.sleep_s >= 0 && n.sleep_s <= 12) return 0;
    if (!n.len_ms || n.pos_ms + 5000 >= n.len_ms) return 0;
    return 1;
}

static void every_100ms(lv_timer_t *t)
{
    uint32_t ms = ui_tick() && !tick_can_wait() ? 100 : 1000;
    if (ms != g_fast_ms) lv_timer_set_period(t, g_fast_ms = ms);
}

/* The polls' pace (polls(), below): 4 a second; once a second with the
 * screen off, where all they'd do sooner is notice a cable, a DAC or a pair
 * of headphones 0.75 s earlier. Not in USB DAC mode, nor while fast_held()
 * (a key; an output that just came or went: what follows it, its EQ, at the
 * usual pace). Each poll keeps its period either way (polls() counts in
 * quarters of a second). */
static void polls_pace(void)
{
    if (!g_polls) return;
    uint32_t ms = g_screen_on || g_relay || fast_held() ? 250 : 1000;
    if (ms == g_polls_ms) return;
    lv_timer_set_period(g_polls, g_polls_ms = ms);
    if (ms == 250) lv_timer_ready(g_polls);       /* (back to it: a look at once) */
}

static void fast_tick_now(void)
{
    fast_hold();
    if (g_fast && g_fast_ms != 100) {
        lv_timer_set_period(g_fast, g_fast_ms = 100);
        lv_timer_ready(g_fast);
    }
    polls_pace();
}

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


/* The network can be used: Wi-Fi has an address, or (on the bench, over USB)
 * a proxy is set: http_proxy=http://10.42.0.2:PORT. */
int net_up(void) { return wifi_connected() || getenv("http_proxy") != NULL; }

/* Why it can't, for what was asked while it couldn't */
const char *net_why(void)
{
    return !wifi_on() ? "Wi-Fi is off: Settings > Wi-Fi" : "Not connected yet: Settings > Wi-Fi";
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
    radio_net_feed();   /* (first: what the UI kept for the connection starts with the network known to be there) */
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
        rng_seed(&r, rng_os_seed(), (uint64_t)clock_ms32());
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

/* Twice a second: the drive follows Wi-Fi; music copied to it (or moved,
 * deleted) is picked up once the copying has stopped for a while; and the
 * clock, when NTP (fathom-ntp) or an HTTPS answer has just set it, is kept
 * for the next start. */
static int g_share_rescan;

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
    clock_poll();
}

/* ---- screen: brightness levels, timeout and dimming ----
 *
 * Brightness is one of UI_BRIGHTNESS_LEVELS steps onto the PWM (level_pwm),
 * never fully dark while the screen is on. With a timeout set, the screen
 * dims to 30 % five seconds before it turns off; any key undims it. While the
 * screen is off, play/pause, Prev/Next and volume still work, and only Power
 * turns it back on (no hold switch on the H2: nothing happens in a pocket).
 */

static int g_dimmed;
static int g_level = UI_BRIGHTNESS_DEFAULT, g_timeout = UI_TIMEOUT_DEFAULT;
uint32_t g_last_input;

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

void screen_set(int on)
{
    if (on != g_screen_on) ui_set_awake(on);
    if (on && !g_screen_on) wifi_wake();                     /* (out and back: a saved network may be in range) */
    wifi_screen(on);                                         /* (dark: the signal's strength isn't asked for) */
    if (!on && g_screen_on) library_release_memory(g_lib);   /* the database's cache: nothing browses meanwhile */
    g_screen_on = on;
    input_set_screen(on);              /* (Power wakes on its press, and turns off on its release) */
    if (on) fast_tick_now();           /* the player and the polls at their usual pace again... */
    else polls_pace();                 /* ...or the polls once a second (the player's tick finds out by itself) */
    g_dimmed = 0;
    g_last_input = clock_ms32();
    screen_apply();
}

void screen_activity(void)
{
    g_last_input = clock_ms32();
    if (g_dimmed) { g_dimmed = 0; screen_apply(); }
}

static void hook_brightness(int level) { g_level = level; g_dimmed = 0; screen_apply(); }
static void hook_timeout(int seconds) { g_timeout = seconds; g_last_input = clock_ms32(); }

int usb_powered(void)
{
    char s[8];
    return sysfile_line(USB_ONLINE, s, sizeof s) == 0 && s[0] == '1';
}

static void screen_timer(lv_timer_t *t)
{
    (void)t;
    int timeout = g_transport && (g_timeout <= 0 || g_timeout > TRANSPORT_TIMEOUT_S) ? TRANSPORT_TIMEOUT_S : g_timeout;
    if (!g_screen_on || timeout <= 0) return;
    if (!g_transport && settings_get_int("display.on_charging", UI_ON_CHARGING_DEFAULT) && usb_powered()) { g_last_input = clock_ms32(); return; }
    uint32_t idle = clock_ms32() - g_last_input, limit = (uint32_t)timeout * 1000;
    if (idle >= limit) screen_set(0);
    else if (!g_dimmed && limit > 5000 && idle >= limit - 5000 && settings_get_int("display.dim", UI_DIM_DEFAULT)) {
        g_dimmed = 1;
        screen_apply();
    }
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

/* The Bluetooth headphones' buttons (IN_MEDIA_*): playback, wherever the UI is
 * and with the screen off; in USB DAC mode, the computer's media keys. */
static void media_key(int k)
{
    g_last_input = clock_ms32();                           /* use, for the idle power-off */
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
    case IN_MEDIA_NEXT: ui_media_key(UI_KEY_NEXT); break;   /* (the song's, on a typing screen too) */
    case IN_MEDIA_PREV: ui_media_key(UI_KEY_PREV); break;
    default: break;
    }
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
        if (!sysfile_line(HW_BATTERY "capacity", a, sizeof a) &&
            !sysfile_line(HW_BATTERY "voltage_now", b, sizeof b)) {
            if (!sysfile_line(HW_BATTERY "temp", c, sizeof c))   /* tenths of a degree */
                snprintf(value, vc, "%d %%, %.2f V, %d \xC2\xB0" "C", atoi(a), atol(b) / 1e6, (atoi(c) + 5) / 10);
            else snprintf(value, vc, "%d %%, %.2f V", atoi(a), atol(b) / 1e6);
        } else snprintf(value, vc, "-");
        return 0;
    }
    case 3: {                    /* the percentage used up, counted by Fathom */
        int used = settings_get_int("battery.used", 0);
        snprintf(label, lc, "Charge cycles");
        snprintf(value, vc, "%d", (used + 50) / 100);    /* (a whole number: "0.0" read oddly) */
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
        if (!netclock_unset_at(now)) ui_clock_text(now, value, vc);
        else snprintf(value, vc, "Not set: <Play> sets it");   /* (or Wi-Fi) */
        return 0;
    }
    case 6: snprintf(label, lc, "Licence"); snprintf(value, vc, "GPL; fonts OFL"); return 0;
    }
    return -1;
}

/* ---- auto power-off: paused or stopped, untouched, on battery ----
 * Not while something is still at work for the owner: the network drive, an
 * update, a podcast being downloaded (or waiting its turn), a scan of the
 * library (those two went unseen: the H2 switched off in the middle of them). */

static void idle_timer(lv_timer_t *t)
{
    (void)t;
    static uint32_t idle_since;
    int minutes = settings_get_int("power.auto_off", UI_AUTO_OFF_DEFAULT);
    player_now_t n = {0};
    if (g_player) player_now(g_player, &n);
    podsync_status_t pods;
    podsync_status(&pods);
    uint32_t now = clock_ms32();
    if (minutes <= 0 || n.playing || usb_powered() || share_busy() || updater_busy() || packs_busy() || uninstaller_busy() ||
        pods.busy || pods.queued || g_scan_state) { idle_since = 0; return; }
    uint32_t since = g_last_input > idle_since ? g_last_input : idle_since;
    if (!idle_since) { idle_since = now; return; }
    if (now - since >= (uint32_t)minutes * 60000) power_off("Idle: powering off");
}

/* ---- the queue, kept across restarts (DATA/queue.txt) ---- */

static char g_queue_path[600];
static uint32_t g_queue_saved_seq;

void queue_save(void)
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
    if (ticks % 6 == 3) card_writable_check();   /* (a minute apart, from 30 s in) */
    if (ticks % 6 == 0) card_full_check();       /* (the same, from a minute in: its toast not over the other's) */
    if (!g_player) return;
    player_now_t n;
    player_now(g_player, &n);
    if (n.playing || player_seq(g_player) != g_queue_saved_seq) queue_save();
}

/* ---- the polls: one timer ----
 * Everything the player polls, on one 250 ms timer: LVGL's timers drift apart,
 * so a dozen of them (250, 300, 500 ms, 1, 10, 30 s) woke the CPU 30 to 40 times
 * a second at rest. Each poll keeps its period (the EQ's: 250 ms, was 300).
 * With the screen off the timer is a 1 s one (polls_pace): k, which counts
 * quarters of a second, then goes on to the next whole second each time, so
 * the 10 s and 30 s polls keep their periods and those under a second come
 * once a second. */
/* (timed for the cover flow's measurements, ui_timing: what on this thread makes a frame late) */
#define TIMED(what, call) do { long t0_ = ui_timing() ? (long)clock_us() : 0; call; if (t0_) ui_took(what, (long)clock_us() - t0_); } while (0)

static void polls(lv_timer_t *t)
{
    (void)t;
    static unsigned k;
    int usb = g_usb_card, bt = g_bt_routed[0] != '\0';
    if (g_polls_ms == 1000) k = (k | 3) + 1;
    else k++;
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
        TIMED("the second's checks", every_second(NULL); now_publish(NULL); charge_poll(NULL); rg_poll(NULL));
    }
    if (k % 40 == 0) { TIMED("the battery's reading and log", poll_battery(NULL); battery_log_poll()); }   /* 10 s */
    if (k % 40 == 20) TIMED("saving the queue's place", queue_timer(NULL));   /* 10 s */
    if (k % 120 == 0) idle_timer(NULL);    /* 30 s */
    /* a USB DAC or Bluetooth headphones just came or went: the usual pace for what follows (the EQ for it) */
    if (usb != g_usb_card || bt != (g_bt_routed[0] != '\0')) fast_hold();
    polls_pace();              /* (the screen went off, a key's 3 s are over, USB DAC mode began) */
}

/* The stop marked clean, for the next start (it checks the whole library
 * otherwise): on the card before returning. */
void mark_clean_stop(void)
{
    char mark[600];
    snprintf(mark, sizeof mark, "%s/stopped-cleanly", g_data_dir);
    int fd = open(mark, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
    if (fd >= 0) { fsync(fd); close(fd); }
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
    if (argc == 2 && !strcmp(argv[1], "--reader")) {
        /* The player's helper for reading files (library/reader.h), started
         * by the player itself: nothing of the player is set up here. The
         * same limits as its own readers (below). */
        tags_set_exact_mp3_length(0);
        tags_set_max_picture(FATHOM_ART_MAX_FILE);
        return reader_serve(player_reader_do);
    }
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
    if (pipe(g_quit_pipe) == 0) {
        for (int i = 0; i < 2; i++) {
            fcntl(g_quit_pipe[i], F_SETFD, FD_CLOEXEC);
            fcntl(g_quit_pipe[i], F_SETFL, fcntl(g_quit_pipe[i], F_GETFL) | O_NONBLOCK);
        }
    } else g_quit_pipe[0] = g_quit_pipe[1] = -1;
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
    lv_tick_set_cb(clock_ms32);
    theme_t *themes = g_themes;
    snprintf(g_user_themes, sizeof g_user_themes, "%s/themes", data);   /* contributed themes on the card */
    snprintf(g_assets_dir, sizeof g_assets_dir, "%s", assets);
    /* character packs (Japanese...): the user's, like themes; looked for only
     * when a name first has a character the fonts lack. Fetched from Settings >
     * Interface > Language packs (net/packfetch.h). */
    snprintf(g_packs_dir, sizeof g_packs_dir, "%s/packs", data);
    ui_packs_dir(g_packs_dir);
    ui_packs_first(settings_get("packs.first", ""));
    pack_songs_load();
    ui_packs_counted(g_pack_songs_known);       /* (never counted: the toast for boxes waits for this start's scan) */
    packs_init(g_packs_dir);
    packs_url_base(settings_get("packs.url", ""));   /* (a test server's: a pack tried before it is published) */
    int n_themes = ui_load_themes(assets, g_user_themes, themes, UI_MAX_THEMES);
    const ui_fonts_t *base_fonts = n_themes > 0 ? ui_theme_fonts(&themes[0]) : NULL;
    if (!base_fonts) { fprintf(stderr, "no usable theme or font in %s\n", assets); return 1; }
    char theme_chosen[sizeof themes[0].name] = "";   /* the theme picked, when this start can't use it */
    if (!theme_given) {                 /* the last theme picked, by name */
        const char *name = settings_get("ui.theme", "");
        for (int i = 0; i < n_themes; i++)
            if (!strcmp(themes[i].name, name)) theme = i;
        if (strlen(name) < sizeof theme_chosen) snprintf(theme_chosen, sizeof theme_chosen, "%s", name);
    }
    if (theme < 0 || theme >= n_themes || themes[theme].status == THEME_ERROR) theme = 0;
    const ui_fonts_t *theme_fonts = ui_theme_fonts(&themes[theme]);
    if (!theme_fonts) { theme = 0; theme_fonts = base_fonts; }
    if (!strcmp(theme_chosen, themes[theme].name)) theme_chosen[0] = '\0';   /* (it's the one in use) */
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
    boot_started();

    /* Library: database and cover cache live next to the music, on the card. */
    char db[512], art[512];
    mkdir(data, 0755);
    snprintf(db, sizeof db, "%s/library.db", data);
    snprintf(art, sizeof art, "%s/art", data);
    mkdir(art, 0755);
    tags_set_exact_mp3_length(0);   /* SD card: don't read whole MP3s for their length */
    tags_set_max_picture(FATHOM_ART_MAX_FILE);   /* embedded covers held in RAM */
    library_set_trying_dir("/tmp");               /* (a file that kills the player: not read again for ever) */
    library_set_trying_keep(data);                /* (and one that takes the H2 down with it: known after the restart) */
    reader_set_helper("/proc/self/exe");          /* (and one that would: read by a helper, which goes in the player's place) */
    /* The last stop: clean if the player left its marker on the way out (below,
     * at the end of main); not, after a power cut, a forced power-off or a
     * crash. Then the library is checked once the player is up. */
    char clean_mark[600], rebuild_mark[600], bad_db[600];
    snprintf(clean_mark, sizeof clean_mark, "%s/stopped-cleanly", data);
    snprintf(rebuild_mark, sizeof rebuild_mark, "%s.rebuild", db);
    snprintf(bad_db, sizeof bad_db, "%s.bad", db);
    int unclean = unlink(clean_mark) != 0 && access(db, F_OK) == 0;
    if (access(rebuild_mark, F_OK) == 0) {            /* found damaged last time: set aside, a new one scanned */
        fprintf(stderr, "library: damaged: set aside as %s, rebuilt\n", bad_db);
        library_set_aside(db);                       /* (its plays owed to the new one: library_import_owed) */
        unlink(rebuild_mark);
        sync();
        unclean = 0;
    }
    int64_t upgrade_songs = library_upgrade_needed(db);
    upgrade_ui_t upgrade = {0};
    if (upgrade_songs >= UPGRADE_SCREEN_SONGS) upgrade_screen_open(&upgrade, upgrade_songs, &themes[theme], theme_fonts);
    library_t *lib = library_open(db, art);
    if (upgrade.scr) upgrade_screen_close(&upgrade, lib, upgrade_songs);
    LAT("main: library open");
    if (!lib && access(db, F_OK) == 0 && !library_file_damaged(db)) {
        /* It didn't open, and the file is a sound database (or that can't be
         * told): no memory at that moment, or the card not answering. Tried
         * again, and the file left where it is. (Any failure to open set it
         * aside as damaged: a new library, the music all read again, and
         * the plays to bring over.) */
        fprintf(stderr, "library: cannot open %s, and it isn't damaged: trying again\n", db);
        for (int i = 0; i < 3 && !lib; i++) {
            sleep(1);
            lib = library_open(db, art);
        }
    } else if (!lib) {
        /* It can't be opened: not a database any more (a damaged card), or
         * the card can't be written. Leaving here meant being started again
         * every 2 s for ever, the logo on the screen and no key served. The
         * file is set aside (library.db.bad) and a new library started: the
         * music is scanned again; play counts and favourites are in the old
         * file. If that can't be done either, the card is the trouble: said,
         * and Power turns the H2 off. */
        fprintf(stderr, "library: cannot open %s: set aside\n", db);
        library_set_aside(db);
        sync();
        lib = library_open(db, art);
    }
    if (!lib) {
        fprintf(stderr, "library: cannot make a new one either: the card?\n");
        card_unusable_screen(&themes[theme], theme_fonts);   /* (it doesn't come back) */
    }
    char pl_dir[600];
    snprintf(pl_dir, sizeof pl_dir, "%s/playlists", data);   /* your own playlists: never in the music folder */
    library_set_playlist_dir(lib, pl_dir);
    char found[600];
    snprintf(found, sizeof found, "%s/covers", data);   /* (every library handle: scans use them too) */
    library_set_found_covers(found);
    fast_walk_setup(lib, music);
    int have = !library_empty(lib);      /* (not listing them all: 2.9 s for 50,000 songs on the H2) */
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
    /* Plays and favourites from a library set aside, once the music is all
     * read again: now, or after the scan that finishes it, at a later start
     * if need be. (Only here, and only in the run that set it aside: a first
     * scan left with Power held never brought them over.) */
    library_import_owed(lib);

    /* The engine's output: a router to the ALSA output (the H2's DAC, USB DACs)
     * or Bluetooth (bluez-alsa, when it's on the card). */
    static audio_output_t out, alsa_out, bt_out;
    LAT("main: library ready");
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
    LAT("main: engine");
    int resumed = player && player_restore(player, g_queue_path) == 0;   /* paused where it was */
    if (player) g_queue_saved_seq = player_seq(player);

    lv_obj_t *main_scr = lv_obj_create(NULL);   /* the UI gets a fresh screen; the splash goes */
    lv_screen_load(main_scr);
    lv_obj_delete(splash);
    ui_logo_release();
    LAT("main: player ready");
    ui_set_new_install(have <= 0);       /* (no library before this start: a fresh install) */
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
        .rescan = rescan, .scan_progress = hook_scan_progress, .restart = restart_player, .usb_drive = usb_drive, .usb_dac = usb_dac_mode, .about = about,
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
    LAT("main: bluetooth");
    if (wifi_available()) {
        hooks.wifi_state = wifi_ui_state;
        hooks.wifi_networks = wifi_ui_networks;
        hooks.wifi_do = wifi_ui_do;
        hooks.wifi_wake = wifi_wake;
        /* fathom/wifi.txt: networks typed on a computer; Wi-Fi on to save them */
        char wtxt[700];
        snprintf(wtxt, sizeof wtxt, "%s/wifi.txt", data);
        int import = access(wtxt, F_OK) == 0;
        if (import && !settings_get_int("wifi.on", 0)) settings_set_int("wifi.on", 1);
        wifi_init(settings_get_int("wifi.on", 0));
        if (import) wifi_import(wtxt);
        LAT("main: wifi");
        http_set_version(FATHOM_VERSION);
        updater_init(CARD_DIR, FATHOM_VERSION, settings_get("update.api", ""));   /* (a test server's) */
        if (!update_is_newer(settings_get("update.found", ""), FATHOM_VERSION) && settings_get("update.found", "")[0])
            settings_set("update.found", "");    /* installed since */
        hooks.update_state = updater_ui_state;
        hooks.update_do = hook_update_do;
        hooks.packs_state = hook_packs_state;   /* (with the updater: both need the card and the network) */
        hooks.packs_do = hook_packs_do;
        LAT("main: updater");
        if (share_available()) {
            hooks.share_state = share_ui_state;
            hooks.share_do = hook_share_do;
            int on = settings_get_int("share.on", 0);
            share_init(music, on, share_password(0));
        }
    }
    LAT("main: share");
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
        char icons[600];                 /* your radio stations' logos, kept small (fetched when Home > Radio reads the list) */
        snprintf(icons, sizeof icons, "%s/radio-icons", data);
        ricon_start(icons);
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
    /* The theme picked couldn't be used at this start (its file or its font
     * not read): the default for this run, and the choice kept. ui_set_theme
     * saves what it's given as the choice, so one such start made the default
     * the saved theme for good. */
    if (theme_chosen[0]) settings_set("ui.theme", theme_chosen);
    LAT("main: theme");
    whats_new_setup(have > 0);
    /* a fresh install (no library before this start): the welcome, once */
    if (have <= 0 && !settings_get_int("welcome.seen", 0)) {
        ui_show_welcome();
        settings_set_int("welcome.seen", 1);
        settings_save();
    }
    screen_set(1);
    g_fast = lv_timer_create(every_100ms, 100, NULL);
    free_space_warm();
    batlog_open(&g_batlog, CARD_DIR "/fathom/battery.log", time(NULL), clock_s());   /* (the clock restored) */
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
        /* (g_charging: from poll_battery just above. A second call here made
         * the start two of the three low readings that power off, at once.) */
        g_charge_mode = !strncmp(why, "vbus", 4) && g_charging && access(STARTED_MARK, F_OK) != 0;
        fprintf(stderr, "power on: %s%s\n", why, g_charge_mode ? " (charging mode)" : "");
        if (g_charge_mode) charge_screen_show();
        else started_mark();
    }
    g_polls = lv_timer_create(polls, 250, NULL);
    levelling_apply();                   /* (off in Pure mode) */
    if (take_tmp_file("/tmp/fathom-usbdisk.msg", usb_msg, sizeof usb_msg) == 0) ui_toast(usbdisk_text(usb_msg), 3000);
    else if (access(music, F_OK) != 0) {
        fprintf(stderr, "library: no music folder (%s)\n", music);
        ui_toast("No Music folder on the card", 5000);
    }
    ui_set_music_missing(access(music, F_OK) != 0);
    boot_markers();                      /* (up: an update's kernel worked) */
    if (uninstaller_pending()) ui_toast("Uninstall Fathom is ready: Settings > About", 5000);
    int scanned_now = force_scan || have <= 0;       /* (the first scan ran on its screen: its covers are still to find) */
    if (unclean && !scanned_now) {
        fprintf(stderr, "library: the last stop wasn't clean: checking it\n");
        g_scan_check = 1;
        scan_start();
    }
    else if (scanned_now && !library_empty(lib) && library_needs_art(lib)) {
        fprintf(stderr, "library: finding covers in the background\n");
        scan_start();
        ui_toast("Finding album covers. Settings > Library shows how far.", 5000);
    }
    else if (back_from_usb && have > 0 && !force_scan) scan_start();
    /* the last scan cut short (switched off during the first one, say): finished
     * in the background, or albums and artists stay missing until a rescan */
    else if (have > 0 && !force_scan && library_scan_unfinished(lib)) {
        fprintf(stderr, "library: finishing a scan that was cut short\n");
        scan_start();
    }
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
    /* covers kept a file each (0.15.1 and older): moved into the covers' store, once */
    else if (have > 0 && !force_scan && library_art_legacy(lib)) {
        fprintf(stderr, "library: moving covers into the store\n");
        scan_start();
    }
    /* no scan at this start, and the library's names never counted (a scan's end counts them) */
    if (!g_scan_state && have > 0 && !force_scan) pack_songs_count_alone();

    LAT("main: loop");
    sh_home_shown();                 /* (the radios', SSH's and the network drive's workers: their first work now) */
    struct pollfd pfd[INPUT_MAX_POLL + 1];
    while (!g_quit) {
        input_event_t ev;
        while (input_next(&ev, clock_ms32())) {
            fast_tick_now();                         /* a key may start something to follow */
            if (ev.kind == IN_JACKS) {
                jacks_changed(ev.hp, ev.lo);
            } else if (ev.kind == IN_VOLUME) {
                volume_step(ev.ui_key);              /* works with the screen off too */
                if (g_screen_on) screen_activity();
                else g_last_input = clock_ms32();        /* (use all the same: the idle power-off counts from it) */
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
                /* screen off: only the media keys act (and count as use, for
                 * the idle power-off); Power wakes the screen */
                int media = ev.ui_key == UI_KEY_OK || ev.ui_key == UI_KEY_PREV || ev.ui_key == UI_KEY_NEXT ||
                            ev.ui_key == UI_KEY_SEEK_BACK || ev.ui_key == UI_KEY_SEEK_FWD;
                if (media) g_last_input = clock_ms32();
                if (ev.ui_key == UI_KEY_OK) ui_key(UI_KEY_PLAY);
                else if (media) ui_key((ui_key_t)ev.ui_key);
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
        int lp = input_timeout(clock_ms32());
        int timeout = wait > 1000 ? 1000 : (int)wait;
        if (lp >= 0 && lp < timeout) timeout = lp;
        int npfd = input_pollfds(pfd, INPUT_MAX_POLL);   /* devices come and go (headphones) */
        if (g_quit_pipe[0] >= 0) pfd[npfd++] = (struct pollfd){.fd = g_quit_pipe[0], .events = POLLIN};   /* (a stop: on_signal) */
        poll(pfd, (nfds_t)npfd, timeout);
    }
    LAT("main: stopping");

    fprintf(stderr, "fathom-player: exiting%s\n", g_usb_drive ? " for USB drive mode" : "");
    scan_stop();
    rg_stop();
    share_shutdown();
    if (g_relay) usb_dac_mode_end("Stopping");
    if (g_usb_drive) {       /* this screen stays up while the card is lent */
        screen_set(1);
        ui_message_screen("USB drive", "Eject the player on your computer, or unplug the cable, to go back to your music.");
        lv_refr_now(NULL);
    } else if (!g_powering_off) {
        DISP(show_last_frame)();
    }
    if (g_batlog_on) batlog_flush(&g_batlog);   /* (a restart or USB drive mode: the H2 stays on) */
    clock_save();
    settings_save();
    ui_save_state();       /* (where a podcast episode was left) */
    /* The three network workers are told to stop, all of them before the
     * rest, and waited for at the end. One inside a name lookup (Wi-Fi up,
     * the router not answering) can't be interrupted for about 5 s; told and
     * waited for one after another here, they took up to 15 s, and S90fathom
     * stop kills the player at 5: before the library was closed and the
     * clean stop marked. */
    podsync_stop_ask();    /* (a download under way: dropped, its .part removed) */
    packs_stop_ask();      /* (a pack being fetched: dropped; what it left is cleared by the next fetch) */
    cf_stop_ask();
    ricon_stop_ask();      /* (an icon being fetched: dropped) */
    queue_save();
    if (player) player_free(player);
    if (eng) engine_free(eng);
    LAT("main: saved, player and engine freed");
    library_close(lib);
    LAT("main: library closed");
    mark_clean_stop();                  /* (it checks the library otherwise) */
    {   /* The network workers, 2 s between the three: they use nothing closed
         * above (files on the card and memory of their own: no library, no
         * player, no UI). One not back by then is left and ends with the
         * player; what it was writing (a .part, a .tmp) is removed when its
         * worker next starts. (Unsubscribes still waiting are done here.) */
        uint32_t t0 = clock_ms32();
        int back = podsync_stop_wait(2000);
        int ms = 2000 - (int)(clock_ms32() - t0);
        back += cf_stop_wait(ms > 0 ? ms : 0);
        ms = 2000 - (int)(clock_ms32() - t0);
        back += ricon_stop_wait(ms > 0 ? ms : 0);
        ms = 2000 - (int)(clock_ms32() - t0);
        back += packs_stop_wait(ms > 0 ? ms : 0);
        if (back < 4) fprintf(stderr, "fathom-player: %d of the network's workers not back in 2 s: left\n", 4 - back);
        LAT("main: network workers stopped");
    }
    return g_usb_drive ? USB_DRIVE_EXIT : 0;
}
