/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* The player on the H2: internet radio as the interface asks for it (the
 * station list's file, whether a station can play now, the stations' live
 * info) and the podcasts' OPML file (main_int.h). Out of main.c, where it
 * stood. */
#include "main_int.h"

/* Home > Radio: the stations in fathom/radio.txt, or until there is one, the
 * example that comes with Fathom (fathom/assets/radio.txt, replaced by updates;
 * a station of yours that's in it too takes its icon from there if it has none). */
int hook_radio_stations(radio_station_t *out, int max)
{
    char path[600], example[600];
    snprintf(path, sizeof path, "%s/radio.txt", g_data_dir);
    snprintf(example, sizeof example, "%s/assets/radio.txt", g_data_dir);
    int n = radio_load_list(path, example, out, max);
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
int hook_radio_add(const radio_station_t *st)
{
    char path[600], example[600];
    snprintf(path, sizeof path, "%s/radio.txt", g_data_dir);
    snprintf(example, sizeof example, "%s/assets/radio.txt", g_data_dir);
    return radio_add(path, example, st);
}

int hook_radio_remove(const char *url)
{
    char path[600], example[600];
    snprintf(path, sizeof path, "%s/radio.txt", g_data_dir);
    snprintf(example, sizeof example, "%s/assets/radio.txt", g_data_dir);
    return radio_remove(path, example, url);
}

/* Home > Podcasts: an OPML file (subscriptions exported from another app) to
 * offer, the first by name at the top of the card, in Music or in
 * fathom/podcasts. */
int hook_podcast_opml(char *path, size_t cap)
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
int hook_radio_ready(char *why, size_t cap)
{
    if (net_up()) return 0;
    char busy[32] = "";
    int on = wifi_ui_state(busy, sizeof busy, NULL);   /* (starting: not on yet, but not off either) */
    if (!wifi_available()) snprintf(why, cap, "The radio needs Wi-Fi");
    else if (!on && !busy[0]) snprintf(why, cap, "Wi-Fi is off: Settings > Wi-Fi");
    else snprintf(why, cap, "Not connected yet: Settings > Wi-Fi");
    return -1;
}

/* Twice a second: the radio's live info (stations whose streams don't say);
 * Wi-Fi switched off stops the radio (rather than it waiting for it). */
void radio_poll(lv_timer_t *t)
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
    radio_net_feed();
    /* The info is asked for once the station plays, not while it connects: a
     * second TLS handshake then is CPU taken from the first on the one core.
     * (While it connects there's none wanted: a station change drops the
     * request for the last one's, in flight or not.) */
    radiometa_want(s && n.playing && !n.connecting ? s->info : NULL);
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

/* The network as the radio sees it: a station that's wanted waits while
 * Wi-Fi is gone by itself (out of range, the router restarting) and comes
 * back with it (player_radio_net). A proxy counts as a network, as in
 * hook_radio_ready. */
void radio_net_feed(void)
{
    if (g_player) player_radio_net(g_player, net_up());
}
