/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * fathom-sim: the Fathom UI on the desktop.
 *
 *   fathom-sim [--theme N] [--zoom Z] [--music DIR]
 *                                              interactive SDL window; --music scans DIR
 *                                              (read-only) into build/sim-data first
 *   fathom-sim --screenshots DIR               headless: write DIR/<theme>-<screen>.bmp
 *   fathom-sim --motion DIR [--theme N]        headless: frames captured mid-animation
 *   fathom-sim --bench [SECONDS]               headless: the UI's wakeups, redraws and CPU per second
 *   fathom-sim --lang-check FILE               headless: every screen's words through the menus, in
 *                                              the language FILE (tools/lang/check.py)
 *
 * Keys: Up/Down = wheel, Enter = centre (OK), Esc/Backspace = Back,
 *       N = long-press Back (Now Playing), Space = play/pause,
 *       Left/Right = previous/next, M = Menu (context menu), H = hold Menu (quick settings),
 *       [ / ] = hold Menu + wheel (brightness), , / . = hold Prev / Next (seek),
 *       T = next theme, Q = quit.
 */
#include <SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <string.h>
#include <sys/stat.h>

#include "core/lang.h"
#include "core/settings.h"
#include "audio/engine.h"
#include "audio/output.h"
#include "library/library.h"
#include "net/radiodir.h"
#include "lvgl.h"
#include "player/player.h"
#include "ui/theme.h"
#include "ui/assets.h"
#include "ui/fonts.h"
#include "ui/ui.h"

#define DB_PATH  "build/sim-data/library.db"
#define ART_DIR  "build/sim-data/art"

static library_t *g_lib;
static int g_bench;         /* --bench: the clock output, for real-time playing */
static engine_t *g_eng;
static player_t *g_player;

static int g_n_themes;      /* built-in themes, then any in build/sim-data/themes */
/* Home > Radio: the example stations (the simulator can't play them: no FFmpeg),
 * or once one is added from Find stations, build/sim-data/radio.txt (never in
 * screenshots: they show the example) */
#define SIM_RADIO "build/sim-data/radio.txt"
static int g_shots;
static int sim_radio(radio_station_t *out, int max)
{
    int n = g_shots ? -1 : radio_load(SIM_RADIO, out, max);
    return n >= 0 ? n : radio_load("assets/radio.txt", out, max);
}
static int sim_radio_add(const radio_station_t *st) { return g_shots ? 1 : radio_add(SIM_RADIO, "assets/radio.txt", st); }
static int sim_radio_remove(const char *url) { return g_shots ? 0 : radio_remove(SIM_RADIO, "assets/radio.txt", url); }
/* Find stations: the directory's answers saved in tests/fixtures/radiodir (no
 * network here); g_dir_fail: as if no server answered, g_dir_slow: one that
 * takes its time (until the request is dropped) */
static int g_dir_fail, g_dir_slow;
static long sim_dir_fetch(const char *url, char *buf, size_t cap, const volatile int *abort)
{
    while (g_dir_slow && !(abort && *abort)) {
        struct timespec t = {0, 20 * 1000 * 1000L};
        nanosleep(&t, NULL);
    }
    if (g_dir_fail || g_dir_slow) return -1;
    const char *name = strstr(url, "/json/countries") ? "countries"
                     : strstr(url, "tag=jazz") && strstr(url, "order=random") ? "stations-jazz-random"
                     : strstr(url, "tag=jazz") ? "stations-jazz"
                     : strstr(url, "countrycode=GB") ? "stations-gb"
                     : strstr(url, "name=") ? "stations-fip"
                     : strstr(url, "/json/url/") ? NULL : "stations-popular";
    if (!name) return 0;                              /* a play counted */
    char path[256];
    snprintf(path, sizeof path, "tests/fixtures/radiodir/%s.json", name);
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t n = fread(buf, 1, cap - 1, f);
    int more = fgetc(f) != EOF;
    fclose(f);
    buf[n] = '\0';
    return more ? -2 : (long)n;
}
/* Settings > Network drive, as on an H2 serving over Wi-Fi */
static int sim_share_state(ui_share_t *st)
{
    *st = (ui_share_t){.state = UI_SHARE_ON, .host = "fathom-h2.local", .ip = "192.168.1.23",
                       .user = "fathom", .password = "k7mq2p9x", .cert = "3F 2A 91 0C 77 AB"};
    return 1;
}
static void sim_share_do(int what) { (void)what; }
/* Settings > SSH, on, with Wi-Fi connected */
static int sim_ssh_state(ui_ssh_t *st)
{
    *st = (ui_ssh_t){.state = UI_SSH_ON, .usb_ip = "10.42.0.1", .wifi_ip = "192.168.1.23", .password = "r4tw8nce"};
    return 1;
}
static void sim_ssh_do(int what) { (void)what; }
/* Sound > Balance and Volume limit, as on the headphone jack (balance on the engine) */
static int g_sim_bal, g_sim_limit;
static void sim_sound_value(int what, char *out, size_t cap)
{
    char n[16];   /* (translated, as the H2's) */
    snprintf(n, sizeof n, "%d", abs(g_sim_bal));
    if (what == UI_SOUND_BALANCE && !g_sim_bal) snprintf(out, cap, "%s", tr("Centre"));
    else if (what == UI_SOUND_BALANCE) tr_fill(out, cap, g_sim_bal < 0 ? "%s dB left" : "%s dB right", n, NULL);
    else if (!g_sim_limit) snprintf(out, cap, "%s", tr("Off"));
    else snprintf(out, cap, "%d dB", g_sim_limit);
}
static int sim_sound_adjust(int what, int dir)
{
    if (what == UI_SOUND_BALANCE) {
        g_sim_bal = g_sim_bal + dir < -12 ? -12 : g_sim_bal + dir > 12 ? 12 : g_sim_bal + dir;
        float g = (float)pow(10.0, -abs(g_sim_bal) / 20.0);
        if (g_eng) engine_set_balance(g_eng, g_sim_bal > 0 ? g : 1.0f, g_sim_bal < 0 ? g : 1.0f);
    } else {
        g_sim_limit = g_sim_limit + dir * 3 < -30 ? -30 : g_sim_limit + dir * 3 > 0 ? 0 : g_sim_limit + dir * 3;
    }
    return 0;
}
/* Sound > Pure mode and the signal path, as for a FLAC on the headphone jack */
static int g_sim_pure;
static void sim_pure_toggle(void) { g_sim_pure = !g_sim_pure; ui_set_pure(g_sim_pure, g_sim_pure); }
static int sim_signal_path(int i, char *label, size_t lc, char *value, size_t vc)
{
    static const char *const rows[][2] = {
        {"File", "FLAC 24-bit 96 kHz"}, {"Levelling", "Off"}, {"EQ", "Off"}, {"Balance", "Centre"},
        {"Volume", "In the DAC -32.0 dB"}, {"Output", "ES9018K2M headphones"}, {"Sent as", "32-bit 96 kHz"},
        {"Result", "Bit-perfect"}};
    if (i < 0 || i >= (int)(sizeof rows / sizeof rows[0])) return -1;
    snprintf(label, lc, "%s", tr(rows[i][0]));   /* (translated as the H2's are) */
    if (g_sim_pure && (i == 1 || i == 2 || i == 3)) snprintf(value, vc, "%s", tr("Bypassed"));
    else if (i == 4) tr_fill(value, vc, "In the DAC %s", "-32.0 dB", NULL);
    else if (i == 5) snprintf(value, vc, "ES9018K2M %s", tr("headphones"));
    else snprintf(value, vc, "%s", i == 0 || i == 6 ? rows[i][1] : tr(rows[i][1]));
    return 0;
}
static void sim_transport_toggle(void) { settings_set_int("sound.transport", !settings_get_int("sound.transport", UI_TRANSPORT_DEFAULT)); }
static void sim_battery_info(char *out, size_t cap) { tr_fill(out, cap, "%s, charging", "86 %", NULL); }
/* Settings > Interface > Language: build/sim-data/lang */
static int sim_languages(char names[][64], int max) { return lang_list("build/sim-data/lang", names, max); }
/* --lang-check: the language stays the one given (Settings > Language does nothing) */
static int g_lang_check;
static void sim_language_set(const char *name)
{
    if (g_lang_check) return;
    char path[300];
    snprintf(path, sizeof path, "build/sim-data/lang/%s.txt", name ? name : "");
    lang_load(name && name[0] ? path : NULL);
}
/* Settings > About > Check for updates: a newer release on offer; the steps
 * go by at a press (the simulator has no network) */
