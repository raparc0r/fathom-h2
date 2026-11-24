/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
#include "player.h"
#include "core/fsafe.h"
#include "core/lat.h"
#include "library/cue.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include "core/rng.h"
#include "shuffle/shuffle.h"

#define SEAM 30
#define EARLY_SKIP_MS 30000
#define RESTART_MS 3000
#define SLEEP_FADE_MS 10000
#define LONG_FILE_MS (20 * 60 * 1000)   /* files this long remember where they were left */
#define LONG_FILE_EDGE_MS 30000         /* ...unless that's within 30 s of either end */

struct player {
    library_t *lib;
    engine_t *eng;

    int64_t *ctx;          /* track ids; the context, then anything queued (append-only) */
    int      n, cap_ctx;
    int     *order;        /* play order: indices into ctx */
    int      n_order, cap_order;
    int      pos;          /* index into order */
    int      user_end;     /* order positions pos+1 .. user_end-1 were queued by hand */
    int      shuffle;
    repeat_t repeat;
    int      stop_after;
    levelling_t levelling;    /* volume levelling: off (bit-perfect), track or album */

    uint32_t eng_seq;      /* engine seq we last acted on */
    unsigned eng_stop;     /* engine stop_seq we last acted on */
    int      own_start;    /* we asked the engine to start a track: its next seq is ours */
    int      fails;        /* tracks in a row that couldn't be played */
    unsigned fail_seq;     /* player_now_t's: one more for each passed over */
    int64_t  fail_track;
    int      fail_output, fail_memory, fail_all, fail_end;
    uint32_t seq;          /* our track-change counter (for the UI) */
    uint64_t saved_hash;   /* player_save: the queue as last written (0: not yet) */
    struct { int pos; uint32_t tag; } ahead[ENGINE_AHEAD];   /* lined up in the engine: order positions */
    int      n_ahead;
    uint32_t last_tag;
    char     next_path[sizeof ((lib_track_t *)0)->path];   /* the first lined up (n_ahead > 0): its file, */
    int64_t  next_id;                                      /* ...and what of it was read ahead (warm_next) */
    uint32_t warm_seq;
    int64_t  warm_id;
    int      warm_level;
    lib_track_t cur;
    int      have_cur;
    uint32_t resume_ms;    /* cued track: start here (restored queue) */

    int      sleep_min;    /* sleep timer setting, 0 = off, PLAYER_SLEEP_END_ALBUM */
    int64_t  sleep_album;  /* end of album: the album playing when it was set */
    uint64_t sleep_at;     /* monotonic ms when it pauses */
    player_shuffle_prefs_t sprefs;   /* Settings > Playback */
    int      ctx_chosen;      /* the context was chosen to shuffle (an album, an artist):
                                 spoken tracks stay in; ctx_songs: songs, never whole albums;
                                 ctx_all: short ones too (they were all it had) */
    int      ctx_songs, ctx_all;
    int      all_pending;     /* Shuffle all started on one song: the whole order follows (shuffle_all_rest) */

    /* internet radio */
    int      radio;        /* on: the engine plays rst[ri] */
    radio_station_t *rst;
    int      rn, ri;
    int      r_want;       /* it should be playing (reconnect if it stops) */
    int      r_tries;      /* reconnects since it last played steadily */
    int      r_heard;      /* it has played since it was asked for: a drop gets all the tries (else: RADIO_TRIES_NEW) */
    uint64_t r_asked_at;   /* when it was asked for (Play, Next, the network back) */
    uint64_t r_sound_at;   /* since when it has been playing without a break, 0 = it isn't */
    int      r_failed;
    uint64_t r_retry_at;   /* monotonic ms of the next reconnect, 0 = none */
    uint64_t r_since;      /* when the stream last (re)started */
    int      r_down;       /* the network is gone (player_radio_net): nothing is tried */
    int      r_began_down; /* ...and was when the last connection was begun */
    uint64_t r_parked_at;  /* ...since when a station wanted has waited for it, 0 = it isn't */
    uint64_t r_failed_at;  /* when it was given up with the network there */
    int      r_lost;       /* given up for want of the network: it comes back with it */
    uint64_t r_sleep_at;   /* the sleep timer's time at the last look (radio_tick) */
    unsigned r_meta_seq;
    char     r_meta[256], r_meta_url[512];
    char     r_artist[256], r_title[256], r_art[512];   /* player_radio_set_info; title "" = none */

    /* a podcast episode */
    int      pod;          /* on: the engine plays ep */
    player_episode_t ep;
    uint32_t ep_pos;       /* where it was last seen (playing), or cued */
    int      ep_ended;     /* it played to its end: said once */
    int      speed;        /* episodes' speed, percent */
};

#define POD_ENGINE_ID (-1000)   /* the engine's id for an episode */

static int64_t now_s(void) { return (int64_t)time(NULL); }

static uint64_t mono_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}

player_t *player_new(library_t *lib, engine_t *eng)
{
    player_t *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    p->lib = lib;
    p->eng = eng;
    eng_status_t st;                   /* (an engine that played before: what's past isn't ours) */
    engine_status(eng, &st);
    p->eng_seq = st.seq;
    p->eng_stop = st.stop_seq;
    p->sprefs = (player_shuffle_prefs_t){.freshness = 1, .skip_interludes = 1};   /* the defaults */
    return p;
}

void player_free(player_t *p)
{
    if (!p) return;
    free(p->ctx);
    free(p->order);
    free(p->rst);
    free(p);
}

static int grow(void **v, int *cap, int need, size_t elem)
{
    if (need <= *cap) return 0;
    int nc = *cap ? *cap : 64;
    while (nc < need) nc *= 2;
    void *nv = realloc(*v, (size_t)nc * elem);
    if (!nv) return -1;
    *v = nv;
    *cap = nc;
    return 0;
}

static void set_ctx(player_t *p, const int64_t *ids, int n)
{
    p->n = 0;
    p->n_ahead = 0;                     /* (positions in the old order) */
    if (grow((void **)&p->ctx, &p->cap_ctx, n ? n : 1, sizeof *p->ctx)) return;
    memcpy(p->ctx, ids, (size_t)n * sizeof *p->ctx);
    p->n = n;
    p->ctx_chosen = p->ctx_songs = p->ctx_all = 0;
}

static int cmp_shuf_id(const void *a, const void *b)
{
    uint32_t x = ((const shuf_track_t *)a)->id, y = ((const shuf_track_t *)b)->id;
    return x < y ? -1 : (x > y);
}

/* Build the play order over the whole context. With shuffle, ctx index `first`
 * (or -1) plays first and the tracks in `tail` (ids that ended the previous
 * cycle) stay away from the start. */
static void build_order(player_t *p, int first, const uint32_t *tail, size_t tail_n)
{
    p->n_order = 0;
    if (p->n == 0 || grow((void **)&p->order, &p->cap_order, p->n, sizeof *p->order)) return;
    if (!p->shuffle) {
        for (int i = 0; i < p->n; i++) p->order[p->n_order++] = i;
        return;
    }

    struct timespec ts0, ts1, ts2, ts3;
    clock_gettime(CLOCK_MONOTONIC, &ts0);
    shuf_track_t *t = malloc((size_t)p->n * sizeof *t);
    uint32_t *out = malloc((size_t)p->n * sizeof *out);
    if (!t || !out) { free(t); free(out); return; }
    /* A few songs of a big library (an album, a playlist, an artist): theirs
     * alone are read, by id. Every song's rows were read and sorted to look
     * these up: over a second on the H2 with 50,000 songs, to shuffle twelve. */
    if (p->n < 4000 && (int64_t)p->n * 8 < library_top_track(p->lib) &&
        library_shuffle_tracks_for(p->lib, p->ctx, p->n, t) == p->n) {
        clock_gettime(CLOCK_MONOTONIC, &ts1);
        ts2 = ts1;
    } else {
        shuf_track_t *all = NULL;
        int na = library_shuffle_tracks(p->lib, &all);
        clock_gettime(CLOCK_MONOTONIC, &ts1);
        /* each of the context's songs among them: through a table by id
         * (ids are small numbers), else by sorting and searching */
        uint32_t top = 0;
        for (int i = 0; i < na; i++)
            if (all[i].id > top) top = all[i].id;
        int32_t *at = na > 0 && top < (uint32_t)na * 8 + 4096 ? malloc(((size_t)top + 1) * sizeof *at) : NULL;
        if (at) {
            memset(at, 0xff, ((size_t)top + 1) * sizeof *at);        /* (-1: none) */
            for (int i = 0; i < na; i++) at[all[i].id] = i;
        } else if (na > 0) qsort(all, (size_t)na, sizeof *all, cmp_shuf_id);
        for (int i = 0; i < p->n; i++) {
            shuf_track_t key = {.id = (uint32_t)p->ctx[i]};
            shuf_track_t *hit = NULL;
            if (at) hit = p->ctx[i] > 0 && (uint64_t)p->ctx[i] <= top && at[key.id] >= 0 ? &all[at[key.id]] : NULL;
            else if (na > 0) hit = bsearch(&key, all, (size_t)na, sizeof *all, cmp_shuf_id);
            if (hit) t[i] = *hit;
            else { memset(&t[i], 0, sizeof t[i]); t[i].id = key.id; }
        }
        free(at);
        free(all);
        clock_gettime(CLOCK_MONOTONIC, &ts2);
    }

    shuf_opts_t o;
    shuf_default_opts(&o, now_s());
    o.first = first;
    o.mode = p->sprefs.albums && !p->ctx_songs ? SHUF_MODE_ALBUMS : SHUF_MODE_SONGS;
    o.freshness = p->sprefs.freshness;
    o.skip_interludes = p->sprefs.skip_interludes && !p->ctx_all;
    o.exclude_spoken = p->sprefs.exclude_spoken && !p->ctx_chosen;
    o.keep_units = p->sprefs.keep_works;
    long c = shuf_build(t, (size_t)p->n, &o, rng_os_seed(), tail, tail_n, out);
    clock_gettime(CLOCK_MONOTONIC, &ts3);
#define MS_(a, b) ((long)((b.tv_sec - a.tv_sec) * 1000 + (b.tv_nsec - a.tv_nsec) / 1000000))
    if (MS_(ts0, ts3) >= 200)                        /* (a big library: where a slow shuffle's time goes) */
        fprintf(stderr, "player: shuffle of %d: %ld ms (songs read %ld, matched %ld, ordered %ld)\n", p->n,
                MS_(ts0, ts3), MS_(ts0, ts1), MS_(ts1, ts2), MS_(ts2, ts3));
#undef MS_
    for (long i = 0; i < c; i++) p->order[p->n_order++] = (int)out[i];
    free(out);
    free(t);
}

