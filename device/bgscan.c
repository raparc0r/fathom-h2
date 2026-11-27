/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* The player on the H2: the library's background scans (after USB drive
 * mode, a card put back, the network drive's changes, Settings > Library >
 * Rescan), and the card's free space read behind (main_int.h). Out of
 * main.c, where it stood. */
#include "main_int.h"

/* ---------- background scans ----------
 * Checking the card for new music takes about a second when nothing changed
 * (the FAT read directly), minutes after adding music (tags, covers), so after
 * USB drive mode, a card put back, a network drive's changes and from
 * Settings > Library > Rescan it runs on a thread with its own connection to
 * the library while the music plays; the lists are refreshed when it's done,
 * if anything changed. */
char g_pl_path[600];
static pthread_t g_scan_thread;
static library_t *g_scan_lib;              /* the scan's own connection */
static int g_scan_result, g_scan_changes;

/* Scans read the music folder's FAT directly when it's on a vfat partition
 * (library_set_fast_walk): find it in /proc/mounts. */
void fast_walk_setup(library_t *lib, const char *music)
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
static volatile int g_free_counted;   /* that first count is done: asking is quick from now on */

static void *free_space_thread(void *arg)
{
    (void)arg;
    thread_nice(19);                    /* below the music and the UI */
    sleep(15);                                                    /* (after the start's own reading) */
    struct statvfs st;
    statvfs(g_music ? g_music : CARD_DIR, &st);
    g_free_counted = 1;
    return NULL;
}

void free_space_warm(void)
{
    thread_detached(free_space_thread, NULL, 64 * 1024);
}

/* The card is full (under 2 MB free): settings, the queue's place and new music
 * can't be saved, which went unsaid. Said once a session, with the screen on
 * (a toast behind a dark screen is never read). Not before the free space has
 * been counted: until then the question reads the whole FAT, on this thread. */
#define CARD_FULL_BYTES (2 * 1024 * 1024)

void card_full_check(void)
{
    static int said;
    struct statvfs st;
    if (said || !g_screen_on || !g_free_counted) return;
    if (statvfs(g_music ? g_music : CARD_DIR, &st) != 0) return;
    unsigned long long free_b = (unsigned long long)st.f_bavail * st.f_frsize;
    if (free_b >= CARD_FULL_BYTES) return;
    said = 1;
    fprintf(stderr, "card: full (%llu KB free): settings and new music can't be saved\n", free_b / 1024);
    ui_toast("The card is full, so settings and new music can't be saved. Make room on a computer.", 8000);
}

/* The background scan's progress, for Settings > Library and Rescan's screen
 * (read on the UI's thread: copied under a lock, the file's name with it) */
static pthread_mutex_t g_bg_mu = PTHREAD_MUTEX_INITIALIZER;
static ui_scan_t g_bg;
static int g_rescan_asked;   /* (Rescan: its end says so even when nothing changed) */
static int g_scan_retry;     /* (Rescan: the scan it starts reads again the files none could read) */
static int g_scan_covers;    /* the background scan made covers: the screen shows them */
int g_scan_check;     /* the next background scan checks the database first (the last stop wasn't clean) */
static volatile int g_db_damaged;   /* ...and found it damaged */
static void bg_progress(int done, int total, const char *path, void *ctx)
{
    (void)ctx;
    if (path && !strcmp(path, "done")) return;
    pthread_mutex_lock(&g_bg_mu);
    if (path && (!strcmp(path, "looking") || !strcmp(path, "checking"))) {
        g_bg.step = UI_SCAN_LOOKING;               /* (total -1: files looked up, not songs found) */
        g_bg.done = done;
        g_bg.total = path[0] == 'c' ? -1 : 0;
        g_bg.file[0] = '\0';
    } else if (path && (!strcmp(path, "sorting") || !strcmp(path, "working"))) {
        g_bg.step = UI_SCAN_ALBUMS;                /* (total -1: at work, done only a number that goes up) */
        g_bg.done = done;
        g_bg.total = path[0] == 'w' ? -1 : total;
        g_bg.file[0] = '\0';
    } else if (path && (!strcmp(path, "albums") || !strcmp(path, "indexes"))) {
        g_bg.step = UI_SCAN_ALBUMS;
        g_bg.done = g_bg.total = 0;
        g_bg.file[0] = '\0';
    } else if (total < 0) {
        g_bg.step = UI_SCAN_COVERS;
        g_bg.total = -total - 1;
        g_bg.done = done;
        g_bg.file[0] = '\0';
    } else {
        g_bg.step = UI_SCAN_SONGS;
        g_bg.total = total;
        g_bg.done = done;
        const char *slash = path ? strrchr(path, '/') : NULL;
        snprintf(g_bg.file, sizeof g_bg.file, "%s", slash ? slash + 1 : path ? path : "");
    }
    pthread_mutex_unlock(&g_bg_mu);
}

int hook_scan_progress(ui_scan_t *p)
{
    if (g_scan_state != 1) return 0;
    pthread_mutex_lock(&g_bg_mu);
    *p = g_bg;
    pthread_mutex_unlock(&g_bg_mu);
    return 1;
}