static ui_update_t g_sim_upd = {.state = UI_UPD_IDLE, .current = "v0.7.0", .latest = "v0.8.0", .size = 16234567};
static void sim_update_state(ui_update_t *st) { *st = g_sim_upd; }
static void sim_update_do(int what)
{
    int next[] = {[UI_UPD_CHECK] = UI_UPD_AVAILABLE, [UI_UPD_START] = UI_UPD_READY,
                  [UI_UPD_CANCEL] = UI_UPD_AVAILABLE, [UI_UPD_INSTALL] = UI_UPD_INSTALLING};
    g_sim_upd.state = next[what];
}
/* Settings > About > Uninstall Fathom: HiBy's V2.3 found; the steps go by at a press */
static ui_uninstall_t g_sim_uni = {.state = UI_UNI_READY, .file = "H2-Firmware-V2.3.upt", .ver = "2025-11-11"};
static void sim_uninstall_state(ui_uninstall_t *st) { *st = g_sim_uni; }
static void sim_uninstall_do(int what)
{
    int next[] = {[UI_UNI_CHECK] = UI_UNI_READY, [UI_UNI_APPLY] = UI_UNI_PENDING,
                  [UI_UNI_CANCEL] = UI_UNI_IDLE, [UI_UNI_RESTART] = UI_UNI_PENDING};
    g_sim_uni.state = next[what];
}
/* A made-up week on battery, for Settings > Power > Battery: charged
 * overnight, mostly playing with the screen off, the screen on a little each
 * hour, paused late; one day switched off for three hours (not known how
 * long: no Wi-Fi), one topped up at lunch */
static int sim_battery_report(batrep_t *rep, int days)
{
    static batrec_t r[8000];
    int n = 0;
    const int64_t now = 1795000000 - 1795000000 % 86400 + 7 * 86400 + 15 * 3600 + 20 * 60;   /* (a Tuesday, 15:20 UTC) */
    double level = 40;
    int last_pct = -1, last_flags = -1;
    for (int64_t t = now - 7 * 86400; t <= now; t += 60) {
        int hour = (int)(t / 3600 % 24), min = (int)(t / 60 % 60), day = (int)((now - t) / 86400);
        int charging = hour < 6 || (day == 2 && hour == 13 && min < 40);
        if (day == 4 && hour >= 12 && hour < 15) {          /* off */
            if (last_flags >= 0 && r[n - 1].kind != BL_OFF) r[n++] = (batrec_t){t, (uint8_t)last_pct, (uint8_t)last_flags, BL_OFF, 0};
            level -= 0.3 / 60;
            continue;
        }
        if (n && r[n - 1].kind == BL_OFF) {                 /* on again: its clock where it stopped */
            r[n++] = (batrec_t){t, (uint8_t)last_pct, (uint8_t)last_flags, BL_BOOT, 0};
            last_flags = -1;
        }
        int act = hour >= 22 ? BL_ACT_PAUSED : min < 10 ? BL_ACT_SCREEN : BL_ACT_PLAYING;
        static const double rate[BL_ACTS] = {4.0, 12.0, 1.5};
        level += charging ? 30.0 / 60 : -rate[act] / 60;
        if (level > 100) level = 100;
        if (level < 1) level = 1;
        int pct = (int)ceil(level - 1e-9);
        int flags = (charging ? BL_CHARGING : 0) | (act == BL_ACT_SCREEN ? BL_SCREEN | BL_PLAYING : act == BL_ACT_PLAYING ? BL_PLAYING : 0);
        if ((pct != last_pct || flags != last_flags || t + 60 > now) && n < 8000) {
            r[n++] = (batrec_t){t, (uint8_t)pct, (uint8_t)flags, BL_SAMPLE, 0};
            last_pct = pct, last_flags = flags;
        }
    }
    batlog_report(r, n, days, rep);
    return 0;
}

