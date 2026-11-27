/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* The player on the H2: measuring loudness for volume levelling, and the
 * reader helper's work (main_int.h). Out of main.c, where it stood. */
#include "main_int.h"

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

/* One song levelled, as rg_thread does it: from its tags if it has them
 * since the last scan (tagged: 1), else measured. 0, 1 stopped half way
 * (g_rg_stop), -1 it couldn't be. */
static int rg_one(const char *path, lib_rg_t *rg, int *tagged)
{
    tags_t t;
    *tagged = 0;
    if (tags_read(path, &t) == 0 && t.has_rg_track) {
        *rg = (lib_rg_t){1, t.rg_track_gain, t.rg_track_peak, t.has_rg_album, t.rg_album_gain, t.rg_album_peak};
        *tagged = 1;
        return 0;
    }
    return rg_measure(path, rg);
}

/* The same done by the player's helper (library/reader.h), which is what a
 * song that crashes the decoder or wants more memory than there is takes
 * with it: the last work on files of unknown kind that the player still did
 * itself. RD_LEVEL's answer. */
#define RD_LEVEL LIBRARY_READER_OPS_END
#define RG_HELPER_MS (30 * 60 * 1000)        /* (an hour-long file is some minutes of decoding) */
typedef struct { lib_rg_t rg; int tagged, rc; } rg_reply_t;

int player_reader_do(int op, const char *path, unsigned char **out, size_t *len)
{
    if (op != RD_LEVEL) return library_reader_do(op, path, out, len);
    rg_reply_t *r = calloc(1, sizeof *r);
    if (!r) return -1;
    r->rc = rg_one(path, &r->rg, &r->tagged);
    *out = (unsigned char *)r;
    *len = sizeof *r;
    return 0;
}

/* A song that couldn't be measured because its file, though there, can't be
 * read just now (the card's error, at the open or at its first bytes) was
 * written down as one that can't be measured: for good. It is left
 * unmeasured: levelling stops, waits ten minutes (rg_poll) and comes back to
 * it, the first song library_rg_next gives. The third time running it is
 * written down after all, or one bad file would keep every song after it
 * from being measured. (The levelling thread's own.) */
static volatile int g_rg_later;

static int rg_later(int64_t id, const char *path)
{
    static int64_t last;
    static int tries;
    char file[1024];
    uint32_t start, end;
    if (cue_part_parse(path, file, sizeof file, &start, &end) > 0) path = file;   /* (a CUE track: its file) */
    if (!file_unreadable(path)) return 0;
    if (id != last) { last = id; tries = 0; }
    if (++tries >= 3) return 0;
    fprintf(stderr, "levelling: %s can't be read just now: left for later\n", path);
    g_rg_later = 1;
    return 1;
}

static void *rg_thread(void *arg)
{
    (void)arg;
    thread_nice(19);                    /* below the music and the UI */
    int64_t id;
    char path[1024];
    int measured = 0, tagged = 0;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    /* the song the player died measuring, last time: marked as one that couldn't
     * be measured, or it would be the first tried at every start */
    char died[32];
    if (library_trying_left("level", died, sizeof died) && atoll(died) > 0) {
        lib_rg_t none = {.state = 3};
        fprintf(stderr, "levelling: the player stopped while measuring track %s: left unmeasured\n", died);
        library_rg_store(g_rg_lib, atoll(died), &none);
    }
    int trying = -1;
    while (!g_rg_stop && library_rg_next(g_rg_lib, &id, path, sizeof path) == 0) {
        lib_rg_t rg = {0};
        char note[32];
        snprintf(note, sizeof note, "%lld", (long long)id);
        library_trying("level", &trying, note);
        int was_tagged = 0, r = -2;
        if (reader_on()) {
            /* by the helper; not while memory is short (a failure then would
             * say nothing about the song, and it would be left unmeasured
             * for good): a few seconds, and looked at again */
            int is_short = 0, st = -1;
            long budget = library_reader_budget(&is_short);
            if (is_short) {
                for (int k = 0; k < 25 && !g_rg_stop; k++) usleep(200 * 1000);
                continue;
            }
            unsigned char *b = NULL;
            size_t n = 0;
            int c = reader_call_long(RD_LEVEL, path, budget, RG_HELPER_MS, &g_rg_stop, &st, &b, &n);
            if (c == READER_STOPPED) { free(b); break; }
            if (c == READER_DIED) {
                fprintf(stderr, "levelling: the reader stopped on %s: left unmeasured\n", path);
                r = -1;
            } else if (c == 0) {
                if (st == 0 && n == sizeof(rg_reply_t)) {
                    rg_reply_t a;
                    memcpy(&a, b, sizeof a);
                    rg = a.rg;
                    was_tagged = a.tagged;
                    r = a.rc;
                } else r = -1;
            }
            free(b);
        }
        if (r == -2) r = rg_one(path, &rg, &was_tagged);     /* (no helper: here, as before) */
        if (r > 0) break;                                /* stopped half way */
        if (r < 0 && rg_later(id, path)) break;          /* the card didn't give it just now */
        if (r < 0) rg = (lib_rg_t){.state = 3};
        else if (was_tagged) tagged++;
        else measured++;
        library_rg_store(g_rg_lib, id, &rg);
        if ((measured + tagged) % 50 == 0) {
            clock_gettime(CLOCK_MONOTONIC, &t1);
            fprintf(stderr, "levelling: %d measured, %d from tags, %.0f s\n", measured, tagged,
                    (double)(t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9);
        }
    }
    library_trying_done("level", &trying);
    /* (the helper and its memory given back, as at a scan's end: it stayed
     * until the next scan otherwise, started from this thread's low
     * priority. Not one a scan has just taken over.) */
    reader_close_nowait();
    clock_gettime(CLOCK_MONOTONIC, &t1);
    fprintf(stderr, "levelling: %d measured, %d from tags in %.0f s\n", measured, tagged,
            (double)(t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9);
    g_rg_running = 0;
    return NULL;
}

void rg_stop(void)
{
    if (!g_rg_lib) return;
    g_rg_stop = 1;
    pthread_join(g_rg_thread, NULL);
    library_close(g_rg_lib);
    g_rg_lib = NULL;
}

/* Every second: start measuring while charging and idle, with levelling on
 * (off, the default: no use for it; measured songs are kept), stop otherwise. */
void rg_poll(lv_timer_t *t)
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
        if (g_rg_later) { g_rg_later = 0; done_at = now_s; }   /* (a song the card didn't give: the same wait) */
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