static int64_t id_at(const player_t *p, int order_pos)
{
    return (order_pos >= 0 && order_pos < p->n_order) ? p->ctx[p->order[order_pos]] : -1;
}

/* Volume levelling (ReplayGain): the factor to play a track at. */
static float track_gain(player_t *p, int64_t id)
{
    return p->levelling == LEVEL_OFF ? 1.0f : library_rg_gain(p->lib, id, p->levelling == LEVEL_ALBUM);
}

/* The order position that plays after `pos` by itself, or -1. */
static int after(const player_t *p, int pos)
{
    if (p->stop_after) return -1;
    if (p->repeat == REPEAT_ONE) return pos;
    if (pos + 1 < p->n_order) return pos + 1;
    if (p->repeat == REPEAT_ALL && !p->shuffle && p->n_order > 0) return 0;   /* wrap */
    return -1;
}

/* Line up what plays after the current track, for gapless playback: the next
 * one, and more after it while they add up to less than AHEAD_MS. The engine
 * goes on through them by itself, so a run of very short tracks stays gapless
 * however late the UI's tick comes to line up more (a busy H2: a scan). */
#define AHEAD_MS 30000
static void catch_up(player_t *p);
static void shuffle_all_rest(player_t *p);
static void queue_next(player_t *p)
{
    if (p->radio || p->pod) return;    /* the queue waits for the radio (or an episode) to be left */
    engine_track_t q[ENGINE_AHEAD];
    char path[ENGINE_AHEAD][sizeof ((lib_track_t *)0)->path];
    int n = 0;
    uint32_t ms = 0;
    int looked = 0;
    for (int was = p->pos, np = after(p, p->pos);
         np >= 0 && n < ENGINE_AHEAD && ms < AHEAD_MS && looked++ < p->n_order + ENGINE_AHEAD;
         was = np, np = after(p, np)) {
        lib_track_t t;
        /* one a scan has removed since the queue was made: passed over (it
         * stopped the lining up there, and playback with it) */
        if (library_track(p->lib, id_at(p, np), &t) != 0) continue;
        /* sleep timer, end of album: the next is another album, or the same
         * one again (repeat one, or all on one album: it would never end) */
        if (p->sleep_album && (t.album_id != p->sleep_album || np <= was)) break;
        if (!++p->last_tag) p->last_tag = 1;   /* (0 is engine_play's) */
        snprintf(path[n], sizeof path[n], "%s", t.path);
        q[n] = (engine_track_t){path[n], t.id, track_gain(p, t.id), p->last_tag};
        p->ahead[n].pos = np;
        p->ahead[n].tag = p->last_tag;
        ms += t.duration_ms;
        n++;
    }
    p->n_ahead = n;
    p->next_id = n ? q[0].id : -1;
    snprintf(p->next_path, sizeof p->next_path, "%s", n ? path[0] : "");
    engine_queue_ahead(p->eng, q, n);
}

/* ---------- the next track's file, read ahead ----------
 * The engine first touches the next track's file when the current one ends,
 * with about 200 ms of audio queued: on a card that has to be read for it (the
 * folder, the FAT, the file's first blocks) that is most of the margin of a
 * gapless join. So the kernel is asked for the file's start beforehand: 512 KB
 * once the current track has under 10 s left, and 128 KB as each track starts,
 * so that Next starts from memory too. Only asked (an open and an advisory
 * call: the kernel reads in its own time, this thread reads nothing), once
 * for each track and successor; files on the card only, never a stream. */
#define WARM_END_MS 10000
#define WARM_END_BYTES (512 * 1024)
#define WARM_START_BYTES (128 * 1024)

static void (*warm_hook)(const char *file, long bytes);

void player_set_warm_hook(void (*hook)(const char *file, long bytes)) { warm_hook = hook; }

/* A track's file: a CUE track's is the file it's a part of. */
static const char *track_file(const char *path, char *buf, size_t cap)
{
    uint32_t cs, ce;
    return cue_part_parse(path, buf, cap, &cs, &ce) > 0 ? buf : path;
}