static const ui_hooks_t g_hooks = {.fonts = ui_theme_fonts, .icon = ui_theme_icon, .radio_stations = sim_radio,
                                   .radio_add = sim_radio_add, .radio_remove = sim_radio_remove,
                                   .share_state = sim_share_state, .share_do = sim_share_do,
                                   .ssh_state = sim_ssh_state, .ssh_do = sim_ssh_do,
                                   .sound_value = sim_sound_value, .sound_adjust = sim_sound_adjust,
                                   .pure_toggle = sim_pure_toggle, .signal_path = sim_signal_path,
                                   .transport_toggle = sim_transport_toggle, .battery_info = sim_battery_info,
                                   .languages = sim_languages, .language_set = sim_language_set,
                                   .update_state = sim_update_state, .update_do = sim_update_do,
                                   .uninstall_state = sim_uninstall_state, .uninstall_do = sim_uninstall_do,
                                   .battery_report = sim_battery_report};

static void cue_sample(void);

/* ---- headless screenshots ---- */

static uint8_t g_fb[UI_W * UI_H * 2] __attribute__((aligned(64)));   /* (LVGL asserts an aligned buffer) */

static void null_flush(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    (void)a; (void)px;
    lv_display_flush_ready(d);
}

static int write_bmp(const char *path, const lv_draw_buf_t *buf)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    int w = buf->header.w, h = buf->header.h;
    int row = (w * 3 + 3) & ~3;
    uint32_t size = 54 + (uint32_t)row * h;
    uint8_t hdr[54] = {'B', 'M'};
    #define PUT32(o, v) do { hdr[o] = (uint8_t)(v); hdr[o+1] = (uint8_t)((v) >> 8); \
                             hdr[o+2] = (uint8_t)((v) >> 16); hdr[o+3] = (uint8_t)((v) >> 24); } while (0)
    PUT32(2, size); PUT32(10, 54); PUT32(14, 40); PUT32(18, w); PUT32(22, h);
    hdr[26] = 1; hdr[28] = 24;
    PUT32(34, (uint32_t)row * h);
    fwrite(hdr, 1, 54, f);
    uint8_t pad[3] = {0};
    for (int y = h - 1; y >= 0; y--) {
        fwrite(buf->data + (size_t)y * buf->header.stride, 1, (size_t)w * 3, f);  /* LVGL RGB888 is B,G,R */
        fwrite(pad, 1, (size_t)(row - w * 3), f);
    }
    fclose(f);
    return 0;
}

