/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "player.h"

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
    uint32_t seq;          /* our track-change counter (for the UI) */
    struct { int pos; uint32_t tag; } ahead[ENGINE_AHEAD];   /* lined up in the engine: order positions */
    int      n_ahead;
    uint32_t last_tag;
    lib_track_t cur;
    int      have_cur;
    uint32_t resume_ms;    /* cued track: start here (restored queue) */

    int      sleep_min;    /* sleep timer setting, 0 = off, PLAYER_SLEEP_END_ALBUM */
    int64_t  sleep_album;  /* end of album: the album playing when it was set */
    uint64_t sleep_at;     /* monotonic ms when it pauses */
    player_shuffle_prefs_t sprefs;   /* Settings > Playback */

    /* internet radio */
    int      radio;        /* on: the engine plays rst[ri] */
    radio_station_t *rst;
    int      rn, ri;
    int      r_want;       /* it should be playing (reconnect if it stops) */
    int      r_tries;      /* reconnects since it last played steadily */
    int      r_failed;
    uint64_t r_retry_at;   /* monotonic ms of the next reconnect, 0 = none */
    uint64_t r_since;      /* when the stream last (re)started */
    unsigned r_meta_seq;
    char     r_meta[256], r_meta_url[512];
    char     r_artist[256], r_title[256], r_art[512];   /* player_radio_set_info; title "" = none */
};

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

    shuf_track_t *all = NULL;
    int na = library_shuffle_tracks(p->lib, &all);
    if (na > 0) qsort(all, (size_t)na, sizeof *all, cmp_shuf_id);
    shuf_track_t *t = malloc((size_t)p->n * sizeof *t);
    uint32_t *out = malloc((size_t)p->n * sizeof *out);
    if (!t || !out) { free(all); free(t); free(out); return; }
    for (int i = 0; i < p->n; i++) {
        shuf_track_t key = {.id = (uint32_t)p->ctx[i]};
        shuf_track_t *hit = na > 0 ? bsearch(&key, all, (size_t)na, sizeof *all, cmp_shuf_id) : NULL;
        if (hit) t[i] = *hit;
        else { memset(&t[i], 0, sizeof t[i]); t[i].id = key.id; }
    }
    free(all);

    shuf_opts_t o;
    shuf_default_opts(&o, now_s());
    o.first = first;
    o.mode = p->sprefs.albums ? SHUF_MODE_ALBUMS : SHUF_MODE_SONGS;
    o.freshness = p->sprefs.freshness;
    o.skip_interludes = p->sprefs.skip_interludes;
    o.exclude_spoken = p->sprefs.exclude_spoken;
    o.keep_units = p->sprefs.keep_works;
    long c = shuf_build(t, (size_t)p->n, &o, rng_os_seed(), tail, tail_n, out);
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
static void queue_next(player_t *p)
{
    if (p->radio) return;              /* the queue waits for the radio to be left */
    engine_track_t q[ENGINE_AHEAD];
    char path[ENGINE_AHEAD][sizeof ((lib_track_t *)0)->path];
    int n = 0;
    uint32_t ms = 0;
    for (int np = after(p, p->pos); np >= 0 && n < ENGINE_AHEAD && ms < AHEAD_MS; np = after(p, np)) {
        lib_track_t t;
        if (library_track(p->lib, id_at(p, np), &t) != 0) break;
        if (p->sleep_album && t.album_id != p->sleep_album) break;   /* sleep timer: end of album */
        if (!++p->last_tag) p->last_tag = 1;   /* (0 is engine_play's) */
        snprintf(path[n], sizeof path[n], "%s", t.path);
        q[n] = (engine_track_t){path[n], t.id, track_gain(p, t.id), p->last_tag};
        p->ahead[n].pos = np;
        p->ahead[n].tag = p->last_tag;
        ms += t.duration_ms;
        n++;
    }
    p->n_ahead = n;
    engine_queue_ahead(p->eng, q, n);
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
    if (st.state == ENG_STOPPED || st.track_id != p->cur.id) return;
    uint32_t at = st.pos_ms;
    if (at < LONG_FILE_EDGE_MS || at + LONG_FILE_EDGE_MS > p->cur.duration_ms) at = 0;
    library_set_resume(p->lib, p->cur.id, at);
}

