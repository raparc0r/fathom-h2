/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* The player on the H2: Settings > Interface > Language packs, the character
 * packs' side of it (the songs each pack would draw, counted; the page's
 * hooks; its poll) (main_int.h). Out of main.c, where it stood. */
#include "main_int.h"

char g_packs_dir[600];

/* The songs in the library with names each pack would draw (library/scripts.h),
 * counted at the end of every scan, on the scan's thread; kept in the settings
 * (packs.songs) so that Home's offer is there from the start, before the
 * start's own scan has ended. */
static pthread_mutex_t g_pack_songs_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_pack_songs[WANT_N], g_pack_songs_new;
volatile int g_pack_songs_known;     /* counted once at least (packs.songs is in the settings) */

void pack_songs_count(library_t *lib)
{
    lib_scripts_t c;
    int w[WANT_N];
    int64_t t0 = clock_ms32();
    if (!lib || library_scripts(lib, &c)) return;       /* (not read through: the last count stands) */
    scripts_want(&c, w);
    fprintf(stderr, "packs: the library's names counted in %lld ms: Japanese %d songs, Korean %d, Chinese %d and %d\n",
            (long long)(clock_ms32() - t0), w[WANT_JA], w[WANT_KO], w[WANT_ZH_HANS], w[WANT_ZH_HANT]);
    pthread_mutex_lock(&g_pack_songs_mu);
    memcpy(g_pack_songs, w, sizeof g_pack_songs);
    g_pack_songs_new = 1;
    pthread_mutex_unlock(&g_pack_songs_mu);
}

void pack_songs_load(void)
{
    int w[WANT_N] = {0};
    g_pack_songs_known = sscanf(settings_get("packs.songs", ""), "%d,%d,%d,%d", &w[0], &w[1], &w[2], &w[3]) == WANT_N;
    pthread_mutex_lock(&g_pack_songs_mu);
    for (int i = 0; i < WANT_N; i++) g_pack_songs[i] = w[i] > 0 ? w[i] : 0;
    pthread_mutex_unlock(&g_pack_songs_mu);
}

/* (the main thread) a new count: kept, and the screen told; 1 if there was one */
static int pack_songs_keep(void)
{
    char v[64];
    pthread_mutex_lock(&g_pack_songs_mu);
    int was_new = g_pack_songs_new;
    g_pack_songs_new = 0;
    snprintf(v, sizeof v, "%d,%d,%d,%d", g_pack_songs[0], g_pack_songs[1], g_pack_songs[2], g_pack_songs[3]);
    pthread_mutex_unlock(&g_pack_songs_mu);
    if (!was_new) return 0;
    if (strcmp(settings_get("packs.songs", ""), v)) settings_set("packs.songs", v);
    g_pack_songs_known = 1;
    return 1;
}

int hook_packs_state(ui_pack_t *out, int cap)
{
    int n = packs_count();
    if (n > cap) n = cap;
    for (int i = 0; i < n; i++) {
        const pack_info_t *p = packs_info(i);
        pack_status_t st;
        packs_status(i, &st);
        memset(&out[i], 0, sizeof out[i]);
        snprintf(out[i].name, sizeof out[i].name, "%s", p->name);
        out[i].bytes = p->zip_size;
        out[i].percent = st.percent;
        out[i].state = st.state == PACK_GETTING ? UI_PACK_GETTING : st.state == PACK_THERE ? UI_PACK_THERE
                       : st.state == PACK_FAILED ? UI_PACK_FAILED : UI_PACK_ABSENT;
        snprintf(out[i].message, sizeof out[i].message, "%s", st.why);
        int want = scripts_want_of(p->id);
        pthread_mutex_lock(&g_pack_songs_mu);
        out[i].songs = want >= 0 ? g_pack_songs[want] : 0;
        pthread_mutex_unlock(&g_pack_songs_mu);
        snprintf(out[i].id, sizeof out[i].id, "%s", p->id);
        out[i].han = p->han;
        snprintf(out[i].brief, sizeof out[i].brief, "%s", p->brief ? p->brief : "");
        out[i].first = p->han && !strcmp(p->id, settings_get("packs.first", ""));
    }
    return n;
}

void hook_packs_do(int i, int what)
{
    switch (what) {
    case UI_PACK_GET:
        /* (through a proxy, on the bench: no Wi-Fi needed) */
        if (!net_up()) packs_refuse(i, net_why());
        else packs_get(i);
        break;
    case UI_PACK_CANCEL: packs_cancel(); break;
    case UI_PACK_REMOVE: packs_delete(i); break;
    case UI_PACK_FIRST:                         /* its shapes for the characters several packs have */
        if (!packs_info(i)) break;
        settings_set("packs.first", packs_info(i)->id);
        ui_packs_first(packs_info(i)->id);
        ui_packs_changed(1);
        break;
    }
}

/* What the fetching thread has done since the last look: the screen told; a
 * pack that came or went: the folder named again (the fonts open what is in
 * it now when a character next needs it) and everything drawn again. */
void packs_poll(void)
{
    static unsigned seq, gen;
    unsigned s = packs_seq(), g = packs_gen();
    if (pack_songs_keep()) {                    /* a scan counted the library's names anew */
        ui_packs_counted(1);
        if (s == seq) { ui_packs_changed(0); return; }
    }
    if (s == seq) return;
    seq = s;
    int drawn = g != gen;
    gen = g;
    if (drawn) ui_packs_dir(g_packs_dir);
    ui_packs_changed(drawn);
}

/* A library never counted, at a start with no scan of its own: the usual
 * start, so the first one after the update that brought the packs (seen on
 * the H2: installed over a library in order, nothing counted, Home silent
 * until some scan). Its names counted alone, once, on a thread and a
 * connection of its own, as a scan's end does; picked up by packs_poll. */
static pthread_t g_count_thread;
static int g_count_started;

static void *count_thread(void *arg)
{
    (void)arg;
    thread_nice(19);                    /* below the music and the UI, as a scan */
    library_t *lib = library_open(g_db_path, g_art_path);
    if (lib) {
        pack_songs_count(lib);
        library_close(lib);
    }
    return NULL;
}

void pack_songs_count_alone(void)
{
    if (g_count_started || g_pack_songs_known || !g_db_path[0]) return;
    if (pthread_create(&g_count_thread, NULL, count_thread, NULL) == 0) g_count_started = 1;
}

/* Before the library is lent or closed: the count, if one is out, has ended
 * (a pass over the names: a second at most). */
void pack_songs_count_wait(void)
{
    if (!g_count_started) return;
    pthread_join(g_count_thread, NULL);
    g_count_started = 0;
}