static int screenshots(const char *dir, theme_t *themes, const ui_fonts_t *fonts)
{
    lv_display_t *d = lv_display_create(UI_W, UI_H);
    lv_display_set_buffers(d, g_fb, NULL, sizeof g_fb, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(d, null_flush);
    ui_init(lv_screen_active(), themes, g_n_themes, fonts, 1, g_lib, g_player);
    ui_set_hooks(&g_hooks);
    ui_set_battery(86, 1);   /* the desktop has no H2 battery: a fixed sample value, charging (two icons) */
    ui_set_bt_link(UI_BT_LINK_CONNECTED);   /* and headphones, so the header shows every kind of icon */

    static const char *const screens[] = {"home", "albums", "tracks", "playlists", "playlist", "nowplaying", "themes",
                                          "quick", "context", "info", "queue", "glyphs", "search",
                                          "artists", "artist", "artistsongs", "albumartists", "homeedit",
                                          "usbask", "wifiask", "interface", "usbdrive", "scrub", "sound", "radio", "radionp", "share", "ssh", "toast", "genres", "folders", "folder", "years", "homeend", "homeall", "signal", "purenp",
                                          "rdir", "rdirgenres", "rdirresults", "rdirbusy", "rdircountries", "rdirsearch",
                                          "rdirctx", "rdirerror", "rdirnp", "rdirnpmenu", "rdirrandom",
                                          "about", "update", "updatelatest", "updatefailed", "updateprogress",
                                          "updateready", "updateinstall", "battery", "battery7", "batterysince",
                                          "batteryhistory", "uninstall", "uninstallchecking", "uninstallfailed",
                                          "uninstallnone", "uninstallpending"};
    static const char *const slug[] = {"studio-mint", "studio-ember", "studio-pastel", "pixel-pastel"};
    for (int t = 0; t < 4 && t < g_n_themes; t++) {   /* the built-in themes */
        ui_set_theme(t);
        for (size_t s = 0; s < sizeof screens / sizeof screens[0]; s++) {
            lv_obj_t *sample = NULL;
            if (!strcmp(screens[s], "glyphs")) {    /* digits vs letters, and the fallback font */
                ui_show("home");
                const ui_fonts_t *ff = ui_theme_fonts(&themes[t]);
                sample = lv_label_create(lv_screen_active());
                lv_obj_set_style_bg_color(sample, lv_color_hex(themes[t].surface), 0);
                lv_obj_set_style_bg_opa(sample, LV_OPA_COVER, 0);
                lv_obj_set_style_text_color(sample, lv_color_hex(themes[t].text), 0);
                if (ff) lv_obj_set_style_text_font(sample, ff->body, 0);
                lv_obj_set_style_pad_all(sample, 8, 0);
                lv_obj_set_width(sample, UI_W);
                lv_label_set_text(sample, "0258 OZSB 1lI7 :09\n\xC5\x8Csaka \xC4\x88u \xC5\x9Cveti\n"
                                          "\xD0\x9C\xD0\xBE\xD1\x81\xD0\xBA\xD0\xB2\xD0\xB0 "
                                          "\xCE\x95\xCE\xBB\xCE\xBB\xCE\xAC\xCE\xB4\xCE\xB1 \xC5\x81\xC3\xB3" "d\xC5\xBA\n"
                                          "Ti\xE1\xBA\xBFng Vi\xE1\xBB\x87t \xC4\x91\xC6\xB0\xE1\xBB\xA3" "c");
                lv_obj_align(sample, LV_ALIGN_BOTTOM_MID, 0, 0);
            } else if (!strcmp(screens[s], "radionp")) {    /* a station, as it connects */
                radio_station_t st[4];
                int n = radio_load("assets/radio.txt", st, 4);
                if (g_player && n > 0) {
                    player_radio_play(g_player, st, n, 0);
                    player_radio_set_info(g_player, "Some Artist", "A song on the radio", NULL);
                }
                ui_show("nowplaying");
            } else if (!strcmp(screens[s], "rdirbusy") || !strcmp(screens[s], "rdirerror")) {   /* still asking; no answer */
                g_dir_slow = !strcmp(screens[s], "rdirbusy");
                g_dir_fail = !g_dir_slow;
                ui_show(screens[s]);
            } else if (!strncmp(screens[s], "rdirnp", 6) || !strcmp(screens[s], "rdirrandom")) {
                /* with the keys: a station of the list played (and Menu on it), or Random station */
                ui_show(strcmp(screens[s], "rdirrandom") ? "rdirresults" : "rdir");
                ui_key(UI_KEY_OK);
                for (int i = 0; i < 100 && !strcmp(screens[s], "rdirrandom"); i++) {   /* the answer comes in */
                    struct timespec ts = {0, 5 * 1000 * 1000L};
                    nanosleep(&ts, NULL);
                    ui_tick();
                }
                if (!strcmp(screens[s], "rdirnpmenu")) ui_key(UI_KEY_MENU);
                ui_set_theme(t);                            /* (the slide finished) */
            } else if (!strcmp(screens[s], "purenp")) {    /* Pure mode: the badge */
                sim_pure_toggle();
                ui_show("nowplaying");
            } else if (!strcmp(screens[s], "usbdrive")) {
                ui_show("nowplaying");
                sample = ui_message_screen("USB drive",
                                           "Eject the H2 on your computer, or unplug the cable, to go back to your music.");
            } else if (!strncmp(screens[s], "update", 6)) {   /* the update's screens, in each state */
                static const struct { const char *name; ui_update_t u; } st[] = {
                    {"update", {.state = UI_UPD_AVAILABLE}},
                    {"updatelatest", {.state = UI_UPD_LATEST}},
                    {"updatefailed", {.state = UI_UPD_FAILED, .message = "Not enough space on the card", .need_mb = 63}},
                    {"updateprogress", {.state = UI_UPD_DOWNLOADING, .done = 6815744, .total = 16234567}},
                    {"updateready", {.state = UI_UPD_READY}},
                    {"updateinstall", {.state = UI_UPD_INSTALLING}},
                };
                for (size_t k = 0; k < sizeof st / sizeof st[0]; k++)
                    if (!strcmp(st[k].name, screens[s])) {
                        ui_update_t u = st[k].u;
                        snprintf(u.current, sizeof u.current, "%s", g_sim_upd.current);
                        snprintf(u.latest, sizeof u.latest, "%s", g_sim_upd.latest);
                        u.size = g_sim_upd.size;
                        g_sim_upd = u;
                    }
                ui_show("update");
            } else if (!strncmp(screens[s], "uninstall", 9)) {   /* Uninstall Fathom, in each state */
                static const struct { const char *name; ui_uninstall_t u; } st[] = {
                    {"uninstall", {.state = UI_UNI_READY, .ver = "2025-11-11"}},
                    {"uninstallchecking", {.state = UI_UNI_CHECKING, .done = 25000000, .total = 61400000}},
                    {"uninstallfailed", {.state = UI_UNI_FAILED, .message = "This update is for another player"}},
                    {"uninstallnone", {.state = UI_UNI_FAILED, .message = "No HiBy update file on the card", .no_file = 1}},
                    {"uninstallpending", {.state = UI_UNI_PENDING}},
                };
                for (size_t k = 0; k < sizeof st / sizeof st[0]; k++)
                    if (!strcmp(st[k].name, screens[s])) g_sim_uni = st[k].u;
                ui_show("uninstall");
            } else if (!strcmp(screens[s], "toast")) {    /* a long one: it wraps */
                ui_show("ssh");
                ui_toast("Over USB and Wi-Fi, with the password", 3000);
            } else {
                ui_show(screens[s]);
            }
            int on_top = !strcmp(screens[s], "usbdrive") || !strcmp(screens[s], "toast") ||
                         (g_sim_upd.state >= UI_UPD_DOWNLOADING && g_sim_upd.state <= UI_UPD_RESTARTING) ||
                         !strcmp(screens[s], "uninstallpending") || !strcmp(screens[s], "uninstallnone");   /* (the update's progress screen) */
            lv_obj_t *what = on_top ? lv_layer_top() : lv_screen_active();
            lv_obj_update_layout(what);
            lv_draw_buf_t *img = lv_snapshot_take(what, LV_COLOR_FORMAT_RGB888);
            if (!img) { fprintf(stderr, "snapshot failed\n"); return 1; }
            char path[1024];
            snprintf(path, sizeof path, "%s/%s-%s.bmp", dir, slug[t], screens[s]);
            if (write_bmp(path, img)) { fprintf(stderr, "cannot write %s\n", path); return 1; }
            lv_draw_buf_destroy(img);
            if (sample) lv_obj_delete(sample);
            if (!strcmp(screens[s], "radionp") || !strncmp(screens[s], "rdirnp", 6) || !strcmp(screens[s], "rdirrandom"))
                cue_sample();   /* off the radio for the next theme */
            if (!strcmp(screens[s], "purenp")) sim_pure_toggle();   /* (off again) */
            g_dir_fail = 0;
            if (g_dir_slow) { g_dir_slow = 0; ui_show("home"); }   /* (the request dropped) */
            if (g_sim_upd.state != UI_UPD_IDLE) {               /* (the progress screen goes) */
                g_sim_upd.state = UI_UPD_IDLE;
                ui_update_changed();
            }
            if (!strcmp(screens[s], "toast"))      /* (gone before the next screens) */
                for (uint32_t c = 0; c < lv_obj_get_child_count(lv_layer_top()); c++)
                    lv_obj_add_flag(lv_obj_get_child(lv_layer_top(), (int32_t)c), LV_OBJ_FLAG_HIDDEN);
            printf("%s\n", path);
        }
    }
    /* contributed themes (build/sim-data/themes): the home screen, to check fonts and colours */
    for (int t = 4; t < g_n_themes; t++) {
        if (themes[t].status == THEME_ERROR) continue;
        ui_set_theme(t);
        ui_show("home");
        lv_obj_update_layout(lv_screen_active());
        lv_draw_buf_t *img = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB888);
        if (!img) return 1;
        char path[1024];
        snprintf(path, sizeof path, "%s/user-%d-home.bmp", dir, t);
        if (write_bmp(path, img)) return 1;
        lv_draw_buf_destroy(img);
        printf("%s (%s: %s)\n", path, themes[t].name, themes[t].note[0] ? themes[t].note : "ok");
    }
    return 0;
}

