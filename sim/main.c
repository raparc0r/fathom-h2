/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
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
 *   fathom-sim --wifi-wait                     headless: a station chosen while Wi-Fi is still joining
 *   fathom-sim --type-test                     headless: typing with the wheel, the cursor moved with Prev and Next
 *   fathom-sim --ui-checks                     headless: the UI's defects found by reading, one check each
 *                                              plays once it has joined (a test)
 *
 * Keys: Up/Down = wheel, Enter = centre (Play), Esc/Backspace = Back,
 *       N = long-press Back (Now Playing), Space = play/pause,
 *       Left/Right = previous/next, M = Menu (context menu), H = hold Menu (quick settings),
 *       [ / ] = hold Menu + wheel (brightness), , / . = hold Prev / Next (seek),
 *       T = next theme, Q = quit.
 */
#include <FLAC/metadata.h>
#include <FLAC/stream_encoder.h>
#ifndef FATHOM_SIM_HEADLESS
#include <SDL.h>
#endif
#include <ftw.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <string.h>
#include <sys/stat.h>

#include "core/datetime.h"
#include "core/tz.h"
#include "core/lang.h"
#include "core/settings.h"
#include "audio/engine.h"
#include "audio/eq.h"
#include "audio/gain.h"
#include "ui/sound_text.h"
#include "audio/output.h"
#include "library/library.h"
#include "net/radiodir.h"
#include "net/podcast.h"
#include "net/podsync.h"
#include "net/coverfind.h"
#include "net/radioicon.h"
#include "audio/crossfeed.h"
#include "audio/width.h"
#include "lvgl.h"
#include "player/player.h"
#include "ui/theme.h"
#include "ui/assets.h"
#include "ui/fonts.h"
#include "ui/ui.h"
#include "core/clock.h"

#define DB_PATH  "build/sim-data/library.db"
#define ART_DIR  "build/sim-data/art"

static library_t *g_lib;
static int g_bench;         /* --bench, --ui-checks: the clock output, for real-time playing */
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
    return g_shots ? radio_load("assets/radio.txt", out, max) : radio_load_list(SIM_RADIO, "assets/radio.txt", out, max);
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
    if (strstr(url, "/byurl?")) return -1;            /* (a stream looked up for its logo: none here) */
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
/* Podcasts: feeds and Apple's search from tests/data/podcasts (no network
 * here); a download is a small stand-in file, a show's image the picture in
 * tests/data/covers. The screenshots start from sample shows of their own
 * (sim_pod_seed). */
static long sim_pod_fetch(const char *url, char *buf, size_t cap, const volatile int *abort)
{
    (void)abort;
    int feed = !strstr(url, "itunes.apple.com");
    const char *file = feed ? "tests/data/podcasts/feed.xml" : "tests/data/podcasts/search.json";
    FILE *f = fopen(file, "rb");
    if (!f) return -1;
    size_t n = fread(buf, 1, cap - 1, f);
    fclose(f);
    buf[n] = '\0';
    /* (the saved feed stops mid-item, and one that doesn't end is taken for
     * cut off and not read: ended here, as tests/test_podsync.c does) */
    if (feed) n += (size_t)snprintf(buf + n, cap - n, "\n</rss>\n");
    return (long)n;
}
static int64_t sim_pod_download(const char *url, const char *path, http_progress_fn progress, void *ctx,
                                const volatile int *abort)
{
    for (int i = 1; i <= 20 && !(abort && *abort); i++) {
        if (progress) progress((uint64_t)i * 5, 100, ctx);
        struct timespec t = {0, 100 * 1000 * 1000L};
        nanosleep(&t, NULL);
    }
    if (abort && *abort) return -1;
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fprintf(f, "%s\n", url);
    fclose(f);
    return 1;
}
static int64_t sim_cf_download(const char *url, const char *path, http_progress_fn progress, void *ctx,
                               const volatile int *abort);
static int64_t sim_pod_get(const char *url, const char *path, http_progress_fn progress, void *ctx,
                           const volatile int *abort)
{
    size_t n = strlen(url);
    if (n > 4 && !strcmp(url + n - 4, ".jpg")) return sim_cf_download(url, path, progress, ctx, abort);   /* a show's image */
    return sim_pod_download(url, path, progress, ctx, abort);
}
/* Covers found online: MusicBrainz "finds" the album asked for (its title and
 * artist read back from the question), and once more live; Apple nothing;
 * every picture is tests/data/covers/cover.jpg */
static void sim_unquote(const char *url, const char *field, char *out, size_t cap)
{
    char dec[1600];
    size_t o = 0;
    for (const char *p = url; *p && o + 1 < sizeof dec; p++) {
        if (*p == '%' && p[1] && p[2]) { char h[3] = {p[1], p[2], 0}; dec[o++] = (char)strtol(h, NULL, 16); p += 2; }
        else dec[o++] = *p;
    }
    dec[o] = '\0';
    out[0] = '\0';
    const char *f = strstr(dec, field);
    if (!f || f[strlen(field)] != '"') return;
    size_t k = 0;
    for (const char *p = f + strlen(field) + 1; *p && *p != '"' && k + 2 < cap; p++) {
        if (*p == '\\' && p[1]) p++;
        if (*p == '"' || *p == '\\') out[k++] = '\\';   /* (back into JSON) */
        out[k++] = *p;
    }
    out[k] = '\0';
}
static long sim_cf_fetch(const char *url, char *buf, size_t cap, const volatile int *abort)
{
    (void)abort;
    if (!strstr(url, "musicbrainz.org")) return snprintf(buf, cap, "{\"resultCount\":0,\"results\":[]}");
    char t[600], a[600];
    sim_unquote(url, "releasegroup:", t, sizeof t);
    sim_unquote(url, "artist:", a, sizeof a);
    return snprintf(buf, cap,
        "{\"release-groups\":[{\"id\":\"sim-1\",\"score\":100,\"title\":\"%s\",\"first-release-date\":\"2002\","
        "\"artist-credit\":[{\"name\":\"%s\"}]},{\"id\":\"sim-2\",\"score\":90,\"title\":\"%s (Live)\","
        "\"first-release-date\":\"2004\",\"artist-credit\":[{\"name\":\"%s\"}]}]}", t, a, t, a);
}
static int64_t sim_cf_download(const char *url, const char *path, http_progress_fn progress, void *ctx,
                               const volatile int *abort)
{
    (void)url; (void)progress; (void)ctx; (void)abort;
    FILE *in = fopen("tests/data/covers/cover.jpg", "rb"), *out = in ? fopen(path, "wb") : NULL;
    char b[8192];
    size_t n, total = 0;
    while (out && (n = fread(b, 1, sizeof b, in)) > 0) total += fwrite(b, 1, n, out);
    if (in) fclose(in);
    if (out) fclose(out);
    return out && total ? (int64_t)total : -1;
}
/* Radio stations' icons: every logo is the picture in tests/data/covers (no
 * network here). The screenshots fetch none: theirs are put there before
 * (sim_icon_seed), so each run shows the same. */
static int g_icons_seeded;
static long sim_icon_fetch(const char *url, char *buf, size_t cap, const volatile int *abort)
{
    (void)url; (void)abort;
    if (g_icons_seeded) return -1;
    FILE *f = fopen("tests/data/covers/cover.jpg", "rb");
    if (!f) return -1;
    size_t n = fread(buf, 1, cap - 1, f);
    fclose(f);
    buf[n] = '\0';
    return (long)n;
}
/* The example stations' icons for the screenshots: albums' covers from the
 * library stand in for the logos (after ricon_start) */
static void sim_icon_seed(const char *dir)
{
    char cmd[300];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    if (system(cmd)) {}
    mkdir(dir, 0755);
    g_icons_seeded = 1;
    radio_station_t st[8];
    int n = radio_load("assets/radio.txt", st, 8), at = 0;
    lib_list_t al;
    static uint16_t px[LIB_ART_LARGE * LIB_ART_LARGE], small[LIB_ART_SMALL * LIB_ART_SMALL];
    if (!g_lib || library_list(g_lib, LIB_ALBUMS, 0, &al) <= 0) return;
    for (int i = 0; i < n; i++) {
        char p[700];
        if (ricon_path(st[i].icon, p, sizeof p)) continue;
        for (at += 3; at < al.n; at++)             /* (not the ones the podcasts took) */
            if (library_art(g_lib, al.ids[at], LIB_ART_LARGE, px) == 0 && library_art(g_lib, al.ids[at], LIB_ART_SMALL, small) == 0) break;
        if (at >= al.n) break;
        /* FATHOM_SIM_FAVICON=file.png: the stations' logos are that picture (a
         * small one: how a site's favicon looks as Now Playing's cover; the
         * "radionp" screen then plays the first station that has a logo) */
        const char *fav = getenv("FATHOM_SIM_FAVICON");
        FILE *ff = fav ? fopen(fav, "rb") : NULL;
        if (ff) {
            static unsigned char enc[64 * 1024];
            size_t en = fread(enc, 1, sizeof enc, ff);
            fclose(ff);
            if (library_logo_decode(enc, en, px, small, NULL)) continue;
        }
        FILE *f = fopen(p, "wb");
        if (f) { fwrite(small, 1, sizeof small, f); fwrite(px, 1, sizeof px, f); fclose(f); }
    }
    library_list_free(&al);
}
/* Settings > Wi-Fi for the screenshots: connected, with a saved network, a new
 * one with a password and an open one in range (made-up names) */
static int shot_wifi_state(char *busy, size_t cap, ui_wifi_status_t *st)
{
    if (cap) busy[0] = '\0';
    if (st) *st = (ui_wifi_status_t){.state = UI_WIFI_CONNECTED, .ssid = "Harbour House", .ip = "192.168.1.24", .dbm = -52};
    return 1;
}
static int shot_wifi_networks(ui_wifi_net_t *out, int max)
{
    static const ui_wifi_net_t NET[] = {
        {"Harbour House", -52, 1, 1, 1}, {"Cafe Upstairs", -61, 1, 0, 0}, {"Old Workshop", -70, 1, 1, 0}, {"Library Guest", -80, 0, 0, 0}};
    int n = (int)(sizeof NET / sizeof NET[0]) < max ? (int)(sizeof NET / sizeof NET[0]) : max;
    for (int i = 0; i < n; i++) out[i] = NET[i];
    return n;
}
static void shot_wifi_do(int what, const char *ssid, const char *password) { (void)what; (void)ssid; (void)password; }

/* What's new, as a release's whatsnew.txt gives it (the screenshots; About's row) */
#define SIM_WHATS_NEW_VERSION "v0.14.1"
#define SIM_WHATS_NEW "Hotfix: Eliminates clicking sound when you skip, go back or seek\n" \
                      "Easier to read update and What's new screens\n"
static int sim_pod_opml(char *path, size_t cap)
{
    snprintf(path, cap, "build/sim-data/podcasts/subscriptions.opml");
    FILE *f = fopen(path, "rb");
    if (f) fclose(f);
    return f ? 1 : 0;
}
/* Sample shows for the screenshots: one read (three episodes: the newest not
 * on the card, one half heard and downloaded, one played), two with new
 * episodes; each with a picture (albums' covers from the library stand in
 * for the shows' images) */