/* Start the track at order_pos; a long file resumes where it was left (if `resume`). */
static void radio_leave(player_t *p);

static void start_at_opt(player_t *p, int order_pos, int resume)
{
    if (order_pos < 0 || order_pos >= p->n_order) return;
    radio_leave(p);
    remember_position(p);
    p->fails = 0;
    set_pos(p, order_pos);
    if (library_track(p->lib, id_at(p, order_pos), &p->cur) != 0) return;
    p->have_cur = 1;
    p->resume_ms = 0;
    p->own_start = 1;
    engine_play_gain(p->eng, p->cur.path, p->cur.id, track_gain(p, p->cur.id));
    if (resume && p->cur.duration_ms >= LONG_FILE_MS) {
        uint32_t at = library_resume(p->lib, p->cur.id);
        if (at) engine_seek_ms(p->eng, at);
    }
    p->seq++;
    queue_next(p);
}

static void start_at(player_t *p, int order_pos) { start_at_opt(p, order_pos, 1); }

void player_play(player_t *p, const int64_t *ids, int n, int start)
{
    set_ctx(p, ids, n);
    build_order(p, p->shuffle ? start : -1, NULL, 0);
    p->user_end = 0;
    start_at(p, p->shuffle ? 0 : start);   /* shuffled: the chosen track is first */
}

void player_cue(player_t *p, const int64_t *ids, int n, int start)
{
    if (p->radio) { engine_stop(p->eng); radio_leave(p); }
    set_ctx(p, ids, n);
    build_order(p, p->shuffle ? start : -1, NULL, 0);
    p->user_end = 0;
    set_pos(p, p->shuffle ? 0 : start);
    if (library_track(p->lib, id_at(p, p->pos), &p->cur) == 0) {
        p->have_cur = 1;
        p->seq++;
    }
}

void player_shuffle_all(player_t *p)
{
    lib_list_t l;
    if (library_list(p->lib, LIB_SONGS, 0, &l) <= 0) return;
    p->shuffle = 1;
    set_ctx(p, l.ids, l.n);
    library_list_free(&l);
    build_order(p, -1, NULL, 0);
    p->user_end = 0;
    start_at(p, 0);
}

/* Start the cued track (from where a restored queue left off). */
static void start_cued(player_t *p)
{
    uint32_t at = p->resume_ms;
    start_at(p, p->pos);
    if (at) engine_seek_ms(p->eng, at);
}

static void radio_connect(player_t *p);

void player_toggle_pause(player_t *p)
{
    if (p->radio) { player_pause(p, p->r_want); return; }
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
            radio_connect(p);
        }
        return;
    }
    eng_status_t st;
    engine_status(p->eng, &st);
    if (st.starting) engine_pause(p->eng, on);           /* about to start: start paused or not */
    else if (on && st.state == ENG_PLAYING) engine_pause(p->eng, 1);
    else if (!on && st.state == ENG_PAUSED) engine_pause(p->eng, 0);
    else if (!on && st.state == ENG_STOPPED && p->have_cur) start_cued(p);
}