/* ---- headless motion test: fake clock, frames captured mid-animation ---- */

static uint32_t g_fake_ms;
static uint32_t fake_tick(void) { return g_fake_ms; }

static void advance(uint32_t ms)
{
    for (uint32_t t = 0; t < ms; t += 5) { g_fake_ms += 5; lv_timer_handler(); }
}

/* ---- headless translation check: every screen's words, through the menus ---- */

static FILE *g_lc_out;

static void lc_labels(lv_obj_t *o, const char *where)
{
    if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return;
    if (lv_obj_check_type(o, &lv_label_class)) {
        const char *t = lv_label_get_text(o);
        if (t && *t) fprintf(g_lc_out, "%s\t%s\n", where, t);
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(o); i++) lc_labels(lv_obj_get_child(o, (int32_t)i), where);
}

static void lc_settle(void)
{
    advance(600);   /* (slides and fades over) */
    ui_tick();
    lv_timer_handler();
}

static void lc_dump(const char *where)
{
    lc_settle();
    lc_labels(lv_screen_active(), where);
    lc_labels(lv_layer_top(), where);
}

/* From Home, down the rows given (their indexes), each opened with OK; 0 if the
 * last one opened something (a deeper list or another screen) */
static int lc_go(const int *path, int n)
{
    ui_show("home");
    for (int k = 0; k < n; k++) {
        int sel, rows, d = ui_list_state(&sel, &rows, NULL);
        if (d < 1 || path[k] >= rows) return -1;
        for (int g = 0; sel != path[k] && g < 200; g++) {
            ui_key(sel < path[k] ? UI_KEY_DOWN : UI_KEY_UP);
            ui_list_state(&sel, NULL, NULL);
        }
        if (sel != path[k]) return -1;
        ui_key(UI_KEY_OK);
        lc_settle();
        if (k < n - 1 && ui_list_state(NULL, NULL, NULL) != d + 1) return -1;
    }
    return 0;
}

static void lc_walk(int *path, int n, const char *where)
{
    char here[512];
    if (lc_go(path, n)) { if (getenv("LC_DEBUG")) fprintf(stderr, "can't reach %s\n", where); return; }
    lc_dump(where);
    int rows, fixed, d = ui_list_state(NULL, &rows, &fixed);
    if (d != n + 1 || n >= 6) return;                         /* not a list (or deep enough) */
    if (getenv("LC_DEBUG")) fprintf(stderr, "%s: depth %d, %d rows\n", where, d, rows);
    int visit = rows > 30 ? 3 : rows;                         /* long lists: music, a few rows */
    for (int i = 0; i < visit; i++) {
        path[n] = i;
        /* Menu on the row: its actions or its help */
        if (lc_go(path, n) == 0) {
            int sel;
            for (int g = 0; ui_list_state(&sel, NULL, NULL) == d && sel != i && g < 200; g++) ui_key(sel < i ? UI_KEY_DOWN : UI_KEY_UP);
            ui_key(UI_KEY_MENU);
            snprintf(here, sizeof here, "%s > [%d] Menu", where, i);
            lc_dump(here);
        }
        snprintf(here, sizeof here, "%s > [%d]", where, i);
        if (!fixed) lc_walk(path, n + 1, here);
    }
}

static int lang_check(const char *file, theme_t *themes, const ui_fonts_t *fonts)
{
    lv_tick_set_cb(fake_tick);
    lv_display_t *d = lv_display_create(UI_W, UI_H);
    lv_display_set_buffers(d, g_fb, NULL, sizeof g_fb, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(d, null_flush);
    if (lang_load(file)) { fprintf(stderr, "cannot load %s\n", file); return 1; }
    g_lang_check = 1;
    ui_init(lv_screen_active(), themes, g_n_themes, fonts, 1, g_lib, g_player);
    ui_set_hooks(&g_hooks);
    ui_set_battery(86, 0);
    ui_set_theme(0);
    g_lc_out = stdout;
    int path[8];
    lc_walk(path, 0, "Home");
    /* Now Playing, its menu, quick settings */
    ui_show("nowplaying"); lc_dump("Now playing");
    ui_key(UI_KEY_MENU); lc_dump("Now playing > Menu");
    ui_show("nowplaying"); ui_key(UI_KEY_MENU_LONG); lc_dump("Quick settings");
    ui_show("home"); ui_toast("Over USB and Wi-Fi, with the password", 3000); lc_dump("Toast");
    return 0;
}

static int frame(const char *dir, const char *name)
{
    lv_obj_update_layout(lv_screen_active());
    lv_draw_buf_t *img = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB888);
    if (!img) return -1;
    char path[1024];
    snprintf(path, sizeof path, "%s/%s.bmp", dir, name);
    int r = write_bmp(path, img);
    lv_draw_buf_destroy(img);
    printf("%s\n", path);
    return r;
}