static void sim_pod_seed(const char *dir)
{
    char cmd[300];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    if (system(cmd)) {}
    static const char *const SHOWS[][2] = {{"The Slow & Steady Show", "https://feeds.example.com/slow.xml"},
                                          {"Radiolab", "https://feeds.simplecast.com/EmVW7VGp"},
                                          {"Caf\xc3\xa9 Talk", "https://example.com/cafe.rss"}};
    for (int i = 0; i < 3; i++) {
        pod_show_t s = {0};
        snprintf(s.title, sizeof s.title, "%s", SHOWS[i][0]);
        snprintf(s.url, sizeof s.url, "%s", SHOWS[i][1]);
        pod_hash(s.url, s.id);
        pod_shows_add(dir, &s);
        s.refreshed = 1795064400;
        s.latest = 1795064400;
        pod_show_save(dir, &s);
        pod_show_seen(dir, s.id, i == 0 ? 1795064400 : 1795000000);
    }
    lib_list_t al;
    static uint16_t px[LIB_ART_LARGE * LIB_ART_LARGE], small[LIB_ART_SMALL * LIB_ART_SMALL];
    if (g_lib && library_list(g_lib, LIB_ALBUMS, 0, &al) > 0) {
        int show = 0;
        for (int i = 0; i < al.n && show < 3; i++) {
            if (library_art(g_lib, al.ids[i], LIB_ART_LARGE, px) || library_art(g_lib, al.ids[i], LIB_ART_SMALL, small)) continue;
            char id[13], p[800];
            pod_hash(SHOWS[show++][1], id);
            pod_cover_path(dir, id, p, sizeof p);
            FILE *c = fopen(p, "wb");
            if (c) { fwrite(px, 1, sizeof px, c); fclose(c); }
            pod_thumb_path(dir, id, p, sizeof p);
            c = fopen(p, "wb");
            if (c) { fwrite(small, 1, sizeof small, c); fclose(c); }
        }
        library_list_free(&al);
    }
    char buf[8192];
    FILE *f = fopen("tests/data/podcasts/feed.xml", "rb");
    size_t n = f ? fread(buf, 1, sizeof buf - 1, f) : 0;
    if (f) fclose(f);
    pod_show_t s = {0};
    pod_episode_t e[8];
    snprintf(s.url, sizeof s.url, "%s", SHOWS[0][1]);
    pod_hash(s.url, s.id);
    int ne = pod_parse_feed(buf, n, &s, e, 8);
    if (ne < 3) return;
    pod_episodes_save(dir, s.id, e, ne);
    pod_state_t st = {0};
    snprintf(st.key, sizeof st.key, "%s", e[1].key);
    st.pos_ms = 23 * 60 * 1000;
    st.len_ms = (uint32_t)e[1].dur_s * 1000;
    pod_state_put(dir, s.id, &st);
    snprintf(st.key, sizeof st.key, "%s", e[2].key);
    st.pos_ms = 0;
    st.played = 1;
    st.played_at = 1795064400;
    pod_state_put(dir, s.id, &st);
    char p[800];
    pod_episode_path(dir, s.id, &e[1], p, sizeof p);
    f = fopen(p, "wb");
    if (f) { fputs("stand-in", f); fclose(f); }
    /* and its chapters, as fetched with the download */
    pod_chapters_path(dir, s.id, e[1].key, p, sizeof p);
    FILE *in = fopen("tests/data/podcasts/chapters.json", "rb"), *out = fopen(p, "wb");
    size_t k;
    while (in && out && (k = fread(buf, 1, sizeof buf, in)) > 0) fwrite(buf, 1, k, out);
    if (in) fclose(in);
    if (out) fclose(out);
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
static int g_sim_bal, g_sim_limit, g_sim_xf, g_sim_width = 3;
static void sim_sound_value(int what, char *out, size_t cap)
{
    /* (the H2's own words: ui/sound_text.h) */
    if (what == UI_SOUND_CROSSFEED) snprintf(out, cap, "%s", tr(crossfeed_name(g_sim_xf)));
    else if (what == UI_SOUND_WIDTH) sound_text_width(WIDTH_STEPS[g_sim_width], out, cap);
    else if (what == UI_SOUND_BALANCE) sound_text_balance(g_sim_bal, out, cap);
    else sound_text_limit_db(g_sim_limit, out, cap);
}
static int sim_sound_adjust(int what, int dir)
{
    if (what == UI_SOUND_CROSSFEED) {
        g_sim_xf = (g_sim_xf + (dir < 0 ? XF_LEVELS - 1 : 1)) % XF_LEVELS;
        if (g_eng) engine_set_crossfeed(g_eng, g_sim_xf);
        return 0;
    }
    if (what == UI_SOUND_WIDTH) {
        g_sim_width = g_sim_width + dir < 0 ? 0 : g_sim_width + dir >= WIDTH_N_STEPS ? WIDTH_N_STEPS - 1 : g_sim_width + dir;
        if (g_eng) engine_set_width(g_eng, WIDTH_STEPS[g_sim_width]);
        return 0;
    }
    if (what == UI_SOUND_BALANCE) {
        g_sim_bal = g_sim_bal + dir < -12 ? -12 : g_sim_bal + dir > 12 ? 12 : g_sim_bal + dir;
        float l, r;
        balance_gains(g_sim_bal, &l, &r);
        if (g_eng) engine_set_balance(g_eng, l, r);
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

/* Settings > Interface > Language packs, with a stand-in for the download:
 * asked for, it is "downloading" (42 %) until the screenshots or a check say
 * it has come; removed, it is absent */
#define SIM_PACKS 4
static ui_pack_t g_sim_packs[SIM_PACKS] = {
    {.id = "ja", .name = "Japanese", .state = UI_PACK_ABSENT, .bytes = 3304868, .han = 1},
    {.id = "ko", .name = "Korean", .state = UI_PACK_ABSENT, .bytes = 2553390},
    {.id = "zh-hans", .name = "Chinese (Simplified)", .state = UI_PACK_ABSENT, .bytes = 3282028, .han = 1, .brief = "Simplified"},
    {.id = "zh-hant", .name = "Chinese (Traditional)", .state = UI_PACK_ABSENT, .bytes = 6637170, .han = 1, .brief = "Traditional"}};
static const char *const SIM_PACK_ID[SIM_PACKS] = {"ja", "ko", "zh-hans", "zh-hant"};
static int g_sim_pack_did = -1, g_sim_pack_at = -1;   /* the last packs_do, and for which pack (tests) */
static int sim_packs_state(ui_pack_t *out, int cap)
{
    int n = cap < SIM_PACKS ? cap : SIM_PACKS;
    for (int i = 0; i < n; i++) out[i] = g_sim_packs[i];
    return n;
}
static void advance(uint32_t ms);              /* (time stepped by hand: below) */
static void sim_packs_all(int state)
{
    for (int k = 0; k < SIM_PACKS; k++) {
        g_sim_packs[k].state = state;
        g_sim_packs[k].first = 0;
        g_sim_packs[k].songs = 0;
    }
}
static void sim_packs_do(int i, int what)
{
    if (i < 0 || i >= SIM_PACKS) return;
    g_sim_pack_did = what;
    g_sim_pack_at = i;
    if (what == UI_PACK_FIRST) {                /* its shapes for the shared characters */
        for (int k = 0; k < SIM_PACKS; k++) g_sim_packs[k].first = k == i;
        ui_packs_first(SIM_PACK_ID[i]);
        ui_packs_changed(1);
        return;
    }
    g_sim_packs[i].message[0] = 0;
    g_sim_packs[i].state = what == UI_PACK_GET ? UI_PACK_GETTING : UI_PACK_ABSENT;
    g_sim_packs[i].percent = 42;
    ui_packs_changed(what == UI_PACK_REMOVE);
}
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
/* Settings > Sound > EQ: one profile, an AutoEQ one (for the EQ editor's screens) */
static const char SIM_EQ[] = "Preamp: -6.0 dB\n"
                             "Filter 1: ON LSC Fc 105 Hz Gain 2.6 dB Q 0.70\n"
                             "Filter 2: ON PK Fc 514 Hz Gain -4.4 dB Q 0.68\n"
                             "Filter 3: ON PK Fc 8903 Hz Gain 6.0 dB Q 1.66\n"
                             "Filter 4: ON PK Fc 183 Hz Gain 2.0 dB Q 0.77\n"
                             "Filter 5: ON PK Fc 4613 Hz Gain 3.4 dB Q 2.46\n"
                             "Filter 6: ON HSC Fc 10000 Hz Gain -0.5 dB Q 0.70\n"
                             "Filter 7: ON PK Fc 1517 Hz Gain -1.0 dB Q 2.33\n"
                             "Filter 8: ON PK Fc 929 Hz Gain 1.2 dB Q 2.75\n"
                             "Filter 9: ON PK Fc 44 Hz Gain -0.5 dB Q 2.16\n"
                             "Filter 10: ON PK Fc 618 Hz Gain -0.5 dB Q 2.86\n";
static int sim_eq_list(char names[][64], int max)
{
    if (max < 1) return 0;
    snprintf(names[0], 64, "AirPods Pro");
    return 1;
}
static int sim_eq_get(const char *name, eq_profile_t *p)
{
    char err[128];
    (void)name;
    return eq_parse(SIM_EQ, p, err, sizeof err);
}
static int sim_eq_put(const char *name, const eq_profile_t *p) { (void)name; (void)p; return 0; }
static void sim_eq_preview(const eq_profile_t *p) { (void)p; }

/* The theme editor: themes saved in the simulator's own themes folder */
#define SIM_THEMES "build/sim-data/themes"
static theme_t *g_themes;
static int sim_theme_save(const theme_t *t, const char *file, char *saved, size_t cap)
{
    return ui_theme_write(SIM_THEMES, t, file, saved, cap);
}
static int sim_themes_reload(void) { return g_n_themes = ui_load_themes(FATHOM_ASSETS_DIR, SIM_THEMES, g_themes, UI_MAX_THEMES); }
static int sim_theme_remove(const char *file) { return ui_theme_delete(SIM_THEMES, file); }

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

/* About's first rows, as on the H2 (the rest is the device's) */
/* The clock as set in About > Clock (0: not set, as on a new H2; the
 * computer's own clock is left alone) */
static int64_t g_sim_clock;

static int sim_clock_set(int64_t t)
{
    g_sim_clock = t;
    return 0;
}

static int sim_about(int i, char *label, size_t lc, char *value, size_t vc)
{
    if (i > 2) return -1;
    if (i == 2) {
        snprintf(label, lc, "Clock");
        if (g_sim_clock) ui_clock_text(g_sim_clock, value, vc);   /* (the device's own words) */
        else snprintf(value, vc, "Not set: <Play> sets it");
        return 0;
    }
    snprintf(label, lc, "%s", i == 0 ? UI_ABOUT_CREDIT : "Version");
    snprintf(value, vc, "%s", i == 0 ? "" : "v0.15.0");
    return 0;
}

/* Rescan's screen: a scan made up, at one step (the screenshots) */
static ui_scan_t g_sim_scan;
static int g_sim_scanning;
static int sim_scan_progress(ui_scan_t *p)
{
    if (!g_sim_scanning) return 0;
    *p = g_sim_scan;
    return 1;
}
static void sim_rescan(void) { g_sim_scanning = 1; }

static const ui_hooks_t g_hooks = {.about = sim_about, .rescan = sim_rescan, .scan_progress = sim_scan_progress, .clock_set = sim_clock_set, .fonts = ui_theme_fonts, .icon = ui_theme_icon, .radio_stations = sim_radio,
                                   .radio_add = sim_radio_add, .radio_remove = sim_radio_remove,
                                   .share_state = sim_share_state, .share_do = sim_share_do,
                                   .ssh_state = sim_ssh_state, .ssh_do = sim_ssh_do,
                                   .sound_value = sim_sound_value, .sound_adjust = sim_sound_adjust,
                                   .pure_toggle = sim_pure_toggle, .signal_path = sim_signal_path,
                                   .transport_toggle = sim_transport_toggle, .battery_info = sim_battery_info,
                                   .languages = sim_languages, .language_set = sim_language_set,
                                   .update_state = sim_update_state, .update_do = sim_update_do,
                                   .packs_state = sim_packs_state, .packs_do = sim_packs_do,
                                   .uninstall_state = sim_uninstall_state, .uninstall_do = sim_uninstall_do,
                                   .eq_list = sim_eq_list, .eq_get = sim_eq_get, .eq_put = sim_eq_put,
                                   .eq_preview = sim_eq_preview,
                                   .podcast_opml = sim_pod_opml,
                                   .theme_save = sim_theme_save, .themes_reload = sim_themes_reload,
                                   .theme_remove = sim_theme_remove,
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
    ui_set_new_install(getenv("SIM_NEW_INSTALL") != NULL);   /* (a fresh install's Home) */
    ui_set_music_missing(getenv("SIM_NO_MUSIC") != NULL);   /* (no Music folder) */
    ui_init(lv_screen_active(), themes, g_n_themes, fonts, 1, g_lib, g_player);
    ui_set_hooks(&g_hooks);
    ui_set_battery(86, 1);   /* the desktop has no H2 battery: a fixed sample value, charging (two icons) */
    ui_set_bt_link(UI_BT_LINK_CONNECTED);   /* and headphones, so the header shows every kind of icon */

    /* FATHOM_SIM_FLOW_FRAMES=1: not the screenshots but the cover flow in
     * motion, a picture every 40 ms into dir (frame-0000.bmp on), for a GIF
     * of it. FATHOM_SIM_THEME: which theme, 0 on.
     * FATHOM_SIM_FLOW_SCRIPT: what the wheel does, read left to right:
     *   >N   to Albums' Nth album (0: the first) before anything is recorded
     *   +    a turn on, and a picture      -   a turn back, and a picture
     *   .N   N pictures with nothing turned
     * so "+.10" is a turn every 440 ms, "+++" three turns 40 ms apart (six
     * that close and the rest go a letter at a time), ".1+" one 80 ms after
     * the last (slow enough to count as one album again). Without it, the
     * sample library's Debut (69) on by four, a quick spin to M, the wheel
     * slowing to Maxinquaye (156), and back by two. */
    if (getenv("FATHOM_SIM_FLOW_FRAMES")) {
        int th = getenv("FATHOM_SIM_THEME") ? atoi(getenv("FATHOM_SIM_THEME")) : 0;
        const char *sc = getenv("FATHOM_SIM_FLOW_SCRIPT");
        if (!sc) sc = ">69 .25 +.10 +.10 +.10 +.10 .6 +++++++++++++++ .1+ .1+ .2+ .2+ .3+ .4+ .30 -.12 -.12 .35";
        ui_set_theme(th >= 0 && th < g_n_themes ? th : 0);
        settings_set_int("display.album_flow", 1);
        ui_set_battery(86, 0);                          /* (not charging: its mark pulses, and every frame would differ) */
        ui_show("albums");
        ui_key(UI_KEY_UP);                              /* (the sample's second album: from the first) */
        int n = 0;
        for (; *sc; sc++) {
            int key = *sc == '+' || *sc == '-', idle = 0;
            if (*sc == '>' && !n) {
                /* (100 ms apart: each turn one album, however many there are) */
                for (int to = (int)strtol(sc + 1, (char **)&sc, 10); to > 0; to--) {
                    lv_tick_inc(100);
                    lv_timer_handler();
                    ui_key(UI_KEY_DOWN);
                }
                sc--;
                continue;
            }
            if (*sc == '.') { idle = (int)strtol(sc + 1, (char **)&sc, 10); sc--; }
            if (!key && !idle) continue;
            if (!n) for (int i = 0; i < 80; i++) { lv_tick_inc(20); lv_timer_handler(); }   /* (at rest first, its covers read) */
            for (int f = 0; f < (key ? 1 : idle); f++, n++) {
                if (key) ui_key(*sc == '+' ? UI_KEY_DOWN : UI_KEY_UP);
                /* (LVGL's clock is stepped here, 40 ms a frame in two halves: the
                 * slides move by it, whatever the computer's own time) */
                lv_tick_inc(20);
                lv_timer_handler();
                lv_tick_inc(20);
                lv_timer_handler();
                lv_obj_update_layout(lv_screen_active());
                lv_draw_buf_t *img = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB888);
                char path[1024];
                snprintf(path, sizeof path, "%s/frame-%04d.bmp", dir, n);
                if (img) { write_bmp(path, img); lv_draw_buf_destroy(img); }
            }
        }
        printf("%d frames in %s\n", n, dir);
        return 0;
    }

    static const char *const screens[] = {"home", "albums", "tracks", "playlists", "playlist", "nowplaying", "themes",
                                          "quick", "context", "info", "queue", "glyphs", "search",
                                          "artists", "artist", "artistsongs", "albumartists", "homeedit",
                                          "usbask", "wifiask", "interface", "packs", "packsgetting", "packsthere", "packsfailed", "packshelp", "packsshared", "homepacks", "packsneeded", "usbdrive", "scrub", "sound", "radio", "radionp", "share", "ssh", "toast", "abouthelp", "genres", "folders", "folder", "years", "homeend", "homeall", "signal", "purenp",
                                          "rdir", "rdirgenres", "rdirresults", "rdirbusy", "rdircountries", "rdirsearch",
                                          "rdirctx", "rdirerror", "rdirnp", "rdirnpmenu", "rdirrandom",
                                          "about", "clockset", "clockdone", "update", "updatelatest", "updatefailed", "updateprogress",
                                          "updateready", "updateinstall", "battery", "battery7", "batterysince",
                                          "batteryhistory", "battery7-tokyo", "batteryhistory-tokyo", "uninstall", "uninstallchecking", "uninstallfailed",
                                          "uninstallnone", "uninstallpending", "homeupdate", "askupdate", "askignore", "askinstall", "askinstallwn", "eqedit", "eqband",
                                          "themesnew", "themeedit", "themecolours", "themecolour", "themesave",
                                          "podcasts", "podshow", "podsearch", "podnp", "podchapters", "podmenu", "podsettings",
                                          "settings", "wifi", "libsettings", "libhelp", "libunread", "unread", "unreadwhere", "xfhelp", "whatsnew", "whatsnewend", "aboutwn", "covermenu", "covermenuown", "coverfind",
                                          "albummenu", "artistmenu", "aartistmenu", "homemove",
                                          "smartlists", "smartopen", "smartmenu",
                                          "cv-tracks", "cv-artist", "cv-artistsongs", "cv-playlist", "cv-queue",
                                          "welcome", "welcomeend", "abouttips", "rescanlooking", "rescanchecking", "rescan", "rescanalbums", "rescanworking", "rescancovers"};
    static const char *const slug[] = {"studio-mint", "studio-ember", "studio-pastel", "pixel-pastel"};
    for (int t = 0; t < 4 && t < g_n_themes; t++) {   /* the built-in themes */
        ui_set_theme(t);
        for (size_t s = 0; s < sizeof screens / sizeof screens[0]; s++) {
            if (s > 0 && !strcmp(screens[s - 1], "wifi")) {   /* (its made-up networks: for that screen only) */
                ui_set_hooks(&g_hooks);
                ui_wifi_changed();
            }
            lv_obj_t *sample = NULL;
            if (g_sim_scanning) { g_sim_scanning = 0; ui_show_scan(); }   /* (the last one's put away) */
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
                    int at = 0;
                    for (int i = 0; getenv("FATHOM_SIM_FAVICON") && i < n; i++)
                        if (st[i].icon[0]) { at = i; break; }
                    player_radio_play(g_player, st, n, at);
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
            } else if (!strncmp(screens[s], "rescan", 6)) {   /* Settings > Library > Rescan, at each step */
                ui_show("libsettings");
                memset(&g_sim_scan, 0, sizeof g_sim_scan);
                if (!strcmp(screens[s], "rescanlooking")) g_sim_scan = (ui_scan_t){.step = UI_SCAN_LOOKING, .done = 3204};
                else if (!strcmp(screens[s], "rescanchecking")) g_sim_scan = (ui_scan_t){.step = UI_SCAN_LOOKING, .done = 118, .total = -1};
                else if (!strcmp(screens[s], "rescan")) {
                    g_sim_scan = (ui_scan_t){.step = UI_SCAN_SONGS, .done = 1225, .total = 9812};
                    snprintf(g_sim_scan.file, sizeof g_sim_scan.file, "%s", "07 - The Long Way Round (Live at the Roundhouse).flac");
                } else if (!strcmp(screens[s], "rescanalbums")) g_sim_scan = (ui_scan_t){.step = UI_SCAN_ALBUMS, .done = 6120, .total = 9812};
                else if (!strcmp(screens[s], "rescanworking")) g_sim_scan = (ui_scan_t){.step = UI_SCAN_ALBUMS, .done = 7, .total = -1};
                else g_sim_scan = (ui_scan_t){.step = UI_SCAN_COVERS, .done = 12, .total = 31};
                g_sim_scanning = 1;
                ui_show_scan();
            } else if (!strcmp(screens[s], "purenp")) {    /* Pure mode: the badge */
                sim_pure_toggle();
                ui_show("nowplaying");
            } else if (!strcmp(screens[s], "usbdrive")) {
                ui_show("nowplaying");
                sample = ui_message_screen("USB drive",
                                           "Eject the player on your computer, or unplug the cable, to go back to your music.");
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
            } else if (!strcmp(screens[s], "homepacks") || !strcmp(screens[s], "packsneeded")) {
                /* the library has names in Japanese and Korean and neither pack is there:
                 * Home's "Get language packs", and the page it opens */
                sim_packs_all(UI_PACK_ABSENT);
                g_sim_packs[0].songs = 342;
                g_sim_packs[1].songs = 27;
                settings_set("packs.ignored", "");
                ui_packs_changed(0);
                ui_show("home");
                for (int k = 0; k < 12; k++) ui_key(UI_KEY_UP);   /* the top: the entry */
                if (!strcmp(screens[s], "packsneeded")) ui_key(UI_KEY_OK);
                ui_set_theme(t);                            /* (the slide finished) */
            } else if (!strncmp(screens[s], "packs", 5)) {       /* Language packs, in each state */
                const char *is = screens[s] + 5;
                g_sim_packs[0].state = !strcmp(is, "getting") ? UI_PACK_GETTING : !strcmp(is, "there") ? UI_PACK_THERE
                                       : !strcmp(is, "failed") ? UI_PACK_FAILED : UI_PACK_ABSENT;
                g_sim_packs[0].percent = 42;
                g_sim_packs[0].message[0] = 0;
                for (int k = 1; k < SIM_PACKS; k++) g_sim_packs[k].state = UI_PACK_ABSENT;
                if (!strcmp(is, "shared")) {                 /* Japanese and both Chinese there: the choice's row */
                    g_sim_packs[0].state = g_sim_packs[2].state = g_sim_packs[3].state = UI_PACK_THERE;
                    g_sim_packs[2].first = 1;
                }
                ui_show("packs");
                if (!strcmp(is, "help")) ui_key(UI_KEY_MENU);    /* what the row is for */
                if (!strcmp(is, "shared")) for (int k = 0; k < SIM_PACKS; k++) { ui_key(UI_KEY_DOWN); advance(150); }   /* onto that row */
                sim_packs_all(UI_PACK_ABSENT);
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
            } else if (!strcmp(screens[s], "homeupdate") || !strcmp(screens[s], "askupdate") ||
                       !strcmp(screens[s], "askignore") || !strncmp(screens[s], "askinstall", 10)) {
                /* a newer version found by the check: first on Home, and its page
                 * (the update's stand-in back at rest: its progress screen takes the keys) */
                g_sim_upd.state = UI_UPD_IDLE;
                ui_update_changed();
                settings_set("update.found", "v0.11.0");
                settings_set("update.ignored", "");
                /* "askinstallwn": as a release has it, What's new in About first */
                if (!strcmp(screens[s], "askinstallwn")) ui_set_whats_new(SIM_WHATS_NEW_VERSION, SIM_WHATS_NEW, 0);
                ui_show("home");
                for (int k = 0; k < 12; k++) ui_key(UI_KEY_UP);   /* the top: the entry */
                ui_key(!strcmp(screens[s], "homeupdate") ? UI_KEY_DOWN : UI_KEY_OK);
                if (!strcmp(screens[s], "askignore")) ui_key(UI_KEY_DOWN), ui_key(UI_KEY_OK);   /* Ignore: Home, without it */
                if (!strncmp(screens[s], "askinstall", 10)) ui_key(UI_KEY_OK);   /* Install: About > Updates */
                ui_set_theme(t);                            /* (the slide finished) */
                settings_set("update.found", "");
            } else if (!strcmp(screens[s], "wifi")) {      /* Settings > Wi-Fi, with made-up networks in range */
                ui_hooks_t h = g_hooks;
                h.wifi_state = shot_wifi_state;
                h.wifi_networks = shot_wifi_networks;
                h.wifi_do = shot_wifi_do;
                ui_set_hooks(&h);
                ui_wifi_changed();
                ui_show("wifi");
                ui_key(UI_KEY_DOWN);
                ui_key(UI_KEY_DOWN);
                ui_key(UI_KEY_DOWN);
                ui_key(UI_KEY_DOWN);                         /* on the new network: its arrow */
                ui_set_theme(t);
            } else if (!strncmp(screens[s], "whatsnew", 8)) {   /* after an update: the release's points (and turned to the end) */
                ui_show("home");
                /* FATHOM_SIM_WHATSNEW: a release's whatsnew.txt instead (its version, then its points) */
                char wv[32] = "", wt[1024] = "";
                FILE *wf = getenv("FATHOM_SIM_WHATSNEW") ? fopen(getenv("FATHOM_SIM_WHATSNEW"), "r") : NULL;
                if (wf) {
                    if (fgets(wv, sizeof wv, wf)) wv[strcspn(wv, "\r\n")] = '\0';
                    wt[fread(wt, 1, sizeof wt - 1, wf)] = '\0';
                    fclose(wf);
                }
                ui_set_whats_new(wt[0] ? wv : SIM_WHATS_NEW_VERSION, wt[0] ? wt : SIM_WHATS_NEW, 1);
                if (!strcmp(screens[s], "whatsnewend")) for (int k = 0; k < 8; k++) ui_key(UI_KEY_DOWN);
            } else if (!strcmp(screens[s], "aboutwn")) {  /* Settings > About, its What's new row */
                ui_set_whats_new(SIM_WHATS_NEW_VERSION, SIM_WHATS_NEW, 0);
                ui_show("about");
            } else if (!strcmp(screens[s], "homemove")) {  /* Settings > Home: Menu > Move, two down (the pill with it) */
                ui_show("homeedit");
                ui_key(UI_KEY_MENU);
                ui_key(UI_KEY_OK);
                ui_key(UI_KEY_DOWN);
                ui_key(UI_KEY_DOWN);
                ui_set_theme(t);                            /* (the pill's slide finished) */
            } else if (!strcmp(screens[s], "xfhelp")) {   /* Menu on Sound > Crossfeed: its help */
                ui_show("sound");
                ui_key(UI_KEY_DOWN);
                ui_key(UI_KEY_MENU);
            } else if (!strcmp(screens[s], "libhelp")) {  /* Menu on Find missing covers: its help */
                ui_show("libsettings");
                ui_key(UI_KEY_MENU);
            } else if (!strcmp(screens[s], "abouthelp")) {   /* About's credit, and Menu: the legal notice */
                ui_show("about");
                for (int k = 0; k < 8; k++) ui_key(UI_KEY_UP);
                ui_key(UI_KEY_MENU);
            } else if (!strncmp(screens[s], "welcome", 7)) {   /* a fresh install's (and turned to the end) */
                ui_show("home");
                ui_show_welcome();
                if (!strcmp(screens[s], "welcomeend")) for (int k = 0; k < 8; k++) ui_key(UI_KEY_DOWN);
            } else if (!strcmp(screens[s], "abouttips")) {     /* Settings > About, its Tips row */
                ui_show("about");
            } else if (!strncmp(screens[s], "cv-", 3)) {   /* Covers in lists: Albums, songs (then back) */
                settings_set_int("display.covers", 3);
                ui_show(screens[s] + 3);
            } else if (!strcmp(screens[s], "clockdone")) {   /* About > Clock set by hand, with the keys: checked */
                g_sim_clock = 0;
                settings_set("clock.tz", "UTC");
                int64_t t0 = (int64_t)time(NULL);       /* (where it starts: the computer's clock) */
                ui_show("about");                       /* (on Check for updates: the Clock row is above) */
                static const ui_key_t keys[] = {
                    UI_KEY_UP, UI_KEY_OK,               /* Date and time, from the clock as it is, in UTC */
                    UI_KEY_BACK,                        /* left at once: nothing set */
                    UI_KEY_OK,
                    UI_KEY_OK, UI_KEY_DOWN, UI_KEY_OK,  /* the year: one on */
                    UI_KEY_DOWN, UI_KEY_OK, UI_KEY_UP, UI_KEY_BACK,   /* the month: one back (Back keeps it) */
                    UI_KEY_DOWN, UI_KEY_OK, UI_KEY_UP, UI_KEY_OK,     /* the day: one back */
                    UI_KEY_DOWN, UI_KEY_OK, UI_KEY_DOWN, UI_KEY_DOWN, UI_KEY_OK,   /* the hours: two on */
                    UI_KEY_DOWN, UI_KEY_OK, UI_KEY_UP, UI_KEY_OK,     /* the minutes: one back */
                    UI_KEY_DOWN, UI_KEY_DOWN, UI_KEY_OK, UI_KEY_DOWN, UI_KEY_DOWN, UI_KEY_OK,   /* the zone: two on (no further) */
                    UI_KEY_BACK};                       /* left: set, the time typed taken as that zone's */
                int left = 1;
                for (size_t q = 0; q < sizeof keys / sizeof keys[0]; q++) {
                    ui_key(keys[q]);
                    if (q == 2) left = g_sim_clock == 0;
                }
                int64_t t1 = (int64_t)time(NULL), want[2];
                int zone = tz_find(TZ_ZONES[2].name);   /* (UTC, then two on) */
                for (int w = 0; w < 2; w++) {           /* (the minute may have turned meanwhile) */
                    dt_t dt;
                    dt_from_time(w ? t1 : t0, &dt);
                    dt_adjust(&dt, DT_YEAR, 1);
                    dt_adjust(&dt, DT_MONTH, -1);
                    dt_adjust(&dt, DT_DAY, -1);
                    dt_adjust(&dt, DT_HOUR, 1);
                    dt_adjust(&dt, DT_HOUR, 1);
                    dt_adjust(&dt, DT_MIN, -1);
                    want[w] = tz_to_utc(zone, dt_to_time(&dt));
                }
                if (!left || (g_sim_clock != want[0] && g_sim_clock != want[1]) ||
                    strcmp(settings_get("clock.tz", ""), TZ_ZONES[2].name)) {
                    fprintf(stderr, "clock set by hand: %lld in %s, expected %lld in %s (left unset first: %d)\n",
                            (long long)g_sim_clock, settings_get("clock.tz", ""), (long long)want[0], TZ_ZONES[2].name, left);
                    return 1;
                }
                /* a zone alone: the clock left as it is */
                int64_t before = g_sim_clock;
                ui_show("about");
                static const ui_key_t zkeys[] = {UI_KEY_UP, UI_KEY_OK, UI_KEY_DOWN, UI_KEY_DOWN, UI_KEY_DOWN, UI_KEY_DOWN,
                                                 UI_KEY_DOWN, UI_KEY_OK, UI_KEY_UP, UI_KEY_OK, UI_KEY_BACK};
                for (size_t q = 0; q < sizeof zkeys / sizeof zkeys[0]; q++) ui_key(zkeys[q]);
                if (g_sim_clock != before || strcmp(settings_get("clock.tz", ""), TZ_ZONES[1].name)) {
                    fprintf(stderr, "a time zone alone: clock %lld (was %lld), zone %s, expected %s\n", (long long)g_sim_clock,
                            (long long)before, settings_get("clock.tz", ""), TZ_ZONES[1].name);
                    return 1;
                }
                ui_show("about");                       /* (its Clock row: the time set, in that zone) */
            } else if (strstr(screens[s], "-tokyo")) {   /* in a time zone (UTC+9): its days and hours */
                char base[32];
                snprintf(base, sizeof base, "%.*s", (int)(strstr(screens[s], "-tokyo") - screens[s]), screens[s]);
                settings_set("clock.tz", "Tokyo, Seoul");
                ui_show(base);
            } else if (!strcmp(screens[s], "toast")) {    /* a long one: it wraps */
                ui_show("ssh");
                ui_toast("Over USB and Wi-Fi, with the password", 3000);
            } else {
                ui_show(screens[s]);
            }
            int on_top = !strcmp(screens[s], "usbdrive") || !strcmp(screens[s], "toast") || !strcmp(screens[s], "unreadwhere") || !strcmp(screens[s], "libhelp") || !strcmp(screens[s], "packshelp") || !strcmp(screens[s], "xfhelp") ||
                         !strncmp(screens[s], "whatsnew", 8) || !strncmp(screens[s], "welcome", 7) || !strcmp(screens[s], "abouthelp") ||
                         !strcmp(screens[s], "clockset") || !strncmp(screens[s], "rescan", 6) ||
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
            if (!strncmp(screens[s], "cv-", 3)) settings_set_int("display.covers", UI_COVERS_DEFAULT);
            if (!strcmp(screens[s], "clockdone") || strstr(screens[s], "-tokyo")) settings_set("clock.tz", "UTC");
            if (!strncmp(screens[s], "whatsnew", 8) || !strncmp(screens[s], "welcome", 7)) ui_key(UI_KEY_BACK);   /* (put away: Back) */
            if (!strcmp(screens[s], "homemove")) {          /* (back where it was, and done) */
                ui_key(UI_KEY_UP);
                ui_key(UI_KEY_UP);
                ui_key(UI_KEY_OK);
            }
            if (!strncmp(screens[s], "whatsnew", 8) || !strcmp(screens[s], "aboutwn") || !strcmp(screens[s], "askinstallwn"))
                ui_set_whats_new("", "", 0);
            if (!strcmp(screens[s], "toast") || !strcmp(screens[s], "unreadwhere") || !strcmp(screens[s], "libhelp") || !strcmp(screens[s], "packshelp") || !strcmp(screens[s], "xfhelp") || !strcmp(screens[s], "abouthelp") ||
                !strcmp(screens[s], "clockset"))   /* (gone before the next screens) */
                for (uint32_t c = 0; c < lv_obj_get_child_count(lv_layer_top()); c++)
                    lv_obj_add_flag(lv_obj_get_child(lv_layer_top(), (int32_t)c), LV_OBJ_FLAG_HIDDEN);
            if (!strcmp(screens[s], "homepacks") || !strcmp(screens[s], "packsneeded")) {   /* (the offer gone before the next screens) */
                sim_packs_all(UI_PACK_ABSENT);
                ui_packs_changed(0);
            }
            printf("%s\n", path);
        }
        /* Albums as cover flow (Settings > Interface > Cover flow): at rest some
         * way into Albums, at the list's start, and half way from one album to
         * the next ("flow", "flowstart", "flowslide") */
        for (int m = 0; m < 6; m++) {
            /* ("flowartist", "flowartistalbums": set to Inside artists, an artist's albums are a cover flow and Albums a list) */
            static const char *const NAME[] = {"flow", "flowstart", "flowslide", "flowfast", "flowartist", "flowartistalbums"};
            if (g_sim_scanning) { g_sim_scanning = 0; ui_show_scan(); }   /* (the last screen's put away) */
            settings_set_int("display.album_flow", m >= 4 ? 2 : 1);
            ui_show(m == 4 ? "artist" : "albums");
            for (int k = 0; m == 3 && k < 12; k++) {       /* quick turns: a letter at a time, once it is quick */
                struct timespec gap = {0, 20 * 1000 * 1000L};
                ui_key(UI_KEY_DOWN);
                nanosleep(&gap, NULL);
                lv_timer_handler();
            }
            for (int k = 0; (m == 0 || m == 2) && k < 9; k++) {   /* (a turn at a time: quick ones leap) */
                struct timespec gap = {0, 100 * 1000 * 1000L};
                ui_key(UI_KEY_DOWN);
                nanosleep(&gap, NULL);
                lv_timer_handler();
            }
            for (int k = 0; k < 40; k++) {                 /* (its covers come a few a tick) */
                struct timespec ts = {0, (m == 2 && k >= 30 ? 9 : 4) * 1000 * 1000L};
                if (m == 2 && k == 30) ui_key(UI_KEY_DOWN);   /* the slide begins: caught about 90 ms in */
                nanosleep(&ts, NULL);
                lv_tick_inc((uint32_t)(ts.tv_nsec / 1000000));
                lv_timer_handler();
            }
            lv_obj_update_layout(lv_screen_active());
            lv_draw_buf_t *img = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB888);
            char path[1024];
            snprintf(path, sizeof path, "%s/%s-%s.bmp", dir, slug[t], NAME[m]);
            if (img) { write_bmp(path, img); lv_draw_buf_destroy(img); printf("%s\n", path); }
            settings_set_int("display.album_flow", 0);
            ui_show("home");
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
    ui_set_new_install(getenv("SIM_NEW_INSTALL") != NULL);   /* (a fresh install's Home) */
    ui_set_music_missing(getenv("SIM_NO_MUSIC") != NULL);   /* (no Music folder) */
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

/* ---- a station chosen before Wi-Fi has joined (the ui_wifi_wait test) ----
 * Wi-Fi as the UI sees it, faked: off, starting, on with nothing joined, or
 * connected. Nothing here reaches a network: the stations are made up and
 * this build's player has nothing that could open them. */
static struct { int on, state, toggles, wakes; char busy[16]; } g_ww;
static int ww_state(char *busy, size_t cap, ui_wifi_status_t *st)
{
    snprintf(busy, cap, "%s", g_ww.busy);
    if (st) *st = (ui_wifi_status_t){.state = g_ww.state};
    return g_ww.on;
}
static int ww_networks(ui_wifi_net_t *out, int max) { (void)out; (void)max; return 0; }
static void ww_do(int what, const char *ssid, const char *password)
{
    (void)ssid; (void)password;
    if (what == UI_WIFI_TOGGLE) g_ww.toggles++;
}
static void ww_wake(void) { g_ww.wakes++; }
static int ww_ready(char *why, size_t cap)
{
    if (g_ww.state == UI_WIFI_CONNECTED) return 0;
    snprintf(why, cap, "%s", g_ww.on || g_ww.busy[0] ? "Not connected yet: Settings > Wi-Fi" : "Wi-Fi is off: Settings > Wi-Fi");
    return -1;
}
static int ww_stations(radio_station_t *out, int max)
{
    static const radio_station_t st[2] = {{.name = "One", .url = "http://127.0.0.1:9/one"},
                                          {.name = "Two", .url = "http://127.0.0.1:9/two"}};
    int n = max < 2 ? max : 2;
    memcpy(out, st, (size_t)n * sizeof *out);
    return n;
}
static void ww_set(int on, int state, const char *busy)
{
    g_ww.on = on;
    g_ww.state = state;
    snprintf(g_ww.busy, sizeof g_ww.busy, "%s", busy);
    ui_wifi_changed();
}
/* the station on the radio (1, 2), 0 off the radio */
static int ww_on_air(void)
{
    const radio_station_t *s = player_radio_station(g_player);
    return !s ? 0 : !strcmp(s->name, "One") ? 1 : 2;
}
static int g_ww_bad;
#define WW(cond) do { if (!(cond)) { fprintf(stderr, "wifi-wait: line %d: %s\n", __LINE__, #cond); g_ww_bad++; } } while (0)

/* A scratch folder to run in (the library and settings go in build/sim-data
 * under where the simulator is run), removed afterwards. */
static int ww_scratch(char *dir, size_t cap)
{
    const char *t = getenv("TMPDIR");
    snprintf(dir, cap, "%s/fathom-wifi-wait-XXXXXX", t && *t ? t : "/tmp");
    if (!mkdtemp(dir) || chdir(dir) || mkdir("build", 0755)) { perror("wifi-wait: scratch folder"); return -1; }
    return 0;
}
static int ww_rm(const char *path, const struct stat *st, int type, struct FTW *f)
{
    (void)st; (void)type; (void)f;
    return remove(path);
}

/* Radio's list with station `row` (1, 2) selected */
static void ww_radio_list(int row)
{
    ui_show("radio");
    int sel = 0;
    for (int g = 0; g < 8 && ui_list_state(&sel, NULL, NULL) && sel != row; g++) ui_key(sel < row ? UI_KEY_DOWN : UI_KEY_UP);
    advance(300);
}

/* ---- typing with the wheel (the ui_typing test): letters go in at the
 * cursor, <Prev> and <Next> move it, Back deletes the letter before it ---- */
static int g_ty_bad;
#define TY(cond) do { if (!(cond)) { fprintf(stderr, "type-test: line %d: %s\n", __LINE__, #cond); g_ty_bad++; } } while (0)
static int ty_is(const char *text, int cursor)
{
    int c = -1;
    const char *e = ui_test_entry(&c);
    if (strcmp(e, text) || c != cursor) fprintf(stderr, "type-test: \"%s\" at %d (wanted \"%s\" at %d)\n", e, c, text, cursor);
    return !strcmp(e, text) && c == cursor;
}

/* Three short songs for the scratch library (Easy, Echo, Ember), so a search
 * has results. */
static int ty_songs(void)
{
    static const char *const NAME[] = {"Easy", "Echo", "Ember"};
    if (mkdir("ty-music", 0755)) return -1;
    for (int i = 0; i < 3; i++) {
        char path[64];
        snprintf(path, sizeof path, "ty-music/%s.flac", NAME[i]);
        FLAC__StreamEncoder *e = FLAC__stream_encoder_new();
        FLAC__stream_encoder_set_channels(e, 2);
        FLAC__stream_encoder_set_bits_per_sample(e, 16);
        FLAC__stream_encoder_set_sample_rate(e, 44100);
        FLAC__StreamMetadata *vc = FLAC__metadata_object_new(FLAC__METADATA_TYPE_VORBIS_COMMENT);
        const char *kv[][2] = {{"TITLE", NAME[i]}, {"ARTIST", "Some Band"}, {"ALBUM", "An Album"}};
        for (int k = 0; k < 3; k++) {
            FLAC__StreamMetadata_VorbisComment_Entry en;
            FLAC__metadata_object_vorbiscomment_entry_from_name_value_pair(&en, kv[k][0], kv[k][1]);
            FLAC__metadata_object_vorbiscomment_append_comment(vc, en, false);
        }
        FLAC__stream_encoder_set_metadata(e, &vc, 1);
        int ok = FLAC__stream_encoder_init_file(e, path, NULL, NULL) == FLAC__STREAM_ENCODER_INIT_STATUS_OK;
        if (ok) {
            static FLAC__int32 pcm[2 * 441];
            FLAC__stream_encoder_process_interleaved(e, pcm, 441);
            FLAC__stream_encoder_finish(e);
        }
        FLAC__stream_encoder_delete(e);
        FLAC__metadata_object_delete(vc);
        if (!ok) return -1;
    }
    return library_scan(g_lib, "ty-music", NULL, NULL) == 3 ? 0 : -1;
}

/* A name with a letter of two bytes (a playlist called "Caf\xc3\xa9" being
 * renamed): the cursor steps over the whole letter and <Back> deletes the
 * whole of it. (By bytes, <Prev> stopped inside the letter, and a letter
 * typed or deleted there left a name that isn't text.) */
static void ty_utf8(void)
{
    setenv("FATHOM_SIM_SEARCH", "caf\xc3\xa9s", 1);
    ui_show("search");
    advance(400);
    TY(ty_is("caf\xc3\xa9s", 6));
    ui_key(UI_KEY_PREV);
    TY(ty_is("caf\xc3\xa9s", 5));
    ui_key(UI_KEY_PREV);                          /* over the whole letter */
    TY(ty_is("caf\xc3\xa9s", 3));
    ui_key(UI_KEY_NEXT);
    TY(ty_is("caf\xc3\xa9s", 5));
    ui_key(UI_KEY_BACK);                          /* and the whole of it deleted */
    TY(ty_is("cafs", 3));
    ui_key(UI_KEY_NEXT);
    ui_key(UI_KEY_NEXT);                          /* (no further than the end) */
    TY(ty_is("cafs", 4));
    for (int i = 0; i < 4; i++) ui_key(UI_KEY_BACK);
    TY(ty_is("", 0));
    ui_key(UI_KEY_BACK);
    advance(400);
}

/* The highlight is drawn on the selected row, whatever happens to the list
 * under it: Settings > Bluetooth loses its rows while Bluetooth starts again
 * (Audio changed), and has them back. In 0.17.0 the highlight stayed on Audio
 * with the selection on the first row, and <Play> turned Bluetooth off. */
static int g_tb_on = 1, g_tb_toggled, g_tb_range;
static int tb_state(char *busy, size_t cap)
{
    snprintf(busy, cap, "%s", g_tb_on ? "" : "Starting");
    return g_tb_on;
}
static int tb_devices(ui_bt_dev_t *out, int max) { (void)out; (void)max; return 0; }
static void tb_do(int what, const char *addr)
{
    (void)addr;
    if (what == UI_BT_RANGE) g_tb_range++;         /* (it starts again a moment later, as on the H2: the test says when) */
    if (what == UI_BT_TOGGLE) g_tb_toggled++;
}
static void ty_pill(void)
{
    ui_hooks_t hooks = g_hooks;
    hooks.bt_state = tb_state;
    hooks.bt_devices = tb_devices;
    hooks.bt_do = tb_do;
    ui_set_hooks(&hooks);
    ui_show("bluetooth");
    advance(400);
    int sel = -1, n = 0;
    ui_list_state(&sel, &n, NULL);
    TY(sel == 0 && n == 3 && ui_test_pill_row() == 0);
    ui_key(UI_KEY_DOWN);
    ui_key(UI_KEY_DOWN);
    advance(400);
    ui_list_state(&sel, NULL, NULL);
    TY(sel == 2 && ui_test_pill_row() == 2);
    ui_key(UI_KEY_OK);                            /* Audio: Bluetooth starts again, */
    ui_tick_1s();                                 /* a moment later (drawn once more with its rows still there), */
    ui_list_state(&sel, &n, NULL);
    TY(g_tb_range == 1 && n == 3 && sel == 2);
    g_tb_on = 0;                                  /* one row left */
    ui_bt_changed();
    advance(400);
    ui_list_state(&sel, &n, NULL);
    TY(n == 1 && sel == 0 && ui_test_pill_row() == 0);
    g_tb_on = 1;                                  /* back on (nothing says so: the page's next drawing sees it): */
    ui_tick_1s();                                 /* Audio selected again, and shown so */
    advance(400);
    ui_list_state(&sel, &n, NULL);
    TY(n == 3 && sel == 2 && ui_test_pill_row() == 2);
    ui_key(UI_KEY_OK);                            /* <Play> there changes Audio again, not Bluetooth */
    TY(g_tb_range == 2 && !g_tb_toggled);
    g_tb_on = 1;
    ui_bt_changed();
    advance(400);
    ui_set_hooks(&g_hooks);
}

static int type_check(theme_t *themes, const ui_fonts_t *fonts)
{
    lv_tick_set_cb(fake_tick);
    lv_display_t *d = lv_display_create(UI_W, UI_H);
    lv_display_set_buffers(d, g_fb, NULL, sizeof g_fb, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(d, null_flush);
    if (!g_player) { fprintf(stderr, "type-test: no player\n"); return 1; }
    ui_init(lv_screen_active(), themes, g_n_themes, fonts, 1, g_lib, g_player);
    ui_set_hooks(&g_hooks);
    ui_set_theme(0);
    setenv("FATHOM_SIM_SEARCH", "abd", 1);        /* typed already; the wheel on E */
    ui_show("search");
    advance(400);
    int depth = ui_list_state(NULL, NULL, NULL);
    TY(ty_is("abd", 3));
    ui_key(UI_KEY_PREV);                          /* before the d */
    TY(ty_is("abd", 2));
    ui_key(UI_KEY_UP);
    ui_key(UI_KEY_UP);                            /* E, D, C */
    ui_key(UI_KEY_OK);
    TY(ty_is("abcd", 3));                         /* in at the cursor, which moves on */
    advance(600);
    ui_key(UI_KEY_BACK);                          /* the letter before the cursor */
    TY(ty_is("abd", 2));
    ui_key(UI_KEY_PREV);
    ui_key(UI_KEY_PREV);
    ui_key(UI_KEY_PREV);                          /* (no further than the start) */
    TY(ty_is("abd", 0));
    ui_key(UI_KEY_BACK);                          /* nothing before it: nothing goes, and Search stays */
    TY(ty_is("abd", 0) && ui_list_state(NULL, NULL, NULL) == depth);
    ui_key(UI_KEY_OK);
    TY(ty_is("cabd", 1));
    advance(100);
    ui_key(UI_KEY_OK);                            /* twice fast: that letter back out (and Results) */
    TY(ty_is("abd", 0));
    advance(600);
    for (int i = 0; i < 5; i++) ui_key(UI_KEY_NEXT);   /* (no further than the end) */
    TY(ty_is("abd", 3));
    ui_key(UI_KEY_BACK);
    ui_key(UI_KEY_BACK);
    ui_key(UI_KEY_BACK);
    TY(ty_is("", 0) && ui_list_state(NULL, NULL, NULL) == depth);
    ui_key(UI_KEY_BACK);                          /* nothing typed: Back leaves */
    advance(400);
    TY(ui_list_state(NULL, NULL, NULL) < depth);

    /* With results: twice <Play> goes to them and the typing is left (no
     * cursor, the wheel moves through the list, the typing row one of its
     * rows); <Play> on that row types again; <Back> goes up to it, then out. */
    TY(ty_songs() == 0);
    setenv("FATHOM_SIM_SEARCH", "", 1);
    ui_show("search");
    advance(400);
    int sel = -1, n = 0;
    TY(ui_test_typing());
    ui_key(UI_KEY_OK);                            /* the wheel's letter: e */
    TY(ty_is("e", 1));
    ui_list_state(&sel, &n, NULL);
    TY(sel == 0 && n > 2);                        /* (the test library has names with a word in e) */
    ui_key(UI_KEY_DOWN);                          /* typing: the wheel changes the letter, not the row */
    ui_list_state(&sel, NULL, NULL);
    TY(sel == 0 && ui_test_typing());
    advance(600);
    ui_key(UI_KEY_OK);
    advance(100);
    ui_key(UI_KEY_OK);                            /* twice fast: the letter back out, and the results */
    ui_list_state(&sel, NULL, NULL);
    TY(ty_is("e", 1) && sel == 1 && !ui_test_typing());
    advance(600);
    ui_key(UI_KEY_DOWN);
    ui_key(UI_KEY_UP);
    ui_key(UI_KEY_UP);                            /* up onto the typing row: a row */
    ui_list_state(&sel, NULL, NULL);
    TY(sel == 0 && !ui_test_typing());
    ui_key(UI_KEY_UP);                            /* (and no further) */
    ui_key(UI_KEY_DOWN);                          /* down again: the first result, the letters as they were */
    ui_list_state(&sel, NULL, NULL);
    TY(sel == 1 && ty_is("e", 1) && !ui_test_typing());
    ui_key(UI_KEY_UP);
    ui_key(UI_KEY_OK);                            /* <Play> on the typing row: typing again, nothing added */
    ui_list_state(&sel, NULL, NULL);
    TY(sel == 0 && ui_test_typing() && ty_is("e", 1));
    advance(100);
    ui_key(UI_KEY_OK);                            /* (not the second of two: a letter) */
    ui_list_state(&sel, NULL, NULL);
    TY(sel == 0 && ui_test_typing() && strlen(ui_test_entry(NULL)) == 2);
    advance(600);
    ui_key(UI_KEY_BACK);
    TY(ty_is("e", 1));
    ui_key(UI_KEY_OK);
    advance(100);
    ui_key(UI_KEY_OK);                            /* to the results again */
    ui_key(UI_KEY_DOWN);
    ui_list_state(&sel, NULL, NULL);
    TY(sel == 2 && !ui_test_typing());
    advance(600);
    ui_key(UI_KEY_BACK);                          /* <Back> in the results: up to the typing row, a row still */
    ui_list_state(&sel, NULL, NULL);
    TY(sel == 0 && !ui_test_typing() && ty_is("e", 1));
    ui_key(UI_KEY_OK);                            /* <Play> there: the letters */
    TY(ui_test_typing() && ty_is("e", 1));
    advance(600);
    ui_key(UI_KEY_OK);
    advance(100);
    ui_key(UI_KEY_OK);                            /* the results once more */
    advance(600);
    depth = ui_list_state(&sel, NULL, NULL);
    TY(sel == 1);
    ui_key(UI_KEY_BACK);
    TY(ui_list_state(NULL, NULL, NULL) == depth); /* (the first <Back>: still Search) */
    ui_key(UI_KEY_BACK);                          /* the second: out, with a letter typed */
    advance(400);
    TY(ui_list_state(NULL, NULL, NULL) < depth);
    ty_utf8();
    ty_pill();
    if (!g_ty_bad) printf("type-test: all checks passed\n");
    return g_ty_bad != 0;
}

static int wifi_wait_check(theme_t *themes, const ui_fonts_t *fonts)
{
    lv_tick_set_cb(fake_tick);
    lv_display_t *d = lv_display_create(UI_W, UI_H);
    lv_display_set_buffers(d, g_fb, NULL, sizeof g_fb, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(d, null_flush);
    if (!g_player) { fprintf(stderr, "wifi-wait: no player\n"); return 1; }
    ui_init(lv_screen_active(), themes, g_n_themes, fonts, 1, g_lib, g_player);
    ui_hooks_t hooks = g_hooks;
    hooks.wifi_state = ww_state;
    hooks.wifi_networks = ww_networks;
    hooks.wifi_do = ww_do;
    hooks.wifi_wake = ww_wake;
    hooks.radio_ready = ww_ready;
    hooks.radio_stations = ww_stations;
    ui_set_hooks(&hooks);
    ui_set_battery(86, 0);
    ui_set_theme(0);
    int depth, sel = -1;

    /* Wi-Fi starting (as at boot): Play on a station is kept, not thrown away */
    ww_set(0, UI_WIFI_OFF, "Starting");
    ww_radio_list(1);
    depth = ui_list_state(&sel, NULL, NULL);
    WW(depth > 0 && sel == 1);
    ui_key(UI_KEY_OK);
    advance(100);
    WW(ww_on_air() == 0);                         /* not yet */
    WW(ui_list_state(NULL, NULL, NULL) == depth); /* and not asked "Turn on Wi-Fi?" */
    WW(g_ww.wakes == 1 && g_ww.toggles == 0);
    ww_set(1, UI_WIFI_ON, "");                    /* started, nothing joined yet */
    WW(ww_on_air() == 0);
    ww_set(1, UI_WIFI_CONNECTING, "");
    WW(ww_on_air() == 0);
    ww_set(1, UI_WIFI_CONNECTED, "");             /* joined: the station chosen starts */
    advance(100);
    WW(ww_on_air() == 1);

    /* on with nothing joined: the same, for the other station */
    ww_set(1, UI_WIFI_ON, "");
    ww_radio_list(2);
    ui_key(UI_KEY_OK);
    WW(ww_on_air() == 1 && g_ww.wakes == 2);
    ww_set(1, UI_WIFI_CONNECTED, "");
    advance(100);
    WW(ww_on_air() == 2);

    /* moved to another row meanwhile: nothing starts by itself */
    ww_set(1, UI_WIFI_ON, "");
    ww_radio_list(1);
    ui_key(UI_KEY_OK);
    ui_key(UI_KEY_UP);
    ww_set(1, UI_WIFI_CONNECTED, "");
    advance(100);
    WW(ww_on_air() == 2 && g_ww.wakes == 3);

    /* never joined: given up after 45 s, and a connection later starts nothing */
    ww_set(1, UI_WIFI_ON, "");
    ww_radio_list(1);
    ui_key(UI_KEY_OK);
    for (int s = 0; s < 46; s++) { g_fake_ms += 1000; ui_tick_1s(); }
    ww_set(1, UI_WIFI_CONNECTED, "");
    advance(100);
    WW(ww_on_air() == 2 && g_ww.wakes == 4);

    /* Wi-Fi off: asked as before (nothing woken), and Turn on still leads to the station */
    ww_set(0, UI_WIFI_OFF, "");
    ww_radio_list(1);
    depth = ui_list_state(NULL, NULL, NULL);
    ui_key(UI_KEY_OK);
    advance(400);
    WW(ui_list_state(NULL, NULL, NULL) == depth + 1);   /* "Turn on Wi-Fi?" */
    WW(g_ww.wakes == 4 && g_ww.toggles == 0);
    ui_key(UI_KEY_OK);                            /* Turn on */
    advance(400);
    WW(g_ww.toggles == 1 && ww_on_air() == 2);
    ww_set(0, UI_WIFI_OFF, "Starting");
    ww_set(1, UI_WIFI_CONNECTED, "");
    advance(100);
    WW(ww_on_air() == 1);

    /* connected: Play plays at once */
    ww_radio_list(2);
    ui_key(UI_KEY_OK);
    WW(ww_on_air() == 2 && g_ww.wakes == 4);

    player_pause(g_player, 1);
    printf("wifi-wait: %s\n", g_ww_bad ? "FAILED" : "ok");
    return g_ww_bad ? 1 : 0;
}

/* ---- the UI's checks (the ui_checks test): one for each defect found by
 * reading ui.c (the audit of 2026-11-26), on a scratch
 * library of four albums of three songs, half a minute of silence each,
 * through the clock output: nothing ends by itself while a check runs. ---- */
static int g_uc_bad;
#define UC(cond) do { if (!(cond)) { fprintf(stderr, "ui-checks: line %d: %s\n", __LINE__, #cond); g_uc_bad++; } } while (0)

#define UC_LONG "Beta Is The Second Letter Of The Greek Alphabet And Too Long For A Row"
static int uc_song(const char *file, const char *title, const char *artist, const char *album, int no)
{
    char path[96], num[8];
    snprintf(path, sizeof path, "uc-music/%s.flac", file);
    snprintf(num, sizeof num, "%d", no);
    FLAC__StreamEncoder *e = FLAC__stream_encoder_new();
    FLAC__stream_encoder_set_channels(e, 2);
    FLAC__stream_encoder_set_bits_per_sample(e, 16);
    FLAC__stream_encoder_set_sample_rate(e, 44100);
    FLAC__StreamMetadata *vc = FLAC__metadata_object_new(FLAC__METADATA_TYPE_VORBIS_COMMENT);
    const char *kv[][2] = {{"TITLE", title}, {"ARTIST", artist}, {"ALBUM", album}, {"TRACKNUMBER", num}};
    for (int k = 0; k < 4; k++) {
        FLAC__StreamMetadata_VorbisComment_Entry en;
        FLAC__metadata_object_vorbiscomment_entry_from_name_value_pair(&en, kv[k][0], kv[k][1]);
        FLAC__metadata_object_vorbiscomment_append_comment(vc, en, false);
    }
    FLAC__stream_encoder_set_metadata(e, &vc, 1);
    int ok = FLAC__stream_encoder_init_file(e, path, NULL, NULL) == FLAC__STREAM_ENCODER_INIT_STATUS_OK;
    if (ok) {
        static FLAC__int32 pcm[2 * 4410];
        for (int i = 0; i < 300; i++) FLAC__stream_encoder_process_interleaved(e, pcm, 4410);
        FLAC__stream_encoder_finish(e);
    }
    FLAC__stream_encoder_delete(e);
    FLAC__metadata_object_delete(vc);
    return ok ? 0 : -1;
}

static int uc_songs(void)
{
    static const char *const ALBUM[] = {"Alpha", UC_LONG, "Gamma", "Delta"};
    static const char *const ARTIST[] = {"Ann", "Bob And A Band Whose Name Is Much Too Long For One Row Of A List", "Cat", "Dan"};
    static const char *const SONG[] = {"Amber", "Aster", "Azure", "Birch", "Bloom", "Brook",
                                       "Cedar", "Clove", "Coral", "Dune", "Dusk", "Dawn"};
    if (mkdir("uc-music", 0755)) return -1;
    for (int i = 0; i < 12; i++)
        if (uc_song(SONG[i], SONG[i], ARTIST[i / 3], ALBUM[i / 3], i % 3 + 1)) return -1;
    return library_scan(g_lib, "uc-music", NULL, NULL) == 12 ? 0 : -1;
}

/* The open list's row `row` selected, with the wheel */
static void uc_row(int row)
{
    int sel = 0;
    for (int g = 0; g < 40 && ui_list_state(&sel, NULL, NULL) && sel != row; g++) {
        ui_key(sel < row ? UI_KEY_DOWN : UI_KEY_UP);
        advance(100);                             /* (slowly: the wheel turned fast goes by more than a row) */
    }
    advance(300);
}

/* An album without a cover: a plain square of the theme's surface colour in
 * Albums. (It was filled with the low 16 bits of the colour's 24, read as a
 * pixel: a blue square on a dark theme, a salmon one on a light one. The
 * songs here have no covers.) */
static void uc_plain_square(void)
{
    for (int t = 0; t < g_n_themes; t++) {
        ui_set_theme(t);
        ui_show("albums");
        advance(600);
        uint32_t c = g_themes[t].surface, low = theme_rgb565(c) & 0xffff;
        uint16_t right = (uint16_t)(((c >> 19) & 31) << 11 | ((c >> 10) & 63) << 5 | ((c >> 3) & 31));
        lv_obj_update_layout(lv_screen_active());
        lv_draw_buf_t *img = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB565);
        UC(img != NULL);
        if (!img) continue;
        long n_right = 0, n_low = 0;
        for (int y = 0; y < UI_H; y++) {
            const uint16_t *row = (const uint16_t *)(const void *)(img->data + (size_t)y * img->header.stride);
            for (int x = 0; x < UI_W; x++) {
                n_right += row[x] == right;
                n_low += low != right && row[x] == low;
            }
        }
        lv_draw_buf_destroy(img);
        UC(n_right >= LIB_ART_SMALL * LIB_ART_SMALL * 3 / 4);
        UC(n_low == 0);
        if (n_low || n_right < LIB_ART_SMALL * LIB_ART_SMALL * 3 / 4)
            printf("theme %d: %ld pixels of the surface's colour, %ld of its low 16 bits\n", t, n_right, n_low);
    }
    ui_set_theme(0);
    ui_show("home");
    advance(400);
}

/* U1: a media key with the screen off starts no scrolling text (a long name
 * selected scrolled on unseen, 60 times a second, until the screen came on) */
static void uc_asleep_marquee(void)
{
    ui_show("albums");
    uc_row(1);                                    /* the long name */
    advance(1500);
    UC(lv_anim_count_running() > 0);              /* lit: it scrolls after a second */
    ui_set_awake(0);
    advance(100);
    UC(lv_anim_count_running() == 0);
    ui_key(UI_KEY_NEXT);
    advance(1500);
    UC(lv_anim_count_running() == 0);
    ui_set_awake(1);
    advance(1500);
    UC(lv_anim_count_running() > 0);              /* and again once it's lit */
    ui_show("home");
    advance(400);
}

/* The whole library cued, paused on song `start` (by name: Amber, Aster ... Dusk) */
static void uc_cue(int start)
{
    lib_list_t songs;
    if (library_list(g_lib, LIB_SONGS, 0, &songs) <= 0) { UC(!"songs"); return; }
    player_cue(g_player, songs.ids, songs.n, start);
    library_list_free(&songs);
    ui_tick();
}
static int uc_index(void)
{
    player_now_t now;
    player_now(g_player, &now);
    return now.index;
}

/* U2: <Prev> and <Next> on a typing screen move the cursor only from the H2's
 * own keys with the screen lit: with it off, and from the headphones' buttons,
 * they are the song's (both were swallowed by the typing) */
static void uc_typing_media(void)
{
    uc_cue(0);
    setenv("FATHOM_SIM_SEARCH", "abd", 1);
    ui_show("search");
    advance(400);
    ui_key(UI_KEY_PREV);                          /* lit: the cursor, not the song */
    UC(ty_is("abd", 2) && uc_index() == 0);
    ui_set_awake(0);
    ui_key(UI_KEY_NEXT);                          /* off: the song */
    UC(ty_is("abd", 2) && uc_index() == 1);
    ui_set_awake(1);
    ui_media_key(UI_KEY_NEXT);                    /* the headphones': the song */
    UC(ty_is("abd", 2) && uc_index() == 2);
    ui_key(UI_KEY_NEXT);
    UC(ty_is("abd", 3) && uc_index() == 2);
    player_pause(g_player, 1);
    setenv("FATHOM_SIM_SEARCH", "", 1);
    ui_show("home");
    advance(400);
}

/* U4: a list that gets shorter by itself takes its selection with it. The
 * queue's last row when the song ends (the selection stayed past the end:
 * <Play> and <Menu> did nothing); Search under quick settings when a rescan
 * drops a song it had found (its results were only read again with Search on
 * top: the song's row stayed, empty). */
static void uc_shrink(void)
{
    int sel = -1, n = 0, n0 = 0;
    uc_cue(9);                                    /* Dawn: Dune and Dusk queued after it */
    ui_show("nowplaying");
    ui_show("queue");
    advance(400);
    ui_list_state(NULL, &n, NULL);
    UC(n == 2);
    uc_row(1);
    player_next(g_player);                        /* (as when the song ends) */
    ui_tick();
    ui_tick_1s();
    advance(400);
    ui_list_state(&sel, &n, NULL);
    UC(n == 1 && sel == 0 && ui_test_pill_row() == 0);
    player_pause(g_player, 1);

    setenv("FATHOM_SIM_SEARCH", "dusk", 1);
    setenv("FATHOM_SIM_SEARCH_ROW", "1", 1);
    ui_show("search");
    advance(400);
    int depth = ui_list_state(NULL, &n0, NULL);
    UC(n0 >= 2);                                  /* the typing row, and Dusk */
    ui_key(UI_KEY_MENU_LONG);                     /* quick settings over Search */
    advance(400);
    UC(ui_list_state(NULL, NULL, NULL) == depth + 1);
    UC(remove("uc-music/Dusk.flac") == 0);
    library_scan(g_lib, "uc-music", NULL, NULL);
    ui_library_changed();
    ui_key(UI_KEY_MENU);                          /* and away */
    advance(400);
    UC(ui_list_state(&sel, &n, NULL) == depth && n == n0 - 1 && sel < n);
    UC(uc_song("Dusk", "Dusk", "Dan", "Delta", 2) == 0);
    library_scan(g_lib, "uc-music", NULL, NULL);
    ui_library_changed();
    setenv("FATHOM_SIM_SEARCH", "", 1);
    unsetenv("FATHOM_SIM_SEARCH_ROW");
    ui_show("home");
    advance(400);
}

/* A song's id, by its title (-1: none) */
static int64_t uc_id(const char *title)
{
    lib_list_t songs;
    int64_t id = -1;
    if (library_list(g_lib, LIB_SONGS, 0, &songs) <= 0) return -1;
    for (int i = 0; i < songs.n; i++) {
        lib_track_t t;
        if (library_track(g_lib, songs.ids[i], &t) == 0 && !strcmp(t.title, title)) id = songs.ids[i];
    }
    library_list_free(&songs);
    return id;
}
static int uc_queued(int64_t id)
{
    for (int i = 0; i < player_queue_len(g_player); i++)
        if (player_queue_at(g_player, i) == id) return i;
    return -1;
}

/* U5: the queue's menu and Move are for a song, not a row: the song changing
 * meanwhile moves the rows up, and "Remove from queue" removed the next song,
 * the wheel moved another. */
static void uc_queue_menu(void)
{
    int64_t dawn = uc_id("Dawn"), dune = uc_id("Dune");
    int sel = -1;
    uc_cue(6);                                    /* Cedar: Clove, Coral, Dawn, Dune, Dusk after it */
    ui_show("nowplaying");
    ui_show("queue");
    advance(400);
    uc_row(2);
    UC(dawn > 0 && player_queue_at(g_player, 2) == dawn);
    ui_key(UI_KEY_MENU);                          /* Play now, Move, Remove from queue, Clear queue */
    advance(400);
    player_next(g_player);                        /* the song ends: Dawn is the second row now */
    ui_tick();
    uc_row(2);
    ui_key(UI_KEY_OK);                            /* Remove from queue */
    advance(400);
    UC(player_queue_len(g_player) == 3 && uc_queued(dawn) < 0 && uc_queued(dune) == 1);
    player_pause(g_player, 1);

    uc_cue(6);
    ui_show("nowplaying");
    ui_show("queue");
    advance(400);
    uc_row(2);
    ui_key(UI_KEY_MENU);
    advance(400);
    uc_row(1);
    ui_key(UI_KEY_OK);                            /* Move: the wheel moves Dawn */
    advance(400);
    player_next(g_player);
    ui_tick();
    ui_tick_1s();
    advance(400);
    ui_list_state(&sel, NULL, NULL);
    UC(sel == 1 && ui_test_pill_row() == 1);      /* the highlight went up with it */
    ui_key(UI_KEY_UP);
    advance(400);
    ui_list_state(&sel, NULL, NULL);
    UC(uc_queued(dawn) == 0 && uc_queued(dune) == 2 && sel == 0);
    ui_key(UI_KEY_OK);
    player_pause(g_player, 1);
    ui_show("home");
    advance(400);
}

/* <Menu> on what is shown, then <Play> on its action `label`; 0 if it was there */
static int uc_menu(const char *label)
{
    ui_key(UI_KEY_MENU);
    advance(400);
    int row = ui_test_ctx_row(label);
    if (row < 0) return -1;
    uc_row(row);
    ui_key(UI_KEY_OK);
    advance(400);
    return 0;
}

/* U6: what is opened from Now Playing doesn't pile up under it. Show queue
 * and back to Now Playing with a long <Back>, ten times, left a Queue each
 * time: with the eight levels in use <Menu> did nothing, and from Now Playing
 * it showed the list under it and marked that level as opened from there. */
static void uc_piles(void)
{
    int n = 0;
    uc_cue(0);
    ui_show("nowplaying");
    advance(400);
    for (int i = 0; i < 10; i++) {
        UC(uc_menu("Show queue") == 0);
        UC(ui_list_state(NULL, &n, NULL) == 2 && n == 11);       /* Home and the Queue */
        ui_key(UI_KEY_BACK_LONG);
        advance(400);
        UC(ui_list_state(NULL, NULL, NULL) == 0);
    }
    for (int i = 0; i < 4; i++) {                 /* Go to artist: its page, once */
        UC(uc_menu("Go to artist") == 0);
        UC(ui_list_state(NULL, NULL, NULL) == 2);
        ui_key(UI_KEY_BACK_LONG);
        advance(400);
    }
    /* quick settings, Bluetooth from them, and back to Now Playing: the same */
    for (int i = 0; i < 4; i++) {
        ui_key(UI_KEY_MENU_LONG);
        advance(400);
        UC(ui_list_state(NULL, NULL, NULL) == 2);
        ui_key(UI_KEY_BACK_LONG);
        advance(400);
    }

    /* Two "Add to playlist" opened from Now Playing, for two songs: the first
     * one's list of playlists is gone when the second opens (left under it,
     * it added the second's song) */
    UC(uc_menu("Add to playlist") == 0);
    ui_key(UI_KEY_BACK_LONG);
    advance(400);
    player_next(g_player);
    ui_tick();
    UC(uc_menu("Add to playlist") == 0);
    UC(ui_list_state(NULL, NULL, NULL) == 2);
    ui_key(UI_KEY_BACK);                          /* (nothing chosen) */
    advance(400);
    UC(ui_list_state(NULL, NULL, NULL) == 0);
    ui_key(UI_KEY_BACK_LONG);
    advance(400);
    UC(ui_list_state(NULL, NULL, NULL) == 1);     /* Home, not the first one's list */
    player_pause(g_player, 1);

    /* ...and one opened from a list, left under pages opened since, is no
     * longer for its song once another has been opened: <Play> in it adds
     * nothing (it made a playlist of the other's song) */
    ui_show("songs");
    advance(400);
    UC(uc_menu("Add to playlist") == 0);
    ui_key(UI_KEY_BACK_LONG);
    advance(400);
    UC(uc_menu("Go to artist") == 0);
    ui_key(UI_KEY_OK);
    advance(400);
    UC(uc_menu("Add to playlist") == 0);
    UC(ui_list_state(NULL, NULL, NULL) == 6);     /* Home, Songs, the first list, the artist, the album, the second */
    for (int i = 0; i < 3; i++) { ui_key(UI_KEY_BACK); advance(400); }
    UC(ui_list_state(NULL, NULL, NULL) == 3);     /* the first one again */
    ui_key(UI_KEY_OK);                            /* New playlist */
    advance(400);
    UC(ui_list_state(NULL, NULL, NULL) == 2 && library_playlist_count_writable(g_lib) == 0);

    /* The ten levels in use, by going from page to page in the lists (a
     * song's artist, an album of theirs, a song's artist...): <Menu> on Now
     * Playing opens nothing, and stays there; the lists are as they were. */
    ui_show("songs");
    advance(400);
    UC(uc_menu("Go to album") == 0);
    for (int i = 0; i < 3; i++) {
        UC(uc_menu("Go to artist") == 0);
        ui_key(UI_KEY_OK);                        /* their album */
        advance(400);
    }
    UC(uc_menu("Go to artist") == 0);
    UC(ui_list_state(NULL, NULL, NULL) == 10);
    ui_key(UI_KEY_BACK_LONG);
    advance(400);
    ui_key(UI_KEY_MENU);
    advance(400);
    UC(ui_list_state(NULL, NULL, NULL) == 0);     /* still Now Playing */
    ui_key(UI_KEY_BACK_LONG);
    advance(400);
    UC(ui_list_state(NULL, NULL, NULL) == 10);
    ui_key(UI_KEY_BACK);
    advance(400);
    UC(ui_list_state(NULL, NULL, NULL) == 9);     /* <Back> closes a page: not to Now Playing */
    ui_show("home");
    advance(400);
}

/* Wi-Fi for the checks: on (ww_state), with these networks in range */
static ui_wifi_net_t g_uc_net[4];
static int g_uc_nets;
static int uc_networks(ui_wifi_net_t *out, int max)
{
    int n = g_uc_nets < max ? g_uc_nets : max;
    memcpy(out, g_uc_net, (size_t)n * sizeof *out);
    return n;
}
static char g_uc_joined[33], g_uc_pressed[18];
static void uc_wifi_do(int what, const char *ssid, const char *password)
{
    (void)password;
    if (what == UI_WIFI_CONNECT) snprintf(g_uc_joined, sizeof g_uc_joined, "%s", ssid);
}
/* ...and Bluetooth: on, with these devices */
static ui_bt_dev_t g_uc_dev[3];
static int uc_bt_state(char *busy, size_t cap) { snprintf(busy, cap, "%s", ""); return 1; }
static int uc_bt_devices(ui_bt_dev_t *out, int max)
{
    int n = max < 3 ? max : 3;
    memcpy(out, g_uc_dev, (size_t)n * sizeof *out);
    return n;
}
static void uc_bt_do(int what, const char *addr)
{
    if (what == UI_BT_PRESS) snprintf(g_uc_pressed, sizeof g_uc_pressed, "%s", addr);
}
static void uc_wifi_hooks(void)
{
    ui_hooks_t hooks = g_hooks;
    hooks.wifi_state = ww_state;
    hooks.wifi_networks = uc_networks;
    hooks.wifi_do = uc_wifi_do;
    hooks.bt_state = uc_bt_state;
    hooks.bt_devices = uc_bt_devices;
    hooks.bt_do = uc_bt_do;
    ui_set_hooks(&hooks);
    ww_set(1, UI_WIFI_ON, "");
}
/* The open list's row that reads `text` selected and <Play> pressed; 0 if it was there */
static int uc_open(const char *text)
{
    int row = ui_test_row(text);
    if (row < 0) return -1;
    uc_row(row);
    ui_key(UI_KEY_OK);
    advance(400);
    return 0;
}

/* U3: a Wi-Fi password typed from quick settings, opened over Search, leaves
 * what was typed in Search as it was (one text for every typing screen: the
 * password took its place, and the wheel was left on a letter Search hasn't) */
static void uc_typing_kept(void)
{
    g_uc_net[0] = (ui_wifi_net_t){.ssid = "Net", .dbm = -50, .secured = 1};
    g_uc_nets = 1;
    uc_wifi_hooks();
    setenv("FATHOM_SIM_SEARCH", "dusk", 1);
    ui_show("search");
    advance(400);
    int depth = ui_list_state(NULL, NULL, NULL);
    UC(ty_is("dusk", 4));
    ui_key(UI_KEY_MENU_LONG);                     /* quick settings > Wi-Fi > Net: its password */
    advance(400);
    UC(uc_open("Wi-Fi") == 0);
    UC(uc_open("Net") == 0);
    UC(ui_list_state(NULL, NULL, NULL) == depth + 3 && ty_is("", 0));
    for (int i = 0; i < 3; i++) ui_key(UI_KEY_MENU);   /* the wheel to the signs: further than Search's letters go */
    ui_key(UI_KEY_OK);
    advance(600);
    UC(strlen(ui_test_entry(NULL)) == 1);
    ui_key(UI_KEY_BACK);                          /* the sign deleted, */
    for (int i = 0; i < 3; i++) { ui_key(UI_KEY_BACK); advance(400); }   /* and back out to Search */
    UC(ui_list_state(NULL, NULL, NULL) == depth && ty_is("dusk", 4));
    ui_key(UI_KEY_OK);                            /* the wheel where it was, on E */
    UC(ty_is("duske", 5));
    advance(600);
    setenv("FATHOM_SIM_SEARCH", "", 1);
    ui_set_hooks(&g_hooks);
    ui_show("home");
    advance(400);
}

/* U11: in Settings > Wi-Fi and Bluetooth the selection is a network or a
 * device, not a row: a search that puts the strongest first leaves it on the
 * one it was on (<Play> joined the one that had taken its row); and <Play>
 * acts on the list as drawn, not on one the platform has since made. */
static void uc_net_follow(void)
{
    static const ui_wifi_net_t A = {.ssid = "Alpha net", .dbm = -40}, B = {.ssid = "Beta net", .dbm = -60},
                               C = {.ssid = "Gamma net", .dbm = -70}, D = {.ssid = "Delta net", .dbm = -30};
    int sel = -1;
    g_uc_net[0] = A; g_uc_net[1] = B; g_uc_net[2] = C;
    g_uc_nets = 3;
    uc_wifi_hooks();
    ui_show("wifi");
    advance(400);
    UC(ui_test_row("Beta net") == 4);
    uc_row(4);
    g_uc_net[0] = B; g_uc_net[1] = C; g_uc_net[2] = A;      /* a search: another order */
    ui_wifi_changed();
    advance(400);
    ui_list_state(&sel, NULL, NULL);
    UC(sel == 3 && ui_test_pill_row() == 3 && ui_test_row("Beta net") == 3);
    ui_key(UI_KEY_OK);
    UC(!strcmp(g_uc_joined, "Beta net"));
    advance(400);
    g_uc_net[0] = D;                                        /* another in its row, not yet said nor drawn: */
    g_uc_joined[0] = 0;
    ui_key(UI_KEY_OK);                                      /* <Play> is for the row as it shows */
    UC(!strcmp(g_uc_joined, "Beta net"));
    advance(400);
    g_uc_net[0] = A; g_uc_net[1] = B; g_uc_net[2] = C;
    ui_wifi_changed();
    advance(400);
    ui_list_state(&sel, NULL, NULL);
    UC(sel == 3 && ui_test_pill_row() == 3);                /* (Beta gone, then back in another row: the row it was on) */

    static const ui_bt_dev_t X = {.addr = "00:00:00:00:00:01", .name = "Xray"}, Y = {.addr = "00:00:00:00:00:02", .name = "Yoke"},
                             Z = {.addr = "00:00:00:00:00:03", .name = "Zulu"};
    g_uc_dev[0] = X; g_uc_dev[1] = Y; g_uc_dev[2] = Z;
    ui_show("bluetooth");
    advance(400);
    UC(ui_test_row("Yoke") == 4);
    uc_row(4);
    g_uc_dev[0] = Y; g_uc_dev[1] = Z; g_uc_dev[2] = X;
    ui_bt_changed();
    advance(400);
    ui_list_state(&sel, NULL, NULL);
    UC(sel == 3 && ui_test_pill_row() == 3);
    g_uc_dev[0] = Z; g_uc_dev[1] = X; g_uc_dev[2] = X;      /* (not yet said nor drawn) */
    ui_key(UI_KEY_OK);
    UC(!strcmp(g_uc_pressed, Y.addr));
    advance(400);
    ui_set_hooks(&g_hooks);
    ui_show("home");
    advance(400);
}

/* The open menu's action `label`, with the wheel and <Play>, no time passing */
static void uc_act(const char *label)
{
    int row = ui_test_ctx_row(label);
    UC(row >= 0);
    for (int i = 0; i < row; i++) ui_key(UI_KEY_DOWN);
    ui_key(UI_KEY_OK);
}

/* U7: "Delete playlist" asks for a second press, of the same menu: armed on
 * one playlist, a single press within 3 s deleted another */
static void uc_delete_armed(void)
{
    int64_t dawn = uc_id("Dawn");
    int64_t one = library_playlist_create(g_lib, "One"), two = library_playlist_create(g_lib, "Two");
    UC(one > 0 && two > 0 && library_playlist_add(g_lib, one, &dawn, 1) == 0 && library_playlist_add(g_lib, two, &dawn, 1) == 0);
    ui_show("playlists");
    advance(400);
    int r1 = ui_test_row("One"), r2 = ui_test_row("Two");
    UC(r1 >= 0 && r2 >= 0);
    uc_row(r1);
    ui_key(UI_KEY_MENU);
    uc_act("Delete playlist");                    /* "Press again": not pressed again */
    ui_key(UI_KEY_BACK);
    advance(50);
    ui_key(r2 > r1 ? UI_KEY_DOWN : UI_KEY_UP);    /* (they are next to each other) */
    ui_key(UI_KEY_MENU);
    uc_act("Delete playlist");                    /* the other one's: its first press */
    UC(library_playlist_count_writable(g_lib) == 2);
    advance(50);
    ui_key(UI_KEY_OK);                            /* and its second */
    UC(library_playlist_count_writable(g_lib) == 1);
    advance(400);
    library_playlist_delete(g_lib, one);
    library_playlist_delete(g_lib, two);
    ui_library_changed();
    ui_show("home");
    advance(400);
}

/* U8: <Menu> on a setting shows its help each time: after a help had gone by
 * itself, the next <Menu> did nothing (it was taken for the press that puts
 * a help away) */
static void uc_help_again(void)
{
    ui_show("settings");
    advance(400);
    ui_key(UI_KEY_MENU);
    advance(200);
    UC(ui_test_toast() != NULL);
    advance(8000);                                /* gone by itself */
    UC(ui_test_toast() == NULL);
    ui_key(UI_KEY_MENU);
    advance(200);
    UC(ui_test_toast() != NULL);
    ui_key(UI_KEY_MENU);                          /* (and <Menu> on a help still puts it away) */
    advance(200);
    UC(ui_test_toast() == NULL);
    ui_show("home");
    advance(400);
}

/* U9: the Welcome (and What's new) over everything: <Menu> held does nothing
 * (quick settings opened under it, there when it was put away) */
static void uc_welcome_menu(void)
{
    ui_show("home");
    advance(400);
    ui_show_welcome();
    advance(400);
    ui_key(UI_KEY_MENU_LONG);
    advance(400);
    UC(ui_list_state(NULL, NULL, NULL) == 1);
    ui_key(UI_KEY_BACK);                          /* the Welcome away: Home */
    advance(400);
    UC(ui_list_state(NULL, NULL, NULL) == 1);
    ui_key(UI_KEY_MENU_LONG);                     /* (and now it opens) */
    advance(400);
    UC(ui_list_state(NULL, NULL, NULL) == 2);
    ui_show("home");
    advance(400);
}

/* U10: the question asked when a computer is connected goes when the cable
 * comes out, also from under quick settings opened over it (it stayed there,
 * and the next cable put a second one on top) */
static void uc_usb_question(void)
{
    ui_show("home");
    advance(400);
    ui_usb_prompt();
    advance(400);
    ui_key(UI_KEY_MENU_LONG);                     /* quick settings over the question */
    advance(400);
    UC(ui_list_state(NULL, NULL, NULL) == 3);
    ui_usb_prompt();                              /* (said again: still one question) */
    advance(400);
    UC(ui_list_state(NULL, NULL, NULL) == 3);
    ui_usb_prompt_close();                        /* the cable out */
    advance(400);
    UC(ui_list_state(NULL, NULL, NULL) == 2 && ui_test_row("Wi-Fi") < 0 && ui_test_row("Brightness") >= 0);   /* quick settings still */
    ui_key(UI_KEY_BACK);
    advance(400);
    UC(ui_list_state(NULL, NULL, NULL) == 1);     /* and Home under them, no question */
}

/* U12: Now Playing > Menu > Find cover that can't start (no network) leaves
 * Now Playing showing, not the list under it; and with Wi-Fi off, the
 * question it asks goes back to Now Playing */
static void uc_find_cover_np(void)
{
    ui_hooks_t hooks = g_hooks;
    hooks.wifi_state = ww_state;
    hooks.wifi_networks = uc_networks;
    hooks.wifi_do = ww_do;
    hooks.radio_ready = ww_ready;
    ui_set_hooks(&hooks);
    ww_set(1, UI_WIFI_ON, "");                    /* on, nothing joined */
    uc_cue(0);
    ui_show("nowplaying");
    advance(400);
    UC(uc_menu("Find cover") == 0);
    UC(ui_list_state(NULL, NULL, NULL) == 0);     /* said why; still Now Playing */
    ww_set(0, UI_WIFI_OFF, "");
    UC(uc_menu("Find cover") == 0);
    UC(ui_list_state(NULL, NULL, NULL) == 2);     /* "Turn on Wi-Fi?" */
    ui_key(UI_KEY_BACK);
    advance(400);
    UC(ui_list_state(NULL, NULL, NULL) == 0);
    ui_set_hooks(&g_hooks);
    ui_show("home");
    advance(400);
}

/* R5: an update that is on the card already (no room to install it, or an
 * install to finish at a restart): Try again fetches nothing, so it is not
 * answered with "Turn on Wi-Fi?" (with Wi-Fi off the update could only be
 * given up). A failure that does need the network still asks. */
/* A character pack whose pages aren't in memory: the text is laid out at its
 * full width and drawn empty, without waiting for the card, and drawn again
 * by itself when the pack's thread has brought the pages in. A pack of two
 * characters is made here (all ink, 12 pixels), and read as from a slow card. */
static int g_uc_draws;
static void uc_count_draw(lv_event_t *e) { (void)e; g_uc_draws++; }

static long uc_ink(lv_obj_t *obj)
{
    lv_obj_update_layout(obj);
    lv_draw_buf_t *img = lv_snapshot_take(obj, LV_COLOR_FORMAT_ARGB8888);
    if (!img) return -1;
    long n = 0;
    for (uint32_t y = 0; y < img->header.h; y++)
        for (uint32_t x = 0; x < img->header.w; x++)
            n += img->data[(size_t)y * img->header.stride + (size_t)x * 4 + 3] > 128;
    lv_draw_buf_destroy(img);
    return n;
}

/* A pack of two characters, 12 pixels, every pixel of them `ink` (0..15), written as `path` */
enum { UCP_CELLS = 2, UCP_PX = 12 };
static int uc_make_pack(const char *dir, const char *path, int ink)
{
    enum { PG = 4096, CELLS = UCP_CELLS, PX = UCP_PX, ROW = 6, CELL = ROW * PX };
    static unsigned char img[3 * PG];
    static const unsigned CP[CELLS] = {0x3042, 0x6771};
    memset(img, 0, sizeof img);
    memcpy(img, "FATHGLY1", 8);
    img[8] = CELLS;
    img[12] = 1;
    img[17] = PG >> 8;                            /* the index at page 1 */
    img[21] = (3 * PG) >> 8;                      /* the file's length */
    unsigned char *h = img + 24;
    h[0] = PX; h[2] = ROW; h[4] = CELL; h[9] = (2 * PG) >> 8; h[12] = (unsigned char)(signed char)-1; h[13] = 4;
    for (int i = 0; i < CELLS; i++) {
        img[PG + i * 4] = (unsigned char)CP[i];
        img[PG + i * 4 + 1] = (unsigned char)(CP[i] >> 8);
        img[PG + i * 4 + 2] = (unsigned char)i;
    }
    memset(img + 2 * PG, ink * 0x11, CELLS * CELL);
    mkdir(dir, 0755);
    FILE *f = fopen(path, "wb");
    int ok = f && fwrite(img, 1, sizeof img, f) == sizeof img;
    if (f) ok = fclose(f) == 0 && ok;
    return ok;
}

static void uc_pack_arrives(void)
{
    enum { CELLS = UCP_CELLS, PX = UCP_PX };
    int ok = uc_make_pack("uc-packs", "uc-packs/glyphs-uc.bin", 15);
    UC(ok);
    if (!ok) return;

    ui_packs_dir("uc-packs");
    ui_packs_test(1, 150);                        /* nothing in memory; 150 ms a page */
    /* (on a screen of its own: on the player's, anything drawn near the label
     * draws it too, and the count below would be the status bar's) */
    lv_obj_t *was = lv_screen_active(), *scr = lv_obj_create(NULL);
    lv_screen_load(scr);
    lv_obj_t *lab = lv_label_create(scr);
    lv_obj_set_style_text_font(lab, ui_font_pack(PX), 0);
    lv_obj_set_style_text_color(lab, lv_color_white(), 0);
    lv_obj_center(lab);
    lv_obj_add_event_cb(lab, uc_count_draw, LV_EVENT_DRAW_MAIN, NULL);
    unsigned empty0, drawn0, redraws0, empty, drawn, redraws;
    ui_packs_counts(&empty0, &drawn0, &redraws0);
    lv_label_set_text(lab, "\xE3\x81\x82\xE6\x9D\xB1");     /* the two characters */
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    g_uc_draws = 0;
    advance(40);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    ui_packs_counts(&empty, &drawn, &redraws);
    UC(g_uc_draws > 0);                           /* drawn... */
    UC((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000 < 120);   /* ...without the page's 150 ms */
    UC(lv_obj_get_width(lab) == CELLS * PX);      /* its place kept */
    UC(empty - empty0 == CELLS && drawn == drawn0 && redraws == redraws0);            /* both empty, nothing asked yet */
    /* Nothing touches the label from here on. When the pages are there (in
     * real time: 3 s at most) the pack's timer asks once for the screen to be
     * drawn again, and the label is, from its cells. */
    for (int i = 0; i < 150 && redraws == redraws0; i++) {
        usleep(20000);
        g_uc_draws = 0;
        advance(40);
        ui_packs_counts(NULL, NULL, &redraws);
    }
    advance(80);
    ui_packs_counts(&empty, &drawn, &redraws);
    UC(redraws == redraws0 + 1);
    UC(g_uc_draws > 0 && drawn - drawn0 == CELLS && empty - empty0 == CELLS);
    advance(1000);
    ui_packs_counts(&empty, &drawn, &redraws);
    UC(redraws == redraws0 + 1 && empty - empty0 == CELLS);   /* once: the timer has gone */
    UC(uc_ink(lab) == CELLS * PX * PX);           /* (looked at last: taking the picture draws it again) */
    lv_screen_load(was);
    lv_obj_delete(scr);
    ui_packs_test(0, 0);
    ui_packs_dir("build/packs");
    advance(100);
}

/* Settings > Interface > Language packs: <Play> on a pack downloads it,
 * cancels the download, or (twice within 3 s) removes it; a failure is said
 * once, with its reason. The download itself is the simulator's stand-in. */
static void uc_packs_page(void)
{
    int n = 0;
    ui_show("interface");
    advance(300);
    UC(ui_test_row("Language packs") >= 0);
    g_sim_packs[0].state = UI_PACK_ABSENT;
    ui_show("packs");
    advance(300);
    ui_list_state(NULL, &n, NULL);
    UC(n == SIM_PACKS && ui_test_row("Japanese") == 0 && ui_test_row("Korean") == 1);

    g_sim_pack_did = -1;
    ui_key(UI_KEY_OK);                            /* not there: download */
    advance(200);
    UC(g_sim_pack_did == UI_PACK_GET && g_sim_packs[0].state == UI_PACK_GETTING);
    ui_key(UI_KEY_OK);                            /* downloading: cancel */
    advance(200);
    UC(g_sim_pack_did == UI_PACK_CANCEL && g_sim_packs[0].state == UI_PACK_ABSENT);

    g_sim_packs[0].state = UI_PACK_THERE;         /* it has come */
    ui_packs_changed(1);
    advance(200);
    g_sim_pack_did = -1;
    ui_key(UI_KEY_OK);                            /* there: once asks... */
    advance(200);
    UC(g_sim_pack_did == -1 && ui_test_toast() && strstr(ui_test_toast(), "again"));
    advance(3200);                                /* ...and is forgotten after 3 s */
    ui_key(UI_KEY_OK);
    advance(200);
    UC(g_sim_pack_did == -1 && g_sim_packs[0].state == UI_PACK_THERE);
    ui_key(UI_KEY_OK);                            /* twice: removed */
    advance(200);
    UC(g_sim_pack_did == UI_PACK_REMOVE && g_sim_packs[0].state == UI_PACK_ABSENT);
    UC(ui_test_toast() && !strcmp(ui_test_toast(), "Removed"));   /* (not "Press again" left on the screen) */
    advance(3500);

    g_sim_packs[0].state = UI_PACK_FAILED;        /* a download that failed: why, once */
    snprintf(g_sim_packs[0].message, sizeof g_sim_packs[0].message, "Couldn't reach the server");
    ui_packs_changed(0);
    advance(200);
    UC(ui_test_toast() && !strcmp(ui_test_toast(), "Couldn't reach the server"));
    advance(4000);
    UC(ui_test_toast() == NULL);
    ui_packs_changed(0);                          /* (nothing new: not said again) */
    advance(200);
    UC(ui_test_toast() == NULL);
    g_sim_pack_did = -1;
    ui_key(UI_KEY_OK);                            /* failed: <Play> tries again */
    advance(200);
    UC(g_sim_pack_did == UI_PACK_GET);
    g_sim_packs[0].state = UI_PACK_ABSENT;
    g_sim_packs[0].message[0] = 0;

    /* the second pack is its own: <Play> on its row asks for it, not for the first */
    ui_key(UI_KEY_DOWN);
    advance(300);
    g_sim_pack_did = g_sim_pack_at = -1;
    ui_key(UI_KEY_OK);
    advance(200);
    UC(g_sim_pack_did == UI_PACK_GET && g_sim_pack_at == 1);
    UC(g_sim_packs[1].state == UI_PACK_GETTING && g_sim_packs[0].state == UI_PACK_ABSENT);
    /* Remove armed on one pack doesn't remove the other */
    g_sim_packs[0].state = g_sim_packs[1].state = UI_PACK_THERE;
    ui_packs_changed(0);
    advance(200);
    g_sim_pack_did = -1;
    ui_key(UI_KEY_OK);                            /* the second: once */
    advance(200);
    ui_key(UI_KEY_UP);
    advance(300);
    ui_key(UI_KEY_OK);                            /* the first: once (not "twice") */
    advance(200);
    UC(g_sim_pack_did == -1 && g_sim_packs[0].state == UI_PACK_THERE && g_sim_packs[1].state == UI_PACK_THERE);
    advance(3500);
    g_sim_packs[0].state = g_sim_packs[1].state = UI_PACK_ABSENT;
    ui_show("home");
    advance(400);
}

/* Language packs with two packs there that share characters: a last row,
 * "Shared characters", and <Play> on it goes round the packs that have them
 * (not Korean, not one that isn't there). With fewer than two: no such row. */
static void uc_packs_shared(void)
{
    int n = 0;
    sim_packs_all(UI_PACK_ABSENT);
    g_sim_packs[0].state = g_sim_packs[1].state = UI_PACK_THERE;     /* Japanese and Korean: nothing shared */
    ui_show("packs");
    advance(300);
    ui_list_state(NULL, &n, NULL);
    UC(n == SIM_PACKS && ui_test_row("Shared characters") < 0);
    g_sim_packs[2].state = UI_PACK_THERE;                            /* and Simplified Chinese */
    ui_packs_changed(1);
    advance(300);
    ui_list_state(NULL, &n, NULL);
    UC(n == SIM_PACKS + 1 && ui_test_row("Shared characters") == SIM_PACKS);
    uc_row(SIM_PACKS);
    g_sim_pack_did = g_sim_pack_at = -1;
    ui_key(UI_KEY_OK);                            /* from Japanese (the first there) to Simplified */
    advance(300);
    UC(g_sim_pack_did == UI_PACK_FIRST && g_sim_pack_at == 2 && g_sim_packs[2].first);
    ui_key(UI_KEY_OK);                            /* and round to Japanese: Korean and the absent Traditional passed over */
    advance(300);
    UC(g_sim_pack_did == UI_PACK_FIRST && g_sim_pack_at == 0 && g_sim_packs[0].first && !g_sim_packs[2].first);
    g_sim_packs[3].state = UI_PACK_THERE;                            /* Traditional too: three to go round */
    ui_packs_changed(1);
    advance(300);
    ui_key(UI_KEY_OK);
    advance(300);
    ui_key(UI_KEY_OK);
    advance(300);
    UC(g_sim_pack_at == 3 && g_sim_packs[3].first);
    /* the one whose shapes are used is removed: the row follows what is left */
    g_sim_packs[3].state = g_sim_packs[2].state = UI_PACK_ABSENT;
    ui_packs_changed(1);
    advance(300);
    ui_list_state(NULL, &n, NULL);
    UC(n == SIM_PACKS && ui_test_row("Shared characters") < 0);
    sim_packs_all(UI_PACK_ABSENT);
    ui_packs_first("");
    ui_show("home");
    advance(400);
}

/* The library has names a pack would draw, and the pack isn't there: Home has
 * "Get language packs", whatever the language and however many. It opens
 * Language packs, where the first row, "Needed by your library", fetches every
 * pack the library needs, one after another; the packs it needs are listed
 * first; "Hide from Home" puts the offer off. */
static void uc_packs_offer(void)
{
    int n = 0, n0 = 0;
    sim_packs_all(UI_PACK_ABSENT);
    settings_set("packs.ignored", "");
    ui_packs_changed(0);
    ui_show("home");
    advance(300);
    ui_list_state(NULL, &n0, NULL);
    UC(ui_test_row("Get language packs") < 0);    /* nothing needed: Home as ever */
    ui_show("packs");
    advance(300);
    ui_list_state(NULL, &n, NULL);
    UC(n == SIM_PACKS && ui_test_row("Needed by your library") < 0 && ui_test_row("Hide from Home") < 0);

    g_sim_packs[1].songs = 27;                    /* a scan found Korean names: one row on Home... */
    ui_packs_changed(0);
    ui_show("home");
    advance(300);
    ui_list_state(NULL, &n, NULL);
    UC(n == n0 + 1 && ui_test_row("Get language packs") == 0);
    g_sim_packs[0].songs = 342;                   /* ...and Japanese: still the one row, the same words */
    ui_packs_changed(0);
    advance(300);
    ui_list_state(NULL, &n, NULL);
    UC(n == n0 + 1 && ui_test_row("Get language packs") == 0);

    uc_row(0);
    ui_key(UI_KEY_OK);                            /* the page: what is needed, its packs first, the others, Hide */
    advance(400);
    ui_list_state(NULL, &n, NULL);
    UC(n == SIM_PACKS + 2 && ui_test_row("Needed by your library") == 0 && ui_test_row("Japanese") == 1 && ui_test_row("Korean") == 2);
    UC(ui_test_row("Chinese (Simplified)") == 3 && ui_test_row("Hide from Home") == SIM_PACKS + 1);

    g_sim_pack_did = g_sim_pack_at = -1;
    ui_key(UI_KEY_OK);                            /* Needed: the first of them is fetched */
    advance(300);
    UC(g_sim_pack_did == UI_PACK_GET && g_sim_pack_at == 0 && g_sim_packs[0].state == UI_PACK_GETTING);
    ui_key(UI_KEY_OK);                            /* again while it downloads: cancelled, and the rest not fetched */
    advance(300);
    UC(g_sim_pack_did == UI_PACK_CANCEL && g_sim_pack_at == 0 && g_sim_packs[1].state == UI_PACK_ABSENT);
    g_sim_pack_did = g_sim_pack_at = -1;
    ui_packs_changed(0);
    advance(300);
    UC(g_sim_pack_did == -1);                     /* (nothing starts by itself after a cancel) */

    ui_key(UI_KEY_OK);                            /* Needed again: the first... */
    advance(300);
    UC(g_sim_pack_did == UI_PACK_GET && g_sim_pack_at == 0);
    g_sim_packs[0].state = UI_PACK_THERE;         /* ...it has come: the next is fetched by itself */
    g_sim_pack_did = g_sim_pack_at = -1;
    ui_packs_changed(1);
    advance(300);
    UC(g_sim_pack_did == UI_PACK_GET && g_sim_pack_at == 1 && g_sim_packs[1].state == UI_PACK_GETTING);
    ui_list_state(NULL, &n, NULL);
    UC(n == SIM_PACKS + 2 && ui_test_row("Needed by your library") == 0);          /* (Korean still to come) */
    g_sim_packs[1].state = UI_PACK_THERE;         /* the last: nothing more is asked for, the two rows go */
    g_sim_pack_did = -1;
    ui_packs_changed(1);
    advance(300);
    ui_list_state(NULL, &n, NULL);
    UC(g_sim_pack_did == -1 && n == SIM_PACKS && ui_test_row("Needed by your library") < 0 && ui_test_row("Hide from Home") < 0);
    UC(ui_test_row("Japanese") == 0 && ui_test_row("Korean") == 1);
    ui_show("home");
    advance(300);
    ui_list_state(NULL, &n, NULL);
    UC(n == n0 && ui_test_row("Get language packs") < 0);                           /* and Home's row with them */

    /* one that fails stops it: the next isn't fetched, and the reason is said */
    g_sim_packs[0].state = g_sim_packs[1].state = UI_PACK_ABSENT;
    ui_packs_changed(1);
    ui_show("packs");
    advance(300);
    ui_key(UI_KEY_OK);
    advance(300);
    g_sim_packs[0].state = UI_PACK_FAILED;
    snprintf(g_sim_packs[0].message, sizeof g_sim_packs[0].message, "Couldn't reach the server");
    g_sim_pack_did = g_sim_pack_at = -1;
    ui_packs_changed(0);
    advance(300);
    UC(g_sim_pack_did == -1 && g_sim_packs[1].state == UI_PACK_ABSENT);
    UC(ui_test_toast() && !strcmp(ui_test_toast(), "Couldn't reach the server"));
    advance(4000);
    ui_key(UI_KEY_OK);                            /* Needed once more: the one that failed is tried again */
    advance(300);
    UC(g_sim_pack_did == UI_PACK_GET && g_sim_pack_at == 0);
    ui_key(UI_KEY_OK);                            /* (cancelled: the stand-in leaves it absent) */
    advance(300);

    /* a row that comes or goes above the selection doesn't move it to another pack: Korean removed
     * (the row for what is needed comes back, first), the highlight is still on Korean. Seen on the H2. */
    {
        int sel = -1;
        g_sim_packs[0].state = g_sim_packs[1].state = UI_PACK_THERE;
        ui_packs_changed(1);
        advance(300);
        uc_row(1);                                /* Korean, with nothing needed: the second row */
        UC(ui_test_row("Korean") == 1);
        g_sim_packs[1].state = UI_PACK_ABSENT;    /* removed */
        ui_packs_changed(1);
        advance(300);
        ui_list_state(&sel, NULL, NULL);
        UC(ui_test_row("Needed by your library") == 0 && ui_test_row("Korean") == 2 && sel == 2);
        g_sim_packs[1].state = UI_PACK_THERE;     /* and back: the row above goes, the highlight stays with it */
        ui_packs_changed(1);
        advance(300);
        ui_list_state(&sel, NULL, NULL);
        UC(ui_test_row("Korean") == 1 && sel == 1);
        g_sim_packs[0].state = g_sim_packs[1].state = UI_PACK_ABSENT;
        ui_packs_changed(1);
        advance(300);
        uc_row(0);
    }

    /* only Korean needed: its pack comes before Japanese's */
    g_sim_packs[0].songs = 0;
    ui_packs_changed(0);
    advance(300);
    UC(ui_test_row("Needed by your library") == 0 && ui_test_row("Korean") == 1 && ui_test_row("Japanese") == 2);
    g_sim_packs[0].songs = 342;
    ui_packs_changed(0);
    advance(300);

    /* Hide from Home: these packs aren't offered again; the page keeps what is needed */
    uc_row(SIM_PACKS + 1);
    ui_key(UI_KEY_OK);
    advance(400);
    UC(!strcmp(settings_get("packs.ignored", ""), "ja,ko"));
    UC(ui_test_toast() && strstr(ui_test_toast(), "Language packs"));
    ui_list_state(NULL, &n, NULL);
    UC(n == SIM_PACKS + 1 && ui_test_row("Hide from Home") < 0 && ui_test_row("Needed by your library") == 0);
    advance(3500);
    ui_show("home");
    advance(300);
    ui_list_state(NULL, &n, NULL);
    UC(n == n0 && ui_test_row("Get language packs") < 0);

    g_sim_packs[2].songs = 12;                    /* later the library gets Chinese names: offered, the others still put off */
    ui_packs_changed(0);
    advance(300);
    ui_list_state(NULL, &n, NULL);
    UC(n == n0 + 1 && ui_test_row("Get language packs") == 0);
    g_sim_packs[2].state = UI_PACK_THERE;         /* fetched: nothing left to offer */
    ui_packs_changed(1);
    advance(300);
    ui_list_state(NULL, &n, NULL);
    UC(n == n0);

    sim_packs_all(UI_PACK_ABSENT);
    settings_set("packs.ignored", "");
    ui_packs_changed(0);
    ui_show("home");
    advance(400);
}

/* Two packs with the same character: the one put first draws it; with none
 * put first, the first by name does. (aa: every pixel full; bb: every pixel
 * faint, under what uc_ink counts.) */
static void uc_packs_order(void)
{
    int ok = uc_make_pack("uc-order", "uc-order/glyphs-aa.bin", 15) && uc_make_pack("uc-order", "uc-order/glyphs-bb.bin", 1);
    UC(ok);
    if (!ok) return;
    const long full = UCP_CELLS * UCP_PX * UCP_PX;
    ui_packs_first("");
    ui_packs_dir("uc-order");
    lv_obj_t *was = lv_screen_active(), *scr = lv_obj_create(NULL);
    lv_screen_load(scr);
    lv_obj_t *lab = lv_label_create(scr);
    lv_obj_set_style_text_font(lab, ui_font_pack(UCP_PX), 0);
    lv_obj_set_style_text_color(lab, lv_color_white(), 0);
    lv_obj_center(lab);
    lv_label_set_text(lab, "\xE3\x81\x82\xE6\x9D\xB1");
    for (int i = 0; i < 50 && uc_ink(lab) != full; i++) { usleep(20000); advance(40); }   /* (its pages may be fetched) */
    UC(uc_ink(lab) == full);                      /* by name: aa */
    ui_packs_first("bb");
    for (int i = 0; i < 50 && uc_ink(lab) != 0; i++) { usleep(20000); advance(40); }
    UC(uc_ink(lab) == 0);                         /* bb put first */
    ui_packs_first("no-such-pack");
    for (int i = 0; i < 50 && uc_ink(lab) != full; i++) { usleep(20000); advance(40); }
    UC(uc_ink(lab) == full);                      /* one that isn't there: by name again */
    ui_packs_first("bb");
    ui_packs_first("");
    for (int i = 0; i < 50 && uc_ink(lab) != full; i++) { usleep(20000); advance(40); }
    UC(uc_ink(lab) == full);                      /* and none */
    lv_screen_load(was);
    lv_obj_delete(scr);
    ui_packs_dir("build/packs");
    advance(100);
}

/* A name with Japanese and no pack for it: the boxes, and once for a card
 * where the packs are */
static void uc_packs_told(void)
{
    const ui_fonts_t *ff = g_n_themes > 0 ? ui_theme_fonts(&g_themes[0]) : NULL;
    UC(ff != NULL);
    if (!ff) return;
    ui_packs_dir("uc-no-packs");                  /* (no such folder) */
    settings_set_int("packs.told", 0);
    g_sim_packs[0].state = UI_PACK_ABSENT;
    ui_packs_wanted(1);
    advance(4000);
    ui_tick_1s();
    advance(100);
    UC(ui_test_toast() == NULL);                  /* nothing Japanese seen: nothing said */
    lv_obj_t *lab = lv_label_create(lv_screen_active());
    lv_obj_set_style_text_font(lab, ff->body, 0);
    lv_label_set_text(lab, "\xE3\x81\x82\xE6\x9D\xB1");
    advance(100);
    ui_tick_1s();
    advance(100);
    UC(ui_test_toast() && strstr(ui_test_toast(), "Language packs"));
    UC(settings_get_int("packs.told", 0) == 1);
    advance(7000);
    UC(ui_test_toast() == NULL);
    lv_label_set_text(lab, "\xE3\x81\x84");      /* another, later: not said again */
    advance(100);
    ui_tick_1s();
    advance(100);
    UC(ui_test_toast() == NULL);
    /* Before the library's names have been counted (a first start after the
     * update): the toast waits, for Home may be about to offer; counted, and
     * nothing offered, it is said. Seen on the H2: both at that first start. */
    settings_set_int("packs.told", 0);
    ui_packs_counted(0);
    lv_label_set_text(lab, "\xE3\x81\x88");
    advance(100);
    ui_tick_1s();
    advance(100);
    UC(ui_test_toast() == NULL && settings_get_int("packs.told", 0) == 0);     /* not yet */
    ui_packs_counted(1);                          /* counted: no names of the library's need a pack (songs 0) */
    ui_tick_1s();
    advance(100);
    UC(ui_test_toast() && strstr(ui_test_toast(), "Language packs") && settings_get_int("packs.told", 0) == 1);
    advance(7000);
    /* ...and counted with names that do: Home offers, the toast keeps quiet */
    settings_set_int("packs.told", 0);
    ui_packs_counted(0);
    lv_label_set_text(lab, "\xE3\x81\x8A");
    advance(100);
    ui_tick_1s();
    advance(100);
    g_sim_packs[0].songs = 342;
    ui_packs_counted(1);
    ui_packs_changed(0);
    ui_tick_1s();
    advance(100);
    UC(ui_test_toast() == NULL && settings_get_int("packs.told", 0) == 0);
    g_sim_packs[0].songs = 0;
    ui_packs_changed(0);
    /* a start where nothing ever counts: the toast is free again after a minute */
    ui_packs_counted(0);
    lv_label_set_text(lab, "\xE3\x81\x8B");
    advance(100);
    ui_tick_1s();
    advance(100);
    UC(ui_test_toast() == NULL);
    advance(61000);
    ui_tick_1s();
    advance(100);
    UC(ui_test_toast() && strstr(ui_test_toast(), "Language packs"));
    advance(7000);
    ui_packs_counted(1);

    /* A pack has just come. The last picture before it was read had boxes in
     * it: that isn't a name still in want of a pack, and nothing is said. (On
     * the H2 the toast came right after everything needed had been fetched.) */
    settings_set_int("packs.told", 0);
    ui_packs_dir("uc-no-packs");
    lv_label_set_text(lab, "\xE3\x81\x82");      /* drawn as a box: no pack */
    advance(100);
    ui_packs_dir("uc-packs");                     /* its pack arrives: the folder named again, the screen told */
    g_sim_packs[0].state = UI_PACK_THERE;
    ui_packs_changed(1);
    advance(200);
    ui_tick_1s();
    advance(100);
    UC(ui_test_toast() == NULL && settings_get_int("packs.told", 0) == 0);
    g_sim_packs[0].state = UI_PACK_ABSENT;
    ui_packs_dir("uc-no-packs");
    ui_packs_changed(1);
    advance(200);
    ui_packs_wanted(1);

    settings_set_int("packs.told", 0);            /* Korean is told the same way (a card not told yet) */
    lv_label_set_text(lab, "\xED\x95\x9C");      /* a Hangul syllable */
    advance(100);
    ui_tick_1s();
    advance(100);
    UC(ui_test_toast() && strstr(ui_test_toast(), "Language packs") && settings_get_int("packs.told", 0) == 1);
    advance(7000);
    settings_set_int("packs.told", 0);            /* with every pack there: nothing to offer */
    sim_packs_all(UI_PACK_THERE);
    lv_label_set_text(lab, "\xE3\x81\x86");
    advance(100);
    ui_tick_1s();
    advance(100);
    UC(ui_test_toast() == NULL && settings_get_int("packs.told", 0) == 0);
    sim_packs_all(UI_PACK_ABSENT);
    lv_obj_delete(lab);
    ui_packs_dir("build/packs");
    advance(100);
}

static void uc_update_no_net(void)
{
    ui_hooks_t hooks = g_hooks;
    hooks.wifi_state = ww_state;
    hooks.wifi_networks = uc_networks;
    hooks.wifi_do = ww_do;
    hooks.radio_ready = ww_ready;
    ui_set_hooks(&hooks);
    ww_set(0, UI_WIFI_OFF, "");
    ui_update_t was = g_sim_upd, failed = {.state = UI_UPD_FAILED, .message = "Not enough space on the card", .need_mb = 63, .no_net = 1};
    snprintf(failed.current, sizeof failed.current, "%s", was.current);
    snprintf(failed.latest, sizeof failed.latest, "%s", was.latest);
    for (int kept = 1; kept >= 0; kept--) {
        failed.no_net = kept;
        g_sim_upd = failed;
        ui_show("update");
        advance(400);
        int before = ui_list_state(NULL, NULL, NULL);
        ui_key(UI_KEY_DOWN);                      /* the second row: Try again */
        advance(200);
        ui_key(UI_KEY_OK);
        advance(400);
        if (kept) UC(g_sim_upd.state == UI_UPD_AVAILABLE && ui_list_state(NULL, NULL, NULL) == before);
        else UC(g_sim_upd.state == UI_UPD_FAILED && ui_list_state(NULL, NULL, NULL) != before);   /* asked for Wi-Fi */
        ui_show("home");
        advance(400);
    }
    g_sim_upd = was;
    ui_set_hooks(&g_hooks);
    ui_show("home");
    advance(400);
}

/* U13: Playlists with nothing in it (no music yet): <Play> opens nothing (it
 * opened "Never played": "no row" and that playlist were the same number).
 * Before the scratch library is made. */
static void uc_empty_playlists(void)
{
    int n = -1;
    ui_show("playlists");
    advance(400);
    UC(ui_list_state(NULL, &n, NULL) == 2 && n == 0);
    ui_key(UI_KEY_OK);
    advance(400);
    UC(ui_list_state(NULL, NULL, NULL) == 2);
    ui_show("home");
    advance(400);
}

/* The scratch library's album `i` (by name: Alpha, Beta..., Delta, Gamma) */
static int64_t uc_album(int i)
{
    lib_list_t al;
    int64_t id = -1;
    if (library_list(g_lib, LIB_ALBUMS, 0, &al) > i) id = al.ids[i];
    library_list_free(&al);
    return id;
}

/* R2: the cover flow shows a cover made while it is open (a scan's cover
 * pass in the background): the album stayed "has none" there until the flow
 * was left and opened again */
static void uc_flow_new_cover(void)
{
    static uint16_t large[LIB_ART_LARGE * LIB_ART_LARGE], small[LIB_ART_SMALL * LIB_ART_SMALL];
    int64_t beta = uc_album(1);
    settings_set_int("display.album_flow", 1);
    ui_show("albums");                            /* (its second album in the middle) */
    advance(400);
    UC(beta > 0 && ui_test_flow_cover(beta) == 0);
    for (size_t i = 0; i < sizeof large / sizeof large[0]; i++) large[i] = 0xF800;
    for (size_t i = 0; i < sizeof small / sizeof small[0]; i++) small[i] = 0xF800;
    UC(library_store_found_cover(g_lib, beta, large, small) == 0);
    ui_covers_changed();
    advance(400);
    UC(ui_test_flow_cover(beta) == 1);
    settings_set_int("display.album_flow", 0);
    ui_show("home");
    advance(400);
}

/* R4: a row under the cover flow doesn't scroll its text there, unseen (the
 * artist's long name, selected in the list the flow was opened from, started
 * scrolling after a second and kept the screen being drawn) */
static void uc_flow_hidden_row(void)
{
    settings_set_int("display.album_flow", 2);    /* an artist's albums as a cover flow */
    ui_show("albumartists");
    uc_row(1);                                    /* the long name */
    advance(1500);
    UC(ui_test_row_scrolls());                    /* in the list: it scrolls */
    ui_key(UI_KEY_OK);
    advance(400);
    UC(ui_test_flow_cover(uc_album(1)) >= 0);     /* the flow, with their album */
    advance(1500);
    UC(!ui_test_row_scrolls());
    ui_key(UI_KEY_BACK);                          /* the list again: it does */
    advance(1900);
    UC(ui_test_row_scrolls());
    settings_set_int("display.album_flow", 0);
    ui_show("home");
    advance(400);
}

/* The station directory for the checks: three made-up stations, whatever is
 * asked (nothing here reaches a network, and this build's player has nothing
 * that could open them) */
static long uc_dir_fetch(const char *url, char *buf, size_t cap, const volatile int *abort)
{
    (void)abort;
    if (!strstr(url, "/stations/")) return strstr(url, "/json/url/") ? 0 : -1;
    return snprintf(buf, cap, "[{\"name\":\"Radio A\",\"url\":\"http://127.0.0.1:9/a\",\"codec\":\"MP3\"},"
                              "{\"name\":\"Radio B\",\"url\":\"http://127.0.0.1:9/b\",\"codec\":\"MP3\"},"
                              "{\"name\":\"Radio C\",\"url\":\"http://127.0.0.1:9/c\",\"codec\":\"MP3\"}]");
}
static int uc_stations(void)
{
    player_now_t now;
    player_now(g_player, &now);
    return now.radio ? now.count : 0;
}
/* the directory's answer, once it is in (the open list has `rows` rows) */
static void uc_dir_wait(int rows)
{
    int n = 0;
    for (int i = 0; i < 300 && (ui_list_state(NULL, &n, NULL), n) != rows; i++) {
        ui_tick();
        usleep(10 * 1000);
    }
    advance(400);
}

/* R6: a station playing by itself (the list it was started from has been
 * left) is given a list again by <Play> on it in a new list: Next and
 * Previous go through that one (it was shown in Now Playing, and stayed alone) */
static void uc_radio_relist(void)
{
    rdir_set_fetch(uc_dir_fetch);
    ui_show("rdirgenres");                        /* Radio > Find stations > Genres, on Jazz */
    advance(400);
    ui_key(UI_KEY_OK);
    uc_dir_wait(4);                               /* Random station, and the three */
    UC(uc_open("Radio B") == 0);
    UC(uc_stations() == 3 && ui_list_state(NULL, NULL, NULL) == 0);
    ui_key(UI_KEY_BACK_LONG);                     /* its list, */
    advance(400);
    ui_key(UI_KEY_BACK);                          /* left: it plays on alone */
    advance(400);
    UC(uc_stations() == 1);
    ui_key(UI_KEY_OK);                            /* the genre opened again: a new list */
    uc_dir_wait(4);
    UC(uc_open("Radio B") == 0);
    UC(ui_list_state(NULL, NULL, NULL) == 0);     /* Now Playing, the station still on */
    UC(uc_stations() == 3);
    ui_key(UI_KEY_NEXT);
    ui_tick();
    {
        const radio_station_t *st = player_radio_station(g_player);
        UC(st && !strcmp(st->name, "Radio C"));
    }
    uc_cue(0);                                    /* (off the radio) */
    UC(uc_stations() == 0);
    ui_show("home");
    advance(400);
}

/* R3 (its second half): the cover flow's pictures (645 KB) are given back
 * while Now Playing is over it, and are there again, on the same album, when
 * the flow shows again */
static void uc_flow_under_np(void)
{
    int sel = -1;
    int64_t beta = uc_album(1);
    settings_set_int("display.album_flow", 1);
    ui_show("albums");
    advance(400);
    UC(ui_test_flow_cover(beta) == 1);            /* (the cover it was given above) */
    ui_key(UI_KEY_BACK_LONG);                     /* Now Playing over it */
    advance(600);
    UC(ui_list_state(NULL, NULL, NULL) == 0 && ui_test_flow_cover(beta) == -1);
    ui_tick_1s();
    UC(ui_test_flow_cover(beta) == -1);
    ui_key(UI_KEY_BACK_LONG);                     /* and the flow again */
    advance(600);
    ui_list_state(&sel, NULL, NULL);
    UC(sel == 1 && ui_test_flow_cover(beta) == 1 && ui_test_flow_cover(uc_album(0)) == 0 && ui_test_flow_cover(uc_album(2)) == 0);
    ui_key(UI_KEY_DOWN);                          /* it turns as before */
    advance(600);
    ui_list_state(&sel, NULL, NULL);
    UC(sel == 2 && ui_test_flow_cover(uc_album(3)) == 0);
    settings_set_int("display.album_flow", 0);
    ui_show("home");
    advance(400);
    UC(ui_test_flow_cover(beta) == -1);
}

/* U14 (the first of four small ones): Rename playlist on a name longer than
 * the typing screen holds shows whole letters (the last was cut in two), and
 * Done with nothing changed leaves the name as it is (it saved the cut one) */
static void uc_rename_long(void)
{
    char name[128] = "", now[128] = "";
    for (int i = 0; i < 40; i++) strcat(name, "\xc3\xa9");      /* forty letters of two bytes */
    int64_t pl = library_playlist_create(g_lib, name), dawn = uc_id("Dawn");
    UC(pl > 0 && library_playlist_add(g_lib, pl, &dawn, 1) == 0);
    ui_show("playlists");
    advance(400);
    int row = ui_test_row(name);
    UC(row >= 0);
    uc_row(row);
    UC(uc_menu("Rename playlist") == 0);
    UC(strlen(ui_test_entry(NULL)) == 62);        /* 31 whole letters */
    library_playlist_delete(g_lib, pl);
    memset(name, 'n', 70);                        /* and one of seventy plain letters */
    name[70] = 0;
    pl = library_playlist_create(g_lib, name);
    UC(pl > 0 && library_playlist_add(g_lib, pl, &dawn, 1) == 0);
    ui_show("playlists");
    advance(400);
    row = ui_test_row(name);
    UC(row >= 0);
    uc_row(row);
    UC(uc_menu("Rename playlist") == 0);
    UC(strlen(ui_test_entry(NULL)) == 63);
    ui_key(UI_KEY_UP);                            /* the wheel on Done */
    ui_key(UI_KEY_OK);
    advance(400);
    library_playlist_name(g_lib, pl, now, sizeof now);
    UC(!strcmp(now, name) && ui_list_state(NULL, NULL, NULL) == 2);
    library_playlist_delete(g_lib, pl);
    ui_library_changed();
    ui_show("home");
    advance(400);
}

/* U14 (the second): the marker on Now Playing's bar, then <Play>: the song
 * goes to the marker, however long since the screen was last drawn (the jump
 * was worked out from where the song was then: up to a second further on) */
static void uc_scrub(void)
{
    player_now_t now;
    uc_cue(0);
    player_pause(g_player, 0);                    /* playing, in real time */
    ui_show("nowplaying");
    advance(400);
    usleep(300 * 1000);
    ui_tick();
    ui_key(UI_KEY_DOWN);                          /* the marker: 5 s on from 0:00 */
    usleep(800 * 1000);                           /* (nothing drawn meanwhile) */
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    ui_key(UI_KEY_OK);
    usleep(150 * 1000);
    player_now(g_player, &now);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    long since = (long)(t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
    long off = (long)now.pos_ms - (5000 + since);         /* (0.8 s late, as it was) */
    if (getenv("UC_DEBUG")) fprintf(stderr, "ui-checks: scrubbed to %u ms, %ld ms after: %ld ms from the marker\n", now.pos_ms, since, off);
    UC(off > -400 && off < 400);
    player_pause(g_player, 1);
    ui_show("home");
    advance(400);
}

/* U14 (the third): Volume levelling with a value in settings.ini that isn't
 * one of its three (edited by hand: -5) shows Off, and <Play> goes on to
 * Track (it went to -1, and the player was given that) */
static void uc_levelling(void)
{
    settings_set_int("sound.levelling", -5);
    ui_show("sound");
    advance(400);
    UC(uc_open("Volume levelling") == 0);
    UC(settings_get_int("sound.levelling", 0) == LEVEL_TRACK && player_levelling(g_player) == LEVEL_TRACK);
    settings_set_int("sound.levelling", LEVEL_OFF);
    player_set_levelling(g_player, LEVEL_OFF);
    ui_show("home");
    advance(400);
}

/* U14 (the fourth): a list of the station directory that says "Waiting for
 * Wi-Fi" is asked for once Wi-Fi has joined, also when Now Playing (or Wi-Fi's
 * own page) is over it by then (it went on saying so until <Play>) */
static void uc_radio_waiting(void)
{
    int n = -1;
    ui_hooks_t hooks = g_hooks;
    hooks.wifi_state = ww_state;
    hooks.wifi_networks = uc_networks;
    hooks.wifi_do = ww_do;
    hooks.wifi_wake = ww_wake;
    hooks.radio_ready = ww_ready;
    ui_set_hooks(&hooks);
    rdir_set_fetch(uc_dir_fetch);
    ww_set(1, UI_WIFI_ON, "");                    /* on, nothing joined yet */
    ui_show("rdirgenres");
    advance(400);
    ui_key(UI_KEY_OK);                            /* Jazz: "Waiting for Wi-Fi" */
    advance(400);
    int depth = ui_list_state(NULL, &n, NULL);
    UC(n == 0);
    ui_key(UI_KEY_BACK_LONG);                     /* Now Playing meanwhile */
    advance(400);
    ww_set(1, UI_WIFI_CONNECTED, "");             /* joined */
    uc_dir_wait(4);
    ui_key(UI_KEY_BACK_LONG);
    advance(400);
    UC(ui_list_state(NULL, &n, NULL) == depth && n == 4);
    ui_set_hooks(&g_hooks);
    ui_show("home");
    advance(400);
}

/* "The library's names aren't counted yet", said before ui_init as the device
 * says it, still holds after: the toast for boxes waits. (Kept where ui_init
 * clears, it was lost: on the H2 the toast came at a first start all the same.) */
static void uc_packs_before_init(const ui_fonts_t *ff)
{
    UC(ff != NULL);
    if (!ff) return;
    ui_packs_dir("uc-no-packs");
    settings_set_int("packs.told", 0);
    sim_packs_all(UI_PACK_ABSENT);
    lv_obj_t *lab = lv_label_create(lv_screen_active());
    lv_obj_set_style_text_font(lab, ff->body, 0);
    lv_label_set_text(lab, "\xE3\x81\x82\xE6\x9D\xB1");     /* boxes: no pack */
    advance(200);
    ui_tick_1s();
    advance(100);
    UC(ui_test_toast() == NULL && settings_get_int("packs.told", 0) == 0);
    ui_packs_counted(1);
    ui_tick_1s();
    advance(100);
    UC(ui_test_toast() && strstr(ui_test_toast(), "Language packs"));
    advance(7000);
    lv_obj_delete(lab);
    settings_set_int("packs.told", 0);
    ui_packs_wanted(1);
    ui_packs_dir("build/packs");
    advance(100);
}

static int ui_check(theme_t *themes, const ui_fonts_t *fonts)
{
    lv_tick_set_cb(fake_tick);
    lv_display_t *d = lv_display_create(UI_W, UI_H);
    lv_display_set_buffers(d, g_fb, NULL, sizeof g_fb, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(d, null_flush);
    if (!g_player) { fprintf(stderr, "ui-checks: no player\n"); return 1; }
    ui_packs_counted(0);                          /* (as the device: said while starting, before ui_init: uc_packs_before_init) */
    ui_init(lv_screen_active(), themes, g_n_themes, fonts, 1, g_lib, g_player);
    ui_set_hooks(&g_hooks);
    ui_set_battery(86, 0);
    ui_set_theme(0);
    library_set_found_covers("uc-covers");        /* (in the scratch folder, as everything here) */
    uc_packs_before_init(fonts);
    uc_empty_playlists();
    if (uc_songs()) { fprintf(stderr, "ui-checks: no scratch library\n"); return 1; }
    ui_library_changed();
    uc_asleep_marquee();
    uc_typing_media();
    uc_shrink();
    uc_queue_menu();
    uc_piles();
    uc_typing_kept();
    uc_net_follow();
    uc_delete_armed();
    uc_help_again();
    uc_welcome_menu();
    uc_usb_question();
    uc_find_cover_np();
    uc_flow_new_cover();
    uc_flow_hidden_row();
    uc_radio_relist();
    uc_flow_under_np();
    uc_rename_long();
    uc_scrub();
    uc_levelling();
    uc_radio_waiting();
    uc_update_no_net();
    uc_pack_arrives();
    uc_packs_page();
    uc_packs_told();
    uc_packs_shared();
    uc_packs_order();
    uc_packs_offer();
    uc_plain_square();
    player_pause(g_player, 1);
    printf("ui-checks: %s\n", g_uc_bad ? "FAILED" : "ok");
    return g_uc_bad ? 1 : 0;
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
    ui_set_new_install(getenv("SIM_NEW_INSTALL") != NULL);   /* (a fresh install's Home) */
    ui_set_music_missing(getenv("SIM_NO_MUSIC") != NULL);   /* (no Music folder) */
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

static unsigned long bench_case(const char *name, int seconds, int max_wait)
{
    unsigned long wakeups = 0;
    uint32_t w0 = clock_ms32();                  /* the change of screen first, not counted */
    while (clock_ms32() - w0 < 1500) { uint32_t w = lv_timer_handler(); usleep((w > 50 ? 50 : w) * 1000); }
    g_flushes = g_flush_px = g_still_px = 0;
    double c0 = cpu_s();
    uint32_t t0 = clock_ms32();
    while (clock_ms32() - t0 < (uint32_t)seconds * 1000) {
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
    uint32_t t0 = clock_ms32(), next = t0;
    while (clock_ms32() - t0 < (uint32_t)seconds * 1000) {
        uint32_t now = clock_ms32();
        if ((int32_t)(now - next) >= 0) {          /* down for 2 s, up for 2 s */
            ui_key((now - t0) / 2000 % 2 ? UI_KEY_UP : UI_KEY_DOWN);
            next += 60;
            steps++;
        }
        uint32_t wait = lv_timer_handler();
        int32_t until = (int32_t)(next - clock_ms32());
        int timeout = wait > (uint32_t)max_wait ? max_wait : (int)wait;
        if (until < timeout) timeout = until < 0 ? 0 : until;
        wakeups++;
        if (timeout > 0) usleep((useconds_t)timeout * 1000);
    }
    uint32_t w0 = clock_ms32();                   /* the last animation ends */
    while (clock_ms32() - w0 < 600) { uint32_t w = lv_timer_handler(); usleep((w > 20 ? 20 : w) * 1000); }
    double c = cpu_s() - c0;
    printf("%-28s %6.1f wakeups/s %6.1f flushes/s %8.0f px/s  cpu %5.2f %%  (%lu steps)\n", name,
           wakeups / (double)seconds, g_flushes / (double)seconds, g_flush_px / (double)seconds,
           100.0 * c / seconds, steps);
}

static int bench(theme_t *themes, const ui_fonts_t *fonts, int seconds)
{
    uint16_t *fb = g_bench_fb;
    lv_tick_set_cb(clock_ms32);
    lv_display_t *d = lv_display_create(UI_W, UI_H);
    lv_display_set_color_format(d, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(d, fb, NULL, sizeof g_bench_fb, LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(d, count_flush);
    ui_set_new_install(getenv("SIM_NEW_INSTALL") != NULL);   /* (a fresh install's Home) */
    ui_set_music_missing(getenv("SIM_NO_MUSIC") != NULL);   /* (no Music folder) */
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

#ifdef FATHOM_SIM_HEADLESS
/* (built without SDL: no window and no sound; the headless modes are all there is) */
int out_sdl_create(audio_output_t *out) { (void)out; return -1; }
#else
static uint32_t sdl_ticks(void) { return SDL_GetTicks(); }
#endif

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
    /* FATHOM_SIM_DB: another library file (a copy with other names, to look at
     * other scripts); FATHOM_SIM_PACKS: where character packs are (build/packs) */
    /* FATHOM_SIM_PACK_FIRST: the pack whose shapes draw the characters several have ("zh-hans") */
    const char *db = getenv("FATHOM_SIM_DB"), *packs = getenv("FATHOM_SIM_PACKS");
    ui_packs_dir(packs ? packs : "build/packs");
    ui_packs_first(getenv("FATHOM_SIM_PACK_FIRST"));
    g_lib = library_open(db && *db ? db : DB_PATH, ART_DIR);
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
    int theme = 0, zoom = 3, bench_s = 0, wifi_wait = 0, type_test = 0, ui_checks = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--screenshots") && i + 1 < argc) shots = argv[++i];
        else if (!strcmp(argv[i], "--motion") && i + 1 < argc) motion_dir = argv[++i];
        else if (!strcmp(argv[i], "--theme") && i + 1 < argc) theme = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--zoom") && i + 1 < argc) zoom = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--music") && i + 1 < argc) music = argv[++i];
        else if (!strcmp(argv[i], "--lang-check") && i + 1 < argc) lang_file = argv[++i];
        else if (!strcmp(argv[i], "--wifi-wait")) wifi_wait = 1;
        else if (!strcmp(argv[i], "--type-test")) type_test = wifi_wait = 1;   /* (set up as that test is) */
        else if (!strcmp(argv[i], "--ui-checks")) ui_checks = wifi_wait = g_bench = 1;   /* (and its songs last, as they do on the H2) */
        else if (!strcmp(argv[i], "--bench")) { bench_s = i + 1 < argc && atoi(argv[i + 1]) > 0 ? atoi(argv[++i]) : 5; g_bench = 1; }
        else { fprintf(stderr, "usage: %s [--theme N] [--zoom Z] [--music DIR] | --screenshots DIR | --motion DIR | --bench [S] | --wifi-wait\n", argv[0]); return 2; }
    }

    lv_init();
    static theme_t themes[UI_MAX_THEMES];
    g_themes = themes;
    g_n_themes = ui_load_themes(FATHOM_ASSETS_DIR, SIM_THEMES, themes, UI_MAX_THEMES);
    const ui_fonts_t *fonts = g_n_themes > 0 ? ui_theme_fonts(&themes[0]) : NULL;
    if (!fonts) return 1;
    char scratch[PATH_MAX] = "";
    if (wifi_wait && ww_scratch(scratch, sizeof scratch)) return 1;   /* (its own empty library, wherever it's run from) */
    setup_player(music, shots || motion_dir || bench_s || lang_file || wifi_wait);
    if (bench_s) return bench(themes, fonts, bench_s);
    if (wifi_wait) {
        int bad = ui_checks ? ui_check(themes, fonts) : type_test ? type_check(themes, fonts) : wifi_wait_check(themes, fonts);
        nftw(scratch, ww_rm, 8, FTW_DEPTH | FTW_PHYS);
        return bad;
    }
    rdir_set_fetch(sim_dir_fetch);
    g_shots = shots != NULL;
    podsync_set_fetch(sim_pod_fetch);
    podsync_set_download(sim_pod_get);
    if (shots || lang_file) sim_pod_seed("build/sim-data/podcasts-shots");
    podsync_start(shots || lang_file ? "build/sim-data/podcasts-shots" : "build/sim-data/podcasts");
    const char *covers = shots || lang_file ? "build/sim-data/covers-shots" : "build/sim-data/covers";
    library_set_found_covers(covers);
    cf_set_fetch(sim_cf_fetch);
    cf_set_download(sim_cf_download);
    if (shots || lang_file) cf_set_mb_gap_ms(0);
    cf_start(covers);
    ricon_set_fetch(sim_icon_fetch);
    ricon_start(shots || lang_file ? "build/sim-data/radio-icons-shots" : "build/sim-data/radio-icons");
    if (shots || lang_file) sim_icon_seed("build/sim-data/radio-icons-shots");
    if (lang_file) { g_shots = 1; return lang_check(lang_file, themes, fonts); }
    if (shots) return screenshots(shots, themes, fonts);
    if (motion_dir) return motion(motion_dir, themes, fonts, theme);

#ifdef FATHOM_SIM_HEADLESS
    (void)zoom;
    (void)theme;
    (void)on_key;
    fprintf(stderr, "fathom-sim: built without SDL (FATHOM_SIM_HEADLESS): the headless modes only\n");
    return 2;
#else
    lv_tick_set_cb(sdl_ticks);
    lv_display_t *disp = lv_sdl_window_create(UI_W, UI_H);
    lv_sdl_window_set_zoom(disp, (uint8_t)(zoom < 1 ? 1 : zoom));
    lv_indev_t *kb = lv_sdl_keyboard_create();
    lv_group_t *g = lv_group_create();
    lv_indev_set_group(kb, g);

    ui_set_new_install(getenv("SIM_NEW_INSTALL") != NULL);   /* (a fresh install's Home) */
    ui_set_music_missing(getenv("SIM_NO_MUSIC") != NULL);   /* (no Music folder) */
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
#endif
}