/* The current track is being left: record it as played or as an early skip. */
static void record(player_t *p, int skipped_by_user)
{
    if (!p->have_cur || p->radio) return;
    eng_status_t st;
    engine_status(p->eng, &st);
    int early = skipped_by_user && st.pos_ms < EARLY_SKIP_MS;
    library_played(p->lib, p->cur.id, early, now_s());
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

void player_next(player_t *p)
{
    if (p->radio) { radio_station(p, (p->ri + 1) % p->rn); return; }
    if (!p->have_cur) return;
    catch_up(p);
    record(p, 1);
    if (p->pos + 1 < p->n_order) start_at(p, p->pos + 1);
    else if (p->repeat == REPEAT_ALL && new_cycle(p) == 0) start_at(p, 0);
    else if (p->repeat == REPEAT_ALL) start_at(p, 0);
}

void player_prev(player_t *p)
{
    if (p->radio) { radio_station(p, (p->ri + p->rn - 1) % p->rn); return; }
    if (!p->have_cur) return;
    catch_up(p);
    eng_status_t st;
    engine_status(p->eng, &st);
    if (st.pos_ms > RESTART_MS || p->pos == 0) start_at_opt(p, p->pos, 0);   /* restart: from the top */
    else start_at(p, p->pos - 1);
}

void player_seek_by(player_t *p, int delta_ms)
{
    eng_status_t st;
    engine_status(p->eng, &st);
    if (st.state == ENG_STOPPED || !st.len_ms || p->radio) return;
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
    if (start_queue_if_empty(p, ids, n)) return;
    catch_up(p);                        /* (after what plays now) */
    if (insert_at(p, p->pos + 1, ids, n)) return;
    p->user_end += n;
    queue_next(p);
}

void player_add_to_queue(player_t *p, const int64_t *ids, int n)
{
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
    int a = p->pos + 1 + i, b = a + dir;
    if (a <= p->pos || b <= p->pos || a >= p->n_order || b >= p->n_order) return;
    int t = p->order[a];
    p->order[a] = p->order[b];
    p->order[b] = t;
    queue_next(p);
}

void player_queue_remove(player_t *p, int i)
{
    int a = p->pos + 1 + i;
    if (a <= p->pos || a >= p->n_order) return;
    memmove(&p->order[a], &p->order[a + 1], (size_t)(p->n_order - a - 1) * sizeof *p->order);
    p->n_order--;
    if (p->user_end > a) p->user_end--;
    queue_next(p);
}

void player_queue_clear(player_t *p)
{
    if (!p->have_cur) return;
    catch_up(p);
    p->n_order = p->pos + 1;
    p->user_end = p->pos + 1;
    queue_next(p);
}

void player_queue_jump(player_t *p, int i)
{
    int a = p->pos + 1 + i;
    if (a <= p->pos || a >= p->n_order) return;
    record(p, 1);
    start_at(p, a);
}

/* ---------- saved queue ---------- */

int player_save(player_t *p, const char *path)
{
    if (!p->have_cur || !p->n_order) return 0;
    eng_status_t st;
    engine_status(p->eng, &st);
    uint32_t at = st.state == ENG_STOPPED || p->radio ? p->resume_ms : st.pos_ms;
    char tmp[600];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "we");
    if (!f) return -1;
    fprintf(f, "fathom-queue 1\nshuffle %d\nrepeat %d\npos %d\nuser_end %d\nposition_ms %u\nctx %d",
            p->shuffle, (int)p->repeat, p->pos, p->user_end, at, p->n);
    for (int i = 0; i < p->n; i++) fprintf(f, " %lld", (long long)p->ctx[i]);
    fprintf(f, "\norder %d", p->n_order);
    for (int i = 0; i < p->n_order; i++) fprintf(f, " %d", p->order[i]);
    fprintf(f, "\n");
    int ok = fflush(f) == 0 && fsync(fileno(f)) == 0;
    ok = fclose(f) == 0 && ok;
    if (!ok || rename(tmp, path)) { unlink(tmp); return -1; }
    return 0;
}