static int motion(const char *dir, theme_t *themes, const ui_fonts_t *fonts, int theme)
{
    lv_tick_set_cb(fake_tick);
    lv_display_t *d = lv_display_create(UI_W, UI_H);
    lv_display_set_buffers(d, g_fb, NULL, sizeof g_fb, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(d, null_flush);
    ui_init(lv_screen_active(), themes, g_n_themes, fonts, 1, g_lib, g_player);
    ui_set_hooks(&g_hooks);
    ui_set_battery(86, 0);   /* the desktop has no H2 battery: a fixed sample value */
    ui_set_theme(theme);
    ui_show("home");
    advance(300);
    char n[32];
    /* 1. pill slides from Albums to Artists */
    ui_key(UI_KEY_DOWN);
    for (int i = 0; i < 4; i++) { snprintf(n, sizeof n, "m%d-a%d", theme, i); frame(dir, n); advance(30); }
    advance(300);
    /* 2. open Albums: the new level slides over Home */
    ui_key(UI_KEY_UP);
    advance(300);
    ui_key(UI_KEY_OK);
    for (int i = 0; i < 4; i++) { snprintf(n, sizeof n, "m%d-b%d", theme, i); advance(i ? 45 : 15); frame(dir, n); }
    advance(300);
    /* 3. scroll past the bottom */
    for (int i = 0; i < 5; i++) { ui_key(UI_KEY_DOWN); advance(i < 4 ? 20 : 0); }
    for (int i = 0; i < 4; i++) { snprintf(n, sizeof n, "m%d-c%d", theme, i); frame(dir, n); advance(30); }
    advance(300);
    /* 4. back to Home: Albums slides away to the right */
    ui_key(UI_KEY_BACK);
    for (int i = 0; i < 4; i++) { snprintf(n, sizeof n, "m%d-d%d", theme, i); advance(i ? 45 : 15); frame(dir, n); }
    advance(300);
    /* 5. Now Playing slides up, then back down */
    ui_key(UI_KEY_BACK_LONG);
    for (int i = 0; i < 4; i++) { snprintf(n, sizeof n, "m%d-e%d", theme, i); advance(i ? 55 : 15); frame(dir, n); }
    advance(300);
    ui_key(UI_KEY_BACK);
    for (int i = 0; i < 4; i++) { snprintf(n, sizeof n, "m%d-f%d", theme, i); advance(i ? 55 : 15); frame(dir, n); }
    advance(300);
    /* 6. scrolling text: a playlist's long rows, re-rendered once a second as the
     * app does; frames every 1.5 s through two back-and-forth cycles */
    ui_show("playlist");
    for (int i = 0; i < 64; i++) {                 /* every 250 ms for 16 s */
        snprintf(n, sizeof n, "m%d-g%02d", theme, i);
        frame(dir, n);
        advance(250);
        if (i % 4 == 3) ui_tick_1s();
    }
    /* 7. fast scrolling: 40 wheel steps 20 ms apart through Songs */
    ui_show("songs");
    advance(300);
    for (int i = 0; i < 40; i++) { ui_key(UI_KEY_DOWN); advance(20); }
    advance(200);
    snprintf(n, sizeof n, "m%d-h0", theme);
    frame(dir, n);
    return 0;
}

/* ---- headless bench: what the UI costs at rest and while playing ----
 * The H2's loop (device/main.c) in real time: the UI's timers, a display in
 * direct mode like the H2's, and a poll that sleeps until the next timer.
 * For each case: loop wakeups, flushes and pixels sent to the panel per second,
 * and the process's CPU (the engine's thread too). */

static unsigned long g_flushes, g_flush_px, g_still_px;   /* still: drawn with no animation running (scrolling text) */
static void every_second(lv_timer_t *t);
static void every_100ms(lv_timer_t *t);

static void count_flush(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    (void)px;
    g_flushes++;
    g_flush_px += (unsigned long)lv_area_get_size(a);
    if (!lv_anim_count_running()) g_still_px += (unsigned long)lv_area_get_size(a);
    if (getenv("FATHOM_BENCH_AREAS")) printf("  flush %d,%d %dx%d\n", (int)a->x1, (int)a->y1, (int)lv_area_get_width(a), (int)lv_area_get_height(a));
    lv_display_flush_ready(d);
}

static double cpu_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t);
    return (double)t.tv_sec + t.tv_nsec / 1e9;
}

static uint32_t mono_ms_sim(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)(t.tv_sec * 1000 + t.tv_nsec / 1000000);
}

static unsigned long bench_case(const char *name, int seconds, int max_wait)
{
    unsigned long wakeups = 0;
    uint32_t w0 = mono_ms_sim();                  /* the change of screen first, not counted */
    while (mono_ms_sim() - w0 < 1500) { uint32_t w = lv_timer_handler(); usleep((w > 50 ? 50 : w) * 1000); }
    g_flushes = g_flush_px = g_still_px = 0;
    double c0 = cpu_s();
    uint32_t t0 = mono_ms_sim();
    while (mono_ms_sim() - t0 < (uint32_t)seconds * 1000) {
        uint32_t wait = lv_timer_handler();
        int timeout = wait > (uint32_t)max_wait ? max_wait : (int)wait;
        wakeups++;
        if (timeout > 0) usleep((useconds_t)timeout * 1000);
    }
    double c = cpu_s() - c0;
    printf("%-28s %6.1f wakeups/s %6.1f flushes/s %8.0f px/s  cpu %5.2f %%\n", name, wakeups / (double)seconds,
           g_flushes / (double)seconds, g_flush_px / (double)seconds, 100.0 * c / seconds);
    return g_still_px;
}

static uint16_t g_bench_fb[UI_W * UI_H] __attribute__((aligned(64)));   /* (LVGL asserts an aligned buffer) */