static void *scan_thread(void *arg)
{
    (void)arg;
    thread_nice(19);                    /* below the music and the UI, as levelling */
    pthread_mutex_lock(&g_bg_mu);
    memset(&g_bg, 0, sizeof g_bg);                   /* (UI_SCAN_LOOKING) */
    pthread_mutex_unlock(&g_bg_mu);
    /* After a stop that wasn't clean: the database read through first. Sound:
     * this scan compares every file again. Damaged: said, and rebuilt at the
     * next start (main: the marker), its play counts brought over. */
    if (g_scan_lib && g_scan_check) {
        g_scan_check = 0;
        if (library_check(g_scan_lib) == 0) library_scan_distrust(g_scan_lib);
        else {
            char mark[600];
            snprintf(mark, sizeof mark, "%s.rebuild", g_db_path);
            FILE *f = fopen(mark, "we");
            if (f) fclose(f);
            g_db_damaged = 1;
            g_scan_result = -1;
            g_scan_changes = 0;
            g_scan_state = 2;
            return NULL;
        }
    }
    if (g_scan_lib && g_scan_retry) library_scan_retry_unread(g_scan_lib, 1);
    g_scan_retry = 0;
    g_scan_result = g_scan_lib ? library_scan(g_scan_lib, g_music, bg_progress, NULL) : -1;
    if (g_scan_lib && g_scan_result >= 0) library_import_owed(g_scan_lib);   /* (a set-aside library's plays, once all is read) */
    g_scan_changes = g_scan_lib && g_scan_result >= 0 ? library_scan_changes(g_scan_lib) : 0;
    /* Names a character pack would draw (Home offers it): counted when the
     * library changed, and once for a library never counted (a start with no
     * scan: pack_songs_count_alone). A pass over every song: not at every
     * start. */
    if (g_scan_lib && g_scan_result >= 0 && (g_scan_changes > 0 || !g_pack_songs_known)) pack_songs_count(g_scan_lib);
    g_scan_state = 2;
    return NULL;
}

void scan_start(void)
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

/* Music files the scans couldn't read (damaged ones, and those set aside
 * because the player stopped while reading them: Settings > Library lists
 * them): said when a scan ends with more of them than when it was last said,
 * so once for each that turns up. */
static void unread_tell(void)
{
    if (!g_lib) return;
    int n = library_unread_count(g_lib), told = settings_get_int("library.unread_told", 0);
    if (n == told) return;
    settings_set_int("library.unread_told", n);
    if (n < told) return;
    char num[16], msg[200];
    snprintf(num, sizeof num, "%d", n);
    lang_fill(msg, sizeof msg, tr(n == 1 ? "%s file couldn't be read. See Settings > Library." : "%s files couldn't be read. See Settings > Library."), num, NULL);
    ui_toast(msg, 7000);
}

/* Called often: picks up a finished scan. */
void scan_poll(lv_timer_t *t)
{
    (void)t;
    if (g_scan_state == 1) {                         /* covers as they're made: shown every 10 s */
        static uint32_t shown_at;
        static int shown;
        pthread_mutex_lock(&g_bg_mu);
        int covers = g_bg.step == UI_SCAN_COVERS ? g_bg.done : 0;
        pthread_mutex_unlock(&g_bg_mu);
        if (covers < shown) shown = 0;               /* (another scan) */
        if (covers > shown && clock_ms32() - shown_at >= 10000) {
            shown = covers;
            shown_at = clock_ms32();
            g_scan_covers = 1;
            ui_covers_changed();
        }
        return;
    }
    if (g_scan_state != 2) return;
    pthread_join(g_scan_thread, NULL);
    library_close(g_scan_lib);
    g_scan_lib = NULL;
    g_scan_state = 0;
    {
        pthread_mutex_lock(&g_bg_mu);
        if (g_bg.step == UI_SCAN_COVERS && g_bg.done > 0) g_scan_covers = 1;
        pthread_mutex_unlock(&g_bg_mu);
    }
    fprintf(stderr, "library: background scan: %d tracks, %d changes\n", g_scan_result, g_scan_changes);
    ui_set_music_missing(g_music && access(g_music, F_OK) != 0);   /* (made since, or gone) */
    if (g_lib) library_art_legacy(g_lib);            /* (its covers may be in the store now) */
    if (g_scan_result >= 0 && g_scan_changes > 0) {
        ui_library_changed();
        ui_toast("Library updated", 1500);
    } else if (g_db_damaged) {
        g_db_damaged = 0;
        ui_toast("The library is damaged. It's rebuilt at the next start; your play counts are kept.", 8000);
    } else if (g_scan_covers) ui_covers_changed();   /* (covers only: shown where they were missing) */
    else if (g_rescan_asked && g_scan_result >= 0) ui_toast("No changes found", 1500);
    else if (g_rescan_asked) ui_toast("Couldn't read the music folder", 2500);
    if (g_scan_result >= 0) unread_tell();           /* (over "Library updated": the one to read) */
    g_rescan_asked = 0;
    g_scan_covers = 0;
    ui_show_scan();                                  /* (its screen goes) */
}

/* On the way out (and before lending the card): stop a scan and wait for it. */
void scan_stop(void)
{
    pack_songs_count_wait();
    if (!g_scan_state) return;
    library_scan_cancel(g_scan_lib);
    pthread_join(g_scan_thread, NULL);
    library_close(g_scan_lib);
    g_scan_lib = NULL;
    g_scan_state = 0;
}

/* Settings > Library > Rescan: the background scan, which the UI then shows
 * over everything (ui_show_scan); one already running is shown as it is */

void rescan(void)
{
    if (!g_lib) return;
    if (!g_scan_state) {
        pthread_mutex_lock(&g_bg_mu);
        memset(&g_bg, 0, sizeof g_bg);               /* (no last scan's numbers for a moment) */
        pthread_mutex_unlock(&g_bg_mu);
        g_scan_retry = 1;                            /* (asked for: the files left out get another try) */
        scan_start();
    }
    g_rescan_asked = 1;
}