int player_restore(player_t *p, const char *path)
{
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
    ok = ok && fscanf(f, " order %d", &no) == 1 && no > 0 && no < 2000000;
    int *order = ok ? malloc((size_t)no * sizeof *order) : NULL;
    for (int i = 0; ok && i < no; i++) ok = order && fscanf(f, "%d", &order[i]) == 1 && order[i] >= 0 && order[i] < n;
    fclose(f);
    ok = ok && pos >= 0 && pos < no;
    lib_track_t t;
    ok = ok && library_track(p->lib, ctx[order[pos]], &t) == 0;   /* the track must still exist */
    if (!ok) { free(ctx); free(order); return -1; }

    if (p->radio) { engine_stop(p->eng); radio_leave(p); }
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
    p->seq++;
    return 0;
}

/* ---------- internet radio ---------- */

#define RADIO_STEADY_MS 30000   /* playing this long: a later drop starts the retries afresh */
#define RADIO_TRIES 6

static void radio_connect(player_t *p)
{
    const radio_station_t *s = &p->rst[p->ri];
    p->r_want = 1;
    p->r_failed = 0;
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
    p->r_meta[0] = p->r_meta_url[0] = p->r_artist[0] = p->r_title[0] = p->r_art[0] = '\0';
    radio_connect(p);
}

void player_radio_play(player_t *p, const radio_station_t *st, int n, int i)
{
    if (n <= 0 || i < 0 || i >= n) return;
    if (n > RADIO_MAX) n = RADIO_MAX;
    radio_station_t *copy = malloc((size_t)n * sizeof *copy);
    if (!copy) return;
    memcpy(copy, st, (size_t)n * sizeof *copy);
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
    p->r_retry_at = 0;
    p->seq++;
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
    if (!p->r_want) return;
    uint64_t now = mono_ms();
    if (st->state == ENG_PAUSED) {      /* the sleep timer, or the output went away */
        p->r_want = 0;
        engine_stop(p->eng);
        engine_set_fade(p->eng, 1.0f);
        p->seq++;
        return;
    }
    if (st->state == ENG_PLAYING && now - p->r_since > RADIO_STEADY_MS) p->r_tries = 0;
    if (st->state != ENG_STOPPED || st->starting) return;
    /* the stream stopped (it dropped, or it couldn't connect): try again, waiting longer each time */
    if (!p->r_retry_at) {
        static const int wait_ms[RADIO_TRIES] = {500, 2000, 4000, 8000, 15000, 30000};
        if (p->r_tries >= RADIO_TRIES) {
            p->r_want = 0;
            p->r_failed = 1;
            p->seq++;
            return;
        }
        p->r_retry_at = now + (uint64_t)wait_ms[p->r_tries++];
    } else if (now >= p->r_retry_at) {
        radio_connect(p);
    }
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
    int stop = np < 0 || ++p->fails >= p->n_order;
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
    if (p->radio) return;
    eng_status_t st;
    engine_status(p->eng, &st);
    follow(p, &st);
}

void player_tick(player_t *p)
{
    run_sleep_timer(p);
    eng_status_t st;
    engine_status(p->eng, &st);
    if (p->radio) { radio_tick(p, &st); return; }
    if (st.starting) return;            /* (what it says is still about the last track) */
    if (st.state == ENG_PLAYING) p->fails = 0;
    follow(p, &st);
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
    /* The engine's figures are for what it plays: not yet this item while it
     * starts, or another one (from a radio station to an album, the station's
     * time showed against the track's length until the album began). */
    int64_t want = p->radio ? -2 - p->ri : p->cur.id;
    int theirs = st.state != ENG_STOPPED && !st.starting && st.track_id == want;
    now->pos_ms = theirs ? st.pos_ms : !p->radio && (st.state == ENG_STOPPED || st.starting) ? p->resume_ms : 0;
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
}

uint32_t player_seq(const player_t *p) { return p->seq; }

void player_set_levelling(player_t *p, levelling_t mode)
{
    catch_up(p);
    p->levelling = mode;
    queue_next(p);   /* the next track, lined up already, takes the new setting */
}

levelling_t player_levelling(const player_t *p) { return p->levelling; }