/* The panel's pixels (only the parts redrawn since) against the screen drawn
 * whole: anything a render changed without LVGL redrawing it shows here. */
static int bench_fb_check(const char *name)
{
    lv_obj_update_layout(lv_screen_active());
    lv_draw_buf_t *img = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB565);
    if (!img) return -1;
    long bad = 0;
    for (int y = 0; y < UI_H; y++)
        if (memcmp(img->data + (size_t)y * img->header.stride, g_bench_fb + (size_t)y * UI_W, UI_W * 2)) bad++;
    lv_draw_buf_destroy(img);
    if (bad) printf("%-28s %ld rows on the panel differ from the screen\n", name, bad);
    return bad ? 1 : 0;
}

static void bench_scroll(const char *name, int seconds, int max_wait)
{
    unsigned long wakeups = 0, steps = 0;
    g_flushes = g_flush_px = g_still_px = 0;
    double c0 = cpu_s();
    uint32_t t0 = mono_ms_sim(), next = t0;
    while (mono_ms_sim() - t0 < (uint32_t)seconds * 1000) {
        uint32_t now = mono_ms_sim();
        if ((int32_t)(now - next) >= 0) {          /* down for 2 s, up for 2 s */
            ui_key((now - t0) / 2000 % 2 ? UI_KEY_UP : UI_KEY_DOWN);
            next += 60;
            steps++;
        }
        uint32_t wait = lv_timer_handler();
        int32_t until = (int32_t)(next - mono_ms_sim());
        int timeout = wait > (uint32_t)max_wait ? max_wait : (int)wait;
        if (until < timeout) timeout = until < 0 ? 0 : until;
        wakeups++;
        if (timeout > 0) usleep((useconds_t)timeout * 1000);
    }
    uint32_t w0 = mono_ms_sim();                   /* the last animation ends */
    while (mono_ms_sim() - w0 < 600) { uint32_t w = lv_timer_handler(); usleep((w > 20 ? 20 : w) * 1000); }
    double c = cpu_s() - c0;
    printf("%-28s %6.1f wakeups/s %6.1f flushes/s %8.0f px/s  cpu %5.2f %%  (%lu steps)\n", name,
           wakeups / (double)seconds, g_flushes / (double)seconds, g_flush_px / (double)seconds,
           100.0 * c / seconds, steps);
}