static void warm_file(const char *file, long bytes)
{
    if (file[0] != '/' || strstr(file, "://")) return;   /* (a file on the card: nothing is fetched for this) */
    if (warm_hook) warm_hook(file, bytes);
#ifdef POSIX_FADV_WILLNEED
    uint32_t t0 = lat_ms();
    int fd = open(file, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;
    posix_fadvise(fd, 0, (off_t)bytes, POSIX_FADV_WILLNEED);
    close(fd);
    LAT("player: next file asked for ahead, %ld KB in %u ms: %s", bytes / 1024, lat_ms() - t0, file);
#endif
}

/* From the tick, the engine playing the current track: the successor's file
 * asked for, a little at the start and more near the end. */
static void warm_next(player_t *p, const eng_status_t *st)
{
    if (!p->n_ahead || !p->next_path[0] || !p->have_cur) return;
    if (st->state != ENG_PLAYING || st->starting || st->track_id != p->cur.id) return;
    int level = st->len_ms && st->pos_ms + WARM_END_MS >= st->len_ms ? 2 : 1;
    if (p->warm_seq == st->seq && p->warm_id == p->next_id && p->warm_level >= level) return;
    p->warm_seq = st->seq;
    p->warm_id = p->next_id;
    p->warm_level = level;
    char nf[sizeof p->next_path], cf[sizeof p->cur.path];
    const char *next = track_file(p->next_path, nf, sizeof nf), *cur = track_file(p->cur.path, cf, sizeof cf);
    if (!strcmp(next, cur)) return;     /* (the same file: repeat one, the next part of a CUE album) */
    warm_file(next, level == 2 ? WARM_END_BYTES : WARM_START_BYTES);
}

static void set_pos(player_t *p, int order_pos)
{
    p->pos = order_pos;
    if (p->user_end < p->pos + 1) p->user_end = p->pos + 1;
}

/* Leaving a long file: keep its position for next time. */
static void remember_position(player_t *p)
{
    if (!p->have_cur || p->cur.duration_ms < LONG_FILE_MS) return;
    eng_status_t st;
    engine_status(p->eng, &st);
    uint32_t at;
    if (st.state != ENG_STOPPED && st.track_id == p->cur.id) at = st.pos_ms;
    /* cued, not started (a restored queue: switched off in an audiobook): its
     * place was only in resume_ms, lost when something else was played */
    else if (st.state == ENG_STOPPED && p->resume_ms && !p->radio && !p->pod) at = p->resume_ms;
    else return;
    if (at < LONG_FILE_EDGE_MS || at + LONG_FILE_EDGE_MS > p->cur.duration_ms) at = 0;
    library_set_resume(p->lib, p->cur.id, at);
}

/* Start the track at order_pos; a long file resumes where it was left (if `resume`). */
static void radio_leave(player_t *p);
static void radio_asked(player_t *p, int afresh);
static void pod_leave(player_t *p);

/* How a track is gone to: started, started paused, or only cued (the engine
 * left stopped, as after a restart: Play starts it). */
enum { GO_PLAY, GO_PAUSED, GO_CUE };

/* resume: a long file from where it was left (library_resume); at: from there (ms; 0 none).
 * Returns 1 when it went to a track, 0 when there was none to go to. */
static int go_to(player_t *p, int order_pos, int resume, uint32_t at, int how)
{
    if (order_pos < 0 || order_pos >= p->n_order) return 0;
    radio_leave(p);
    pod_leave(p);
    /* a track a scan has removed since: the next one still there (none:
     * left as it was, rather than an empty Now Playing that Play can't start) */
    lib_track_t t;
    while (order_pos < p->n_order && library_track(p->lib, id_at(p, order_pos), &t) != 0) order_pos++;
    if (order_pos >= p->n_order) return 0;
    remember_position(p);
    p->fails = 0;
    set_pos(p, order_pos);
    p->cur = t;
    p->have_cur = 1;
    p->resume_ms = 0;
    p->own_start = 1;
    /* end of album, set for one and another chosen: the end of this one (the
     * old one's id would stop it after its first track) */
    if (p->sleep_album && p->cur.album_id) p->sleep_album = p->cur.album_id;
    if (how == GO_CUE) {                /* (a long file's place: start_cued looks it up) */
        if (!resume && p->cur.duration_ms >= LONG_FILE_MS) library_set_resume(p->lib, p->cur.id, 0);
        p->own_start = 0;
        p->resume_ms = at;
        p->seq++;
        return 1;
    }
    if (!at && resume && p->cur.duration_ms >= LONG_FILE_MS) at = library_resume(p->lib, p->cur.id);
    /* from there: nothing before it heard; paused: nothing of it at all until
     * the resume (said with the play: a pause after it came too late) */
    engine_play_at(p->eng, p->cur.path, p->cur.id, track_gain(p, p->cur.id), at, how == GO_PAUSED);
    p->seq++;
    queue_next(p);
    return 1;
}

static void start_at_opt(player_t *p, int order_pos, int resume, uint32_t at)
{
    go_to(p, order_pos, resume, at, GO_PLAY);
}

static void start_at(player_t *p, int order_pos) { start_at_opt(p, order_pos, 1, 0); }

void player_play(player_t *p, const int64_t *ids, int n, int start)
{
    p->all_pending = 0;
    set_ctx(p, ids, n);
    build_order(p, p->shuffle ? start : -1, NULL, 0);
    p->user_end = 0;
    start_at(p, p->shuffle ? 0 : start);   /* shuffled: the chosen track is first */
}

void player_cue(player_t *p, const int64_t *ids, int n, int start)
{
    p->all_pending = 0;
    if (p->radio) { engine_stop(p->eng); radio_leave(p); }
    if (p->pod) { engine_stop(p->eng); pod_leave(p); }
    set_ctx(p, ids, n);
    build_order(p, p->shuffle ? start : -1, NULL, 0);
    p->user_end = 0;
    set_pos(p, p->shuffle ? 0 : start);
    if (library_track(p->lib, id_at(p, p->pos), &p->cur) == 0) {
        p->have_cur = 1;
        p->seq++;
    }
}

/* ---- Shuffle all on a big library: a song at once, the order after ----
 * Every song is listed, read and put in order before the first one could
 * start: 2.6 s on the H2 with 50,000 songs, in silence, on Home. Instead a
 * first song is picked from a dozen taken at random (one the order could
 * start with: not an interlude or spoken word where those are left out, the
 * longest unheard of them when freshness counts, its album's first track
 * when whole albums are shuffled) and started as a queue of one; the whole
 * order is built around it at the next tick (shuffle_all_rest), the way
 * turning shuffle on rebuilds around the playing track. Anything that looks
 * at the queue before then builds it first. */
#define SHUFFLE_SOON_MIN 5000   /* (songs: below, the whole build is a fraction of a second) */
static int64_t shuffle_soon_min = SHUFFLE_SOON_MIN;

void player_test_shuffle_soon(int min_songs) { shuffle_soon_min = min_songs > 0 ? min_songs : SHUFFLE_SOON_MIN; }

static int64_t shuffle_first(player_t *p)
{
    int64_t top = library_top_track(p->lib);
    if (top < shuffle_soon_min) return 0;
    enum { TRIES = 12 };
    int64_t ids[TRIES];
    shuf_track_t t[TRIES];
    int n = 0;
    uint64_t r = rng_os_seed() | 1;
    for (int i = 0; i < TRIES; i++) {
        r ^= r << 13; r ^= r >> 7; r ^= r << 17;      /* (xorshift: which dozen, nothing more) */
        int64_t id = library_track_from(p->lib, 1 + (int64_t)(r % (uint64_t)top));
        if (id > 0) ids[n++] = id;
    }
    if (n == 0 || library_shuffle_tracks_for(p->lib, ids, n, t) != n) return 0;
    shuf_opts_t o;
    shuf_default_opts(&o, now_s());
    int best = -1;
    for (int i = 0; i < n; i++) {
        if (!t[i].duration_ms) continue;                                        /* (gone, or nothing known of it) */
        if (p->sprefs.exclude_spoken && (t[i].flags & SHUF_F_SPOKEN)) continue;
        if (p->sprefs.skip_interludes && t[i].duration_ms < o.interlude_ms) continue;
        if (best < 0 || (p->sprefs.freshness && t[i].last_played < t[best].last_played)) best = i;
    }
    if (best < 0) return 0;
    int64_t id = ids[best];
    if (p->sprefs.albums) {                            /* whole albums: from its first track */
        lib_list_t al;
        if (library_list(p->lib, LIB_ALBUM_TRACKS, t[best].album, &al) > 0) id = al.ids[0];
        library_list_free(&al);
    }
    return id;
}

/* The rest of a Shuffle all that started on one song (all_pending) */
static void shuffle_all_rest(player_t *p)
{
    if (!p->all_pending) return;
    p->all_pending = 0;
    if (p->radio || p->pod || !p->shuffle || p->n != 1 || p->n_order != 1) return;   /* (something else was started since) */
    int64_t cur = p->ctx[0];
    lib_list_t l;
    if (library_list(p->lib, LIB_SONGS, 0, &l) <= 0) return;
    int at = -1;
    for (int i = 0; i < l.n; i++)
        if (l.ids[i] == cur) { at = i; break; }
    if (at < 0) { library_list_free(&l); return; }    /* (removed meanwhile: the queue of one stays) */
    set_ctx(p, l.ids, l.n);
    library_list_free(&l);
    build_order(p, at, NULL, 0);
    if (p->n_order == 0 || p->order[0] != at) {        /* (no order made, no memory: the one song again) */
        set_ctx(p, &cur, 1);
        build_order(p, 0, NULL, 0);
    }
    p->pos = 0;
    p->user_end = 0;
    p->seq++;
    queue_next(p);
}

void player_shuffle_all(player_t *p)
{
    p->all_pending = 0;
    int64_t first = shuffle_first(p);
    if (first > 0) {
        p->shuffle = 1;
        set_ctx(p, &first, 1);
        build_order(p, 0, NULL, 0);
        if (p->n_order == 1) {
            p->user_end = 0;
            start_at(p, 0);
            p->all_pending = 1;
            return;
        }
    }
    lib_list_t l;
    if (library_list(p->lib, LIB_SONGS, 0, &l) <= 0) return;
    p->shuffle = 1;
    set_ctx(p, l.ids, l.n);
    library_list_free(&l);
    build_order(p, -1, NULL, 0);
    p->user_end = 0;
    start_at(p, 0);
}

int player_shuffle_these(player_t *p, const int64_t *ids, int n, int songs)
{
    p->all_pending = 0;
    if (n <= 0) return 0;
    p->shuffle = 1;
    set_ctx(p, ids, n);
    p->ctx_chosen = 1;
    p->ctx_songs = songs;
    build_order(p, -1, NULL, 0);
    if (p->n_order == 0) {                      /* only interludes: those, then */
        p->ctx_all = 1;
        build_order(p, -1, NULL, 0);
    }
    if (p->n_order == 0) return 0;
    p->user_end = 0;
    start_at(p, 0);
    return p->n_order;
}

/* Start the cued track (from where a restored queue left off). */
static void start_cued(player_t *p)
{
    start_at_opt(p, p->pos, 1, p->resume_ms);
}

static void radio_connect(player_t *p);
static void pod_start(player_t *p);

void player_toggle_pause(player_t *p)
{
    if (p->radio) { player_pause(p, p->r_want); return; }
    if (p->pod) {
        eng_status_t st;
        engine_status(p->eng, &st);
        player_pause(p, st.starting ? !st.start_paused : st.state == ENG_PLAYING);
        return;
    }
    eng_status_t st;
    engine_status(p->eng, &st);
    if (st.starting) engine_pause(p->eng, !st.start_paused);   /* about to start: flip how */
    else if (st.state == ENG_PLAYING) engine_pause(p->eng, 1);
    else if (st.state == ENG_PAUSED) engine_pause(p->eng, 0);
    else if (p->have_cur) start_cued(p);
}

void player_pause(player_t *p, int on)
{
    if (p->radio) {                     /* live: stop, and connect again to go on */
        if (on && p->r_want) {
            p->r_want = 0;
            p->r_retry_at = 0;
            engine_stop(p->eng);
        } else if (!on && !p->r_want) {
            p->r_tries = 0;
            radio_asked(p, 1);
            radio_connect(p);
        }
        return;
    }
    eng_status_t st;
    engine_status(p->eng, &st);
    if (p->pod) {
        if (st.starting) engine_pause(p->eng, on);
        else if (on && st.state == ENG_PLAYING) engine_pause(p->eng, 1);
        else if (!on && st.state == ENG_PAUSED) engine_pause(p->eng, 0);
        else if (!on && st.state == ENG_STOPPED) pod_start(p);   /* from where it was left (or the start) */
        return;
    }
    if (st.starting) engine_pause(p->eng, on);           /* about to start: start paused or not */
    else if (on && st.state == ENG_PLAYING) engine_pause(p->eng, 1);
    else if (!on && st.state == ENG_PAUSED) engine_pause(p->eng, 0);
    else if (!on && st.state == ENG_STOPPED && p->have_cur) start_cued(p);
}

/* The current track is being left: record it as played or as an early skip.
 * In two halves for a skip: what it was taken before the next track starts,
 * and written (the database, on the card) once the engine has it, so the
 * next track doesn't wait for the write.
 * A skip counts only for a track the engine is on: one that is only cued
 * (after "stop after this track" or a sleep timer's end, after a restart),
 * one still starting, or the last of the queue after it ended (its play is
 * counted already) was never heard, and the engine's position is the track
 * before's: it made a play, or an early skip, of a song nobody listened to. */
typedef struct { int64_t id; int early, valid; } played_t;

static played_t record_take(player_t *p, int skipped_by_user)
{
    played_t r = {0};
    if (!p->have_cur || p->radio || p->pod) return r;
    eng_status_t st;
    engine_status(p->eng, &st);
    if (skipped_by_user && (st.state == ENG_STOPPED || st.starting || st.track_id != p->cur.id)) return r;
    r.id = p->cur.id;
    r.early = skipped_by_user && st.pos_ms < EARLY_SKIP_MS;
    r.valid = 1;
    return r;
}

static void record_put(player_t *p, const played_t *r)
{
    if (r->valid) library_played(p->lib, r->id, r->early, now_s());
}

static void record(player_t *p, int skipped_by_user)
{
    played_t r = record_take(p, skipped_by_user);
    record_put(p, &r);
}

/* Start a new shuffle cycle after the last one ran out (repeat all). */
static int new_cycle(player_t *p)
{
    if (!p->shuffle || p->n_order == 0) return -1;
    uint32_t tail[SEAM];
    size_t tn = 0;
    for (int i = p->n_order - SEAM < 0 ? 0 : p->n_order - SEAM; i < p->n_order; i++)
        tail[tn++] = (uint32_t)id_at(p, i);
    build_order(p, -1, tail, tn);
    p->user_end = 0;
    return 0;
}

static void radio_station(player_t *p, int i);

/* How Next and Prev go to a track: playing on if it was playing, and paused
 * if it was paused (a press in a pocket starts nothing), or only cued if
 * nothing was started (as after a restart, or the queue at its end). */
static int skip_how(player_t *p)
{
    eng_status_t st;
    engine_status(p->eng, &st);
    if (st.starting) return st.start_paused ? GO_PAUSED : GO_PLAY;
    if (st.state == ENG_PAUSED) return GO_PAUSED;
    if (st.state == ENG_STOPPED) return GO_CUE;
    return GO_PLAY;
}

/* Radio: the station before or after; stopped by hand (not connected), only
 * chosen. One that was given up ("Can't reach the station") was wanted: the
 * next is played, as it would have been a moment before (it was only chosen,
 * "Stopped", and Play had to follow: seen on the H2, 2026-11-24). */
static void radio_skip(player_t *p, int by)
{
    int i = (p->ri + p->rn + by) % p->rn;
    if (p->r_want || p->r_failed) { radio_station(p, i); return; }
    p->ri = i;
    p->r_tries = 0;
    p->r_failed = 0;
    p->r_lost = 0;
    p->r_meta[0] = p->r_meta_url[0] = p->r_artist[0] = p->r_title[0] = p->r_art[0] = '\0';
    p->seq++;
}

void player_next(player_t *p)
{
    shuffle_all_rest(p);
    if (p->radio) { radio_skip(p, 1); return; }
    if (p->pod) { player_seek_by(p, PLAYER_SKIP_FWD_MS); return; }
    if (!p->have_cur) return;
    catch_up(p);
    int how = skip_how(p);
    played_t was = record_take(p, 1);
    int went = 0;
    if (p->pos + 1 < p->n_order) went = go_to(p, p->pos + 1, 1, 0, how);
    else if (p->repeat == REPEAT_ALL && new_cycle(p) == 0) went = go_to(p, 0, 1, 0, how);
    else if (p->repeat == REPEAT_ALL) went = go_to(p, 0, 1, 0, how);
    if (went) record_put(p, &was);      /* (the end of the queue, repeat off: nothing was left) */
}

void player_prev(player_t *p)
{
    shuffle_all_rest(p);
    if (p->radio) { radio_skip(p, -1); return; }
    if (p->pod) { player_seek_by(p, -PLAYER_SKIP_BACK_MS); return; }
    if (!p->have_cur) return;
    catch_up(p);
    int how = skip_how(p);
    eng_status_t st;
    engine_status(p->eng, &st);
    uint32_t in = how == GO_CUE ? p->resume_ms : st.pos_ms;
    if (in > RESTART_MS || p->pos == 0) go_to(p, p->pos, 0, 0, how);   /* restart: from the top */
    else go_to(p, p->pos - 1, 1, 0, how);
}

void player_seek_by(player_t *p, int delta_ms)
{
    eng_status_t st;
    engine_status(p->eng, &st);
    if (st.state == ENG_STOPPED || !st.len_ms || p->radio || (p->pod && st.stream)) return;
    int64_t to = (int64_t)st.pos_ms + delta_ms;
    if (to < 0) to = 0;
    if (to > (int64_t)st.len_ms - 500) to = (int64_t)st.len_ms - 500;
    if (to < 0) to = 0;
    engine_seek_ms(p->eng, (uint32_t)to);
}

void player_set_shuffle_prefs(player_t *p, const player_shuffle_prefs_t *prefs)
{
    if (p && prefs) p->sprefs = *prefs;
}

void player_set_shuffle(player_t *p, int on)
{
    shuffle_all_rest(p);
    if (p->shuffle == on) return;
    catch_up(p);
    p->shuffle = on;
    if (!p->have_cur || p->n == 0) return;
    /* keep the current track playing; rebuild the order around it */
    int cur_ctx = p->order[p->pos];
    build_order(p, on ? cur_ctx : -1, NULL, 0);
    p->pos = 0;
    if (!on)
        for (int i = 0; i < p->n_order; i++)
            if (p->order[i] == cur_ctx) { p->pos = i; break; }
    p->user_end = p->pos + 1;   /* what was queued by hand is now part of the order */
    queue_next(p);
}

int player_shuffle(const player_t *p) { return p->shuffle; }

void player_set_repeat(player_t *p, repeat_t r)
{
    shuffle_all_rest(p);
    catch_up(p);
    p->repeat = r;
    if (p->have_cur) queue_next(p);
}

repeat_t player_repeat(const player_t *p) { return p->repeat; }

void player_set_stop_after(player_t *p, int on)
{
    catch_up(p);                        /* ("this track" is the one playing) */
    p->stop_after = !!on;
    if (p->have_cur) queue_next(p);
}

int player_stop_after(const player_t *p) { return p->stop_after; }

void player_set_sleep(player_t *p, int minutes)
{
    catch_up(p);
    int was_album = p->sleep_album != 0;
    p->sleep_album = 0;
    if (minutes == PLAYER_SLEEP_END_ALBUM && p->have_cur && p->cur.album_id) {
        p->sleep_min = PLAYER_SLEEP_END_ALBUM;     /* stops when the next track is from another album */
        p->sleep_at = 0;
        p->sleep_album = p->cur.album_id;
    } else {
        p->sleep_min = minutes > 0 ? minutes : 0;
        p->sleep_at = minutes > 0 ? mono_ms() + (uint64_t)minutes * 60000 : 0;
    }
    engine_set_fade(p->eng, 1.0f);
    if ((was_album || p->sleep_album) && p->have_cur) queue_next(p);   /* line up (or not) the next track */
}

int player_sleep_minutes(const player_t *p) { return p->sleep_min; }

/* ---------- the queue ---------- */

static int insert_at(player_t *p, int at, const int64_t *ids, int n)
{
    if (n <= 0) return 0;
    if (grow((void **)&p->ctx, &p->cap_ctx, p->n + n, sizeof *p->ctx) ||
        grow((void **)&p->order, &p->cap_order, p->n_order + n, sizeof *p->order))
        return -1;
    memmove(&p->order[at + n], &p->order[at], (size_t)(p->n_order - at) * sizeof *p->order);
    for (int i = 0; i < n; i++) {
        p->ctx[p->n] = ids[i];
        p->order[at + i] = p->n++;
    }
    p->n_order += n;
    return 0;
}

/* Nothing loaded yet: the added tracks become the queue, cued. */
static int start_queue_if_empty(player_t *p, const int64_t *ids, int n)
{
    if (p->have_cur) return 0;
    player_cue(p, ids, n, 0);
    return 1;
}

void player_play_next(player_t *p, const int64_t *ids, int n)
{
    shuffle_all_rest(p);
    if (start_queue_if_empty(p, ids, n)) return;
    catch_up(p);                        /* (after what plays now) */
    if (insert_at(p, p->pos + 1, ids, n)) return;
    p->user_end += n;
    queue_next(p);
}

void player_add_to_queue(player_t *p, const int64_t *ids, int n)
{
    shuffle_all_rest(p);
    if (start_queue_if_empty(p, ids, n)) return;
    catch_up(p);
    int at = p->user_end > p->pos + 1 ? p->user_end : p->pos + 1;
    if (at > p->n_order) at = p->n_order;
    if (insert_at(p, at, ids, n)) return;
    p->user_end = at + n;
    queue_next(p);
}

int player_queue_len(const player_t *p)
{
    return p->have_cur && p->n_order > p->pos + 1 ? p->n_order - p->pos - 1 : 0;
}

int64_t player_queue_at(const player_t *p, int i)
{
    return id_at(p, p->pos + 1 + i);
}

void player_queue_move(player_t *p, int i, int dir)
{
    shuffle_all_rest(p);
    int a = p->pos + 1 + i, b = a + dir;
    if (a <= p->pos || b <= p->pos || a >= p->n_order || b >= p->n_order) return;
    int t = p->order[a];
    p->order[a] = p->order[b];
    p->order[b] = t;
    queue_next(p);
}

void player_queue_remove(player_t *p, int i)
{
    shuffle_all_rest(p);
    int a = p->pos + 1 + i;
    if (a <= p->pos || a >= p->n_order) return;
    memmove(&p->order[a], &p->order[a + 1], (size_t)(p->n_order - a - 1) * sizeof *p->order);
    p->n_order--;
    if (p->user_end > a) p->user_end--;
    queue_next(p);
}

void player_queue_clear(player_t *p)
{
    shuffle_all_rest(p);
    if (!p->have_cur) return;
    catch_up(p);
    p->n_order = p->pos + 1;
    p->user_end = p->pos + 1;
    queue_next(p);
}

void player_queue_jump(player_t *p, int i)
{
    shuffle_all_rest(p);
    int a = p->pos + 1 + i;
    if (a <= p->pos || a >= p->n_order) return;
    played_t was = record_take(p, 1);
    if (go_to(p, a, 1, 0, GO_PLAY)) record_put(p, &was);
}

/* ---------- saved queue ----------
 * queue.txt: the queue (shuffle, repeat, the context and the play order). It's
 * written again only when one of those changed (a new file renamed over the
 * old: core/fsafe.h): not at every track change, which for a shuffle of
 * 50,000 songs was 600 KB printed and synced on the UI thread each time.
 * Where playback is in it goes to PATH.pos, a small record written over
 * itself in place (at every track change, and every 10 s while playing: no
 * file replaced, nothing for a power cut to leave half done):
 *   fathom-pos ID MS POS USER_END
 * the track, the position in it, its place in the order, and where what was
 * queued by hand ends. Restoring takes the place from the record when the
 * track at POS in the restored order is ID; otherwise (no record, one from
 * another queue, a cut between the two writes) from queue.txt, which has the
 * place as it was when the queue was last written. A record from before
 * (fathom-pos ID MS) gives the position when ID is the track at queue.txt's
 * place. An order that is just the context, first to last (shuffle off,
 * nothing queued by hand), is written as "order identity". */

/* The order is the context itself, each track once and in its own order. */
static int order_is_identity(const player_t *p)
{
    if (p->n_order != p->n) return 0;
    for (int i = 0; i < p->n_order; i++)
        if (p->order[i] != i) return 0;
    return 1;
}

static uint64_t queue_hash(const player_t *p)
{
    uint64_t h = 14695981039346656037ULL;
    const int head[4] = {p->shuffle, (int)p->repeat, p->n, p->n_order};   /* (not the place: that's PATH.pos's) */
#define HASH_BYTES(ptr, len) do { const unsigned char *b_ = (const unsigned char *)(ptr); \
        for (size_t i_ = 0; i_ < (size_t)(len); i_++) h = (h ^ b_[i_]) * 1099511628211ULL; } while (0)
    HASH_BYTES(head, sizeof head);
    HASH_BYTES(p->ctx, (size_t)p->n * sizeof *p->ctx);
    HASH_BYTES(p->order, (size_t)p->n_order * sizeof *p->order);
#undef HASH_BYTES
    return h | 1;
}

