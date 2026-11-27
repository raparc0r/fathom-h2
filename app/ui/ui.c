/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
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
#include "ui/ui_internal.h"

/* ---------- menus ---------- */

/* Home: every item, in the default order. Settings > Interface > Home screen items shows, hides and
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
/* Hidden until shown in Settings > Interface > Home screen items (fewer people tag them); on a fresh
 * install also Genres and Folders, for a shorter Home (ui_set_new_install) */
static int g_new_install;
static int home_hidden_by_default(int h)
{
    return h == H_COMPOSERS || h == H_YEARS || (g_new_install && (h == H_GENRES || h == H_FOLDERS));
}

void ui_set_new_install(int on) { g_new_install = on; }
static int g_music_missing;
void ui_set_music_missing(int missing) { g_music_missing = missing; }
static void home_save(void);
/* Their icons: Tabler Icons code points (player-play, arrows-shuffle, search,
 * disc, microphone-2, users, music, playlist, heart, clock-plus, history,
 * radio, settings; then category, feather, calendar, folder), and Pixel Pastel's own, drawn on its 10 px grid (pixel_icons.h,
 * from tools/icons/pixel_icons.py), in the same order. */
static const uint32_t HOME_ICON[H_N] = {0xed46, 0xf000, 0xeb1c, 0xea90, 0xef2c, 0xebf2,
                                        0xeafc, 0xeec0, 0xeabe, 0xf7c5, 0xebea, 0xef2d, 0xeb20,
                                        0xf1f6, 0xee8b, 0xea53, 0xeaad,    /* category, feather, calendar, folder */
                                        PODCAST_ICON};
#include "pixel_icons.h"