static int bench(theme_t *themes, const ui_fonts_t *fonts, int seconds)
{
    uint16_t *fb = g_bench_fb;
    lv_tick_set_cb(mono_ms_sim);
    lv_display_t *d = lv_display_create(UI_W, UI_H);
    lv_display_set_color_format(d, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(d, fb, NULL, sizeof g_bench_fb, LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(d, count_flush);
    ui_init(lv_screen_active(), themes, g_n_themes, fonts, 1, g_lib, g_player);
    ui_set_hooks(&g_hooks);
    ui_set_battery(86, 0);
    ui_set_theme(0);
    lv_timer_create(every_second, 1000, NULL);
    lv_timer_create(every_100ms, 100, NULL);
    int max_wait = getenv("FATHOM_BENCH_MAX_WAIT") ? atoi(getenv("FATHOM_BENCH_MAX_WAIT")) : 1000;
    int bad = 0;

    ui_show("home");
    bad += bench_case("home, paused", seconds, max_wait) != 0;   /* at rest: nothing redrawn */
    bad += bench_fb_check("home, paused");
    ui_show("albums");
    bad += bench_case("albums, paused", seconds, max_wait) != 0;   /* at rest: nothing redrawn */
    bad += bench_fb_check("albums, paused");
    ui_show("nowplaying");
    bad += bench_case("now playing, paused", seconds, max_wait) != 0;   /* at rest: nothing redrawn */
    bad += bench_fb_check("now playing, paused");
    if (g_player) player_pause(g_player, 0);
    bench_case("now playing, playing", seconds, max_wait);
    bad += bench_fb_check("now playing, playing");
    ui_show("albums");
    bad += bench_case("albums, playing", seconds, max_wait) != 0;   /* a list while music plays: nothing either */
    bad += bench_fb_check("albums, playing");
    ui_show("nowplaying");
    ui_set_awake(0);
    bad += bench_case("screen off, playing", seconds, max_wait) != 0;   /* at rest: nothing redrawn */
    if (g_player) player_pause(g_player, 1);
    bad += bench_case("screen off, paused", seconds, max_wait) != 0;   /* at rest: nothing redrawn */
    ui_set_awake(1);
    bench_case("woken, paused", 2, max_wait);
    bad += bench_fb_check("woken, paused");
    ui_show("songs");                              /* the wheel turned, a step every 60 ms */
    bench_scroll("songs, scrolling", seconds, max_wait);
    bad += bench_fb_check("songs, scrolling");
    return bad ? 1 : 0;
}

/* ---- interactive ---- */

static uint32_t sdl_ticks(void) { return SDL_GetTicks(); }

static int g_theme;  /* theme shown now; T cycles from here */

static void on_key(lv_event_t *e)
{
    uint32_t k = lv_event_get_key(e);
    switch (k) {
    case LV_KEY_UP: ui_key(UI_KEY_UP); break;
    case LV_KEY_DOWN: ui_key(UI_KEY_DOWN); break;
    case LV_KEY_ENTER: ui_key(UI_KEY_OK); break;
    case LV_KEY_ESC: case LV_KEY_BACKSPACE: ui_key(UI_KEY_BACK); break;
    case LV_KEY_LEFT: ui_key(UI_KEY_PREV); break;
    case LV_KEY_RIGHT: ui_key(UI_KEY_NEXT); break;
    case 'n': case 'N': ui_key(UI_KEY_BACK_LONG); break;
    case 'm': case 'M': ui_key(UI_KEY_MENU); break;
    case 'h': case 'H': ui_key(UI_KEY_MENU_LONG); break;
    case '[': ui_key(UI_KEY_DIMMER); break;
    case ',': ui_key(UI_KEY_SEEK_BACK); break;
    case '.': ui_key(UI_KEY_SEEK_FWD); break;
    case ']': ui_key(UI_KEY_BRIGHTER); break;
    case ' ': ui_key(UI_KEY_PLAY); break;
    case 't': case 'T':
        g_theme = (g_theme + 1) % ui_theme_count();
        ui_set_theme(g_theme);
        break;
    case 'q': case 'Q': exit(0);
    default: break;
    }
}

static void every_second(lv_timer_t *t) { (void)t; ui_tick_1s(); }
static void every_100ms(lv_timer_t *t) { (void)t; (void)ui_tick(); }

static void scan_progress(int done, int total, const char *path, void *ctx)
{
    (void)ctx;
    if (total > 0) fprintf(stderr, "\rscanning %d/%d  %-50.50s", done, total, path);
}

/* Screenshots: the second album, cued (not playing) */
static void cue_sample(void)
{
    if (!g_player || !g_lib) return;
    lib_list_t albums, tracks;
    if (library_list(g_lib, LIB_ALBUMS, 0, &albums) > 1 &&
        library_list(g_lib, LIB_ALBUM_TRACKS, albums.ids[1], &tracks) > 0) {
        player_cue(g_player, tracks.ids, tracks.n, 0);
        library_list_free(&tracks);
        if (getenv("FATHOM_SIM_PLAYING")) {      /* showcase pictures: playing, a little way in */
            player_pause(g_player, 0);
            usleep(300 * 1000);
            player_seek_by(g_player, 83000);
            usleep(300 * 1000);
            player_tick(g_player);
        }
    }
    library_list_free(&albums);
}

/* Library, engine and player. Screenshots use a silent output and a cued (not playing)
 * album; with FATHOM_SIM_PLAYING set, it plays (silently, in real time) from 1:23. */
static void setup_player(const char *music, int silent)
{
    mkdir("build/sim-data", 0755);
    settings_load(silent ? "" : "build/sim-data/settings.ini");   /* screenshots: defaults, never saved */
    sim_language_set(settings_get("display.language", ""));
    g_lib = library_open(DB_PATH, ART_DIR);
    if (!g_lib) return;
    library_set_playlist_dir(g_lib, "build/sim-data/playlists");
    if (music) {
        int n = library_scan(g_lib, music, scan_progress, NULL);
        fprintf(stderr, "\nlibrary: %d tracks\n", n);
    }
    static audio_output_t out;
    static out_null_stats_t stats;
    if (silent && (getenv("FATHOM_SIM_PLAYING") || g_bench)) out_clock_create(&out);   /* silent, in real time */
    else if (silent || out_sdl_create(&out)) out_null_create(&out, &stats);
    g_eng = engine_new(&out);
    g_player = g_eng ? player_new(g_lib, g_eng) : NULL;
    if (silent) cue_sample();
}

int main(int argc, char **argv)
{
    const char *shots = NULL, *motion_dir = NULL, *music = NULL, *lang_file = NULL;
    int theme = 0, zoom = 3, bench_s = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--screenshots") && i + 1 < argc) shots = argv[++i];
        else if (!strcmp(argv[i], "--motion") && i + 1 < argc) motion_dir = argv[++i];
        else if (!strcmp(argv[i], "--theme") && i + 1 < argc) theme = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--zoom") && i + 1 < argc) zoom = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--music") && i + 1 < argc) music = argv[++i];
        else if (!strcmp(argv[i], "--lang-check") && i + 1 < argc) lang_file = argv[++i];
        else if (!strcmp(argv[i], "--bench")) { bench_s = i + 1 < argc && atoi(argv[i + 1]) > 0 ? atoi(argv[++i]) : 5; g_bench = 1; }
        else { fprintf(stderr, "usage: %s [--theme N] [--zoom Z] [--music DIR] | --screenshots DIR | --motion DIR | --bench [S]\n", argv[0]); return 2; }
    }

    lv_init();
    static theme_t themes[UI_MAX_THEMES];
    g_n_themes = ui_load_themes(FATHOM_ASSETS_DIR, "build/sim-data/themes", themes, UI_MAX_THEMES);
    const ui_fonts_t *fonts = g_n_themes > 0 ? ui_theme_fonts(&themes[0]) : NULL;
    if (!fonts) return 1;
    setup_player(music, shots || motion_dir || bench_s || lang_file);
    if (bench_s) return bench(themes, fonts, bench_s);
    rdir_set_fetch(sim_dir_fetch);
    g_shots = shots != NULL;
    if (lang_file) { g_shots = 1; return lang_check(lang_file, themes, fonts); }
    if (shots) return screenshots(shots, themes, fonts);
    if (motion_dir) return motion(motion_dir, themes, fonts, theme);

    lv_tick_set_cb(sdl_ticks);
    lv_display_t *disp = lv_sdl_window_create(UI_W, UI_H);
    lv_sdl_window_set_zoom(disp, (uint8_t)(zoom < 1 ? 1 : zoom));
    lv_indev_t *kb = lv_sdl_keyboard_create();
    lv_group_t *g = lv_group_create();
    lv_indev_set_group(kb, g);

    ui_init(lv_screen_active(), themes, g_n_themes, fonts, 1, g_lib, g_player);
    ui_set_hooks(&g_hooks);
    ui_set_battery(86, 0);   /* the desktop has no H2 battery: a fixed sample value */
    g_theme = theme >= 0 && theme < g_n_themes ? theme : 0;
    ui_set_theme(g_theme);

    lv_obj_t *sink = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(sink);
    lv_obj_set_size(sink, 0, 0);
    lv_group_add_obj(g, sink);
    lv_obj_add_event_cb(sink, on_key, LV_EVENT_KEY, NULL);
    lv_timer_create(every_second, 1000, NULL);
    lv_timer_create(every_100ms, 100, NULL);

    for (;;) {
        uint32_t wait = lv_timer_handler();
        SDL_Delay(wait < 5 ? 5 : wait > 16 ? 16 : wait);
    }
}