static void save_position(const player_t *p, const char *path, uint32_t at)
{
    char pos[620], rec[64];   /* (at most 57 characters: file_put_small takes 62) */
    snprintf(pos, sizeof pos, "%s.pos", path);
    snprintf(rec, sizeof rec, "fathom-pos %lld %u %d %d", (long long)p->cur.id, at, p->pos, p->user_end);
    file_put_small(pos, rec, 1);
}

/* Where the queue's current track is, for the saved queue: the engine's
 * position only when the engine is on that track (player_now()'s rule). While
 * it still starts, the engine's figures are the track before's: saved then, a
 * power cut restored the new track at the old one's position. Cued or about
 * to start: where it starts. The radio or an episode on: where the queue was
 * left. */
static uint32_t queue_position(const player_t *p, const eng_status_t *st)
{
    if (p->radio || p->pod) return p->resume_ms;
    if (st->state != ENG_STOPPED && !st->starting && st->track_id == p->cur.id) return st->pos_ms;
    if (st->starting && st->start_ms) return st->start_ms;
    return st->state == ENG_STOPPED || st->starting ? p->resume_ms : 0;
}

int player_save(player_t *p, const char *path)
{
    shuffle_all_rest(p);
    if (!p->have_cur || !p->n_order) return 0;
    eng_status_t st;
    engine_status(p->eng, &st);
    uint32_t at = queue_position(p, &st);
    uint64_t hash = queue_hash(p);
    if (hash == p->saved_hash) {                     /* the same queue: where it is, only */
        save_position(p, path, at);
        return 0;
    }
    char tmp[600];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "we");
    if (!f) return -1;
    fprintf(f, "fathom-queue 1\nshuffle %d\nrepeat %d\npos %d\nuser_end %d\nposition_ms %u\nctx %d",
            p->shuffle, (int)p->repeat, p->pos, p->user_end, at, p->n);
    for (int i = 0; i < p->n; i++) fprintf(f, " %lld", (long long)p->ctx[i]);
    if (order_is_identity(p)) {
        fprintf(f, "\norder identity");
    } else {
        fprintf(f, "\norder %d", p->n_order);
        for (int i = 0; i < p->n_order; i++) fprintf(f, " %d", p->order[i]);
    }
    fprintf(f, "\n");
    int ok = fflush(f) == 0 && fsync(fileno(f)) == 0;
    ok = fclose(f) == 0 && ok;
    if (!ok || file_replace(tmp, path)) { unlink(tmp); return -1; }
    p->saved_hash = hash;
    save_position(p, path, at);
    return 0;
}