/* Letters for the wheel: search (results last) and names (done last). */
static const char SEARCH_CHARS[] = "abcdefghijklmnopqrstuvwxyz0123456789 ";
static const char NAME_CHARS[] = "abcdefghijklmnopqrstuvwxyz0123456789 -'&.,!?()";
/* passwords: every printable character, in groups Menu jumps between */
static const char PASS_CHARS[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
                                 " !\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~";
static const int PASS_GROUPS[] = {0, 26, 52, 62};   /* a, A, 0, symbols */
static const char *const HISTORY[] = {"Recently played", "Most played"};
/* Settings' pages, in the order the owner set (2026-11-23): what's most
 * wanted first. (The Home screen's page is a row of Interface's.) */
static const char *const SETTINGS[] = {"Sound", "Playback", "Interface", "Wi-Fi", "Bluetooth", "Podcasts", "Display",
                                       "Library", "USB connection", "Network drive", "Power", "SSH", "About"};

/* Quick settings (quick_rows(): Bluetooth's and Wi-Fi's rows where they are) */
static const setting_t QUICK[] = {ST_BRIGHTNESS, ST_SHUFFLE, ST_REPEAT, ST_EQ, ST_WIDTH, ST_BT, ST_WIFI,
                                  ST_SLEEP, ST_STOP_AFTER};
/* (the two modes for the cleanest sound, then the tone, then the level; USB
 * DAC mode is USB connection's) */
static const setting_t SOUND_ROWS[] = {ST_OUTPUT, ST_PURE, ST_TRANSPORT, ST_EQ, ST_CROSSFEED, ST_WIDTH, ST_BALANCE,
                                       ST_LEVELLING, ST_VOL_LIMIT, ST_DAC_FILTER};
/* The sound hook's name for a setting changed with the wheel. */
static int sound_of(setting_t st)
{
    return st == ST_BALANCE ? UI_SOUND_BALANCE : st == ST_WIDTH ? UI_SOUND_WIDTH : UI_SOUND_LIMIT;
}   /* sound_rows(): those with their hooks */
static const setting_t PLAYBACK_ROWS[] = {ST_SHUFFLE, ST_SHUF_BY, ST_SHUF_FRESH, ST_SHUF_INTERLUDES, ST_SHUF_WORKS,
                                         ST_SHUF_SPOKEN, ST_REPEAT, ST_SLEEP, ST_PAUSE_UNPLUG};

/* Settings > Playback's shuffle rows, to the player (at start and when changed). */
static void shuffle_prefs_apply(void);
static const setting_t POWER_ROWS[] = {ST_BATTERY, ST_AUTO_OFF};
static const setting_t POD_ROWS[] = {ST_POD_AUTO, ST_POD_KEEP, ST_POD_CLEAN, ST_POD_SPEED};
static const int POD_KEEPS[] = {1, 2, 3, 5};
static const int POD_CLEANS[] = {1, 3, 7, 30, 0};            /* days; 0 = never */
static const int POD_SPEEDS[] = {100, 125, 150, 175, 200};   /* percent */
/* (the last, Files not read, only while there are any: library_rows()) */
static const setting_t LIBRARY_ROWS[] = {ST_RESCAN, ST_FIND_COVERS, ST_N_SONGS, ST_N_ALBUMS, ST_N_ARTISTS, ST_N_PLAYLISTS, ST_N_UNREAD};
static const setting_t USB_ROWS[] = {ST_USB_COMPUTER, ST_USB_DRIVE, ST_USB_DAC};
static const int AUTO_OFFS[] = {5, 15, 30, 0};               /* minutes; 0 = never */
static const int SLEEPS[] = {0, 15, 30, 45, 60, 90, PLAYER_SLEEP_END_ALBUM};   /* minutes; 0 = off */
static const setting_t DISPLAY_ROWS[] = {ST_BRIGHTNESS, ST_TIMEOUT, ST_DIM, ST_ON_CHARGING};
/* (Language packs last: it isn't there without the packs' hooks) */
static const setting_t INTERFACE_ROWS[] = {ST_THEME, ST_HOME_SCREEN, ST_COVERS, ST_ALBUM_FLOW, ST_ALBUM_ARTISTS, ST_LANGUAGE, ST_PACKS};
static const int TIMEOUTS[] = {15, 30, 60, 120, 300, 0};   /* seconds; 0 = never */

static const char *const CTX_LABEL[] = {
    "Play", "Play next", "Add to queue", "Show queue", "Go to album", "Go to artist", "Info",
    "Play now", "Move", "Remove from queue", "Clear queue", "Add to favourites", "Remove from favourites",
    "Add to playlist", "Remove from playlist", "Move", "Delete playlist", "Rename playlist", "Move",
    "Forget this device", "Forget this network",
    "Play", "Add to your stations", "Remove from your stations",
    "Check for new episodes", "Unsubscribe", "Download", "Cancel download", "Delete download", "Mark as played",
    "Mark as not played", "Chapters", "Speed", "Find cover", "Remove cover", "Find another cover",
    "Shuffle", "Lyrics",
};

struct ui_state g_ui;   /* (S: ui_internal.h) */

static const char *const RD_MENU[] = {"Search by name", "Random station", "Popular", "Genres", "Countries"};

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

static void packs_look(void);
typedef struct { int kind, pack; } pack_row_t;  /* (a row of Language packs: below) */
static int packs_rows(pack_row_t *r);

/* A pack the library's names call for that isn't there: among `ids`, a list
 * as packs.ignored keeps ("ja,ko")? */
static int id_listed(const char *ids, const char *id)
{
    size_t n = strlen(id);
    for (const char *p = ids; n && (p = strstr(p, id)); p += n)
        if ((p == ids || p[-1] == ',') && (p[n] == ',' || !p[n])) return 1;
    return 0;
}

/* The packs Home offers: those the library has names for (songs), not there,
 * not put off. Their places among S.packs into at[] (UI_MAX_PACKS); how many. */
static int packs_offered_now(int *at);
static int packs_offered(int *at)
{
    if (!S.hooks.packs_state || !S.hooks.packs_do) return 0;
    packs_look();
    return packs_offered_now(at);
}

/* ...from the packs as last looked at (not asking the device again) */
static int packs_offered_now(int *at)
{
    if (!S.hooks.packs_state || !S.hooks.packs_do) return 0;
    const char *ignored = settings_get("packs.ignored", "");
    int n = 0;
    for (int i = 0; i < S.npacks; i++)
        if (S.packs[i].songs > 0 && S.packs[i].state != UI_PACK_THERE && !id_listed(ignored, S.packs[i].id)) {
            if (at) at[n] = i;
            n++;
        }
    return n;
}

static void home_rebuild(void)
{
    S.home_n = 0;
    if (update_offered()) S.home[S.home_n++] = H_UPDATE;
    if (packs_offered(NULL)) S.home[S.home_n++] = H_PACKS;
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
    /* a fresh install's shorter Home, kept (from the next start it's no longer new) */
    if (g_new_install && !settings_get("home.items", "")[0]) home_save();
    home_rebuild();
}

/* home.items: Home's items in order, "-" before a hidden one */
static void home_save(void)
{
    char buf[512] = "";
    size_t w = 0;
    for (int i = 0; i < H_N && w < sizeof buf; i++) {
        int h = S.home_order[i];
        w += (size_t)snprintf(buf + w, sizeof buf - w, "%s%s%s", i ? "," : "", S.home_hidden[h] ? "-" : "", HOME_ID[h]);
    }
    settings_set("home.items", buf);
}

/* Settings > Interface > Home screen items changed something: keep it, and keep Home's selection valid. */
static void home_changed(void)
{
    int was = S.stack[0].sel < S.home_n ? S.home[S.stack[0].sel] : H_SETTINGS;
    home_save();
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
    int has_packs = (S.home_n > 0 && S.home[0] == H_PACKS) || (S.home_n > 1 && S.home[1] == H_PACKS);
    if (has == (update_offered() != NULL) && has_packs == (packs_offered(NULL) > 0)) return;
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

/* Settings > Bluetooth: what the platform says now. Read where the page is
 * drawn (render_list) and nowhere else: read at every count of its rows, a
 * key acted on a list newer than the one on the screen. The selection is a
 * device, not a row: it stays on its device when the list comes in another
 * order. (The same for Wi-Fi's networks, below: by row, a search that put
 * the strongest first left the highlight on another network, which <Play>
 * then joined.) */
static void bt_look(void)
{
    char was[sizeof S.stack / sizeof S.stack[0]][18];
    for (int d = 0; d < S.depth; d++) {
        int i = S.stack[d].sel - BT_DEV_ROW;
        snprintf(was[d], sizeof was[d], "%s", S.stack[d].kind == L_BLUETOOTH && i >= 0 && i < S.bt_n ? S.bt[i].addr : "");
    }
    S.bt_n = 0;
    S.bt_on = 0;
    S.bt_busy[0] = '\0';
    if (!S.hooks.bt_state) return;
    S.bt_on = S.hooks.bt_state(S.bt_busy, sizeof S.bt_busy);
    if (S.bt_on && S.hooks.bt_devices) S.bt_n = S.hooks.bt_devices(S.bt, (int)(sizeof S.bt / sizeof S.bt[0]));
    for (int d = 0; d < S.depth; d++)
        for (int i = 0; was[d][0] && i < S.bt_n; i++)
            if (!strcmp(S.bt[i].addr, was[d])) { S.stack[d].sel = BT_DEV_ROW + i; break; }
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
    char was[sizeof S.stack / sizeof S.stack[0]][33];
    for (int d = 0; d < S.depth; d++) {
        int i = S.stack[d].sel - 3;                  /* (its networks' rows: wifi_row) */
        snprintf(was[d], sizeof was[d], "%s", S.stack[d].kind == L_WIFI && i >= 0 && i < S.wifi_n ? S.wifi[i].ssid : "");
    }
    S.wifi_n = 0;
    S.wifi_on = 0;
    S.wifi_busy[0] = '\0';
    memset(&S.wifi_st, 0, sizeof S.wifi_st);
    if (!S.hooks.wifi_state) return;
    S.wifi_on = S.hooks.wifi_state(S.wifi_busy, sizeof S.wifi_busy, &S.wifi_st);
    if (S.wifi_on && S.hooks.wifi_networks)
        S.wifi_n = S.hooks.wifi_networks(S.wifi, (int)(sizeof S.wifi / sizeof S.wifi[0]));
    for (int d = 0; d < S.depth; d++)
        for (int i = 0; was[d][0] && i < S.wifi_n; i++)
            if (!strcmp(S.wifi[i].ssid, was[d])) { S.stack[d].sel = 3 + i; break; }
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
enum { AB_INFO, AB_WHATSNEW, AB_TIPS, AB_CHECK, AB_AUTO, AB_UNINSTALL, AB_RESET };
static int about_row(int i)
{
    if (i < S.about_n) return AB_INFO;
    i -= S.about_n;
    if (S.wn_text[0] && i-- == 0) return AB_WHATSNEW;   /* (a release's: not in builds of my own) */
    if (i-- == 0) return AB_TIPS;                       /* (the welcome's) */
    if (S.hooks.update_do) {
        if (i < 2) return i == 0 ? AB_CHECK : AB_AUTO;
        i -= 2;
    }
    if (S.hooks.uninstall_do && i-- == 0) return AB_UNINSTALL;
    return AB_RESET;
}

/* Uninstall Fathom's rows, by its state: what it found, then what to do. */
enum { UR_STATUS, UR_LOOK, UR_VERSION, UR_GO };
/* Language packs: the packs, as the device has them now */
static void packs_look(void)
{
    S.npacks = S.hooks.packs_state ? S.hooks.packs_state(S.packs, UI_MAX_PACKS) : 0;
    if (S.npacks < 0) S.npacks = 0;
    if (S.npacks > UI_MAX_PACKS) S.npacks = UI_MAX_PACKS;
}

/* With two or more packs there that share characters (Japanese and a Chinese,
 * the two Chinese): the one whose shapes are used, for the list's last row,
 * "Shared characters". -1: fewer than two, no such row. */
static int packs_first(void)
{
    int there = 0, first = -1, marked = -1;
    for (int i = 0; i < S.npacks; i++) {
        if (!S.packs[i].han || S.packs[i].state != UI_PACK_THERE) continue;
        there++;
        if (first < 0) first = i;
        if (S.packs[i].first && marked < 0) marked = i;
    }
    return there < 2 ? -1 : marked >= 0 ? marked : first;
}

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
    case L_BLUETOOTH: return S.bt_on ? BT_DEV_ROW + S.bt_n : 1;     /* (as last drawn: bt_look) */
    case L_WIFI: return S.wifi_on ? 3 + S.wifi_n : 1;   /* see wifi_row */
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
    case L_QUICK: return quick_n();
    case L_USB: return (int)(sizeof USB_ROWS / sizeof USB_ROWS[0]);
    case L_DISPLAY: return (int)(sizeof DISPLAY_ROWS / sizeof DISPLAY_ROWS[0]);
    case L_INTERFACE: return (int)(sizeof INTERFACE_ROWS / sizeof INTERFACE_ROWS[0]) - (S.hooks.packs_state == NULL);
    case L_PACKS: return packs_rows(NULL);
    case L_SOUND: {
        setting_t r[sizeof SOUND_ROWS / sizeof SOUND_ROWS[0]];
        return sound_rows(r);
    }
    case L_PLAYBACK: return (int)(sizeof PLAYBACK_ROWS / sizeof PLAYBACK_ROWS[0]);
    case L_POWER: return (int)(sizeof POWER_ROWS / sizeof POWER_ROWS[0]);
    case L_POD_SETTINGS: return (int)(sizeof POD_ROWS / sizeof POD_ROWS[0]);
    case L_LIBRARY: return (int)(sizeof LIBRARY_ROWS / sizeof LIBRARY_ROWS[0]) - (S.lib_unread <= 0);
    case L_UNREAD: return S.lib_unread > 0 ? S.lib_unread : 1;
    case L_SIGNAL: {
        int n = 0;
        char a[8], b[8];
        while (S.hooks.signal_path && n < 16 && S.hooks.signal_path(n, a, sizeof a, b, sizeof b) == 0) n++;
        return n;
    }
    case L_ABOUT:
        about_look();
        return S.about_n + (S.wn_text[0] != 0) + 1 + 2 * (S.hooks.update_do != NULL) + (S.hooks.uninstall_do != NULL) + 1;   /* about_row */
    case L_UNINSTALL: uninstall_look(); return uninstall_rows();
    case L_UPDATE: update_look(); return S.upd.state == UI_UPD_AVAILABLE || S.upd.state == UI_UPD_FAILED ? 2 : 1;
    case L_BATTERY: return 3;                      /* the graph's range, since full charge, history */
    case L_BAT_SPAN: return 2 + BL_ACTS;            /* on battery, at this rate, each activity */
    case L_BAT_HISTORY: return S.bat.nspans ? S.bat.nspans + 1 : 0;   /* + the average */
    case L_CONTEXT: return S.ctx_n;
    case L_INFO: return INFO_N;
    case L_QUEUE: return S.player ? player_queue_len(S.player) : 0;
    case L_PICK_PLAYLIST: return 1 + S.pick_pls.n;   /* "New playlist" + yours */
    case L_SEARCH: return 1 + S.found.n;
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

/* The value after cur in a table of n, round; tab[miss] where cur isn't one of them */
#define COUNT_OF(a) ((int)(sizeof (a) / sizeof (a)[0]))
static int next_of(const int *tab, int n, int cur, int miss)
{
    int i = 0;
    while (i < n && tab[i] != cur) i++;
    return tab[i < n ? (i + 1) % n : miss];
}

/* An on/off setting, turned the other way */
static void setting_flip(const char *key, int def)
{
    settings_set_int(key, !settings_get_int(key, def));
}

/* Podcasts' speed: the next one, for what plays now and kept for the next */
static void pod_speed_next(void)
{
    int to = next_of(POD_SPEEDS, COUNT_OF(POD_SPEEDS), S.player ? player_speed(S.player) : 100, 1);
    if (S.player) player_set_speed(S.player, to);
    settings_set_int("podcasts.speed", to);
}

static void next_timeout(void)
{
    int v = next_of(TIMEOUTS, COUNT_OF(TIMEOUTS), screen_timeout(), 1);
    settings_set_int("display.timeout", v);
    if (S.hooks.screen_timeout) S.hooks.screen_timeout(v);
}

/* Quick settings' rows: Bluetooth's, Wi-Fi's and the stereo width's only where there are. */
static int quick_rows(setting_t *out)
{
    int n = 0;
    for (size_t i = 0; i < sizeof QUICK / sizeof QUICK[0]; i++) {
        if (QUICK[i] == ST_BT && !S.hooks.bt_state) continue;
        if (QUICK[i] == ST_WIFI && !S.hooks.wifi_state) continue;
        if (QUICK[i] == ST_WIDTH && !S.hooks.sound_value) continue;
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
        if ((st == ST_BALANCE || st == ST_VOL_LIMIT || st == ST_CROSSFEED || st == ST_WIDTH) && !S.hooks.sound_value) continue;
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

/* The pages whose rows are settings (setting_at), as a switch's labels:
 * `case SETTING_PAGES:` */
#define SETTING_PAGES L_QUICK: case L_DISPLAY: case L_SOUND: case L_PLAYBACK: case L_POWER: case L_LIBRARY: case L_USB: \
                      case L_POD_SETTINGS: case L_INTERFACE
static int is_setting_level(list_kind_t k)
{
    switch (k) {
    case SETTING_PAGES: return 1;
    default: return 0;
    }
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

/* (each 1 if the level was opened, 0 if every level is in use: stack_full) */
static int push_as(list_kind_t kind, const char *title, int64_t parent, int ui);
static int stack_full(void);
/* A level titled with a name (an album, an artist, a device) */
static int push(list_kind_t kind, const char *title, int64_t parent) { return push_as(kind, title, parent, 0); }
/* A level titled with the UI's own words ("Albums", "Settings"): translated */
/* Settings > Library's counts: read as it opens, and again when the library
 * changes under it (a rescan's end shows it) */
static void lib_counts_load(void)
{
    static const lib_view_t v[4] = {LIB_SONGS, LIB_ALBUMS, LIB_ARTISTS, LIB_PLAYLISTS};
    /* (counted by the library: each was a whole sorted list made and thrown away, seconds for a big library) */
    for (int i = 0; S.lib && i < 4; i++) S.lib_counts[i] = library_count(S.lib, v[i], 0);
    S.lib_unread = S.lib ? library_unread_count(S.lib) : 0;
}

/* Files not read: a file's name, the whole row for it (<Play> says where it is) */
static void unread_row(int i, char *main, size_t mn)
{
    char path[1024];
    if (!S.lib || S.lib_unread <= 0 || library_unread(S.lib, i, path, sizeof path)) {
        snprintf(main, mn, "%s", tr("None"));
        return;
    }
    const char *name = strrchr(path, '/');
    snprintf(main, mn, "%s", name ? name + 1 : path);
}

static int push_ui(list_kind_t kind, const char *title, int64_t parent) { return push_as(kind, title, parent, 1); }
static void about_look(void);
/* once Wi-Fi connects: nothing, OK again, ask the directory again, search for the podcast typed */
enum { WT_NONE, WT_OK, WT_RDIR, WT_POD_SEARCH };
static int wifi_ask_if_off(int then);
static void pod_search_run(void);
static void open_selected(void);

/* Opens the editor on a profile; it plays while it's being edited. */
static void eqe_open(const char *name)
{
    if (stack_full()) return;
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

/* Volume levelling's measuring: songs ready (gains from tags, or measured)
 * of all; looked up again at most every 5 s (the rows are drawn each
 * second). The percent ready, or -1: levelling off, or nothing left. */
static int levelling_left(int *done, int *total)
{
    static int d, n;
    static uint32_t at;
    static int looked;
    if (!looked || lv_tick_elaps(at) >= 5000) {
        d = n = 0;
        if (S.lib) library_rg_progress(S.lib, &d, &n);
        at = lv_tick_get();
        looked = 1;
    }
    if (done) *done = d;
    if (total) *total = n;
    if (settings_get_int("sound.levelling", LEVEL_OFF) == LEVEL_OFF || n <= 0 || d >= n) return -1;
    return (int)((int64_t)d * 100 / n);   /* (99 till the last one) */
}

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
    case ST_HOME_SCREEN: snprintf(main, mn, "%s", tr("Home screen items")); break;
    case ST_PACKS: snprintf(main, mn, "%s", tr("Language packs")); break;
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
    case ST_ALBUM_FLOW: {
        static const char *const W[] = {"Off", "Everywhere", "Inside artists"};
        int v = settings_get_int("display.album_flow", UI_ALBUM_FLOW_DEFAULT);
        snprintf(main, mn, "%s", tr("Cover flow"));
        snprintf(meta, tn, "%s", tr(W[v >= 0 && v <= 2 ? v : 0]));
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
        int pc = S.pure ? -1 : levelling_left(NULL, NULL);
        if (pc >= 0) snprintf(meta, tn, "%s, %d%%", tr(how[v >= 0 && v <= 2 ? v : 0]), pc);   /* (measuring) */
        else snprintf(meta, tn, "%s", tr(S.pure ? "Bypassed" : how[v >= 0 && v <= 2 ? v : 0]));
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
    case ST_WIDTH:
    case ST_VOL_LIMIT: {
        char v[48] = "";
        snprintf(main, mn, "%s", tr(st == ST_BALANCE ? "Balance" : st == ST_WIDTH ? "Stereo width" : "Volume limit"));
        if (st != ST_VOL_LIMIT && S.pure) { snprintf(meta, tn, "%s", tr("Bypassed")); break; }
        if (S.hooks.sound_value) S.hooks.sound_value(sound_of(st), v, sizeof v);
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
    case ST_RESCAN: {
        snprintf(main, mn, "%s", tr("Rescan the music folder"));
        ui_scan_t p;   /* (one running: how far, every second) */
        if (S.hooks.scan_progress && S.hooks.scan_progress(&p)) {
            char d[16], t[16];
            snprintf(d, sizeof d, "%d", p.done);
            snprintf(t, sizeof t, "%d", p.total);
            int covers = p.step == UI_SCAN_COVERS;
            if (covers && p.total > 0 && p.done <= p.total) tr_fill(meta, tn, "Covers %s of %s", d, t);
            else if (covers) snprintf(meta, tn, "%s", tr("Covers"));
            else if (p.step == UI_SCAN_SONGS && p.total > 0 && p.done < p.total) tr_fill(meta, tn, "%s of %s", d, t);
            else snprintf(meta, tn, "%s", tr("Working"));
        }
        break;
    }
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
    case ST_N_UNREAD: snprintf(main, mn, "%s", tr("Files not read")); snprintf(meta, tn, "%d", S.lib_unread); break;
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
            tr_fill(main, mn, "Disc %s, track %s", d, n);
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
/* Where the next letter goes in what's typed (its end, unless <Prev> moved it). */
static int entry_cur(void)
{
    int len = (int)strlen(S.entry);
    if (S.entry_back > len) S.entry_back = len;
    if (S.entry_back < 0) S.entry_back = 0;
    return len - S.entry_back;
}

/* A name being changed can have letters of several bytes (the é of "Café"):
 * the cursor steps over a whole one, and <Back> deletes a whole one. Where
 * the letter before byte `i` starts, and where the one at `i` ends. */
static int entry_before(const char *s, int i)
{
    if (i > 0) i--;
    while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80) i--;
    return i;
}

static int entry_after(const char *s, int i)
{
    if (s[i]) i++;
    while (((unsigned char)s[i] & 0xC0) == 0x80) i++;
    return i;
}

const char *ui_test_entry(int *cursor)
{
    if (cursor) *cursor = entry_cur();
    return S.entry;
}

static void entry_row(list_kind_t k, char *main, size_t mn, char wheel[3][WHEEL_CAP])
{
    int cur = entry_cur();
    if ((k == L_SEARCH || k == L_RDIR_SEARCH || k == L_POD_SEARCH) && !S.entry[0]) snprintf(main, mn, "_");   /* (how to type is said under it) */
    else if (k == L_WIFI_PASS && !S.entry[0]) snprintf(main, mn, "%s", tr("Password"));
    else if (S.entry[cur]) snprintf(main, mn, "%.*s\xC2\xA6%s", cur, S.entry, S.entry + cur);   /* inside it: a broken bar, no letter of a password */
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

/* The wheel is typing: the typing row is selected and hasn't been left. Once
 * the results under it were gone to (twice <Play>, or they came), the row is
 * one more row of the list: the wheel passes over it (an overshoot at the top
 * of the results no longer changed letters, with twice <Play> to get back),
 * what's typed shows without its cursor or the wheel's letters, and <Play> on
 * it types again (<Back> in the results goes up to it; <Back> on it leaves). */
static int typing_on(level_t *l)
{
    if (!is_typing_row(l, 0)) return 0;
    if (l->sel != 0) l->typed = 1;
    return !l->typed;
}

int ui_test_typing(void) { return typing_on(&S.stack[S.depth - 1]); }
int ui_test_pill_row(void) { return S.stack[S.depth - 1].top + S.pill_target / ROW_PITCH; }
int ui_test_ctx_row(const char *label)
{
    for (int i = 0; S.stack[S.depth - 1].kind == L_CONTEXT && i < S.ctx_n; i++)
        if (!strcmp(CTX_LABEL[S.ctx[i]], label)) return i;
    return -1;
}

/* Search result r (the closest names first, whatever they are): 0 an artist,
 * 1 an album, 2 a song, and its id (-1: none). */
static int search_kind(int r, int64_t *id)
{
    if (r < 0 || r >= S.found.n) { *id = -1; return 2; }
    *id = S.found.hits[r].id;
    return S.found.hits[r].kind == LIB_HIT_ARTIST ? 0 : S.found.hits[r].kind == LIB_HIT_ALBUM ? 1 : 2;
}

static void search_row(int r, char *main, size_t mn, char *meta, size_t tn)
{
    int64_t id;
    int kind = search_kind(r, &id);
    if (!S.lib || id < 0) return;
    /* (what each is, its icon says: render(); at the right, as in its own
     * list: an artist's albums, an album's artist or year, a song's length.
     * A song's artist goes after its title, smaller: render()) */
    if (kind == 0) {
        library_row(S.lib, LIB_ARTISTS, id, main, mn, meta, tn);
        /* the albums opening it shows (push_artist): those it's credited on,
         * or its own where there are none (an album artist only: it said 0) */
        int own = 0, credited = 0;
        library_artist_counts(S.lib, id, &own, &credited);
        snprintf(meta, tn, "%d", credited > 0 ? credited : own);
    }
    else if (kind == 1) library_row(S.lib, LIB_ALBUMS, id, main, mn, meta, tn);
    else library_row(S.lib, LIB_SONGS, id, main, mn, meta, tn);
}

static void search_run(void)
{
    const char *text = S.entry_kept_on ? S.entry_kept : S.entry;   /* (a password being typed over Search: Search's own) */
    library_search_free(&S.found);
    if (S.lib && text[0]) library_search(S.lib, text, &S.found);
}

/* The typing text emptied, the cursor at its end and the wheel on its first
 * character: wiped whole (a password), or only cut short */
static void entry_wipe(void)
{
    memset(S.entry, 0, sizeof S.entry);
    S.entry_back = 0;
    S.entry_ch = 0;
}

static void entry_empty(void)
{
    S.entry[0] = '\0';
    S.entry_back = 0;
    S.entry_ch = 0;
}

/* The lowest open level of a kind (its index in the stack), -1 if none is */
static int level_find(list_kind_t kind)
{
    for (int d = 0; d < S.depth; d++)
        if (S.stack[d].kind == kind) return d;
    return -1;
}

/* The Wi-Fi password's screen opens: what another typing screen under it
 * holds is put aside (one text serves them all), and is back when the last
 * password screen closes. The password never stays in memory after it. */
static void entry_keep(void)
{
    if (!S.entry_kept_on) {
        memcpy(S.entry_kept, S.entry, sizeof S.entry_kept);
        S.entry_kept_ch = S.entry_ch;
        S.entry_kept_back = S.entry_back;
        S.entry_kept_on = 1;
    }
    entry_wipe();
}

static void entry_unkeep(void)
{
    entry_wipe();
    if (level_find(L_WIFI_PASS) >= 0) return;      /* (another under it: typed again there) */
    if (!S.entry_kept_on) return;
    memcpy(S.entry, S.entry_kept, sizeof S.entry);
    S.entry_ch = S.entry_kept_ch;
    S.entry_back = S.entry_kept_back;
    memset(S.entry_kept, 0, sizeof S.entry_kept);
    S.entry_kept_on = 0;
}

/* ---------- the station directory: what the lists show ---------- */

/* Your stations as the card has them now (-1: none can be read), and their
 * icons: each one's place in the list's cache, and those not on the card yet
 * fetched meanwhile if the network's up (never a directory list's: only
 * stations of yours). */
#define COV_RADIO (1LL << 61)   /* (with COV_POD, below) */
#define COV_KEY_BITS 0xFFFFFFFFFFFFLL
static void thumbs_reset(int64_t tag);
/* Which picture each station's row shows: its own logo's, its site's icon's
 * or the directory's, whichever the card has. Asked again whenever an icon
 * arrives: a station whose logo was no use gets its site's icon then, under
 * another name (the row kept waiting for the logo's file until Radio was
 * left and opened again). */
static void radio_keys(void)
{
    for (int i = 0; i < S.radio_n && i < RADIO_MAX; i++) {
        char id[13];
        ricon_station_id(&S.radio[i], id);       /* (its own logo, or its site's icon) */
        S.radio_key[i] = id[0] ? COV_RADIO | (int64_t)(strtoull(id, NULL, 16) & COV_KEY_BITS) : -1;
    }
}

static void radio_read(void)
{
    S.radio_n = S.hooks.radio_stations ? S.hooks.radio_stations(S.radio, RADIO_MAX) : -1;
    radio_keys();
    char why[96];
    int net = !S.hooks.radio_ready || S.hooks.radio_ready(why, sizeof why) == 0;
    ricon_sync(S.radio, S.radio_n, net);
    thumbs_reset(COV_RADIO);
}

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
/* Why an update or the uninstall failed: for want of space (how much it
 * needs), or its own message */
static void failure_text(char *out, size_t cap, int need_mb, const char *message)
{
    if (need_mb > 0) tr_num(out, cap, "Not enough space on the card: %s MB needed", need_mb);
    else snprintf(out, cap, "%s", tr(message));
}

/* ...and said whole in a toast, where a row cuts it */
static void failure_toast(int need_mb, const char *message)
{
    char why[200];
    failure_text(why, sizeof why, need_mb, message);
    ui_toast(why, 3000);
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
        if (i == 0) { tr_fill(main, mn, "Update to %s", u->latest, NULL); fmt_mb(meta, tn, (uint64_t)u->size); }
        else { snprintf(main, mn, "This version"); short_version(meta, tn, u->current); }
        break;
    case UI_UPD_FAILED:
        if (i == 0) failure_text(main, mn, u->need_mb, u->message);
        else snprintf(main, mn, "Try again");
        break;
    default: tr_fill(main, mn, "Updating to %s", u->latest, NULL); break;   /* (under the progress screen) */
    }
}

/* Why Uninstall can't go on, translated: the text, or the space it needs */

/* The packs the library has names for that aren't there: their places among
 * S.packs into at[] (UI_MAX_PACKS); how many. (packs_offered: the same, less
 * those put off: what Home offers.) */
static int packs_needed(int *at)
{
    int n = 0;
    for (int i = 0; i < S.npacks; i++)
        if (S.packs[i].songs > 0 && S.packs[i].state != UI_PACK_THERE) {
            if (at) at[n] = i;
            n++;
        }
    return n;
}

/* ...and the one of them being fetched now (its place among S.packs; the last, were there two), -1 if none */
static int packs_getting(void)
{
    int at[UI_MAX_PACKS], k = packs_needed(at), getting = -1;
    for (int j = 0; j < k; j++)
        if (S.packs[at[j]].state == UI_PACK_GETTING) getting = at[j];
    return getting;
}

/* Language packs' rows: "Needed by your library" while it needs any; the
 * packs, those it has names for first; "Shared characters" with two packs
 * that share some; "Hide from Home" while Home offers. */
enum { PR_NEEDED, PR_PACK, PR_SHARED, PR_HIDE };
#define PACK_ROWS (UI_MAX_PACKS + 3)

static int packs_rows_now(pack_row_t *r);

static int packs_rows(pack_row_t *r)
{
    pack_row_t own[PACK_ROWS];
    packs_look();
    return packs_rows_now(r ? r : own);
}

/* ...from the packs as last looked at (not asking the device again) */
static int packs_rows_now(pack_row_t *r)
{
    int n = 0;
    if (packs_needed(NULL)) r[n++] = (pack_row_t){PR_NEEDED, -1};
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < S.npacks; i++)
            if ((S.packs[i].songs > 0) == (pass == 0)) r[n++] = (pack_row_t){PR_PACK, i};
    if (packs_first() >= 0) r[n++] = (pack_row_t){PR_SHARED, -1};
    if (packs_offered_now(NULL)) r[n++] = (pack_row_t){PR_HIDE, -1};
    return n;
}

/* A row of the page. A pack's: its name; beside it the download's size, how
 * far it is, or that it is there. */
static void packs_row_text(int i, char *main, size_t mn, char *meta, size_t tn)
{
    pack_row_t r[PACK_ROWS];
    int n = packs_rows_now(r);                 /* (as list_count has just looked: the device isn't asked again for each row) */
    if (i < 0 || i >= n) return;
    switch (r[i].kind) {
    case PR_NEEDED: {
        /* what the library needs: how far the pack being fetched is, or how much there is to fetch */
        int at[UI_MAX_PACKS], k = packs_needed(at), getting = packs_getting();
        uint64_t bytes = 0;
        for (int j = 0; j < k; j++)
            if (S.packs[at[j]].bytes > 0) bytes += (uint64_t)S.packs[at[j]].bytes;
        snprintf(main, mn, "%s", tr("Needed by your library"));
        if (getting >= 0) snprintf(meta, tn, "%d %%", S.packs[getting].percent);
        else fmt_mb(meta, tn, bytes);
        break;
    }
    case PR_SHARED: {
        const ui_pack_t *f = &S.packs[packs_first()];
        snprintf(main, mn, "%s", tr("Shared characters"));
        snprintf(meta, tn, "%s", tr(f->brief[0] ? f->brief : f->name));
        break;
    }
    case PR_HIDE: snprintf(main, mn, "%s", tr("Hide from Home")); break;
    default: {
        const ui_pack_t *p = &S.packs[r[i].pack];
        snprintf(main, mn, "%s", tr(p->name));
        switch (p->state) {
        case UI_PACK_GETTING: snprintf(meta, tn, "%d %%", p->percent); break;
        case UI_PACK_THERE: snprintf(meta, tn, "%s", tr("Installed")); break;
        case UI_PACK_FAILED: snprintf(meta, tn, "%s", tr("Try again")); break;
        default: fmt_mb(meta, tn, (uint64_t)(p->bytes > 0 ? p->bytes : 0)); break;
        }
        break;
    }
    }
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
        if (u->state == UI_UNI_FAILED) { failure_text(main, mn, u->need_mb, u->message); break; }
        snprintf(main, mn, "%s", u->state == UI_UNI_CHECKING ? "Checking the update file"
                                 : u->state == UI_UNI_APPLYING ? "Getting the update ready"
                                 : "Ready to uninstall");
        if (u->total && u->state != UI_UNI_PENDING)
            snprintf(meta, tn, "%d %%", (int)(u->done * 100 / u->total));
        break;
    }
}

static void rows_box_place(void);

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
    tr_fill(main, mn, "Import %s", b ? b + 1 : S.pod_opml, NULL);
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
                                    : s->refreshed ? "No episodes" : "<Menu>: Check for new episodes"));
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
                                    : S.pod_search == -2 ? netclock_why("Couldn't reach the directory") : "No podcasts found"));
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
    if (name) tr_fill(name, cap, TONES[t].fmt, tr(ACCENTS[h].name), NULL);
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
        if (covers_in(l->kind) && is_album_list(l->kind) &&
            !(album_artists_on() && library_row_shows_artist(view_of(l->kind))))
            meta[0] = '\0';   /* (an album's cover takes the year's place; a song keeps its length) */
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
        if (S.home[i] == H_PACKS) { snprintf(main, mn, "%s", tr("Get language packs")); break; }
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
            else tr_fill(meta, tn, "%s, open", tr(sig), NULL);
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
    case SETTING_PAGES:
        setting_row(setting_at(l, i), S.editing && i == l->sel, main, mn, meta, tn);
        break;
    case L_SIGNAL: if (S.hooks.signal_path) S.hooks.signal_path(i, main, mn, meta, tn); break;
    case L_UNREAD: unread_row(i, main, mn); break;
    case L_ABOUT:
        switch (about_row(i)) {
        case AB_RESET: snprintf(main, mn, "Reset all settings"); break;
        case AB_WHATSNEW:
            snprintf(main, mn, "%s", tr("What's new"));
            snprintf(meta, tn, "%s", S.wn_ver);
            break;
        case AB_TIPS: snprintf(main, mn, "%s", tr("Tips")); break;
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
    case L_PACKS: packs_row_text(i, main, mn, meta, tn); break;
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
#define NP_ARTIST_Y 74     /* ...and the artist's, in the subtitle size (see apply_theme) */
#define NP_NEXT_Y 148
/* Where a font's capitals start below the top of its line, and how tall they are. */
int ui_test_row(const char *text)
{
    const level_t *l = &S.stack[S.depth - 1];
    for (int i = 0, n = list_count(l); i < n; i++) {
        char main[256] = "", meta[128] = "";
        int playing = 0;
        list_item(l, i, main, sizeof main, meta, sizeof meta, &playing);
        if (!strcmp(main, text)) return i;
    }
    return -1;
}

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

static void covers_reset(int forget);
static void covers_shape(void);

static void apply_theme(void)
{
    const theme_t *t = T();
    covers_reset(0);                       /* (the no-cover square takes the theme's colour) */
    covers_shape();                        /* (and the pictures in lists its corners) */
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
    fill(S.posbar, t->text_muted);
    lv_obj_set_style_bg_opa(S.posbar, LV_OPA_60, 0);
    lv_obj_set_style_radius(S.posbar, t->corner == THEME_CORNER_ROUND ? 2 : 0, 0);
    S.text_x0 = t->cursor_arrow ? 24 : 10;

    for (int r = 0; r < SLOTS; r++) {
        lv_obj_set_style_radius(S.row_dot[r], LV_RADIUS_CIRCLE, 0);
        fill(S.row_dot[r], t->signal);
        style_label(S.row_lbl[r], S.f->body, t->text);
        style_label(S.row_meta[r], VALUE_FONT, t->text_muted);
        style_label(S.row_lt[r], S.f->body, t->text);
        style_label(S.row_la[r], VALUE_FONT, t->text_muted);
        /* one line each (cut with "...", never wrapped), on one baseline */
        int lt = lv_font_get_line_height(S.f->body), la = lv_font_get_line_height(VALUE_FONT);
        int yt = (ROW_H - lt) / 2, base = yt + lt - S.f->body->base_line;
        lv_obj_set_height(S.row_lt[r], lt);
        lv_obj_set_height(S.row_la[r], la);
        lv_obj_set_y(S.row_lt[r], yt);
        lv_obj_set_y(S.row_la[r], base - (la - VALUE_FONT->base_line));
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
    /* The artist in the body size (Atkinson: 20 px, where the other second
     * lines are 18), raised by what that adds to the line so its foot stays
     * where it was, clear of the album under it. (Ark Pixel's two sizes are
     * the same: nothing moves.) */
    style_label(S.np_artist, S.f->body, t->artist);
    lv_obj_set_y(S.np_artist, NP_ARTIST_Y - (lv_font_get_line_height(S.f->body) - lv_font_get_line_height(S.f->sub)));
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

    /* Albums as cover flow */
    fill(S.flow, t->bg);
    style_label(S.flow_title, S.f->body, t->text);
    style_label(S.flow_by, VALUE_FONT, t->text_muted);
    {
        /* Under the covers: the album's name, who it is by, and the letters
         * (or the line for where in the list). Three lines of a tall font
         * (Pixel Pastel's) are more than the room there is: the first goes up
         * a little, the last may lose the foot of its line (capitals have
         * none), and the lines close up by the rest. */
        int lb = lv_font_get_line_height(S.f->body), lv = lv_font_get_line_height(VALUE_FONT), lm = lv_font_get_line_height(S.f->meta);
        int room = UI_H - HEADER_H - (FLOW_H - 2), over = lb + 1 + lv + lm > room ? lb + 1 + lv + lm - room : 0;
        int up = over > 3 ? 3 : over, spill = over - up > 3 ? 3 : over - up, close = (over - up - spill + 1) / 2;
        int ty = FLOW_H - 2 - up, by = ty + lb + 1 - close, ay = by + lv - close;
        lv_obj_set_y(S.flow_title, ty);
        lv_obj_set_y(S.flow_by, by);
        /* every letter where they fit side by side; every other one in a wide font */
        S.flow_az_every = text_w("#ABCDEFGHIJKLMNOPQRSTUVWXYZ", S.f->meta) + 27 > UI_W - 16 ? 2 : 1;
        for (int i = 0; i < 27; i++) {
            style_label(S.flow_az[i], S.f->meta, t->text_muted);
            lv_obj_set_pos(S.flow_az[i], 8 + i * (UI_W - 16) / 27 - 6, ay);
        }
        S.flow_az_on = -1;
        /* The letter that's on, in a round theme: in a pill of the selection's
         * colours (the signal's colour alone, on a letter 15 pixels tall,
         * was hard to pick out: the owner, on the H2). A pixel theme's
         * letters are big and bright enough lit. */
        fill(S.flow_az_pill, t->select_bg);
        lv_obj_set_size(S.flow_az_pill, (UI_W - 16) / 27 + 4, lm - 1);    /* (no wider: the letters at each side stay whole) */
        lv_obj_set_y(S.flow_az_pill, ay);
        lv_obj_set_style_radius(S.flow_az_pill, (lm - 1) / 2, 0);
    }
    fill(S.flow_track, t->surface);
    fill(S.flow_thumb, t->signal);
    lv_obj_set_style_radius(S.flow_track, t->corner == THEME_CORNER_ROUND ? 2 : 0, 0);
    lv_obj_set_style_radius(S.flow_thumb, t->corner == THEME_CORNER_ROUND ? 2 : 0, 0);

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
        image_dsc(&dsc, LV_COLOR_FORMAT_A8, PX, PX, px, PX * PX);
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
        image_dsc(&dsc, LV_COLOR_FORMAT_A8, MARK_PX, MARK_PX, px, sizeof px);
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
        image_dsc(&dsc[k], LV_COLOR_FORMAT_A8, PX, PX, px[k], PX * PX);
    }
    return &dsc[k];
}

static void utf8_of(uint32_t c, char out[5])
{
    out[utf8_encode(c, out)] = 0;
}

/* Lists of the UI's own words (menus, settings), translated; not the music's
 * (albums, artists, songs...), radio stations, devices or networks. */
static int ui_text_list(list_kind_t k)
{
    switch (k) {
    case L_HOME: case L_HOME_EDIT: case L_HISTORY: case L_SETTINGS: case L_WIFI_ASK: case L_UPD_ASK:
    case L_EQ_EDIT: case L_EQ_BAND: case L_THEME_EDIT: case L_THEME_COLOURS: case L_PODCASTS: case L_SHARE: case L_SSH: case L_ABOUT: case L_CONTEXT: case L_USB_ASK: case L_UPDATE:
    case L_UNINSTALL: case L_PACKS:
    case L_RDIR: case L_RDIR_GENRES: case L_BATTERY: case L_BAT_SPAN:
        return 1;
    default:
        return 0;
    }
}

/* Settings > Interface > Covers in lists (display.covers: 1 Albums, the default; 2 Songs):
 * in every list of albums (an artist's, a genre's, Recently added...) each
 * album's cover before it, in place of the year; in every list of songs (an
 * album's, playlists, Favourites, History, the play queue...) each song's
 * album's, the length kept */
static int covers_in(list_kind_t k)
{
    int v = settings_get_int("display.covers", UI_COVERS_DEFAULT);
    return (is_album_list(k) && (v & 1)) || ((is_track_list(k) || k == L_QUEUE) && (v & 2));
}

/* Podcasts and Radio: each show's or station's picture before its name, the
 * size of an album's cover in a list (one without: the list's icon in its
 * place). Not with Covers in lists off. */
static int thumbs_in(list_kind_t k)
{
    return (k == L_PODCASTS || k == L_RADIO) && settings_get_int("display.covers", UI_COVERS_DEFAULT) != 0;
}

/* Albums' covers: the last COV_CACHE read, so scrolling a row reads one, not a
 * screenful (2.3 KB each from the card's art cache, no decoding), and each
 * visible row's pixels. Taken when Albums is drawn, given back when it goes
 * or the screen turns off: about 45 KB, and nothing elsewhere. The same cache
 * holds the pictures of Podcasts' shows and Radio's stations, under keys of
 * their own: COV_POD | a show's id (12 hex digits) as a number, for its
 * ID/thumb.565; COV_RADIO | a station's icon's id, the same way (radio_read). */
#define COV_POD (1LL << 60)
#define COV_CACHE 12
static struct covers {
    struct cov { int64_t id; uint32_t used; int ok; uint16_t px[LIB_ART_SMALL * LIB_ART_SMALL]; } cache[COV_CACHE];
    /* a row's picture as LVGL draws it: its pixels, then how much of each
     * shows (RGB565A8's alpha plane: the corners rounded off, covers_shape) */
    struct cov_slot { uint16_t px[LIB_ART_SMALL * LIB_ART_SMALL]; uint8_t show[LIB_ART_SMALL * LIB_ART_SMALL]; } slot[SLOTS];
    uint32_t tick;
} *g_cov;

/* The pictures in lists have the theme's corners: rounded (COVER_RADIUS, with
 * the pill, the chip and Now Playing's cover) or square. Rounded, each is
 * drawn through a mask of its own (no layer, nothing to clip: the corner
 * pixels are simply let through less); square, as plain pixels. */
#define COVER_RADIUS 5
static void covers_shape(void)
{
    if (!g_cov) return;
    int round = T()->corner == THEME_CORNER_ROUND, n = LIB_ART_SMALL, r = COVER_RADIUS;
    uint8_t mask[LIB_ART_SMALL * LIB_ART_SMALL];
    memset(mask, 255, sizeof mask);
    for (int y = 0; round && y < r; y++)
        for (int x = 0; x < r; x++) {
            int in = 0;                            /* of 16 points in the pixel, those inside the corner's circle */
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++) {
                    int dx = 8 * r - (8 * x + 2 * sx + 1), dy = 8 * r - (8 * y + 2 * sy + 1);   /* (in eighths of a pixel) */
                    in += dx * dx + dy * dy <= 64 * r * r;
                }
            uint8_t a = (uint8_t)(in * 255 / 16);
            mask[y * n + x] = mask[y * n + (n - 1 - x)] = mask[(n - 1 - y) * n + x] = mask[(n - 1 - y) * n + (n - 1 - x)] = a;
        }
    for (int j = 0; j < SLOTS; j++) {
        memcpy(g_cov->slot[j].show, mask, sizeof mask);
        S.cov_dsc[j].header.cf = round ? LV_COLOR_FORMAT_RGB565A8 : LV_COLOR_FORMAT_RGB565;
        S.cov_dsc[j].data = (const uint8_t *)&g_cov->slot[j];
        S.cov_dsc[j].data_size = round ? sizeof g_cov->slot[j] : sizeof g_cov->slot[j].px;
        lv_image_cache_drop(&S.cov_dsc[j]);
        lv_obj_invalidate(S.row_cov[j]);
    }
}

static int covers_take(void)
{
    if (g_cov) return 1;
    if (!(g_cov = calloc(1, sizeof *g_cov))) return 0;
    for (int j = 0; j < SLOTS; j++) S.cov_id[j] = -1;
    covers_shape();
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

/* A show's or a station's picture for the list (2.3 KB read from the card): 1 if it has one */
static int thumb_read(int64_t key, uint16_t *px)
{
    char id[13], p[800];
    snprintf(id, sizeof id, "%012llx", (unsigned long long)(key & COV_KEY_BITS));
    if (key & COV_RADIO) return ricon_read(id, LIB_ART_SMALL, px);
    if (!(key & COV_POD) || !podsync_dir()[0]) return 0;
    pod_thumb_path(podsync_dir(), id, p, sizeof p);
    FILE *f = fopen(p, "rbe");
    size_t want = sizeof(uint16_t) * LIB_ART_SMALL * LIB_ART_SMALL;
    int ok = f && fread(px, 1, want, f) == want;
    if (f) fclose(f);
    return ok;
}

/* The picture a row of Podcasts or Radio shows: its key, or -1 for a row that
 * isn't a show or a station, or a station with no icon */
static int64_t thumb_key(const level_t *l, int i)
{
    if (l->kind == L_RADIO) return i >= 1 && i - 1 < S.radio_n ? S.radio_key[i - 1] : -1;
    if (l->kind == L_PODCASTS && i >= 1 && i - 1 < S.pod_n)
        return COV_POD | (int64_t)(strtoull(S.pod_shows[i - 1].id, NULL, 16) & COV_KEY_BITS);
    return -1;
}

/* A theme's colour as one pixel of a picture (theme_rgb565 isn't that: it is
 * still 24 bits, rounded to what the screen can show) */
static uint16_t rgb565_of(uint32_t rgb)
{
    return (uint16_t)(((rgb >> 19) & 31) << 11 | ((rgb >> 10) & 63) << 5 | ((rgb >> 3) & 31));
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
    e->ok = id >= COV_POD ? thumb_read(id, e->px) : S.lib && library_art(S.lib, id, LIB_ART_SMALL, e->px) == 0;
    return e->ok ? e->px : NULL;
}

/* Shows' or stations' pictures changed on the card (tag: COV_POD, COV_RADIO): read again, the albums' left as they are */
static void thumbs_reset(int64_t tag)
{
    for (int j = 0; j < SLOTS; j++)
        if (S.cov_id[j] >= 0 && (S.cov_id[j] & tag)) S.cov_id[j] = -1;
    for (int k = 0; g_cov && k < COV_CACHE; k++)
        if (g_cov->cache[k].id >= 0 && (g_cov->cache[k].id & tag)) g_cov->cache[k].used = 0;
}

static void flow_forget(int all);

/* (the theme changed, or the library: each row's cover drawn again; a new
 * library's covers read again, and the cover flow's where it had none) */
static void covers_reset(int forget)
{
    for (int j = 0; j < SLOTS; j++) S.cov_id[j] = -1;
    if (forget && g_cov) for (int k = 0; k < COV_CACHE; k++) g_cov->cache[k].used = 0;
    if (forget) flow_forget(0);
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
    int min_a = lv_font_get_line_height(VALUE_FONT) * 2;   /* less room than this: no artist */
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

/* Settings: a row that opens a page or a list of its own (where Play on the
 * others changes them in place): it ends in an arrow. */
static int row_opens(const level_t *l, int i)
{
    switch (l->kind) {
    case L_SETTINGS: return 1;
    case SETTING_PAGES:
        switch (setting_at(l, i)) {
        case ST_THEME: case ST_HOME_SCREEN: case ST_EQ: case ST_BT: case ST_WIFI: case ST_N_UNREAD: case ST_PACKS: return 1;
        case ST_BATTERY: return S.hooks.battery_report != NULL;
        case ST_OUTPUT: return S.hooks.signal_path != NULL;
        default: return 0;
        }
    case L_ABOUT: {
        int r = about_row(i);
        return r == AB_WHATSNEW || r == AB_TIPS || r == AB_CHECK || r == AB_UNINSTALL ||
               (r == AB_INFO && !strcmp(S.about_lbl[i], "Clock") && S.hooks.clock_set != NULL);
    }
    /* Wi-Fi: a network that's new and has a password opens the screen to
     * type it (a saved or an open one is joined where it is). Nothing on
     * Bluetooth's, the network drive's or SSH's pages opens anything: their
     * rows act in place (Find headphones lists what it finds on its page). */
    case L_WIFI: {
        int net;
        if (!S.hooks.wifi_do || wifi_row(i, &net) != WR_NET || net >= S.wifi_n) return 0;
        return S.wifi[net].secured && !S.wifi[net].saved && !S.wifi[net].connected;
    }
    default: return 0;
    }
}

/* A page with such rows: the arrow has a column of its own there, kept free
 * on the other rows, so their values end where the arrowed rows' do ("Off"
 * over "Off >", not under its arrow). */
static int arrow_page(const level_t *l, int n)
{
    switch (l->kind) {
    case L_SETTINGS: case SETTING_PAGES: case L_ABOUT: case L_WIFI:
        for (int i = 0; i < n; i++)
            if (row_opens(l, i)) return 1;
        return 0;
    default: return 0;
    }
}

static void fill_slots(const level_t *l, int n)
{
    int arrow_w = 0;                               /* the arrow's column on this page (0: none) */
    if (arrow_page(l, n)) arrow_w = text_w(" \xE2\x80\xBA", VALUE_FONT);
    /* (the same for every row: asked of the settings once a draw, not once a row) */
    int page_thumbs = thumbs_in(l->kind), page_covers = covers_in(l->kind), artists_on = album_artists_on();
    for (int j = 0; j < SLOTS; j++) {
        int i = l->top + j - 1;
        int visible = i >= 0 && i < n;
        S.slot_playing[j] = 0;
        S.slot_accent[j] = visible && l->kind == L_HOME && (S.home[i] == H_UPDATE || S.home[i] == H_PACKS);
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
        int opens = row_opens(l, i);
        if (opens) {                               /* "Studio Mint >", or the arrow alone */
            size_t k = strlen(meta);
            snprintf(meta + k, sizeof meta - k, "%s\xE2\x80\xBA", k ? " " : "");
        }
        int meta_in = arrow_w && !opens ? arrow_w : 0;   /* a value on a page with arrows: clear of their column */

        int x = S.text_x0;
        int eq = playing && T()->playing_equalizer;

        /* Albums: the cover first, in a straight column; the playing marker after it */
        int thumbs = page_thumbs && covers_take();                  /* Podcasts, Radio: the show's or station's picture in that column */
        int cover = !thumbs && page_covers && S.lib && covers_take();   /* the album's cover (none: a plain square) */
        int64_t id = -1;
        if (thumbs) {
            id = thumb_key(l, i);
        } else if (cover) {
            int64_t tid = l->kind == L_QUEUE ? (S.player ? player_queue_at(S.player, i) : -1)   /* (the queue: the player's) */
                        : l->items.ids[i];
            id = is_album_list(l->kind) ? tid : tid >= 0 ? library_track_album(S.lib, tid) : -1;
        }
        int blank = thumbs;                        /* a row of theirs with no picture: an icon in its place, below */
        if (cover || (thumbs && id >= 0)) {
            if (S.cov_id[j] != id) {
                const uint16_t *px = cover_px(id);
                uint16_t *to = g_cov->slot[j].px;
                if (px) memcpy(to, px, sizeof g_cov->slot[j].px);
                else if (cover) {
                    uint16_t c = rgb565_of(T()->surface);
                    for (size_t k = 0; k < sizeof g_cov->slot[j].px / 2; k++) to[k] = c;
                }
                S.cov_id[j] = id;
                S.cov_has[j] = px != NULL;
                if (px || cover) {
                    lv_image_cache_drop(&S.cov_dsc[j]);
                    lv_image_set_src(S.row_cov[j], &S.cov_dsc[j]);
                    lv_obj_invalidate(S.row_cov[j]);
                }
            }
            if (thumbs) blank = !S.cov_has[j];
        }
        show(S.row_cov[j], cover || (thumbs && !blank));
        if (cover || (thumbs && !blank)) {
            if (lv_obj_get_style_x(S.row_cov[j], 0) != x) lv_obj_set_x(S.row_cov[j], x);
            x += LIB_ART_SMALL + ICON_GAP;
        }

        /* Home (and Settings > Interface > Home screen items): an icon before each item; on Now playing,
         * while music plays, the equalizer (or dot) takes its place, centred in
         * the icons' column */
        int hid = l->kind == L_HOME ? S.home[i] : l->kind == L_HOME_EDIT ? S.home_order[i] : -1;
        if (hid == H_PACKS) hid = H_UPDATE;        /* (the offer of packs: the update's icon) */
        uint32_t cp = hid < 0 ? 0 : hid == H_UPDATE ? UPDATE_ICON : HOME_ICON[hid];
        int pic = hid;
        if (l->kind == L_PLAYLISTS && i < l->items.n) {      /* Playlists: smart ones apart from yours */
            int smart = l->items.ids[i] < 0;
            cp = smart ? SMART_ICON : HOME_ICON[H_PLAYLISTS];
            pic = smart ? PIXEL_SMART : H_PLAYLISTS;
        }
        if (l->kind == L_SEARCH) {                 /* Search: Home's icon for what each result is, and its own on the typing row */
            int64_t rid;
            int kind = i > 0 ? search_kind(i - 1, &rid) : -1;
            pic = i == 0 ? H_SEARCH : kind == 0 ? H_ARTISTS : kind == 1 ? H_ALBUMS : H_SONGS;
            cp = HOME_ICON[pic];
        }
        if (blank) {                               /* Podcasts, Radio: a row without a picture: Search's icon to find some, else the list's own */
            pic = i == 0 ? H_SEARCH : l->kind == L_RADIO ? H_RADIO : H_PODCASTS;
            cp = HOME_ICON[pic];
        }
        int pix = pic >= 0 && T()->icons_pixel;
        int glyph = pic >= 0 && !pix && S.f->icon;
        int col = pix || glyph || blank;
        int colw = blank ? LIB_ART_SMALL : ICON_W; /* (the pictures' column: the icon in its middle) */
        for (int k = 0; k < 3; k++) {          /* three bars, 11 px across */
            show(S.row_eq[j][k], eq);
            lv_obj_set_x(S.row_eq[j][k], x + (col ? (colw - 11) / 2 : 0) + k * 4);
        }
        lv_obj_set_x(S.row_dot[j], x + (col ? (colw - 8) / 2 : 0));
        show(S.row_icon[j], glyph && !playing);
        show(S.row_picon[j], pix && !playing);
        if (glyph && !playing) {
            char u[5];
            utf8_of(cp, u);
            set_text(S.row_icon[j], u);
            int gx = x + (colw - ICON_W) / 2;
            if (lv_obj_get_style_x(S.row_icon[j], 0) != gx) lv_obj_set_x(S.row_icon[j], gx);
        }
        if (pix && !playing) {
            const lv_image_dsc_t *img = pixel_icon(pic);
            if (lv_image_get_src(S.row_picon[j]) != img) lv_image_set_src(S.row_picon[j], img);
            int px = x + (colw - PIXEL_ICON_GRID * PIXEL_ICON_SCALE) / 2;
            if (lv_obj_get_style_x(S.row_picon[j], 0) != px) lv_obj_set_x(S.row_picon[j], px);
        }
        if (col) x += colw + ICON_GAP;
        else if (eq) x += 18;
        else if (playing) x += 14;
        /* a list to pick one from (themes, EQs): the mark's room on every row,
         * so the one marked doesn't stand in from the others */
        else if (l->kind == L_THEMES || l->kind == L_EQ) x += T()->playing_equalizer ? 18 : 14;
        int swatch = (l->kind == L_THEMES && i < S.n_themes) || l->kind == L_THEME_COLOURS;
        show(S.row_sw[j], swatch);
        if (swatch && l->kind == L_THEME_COLOURS) {   /* a colour of the theme being made */
            uint32_t c = the_get(i);
            fill(S.row_sw[j], c);
            set_border_color(S.row_sw[j], T()->text_muted);
            set_radius(S.row_sw[j], T()->corner == THEME_CORNER_ROUND ? 4 : 0);
            for (int q = 0; q < 3; q++) fill(S.row_sw_part[j][q], c);
            if (lv_obj_get_style_x(S.row_sw[j], 0) != x) lv_obj_set_x(S.row_sw[j], x);
            x += SWATCH_W + ICON_GAP;
        } else if (swatch) {   /* the theme's colours, in miniature */
            const theme_t *th = &S.themes[i];
            fill(S.row_sw[j], th->bg);
            set_border_color(S.row_sw[j], T()->text_muted);
            set_radius(S.row_sw[j], th->corner == THEME_CORNER_ROUND ? 4 : 0);
            fill(S.row_sw_part[j][0], th->select_bg);
            set_radius(S.row_sw_part[j][0], th->corner == THEME_CORNER_ROUND ? 2 : 0);
            fill(S.row_sw_part[j][1], th->text);
            fill(S.row_sw_part[j][2], th->signal);
            if (lv_obj_get_style_x(S.row_sw[j], 0) != x) lv_obj_set_x(S.row_sw[j], x);
            x += SWATCH_W + ICON_GAP;
        }

        int typing = is_typing_row(l, i), live = typing && typing_on(&S.stack[S.depth - 1]);
        show(S.row_wheel[j], live);
        if (typing && !live) {                     /* left for the results: what was typed, as it is */
            snprintf(main, sizeof main, "%s", S.entry);
            meta[0] = '\0';
        }
        if (live) {
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
        /* room for the meta: only what it takes. (A column 70 px wide was
         * kept on every row, whatever the meta: a name was cut short beside a
         * count of one digit, a setting's name beside "Off".) */
        int meta_w = 0;
        lv_point_t sz = {0, 0};
        if (meta[0] && !typing) {
            sz.x = text_w(meta, VALUE_FONT);
            if (sz.x + 18 + meta_in > meta_w) meta_w = sz.x + 18 + meta_in;
        }
        if (lv_obj_get_style_translate_x(S.row_meta[j], 0) != -meta_in) lv_obj_set_style_translate_x(S.row_meta[j], -meta_in, 0);
        if (S.slot_mark[j] && !typing) {          /* on the card: the mark, just before the meta */
            int mx = UI_W - 2 * ROW_MARGIN - 10 - (meta[0] ? sz.x + MARK_GAP : 0) - MARK_PX;
            if (lv_image_get_src(S.row_mark[j]) != on_card_mark()) lv_image_set_src(S.row_mark[j], on_card_mark());
            if (lv_obj_get_style_x(S.row_mark[j], 0) != mx) lv_obj_set_x(S.row_mark[j], mx);
            show(S.row_mark[j], 1);
            if (meta[0]) { if (sz.x + 18 + MARK_PX + MARK_GAP > meta_w) meta_w = sz.x + 18 + MARK_PX + MARK_GAP; }
            else meta_w = MARK_PX + 18;
        }
        int line = !typing && library_backed(l->kind) && library_row_shows_artist(view_of(l->kind)) &&
                   artists_on;
        /* Search: an album found is drawn as in Albums; a song has its artist
         * after its title the same way, and keeps its length at the right */
        char by[160] = "";
        const char *after = meta;                  /* what follows the title on the line */
        int keep_meta = 0;
        if (l->kind == L_SEARCH && i > 0 && !typing) {
            int64_t rid;
            int kind = search_kind(i - 1, &rid);
            lib_track_t st;
            if (kind == 1) line = meta[0] && artists_on;
            else if (kind == 2 && S.lib && rid >= 0 && library_track(S.lib, rid, &st) == 0 && st.artist[0]) {
                snprintf(by, sizeof by, "%s", st.artist);
                after = by;
                keep_meta = meta[0] != '\0';
                line = 1;
            }
        }
        /* Home: the song (station, episode) that's on after "Now playing" */
        if (l->kind == L_HOME && S.home[i] == H_NOWPLAYING && S.now.active && S.now.track.title[0]) {
            snprintf(by, sizeof by, "%s", S.now.track.title);
            after = by;
            line = 1;
        }
        /* About: a value that leaves the row's name no room (a wide font:
         * Pixel Pastel showed "Ve..." beside a version, "..." beside the
         * clock). The name in full and the value after it on one line, cut
         * at its end; the selected row scrolls, as an album's does. */
        if (!line && !typing && l->kind == L_ABOUT && meta[0] && !S.slot_mark[j]) {
            if (text_w(main, S.f->body) > UI_W - 2 * ROW_MARGIN - x - meta_w) line = 1;
        }
        S.line_on[j] = line;
        show(S.row_line[j], line);
        show(S.row_lbl[j], !line);
        show(S.row_meta[j], !line || keep_meta);
        if (line) {   /* an album: its title and artist on one line */
            int lw = UI_W - 2 * ROW_MARGIN - x - (keep_meta ? meta_w : 10);
            int same = text_is(S.row_lt[j], main) && text_is(S.row_la[j], after) &&
                       lv_obj_get_style_x(S.row_line[j], 0) == x && line_width(j) == lw;
            if (keep_meta) set_text(S.row_meta[j], meta);
            if (!same || !lv_anim_get(S.row_line[j], line_scroll_cb)) {   /* (a scrolling one left to scroll) */
                S.line_tw[j] = text_w(main, S.f->body);
                S.line_aw[j] = after[0] ? text_w(after, VALUE_FONT) : 0;
                set_pos_width(S.row_line[j], x, 0, lw);
                set_text(S.row_lt[j], main);
                set_text(S.row_la[j], after);
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

static void queue_move_follow(level_t *l);

static void render_list(render_mode_t mode)
{
    level_t *l = &S.stack[S.depth - 1];
    if (l->kind == L_BLUETOOTH || l->kind == L_QUICK) bt_look();   /* (what these pages show: as it is now) */
    if (l->kind == L_WIFI || l->kind == L_QUICK) wifi_look();
    int n = list_count(l);
    /* Settings > Bluetooth, Audio changed: Bluetooth started again and its
     * rows are back, so the selection goes back to Audio (it was on the one
     * row left meanwhile). Here, where the page is drawn every second: the
     * H2 doesn't always say "changed" at the moment the rows return. */
    if (l->kind != L_BLUETOOTH) S.bt_back_to = S.bt_back_gone = 0;
    else if (S.bt_back_to && n <= S.bt_back_to) S.bt_back_gone = 1;     /* (its rows have gone: only then are they "back") */
    else if (S.bt_back_to && S.bt_back_gone) {
        l->sel = S.bt_back_to;
        S.bt_back_to = S.bt_back_gone = 0;
    }
    queue_move_follow(l);
    /* A list that got shorter by itself (the queue when the song ends, your
     * stations after one is removed from Now Playing, Library without its
     * "Files not read"): the selection comes back onto its last row. Left
     * past the end, <Play> and <Menu> did nothing. */
    if (l->sel >= n) l->sel = n ? n - 1 : 0;
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
    /* A list longer than the screen: a thin bar in the right margin, as long
     * as the part that shows and as far down as the selection is (so every
     * turn of the wheel moves it). Not on the pages with a graph over their
     * rows. Moved only when it changes: an idle list isn't redrawn. */
    int long_list = rows == ROWS && n > rows;
    show(S.posbar, long_list);
    if (long_list) {
        int track = UI_H - HEADER_H - 2 * POSBAR_PAD, h = track * rows / n;
        if (h < POSBAR_MIN) h = POSBAR_MIN;
        int y = POSBAR_PAD + (int)((int64_t)(track - h) * l->sel / (n - 1));
        if (y != S.posbar_y || h != S.posbar_h) {
            lv_obj_set_y(S.posbar, y);
            lv_obj_set_height(S.posbar, h);
            S.posbar_y = y;
            S.posbar_h = h;
        }
    }
    /* A typing screen with nothing under its row (nothing typed yet, nothing
     * found, a name or a password being typed): how the wheel types, where
     * the results would be */
    int typing_alone = n == 1 && is_typing_row(l, 0);
    show(S.empty, n == 0 || typing_alone);
    static int empty_dy = -10;                 /* (moved only when it changes: an idle list isn't redrawn) */
    int dy = typing_alone ? ROW_H / 2 : -10;
    if (dy != empty_dy) {
        lv_obj_align(S.empty, LV_ALIGN_CENTER, 0, dy);
        empty_dy = dy;
    }
    if (typing_alone) {
        char help[320];
        /* typed, and nothing has it: how to mend what's typed (the whole of
         * the help under "Nothing found" is taller than the screen in a pixel font) */
        if (l->kind == L_SEARCH && S.entry[0])
            snprintf(help, sizeof help, "%s\n\n%s", tr("Nothing found"), tr("<Back> deletes, <Prev> and <Next> move the cursor."));
        else
            snprintf(help, sizeof help, "%s",
                     tr("Scroll the wheel, <Play> once to type, twice fast to finish, <Back> deletes, <Prev> and <Next> move the cursor."));
        set_text(S.empty, help);
    }
    if (n == 0) {
        const char *msg = l->kind == L_QUEUE ? "Nothing queued after this track"
                        : l->kind == L_PLAYLISTS ? "No playlists in the music folder"
                        : l->kind == L_PLAYLIST_TRACKS ? "None of these songs are on the card"
                        : l->kind == L_SMART_TRACKS ? "Nothing here now"
                        : l->kind == L_FAVOURITES ? "No favourites yet: <Menu> on a song"
                        : l->kind == L_RECENT_PLAYED || l->kind == L_MOST_PLAYED ? "Nothing played yet"
                        : l->kind == L_BAT_HISTORY ? "No time on battery yet"
                        : l->kind == L_RDIR_RESULTS || l->kind == L_RDIR_COUNTRIES
                            ? (S.rd_job == RD_JOB_LIST ? "Searching\xE2\x80\xA6" : S.rd_state == RD_FAILED ? S.rd_msg
                               : l->kind == L_RDIR_COUNTRIES ? "No countries found" : "No stations found")
                        : !library_backed(l->kind) ? "Nothing here"
                        /* the whole library empty: what to do about it */
                        : g_music_missing ? "No Music folder on the card: make one at the top of the card and copy music into it"
                        : S.lib && library_empty(S.lib) ? "No music yet. Plug the player into a computer, choose Use as a USB drive and copy music into the Music folder"
                        : "No music found";
        set_text(S.empty, tr(msg));
    }

    /* A page that only wanted its contents drawn again, with the selection
     * somewhere else since it was last drawn (its list got shorter under the
     * selection, say): the highlight goes where the selection is. Left where
     * it was, <Play> acted on a row other than the one shown (Bluetooth
     * turned off from its Audio row, 0.17.0). */
    int moved = mode == R_CONTENT && !new_level && (target != S.pill_target || top_delta != 0);
    if (mode == R_CONTENT && !new_level && !moved) {
        color_slots(slot_under_pill());
    } else if (mode == R_SNAP || new_level || moved) {
        stop_list_anims();
        S.pill_y = S.pill_target = target;
        S.rows_off = 0;
        graphs_place(l->kind);
        if (!covers_in(l->kind) && !thumbs_in(l->kind)) covers_free();   /* (a list without covers: their memory back) */
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

/* The station's logo is no longer what art_px holds: read again when it's wanted */
static void logo_forget(void)
{
    S.radio_logo = 0;
    snprintf(S.logo_url, sizeof S.logo_url, "?");
}

/* On the radio with no cover for the song: the station's own logo, if it's on
 * the card at Now Playing's size (a station of yours whose icon was fetched).
 * Read once for a station, and again when the icons on the card change. 1 if
 * art_px holds it. */
static int radio_logo(void)
{
    const radio_station_t *st = S.player ? player_radio_station(S.player) : NULL;
    unsigned seq = ricon_seq();
    const char *url = st ? st->url : "";
    if (strcmp(url, S.logo_url) || seq != S.logo_seq) {
        char id[13] = "";
        if (st) ricon_station_id(st, id);          /* (its own logo, or its site's icon) */
        snprintf(S.logo_url, sizeof S.logo_url, "%s", url);
        S.logo_seq = seq;
        S.radio_logo = id[0] && ricon_read(id, LIB_ART_LARGE, S.art_px);
        if (S.radio_logo) {
            lv_image_cache_drop(&S.art_dsc);   /* same descriptor, new pixels */
            lv_obj_invalidate(S.np_img);
        }
    }
    return S.radio_logo;
}

/* Cover art for the current track, from the library's RGB565 cache. */
static void load_art(void)
{
    S.np_seq = S.player ? player_seq(S.player) : 0;
    if (S.now.radio) return;           /* the radio's cover comes by ui_set_radio_art */
    S.radio_art = 0;
    logo_forget();                     /* (art_px is about to hold something else) */
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
    int art = S.radio_art || radio_logo();   /* the song's cover, else the station's logo, else the radio's icon */
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
    /* The output's name is in the middle of that line: a long word on the
     * right ("Can't reach the station") ran into it, one drawn over the
     * other. The output gives way then. */
    const char *out = tr(S.output);
    set_text(S.np_out, UI_W / 2 + text_w(out, S.f->meta) / 2 + 6 > UI_W - 12 - text_w(state, S.f->meta) ? "" : out);
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
        set_text(S.h_left, tr(S.now.active && S.now.radio ? "Radio" : S.now.active && S.now.podcast ? "Podcast" : "Playing"));
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
        /* "Playing" (it said "Now playing": the status icons need the room) */
        snprintf(h, sizeof h, "%s \xC2\xB7 %d/%d", tr("Playing"), S.now.index + 1, S.now.count);
        /* too wide beside the status icons (a big queue, a wide font, five icons): the numbers alone */
        int room = lv_obj_get_style_width(S.h_left, 0);          /* (as the header's status last left it) */
        if (room <= 0 || room > UI_W / 2 + 30) room = UI_W / 2 + 30;
        if (text_w(h, S.f->head) > room) snprintf(h, sizeof h, "%d/%d", S.now.index + 1, S.now.count);
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
 * battery %. The Bluetooth icon is dimmed while Bluetooth is on with no
 * headphones connected. */
#define HDR_DIM_OPA LV_OPA_60   /* a dimmed icon: this much of the muted colour */
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
        /* the accent for the first, what's playing; the others (shuffle,
         * repeat, Bluetooth, Wi-Fi, charging) in the text's colour: all in
         * the accent, none stood out */
        uint32_t ink = dim ? T()->text_muted : i == 0 ? T()->mode : T()->text;
        lv_obj_set_style_image_recolor(S.h_slot_img[i], C(ink), 0);
        /* dimmed (Bluetooth or Wi-Fi on, with nothing connected): the muted
         * colour, and fainter still, so it's plainly not the connected one */
        lv_obj_set_style_image_opa(S.h_slot_img[i], !dim ? LV_OPA_COVER : tint ? HDR_DIM_OPA : LV_OPA_30, 0);
        lv_obj_set_style_text_color(S.h_slot_sym[i], C(ink), 0);
        lv_obj_set_style_text_opa(S.h_slot_sym[i], dim ? HDR_DIM_OPA : LV_OPA_COVER, 0);
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
    S.help_shown = 0;       /* (a help gone by itself: the next <Menu> shows one, it doesn't put this one away) */
}

const char *ui_test_toast(void)
{
    return S.toast && !lv_obj_has_flag(S.toast, LV_OBJ_FLAG_HIDDEN) ? lv_label_get_text(S.toast_lbl) : NULL;
}

/* ---------- screens over everything: what they share ----------
 * overlay_new: the whole screen in the theme's background, on the top layer
 * (its maker moves it behind the toasts and the volume bar once it is built).
 * overlay_close: it goes. overlay_column: what it says, a column centred in
 * the room over the hint at its foot. text_block: a text that wraps, centred
 * in w px. KEYS_PLAYERS (a switch's labels): the keys that stay the music's
 * and the screen's while one is up. */
static lv_obj_t *overlay_new(void)
{
    lv_obj_t *o = box(lv_layer_top(), 0, 0, UI_W, UI_H);
    fill(o, T()->bg);
    return o;
}

static void overlay_close(lv_obj_t **ov)
{
    if (*ov) lv_obj_delete(*ov);
    *ov = NULL;
}

static lv_obj_t *overlay_column(lv_obj_t *ov)
{
    lv_obj_t *col = box(ov, 0, 0, UI_W, UI_H - 36);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(col, 12, 0);
    return col;
}

static lv_obj_t *text_block(lv_obj_t *parent, const lv_font_t *font, uint32_t colour, int w)
{
    lv_obj_t *l = lv_label_create(parent);
    style_label(l, font, colour);
    lv_obj_set_width(l, w);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    return l;
}

#define KEYS_PLAYERS UI_KEY_PLAY: case UI_KEY_PREV: case UI_KEY_NEXT: case UI_KEY_SEEK_BACK: case UI_KEY_SEEK_FWD: \
                     case UI_KEY_BRIGHTER: case UI_KEY_DIMMER

/* A progress or level bar's look: the theme's corners, its track in `track`,
 * filled in the accent */
static void bar_style(lv_obj_t *bar, uint32_t track)
{
    const theme_t *t = T();
    int r = t->corner == THEME_CORNER_ROUND ? 6 : 0;
    lv_obj_set_style_radius(bar, r, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, r, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(bar, C(track), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, C(t->signal), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);   /* (no theme to set it) */
}

/* What pops up for a moment (a toast, the level bar, the list's letter): its
 * look, the theme's as it is now (its surface and corners, a 2 px edge in the
 * accent, its text in `font`), and the one-shot timer that hides it, started
 * or started again */
static void popup_style(lv_obj_t *pop, lv_obj_t *lbl, const lv_font_t *font)
{
    const theme_t *t = T();
    lv_obj_set_style_bg_color(pop, C(t->surface), 0);
    lv_obj_set_style_bg_opa(pop, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(pop, t->radius, 0);
    lv_obj_set_style_border_color(pop, C(t->signal), 0);
    lv_obj_set_style_border_width(pop, 2, 0);
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_style_text_color(lbl, C(t->text), 0);
}

static void popup_arm(lv_timer_t **timer, lv_timer_cb_t hide, uint32_t ms)
{
    if (*timer) lv_timer_reset(*timer);
    else {
        *timer = lv_timer_create(hide, ms, NULL);
        lv_timer_set_repeat_count(*timer, 1);
    }
}

lv_obj_t *ui_message_screen(const char *title, const char *body)
{
    const theme_t *t = T();
    lv_obj_t *o = overlay_new();
    lv_obj_t *a = lv_label_create(o);
    style_label(a, S.f->title, t->text);
    lv_label_set_text(a, tr(title));
    lv_obj_align(a, LV_ALIGN_TOP_MID, 0, 62);
    lv_obj_t *b = text_block(o, S.f->body, t->text_muted, UI_W - 40);
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
static void update_overlay_close(void) { overlay_close(&S.upd_ov); }

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
        S.upd_ov = overlay_new();
        lv_obj_t *col = overlay_column(S.upd_ov);
        S.upd_title = text_block(col, S.f->title, t->text, UI_W - 32);
        S.upd_bar = lv_bar_create(col);
        lv_obj_set_size(S.upd_bar, UI_W - 64, 12);
        bar_style(S.upd_bar, t->surface);
        lv_bar_set_range(S.upd_bar, 0, 1000);
        S.upd_detail = text_block(col, S.f->body, t->text, UI_W - 32);
        S.upd_hint = text_block(S.upd_ov, S.f->body, t->text_muted, UI_W - 32);   /* (the lists' size: what to press, read at a glance) */
        lv_obj_align(S.upd_hint, LV_ALIGN_BOTTOM_MID, 0, -12);
        lv_obj_move_background(S.upd_ov);            /* toasts and the volume bar stay over it */
    }
    char title[96], detail[96], a[24], b[24];
    const char *hint = "<Back> cancels";
    int permille = u->total ? (int)(u->done * 1000 / u->total) : 0;
    detail[0] = '\0';
    fmt_mb(a, sizeof a, u->done);
    fmt_mb(b, sizeof b, u->total);
    switch (u->state) {
    case UI_UPD_DOWNLOADING:
        tr_fill(title, sizeof title, "Downloading %s", u->latest, NULL);
        if (u->total) tr_fill(detail, sizeof detail, "%s of %s", a, b);
        break;
    case UI_UPD_VERIFYING:
        snprintf(title, sizeof title, "%s", tr("Checking the download"));
        break;
    case UI_UPD_UNPACKING:
        tr_fill(title, sizeof title, "Unpacking %s", u->latest, NULL);
        if (u->total) tr_fill(detail, sizeof detail, "%s of %s", a, b);
        break;
    case UI_UPD_READY:
        tr_fill(title, sizeof title, "Ready to install %s", u->latest, NULL);
        snprintf(detail, sizeof detail, "%s", tr("The player restarts to finish. Your settings and music stay."));
        hint = "<Play> installs, <Back> cancels";
        permille = 1000;
        break;
    case UI_UPD_INSTALLING:
        tr_fill(title, sizeof title, "Installing %s", u->latest, NULL);
        snprintf(detail, sizeof detail, "%s", tr("Don't turn the player off"));
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
    /* on one line: at the lists' size where it fits, else the smaller one (a
     * wide font: Pixel Pastel); the column centred in the room above it */
    lv_obj_set_style_text_font(S.upd_hint, S.f->body, 0);
    lv_obj_update_layout(S.upd_hint);
    if (lv_obj_get_height(S.upd_hint) > lv_font_get_line_height(S.f->body) * 3 / 2)
        lv_obj_set_style_text_font(S.upd_hint, S.f->meta, 0);
    lv_obj_update_layout(S.upd_hint);
    lv_obj_set_height(lv_obj_get_parent(S.upd_title), UI_H - 12 - lv_obj_get_height(S.upd_hint) - 4);
    lv_bar_set_value(S.upd_bar, permille, LV_ANIM_OFF);
}

/* ---------- Rescan's progress ----------
 * Settings > Library > Rescan the music folder: the scan (the player's
 * background one) shown as the first scan is, over everything: each step
 * named, how far, about how long it has left, the song it's reading. The
 * music plays on (Play, Prev, Next, the volume work); Back puts it away and
 * the scan carries on (the Rescan row says how far). It goes when the scan
 * ends ("Library updated", or that nothing changed). */

static void scan_overlay_close(void)
{
    if (S.scan_timer) lv_timer_delete(S.scan_timer);
    overlay_close(&S.scan_ov);
    S.scan_timer = NULL;
    S.scan_step = S.scan_count = S.scan_left = S.scan_file = NULL;
}

/* ---- a progress screen's parts (ui.h): this file's rescan overlay, and the
 * device's screens that come before the interface is up (the first scan, a
 * library brought up to date, a card that can't be used). They were written
 * out in both. ---- */

/* the column the rows stand in (a title that wraps pushes the rest down) */
lv_obj_t *ui_scan_column(lv_obj_t *parent)
{
    lv_obj_t *col = box(parent, 0, 0, UI_W, UI_H - 44);
    lv_obj_set_style_pad_hor(col, 16, 0);
    lv_obj_set_style_pad_top(col, 16, 0);
    lv_obj_set_style_pad_row(col, 6, 0);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    return col;
}

/* a row of the column: one line (cut with "...", its place kept when empty),
 * or wrapped (the title) */
lv_obj_t *ui_scan_label(lv_obj_t *col, const lv_font_t *f, uint32_t colour, int wrap)
{
    lv_obj_t *l = lv_label_create(col);
    style_label(l, f, colour);
    lv_obj_set_width(l, UI_W - 32);
    lv_label_set_long_mode(l, wrap ? LV_LABEL_LONG_WRAP : LV_LABEL_LONG_DOT);
    if (!wrap) lv_obj_set_height(l, lv_font_get_line_height(f));   /* (one line, kept when empty) */
    lv_label_set_text(l, "");
    return l;
}

/* the note at the foot */
lv_obj_t *ui_scan_note(lv_obj_t *parent, const lv_font_t *f, uint32_t colour)
{
    lv_obj_t *l = lv_label_create(parent);
    style_label(l, f, colour);
    lv_obj_set_width(l, UI_W - 32);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_align(l, LV_ALIGN_BOTTOM_LEFT, 16, -14);
    return l;
}

/* The column down to the note (rows that don't fit are cut, not drawn over
 * it: a big font's title and note on two lines each). Once the note's text
 * is set. */
void ui_scan_fit(lv_obj_t *col, lv_obj_t *note)
{
    lv_obj_update_layout(note);
    lv_obj_set_height(col, UI_H - 14 - lv_obj_get_height(note) - 6);
    lv_obj_update_layout(col);
    for (uint32_t i = 0; i < lv_obj_get_child_count(col); i++) {   /* (whole rows only) */
        lv_obj_t *r = lv_obj_get_child(col, (int32_t)i);
        if (lv_obj_get_y(r) + lv_obj_get_height(r) > lv_obj_get_height(col)) lv_obj_add_flag(r, LV_OBJ_FLAG_HIDDEN);
    }
}

/* "Less than a minute left"...: the background scan's screen here, and the
 * device's own first-scan screen (ui.h) */
void ui_left_words(uint32_t left_s, char *t, size_t n)
{
    char m[16];
    if (left_s < 45) snprintf(t, n, "%s", tr("Less than a minute left"));
    else if (left_s < 90) snprintf(t, n, "%s", tr("About a minute left"));
    else {
        snprintf(m, sizeof m, "%u", (left_s + 30) / 60);
        tr_fill(t, n, "About %s minutes left", m, NULL);
    }
}

static void scan_overlay_update(void)
{
    ui_scan_t p;
    if (!S.scan_ov) return;
    if (!S.hooks.scan_progress || !S.hooks.scan_progress(&p)) { scan_overlay_close(); return; }
    uint32_t now = lv_tick_get();
    if (p.step != S.scan_was) {                      /* the next step: its own clock */
        S.scan_was = p.step;
        S.scan_t0 = now;
        S.scan_left_at = 0;
        static const char *const SCAN_STEP[] = {"Looking for music", "Reading songs", "Sorting albums and artists", "Finding covers"};
        lv_label_set_text(S.scan_step, tr(SCAN_STEP[p.step >= 0 && p.step <= UI_SCAN_COVERS ? p.step : 0]));
        lv_label_set_text(S.scan_left, "");
    }
    char a[16], b[16], t[64] = "";
    snprintf(a, sizeof a, "%d", p.done);
    snprintf(b, sizeof b, "%d", p.total);
    int counted = (p.step == UI_SCAN_SONGS || p.step == UI_SCAN_COVERS) && p.total > 0 && p.done <= p.total;
    if (counted) tr_fill(t, sizeof t, p.step == UI_SCAN_COVERS ? "%s of %s albums" : "%s of %s songs", a, b);
    /* albums and artists: the songs it goes through, then the database's own work (a word, its dots moving) */
    else if (p.step == UI_SCAN_ALBUMS && p.total > 0 && p.done <= p.total) tr_fill(t, sizeof t, "%s of %s songs", a, b);
    else if (p.step == UI_SCAN_ALBUMS && p.total < 0) snprintf(t, sizeof t, "%s%s", tr("Working"), &"..."[2 - p.done % 3]);
    /* the folder being listed: how many so far (then, for files it looks up one by one, how many of those) */
    else if (p.step == UI_SCAN_LOOKING && p.done > 0) tr_fill(t, sizeof t, p.total < 0 ? "%s songs checked" : "%s songs found", a, NULL);
    lv_label_set_text(S.scan_count, t);
    /* about how long this step has left, once it can tell (10 s in, 2 % done) */
    uint32_t el = now - S.scan_t0;
    if (!counted || p.done <= 0 || el < 10000 || p.done * 50 < p.total) lv_label_set_text(S.scan_left, "");
    else if (!S.scan_left_at || now - S.scan_left_at >= 2000) {   /* (steadier: every 2 s) */
        S.scan_left_at = now;
        ui_left_words((uint32_t)((uint64_t)el * (uint32_t)(p.total - p.done) / (uint32_t)p.done / 1000), t, sizeof t);
        lv_label_set_text(S.scan_left, t);
    }
    lv_label_set_text(S.scan_file, p.step == UI_SCAN_SONGS ? p.file : "");
}

static void scan_overlay_tick(lv_timer_t *t)
{
    (void)t;
    scan_overlay_update();
}

void ui_show_scan(void)
{
    ui_scan_t p;
    if (!S.hooks.scan_progress || !S.hooks.scan_progress(&p)) { scan_overlay_close(); return; }
    if (S.scan_ov) { scan_overlay_update(); return; }
    const theme_t *t = T();
    S.scan_ov = overlay_new();
    /* as the first scan's screen */
    lv_obj_t *col = ui_scan_column(S.scan_ov);
    lv_label_set_text(ui_scan_label(col, S.f->title, t->text, 1), tr("Reading your library"));
    S.scan_step = ui_scan_label(col, S.f->body, t->text, 0);
    S.scan_count = ui_scan_label(col, S.f->body, t->signal, 0);
    S.scan_left = ui_scan_label(col, S.f->meta, t->text_muted, 0);
    S.scan_file = ui_scan_label(col, S.f->meta, t->text_muted, 0);
    lv_obj_t *hint = ui_scan_note(S.scan_ov, S.f->meta, t->text_muted);
    lv_label_set_text(hint, tr("<Back> hides this"));   /* (and says the scan carries on) */
    ui_scan_fit(col, hint);
    lv_obj_move_background(S.scan_ov);             /* toasts and the volume bar stay over it */
    S.scan_was = -1;
    S.scan_timer = lv_timer_create(scan_overlay_tick, 250, NULL);
    scan_overlay_update();
}

static int scan_overlay_key(ui_key_t k)
{
    switch (k) {
    case KEYS_PLAYERS:
        return 0;                                   /* the music's and the screen's, as anywhere */
    case UI_KEY_BACK:
        scan_overlay_close();
        ui_toast("The rescan carries on", 1500);
        return 1;
    default:
        return 1;
    }
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
    overlay_close(&S.wn_ov);
    S.wn_body = S.wn_hint = NULL;
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
        lv_label_set_text(S.wn_hint, tr(S.wn_last < S.wn_n - 1 ? "Turn for more, <Play>: done" : "<Play> or <Back>: done"));
}

/* The welcome, once after a fresh install's first scan (ui_show_welcome) and
 * from Settings > About > Tips: the keys nobody would guess, one per point */
static const char WELCOME[] =
    "Hold <Back>: Now Playing, and back\n"
    "Hold <Menu>: quick settings, like brightness and EQ\n"
    "<Menu> on a song, album or artist: what you can do with it\n"
    "<Menu> on a setting: what it does\n"
    "Screen off: <Play>, <Prev>, <Next> and volume still work";

static void points_open(int welcome);
static void whats_new_open(void) { points_open(0); }

static void points_open(int welcome)
{
    const char *text = welcome ? WELCOME : S.wn_text;
    if (!text[0]) return;
    whats_new_close();
    const theme_t *t = T();
    S.wn_ov = overlay_new();
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
    if (welcome) snprintf(tt, sizeof tt, "%s", tr("Welcome to Fathom"));
    else tr_fill(tt, sizeof tt, "What's new in %s", S.wn_ver, NULL);
    lv_label_set_text(title, tt);
    S.wn_body = box(col, 0, 0, LV_PCT(100), 10);
    lv_obj_set_flex_grow(S.wn_body, 1);
    lv_obj_set_flex_flow(S.wn_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(S.wn_body, 8, 0);
    S.wn_n = S.wn_first = 0;
    const lv_font_t *pf = S.f->body;              /* (the lists' 20 px: easy to read; the wheel scrolls the rest) */
    int lh = lv_font_get_line_height(pf), dot = 7;
    for (const char *p = text; *p;) {
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
        /* a line that starts with spaces: a point under the one before it
         * ("Improved:", then what was), set in under its text, its dot quieter */
        int sub = line[0] == ' ';
        const char *words = line + strspn(line, " ");
        if (sub) lv_obj_set_style_pad_left(row, dot + 10, 0);
        lv_obj_t *d = box(row, 0, 0, dot, dot);
        fill(d, sub ? t->text_muted : t->signal);
        lv_obj_set_style_radius(d, t->corner == THEME_CORNER_ROUND ? LV_RADIUS_CIRCLE : 0, 0);
        lv_obj_set_style_margin_top(d, (lh - dot) / 2, 0);
        lv_obj_t *l = lv_label_create(row);
        style_label(l, pf, t->text);
        lv_obj_set_flex_grow(l, 1);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_label_set_text(l, tr(words));
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
    case UI_KEY_MENU_LONG:
        return 1;                                   /* (quick settings would open under it, unseen) */
    default:
        return 0;                                   /* the music's and the screen's, as anywhere */
    }
}

void ui_show_welcome(void) { points_open(1); }

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
    case KEYS_PLAYERS:
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
    else if (st == UI_UPD_FAILED && sel == 0) failure_toast(S.upd.need_mb, S.upd.message);   /* (whole, if the row cuts it) */
    else if (st == UI_UPD_IDLE || st == UI_UPD_LATEST || st == UI_UPD_FAILED) {
        /* (an update already on the card: Try again fetches nothing, so Wi-Fi isn't asked for) */
        if ((st == UI_UPD_FAILED && S.upd.no_net) || !wifi_ask_if_off(WT_OK)) S.hooks.update_do(UI_UPD_CHECK);
    }
    ui_update_changed();
}

static void pop(void);

/* Uninstall Fathom, over everything:
 *   made ready: what happens next, until the H2 restarts into HiBy's recovery
 *   (OK) or it's cancelled (Back);
 *   no update file on the card: where to get one (OK looks again, Back goes back). */
static int uni_ov_pending;      /* which of the two is up */

static void uninstall_overlay_close(void) { overlay_close(&S.uni_ov); }

static void uninstall_overlay(void)
{
    level_t *l = &S.stack[S.depth - 1];
    int pending = S.uni.state == UI_UNI_PENDING, none = S.uni.state == UI_UNI_FAILED && S.uni.no_file;
    if ((!pending && !none) || l->kind != L_UNINSTALL) { uninstall_overlay_close(); return; }
    if (S.uni_ov && uni_ov_pending == pending) return;
    uninstall_overlay_close();
    uni_ov_pending = pending;
    const theme_t *t = T();
    S.uni_ov = overlay_new();
    lv_obj_t *col = overlay_column(S.uni_ov);
    lv_obj_t *title = text_block(col, S.f->title, t->text, UI_W - 32);
    lv_label_set_text(title, tr(pending ? "Ready to uninstall" : "No update file"));
    lv_obj_t *body = text_block(col, S.f->body, t->text, UI_W - 32);
    /* (each text on one line: tools/lang/strings.py reads them so) */
    static const char *const ready = "The player restarts into HiBy's recovery, which puts HiBy's firmware back. Don't turn it off meanwhile.";
    static const char *const where = "Get the firmware (.upt) from the maker's website, your version or newer, and put it at the top of the card or in Music.";
    lv_label_set_text(body, tr(pending ? ready : where));
    lv_obj_t *hint = text_block(S.uni_ov, S.f->meta, t->text_muted, UI_W - 32);
    lv_label_set_text(hint, tr(pending ? "<Play> restarts, <Back> cancels" : "<Play> looks again"));
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
    case KEYS_PLAYERS:
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

/* "Press again to ...": what can't be undone takes two presses within 3 s.
 * pressed_again: this press at `now` is the second (1, and the first is
 * forgotten). press_arm: it is the first, remembered at `at`, and what a
 * second will do is said. press_twice: the two together, 1 on the second. */
static int pressed_again(uint32_t *armed, uint32_t now)
{
    if (!(*armed && now - *armed < 3000)) return 0;
    *armed = 0;
    return 1;
}

static void press_arm(uint32_t *armed, uint32_t at, const char *again)
{
    *armed = at;
    ui_toast(again, 3000);
}

static int press_twice(uint32_t *armed, const char *again)
{
    uint32_t now = lv_tick_get();
    if (pressed_again(armed, now)) return 1;
    press_arm(armed, now, again);
    return 0;
}

static void uninstall_ok(int sel)
{
    if (!S.hooks.uninstall_do) return;
    uninstall_look();
    switch (uninstall_row(sel)) {
    case UR_LOOK: S.hooks.uninstall_do(UI_UNI_CHECK); break;
    case UR_GO:                                     /* press twice */
        if (press_twice(&S.uni_armed, "Press again to uninstall Fathom")) S.hooks.uninstall_do(UI_UNI_APPLY);
        break;
    case UR_STATUS:
        if (S.uni.state == UI_UNI_FAILED) failure_toast(S.uni.need_mb, S.uni.message);   /* (whole, if the row cuts it) */
        break;
    default: break;
    }
    ui_uninstall_changed();
}

/* "Needed by your library": the next pack it needs that isn't there is asked
 * for (one is fetched at a time); none left, or one failed: that's all. */
static void packs_all_next(void)
{
    int at[UI_MAX_PACKS], n = packs_needed(at);
    for (int j = 0; j < n && S.packs_all; j++) {
        if (S.packs[at[j]].state == UI_PACK_GETTING) return;            /* (one under way) */
        if (S.packs[at[j]].state == UI_PACK_FAILED) S.packs_all = 0;
    }
    for (int j = 0; j < n && S.packs_all; j++)
        if (S.packs[at[j]].state == UI_PACK_ABSENT) { S.hooks.packs_do(at[j], UI_PACK_GET); return; }
    S.packs_all = 0;
}

/* Language packs, OK on a row. A pack: download it; cancel the download; or,
 * twice within 3 s, remove it. */
static void packs_ok(int sel)
{
    if (!S.hooks.packs_do) return;
    pack_row_t r[PACK_ROWS];
    int n = packs_rows(r);
    if (sel < 0 || sel >= n) return;
    switch (r[sel].kind) {
    case PR_NEEDED: {                              /* everything the library needs; again: the one under way cancelled */
        int at[UI_MAX_PACKS], k = packs_needed(at), getting = packs_getting();
        if (getting >= 0) {
            S.packs_all = 0;
            S.hooks.packs_do(getting, UI_PACK_CANCEL);
            return;
        }
        for (int j = 0; j < k; j++)                /* (one that failed is tried again) */
            if (S.packs[at[j]].state == UI_PACK_FAILED) { S.packs_all = 1; S.hooks.packs_do(at[j], UI_PACK_GET); return; }
        S.packs_all = 1;
        packs_all_next();
        return;
    }
    case PR_SHARED: {                              /* the next pack that has them */
        int now = packs_first(), next = now;
        do next = (next + 1) % S.npacks;
        while (next != now && !(S.packs[next].han && S.packs[next].state == UI_PACK_THERE));
        if (next != now) S.hooks.packs_do(next, UI_PACK_FIRST);
        return;
    }
    case PR_HIDE: {                                /* the packs offered now aren't offered again; one the library comes to need later is */
        int at[UI_MAX_PACKS], k = packs_offered(at);
        char ids[UI_MAX_PACKS * 13] = "";
        snprintf(ids, sizeof ids, "%s", settings_get("packs.ignored", ""));
        for (int j = 0; j < k; j++) {
            const char *pack = S.packs[at[j]].id;
            if (!pack[0] || id_listed(ids, pack)) continue;
            size_t len = strlen(ids);
            snprintf(ids + len, sizeof ids - len, "%s%s", len ? "," : "", pack);
        }
        settings_set("packs.ignored", ids);
        settings_save();
        ui_packs_changed(0);                       /* (the row goes, and Home's with it) */
        ui_toast("Still in Settings > Interface > Language packs", 3000);
        return;
    }
    default: break;
    }
    int pack = r[sel].pack;
    switch (S.packs[pack].state) {
    case UI_PACK_GETTING:
        S.packs_all = 0;                           /* (cancelled by hand: the rest aren't fetched either) */
        S.hooks.packs_do(pack, UI_PACK_CANCEL);
        break;
    case UI_PACK_THERE: {
        uint32_t now = lv_tick_get();
        if (S.pack_armed_i == pack && pressed_again(&S.pack_armed, now)) {    /* (the same pack, twice) */
            S.hooks.packs_do(pack, UI_PACK_REMOVE);
            ui_toast("Removed", 1500);            /* (in place of "Press again...", still up: seen on the H2) */
        } else {
            S.pack_armed_i = pack;
            press_arm(&S.pack_armed, now ? now : 1, "Press again to remove it");   /* (armed at tick 0 too) */
        }
        break;
    }
    default: S.hooks.packs_do(pack, UI_PACK_GET); break;
    }
}

void ui_packs_changed(int drawn)
{
    /* the page's selected row, as it is before anything changes (below) */
    level_t *top = &S.stack[S.depth - 1];
    pack_row_t on = {-1, -1};
    if (top->kind == L_PACKS) {
        pack_row_t r[PACK_ROWS];
        int n = packs_rows_now(r);
        if (top->sel >= 0 && top->sel < n) on = r[top->sel];
    }
    /* a download that has just failed: why, once */
    ui_pack_t was[UI_MAX_PACKS];
    int nwas = S.npacks;
    memcpy(was, S.packs, sizeof was);
    packs_look();
    for (int i = 0; i < S.npacks; i++)
        if (S.packs[i].state == UI_PACK_FAILED && S.packs[i].message[0] &&
            (i >= nwas || was[i].state != UI_PACK_FAILED || strcmp(was[i].message, S.packs[i].message)))
            ui_toast(S.packs[i].message, 3500);
    if (S.packs_all) packs_all_next();            /* (one has come: the next the library needs) */
    if (drawn) {
        lv_obj_invalidate(lv_screen_active());
        lv_obj_invalidate(lv_layer_top());
    }
    home_sync();                                  /* (a pack came, or the library's names changed: Home's offer with it) */
    level_t *l = &S.stack[S.depth - 1];
    if (S.now_playing_view) return;
    if (l->kind == L_PACKS) {
        /* A row came or went (what the library needs, Hide): the highlight stays
         * on the row it was on, wherever that is now; on the last, if it's gone. */
        pack_row_t r[PACK_ROWS];
        int n = packs_rows(r), sel = l->sel;
        for (int i = 0; i < n; i++)
            if (r[i].kind == on.kind && r[i].pack == on.pack) l->sel = i;
        if (l->sel >= n) l->sel = n - 1;
        if (l->sel < 0) l->sel = 0;
        render(l->sel != sel ? R_SNAP : R_CONTENT);
    } else if (l->kind == L_INTERFACE || l->kind == L_HOME || drawn) {
        render(R_CONTENT);
    }
}

/* What a page shows has changed: while it is the one on top (PC_QUICK: or
 * quick settings is) its selection is kept within its rows and it is drawn
 * again. PC_SNAP: at once where the selection had to move (the list got
 * shorter under it). PC_FLOOR: with no rows left, the selection is row 0. */
enum { PC_QUICK = 1, PC_SNAP = 2, PC_FLOOR = 4 };
static void page_changed(list_kind_t kind, int how)
{
    level_t *l = &S.stack[S.depth - 1];
    if ((l->kind != kind && !((how & PC_QUICK) && l->kind == L_QUICK)) || S.now_playing_view) return;
    int n = list_count(l), sel = l->sel;      /* (list_count reads the page's state again) */
    if (l->sel >= n) l->sel = n - 1;
    if ((how & PC_FLOOR) && l->sel < 0) l->sel = 0;
    render((how & PC_SNAP) && l->sel != sel ? R_SNAP : R_CONTENT);
}

void ui_uninstall_changed(void)
{
    uninstall_look();
    uninstall_overlay();
    page_changed(L_UNINSTALL, PC_SNAP | PC_FLOOR);   /* (the list got shorter under the selection: it moves) */
}

void ui_update_changed(void)
{
    update_look();
    update_overlay();
    page_changed(L_UPDATE, PC_SNAP);   /* (Try again: the list got shorter under the selection) */
}

void ui_set_radio_art(const uint16_t *px)
{
    if (px) {
        memcpy(S.art_px, px, sizeof S.art_px);
        lv_image_cache_drop(&S.art_dsc);   /* same descriptor, new pixels */
        lv_obj_invalidate(S.np_img);
    }
    S.radio_art = px != NULL;
    logo_forget();                         /* (art_px holds the song's cover, or did: the station's logo read again when it's its turn) */
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
    popup_style(S.toast, S.toast_lbl, font);
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
    /* (over the update's screen, which is on the same layer and made after it:
     * "Charge the player to 15 %" at Install was drawn underneath) */
    if (S.upd_ov) lv_obj_move_foreground(S.toast);
    lv_obj_align(S.toast, LV_ALIGN_BOTTOM_MID, 0, -16);
    popup_arm(&S.toast_timer, toast_hide, (uint32_t)ms);
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
    case ST_HOME_SCREEN: return "Show, hide and move the items on the Home screen.";
    case ST_PACKS: return "The characters that names in Japanese, Korean and Chinese need, to download once.";
    case ST_COVERS: return "Show album covers in lists of albums and of songs.";
    case ST_ALBUM_FLOW: return "Browse albums by their covers: in every list of albums, or only inside an artist.";
    case ST_ALBUM_ARTISTS: return "Display artist name when browsing albums.";
    case ST_BRIGHTNESS: return "How bright the screen is. Press <Play>, then turn the wheel.";
    case ST_TIMEOUT: return "How long the screen stays on. Saves battery.";
    case ST_DIM: return "Dims the screen 5 seconds before it turns off.";
    case ST_ON_CHARGING: return "Keeps the screen on while the player is plugged in.";
    case ST_LANGUAGE: return "Interface language. More in fathom/lang.";
    case ST_OUTPUT: return "Where the sound goes. <Play> shows each step in the path.";
    case ST_PURE: return "Skips EQ, crossfeed, stereo width, balance and levelling, for bit-perfect sound.";
    case ST_EQ: return "Shape the sound with an equaliser. Each output keeps its own.";
    case ST_BALANCE: return "Left/right balance. Press <Play>, then turn the wheel.";
    case ST_CROSSFEED: return "Each ear hears a little of the other side, as with speakers.";
    case ST_WIDTH: return "How far apart left and right are, from mono to wider. Press <Play>, then turn the wheel.";
    case ST_VOL_LIMIT: return "Caps the volume, to protect your ears. Per output.";
    case ST_LEVELLING: return "Plays songs at the same loudness, or albums as mastered.";
    case ST_TRANSPORT: return "With a USB DAC: Bluetooth and screen off for the cleanest sound.";
    case ST_USB_DAC: return "Use the player as an external USB DAC.";
    case ST_DAC_FILTER: return "The DAC chip's filter, for headphones and line out. To taste.";
    case ST_SHUFFLE: return "Shuffle current playlist.";
    case ST_SHUF_BY: return "Shuffle songs or shuffle albums.";
    case ST_SHUF_FRESH: return "Songs you haven't heard in a while come up sooner.";
    case ST_SHUF_INTERLUDES: return "Leaves short intros, skits and interludes out of shuffle.";
    case ST_SHUF_WORKS: return "Keeps a work's parts together and in order, like a symphony's movements.";
    case ST_SHUF_SPOKEN: return "Leaves out tracks whose genre is audiobook, podcast or spoken.";
    case ST_REPEAT: return "Repeat all, or one song.";
    case ST_PAUSE_UNPLUG: return "Pauses when wired headphones are unplugged.";
    case ST_BATTERY: return "Charge and time left. <Play> shows the battery history.";
    case ST_AUTO_OFF: return "Turns the player off when it's not in use.";
    case ST_POD_AUTO: return "Your shows' new episodes download by themselves, while charging on Wi-Fi.";
    case ST_POD_KEEP: return "How many of each show's newest unplayed episodes are downloaded.";
    case ST_POD_CLEAN: return "A played episode's download is deleted after this long.";
    case ST_POD_SPEED: return "Episodes faster, without the voices going up. Music always plays as it is.";
    case ST_SLEEP: return "Stops the music after a set time or at the end of the album.";
    case ST_RESCAN: return "Read the music folder again, showing how far it has got. Files not read before are tried again. <Back> lets it carry on while you listen.";
    case ST_FIND_COVERS: return "Finds covers online for albums without one. Keeps only sure matches.";
    case ST_N_SONGS: case ST_N_ALBUMS: case ST_N_ARTISTS: case ST_N_PLAYLISTS: return "Library counts.";
    case ST_N_UNREAD: return "Music files Fathom couldn't read: damaged, or ones that stopped it. They aren't in the library. Rescan tries them again.";
    case ST_USB_COMPUTER: return "What to do when plugged into a computer.";
    case ST_USB_DRIVE: return "Use the player as a USB drive, to copy music.";
    default: return NULL;
    }
}

static const char *page_help(int set)
{
    static const char *const H[] = {
        [SET_INTERFACE] = "Themes, the Home screen, list settings and languages.",
        [SET_DISPLAY] = "Screen brightness and time-outs.",
        [SET_SOUND] = "Output, EQ, stereo width, balance, volume and levelling.",
        [SET_BLUETOOTH] = "Pair and connect Bluetooth headphones and speakers.",
        [SET_WIFI] = "Join and manage Wi-Fi networks.",
        [SET_SHARE] = "Copy music to the player from a computer, over Wi-Fi.",
        [SET_SSH] = "Connect to the player through SSH.",
        [SET_USB] = "What the player does when you plug it into a computer.",
        [SET_PLAYBACK] = "Shuffle options, repeat, sleep timer and pause on unplug.",
        [SET_PODCASTS] = "Downloading new episodes, keeping and deleting them, speed.",
        [SET_POWER] = "Battery and auto power off.",
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
        case WR_LINK: return "The network you're on. <Play> shows its address and signal.";
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
        case AB_RESET: return "Reset all settings to defaults. Press <Play> twice.";
        case AB_UNINSTALL: return "Put the player back on HiBy's own firmware.";
        case AB_AUTO: return "Check for updates three times a day over Wi-Fi.";
        case AB_CHECK: return "Check for a newer version of Fathom.";
        case AB_TIPS: return "The keys worth knowing, as shown after installing.";
        case AB_WHATSNEW: return NULL;
        default:
            if (!strcmp(S.about_lbl[i], UI_ABOUT_CREDIT)) return UI_ABOUT_NOTICE;
            if (!strcmp(S.about_lbl[i], "Clock"))
                return S.hooks.clock_set ? "Set from the internet over Wi-Fi, or by hand: press <Play>. Your time zone is there too."
                                         : "Set from the internet over Wi-Fi. In UTC.";
            if (!strcmp(S.about_lbl[i], "Charge cycles")) return "Battery wear, in full charges used.";
            return NULL;
        }
    case L_UPD_ASK:
        return i == 0 ? "Download and install it. Also in Settings > About."
                      : "Hide it from Home until a newer version is out.";
    case L_PACKS: {
        pack_row_t r[PACK_ROWS];
        if (i >= packs_rows(r)) return NULL;
        switch (r[i].kind) {
        case PR_NEEDED: return "Some of your songs have names in other languages. <Play> downloads the characters they need, once, over Wi-Fi.";
        case PR_SHARED: return "Chinese and Japanese share many characters, drawn a little differently. <Play> changes whose are used.";
        case PR_HIDE: return "Take Get language packs off Home. The packs stay here.";
        default: break;
        }
        switch (S.packs[r[i].pack].state) {
        case UI_PACK_GETTING: return "Downloading. <Play> cancels.";
        case UI_PACK_THERE: return "Names in this language are shown. Press <Play> twice to remove it.";
        case UI_PACK_FAILED: return "Press <Play> to try again, over Wi-Fi.";
        default: return "Press <Play> to download it, over Wi-Fi. It stays on the card.";
        }
    }
    case L_UNINSTALL:
        if (i >= n) return NULL;
        switch (uninstall_row(i)) {
        case UR_LOOK: return "Put HiBy's update file for the player at the top of the card or in Music.";
        case UR_VERSION: return "Use your player's version of HiBy's firmware, or a newer one.";
        case UR_GO: return "Press <Play> twice. Your music stays on the card.";
        default: return NULL;
        }
    default:
        if (!is_setting_level(l->kind) || l->kind == L_QUICK || i >= n) return NULL;
        if (setting_at(l, i) == ST_LEVELLING && !S.pure) {   /* (and how far its measuring is) */
            static char h[240];
            int done, total;
            if (levelling_left(&done, &total) >= 0) {
                char d[16], t[16];
                snprintf(d, sizeof d, "%d", done);
                snprintf(t, sizeof t, "%d", total);
                tr_fill(h, sizeof h, "%s of %s songs ready. The rest are measured while charging and not playing.",
                        d, t);
                return h;
            }
        }
        return setting_help(setting_at(l, i));
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
    popup_style(S.lvl, S.lvl_lbl, S.f->meta);
    bar_style(S.lvl_bar, t->bg);
    lv_bar_set_range(S.lvl_bar, 0, max);
    lv_bar_set_value(S.lvl_bar, value, LV_ANIM_OFF);
    if (value_text) lv_label_set_text_fmt(S.lvl_lbl, "%s  %s", tr(text), value_text);
    else lv_label_set_text_fmt(S.lvl_lbl, "%s  %d / %d", tr(text), value, max);
    lv_obj_remove_flag(S.lvl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(S.lvl, LV_ALIGN_BOTTOM_MID, 0, -16);
    if (S.toast) lv_obj_add_flag(S.toast, LV_OBJ_FLAG_HIDDEN);   /* one overlay at a time */
    S.help_shown = 0;   /* (a help it hid: the next Menu isn't "put it away") */
    popup_arm(&S.lvl_timer, level_hide, 1200);
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
        tr_fill(src, sizeof src, "Source: %s", m.source == CF_APPLE ? "Apple" : "MusicBrainz", NULL);
        set_text(S.cf_note, pic == -2 ? tr("Couldn't fetch it") : src);
        snprintf(hint, sizeof hint, "%d / %d  %s", S.cf_k + 1, n,
                 tr(pic == 1 ? "<Play>: use this cover" : pic == 0 ? "Fetching its picture\xE2\x80\xA6"
                    : pic == -2 ? "<Play>: try again" : "No picture"));
        set_text(S.cf_hint, hint);
    } else {
        set_text(S.cf_title, st == 0 ? tr("Looking\xE2\x80\xA6") : st == -2 ? tr("Couldn't look it up") : tr("Nothing found"));
        set_text(S.cf_artist, "");
        set_text(S.cf_meta, "");
        set_text(S.cf_note, st == -2 ? tr("No network, or no answer") : "");
        set_text(S.cf_hint, st == 0 ? "" : tr("<Back>: done"));
    }
}

/* A song the player couldn't play and passed over: said (once the screen is
 * on, if it was off), with why when it's the output, or when none could play */
static unsigned g_fail_seen;
static void say_failed(void)
{
    if (S.now.fail_seq == g_fail_seen || S.asleep) return;
    g_fail_seen = S.now.fail_seq;
    int bt = S.bt_link == UI_BT_LINK_CONNECTED;   /* (the music goes to the headphones) */
    char msg[192], title[96] = "";
    lib_track_t t;
    if (S.lib && library_track(S.lib, S.now.fail_track, &t) == 0) snprintf(title, sizeof title, "%s", t.title);
    if (S.now.fail_all && S.now.fail_output)
        snprintf(msg, sizeof msg, "%s", tr(bt ? "Bluetooth can't play these songs' format" : "This output can't play these songs' format"));
    else if (S.now.fail_all)
        snprintf(msg, sizeof msg, "%s", tr("None of these songs could be played"));
    else if (S.now.fail_memory)                     /* hours of AAC in one file: its table of frames won't fit */
        tr_fill(msg, sizeof msg, S.now.fail_end ? "\u201C%s\u201D is too long for the player's memory"
                                                : "\u201C%s\u201D is too long for the player's memory: skipped", title, NULL);
    else if (S.now.fail_output && S.now.fail_end)   /* (the last: nothing after it to skip to) */
        tr_fill(msg, sizeof msg, bt ? "Bluetooth can't play \u201C%s\u201D" : "This output can't play \u201C%s\u201D", title, NULL);
    else if (S.now.fail_output)
        tr_fill(msg, sizeof msg, bt ? "Bluetooth can't play \u201C%s\u201D: skipped" : "This output can't play \u201C%s\u201D: skipped", title, NULL);
    else if (S.now.fail_end)
        tr_fill(msg, sizeof msg, "Couldn't play \u201C%s\u201D", title, NULL);
    else
        tr_fill(msg, sizeof msg, "Couldn't play \u201C%s\u201D: skipped", title, NULL);
    ui_toast(msg, 3000);
}

/* ---------- albums as cover flow ----------
 * Settings > Interface > Cover flow. A list of albums (Albums, an
 * artist's, a genre's, a year's...) shown by its covers: the selected one in
 * the middle, three at each side turned towards it, its name and artist
 * under it, and a line at the foot for where in the list it is. Everything
 * else is the list's: the wheel moves the selection, Play opens the album,
 * Menu is its menu, Back goes back. Only what is drawn differs (render_flow
 * in place of render_list), so the list's keys and levels know nothing of it.
 *
 * The covers are the library's large ones (136 pixels, 37 KB each, read from
 * the card's art cache as they come into view), ten held at most, with each
 * one's narrow copy for when it is turned: 0.6 MB with the picture, taken when
 * a cover flow is drawn and given back when it goes or the screen turns off. */
_Static_assert(FLOW_COVER == LIB_ART_LARGE && FLOW_W == UI_W, "the cover flow's sizes are the library's and the screen's");
#define FLOW_HELD 10
#define DUR_FLOW 220            /* ms: from one album to the next */
static struct flow_mem {
    uint16_t px[FLOW_W * FLOW_H];
    struct flow_ent {
        int64_t id;
        uint32_t used;          /* 0: free */
        int has;                /* its cover read; 0: the album has none */
        uint16_t full[FLOW_COVER * FLOW_COVER], narrow[FLOW_NARROW * FLOW_COVER];
    } e[FLOW_HELD];
    uint32_t tick;
    /* an album with no cover: a record drawn on a tile, in the theme's colours */
    uint16_t none_full[FLOW_COVER * FLOW_COVER], none_narrow[FLOW_NARROW * FLOW_COVER];
    int none_theme;
    /* `touch /tmp/fathom-flow` before a cover flow is opened: each slide's
     * frames and what drawing them took go to the log (measured on the H2) */
    int timed, frames, reads;
    long paint_us, paint_max, read_us, began_ms;
    long frame_at;              /* the last frame of the slide under way (0: its first is to come) */
    const char *took_what;      /* the longest thing the UI's thread did since that frame (ui_took) */
    long took_us;
} *g_flow;

/* Settings > Interface > Cover flow (display.album_flow): 1 every list of
 * albums; 2 only an artist's (an album artist's, an artist's by their credits,
 * a composer's), Albums and the rest staying lists. */
static long g_render_head_us;      /* (timed: what a render does before the screen's own part) */

/* While slides are timed: something on the UI's thread took this long (the
 * platform says its own: the queue's place saved, its polls). A slide's frame
 * that comes late names the longest since the frame before. */
int ui_timing(void) { return g_flow && g_flow->timed; }

void ui_took(const char *what, long us)
{
    if (!g_flow || !g_flow->timed || us <= g_flow->took_us) return;
    g_flow->took_what = what;
    g_flow->took_us = us;
}

static int flow_on(const level_t *l)
{
    if (!S.lib || !is_album_list(l->kind) || l->items.n <= 0 || S.moving) return 0;
    int v = settings_get_int("display.album_flow", UI_ALBUM_FLOW_DEFAULT);
    if (v == 2) return l->kind == L_ARTIST_ALBUMS || l->kind == L_CREDIT_ALBUMS || l->kind == L_COMPOSER_ALBUMS;
    return v == 1;
}

static int row_initial(const level_t *l, int i);

/* A cover flow of albums in the order of their names: the letters show along
 * its foot (a list no longer than what shows at once has no use for them) */
static int flow_lettered(const level_t *l)
{
    return (l->kind == L_ALBUMS || l->kind == L_GENRE_ALBUMS || l->kind == L_YEAR_ALBUMS) && l->items.n > FLOW_N;
}

static void flow_anim_cb(void *var, int32_t v);

static void flow_free(void)
{
    if (S.flow) {
        lv_anim_delete(S.flow_img, flow_anim_cb);
        show(S.flow, 0);
    }
    if (S.flow_more) { lv_timer_delete(S.flow_more); S.flow_more = NULL; }
    S.flow_shift = 0;
    S.flow_sel = -1;
    S.flow_ids = NULL;
    S.flow_named = -1;
    if (!g_flow) return;
    lv_image_cache_drop(&S.flow_dsc);
    S.flow_dsc.data = NULL;
    free(g_flow);
    g_flow = NULL;
}

int ui_test_flow_cover(int64_t album)
{
    for (int k = 0; g_flow && k < FLOW_HELD; k++)
        if (g_flow->e[k].used && g_flow->e[k].id == album) return g_flow->e[k].has;
    return -1;
}

/* An album's cover from those held; read from the card if it isn't and *budget
 * allows (NULL then: its place shows a plain tile until it is) */
static struct flow_ent *flow_cover(int64_t id, int *budget)
{
    struct flow_ent *old = &g_flow->e[0];
    for (int k = 0; k < FLOW_HELD; k++) {
        struct flow_ent *e = &g_flow->e[k];
        if (e->used && e->id == id) { e->used = ++g_flow->tick; return e; }
        if (e->used < old->used) old = e;
    }
    if (*budget <= 0) return NULL;
    (*budget)--;
    long t0 = g_flow->timed ? (long)clock_us() : 0;
    old->id = id;
    old->used = ++g_flow->tick;
    old->has = library_art(S.lib, id, LIB_ART_LARGE, old->full) == 0;
    if (old->has) flow_narrow(old->full, old->narrow);
    if (g_flow->timed) {
        g_flow->reads++;
        g_flow->read_us += (long)clock_us() - t0;
        ui_took("a cover read", (long)clock_us() - t0);
    }
    return old;
}

/* The tile for an album with no cover: a record (its rim, its label's ring,
 * its hole) on the theme's surface colour. Drawn again when the theme changes. */
static void flow_none(void)
{
    const theme_t *t = T();
    uint16_t ground = rgb565_of(t->surface), line = rgb565_of(theme_fit(t->text_muted, t->surface, 2.0));
    const int c = FLOW_COVER / 2;
    for (int y = 0; y < FLOW_COVER; y++)
        for (int x = 0; x < FLOW_COVER; x++) {
            int d2 = (2 * x + 1 - 2 * c) * (2 * x + 1 - 2 * c) + (2 * y + 1 - 2 * c) * (2 * y + 1 - 2 * c);   /* (twice the distance, squared) */
            int on = (d2 >= 4 * 40 * 40 && d2 <= 4 * 43 * 43) || (d2 >= 4 * 14 * 14 && d2 <= 4 * 16 * 16) || d2 <= 4 * 4 * 4;
            g_flow->none_full[y * FLOW_COVER + x] = on ? line : ground;
        }
    flow_narrow(g_flow->none_full, g_flow->none_narrow);
    g_flow->none_theme = S.theme_gen;
}

static void flow_more_cb(lv_timer_t *t);

/* The picture, for the list's selection and the slide as it is now. A cover
 * not held yet is read here, `budget` of them at most (37 KB from the card
 * each, 2 to 16 ms on the H2): one for a key's own picture and for a frame of
 * a slide (a turn of the wheel, or a frame, mustn't wait for seven); three a
 * time once the wheel is still (flow_more_cb), until all are there. */
static void flow_paint(const level_t *l, int budget)
{
    if (!g_flow) return;
    flow_cover_t cov[FLOW_N];
    int missing = 0;
    long t0 = g_flow->timed ? (long)clock_us() : 0;
    if (g_flow->none_theme != S.theme_gen) flow_none();
    /* (the middle one first, then outwards: what is read first is what shows most) */
    static const int ORDER[FLOW_N] = {4, 3, 5, 2, 6, 1, 7, 0, 8};
    for (int o = 0; o < FLOW_N; o++) {
        int k = ORDER[o], i = l->sel + k - FLOW_REACH;
        cov[k] = (flow_cover_t){0, NULL, NULL};
        if (i < 0 || i >= l->items.n) continue;
        cov[k].there = 1;
        struct flow_ent *e = flow_cover(l->items.ids[i], &budget);
        if (!e) { missing++; continue; }
        if (e->has) { cov[k].full = e->full; cov[k].narrow = e->narrow; }
        else { cov[k].full = g_flow->none_full; cov[k].narrow = g_flow->none_narrow; }
    }
    const theme_t *t = T();
    int shift = S.flow_shift;
    if (t->motion_stepped) shift = shift / 64 * 64;         /* (a pixel theme's moves are steps) */
    flow_draw(g_flow->px, cov, shift, rgb565_of(t->bg), rgb565_of(t->surface), t->corner == THEME_CORNER_ROUND);
    lv_image_cache_drop(&S.flow_dsc);
    lv_obj_invalidate(S.flow_img);
    if (g_flow->timed) {
        long us = (long)clock_us() - t0;
        g_flow->frames++;
        g_flow->paint_us += us;
        if (us > g_flow->paint_max) g_flow->paint_max = us;
    }
    if (missing && !S.flow_shift && !S.flow_more) {
        S.flow_more = lv_timer_create(flow_more_cb, 30, NULL);
        lv_timer_set_repeat_count(S.flow_more, 1);
    }
}

static void flow_more_cb(lv_timer_t *t)
{
    (void)t;
    S.flow_more = NULL;                         /* (a timer that has run its count deletes itself) */
    level_t *l = &S.stack[S.depth - 1];
    if (g_flow && !S.asleep && !S.now_playing_view && flow_on(l) && l->items.ids == S.flow_ids) flow_paint(l, 3);
}

/* Covers were made or changed while the cover flow holds some (a scan's cover
 * pass in the background, Find cover): they are read again, all of them or
 * only the albums held as having none, and the picture is drawn again. (An
 * album whose cover had come went on showing the record until the flow was
 * left and opened again: only the lists' thumbnails were forgotten.) */
static void flow_forget(int all)
{
    if (!g_flow) return;
    for (int k = 0; k < FLOW_HELD; k++)
        if (all || !g_flow->e[k].has) g_flow->e[k].used = 0;
    level_t *l = &S.stack[S.depth - 1];
    /* (at rest: a slide's next frame reads them anyway) */
    if (!S.asleep && !S.now_playing_view && !S.flow_shift && flow_on(l) && l->items.ids == S.flow_ids) flow_paint(l, 1);
}

static void flow_anim_cb(void *var, int32_t v)
{
    (void)var;
    level_t *l = &S.stack[S.depth - 1];
    S.flow_shift = (int)v;
    if (g_flow && g_flow->timed) {
        long now = (long)clock_us();
        if (g_flow->frame_at && now - g_flow->frame_at > 80000)
            fprintf(stderr, "flow: %ld ms between two frames; the longest thing meanwhile: %s, %ld ms\n",
                    (now - g_flow->frame_at) / 1000, g_flow->took_what ? g_flow->took_what : "nothing timed", g_flow->took_us / 1000);
        g_flow->frame_at = now;
        g_flow->took_what = NULL;
        g_flow->took_us = 0;
    }
    if (g_flow && flow_on(l) && l->items.ids == S.flow_ids) flow_paint(l, 1);
    if (g_flow && g_flow->timed && v == 0 && g_flow->frames) {       /* the slide's end */
        long ms = (long)clock_us() / 1000 - g_flow->began_ms;
        fprintf(stderr, "flow: a slide of %ld ms in %d frames; drawing %ld us a frame, %ld at most; %d covers read, %ld us each\n",
                ms, g_flow->frames, g_flow->paint_us / g_flow->frames, g_flow->paint_max, g_flow->reads,
                g_flow->reads ? g_flow->read_us / g_flow->reads : 0);
    }
}

/* The top level as a cover flow: 1 if it is one (and drawn), 0 for the list. */
static int render_flow(render_mode_t mode)
{
    level_t *l = &S.stack[S.depth - 1];
    if (!flow_on(l)) { flow_free(); return 0; }
    if (!g_flow) {
        if (!(g_flow = calloc(1, sizeof *g_flow))) return 0;      /* (no memory for it: the list) */
        image_dsc(&S.flow_dsc, LV_COLOR_FORMAT_RGB565, FLOW_W, FLOW_H, g_flow->px, sizeof g_flow->px);
        lv_image_set_src(S.flow_img, &S.flow_dsc);
        S.flow_sel = -1;
        g_flow->none_theme = -1;
        g_flow->timed = access("/tmp/fathom-flow", F_OK) == 0;
        if (g_flow->timed) fprintf(stderr, "flow: %zu bytes held while it shows\n", sizeof *g_flow);
    }
    int n = l->items.n;
    if (l->sel < 0) l->sel = 0;
    if (l->sel >= n) l->sel = n - 1;
    char head[136];
    const char *title = l->title_ui ? tr(l->title) : l->title;
    if (S.depth > 1) snprintf(head, sizeof head, "\xe2\x80\xb9 %s", title);
    else snprintf(head, sizeof head, "%s", title);
    set_text(S.h_left, head);
    S.last_depth = S.depth;
    S.last_kind = l->kind;
    S.last_parent = l->parent;
    S.last_top = l->top;

    /* the same list as last drawn, one album on: the row slides there. Anything
     * else (another list, a jump, the library or the theme changed): at rest. */
    int same = S.flow_ids == l->items.ids && S.flow_gen == S.lib_gen && S.flow_sel >= 0;
    if (S.flow_gen != S.lib_gen || S.flow_theme != S.theme_gen)
        for (int k = 0; k < FLOW_HELD; k++) g_flow->e[k].used = 0;          /* (covers may have changed: read again) */
    int by = same ? l->sel - S.flow_sel : 0;
    int changed = !same || by || S.flow_theme != S.theme_gen || !lv_obj_is_visible(S.flow);
    if (same && (by == 1 || by == -1) && mode == R_ANIMATE) {
        if (g_flow->timed) {
            g_flow->frames = g_flow->reads = 0;
            g_flow->paint_us = g_flow->paint_max = g_flow->read_us = 0;
            g_flow->began_ms = (long)clock_us() / 1000;
            if (!lv_anim_get(S.flow_img, flow_anim_cb)) g_flow->frame_at = 0;   /* (a new slide: no frame before its first) */
        }
        /* (a slide under way: on from where it is, up to a place and a half
         * away. Held to one place, a turn made before the last slide was
         * over threw the rest of it away: the row jumped.) */
        int from = S.flow_shift + by * FLOW_STEP;
        if (from > FLOW_SHIFT_MAX) from = FLOW_SHIFT_MAX;
        if (from < -FLOW_SHIFT_MAX) from = -FLOW_SHIFT_MAX;
        S.flow_shift = from;
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, S.flow_img);
        lv_anim_set_exec_cb(&a, flow_anim_cb);
        lv_anim_set_values(&a, from, 0);
        lv_anim_set_duration(&a, DUR_FLOW);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_start(&a);                              /* (takes the place of one already running on it) */
        changed = 2;                                    /* (its first frame is drawn as it starts: not again below) */
    } else if (changed) {
        lv_anim_delete(S.flow_img, flow_anim_cb);
        S.flow_shift = 0;
    }
    S.flow_ids = l->items.ids;
    S.flow_sel = l->sel;
    S.flow_gen = S.lib_gen;
    S.flow_theme = S.theme_gen;
    show(S.flow, 1);
    lv_obj_move_foreground(S.flow);
    long tp0 = g_flow->timed ? (long)clock_us() : 0;
    if (changed == 1) flow_paint(l, 1);
    long tp1 = g_flow->timed ? (long)clock_us() : 0, ti1 = tp1, tl0 = tp1;

    /* its name, and who it is by; the one that's playing in the signal's colour */
    int64_t id = l->items.ids[l->sel];
    if (changed || id != S.flow_named) {
        lib_album_t al;
        memset(&al, 0, sizeof al);
        library_album_info(S.lib, id, &al);
        char line[300];
        if (al.artist[0] && al.year) snprintf(line, sizeof line, "%s \xC2\xB7 %d", al.artist, al.year);
        else if (al.year) snprintf(line, sizeof line, "%d", al.year);
        else snprintf(line, sizeof line, "%s", al.artist);
        set_text(S.flow_title, al.title[0] ? al.title : tr("Unknown album"));
        set_text(S.flow_by, line);
        S.flow_named = id;
        if (g_flow->timed) ti1 = (long)clock_us();
    }
    set_text_color(S.flow_title, C(S.now.active && S.now.track.album_id == id && !S.now.radio ? T()->signal : T()->text));
    /* where in the list: the letters, the album's lit (a list by name), or a line */
    if (g_flow->timed) tl0 = (long)clock_us();
    int az = flow_lettered(l), pill = az && T()->corner == THEME_CORNER_ROUND;
    show(S.flow_track, !az);
    show(S.flow_az_pill, pill);
    if (az) {
        int on = row_initial(l, l->sel), every = S.flow_az_every;
        uint32_t lit = pill ? T()->select_fg : T()->signal;
        for (int i = 0; i < 27; i++) {
            /* (every other one: the one that's on always, its neighbours making room for it) */
            int there = every == 1 || i == on || (i % 2 == 0 && abs(i - on) != 1);
            show(S.flow_az[i], there);
            if (i == on || i == S.flow_az_on) set_text_color(S.flow_az[i], C(i == on ? lit : T()->text_muted));
        }
        S.flow_az_on = on;
        /* (its letter's place is 11 wide in a label of 23: the pill, 15, about its middle) */
        int px = 8 + on * (UI_W - 16) / 27 - 6 + ((UI_W - 16) / 27 + 12) / 2 - ((UI_W - 16) / 27 + 4) / 2;
        if (pill && lv_obj_get_style_x(S.flow_az_pill, 0) != px) lv_obj_set_x(S.flow_az_pill, px);
    } else {
        for (int i = 0; i < 27; i++) show(S.flow_az[i], 0);
        int tw = UI_W - 24, w = n > 1 ? tw / n : tw;
        if (w < 16) w = 16;
        int x = n > 1 ? (int)((long)(tw - w) * l->sel / (n - 1)) : 0;
        if (lv_obj_get_style_width(S.flow_thumb, 0) != w) lv_obj_set_width(S.flow_thumb, w);
        if (lv_obj_get_style_x(S.flow_thumb, 0) != x) lv_obj_set_x(S.flow_thumb, x);
    }
    if (g_flow->timed && changed)
        fprintf(stderr, "flow: drawn for a key: the player and the header %ld us, the picture %ld us, the album's name %ld us, its letter %ld us\n",
                g_render_head_us, tp1 - tp0, ti1 - tp1, (long)clock_us() - tl0);
    return 1;
}

static void render(render_mode_t mode)
{
    int np_changed = S.now_playing_view != S.last_np;
    long th0 = ui_timing() ? (long)clock_us() : 0;
    if (S.player) player_now(S.player, &S.now);
    if (S.player) say_failed();
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
    if (th0) g_render_head_us = (long)clock_us() - th0;
    if (cfv) {
        render_cf();
    } else if (S.now_playing_view) {
        if (g_flow) flow_free();    /* (a cover flow under it: its pictures given back meanwhile, read again when it shows) */
        render_np();
        if (np_changed && mode == R_ANIMATE) start_transition(TR_NP_IN);
    } else if (np_changed && mode == R_ANIMATE) {
        if (!render_flow(R_SNAP)) render_list(R_SNAP);
        start_transition(TR_NP_OUT);
    } else {
        if (!render_flow(mode)) render_list(mode);
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

/* A library level's rows read into it (its items empty before: a new level,
 * or freed by the caller); what it lists is its kind and its parents */
static void level_load(level_t *l)
{
    if (!library_backed(l->kind) || !S.lib) return;
    if (l->kind == L_CREDIT_TRACKS) library_list_credit_tracks(S.lib, l->parent, l->parent2, &l->items);
    else if (l->kind == L_COMPOSER_TRACKS) library_list_composer_tracks(S.lib, l->parent, l->parent2, &l->items);
    else if (l->kind == L_PLAYLISTS) playlists_load(l);
    else library_list(S.lib, view_of(l->kind), l->parent, &l->items);
}

/* Every level is in use (pages opened one from another, ten deep): said,
 * and nothing more opens until <Back> closes one. (Nothing was said, and
 * what followed a push wrote to the level on top as if it were the new one:
 * from Now Playing, <Menu> showed the list under it.) */
static int stack_full(void)
{
    if (S.depth < (int)(sizeof S.stack / sizeof S.stack[0])) return 0;
    ui_toast("Too many screens open: <Back> closes one", 2500);
    return 1;
}

static int push_as(list_kind_t kind, const char *title, int64_t parent, int ui)
{
    if (stack_full()) return 0;
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
    if (kind == L_CREDIT_TRACKS || kind == L_COMPOSER_TRACKS) return 1;   /* push_*_tracks() fills it: the album isn't known here */
    level_load(l);
    if (kind == L_THEMES) l->sel = S.cur_theme;
    if (kind == L_EQ) {                    /* the profiles on the card, now; the current one selected */
        S.eq_n = S.hooks.eq_list ? S.hooks.eq_list(S.eq_names, 64) : 0;
        for (int i = 0; i < S.eq_n; i++)
            if (!strcmp(S.eq_names[i], S.eq_name)) l->sel = i + 1;
        ui_toast(S.eq_n ? "<Menu> on an EQ: edit it" : "AutoEQ files go in fathom/eq. Or: New EQ", 2500);
    }
    if (kind == L_LIBRARY) lib_counts_load();
    return 1;
}

/* An artist's songs on one album (Artists > artist > album). */
static void push_credit_tracks(const char *title, int64_t artist, int64_t album)
{
    if (!push(L_CREDIT_TRACKS, title, artist)) return;
    level_t *l = &S.stack[S.depth - 1];
    l->parent2 = album;
    level_load(l);
}

static void push_composer_tracks(const char *title, int64_t composer, int64_t album)
{
    if (!push(L_COMPOSER_TRACKS, title, composer)) return;
    level_t *l = &S.stack[S.depth - 1];
    l->parent2 = album;
    level_load(l);
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

static void radio_list_left(void);

/* The open list's stations go (its level closed, or another search asked in
 * it): a station on air that was started from them has lost its list. */
static void rd_free_list(void)
{
    if (S.radio_from > 0) radio_list_left();
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

/* The network can be used now (1). Else (0) Wi-Fi is asked for, or waited
 * for (then: what to do once it has connected, WT_*); or, with neither, why
 * not is said: the device's reason, or `need` where it gives none. */
static int net_or_ask(int then, const char *need)
{
    char why[96] = "";
    if (!S.hooks.radio_ready || S.hooks.radio_ready(why, sizeof why) == 0) return 1;
    if (!wifi_ask_if_off(then)) ui_toast(why[0] ? why : need, 2500);
    return 0;
}

/* The same for the Lyrics screen (ui_lyrics.c), which has just put itself
 * away: the question, if one is asked, is drawn where it was opened from. */
int ui_net_or_ask(const char *need)
{
    int r = net_or_ask(WT_NONE, need);
    render(R_SNAP);
    return r;
}

/* The directory can be asked now (Wi-Fi is connected); else why not. */
static int rd_ready(char *why, size_t cap)
{
    why[0] = '\0';
    if (!S.hooks.radio_ready || S.hooks.radio_ready(why, cap) == 0) return 1;
    if (!why[0]) snprintf(why, cap, "The radio needs Wi-Fi");
    return 0;
}

/* ...to play a station: Wi-Fi asked for, or why not said */
static int rd_net(void) { return net_or_ask(WT_OK, "The radio needs Wi-Fi"); }

#define RD_UNREACHABLE "Couldn't reach the station directory. <Play> tries again"
/* (an HTTPS request that failed: the clock, while it isn't set, netclock.h) */
#define RD_FAILED_WHY() netclock_why(RD_UNREACHABLE)

/* Ask for the list on top: stations for q, or (NULL) the countries. */
static void rd_ask(const rdir_query_t *q)
{
    rd_stop();
    S.rd_state = RD_IDLE;
    if (!rd_ready(S.rd_msg, sizeof S.rd_msg)) {
        S.rd_state = RD_FAILED;
        int asked = wifi_ask_if_off(WT_RDIR);
        if (asked) {
            snprintf(S.rd_msg, sizeof S.rd_msg, "%s", tr(asked == 2 ? "Waiting for Wi-Fi" : "Wi-Fi is off"));
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
    if (!push_as(L_RDIR_RESULTS, title, 0, ui)) return;   /* (a genre's kept in English: shown translated) */
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
    if (!S.player || !st || i < 0 || i >= n) return;
    if (!rd_net()) return;
    if (rd_on_air(&st[i])) {
        /* On already: shown, and this list is its list from now on (playing
         * by itself since the list it was started from was left, it stayed
         * alone: no Next, though it was chosen in a list again). */
        player_radio_relist(S.player, st, n, i);
        S.radio_from = S.depth;
        S.now_playing_view = 1;
        return;
    }
    player_radio_play(S.player, st, n, i);
    S.radio_from = S.depth;                    /* (the list's level is the one on top) */
    if (meta) rdir_count_click(meta[i].uuid);
    S.now_playing_view = 1;
}

/* Random station: the directory's stations for q in random order, played
 * from the first; Next goes on to another. */
static void rd_random(const rdir_query_t *q)
{
    if (!S.player) return;
    if (!rd_net()) return;
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
        S.radio_from = 0;
        rdir_count_click(r->meta[0].uuid);
        rd_to_np();
        return;
    }
    if (r->n == 0) { ui_toast("No stations found", 2000); return; }
    /* the directory didn't answer (Wi-Fi is on: it was asked): one of your own stations, at random */
    radio_read();
    radio_station_t *mix = S.radio_n > 0 ? malloc((size_t)S.radio_n * sizeof *mix) : NULL;
    if (!mix) { ui_toast(netclock_why("Couldn't reach the station directory"), 2500); return; }
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
    S.radio_from = 0;
    free(mix);
    ui_toast(netclock_why("Couldn't reach the directory: one of your stations"), 3000);
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
        snprintf(S.rd_msg, sizeof S.rd_msg, "%s", RD_FAILED_WHY());
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

/* The station on air is one of the user's (as last read from the card): Next
 * and Previous go through theirs from it. 1 if it is. */
static int radio_to_yours(void)
{
    const radio_station_t *cur = S.player ? player_radio_station(S.player) : NULL;
    for (int i = 0; cur && i < S.radio_n; i++)
        if (!strcmp(S.radio[i].url, cur->url)) {
            player_radio_relist(S.player, S.radio, S.radio_n, i);
            return 1;
        }
    return 0;
}

/* The list of the directory a station was started from has been left (Back
 * out of it, or another search typed in its place: rd_free_list; the screen
 * going dark, or Now Playing over it, isn't that). Next
 * and Previous went on through a search no longer there to be seen: now the
 * user's own stations if the one on air is theirs, and else nothing, the
 * station playing on by itself (the owner's rule, 2026-11-25). */
static void radio_list_left(void)
{
    S.radio_from = 0;
    const radio_station_t *cur = S.player ? player_radio_station(S.player) : NULL;
    if (!cur || radio_to_yours()) return;
    radio_station_t one = *cur;
    player_radio_relist(S.player, &one, 1, 0);
    S.radio_from = -1;
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
    if (l->kind == L_COVER_FIND) cf_find_close();    /* (however it is closed: its search ends) */
    library_list_free(&l->items);
    S.depth--;
    if (l->kind == L_WIFI_PASS) entry_unkeep();
    S.editing = 0;
    S.moving = 0;
    /* the directory: a request for a level that's gone is dropped, and a list's stations go with it */
    if (S.rd_job && S.rd_job_depth > S.depth) rd_stop();
    if (l->kind == L_RDIR_RESULTS || l->kind == L_RDIR_SEARCH) rd_free_list();
    if (l->kind == L_RDIR_COUNTRIES) rd_free_countries();
    if (to_np) S.now_playing_view = 1;
}

static void pop(void) { pop_level(0); }

/* Open an overlay-like level (quick settings, context menu) from wherever we
 * are; 0 if it couldn't be (every level in use: Now Playing stays). */
static int push_from_here(list_kind_t kind, const char *title, int64_t parent, int ui)
{
    if (!push_as(kind, title, parent, ui)) return 0;
    S.stack[S.depth - 1].back_to_np = S.now_playing_view;
    S.now_playing_view = 0;
    return 1;
}

/* A menu opened from Now Playing (its own, or quick settings): what was
 * opened from it before goes first, with anything opened from that: the
 * queue, a song's info, the artist's page, quick settings and Bluetooth's
 * page from them, left under Now Playing by a long <Back>. Kept, each visit
 * left a level or two more, until all of them were in use. */
static void np_levels_close(void)
{
    int d = 1;
    while (d < S.depth && !S.stack[d].back_to_np && !S.stack[d].from_np) d++;
    while (S.depth > d) pop_level(1);
}

/* Something that needs Wi-Fi, with Wi-Fi off: ask to turn it on (1), and
 * once it has connected do `then` (WT_*) where it was asked. On or starting
 * but not connected yet (the 10-20 s after a start, mostly): `then` is kept
 * for when it connects, as after "Turn on", and Wi-Fi is told to look for a
 * network now (2): what was pressed isn't thrown away. With nothing to do
 * then, from Now Playing (wifi_then_run does nothing there), or with no Wi-Fi
 * at all, the caller says why as before (0). */
static int wifi_ask_if_off(int then)
{
    if (!S.hooks.wifi_state || !S.hooks.wifi_do) return 0;
    char busy[32] = "";
    ui_wifi_status_t st = {0};
    if (S.hooks.wifi_state(busy, sizeof busy, &st) || busy[0]) {
        char why[96];
        if (then == WT_NONE || S.now_playing_view || st.state == UI_WIFI_CONNECTED || !strcmp(busy, "Stopping")) return 0;
        if (S.hooks.radio_ready && S.hooks.radio_ready(why, sizeof why) == 0) return 0;   /* (usable as it is: a proxy) */
        level_t *l = &S.stack[S.depth - 1];
        S.wifi_then.what = then;
        S.wifi_then.armed = 1;
        S.wifi_then.depth = S.depth;
        S.wifi_then.kind = l->kind;
        S.wifi_then.sel = l->sel;
        S.wifi_then.at = lv_tick_get();
        if (S.hooks.wifi_wake) S.hooks.wifi_wake();
        if (then != WT_RDIR) ui_toast("Waiting for Wi-Fi", 2500);   /* (the directory's lists say it on a line) */
        return 2;
    }
    if (S.stack[S.depth - 1].kind == L_WIFI_ASK) return 1;
    S.wifi_then.what = then;
    S.wifi_then.armed = 0;
    return push_from_here(L_WIFI_ASK, "Turn on Wi-Fi?", 0, 1);
}

/* Wi-Fi connected (or gave up): what "Turn on Wi-Fi" was for. */
static void wifi_then_run(int connected)
{
    if (!S.wifi_then.armed) return;
    S.wifi_then.armed = 0;
    level_t *l = &S.stack[S.depth - 1];
    /* A list of the directory that says "Waiting for Wi-Fi", still open with
     * another screen over it by now (Now Playing, or quick settings and
     * Wi-Fi's page: where one goes to join a network): it is asked for, or
     * says it couldn't connect, all the same. Passed over as moved on from,
     * it said Waiting until <Play> was pressed on it. */
    int d = S.wifi_then.depth;
    if (S.wifi_then.what == WT_RDIR && d >= 1 && d <= S.depth && S.stack[d - 1].kind == S.wifi_then.kind &&
        (d < S.depth || (connected && S.now_playing_view)) && S.rd_state == RD_FAILED && !S.rd_job) {
        char why[96];
        if (!connected) snprintf(S.rd_msg, sizeof S.rd_msg, "%s", "Couldn't connect: Settings > Wi-Fi");
        else if (rd_ready(why, sizeof why)) {
            rd_ask(S.wifi_then.q_null ? NULL : &S.wifi_then.q);
            if (S.rd_job == RD_JOB_LIST) S.rd_job_depth = d;     /* (the answer is for its level, not the one on top) */
        }
        return;
    }
    if (!connected) {
        if (S.depth == S.wifi_then.depth && l->kind == S.wifi_then.kind) {
            if (S.wifi_then.what == WT_RDIR && S.rd_state == RD_FAILED && !S.rd_job) {   /* (its line said Waiting) */
                snprintf(S.rd_msg, sizeof S.rd_msg, "%s", "Couldn't connect: Settings > Wi-Fi");
                render(R_CONTENT);
            }
            ui_toast("Couldn't connect: Settings > Wi-Fi", 2500);
        }
        return;
    }
    if (S.now_playing_view || S.depth != S.wifi_then.depth || l->kind != S.wifi_then.kind) return;   /* moved on */
    if (S.wifi_then.what == WT_RDIR) rd_ask(S.wifi_then.q_null ? NULL : &S.wifi_then.q);
    else if (S.wifi_then.what == WT_POD_SEARCH) pod_search_run();
    else if (S.wifi_then.what == WT_OK && l->sel == S.wifi_then.sel) open_selected();
    render(R_ANIMATE);
}

void ui_usb_prompt(void)
{
    for (int d = 1; d < S.depth; d++)             /* (asked already: under quick settings too, not a second one) */
        if (S.stack[d].kind == L_USB_ASK) return;
    if (S.stack[S.depth - 1].kind == L_CONTEXT) pop_level(1);
    if (S.depth >= (int)(sizeof S.stack / sizeof S.stack[0])) pop_level(1);   /* (every level in use: the top one makes way for the question) */
    push_from_here(L_USB_ASK, "Connected to a computer", 0, 1);
    render(R_ANIMATE);
}

void ui_usb_prompt_close(void)
{
    int d = S.depth - 1;
    while (d > 0 && S.stack[d].kind != L_USB_ASK) d--;
    if (d <= 0) return;
    if (d == S.depth - 1) {
        pop();
        render(R_ANIMATE);
        return;
    }
    /* Under something opened over it (quick settings, a page of theirs): taken
     * out from between the levels, which stay as they are; the one that was
     * on it closes to where the question would have. (Left there, it showed
     * again with no cable, and the next cable put a second one on top.) */
    int to_np = S.stack[d].back_to_np;
    memmove(&S.stack[d], &S.stack[d + 1], (size_t)(S.depth - 1 - d) * sizeof S.stack[0]);
    S.depth--;
    if (to_np) S.stack[d].back_to_np = 1;
    /* (what is kept by a level's depth, for the levels that came down one) */
    if (S.rd_job && S.rd_job_depth > d + 1) S.rd_job_depth--;
    if (S.radio_from > d + 1) S.radio_from--;
    if (S.wifi_then.depth > d + 1) S.wifi_then.depth--;
    render(R_CONTENT);
}

static void reset_restart(lv_timer_t *t)
{
    (void)t;
    if (S.hooks.restart) S.hooks.restart();
}

static void open_quick(void)
{
    /* Open already (left under Now Playing, or under a page opened from it:
     * Bluetooth, Wi-Fi): shown, that page closed. A second one over it, each
     * time, used the levels up. */
    for (int d = S.depth - 1; d > 0; d--)
        if (S.stack[d].kind == L_QUICK) {
            while (S.depth > d + 1) pop_level(1);
            S.now_playing_view = 0;
            return;
        }
    if (S.now_playing_view) np_levels_close();
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
    if (!S.hooks.theme_save || stack_full()) return;
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
            tr_fill(base, sizeof base, "%s copy", src->name, NULL);
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
    if (!S.hooks.theme_remove || !S.the_file[0]) return;
    if (!press_twice(&S.the_del_armed, "Press again to delete this theme")) return;
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
        if (stack_full()) return;
        S.rename_theme = 1;
        snprintf(S.entry, sizeof S.entry, "%s", t->name);
        S.entry_back = 0;
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

/* The network can be used now; else say why, offer to turn on Wi-Fi, or wait
 * for it to connect (then: what to do once it has, WT_*): 0 */
static int pod_net(int then) { return net_or_ask(then, "Podcasts need Wi-Fi for this"); }

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
        if (!pod_net(WT_OK)) return;
        snprintf(pe.path, sizeof pe.path, "%s", e->url);
        if (st.pos_ms >= 60000) {
            char at[16], msg[96];
            fmt_time(at, sizeof at, (int)(st.pos_ms / 1000), 0);
            tr_fill(msg, sizeof msg, "From the start: download it to go on from %s", at, NULL);
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
    /* the feeds last read over an hour ago: read again, with the network
     * already up. By the newest reading among them: by the oldest, one show
     * whose feed has died (its time never moves) had every feed read again
     * at each opening of Podcasts, as it had at every start (podsync_auto). */
    int64_t now = (int64_t)time(NULL), newest = 0;
    for (int i = 0; i < S.pod_n; i++)
        if (S.pod_shows[i].refreshed > newest) newest = S.pod_shows[i].refreshed;
    char why[96];
    /* (as the automatic round it is: a search or a Download asked for right
     * after opening Podcasts goes ahead of it, not behind every show) */
    if (S.pod_n && now - newest > 3600 && S.hooks.radio_ready && S.hooks.radio_ready(why, sizeof why) == 0)
        podsync_refresh_auto();
}

static void pod_open_show(int i)
{
    if (i < 0 || i >= S.pod_n || stack_full()) return;
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
    if (!pod_net(WT_POD_SEARCH)) return;
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
        /* (each one, the first too: it was passed over, the count going from
         * 0 to 1 with nothing said, and the row just stopped showing progress.
         * The card nearly full is its own sentence, not something that
         * couldn't be got: podsync.c's download()) */
        char msg[128];
        if (!strcmp(st.failed_what, "The card is nearly full")) snprintf(msg, sizeof msg, "%s", tr("The card is nearly full"));
        else tr_fill(msg, sizeof msg, "Couldn't get %s", st.failed_what, NULL);
        ui_toast(msg, 3000);
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
        thumbs_reset(COV_POD);                    /* a show's picture may have arrived: the list's read again */
        if (S.now.podcast && !S.pod_cover) load_art();   /* ...and the one playing's */
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
    S.delete_armed = 0;     /* ("Press again to delete" was said in another menu: within 3 s, one press here deleted this playlist) */
    char title[128] = "";
    int on_np = S.now_playing_view;
    /* A menu or quick settings left open under Now Playing (long Back): put
     * it away first, or a second menu went on top and the first, uncovered
     * later, showed one item's title with the other's actions. */
    while (on_np && S.depth > 1 &&
           (S.stack[S.depth - 1].kind == L_CONTEXT || S.stack[S.depth - 1].kind == L_QUICK))
        pop_level(1);
    if (on_np) np_levels_close();
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
    if (l->kind == L_HOME_EDIT && !on_np) {   /* Settings > Interface > Home screen items: move an item */
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
            radio_read();
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
            CTX_ADD(CTX_LYRICS);                    /* (near the top here: the song is playing) */
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
        if (!on_np) CTX_ADD(CTX_LYRICS);
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
static int cover_net(int then) { return net_or_ask(then, "Finding covers needs Wi-Fi"); }

/* An album's cover changed: its rows and Now Playing drawn again */
static void cover_changed(int64_t album)
{
    covers_reset(1);
    flow_forget(1);                               /* (one it had a cover for, too: another found, or removed) */
    if (S.now.active && !S.now.radio && !S.now.podcast && S.now.track.album_id == album) load_art();
}

/* Context menu > Find cover: its matches, shown one at a time */
static void cover_find_open(int64_t album, int from_np)
{
    lib_album_t a;
    if (!S.lib || library_album_info(S.lib, album, &a)) return;
    if (!a.title[0]) { ui_toast("It has no album name to look for", 2500); return; }
    if (stack_full() || !cover_net(WT_NONE)) return;
    if (cf_find(&a)) { ui_toast("Can't look covers up here", 2000); return; }
    S.cf_album = album;
    S.cf_k = 0;
    S.cf_drawn = -1;
    if (!push(L_COVER_FIND, a.title, 0)) { cf_find_close(); return; }   /* (titled with the album: what's looked for) */
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
            if (b.failed && !b.done) snprintf(msg, sizeof msg, "%s", tr(netclock_why("Couldn't reach MusicBrainz or Apple")));
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

/* The queue's row a song is on now, looked for from the row it was on: the
 * rows move up when the song changes, and a menu opened on a row, or a move
 * begun there, is for the song (-1: no longer queued). */
static int queue_row_of(int64_t track, int was)
{
    int n = S.player ? player_queue_len(S.player) : 0;
    if (was >= 0 && was < n && player_queue_at(S.player, was) == track) return was;
    for (int i = (was < n ? was : n) - 1; i >= 0; i--)        /* (moved up: the nearest above first) */
        if (player_queue_at(S.player, i) == track) return i;
    for (int i = was + 1; i < n; i++)
        if (player_queue_at(S.player, i) == track) return i;
    return -1;
}

/* A queue's menu acted on: the row of its song now, which the Queue's
 * selection goes to; -1, and said, when the song has left the queue. */
static int queue_ctx_row(int64_t track, int was)
{
    int at = queue_row_of(track, was);
    level_t *l = &S.stack[S.depth - 1];
    if (at < 0) ui_toast("No longer in the queue", 1500);
    else if (l->kind == L_QUEUE) l->sel = at;
    return at;
}

/* Moving a queued song with the wheel: the selection stays on it when the
 * song playing changes (the move ends if it's the one that started). */
static void queue_move_follow(level_t *l)
{
    if (!S.moving || l->kind != L_QUEUE) return;
    int at = queue_row_of(S.move_track, l->sel);
    if (at < 0) S.moving = 0;
    else l->sel = at;
}

/* Rename playlist: its name as the typing screen takes it, 63 bytes of it at
 * most, cut between two letters (a letter of several bytes was cut through,
 * and showed as a broken one). 1 if it was cut. */
static int rename_load(int64_t id)
{
    char full[256] = "";
    library_playlist_name(S.lib, id, full, sizeof full);
    snprintf(S.entry, sizeof S.entry, "%s", full);
    int n = (int)strlen(S.entry), i = entry_before(S.entry, n);   /* its last letter: all of its bytes there? */
    unsigned char c = (unsigned char)S.entry[i];
    int want = c < 0x80 ? 1 : c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : 2;
    if (n - i < want) S.entry[i] = '\0';
    return strlen(full) > strlen(S.entry);
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
        if (S.stack[S.depth - 1].kind != L_QUEUE && !push_ui(L_QUEUE, "Queue", 0)) break;   /* (one on top already: that one) */
        S.stack[S.depth - 1].back_to_np = from_np;
        break;
    case CTX_GO_ALBUM:
        pop_level(1);
        if (library_album_name(S.lib, album, name, sizeof name) == 0 && push(L_TRACKS, name, album)) {
            level_t *l = &S.stack[S.depth - 1];
            l->from_np = from_np;
            for (int i = 0; i < l->items.n; i++)
                if (l->items.ids[i] == track) l->sel = i;
        }
        break;
    case CTX_GO_ARTIST:
        pop_level(1);
        if (library_artist_name(S.lib, artist, name, sizeof name) == 0 && push(L_ARTIST_ALBUMS, name, artist)) {
            level_t *l = &S.stack[S.depth - 1];
            l->from_np = from_np;
            for (int i = 0; i < l->items.n; i++)
                if (l->items.ids[i] == album) l->sel = i;
        }
        break;
    case CTX_INFO:
        pop_level(1);
        if (push_ui(L_INFO, "Info", track)) S.stack[S.depth - 1].back_to_np = from_np;
        break;
    case CTX_LYRICS:
        pop_level(1);
        if (from_np) S.now_playing_view = 1;        /* (under the lyrics, and there when they're put away) */
        lyrics_open(track, from_np);
        break;
    case CTX_FIND_COVER:
    case CTX_FIND_ANOTHER:
        pop_level(1);
        {
            /* From Now Playing, and it couldn't start (no network, no album
             * name to look for): Now Playing again, not the list under it;
             * asked to turn Wi-Fi on instead: Now Playing after the answer. */
            int depth = S.depth;
            cover_find_open(album, from_np);
            if (from_np && S.depth == depth) S.now_playing_view = 1;
            else if (from_np && S.stack[S.depth - 1].kind == L_WIFI_ASK) S.stack[S.depth - 1].back_to_np = 1;
        }
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
        if (S.player && (qidx = queue_ctx_row(track, qidx)) >= 0) { player_queue_jump(S.player, qidx); S.now_playing_view = 1; }
        break;
    case CTX_Q_MOVE:
        pop_level(1);
        if (queue_ctx_row(track, qidx) < 0) break;
        S.moving = 1;
        S.move_track = track;
        break;
    case CTX_Q_REMOVE:
        pop_level(1);
        if (S.player && (qidx = queue_ctx_row(track, qidx)) >= 0) player_queue_remove(S.player, qidx);
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
        /* (the list of playlists still open from the last time, on top: closed;
         * one further down is no longer for these songs: pick_gen) */
        if (S.stack[S.depth - 1].kind == L_PICK_PLAYLIST) pop_level(1);
        lib_list_t all;
        library_list(S.lib, LIB_PLAYLISTS, 0, &all);           /* yours only */
        S.pick_pls.ids = malloc((size_t)(all.n ? all.n : 1) * sizeof *S.pick_pls.ids);
        S.pick_pls.n = 0;
        for (int i = 0; S.pick_pls.ids && i < all.n; i++)
            if (library_playlist_writable(S.lib, all.ids[i])) S.pick_pls.ids[S.pick_pls.n++] = all.ids[i];
        library_list_free(&all);
        if (push_ui(L_PICK_PLAYLIST, "Add to playlist", ++S.pick_gen)) S.stack[S.depth - 1].back_to_np = from_np;
        break;
    }
    case CTX_PL_REMOVE: {
        pop_level(1);
        level_t *l = &S.stack[S.depth - 1];
        if (l->kind == L_PLAYLIST_TRACKS && library_playlist_remove(S.lib, l->parent, S.ctx_plpos) == 0) {
            library_list_free(&l->items);
            level_load(l);
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
        radio_read();
        /* (the one playing by itself, its search left: one of theirs from now) */
        if (S.radio_from == -1 && radio_to_yours()) S.radio_from = 0;
        break;
    }
    case CTX_RADIO_REMOVE: {
        if (!press_twice(&S.rd_del_armed, "Press again to remove it")) break;
        pop_level(0);
        int r = S.hooks.radio_remove ? S.hooks.radio_remove(S.rd_ctx.url) : -1;
        ui_toast(r == 0 ? "Removed from your stations" : r == 1 ? "Not in your stations" : "Couldn't save fathom/radio.txt", 2000);
        radio_read();
        level_t *l = &S.stack[S.depth - 1];
        if (l->kind == L_RADIO && !S.now_playing_view) {
            int n = list_count(l);
            if (l->sel >= n) l->sel = n - 1;
        }
        break;
    }
    case CTX_POD_SPEED: pod_speed_next(); break;    /* the next speed; the menu stays (Play again: the next) */
    case CTX_POD_CHAPTERS: {
        int back = S.stack[S.depth - 1].back_to_np;
        pop_level(1);
        if (!push_ui(L_POD_CHAPTERS, "Chapters", 0)) break;
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
        if (!pod_net(WT_NONE)) break;   /* (OK on the row under the menu is something else: nothing kept) */
        podsync_refresh(u);
        ui_toast("Checking for new episodes", 1500);
        break;
    }
    case CTX_POD_UNSUB: {
        if (!press_twice(&S.pod_del_armed, "Press again to unsubscribe (its downloads go too)")) break;
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
            if (!pod_net(WT_NONE)) break;
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
        pop_level(1);                              /* (a level free again: the push below has room) */
        rename_load(S.rename_id);
        S.entry_back = 0;                          /* (the cursor at the end, wherever the last typing left it) */
        S.entry_ch = 0;
        push_ui(L_RENAME, "Rename playlist", 0);
        break;
    case CTX_PL_DELETE: {
        if (!press_twice(&S.delete_armed, "Press again to delete the playlist")) break;
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
            level_load(l);
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
    case ST_BALANCE: case ST_WIDTH: case ST_VOL_LIMIT:
        if (st != ST_VOL_LIMIT && S.pure) { ui_toast("Bypassed in Pure mode", 1500); break; }
        if (S.hooks.sound_adjust && S.hooks.sound_adjust(sound_of(st), 0) == 0)
            S.editing = !S.editing;
        break;
    case ST_SHUFFLE: if (S.player) player_set_shuffle(S.player, !player_shuffle(S.player)); break;
    case ST_REPEAT: if (S.player) player_set_repeat(S.player, (repeat_t)((player_repeat(S.player) + 1) % 3)); break;
    case ST_SLEEP:
        if (!S.player) break;
        player_set_sleep(S.player, next_of(SLEEPS, COUNT_OF(SLEEPS), player_sleep_minutes(S.player), 0));   /* (SLEEPS[0]: off) */
        break;
    case ST_STOP_AFTER: if (S.player) player_set_stop_after(S.player, !player_stop_after(S.player)); break;
    case ST_DIM: setting_flip("display.dim", UI_DIM_DEFAULT); break;
    case ST_ON_CHARGING: setting_flip("display.on_charging", UI_ON_CHARGING_DEFAULT); break;
    case ST_COVERS: settings_set_int("display.covers", (settings_get_int("display.covers", UI_COVERS_DEFAULT) + 1) & 3); break;
    case ST_ALBUM_FLOW: {
        int v = settings_get_int("display.album_flow", UI_ALBUM_FLOW_DEFAULT);
        settings_set_int("display.album_flow", v >= 0 && v < 2 ? v + 1 : 0);
        break;
    }
    case ST_THEME: push_ui(L_THEMES, "Theme", 0); break;
    case ST_PACKS: push_ui(L_PACKS, "Language packs", 0); break;
    case ST_HOME_SCREEN:
        push_ui(L_HOME_EDIT, "Home screen items", 0);
        ui_toast("<Play> shows or hides. <Menu>: Move", 2500);
        break;
    case ST_ALBUM_ARTISTS:
        settings_set_int("display.album_artists", !album_artists_on());
        library_set_album_artists(S.lib, album_artists_on());
        break;
    case ST_DAC_FILTER: if (S.hooks.dac_filter_next) S.hooks.dac_filter_next(); break;
    case ST_EQ: {                          /* the profiles, for the output playing now */
        if (S.pure) { ui_toast("Bypassed in Pure mode", 1500); break; }
        char t[80];
        tr_fill(t, sizeof t, "EQ: %s", S.eq_out[0] ? S.eq_out : tr("this output"), NULL);
        push(L_EQ, t, 0);
        break;
    }
    case ST_LEVELLING: {
        if (S.pure) { ui_toast("Bypassed in Pure mode", 1500); break; }
        int v = settings_get_int("sound.levelling", LEVEL_OFF);
        if (v < LEVEL_OFF || v > LEVEL_ALBUM) v = LEVEL_OFF;      /* (a value edited by hand: shown as Off, and Off it is) */
        v = (v + 1) % 3;
        settings_set_int("sound.levelling", v);
        if (S.player) player_set_levelling(S.player, (levelling_t)v);
        int done = 0, total = 0;
        if (v == LEVEL_TRACK && levelling_left(&done, &total) >= 0) {   /* (just turned on) */
            char msg[200];
            char d[16], n[16];
            snprintf(d, sizeof d, "%d", done);
            snprintf(n, sizeof n, "%d", total);
            tr_fill(msg, sizeof msg, "%s of %s songs ready. The rest are measured while the player is charging"
                                     " and not playing, which can take hours.", d, n);
            toast_show(msg, 7000, S.f->sub);
        }
        break;
    }
    case ST_PAUSE_UNPLUG: setting_flip("playback.pause_on_unplug", UI_PAUSE_ON_UNPLUG_DEFAULT); break;
    case ST_SHUF_BY: case ST_SHUF_FRESH: case ST_SHUF_INTERLUDES: case ST_SHUF_WORKS: case ST_SHUF_SPOKEN: {
        static const char *const key[] = {"shuffle.albums", "shuffle.freshness", "shuffle.skip_interludes",
                                          "shuffle.keep_works", "shuffle.exclude_spoken"};
        static const int def[] = {UI_SHUF_ALBUMS_DEFAULT, UI_SHUF_FRESH_DEFAULT, UI_SHUF_INTERLUDES_DEFAULT,
                                  UI_SHUF_WORKS_DEFAULT, UI_EXCLUDE_SPOKEN_DEFAULT};
        int k = (int)(st - ST_SHUF_BY);
        setting_flip(key[k], def[k]);
        shuffle_prefs_apply();
        if (S.player && player_shuffle(S.player)) ui_toast("From the next shuffle", 1500);
        break;
    }
    case ST_AUTO_OFF:
        settings_set_int("power.auto_off", next_of(AUTO_OFFS, COUNT_OF(AUTO_OFFS), settings_get_int("power.auto_off", UI_AUTO_OFF_DEFAULT), 1));
        break;
    case ST_POD_AUTO: setting_flip("podcasts.auto", 1); break;
    case ST_POD_KEEP:
        settings_set_int("podcasts.keep", next_of(POD_KEEPS, COUNT_OF(POD_KEEPS), settings_get_int("podcasts.keep", 2), 0));
        break;
    case ST_POD_CLEAN:
        settings_set_int("podcasts.clean_days", next_of(POD_CLEANS, COUNT_OF(POD_CLEANS), settings_get_int("podcasts.clean_days", 7), 0));
        break;
    case ST_POD_SPEED: pod_speed_next(); break;
    case ST_RESCAN: if (S.hooks.rescan) S.hooks.rescan(); ui_show_scan(); break;
    case ST_FIND_COVERS: covers_find_all(); break;
    case ST_N_UNREAD: push_ui(L_UNREAD, "Files not read", 0); break;
    case ST_USB_DRIVE: if (S.hooks.usb_drive) S.hooks.usb_drive(); break;
    case ST_USB_COMPUTER: {      /* Ask, USB drive, USB DAC, Just charge */
        static const int order[] = {UI_USB_ASK, UI_USB_DRIVE, UI_USB_DAC, UI_USB_CHARGE};
        settings_set_int("usb.computer", next_of(order, COUNT_OF(order), settings_get_int("usb.computer", UI_USB_ASK), 1));
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

/* ---- About > Clock, set by hand: Date and time, on a screen of its own ----
 * The date and time on one row, 2026 Nov 22 18:05, in the time zone below it
 * (its offset from UTC over the row). The wheel goes from field to field, the
 * zone last; Play on one changes it (arrows over and under it): the wheel then
 * changes its value, Play (or Back) keeps it. Back leaves, setting the clock
 * to what the row shows if a field was changed, and keeping the zone if it
 * was. A zone chosen before the time is changed shows the same moment in it;
 * after, the time typed is taken as that zone's. The H2's clock stays in UTC
 * (clock.tz only changes how it's shown); seconds start at :00. NTP or an
 * HTTPS answer sets it again when Wi-Fi can (device/main.c). */
enum { CK_ZONE = DT_FIELDS };          /* the places the wheel goes: the fields, then the zone */
static struct {
    int cur, editing;
    int zone, zone0;                   /* the zone shown, and as it was (clock.tz) */
    int typed;                         /* a field of the date or time changed */
    lv_obj_t *field[DT_FIELDS], *val[DT_FIELDS], *zfield, *zlbl, *offs, *up, *down, *hint;
} CK;

/* The time zone times are shown in (About > Clock: clock.tz; UTC unless chosen) */
int ui_zone(void) { return tz_find(settings_get("clock.tz", "UTC")); }

/* A month's short name (1-12), translated: for every date the interface or
 * the platform writes (the battery's days, About's clock, setting it) */
const char *ui_month(int mon)
{
    static const char *const MON[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    return tr(MON[(mon + 11) % 12]);
}

void ui_clock_text(int64_t now, char *out, size_t cap)
{
    int z = ui_zone();
    dt_t t;
    dt_split(tz_to_local(z, now), &t);
    char d[8], date[48];
    snprintf(d, sizeof d, "%d", t.day);
    snprintf(date, sizeof date, "%s %s", d, ui_month(t.mon));     /* (as the battery's days) */
    snprintf(out, cap, "%s %d, %02d:%02d%s", date, t.year, t.hour, t.min, z ? "" : " UTC");
}

static void clock_close(void) { overlay_close(&S.clk_ov); }

/* The clock now, as it reads in the zone shown (or 2026-01-01 while it isn't set) */
static void clock_now(void)
{
    int64_t now = (int64_t)time(NULL);
    dt_from_time(netclock_unset_at(now) ? 0 : tz_to_local(CK.zone, now), &S.clk);
}

/* The fields' values */
static void clock_update_values(void)
{
    const dt_t *d = &S.clk;
    char v[16];
    snprintf(v, sizeof v, "%d", d->year);
    lv_label_set_text(CK.val[DT_YEAR], v);
    lv_label_set_text(CK.val[DT_MONTH], ui_month(d->mon));
    snprintf(v, sizeof v, "%d", d->day);
    lv_label_set_text(CK.val[DT_DAY], v);
    snprintf(v, sizeof v, "%02d", d->hour);
    lv_label_set_text(CK.val[DT_HOUR], v);
    snprintf(v, sizeof v, "%02d", d->min);
    lv_label_set_text(CK.val[DT_MIN], v);
}

/* ...the zone, which one is chosen, and whether it's being changed */
static void clock_update(void)
{
    if (!S.clk_ov) return;
    const theme_t *t = T();
    clock_update_values();
    lv_label_set_text(CK.zlbl, TZ_ZONES[CK.zone].name);
    char o[16];
    tz_offset_text(CK.zone, tz_to_utc(CK.zone, dt_to_time(&S.clk)), o, sizeof o);
    lv_label_set_text(CK.offs, o);
    for (int i = 0; i <= CK_ZONE; i++) {
        lv_obj_t *b = i < CK_ZONE ? CK.field[i] : CK.zfield, *l = i < CK_ZONE ? CK.val[i] : CK.zlbl;
        int on = CK.cur == i;
        fill(b, on ? t->select_bg : t->surface);
        lv_obj_set_style_text_color(l, C(on ? t->select_fg : t->text), 0);
    }
    lv_obj_update_layout(S.clk_ov);
    show(CK.up, CK.editing);
    show(CK.down, CK.editing);
    if (CK.editing) {
        lv_obj_t *b = CK.cur < CK_ZONE ? CK.field[CK.cur] : CK.zfield;
        lv_obj_align_to(CK.up, b, LV_ALIGN_OUT_TOP_MID, 0, -2);
        lv_obj_align_to(CK.down, b, LV_ALIGN_OUT_BOTTOM_MID, 0, 2);
    }
    lv_label_set_text(CK.hint, tr(CK.editing ? "Turn to change, <Play> to keep it" : "Turn to choose, <Play> to change. <Back> when done"));
}

static void clock_open(void)
{
    clock_close();
    CK.cur = DT_YEAR;
    CK.editing = CK.typed = 0;
    CK.zone = CK.zone0 = ui_zone();
    clock_now();
    const theme_t *t = T();
    int radius = t->corner == THEME_CORNER_ROUND ? (t->radius < 8 ? t->radius : 8) : 0;
    S.clk_ov = overlay_new();
    lv_obj_t *title = lv_label_create(S.clk_ov);
    style_label(title, S.f->title, t->text);
    lv_label_set_text(title, tr("Date and time"));
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);
    CK.offs = lv_label_create(S.clk_ov);                 /* the zone's offset: UTC+1 */
    style_label(CK.offs, S.f->meta, t->text_muted);
    lv_obj_set_width(CK.offs, UI_W);
    lv_obj_set_style_text_align(CK.offs, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(CK.offs, LV_ALIGN_TOP_MID, 0, 42);
    /* the row: year month day  hours:minutes */
    lv_obj_t *row = box(S.clk_ov, 0, 84, UI_W, 44);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 5, 0);
    for (int i = 0; i < DT_FIELDS; i++) {
        if (i == DT_HOUR) box(row, 0, 0, 6, 1);       /* (a wider gap between the date and the time) */
        if (i == DT_MIN) {                             /* hours:minutes */
            lv_obj_t *colon = lv_label_create(row);
            style_label(colon, S.f->title, t->text);
            lv_label_set_text(colon, ":");
        }
        CK.field[i] = box(row, 0, 0, LV_SIZE_CONTENT, 40);
        lv_obj_set_style_pad_hor(CK.field[i], 7, 0);
        lv_obj_set_style_radius(CK.field[i], radius, 0);
        CK.val[i] = lv_label_create(CK.field[i]);
        style_label(CK.val[i], S.f->title, t->text);
        lv_obj_center(CK.val[i]);
    }
    /* a wide font (Pixel's): closer together, then smaller, to fit the screen */
    for (int pass = 0; pass < 2; pass++) {
        clock_update_values();
        lv_obj_update_layout(row);
        int32_t w = 0;
        for (uint32_t c = 0; c < lv_obj_get_child_count(row); c++) w += lv_obj_get_width(lv_obj_get_child(row, (int32_t)c));
        w += (int32_t)(lv_obj_get_child_count(row) - 1) * lv_obj_get_style_pad_column(row, 0);
        if (w <= UI_W - 12) break;
        lv_obj_set_style_pad_column(row, 2, 0);
        for (int i = 0; i < DT_FIELDS; i++) {
            lv_obj_set_style_pad_hor(CK.field[i], 4, 0);
            if (pass) style_label(CK.val[i], S.f->body, t->text);
        }
    }
    /* the time zone */
    CK.zfield = box(S.clk_ov, 0, 0, LV_SIZE_CONTENT, 38);
    lv_obj_set_style_min_width(CK.zfield, 180, 0);
    lv_obj_set_style_pad_hor(CK.zfield, 14, 0);
    lv_obj_set_style_radius(CK.zfield, radius, 0);
    lv_obj_align(CK.zfield, LV_ALIGN_TOP_MID, 0, 148);
    CK.zlbl = lv_label_create(CK.zfield);
    style_label(CK.zlbl, S.f->body, t->text);
    lv_obj_center(CK.zlbl);
    CK.up = lv_label_create(S.clk_ov);
    CK.down = lv_label_create(S.clk_ov);
    style_label(CK.up, &lv_font_montserrat_14, t->select_bg);
    style_label(CK.down, &lv_font_montserrat_14, t->select_bg);
    lv_label_set_text(CK.up, LV_SYMBOL_UP);
    lv_label_set_text(CK.down, LV_SYMBOL_DOWN);
    CK.hint = lv_label_create(S.clk_ov);
    style_label(CK.hint, S.f->meta, t->text_muted);
    lv_obj_set_width(CK.hint, UI_W - 24);
    lv_obj_set_style_text_align(CK.hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(CK.hint, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_move_background(S.clk_ov);                 /* toasts and the volume bar stay over it */
    clock_update();
}

/* Back: the clock set to what's shown if a field was changed, the zone kept if it was */
static void clock_leave(void)
{
    int zoned = CK.zone != CK.zone0;
    if (CK.typed) {
        if (!S.hooks.clock_set || S.hooks.clock_set(tz_to_utc(CK.zone, dt_to_time(&S.clk)))) {
            ui_toast("Couldn't set the clock", 2500);
            return;                                    /* (still here: Back again to leave it) */
        }
    }
    if (zoned) {
        settings_set("clock.tz", TZ_ZONES[CK.zone].name);
        settings_save();
    }
    clock_close();
    S.about_at = 0;                                    /* (About shows the new time) */
    if (CK.typed) ui_toast("Clock set", 1500);
    else if (zoned) ui_toast("Time zone set", 1500);
    render(R_CONTENT);
    CK.typed = 0;
    CK.zone0 = CK.zone;
}

/* A key while it's up: 1 if it was the clock's. */
static int clock_key(ui_key_t k)
{
    switch (k) {
    case KEYS_PLAYERS:
        return 0;                                      /* the music's and the screen's, as anywhere */
    case UI_KEY_UP:
    case UI_KEY_DOWN: {
        int d = k == UI_KEY_DOWN ? 1 : -1;
        if (!CK.editing) {
            CK.cur = CK.cur + d < 0 ? 0 : CK.cur + d > CK_ZONE ? CK_ZONE : CK.cur + d;
        } else if (CK.cur < CK_ZONE) {
            dt_adjust(&S.clk, CK.cur, d);
            CK.typed = 1;
        } else {
            int n = tz_count();
            CK.zone = (CK.zone + d + n) % n;
            if (!CK.typed) clock_now();                /* (the same moment, in that zone) */
        }
        break;
    }
    case UI_KEY_OK:
        CK.editing = !CK.editing;
        break;
    case UI_KEY_BACK:
    case UI_KEY_BACK_LONG:
        if (CK.editing) {
            CK.editing = 0;                            /* (kept, as Play) */
            break;
        }
        clock_leave();
        return 1;
    default: break;
    }
    clock_update();
    return 1;
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
        case H_PACKS: push_ui(L_PACKS, "Language packs", 0); break;   /* (its first row: what the library needs) */
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
            entry_empty();
            search_run();
            push_ui(L_SEARCH, "Search", 0);
            break;
        case H_FAVOURITES: push_ui(L_FAVOURITES, "Favourites", 0); break;
        case H_RECENT_ADDED: push_ui(L_RECENT_ADDED, "Recently added", 0); break;
        case H_HISTORY: push_ui(L_HISTORY, "History", 0); break;
        case H_RADIO:   /* the station playing selected, else your first */
            radio_read();
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
        if (!net_or_ask(WT_OK, "")) break;   /* (one of your stations: only the device's own reason is said) */
        if (rd_on_air(&S.radio[i])) { S.now_playing_view = 1; break; }
        player_radio_play(S.player, S.radio, S.radio_n, i);
        S.radio_from = 0;
        S.now_playing_view = 1;
        break;
    }
    case L_RDIR:
        switch (l->sel) {
        case RD_MENU_SEARCH:
            if (stack_full()) break;
            entry_empty();
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
            if (push_ui(L_RDIR_COUNTRIES, "Countries", 0)) rd_ask(NULL);
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
        if (l->sel >= l->items.n) break;       /* (no row: -1 is that, and a smart playlist's number) */
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
                tr_fill(t, sizeof t, "%s, signal %s dBm", S.wifi_st.ip, dbm);
                ui_toast(t, 2500);
            }
        } else if (row == WR_SEARCH) {
            S.hooks.wifi_do(UI_WIFI_SCAN, NULL, NULL);
        } else if (net < S.wifi_n) {
            const ui_wifi_net_t *w = &S.wifi[net];
            if (w->connected) ui_toast("Connected", 1000);
            else if (w->saved || !w->secured) S.hooks.wifi_do(UI_WIFI_CONNECT, w->ssid, "");
            else if (!stack_full()) {                      /* a new network: its password */
                snprintf(S.wifi_pass_ssid, sizeof S.wifi_pass_ssid, "%s", w->ssid);
                entry_keep();
                char t[80];
                tr_fill(t, sizeof t, "Password for %s", w->ssid, NULL);
                push(L_WIFI_PASS, t, 0);
                ui_toast("<Play> adds a letter, <Menu>: a A 0 !", 2500);
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
        S.bt_back_to = S.bt_back_gone = 0;
        if (l->sel == 0) S.hooks.bt_do(UI_BT_TOGGLE, NULL);
        else if (l->sel == 1) S.hooks.bt_do(UI_BT_SCAN, NULL);
        else if (l->sel == 2) {
            S.hooks.bt_do(UI_BT_RANGE, NULL);
            S.bt_back_to = l->sel;                 /* Bluetooth starts again: its rows go, and come back */
            ui_toast(settings_get_int("bluetooth.range", UI_BT_RANGE_DEFAULT) ? "Longer range: lower bitrate, no aptX. Reconnecting"
                                                            : "Best sound. Reconnecting", 2500);
        }
        else if (l->sel - BT_DEV_ROW < S.bt_n) S.hooks.bt_do(UI_BT_PRESS, S.bt[l->sel - BT_DEV_ROW].addr);
        break;
    case L_SETTINGS: {
        static const list_kind_t sub[] = {L_SOUND, L_PLAYBACK, L_INTERFACE, L_WIFI, L_BLUETOOTH, L_POD_SETTINGS, L_DISPLAY,
                                          L_LIBRARY, L_USB, L_SHARE, L_POWER, L_SSH, L_ABOUT};   /* (as SETTINGS) */
        push_ui(sub[l->sel], SETTINGS[l->sel], 0);
        if (sub[l->sel] == L_WIFI && S.hooks.wifi_do) S.hooks.wifi_do(UI_WIFI_SCAN, NULL, NULL);   /* fresh networks */
        break;
    }
    case L_ABOUT:
        if (about_row(l->sel) == AB_INFO && !strcmp(S.about_lbl[l->sel], "Clock") && S.hooks.clock_set) {
            clock_open();
        } else if (about_row(l->sel) == AB_WHATSNEW) {
            whats_new_open();
        } else if (about_row(l->sel) == AB_TIPS) {
            points_open(1);
        } else if (about_row(l->sel) == AB_CHECK) {    /* Check for updates: from here, or */
            if (stack_full() || wifi_ask_if_off(WT_OK)) break;
            S.hooks.update_do(UI_UPD_CHECK);
            push_ui(L_UPDATE, "Updates", 0);
        } else if (about_row(l->sel) == AB_AUTO) {     /* by itself (on by default) */
            int on = !settings_get_int("update.auto", UI_UPDATE_AUTO_DEFAULT);
            settings_set_int("update.auto", on);
            settings_save();
            if (on) ui_toast("Three times a day, while the player rests on Wi-Fi", 3000);
        } else if (about_row(l->sel) == AB_UNINSTALL) {
            if (stack_full()) break;
            uninstall_look();
            if (S.uni.state != UI_UNI_PENDING) S.hooks.uninstall_do(UI_UNI_CHECK);
            S.uni_armed = 0;
            push_ui(L_UNINSTALL, "Uninstall Fathom", 0);
            ui_uninstall_changed();
        } else if (about_row(l->sel) == AB_RESET) {   /* Reset all settings: press twice */
            uint32_t now = lv_tick_get();
            if (pressed_again(&S.reset_armed, now)) {
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
                press_arm(&S.reset_armed, now, "Press again to reset all settings");
            }
        }
        break;
    case L_UPDATE: update_ok(l->sel); break;
    case L_UNINSTALL: uninstall_ok(l->sel); break;
    case L_PACKS: packs_ok(l->sel); break;
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
    case L_UNREAD: {                               /* where the file is, in full */
        char path[1024];
        if (S.lib && S.lib_unread > 0 && library_unread(S.lib, l->sel, path, sizeof path) == 0)
            toast_show(!strncmp(path, "/mnt/sd/", 8) ? path + 8 : path, 6000, S.f->body);
        break;
    }
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
            if (stack_full()) break;
            S.eqe.band[n] = (eq_band_t){EQ_PEAK, 1000, 0, 1};
            S.eqe.n++;
            eqe_changed(1);
            char t[80], hz[24];
            fmt_hz(hz, sizeof hz, 1000);
            snprintf(t, sizeof t, "%s %s", tr(EQ_TYPE_NAME[EQ_PEAK]), hz);
            push(L_EQ_BAND, t, n);
        } else {                                                 /* delete: press twice */
            uint32_t now = lv_tick_get();
            if (pressed_again(&S.eqe_del_armed, now)) {
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
                press_arm(&S.eqe_del_armed, now, "Press again to delete this EQ");
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
            if (stack_full()) break;
            entry_empty();
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
    case SETTING_PAGES:
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
            else if (kind == 2 && S.player) {    /* the songs found, from this one */
                int at = 0;
                for (int i = 0; i < S.found.n_songs; i++)
                    if (S.found.songs[i] == rid) { at = i; break; }
                player_play(S.player, S.found.songs, S.found.n_songs, at);
                S.now_playing_view = 1;
            }
        }
        break;
    case L_PICK_PLAYLIST: {
        int64_t pl;
        if (l->parent != S.pick_gen) {         /* left open while another was used: its songs are gone */
            pop();
            ui_toast("Choose Add to playlist again", 2000);
            break;
        }
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
            tr_fill(msg, sizeof msg, "Added to %s", nm, NULL);
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

/* The letter row i sorts under ("The Beatles" under B, digits under #): 1 to
 * 26 for A to Z, 0 for "#". */
static int row_initial(const level_t *l, int i)
{
    char main[256], meta[32];
    int playing;
    list_item(l, i, main, sizeof main, meta, sizeof meta, &playing);
    const char *p = main;     /* as the library sorts: leading punctuation, then "the ", skipped */
    while (*p && (unsigned char)*p < 0x80 && !isalnum((unsigned char)*p)) p++;
    if (!*p) p = main;
    if (l->kind != L_SONGS && l->kind != L_FAVOURITES && !strncasecmp(p, "the ", 4)) p += 4;
    char fold[8];
    path_loose_key(p, 1 << 30, fold, sizeof fold);     /* first letter, accents folded */
    return fold[0] >= 'a' && fold[0] <= 'z' ? fold[0] - 'a' + 1 : 0;
}

/* The first row of the letter row i is under, and the first of the next
 * letter (the last row, if there is none): the rows under a letter are
 * together, so a few rows are looked at, not all of them. */
static int letter_first(const level_t *l, int i)
{
    int a = row_initial(l, i), lo = i, hi, step = 1;
    while ((hi = lo - step) >= 0 && row_initial(l, hi) == a) { lo = hi; step *= 2; }
    if (hi < -1) hi = -1;
    while (lo - hi > 1) {
        int mid = (lo + hi) / 2;
        if (row_initial(l, mid) == a) lo = mid; else hi = mid;
    }
    return lo;
}

static int letter_next(const level_t *l, int i)
{
    int n = list_count(l), a = row_initial(l, i), lo = i, hi, step = 1;
    while ((hi = lo + step) < n && row_initial(l, hi) == a) { lo = hi; step *= 2; }
    if (hi > n) hi = n;
    while (hi - lo > 1) {
        int mid = (lo + hi) / 2;
        if (row_initial(l, mid) == a) lo = mid; else hi = mid;
    }
    return hi < n ? hi : n - 1;
}

/* The letter the selected row sorts under, shown for a moment over the list. */
static void show_initial(const level_t *l)
{
    if (!alphabetical(l->kind) || l->sel >= list_count(l)) return;
    int a = row_initial(l, l->sel);
    char letter[4] = "#";
    if (a) { letter[0] = (char)('A' + a - 1); letter[1] = 0; }

    if (!S.letter) {
        S.letter = box(lv_layer_top(), 0, 0, 72, 72);
        S.letter_lbl = lv_label_create(S.letter);
        lv_obj_center(S.letter_lbl);
    }
    popup_style(S.letter, S.letter_lbl, S.f->title);
    lv_label_set_text(S.letter_lbl, letter);
    lv_obj_center(S.letter_lbl);
    lv_obj_align(S.letter, LV_ALIGN_CENTER, 0, 10);
    lv_obj_remove_flag(S.letter, LV_OBJ_FLAG_HIDDEN);
    popup_arm(&S.letter_timer, initial_hide, 700);
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
    /* (no row under a cover flow: they are another list's, covered: one scrolled on there unseen) */
    const level_t *top = &S.stack[S.depth - 1];
    int slot = on && !S.now_playing_view && !flow_on(top) && list_count(top) > 0 ? slot_under_pill() : -1;
    if (slot >= 0 && slot < SLOTS && !S.line_on[slot]) lv_label_set_long_mode(S.row_lbl[slot], LV_LABEL_LONG_SCROLL);
    for (int r = 0; r < SLOTS; r++)   /* album rows: title and artist as one line */
        if (S.line_on[r]) line_scroll(r, r == slot);
}

int ui_test_row_scrolls(void)
{
    for (int r = 0; r < SLOTS; r++)
        if (lv_label_get_long_mode(S.row_lbl[r]) == LV_LABEL_LONG_SCROLL || lv_anim_get(S.row_line[r], line_scroll_cb)) return 1;
    return 0;
}

static void marquee_fire(lv_timer_t *t)
{
    if (!S.asleep) marquee_set(1);   /* (the screen off: nothing scrolls unseen) */
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

void ui_covers_changed(void)
{
    /* covers made since they were asked for (a scan finding them in the
     * background): the lists' thumbnails forgotten and drawn again, and Now
     * Playing's cover, if it had none */
    covers_reset(1);
    if (S.now_playing_view && S.player && !S.now.radio && !S.now.podcast) load_art();
    render(R_CONTENT);
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
        level_load(l);
        int sel = l->sel < l->items.n ? l->sel : (l->items.n ? l->items.n - 1 : 0);
        for (int i = 0; i < l->items.n; i++)
            if (l->items.ids[i] == was) { sel = i; break; }
        l->sel = sel;
        if (l->top > sel) l->top = sel;
    }
    if (level_find(L_SEARCH) >= 0) search_run();   /* (Search wherever it is: under a menu or an album it found, too) */
    if (level_find(L_LIBRARY) >= 0) lib_counts_load();
    render(R_SNAP);
}

void ui_wifi_changed(void)
{
    ui_wifi_status_t st = {0};
    char busy[32];
    int on = S.hooks.wifi_state ? S.hooks.wifi_state(busy, sizeof busy, &st) : 0;
    /* the header's icon: hidden with Wi-Fi off, dimmed while it's on with no
     * network (it showed only while one was being joined: Wi-Fi on and out of
     * range looked off), full once connected */
    int link = !on ? 0 : st.state == UI_WIFI_CONNECTED ? 2 : 1;
    if (link != S.wifi_link) {
        S.wifi_link = link;
        if (S.h_right) render_header_status();
    }
    if (link == 2) wifi_then_run(1);
    page_changed(L_WIFI, PC_QUICK);
}

void ui_share_changed(void) { page_changed(L_SHARE, 0); }

void ui_ssh_changed(void) { page_changed(L_SSH, 0); }

void ui_bt_changed(void) { page_changed(L_BLUETOOTH, PC_QUICK); }

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
        /* Nothing more is drawn from here: what the panel holds is the screen
         * as it was. (The covers given back first, their pictures hidden, were
         * drawn so once more: a cover flow's panel gone, the rows under it,
         * which are another list's, were what the panel held, and what showed
         * for a moment when the screen came back on. Seen in a screenshot of
         * the owner's H2, its screen off in Albums: Home's rows under
         * "Albums", 2026-11-25.) */
        lv_display_enable_invalidation(NULL, false);
        covers_free();                        /* (read again when the screen's back) */
        flow_free();
        render_header_status();               /* (the charging pulse stops) */
        marquee_set(0);
        if (S.marquee_timer) lv_timer_pause(S.marquee_timer);
        lyrics_awake(0);
        return;
    }
    lv_display_enable_invalidation(NULL, true);
    render_mode_t m = S.wake_mode;
    S.wake_mode = R_CONTENT;
    render(m);
    lyrics_awake(1);
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

static void key_do(ui_key_t k);

/* Done on a playlist's name too long for the typing screen, with nothing
 * changed in the part it shows: no rename (it saved the cut name). */
static int rename_same(void)
{
    char typed[sizeof S.entry];
    memcpy(typed, S.entry, sizeof typed);
    int cut = rename_load(S.rename_id);
    int same = cut && !strcmp(typed, S.entry);
    memcpy(S.entry, typed, sizeof S.entry);
    return same;
}

void ui_key(ui_key_t k)
{
    long t0 = ui_timing() ? (long)clock_us() : 0;
    key_do(k);
    if (t0) {
        long us = (long)clock_us() - t0;
        ui_took("a key", us);
        if (us > 15000) fprintf(stderr, "flow: a key took %ld ms\n", us / 1000);
    }
}

void ui_media_key(ui_key_t k)
{
    S.media_key = 1;
    ui_key(k);
    S.media_key = 0;
}

static void key_do(ui_key_t k)
{
    int had_help = S.help_shown;
    help_hide();
    /* (a media key with the screen off: no text set scrolling, which went on
     * 60 times a second until the screen came on; waking arms it) */
    if (!S.asleep) marquee_arm();
    if (k == UI_KEY_OK || k == UI_KEY_BACK || k == UI_KEY_BACK_LONG) capture();
    level_t *l = &S.stack[S.depth - 1];
    player_t *pl = S.player;
    if (S.upd_ov && update_overlay_key(k)) return;
    if (S.scan_ov && scan_overlay_key(k)) return;
    if (S.wn_ov && whats_new_key(k)) return;
    if (S.lyr_ov && lyrics_key(k)) return;
    if (S.uni_ov && uninstall_overlay_key(k)) return;
    if (S.clk_ov && clock_key(k)) return;
    /* Typing: <Prev> and <Next> move the cursor through what's typed (not the
     * track: a letter wrong at the start no longer means deleting the rest).
     * With the screen off, and from the headphones' buttons, they are the
     * track's as everywhere: nobody is typing with those. */
    if (!S.asleep && !S.media_key && !S.now_playing_view && typing_on(l) && (k == UI_KEY_PREV || k == UI_KEY_NEXT)) {
        int cur = entry_cur();
        int len = (int)strlen(S.entry);
        if (k == UI_KEY_PREV && cur > 0) S.entry_back = len - entry_before(S.entry, cur);
        if (k == UI_KEY_NEXT && S.entry_back > 0) S.entry_back = len - entry_after(S.entry, cur);
        S.ok_added = 0;
        render(R_CONTENT);
        return;
    }
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
                player_now_t at;        /* (where the song is now: S.now is as last drawn, up to a second ago) */
                player_now(pl, &at);
                player_seek_by(pl, S.scrub_s * 1000 - (int)at.pos_ms);
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
    if (typing_on(l)) {                               /* typing with the wheel */
        const char *letters = entry_letters(l->kind);
        int nl = (int)strlen(letters);
        if (S.entry_ch < 0 || S.entry_ch > nl) S.entry_ch = nl;   /* (never a letter this screen hasn't) */
        size_t len = strlen(S.entry);
        size_t cur = (size_t)entry_cur();          /* letters go in, and Back takes one out, here */
        switch (k) {
        case UI_KEY_UP: S.entry_ch = (S.entry_ch + nl) % (nl + 1); S.ok_added = 0; break;
        case UI_KEY_DOWN: S.entry_ch = (S.entry_ch + 1) % (nl + 1); S.ok_added = 0; break;
        case UI_KEY_OK: {
            /* a double-click: undo the letter the first click added, then Results / Done */
            int dbl = S.ok_added && len && lv_tick_elaps(S.ok_tick) < DOUBLE_CLICK_MS;
            S.ok_added = 0;
            if (dbl && cur > 0) {                    /* (the one just added: before the cursor) */
                memmove(S.entry + cur - 1, S.entry + cur, len - cur + 1);
                len--;
                cur--;
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
                    S.entry_back = 0;
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
                else if (rename_same()) pop();            /* (a long name, not changed: it stays whole) */
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
                if (l->kind == L_RENAME && c >= 'a' && c <= 'z' && (cur == 0 || S.entry[cur - 1] == ' ')) c -= 32;
                if (l->kind == L_WIFI_PASS && len + 1 > 63) { ui_toast("63 characters at most", 1500); break; }
                memmove(S.entry + cur + 1, S.entry + cur, len - cur + 1);
                S.entry[cur] = c;
                if (l->kind == L_SEARCH) search_run();
                S.ok_added = 1;
                S.ok_tick = lv_tick_get();
            }
            break;
        }
        case UI_KEY_BACK:                                  /* delete the letter before the cursor; nothing typed: leave */
            S.ok_added = 0;
            if (cur > 0) {
                memmove(S.entry + entry_before(S.entry, (int)cur), S.entry + cur, len - cur + 1);
                if (l->kind == L_SEARCH) search_run();
            } else if (!len) {
                pop();
            }                                              /* (at the start of something typed: nothing to delete) */
            break;
        case UI_KEY_BACK_LONG: S.now_playing_view = 1; break;
        default: break;
        }
        render(R_ANIMATE);
        return;
    }
    /* The typing left for the results. <Back> in them: up to the typing row
     * (a row still: what's typed is kept, and nothing is deleted); <Back>
     * there: out of the screen (two presses however much is typed, where it
     * took one a letter); <Play> there: the letters again. */
    if (is_typing_row(l, 0) && (k == UI_KEY_BACK || (k == UI_KEY_OK && l->sel == 0))) {
        if (k == UI_KEY_OK) {
            l->typed = 0;
            S.ok_added = 0;                        /* (this <Play> isn't the first of two) */
        } else if (l->sel > 0) {
            l->sel = 0;
        } else {
            pop();
        }
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
    queue_move_follow(l);
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
        if (!press_twice(&S.the_back_armed, "Not saved: press <Back> again to leave it")) {
            render(R_CONTENT);
            return;
        }
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
                S.hooks.sound_adjust(sound_of(st), dir);
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
        if (flow_on(l)) {
            /* A cover flow: a quick turn goes a letter at a time (the next
             * one's first album; back, this letter's first, then the one
             * before's), where the list's leaps of 8 and 20 albums passed
             * whole letters by; the letters are along the foot, so none is
             * shown over the covers. Not by name: three albums at most. */
            if (step > 1 && flow_lettered(l) && n > 0) {
                if (k == UI_KEY_DOWN) l->sel = letter_next(l, l->sel);
                else {
                    int first = letter_first(l, l->sel);
                    l->sel = first < l->sel || first == 0 ? first : letter_first(l, first - 1);
                }
                break;
            }
            if (step > 3) step = 3;
        }
        int sel = l->sel + (k == UI_KEY_DOWN ? step : -step);
        l->sel = sel < 0 ? 0 : sel > n - 1 ? (n ? n - 1 : 0) : sel;
        if (step > 1 && !flow_on(l)) show_initial(l);
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

static int tick_do(void);

int ui_tick(void)
{
    long t0 = ui_timing() ? (long)clock_us() : 0;
    int r = tick_do();
    if (t0) ui_took("following the player", (long)clock_us() - t0);
    return r;
}

static int tick_do(void)
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

/* (Not in S: the device says this as it starts, before ui_init, which clears
 * S. Kept there, it was lost, and the toast didn't wait: seen on the H2.) */
static int g_packs_uncounted;       /* the library's names aren't counted yet... */
static uint32_t g_packs_uncounted_at;   /* ...said when */

void ui_packs_counted(int counted)
{
    g_packs_uncounted = !counted;
    g_packs_uncounted_at = lv_tick_get();
}

/* Names drawn as boxes for want of a character pack: said once for a card
 * (packs.told), with where the packs are, when the screen is on to read it */
static void packs_tell(void)
{
    if (!S.hooks.packs_state || S.asleep || !ui_packs_wanted(0)) return;
    /* The library's names not counted yet (the scan of a first start after the
     * update is still at it): Home may be about to offer the packs. It waits,
     * and what it had to say is kept; a minute at most. */
    if (g_packs_uncounted && lv_tick_elaps(g_packs_uncounted_at) < 60000) return;
    ui_packs_wanted(1);
    if (settings_get_int("packs.told", 0)) return;
    if (packs_offered(NULL)) return;              /* (Home says it already, and better) */
    packs_look();
    int absent = 0;
    for (int i = 0; i < S.npacks; i++) absent += S.packs[i].state == UI_PACK_ABSENT;
    if (!absent) return;
    settings_set_int("packs.told", 1);
    ui_toast("Some names need a language pack: Settings > Interface > Language packs", 6000);
}

void ui_tick_1s(void)
{
    packs_tell();
    if (S.wifi_then.armed && lv_tick_elaps(S.wifi_then.at) >= 45000) wifi_then_run(0);   /* (no network joined, say) */
    list_kind_t k = S.stack[S.depth - 1].kind;
    if ((k == L_BATTERY || k == L_BAT_SPAN || k == L_BAT_HISTORY) && lv_tick_elaps(S.bat_at) >= 60000) battery_load();
    pod_second();
    unsigned icons = ricon_seq();          /* a station's icon arrived: Radio's list reads its pictures again */
    if (icons != S.ricon_seq) {
        S.ricon_seq = icons;
        radio_keys();
        thumbs_reset(COV_RADIO);
    }
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

int ui_list_state(int *sel, int *n, int *home_edit)
{
    const level_t *l = &S.stack[S.depth - 1];
    if (sel) *sel = l->sel;
    if (n) *n = list_count(l);
    if (home_edit) *home_edit = l->kind == L_HOME_EDIT;
    return S.now_playing_view ? 0 : S.depth;
}

/* A screen by its name, from Home with nothing open. The player asks for one,
 * "nowplaying", when it starts where it was (device/main.c); the simulator
 * for every screen it takes a picture of or checks (ui_show.inc, built into
 * it alone). */
#ifdef FATHOM_UI_SHOW
#include "ui/ui_show.inc"
#endif

void ui_show(const char *what)
{
    clock_close();
    lyrics_close();
    S.scrubbing = 0;
    finish_transition();
    pending_free();
    while (S.depth > 1) pop();
    S.stack[0].sel = home_index(H_ALBUMS);
    S.now_playing_view = 0;
    if (!strcmp(what, "nowplaying")) S.now_playing_view = 1;
#ifdef FATHOM_UI_SHOW
    else show_named(what);
#endif
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
    image_dsc(&S.art_dsc, LV_COLOR_FORMAT_RGB565, LIB_ART_LARGE, LIB_ART_LARGE, S.art_px, sizeof S.art_px);
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

    S.posbar = box(S.list, UI_W - POSBAR_W - 1, POSBAR_PAD, POSBAR_W, POSBAR_MIN);
    show(S.posbar, 0);
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
        image_dsc(&S.cov_dsc[j], LV_COLOR_FORMAT_RGB565, LIB_ART_SMALL, LIB_ART_SMALL, NULL, 0);   /* (its pixels: when a cover is shown) */
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
    /* Albums as cover flow: over the list's rows, in the list's own box (it
     * slides with it when a level opens or closes) */
    S.flow = box(S.list, 0, 0, UI_W, UI_H - HEADER_H);
    S.flow_img = lv_image_create(S.flow);
    lv_obj_set_pos(S.flow_img, 0, 0);
    S.flow_title = label(S.flow);
    lv_obj_set_pos(S.flow_title, 12, FLOW_H - 2);
    lv_obj_set_width(S.flow_title, UI_W - 24);
    lv_obj_set_style_text_align(S.flow_title, LV_TEXT_ALIGN_CENTER, 0);
    S.flow_by = label(S.flow);
    lv_obj_set_pos(S.flow_by, 12, FLOW_H + 24);
    lv_obj_set_width(S.flow_by, UI_W - 24);
    lv_obj_set_style_text_align(S.flow_by, LV_TEXT_ALIGN_CENTER, 0);
    S.flow_az_pill = box(S.flow, 0, 0, 19, 18);     /* (made first: behind the letters) */
    show(S.flow_az_pill, 0);
    for (int i = 0; i < 27; i++) {
        char one[2] = {i ? (char)('A' + i - 1) : '#', 0};
        S.flow_az[i] = label(S.flow);
        lv_obj_set_width(S.flow_az[i], (UI_W - 16) / 27 + 12);     /* (its place and 6 px each side: a W is wider than a place) */
        lv_obj_set_style_text_align(S.flow_az[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(S.flow_az[i], LV_LABEL_LONG_CLIP);
        lv_label_set_text(S.flow_az[i], one);
        show(S.flow_az[i], 0);
    }
    S.flow_az_on = -1;
    S.flow_track = box(S.flow, 12, UI_H - HEADER_H - 8, UI_W - 24, 3);
    S.flow_thumb = box(S.flow_track, 0, 0, 16, 3);
    S.flow_sel = -1;
    show(S.flow, 0);

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
    const int title_h = 64, artist_y = NP_ARTIST_Y, album_y = NP_ALBUM_Y, chip_y = 122;
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