int player_restore(player_t *p, const char *path)
{
    p->all_pending = 0;
    FILE *f = fopen(path, "re");
    if (!f) return -1;
    int version = 0, shuffle = 0, repeat = 0, pos = 0, user_end = 0, n = 0, no = 0;
    unsigned at = 0;
    int ok = fscanf(f, "fathom-queue %d shuffle %d repeat %d pos %d user_end %d position_ms %u ctx %d",
                    &version, &shuffle, &repeat, &pos, &user_end, &at, &n) == 7 &&
             version == 1 && n > 0 && n < 1000000;
    int64_t *ctx = ok ? malloc((size_t)n * sizeof *ctx) : NULL;
    for (int i = 0; ok && i < n; i++) {
        long long v;
        ok = ctx && fscanf(f, "%lld", &v) == 1;
        if (ok) ctx[i] = v;
    }
    char word[16] = "";                              /* the order: how many and each, or "identity" */
    ok = ok && fscanf(f, " order %15s", word) == 1;
    int identity = ok && !strcmp(word, "identity");
    if (identity) {
        no = n;
    } else if (ok) {
        char *end;
        long v = strtol(word, &end, 10);
        ok = end != word && !*end && v > 0 && v < 2000000;
        no = ok ? (int)v : 0;
    }
    int *order = ok ? malloc((size_t)no * sizeof *order) : NULL;
    for (int i = 0; ok && i < no; i++) {
        if (identity && order) order[i] = i;
        else ok = order && fscanf(f, "%d", &order[i]) == 1 && order[i] >= 0 && order[i] < n;
    }
    fclose(f);
    if (ok) {   /* where it was, as last noted (player_save) */
        char posf[620];
        long long pid = 0;
        unsigned ms = 0;
        int rpos = -1, rend = -1;
        snprintf(posf, sizeof posf, "%s.pos", path);
        FILE *pf = fopen(posf, "re");
        int got = pf ? fscanf(pf, "fathom-pos %lld %u %d %d", &pid, &ms, &rpos, &rend) : 0;
        if (pf) fclose(pf);
        if (got == 4) {
            /* the place too, when it's this queue's: that track is there */
            if (rpos >= 0 && rpos < no && (long long)ctx[order[rpos]] == pid) {
                pos = rpos;
                user_end = rend < pos + 1 ? pos + 1 : rend > no ? no : rend;
                at = ms;
            }
        } else if (got >= 2 && pos >= 0 && pos < no && (long long)ctx[order[pos]] == pid) {
            at = ms;                                 /* (a record from before: the position only) */
        }
    }
    ok = ok && pos >= 0 && pos < no;
    lib_track_t t;
    ok = ok && library_track(p->lib, ctx[order[pos]], &t) == 0;   /* the track must still exist */
    if (!ok) { free(ctx); free(order); return -1; }

    if (p->radio) { engine_stop(p->eng); radio_leave(p); }
    if (p->pod) { engine_stop(p->eng); pod_leave(p); }
    free(p->ctx);
    free(p->order);
    p->ctx = ctx; p->n = p->cap_ctx = n;
    p->n_ahead = 0;
    p->order = order; p->n_order = p->cap_order = no;
    p->shuffle = !!shuffle;
    p->repeat = repeat >= REPEAT_OFF && repeat <= REPEAT_ONE ? (repeat_t)repeat : REPEAT_OFF;
    p->pos = pos;
    p->user_end = user_end;
    p->cur = t;
    p->have_cur = 1;
    p->resume_ms = at;
    p->saved_hash = queue_hash(p);                   /* (as the file has it) */
    p->seq++;
    return 0;
}

/* ---------- internet radio ---------- */

#define RADIO_STEADY_MS 30000   /* playing this long: a later drop starts the retries afresh */
#define RADIO_TRIES 6
/* A station that hasn't played yet since it was asked for (Play on it, Next,
 * Play again after it was given up) is given up sooner: one that fails at
 * once (its server refuses, its name doesn't exist) after the first
 * connection and three more, 6.5 s; one that takes its time to fail (nothing
 * answers: 10 s a connection) at the first failure 20 s or more after it was
 * asked for. It said "Connecting..." for a minute, and two, before. A station
 * that was playing and dropped keeps all its tries: that's where waiting
 * helps. */
#define RADIO_TRIES_NEW 3
#define RADIO_NEW_MS 20000
#define RADIO_HEARD_MS 5000     /* playing this long without a break: it has "played" */
/* With the network gone (player_radio_net) a station that's wanted waits for
 * it without trying, this long: then it's given up, so that the player counts
 * as idle and can turn itself off. It comes back with the network all the
 * same. */
#define RADIO_OFFLINE_MS (10 * 60000)
/* A station given up this soon before the network was seen to go was lost to
 * it too: the link is noticed gone some time after it stops carrying anything. */
#define RADIO_LAG_MS 120000
static int radio_pace = 1;      /* tests: all these waits so many times shorter */

static int radio_new_ms = RADIO_NEW_MS;   /* (not shortened with the waits: a test sets it) */

void player_radio_test_pace(int times) { radio_pace = times > 0 ? times : 1; }
void player_radio_test_new_ms(int ms) { radio_new_ms = ms > 0 ? ms : RADIO_NEW_MS; }

/* The station is asked for afresh (by the listener, or by the network coming
 * back to one not heard yet): its short patience starts now. */
static void radio_asked(player_t *p, int afresh)
{
    if (afresh) p->r_heard = 0;
    p->r_asked_at = mono_ms();
    p->r_sound_at = 0;
}

static void radio_connect(player_t *p)
{
    const radio_station_t *s = &p->rst[p->ri];
    p->r_want = 1;
    p->r_failed = 0;
    p->r_lost = 0;
    p->r_parked_at = 0;
    p->r_began_down = p->r_down;
    p->r_sleep_at = p->sleep_at;
    p->r_retry_at = 0;
    p->r_since = mono_ms();
    p->own_start = 1;
    engine_play(p->eng, s->url, -2 - p->ri);
    p->seq++;
}

static void radio_station(player_t *p, int i)
{
    if (i < 0 || i >= p->rn) return;
    p->ri = i;
    p->r_tries = 0;
    radio_asked(p, 1);
    p->r_meta[0] = p->r_meta_url[0] = p->r_artist[0] = p->r_title[0] = p->r_art[0] = '\0';
    radio_connect(p);
}

void player_radio_play(player_t *p, const radio_station_t *st, int n, int i)
{
    shuffle_all_rest(p);                /* (the queue it comes back to: whole) */
    if (n <= 0 || i < 0 || i >= n) return;
    if (n > RADIO_MAX) n = RADIO_MAX;
    radio_station_t *copy = malloc((size_t)n * sizeof *copy);
    if (!copy) return;
    memcpy(copy, st, (size_t)n * sizeof *copy);
    pod_leave(p);
    if (!p->radio && p->have_cur) {
        /* the queue is kept, cued where it was (saved queues too) */
        eng_status_t es;
        engine_status(p->eng, &es);
        remember_position(p);
        if (es.state != ENG_STOPPED && es.track_id == p->cur.id) p->resume_ms = es.pos_ms;
    }
    engine_queue_next(p->eng, NULL, -1);
    p->n_ahead = 0;
    free(p->rst);
    p->rst = copy;
    p->rn = n;
    p->radio = 1;
    radio_station(p, i);
}

static void radio_leave(player_t *p)
{
    if (!p->radio) return;
    p->radio = 0;
    p->r_want = 0;
    p->r_lost = 0;
    p->r_retry_at = 0;
    p->seq++;
}

/* The network came or went. Gone, a station that's wanted waits (radio_tick);
 * back, it connects at once with its tries afresh, and so does one that was
 * given up for want of the network. One stopped by hand, or given up with the
 * network there (a dead address), stays as it is. */
void player_radio_net(player_t *p, int up)
{
    if (p->r_down == !up) return;
    p->r_down = !up;
    if (!p->radio) return;
    if (!up) {
        if (p->r_failed && !p->r_want && mono_ms() - p->r_failed_at <= (uint64_t)(RADIO_LAG_MS / radio_pace)) p->r_lost = 1;
        if (p->r_want || p->r_lost) fprintf(stderr, "radio: the network went: the station waits for it\n");
        return;
    }
    if (!p->r_want && !p->r_lost) return;
    fprintf(stderr, "radio: the network is back%s\n", p->r_lost ? ": the station given up without it is tried again" : "");
    p->r_tries = 0;
    radio_asked(p, 0);                  /* (one that was playing before it went keeps all its tries) */
    eng_status_t st;
    engine_status(p->eng, &st);
    int on_its_way = st.starting || (st.state != ENG_STOPPED && st.connecting);
    if (p->r_want && st.state != ENG_STOPPED && !on_its_way) return;   /* it never stopped: what it had lasted */
    if (p->r_want && on_its_way && !p->r_began_down) return;           /* begun with the network there: left to it */
    radio_connect(p);                   /* (one begun while the network was gone is begun again) */
}

int player_radio(const player_t *p) { return p->radio; }

const radio_station_t *player_radio_station(const player_t *p)
{
    return p->radio ? &p->rst[p->ri] : NULL;
}

const radio_station_t *player_radio_at(const player_t *p, int i)
{
    return p->radio && i >= 0 && i < p->rn ? &p->rst[i] : NULL;
}

void player_radio_set_info(player_t *p, const char *artist, const char *title, const char *art_url)
{
    if (!p->radio) return;
    const char *a = artist ? artist : "", *t = title ? title : "", *u = art_url ? art_url : "";
    if (!strcmp(a, p->r_artist) && !strcmp(t, p->r_title) && !strcmp(u, p->r_art)) return;
    snprintf(p->r_artist, sizeof p->r_artist, "%s", a);
    snprintf(p->r_title, sizeof p->r_title, "%s", t);
    snprintf(p->r_art, sizeof p->r_art, "%s", u);
    p->seq++;
}

/* A URL that names an image (http(s), ending .jpg/.jpeg/.png before any query). */
static int image_url(const char *u)
{
    if (strncmp(u, "http://", 7) && strncmp(u, "https://", 8)) return 0;
    size_t n = strcspn(u, "?#");
    static const char *const ext[] = {".jpg", ".jpeg", ".png"};
    for (int i = 0; i < 3; i++) {
        size_t k = strlen(ext[i]);
        if (n > k && !strncasecmp(u + n - k, ext[i], k)) return 1;
    }
    return 0;
}

static void radio_tick(player_t *p, const eng_status_t *st)
{
    p->eng_seq = st->seq;
    p->eng_stop = st->stop_seq;
    p->own_start = 0;
    if (st->meta_seq != p->r_meta_seq) {
        p->r_meta_seq = st->meta_seq;
        if (strcmp(st->meta, p->r_meta) || strcmp(st->meta_url, p->r_meta_url)) {
            snprintf(p->r_meta, sizeof p->r_meta, "%s", st->meta);
            snprintf(p->r_meta_url, sizeof p->r_meta_url, "%s", st->meta_url);
            p->seq++;
        }
    }
    uint64_t now = mono_ms();
    /* The sleep timer ran out (it wasn't taken off): with the stream stopped,
     * between tries or waiting for the network, pausing the engine did
     * nothing, and the station came back later. */
    int slept = !p->sleep_at && p->r_sleep_at && now >= p->r_sleep_at;
    p->r_sleep_at = p->sleep_at;
    if (slept) p->r_lost = 0;
    if (!p->r_want) return;
    if (st->state == ENG_PAUSED || slept) {   /* the sleep timer, or the output went away */
        p->r_want = 0;
        p->r_retry_at = 0;
        engine_stop(p->eng);
        engine_set_fade(p->eng, 1.0f);
        p->seq++;
        return;
    }
    if (st->state == ENG_PLAYING && now - p->r_since > (uint64_t)(RADIO_STEADY_MS / radio_pace)) p->r_tries = 0;
    if (st->state == ENG_PLAYING && !st->connecting && !st->starting) {
        if (!p->r_sound_at) p->r_sound_at = now ? now : 1;
        else if (now - p->r_sound_at >= (uint64_t)(RADIO_HEARD_MS / radio_pace)) p->r_heard = 1;
    } else {
        p->r_sound_at = 0;
    }
    if (st->state != ENG_STOPPED || st->starting) return;
    /* the stream stopped (it dropped, or it couldn't connect) */
    if (p->r_down) {
        /* no network: nothing to try, and no try counted. It waits (shown as
         * connecting) for player_radio_net, though not for ever. */
        p->r_retry_at = 0;
        if (!p->r_parked_at) p->r_parked_at = now ? now : 1;
        else if (now - p->r_parked_at >= (uint64_t)(RADIO_OFFLINE_MS / radio_pace)) {
            p->r_want = 0;
            p->r_failed = 1;
            p->r_lost = 1;
            p->seq++;
            fprintf(stderr, "radio: no network for %d s: given up until it's back\n", RADIO_OFFLINE_MS / radio_pace / 1000);
        }
        return;
    }
    p->r_parked_at = 0;
    /* try again, waiting longer each time */
    if (!p->r_retry_at) {
        static const int wait_ms[RADIO_TRIES] = {500, 2000, 4000, 8000, 15000, 30000};
        int over = p->r_heard ? p->r_tries >= RADIO_TRIES
                              : p->r_tries >= RADIO_TRIES_NEW || now - p->r_asked_at >= (uint64_t)radio_new_ms;
        if (over) {
            p->r_want = 0;
            p->r_failed = 1;
            p->r_failed_at = now;
            p->seq++;
            return;
        }
        p->r_retry_at = now + (uint64_t)(wait_ms[p->r_tries++] / radio_pace);
    } else if (now >= p->r_retry_at) {
        radio_connect(p);
    }
}

/* ---------- podcast episodes ---------- */

static void pod_start(player_t *p)
{
    int stream = !strncmp(p->ep.path, "http://", 7) || !strncmp(p->ep.path, "https://", 8);
    engine_set_speed(p->eng, p->speed ? p->speed : 100);
    p->own_start = 1;
    p->ep_ended = 0;
    engine_play_at(p->eng, p->ep.path, POD_ENGINE_ID, 1.0f, stream ? 0 : p->ep_pos, 0);   /* (from there: nothing before it heard) */
    p->seq++;
}

void player_episode_play(player_t *p, const player_episode_t *e)
{
    shuffle_all_rest(p);                /* (the queue it comes back to: whole) */
    if (p->radio) radio_leave(p);
    else if (!p->pod && p->have_cur) {
        /* the queue is kept, cued where it was */
        eng_status_t es;
        engine_status(p->eng, &es);
        remember_position(p);
        if (es.state != ENG_STOPPED && es.track_id == p->cur.id) p->resume_ms = es.pos_ms;
    }
    engine_queue_next(p->eng, NULL, -1);
    p->n_ahead = 0;
    eng_status_t now;
    engine_status(p->eng, &now);
    p->eng_stop = now.stop_seq;                   /* (a stop before this one isn't its end) */
    p->pod = 1;
    p->ep = *e;
    p->ep_pos = e->start_ms;
    pod_start(p);
}

static void pod_leave(player_t *p)
{
    if (!p->pod) return;
    engine_set_speed(p->eng, 100);                /* music as it is */
    p->pod = 0;
    p->ep_ended = 0;
    p->seq++;
}

const player_episode_t *player_episode(player_t *p, uint32_t *pos_ms, uint32_t *len_ms)
{
    if (!p->pod) return NULL;
    eng_status_t st;
    engine_status(p->eng, &st);
    int theirs = st.state != ENG_STOPPED && !st.starting && st.track_id == POD_ENGINE_ID;
    if (pos_ms) *pos_ms = theirs ? st.pos_ms : p->ep_pos;
    if (len_ms) *len_ms = theirs && st.len_ms ? st.len_ms : p->ep.len_ms;
    return &p->ep;
}

void player_set_speed(player_t *p, int pct)
{
    p->speed = pct < 50 ? 50 : pct > 300 ? 300 : pct;
    if (p->pod) engine_set_speed(p->eng, p->speed);
}

int player_speed(const player_t *p) { return p->speed ? p->speed : 100; }

int player_episode_ended(player_t *p)
{
    if (!p->pod || !p->ep_ended) return 0;
    p->ep_ended = 0;
    return 1;
}

static void pod_tick(player_t *p, const eng_status_t *st)
{
    p->eng_seq = st->seq;
    p->own_start = 0;
    if (st->starting) return;
    if (st->state != ENG_STOPPED && st->track_id == POD_ENGINE_ID) {
        p->ep_pos = st->pos_ms;
        if (st->len_ms) p->ep.len_ms = st->len_ms;
    }
    if (st->stop_seq == p->eng_stop || st->state != ENG_STOPPED) return;
    p->eng_stop = st->stop_seq;
    if (st->track_id != POD_ENGINE_ID) return;    /* (what played before it) */
    if (st->error) { p->seq++; return; }          /* (it couldn't be played: it stays cued) */
    p->ep_pos = 0;                                /* played to its end */
    p->ep_ended = 1;
    p->seq++;
}

/* ---------- following the engine ---------- */

static void run_sleep_timer(player_t *p)
{
    if (!p->sleep_at) return;
    uint64_t now = mono_ms();
    if (now >= p->sleep_at) {
        engine_pause(p->eng, 1);
        engine_set_fade(p->eng, 1.0f);
        p->sleep_min = 0;
        p->sleep_at = 0;
    } else if (p->sleep_at - now < SLEEP_FADE_MS) {
        engine_set_fade(p->eng, (float)(p->sleep_at - now) / SLEEP_FADE_MS);
    }
}

/* The current track played to its end. */
static void finished(player_t *p)
{
    record(p, 0);
    if (p->cur.duration_ms >= LONG_FILE_MS) library_set_resume(p->lib, p->cur.id, 0);
}

/* The current track couldn't be played (gone, damaged, a format the output
 * can't take: a 22 kHz MP3 over Bluetooth, 5.1): on to the next one. Not for
 * ever: after a whole queue's worth in a row (an output that takes none of
 * them, with repeat on), it stops there. */
static void skip_failed(player_t *p)
{
    int np = p->pos + 1 < p->n_order ? p->pos + 1 : p->repeat == REPEAT_ALL ? 0 : -1;
    p->fails++;                         /* (the last one counted too: a queue none of which played) */
    int stop = np < 0 || p->fails >= p->n_order;
    eng_status_t st;
    engine_status(p->eng, &st);
    p->fail_seq++;                      /* (said on the screen) */
    p->fail_track = p->cur.id;
    p->fail_output = st.error_output;
    p->fail_memory = st.error_memory;
    p->fail_all = p->fails >= p->n_order;   /* every one in the queue, in a row */
    p->fail_end = stop && !p->fail_all;     /* the last one, others before it played: stopped there */
    fprintf(stderr, "player: track %lld couldn't be played: %s\n", (long long)p->cur.id, stop ? "stopped" : "skipped");
    if (stop) return;
    if (np == 0 && p->shuffle && new_cycle(p)) return;
    int fails = p->fails;
    start_at(p, np);
    p->fails = fails;
}

/* The engine started a track since we last looked: which one in the queue. */
static void follow(player_t *p, const eng_status_t *st)
{
    if (st->starting || st->seq == p->eng_seq) return;
    p->eng_seq = st->seq;
    int ours = p->own_start, j = -1;
    p->own_start = 0;
    for (int i = 0; st->tag && i < p->n_ahead && j < 0; i++)
        if (p->ahead[i].tag == st->tag && id_at(p, p->ahead[i].pos) == st->track_id) j = i;
    if (j >= 0) {
        /* it moved on by itself (gapless) through what we lined up: maybe
         * several tracks, if they were short */
        for (int i = 0; i <= j; i++) {
            finished(p);
            set_pos(p, p->ahead[i].pos);
            library_track(p->lib, id_at(p, p->pos), &p->cur);
        }
        p->seq++;
        queue_next(p);
    } else if (ours && st->track_id == p->cur.id) {
        /* the track we started ourselves */
    } else if (st->track_id >= 0 && st->track_id != p->cur.id) {
        /* the queue changed while the engine switched: find the track it plays */
        for (int i = p->pos + 1; i < p->n_order; i++)
            if (id_at(p, i) == st->track_id) { set_pos(p, i); break; }
        library_track(p->lib, st->track_id, &p->cur);
        p->seq++;
        queue_next(p);
    }
}

/* Before a change to the queue or a skip: the engine may have gone on since
 * the last tick (short tracks; keys come before the tick after a busy spell),
 * and "next" is after what plays now. */
static void catch_up(player_t *p)
{
    if (p->radio || p->pod) return;
    eng_status_t st;
    engine_status(p->eng, &st);
    follow(p, &st);
}

void player_tick(player_t *p)
{
    shuffle_all_rest(p);
    run_sleep_timer(p);
    eng_status_t st;
    engine_status(p->eng, &st);
    if (p->radio) { radio_tick(p, &st); return; }
    if (p->pod) { pod_tick(p, &st); return; }
    if (st.starting) return;            /* (what it says is still about the last track) */
    if (st.state == ENG_PLAYING) p->fails = 0;
    follow(p, &st);
    warm_next(p, &st);
    /* playback stopped by itself: once for each time */
    if (st.stop_seq == p->eng_stop || st.state != ENG_STOPPED) return;
    p->eng_stop = st.stop_seq;
    if (!p->have_cur) return;
    if (st.error) {
        skip_failed(p);
    } else if (p->stop_after || p->sleep_album) {
        /* stopped as asked: cue the next track, so Play carries on */
        p->stop_after = 0;
        p->sleep_album = 0;
        if (p->sleep_min == PLAYER_SLEEP_END_ALBUM) p->sleep_min = 0;
        finished(p);
        if (p->pos + 1 < p->n_order) {
            set_pos(p, p->pos + 1);
            library_track(p->lib, id_at(p, p->pos), &p->cur);
            p->resume_ms = 0;
            p->seq++;
        }
        engine_stop(p->eng);
    } else if (p->repeat == REPEAT_ALL && p->shuffle && p->pos + 1 >= p->n_order) {
        finished(p);                    /* shuffle cycle ran out: a fresh one */
        if (new_cycle(p) == 0) start_at(p, 0);
    } else if (p->n_ahead > 0) {
        /* it played all it had lined up before we came to line up more (more
         * than ENGINE_AHEAD tracks, or AHEAD_MS, while the UI was held up):
         * on with the next, after a gap rather than stopping */
        finished(p);
        start_at(p, p->ahead[0].pos);
    } else {
        finished(p);                    /* the end of the queue */
    }
}

int player_idle(player_t *p)
{
    if (p->sleep_at || (p->radio && p->r_want)) return 0;
    eng_status_t st;
    engine_status(p->eng, &st);
    return st.state != ENG_PLAYING && !st.starting && st.seq == p->eng_seq &&
           (st.stop_seq == p->eng_stop || st.state != ENG_STOPPED);
}

void player_now(player_t *p, player_now_t *now)
{
    memset(now, 0, sizeof *now);
    eng_status_t st;
    engine_status(p->eng, &st);
    now->active = p->have_cur;
    now->playing = st.starting ? !st.start_paused : st.state == ENG_PLAYING;
    now->stopped = !p->have_cur || (st.state == ENG_STOPPED && st.ended);
    now->track = p->cur;
    now->fail_seq = p->fail_seq;
    now->fail_track = p->fail_track;
    now->fail_output = p->fail_output;
    now->fail_memory = p->fail_memory;
    now->fail_all = p->fail_all;
    now->fail_end = p->fail_end;
    /* The engine's figures are for what it plays: not yet this item while it
     * starts, or another one (from a radio station to an album, the station's
     * time showed against the track's length until the album began). */
    int64_t want = p->radio ? -2 - p->ri : p->pod ? POD_ENGINE_ID : p->cur.id;
    int theirs = st.state != ENG_STOPPED && !st.starting && st.track_id == want;
    /* not started yet (or stopped): where it starts; an episode's own place, not the music's */
    uint32_t parked = st.starting && st.start_ms ? st.start_ms : p->pod ? p->ep_pos : p->resume_ms;
    now->pos_ms = theirs ? st.pos_ms : !p->radio && (st.state == ENG_STOPPED || st.starting) ? parked : 0;
    now->len_ms = theirs && st.len_ms ? st.len_ms : p->cur.duration_ms;
    now->shuffle = p->shuffle;
    now->repeat = p->repeat;
    now->stop_after = p->stop_after;
    if (p->sleep_at) {
        uint64_t m = mono_ms();
        now->sleep_s = p->sleep_at > m ? (int)((p->sleep_at - m + 999) / 1000) : 0;
    } else {
        now->sleep_s = -1;
    }
    now->index = p->pos;
    now->count = p->n_order;
    now->fmt = st.fmt;
    if (p->radio) {
        const radio_station_t *s = &p->rst[p->ri];
        lib_track_t *t = &now->track;
        memset(t, 0, sizeof *t);
        t->id = t->album_id = t->artist_id = -1;
        snprintf(t->path, sizeof t->path, "%s", s->url);
        snprintf(t->album, sizeof t->album, "%s", s->name);
        if (p->r_title[0]) {
            snprintf(t->artist, sizeof t->artist, "%s", p->r_artist);
            snprintf(t->title, sizeof t->title, "%s", p->r_title);
            snprintf(now->art_url, sizeof now->art_url, "%s", p->r_art);
        } else if (p->r_meta[0]) {
            radio_split_meta(p->r_meta, t->artist, sizeof t->artist, t->title, sizeof t->title);
            if (image_url(p->r_meta_url)) snprintf(now->art_url, sizeof now->art_url, "%s", p->r_meta_url);
        }
        if (!t->title[0]) snprintf(t->title, sizeof t->title, "%s", s->name);
        snprintf(t->codec, sizeof t->codec, "%s", st.fmt.codec);
        t->sample_rate = st.fmt.rate;
        t->bits = st.fmt.bits;
        t->channels = st.fmt.channels;
        now->active = 1;
        now->radio = 1;
        now->playing = p->r_want;
        now->stopped = !p->r_want;
        now->connecting = p->r_want && (st.connecting || st.starting || p->r_retry_at || st.state != ENG_PLAYING);
        now->radio_failed = p->r_failed;
        now->pos_ms = p->r_want && st.state == ENG_PLAYING ? st.pos_ms : 0;
        now->len_ms = 0;
        now->index = p->ri;
        now->count = p->rn;
        now->stop_after = 0;
    }
    if (p->pod) {
        lib_track_t *t = &now->track;
        memset(t, 0, sizeof *t);
        t->id = t->album_id = t->artist_id = -1;
        snprintf(t->path, sizeof t->path, "%s", p->ep.path);
        snprintf(t->title, sizeof t->title, "%s", p->ep.title);
        snprintf(t->artist, sizeof t->artist, "%s", p->ep.show);
        snprintf(t->album, sizeof t->album, "%s", p->ep.show);
        snprintf(t->codec, sizeof t->codec, "%s", st.fmt.codec);
        t->sample_rate = st.fmt.rate;
        t->bits = st.fmt.bits;
        t->channels = st.fmt.channels;
        now->active = 1;
        now->podcast = 1;
        now->podcast_stream = !strncmp(p->ep.path, "http://", 7) || !strncmp(p->ep.path, "https://", 8);
        now->stopped = st.state == ENG_STOPPED && !st.starting;
        now->pos_ms = theirs ? st.pos_ms : p->ep_pos;
        now->len_ms = theirs && st.len_ms ? st.len_ms : p->ep.len_ms;
        t->duration_ms = now->len_ms;
        now->index = 0;
        now->count = 1;
        now->stop_after = 0;
    }
}

uint32_t player_seq(const player_t *p) { return p->seq; }

void player_set_levelling(player_t *p, levelling_t mode)
{
    catch_up(p);
    p->levelling = mode;
    queue_next(p);   /* the next track, lined up already, takes the new setting */
}

levelling_t player_levelling(const player_t *p) { return p->levelling; }
