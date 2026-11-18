/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Player: the play queue on top of the engine. A context (album, artist, all
 * songs, a playlist) is played in natural order or as a shuffle cycle.
 * Tracks can be put in front of the rest: "play next" right
 * after the current one, "add to queue" after the ones already added.
 * Call player_tick() regularly (the UI does, ~10x per second): it follows the
 * engine across gapless track changes, lines up the next track, runs the sleep
 * timer and records history.
 */
#ifndef FATHOM_PLAYER_H
#define FATHOM_PLAYER_H

#include <stdint.h>

#include "audio/engine.h"
#include "library/library.h"
#include "player/radio.h"

typedef struct player player_t;

typedef enum { REPEAT_OFF, REPEAT_ALL, REPEAT_ONE } repeat_t;
/* Volume levelling (ReplayGain 2.0): off keeps playback bit-perfect. */
typedef enum { LEVEL_OFF, LEVEL_TRACK, LEVEL_ALBUM } levelling_t;

typedef struct {
    int         active;       /* something is loaded */
    int         playing;      /* not paused/stopped */
    int         stopped;      /* nothing loaded, or the queue played to its end (a cued track is "paused") */
    lib_track_t track;
    uint32_t    pos_ms, len_ms;
    int         shuffle;
    repeat_t    repeat;
    int         stop_after;   /* stop when the current track ends */
    int         sleep_s;      /* sleep timer: seconds left, -1 = off */
    int         index, count; /* position in the queue (radio: the station, of how many) */
    audio_format_t fmt;
    /* Internet radio: track.album is the station, track.title/.artist what it
     * says is playing (title = the station's name until it says), pos_ms the
     * time listened, len_ms 0. */
    int         radio;
    int         connecting;   /* connecting, or reconnecting after the stream dropped */
    int         radio_failed; /* gave up reconnecting (Play tries again) */
    char        art_url[512]; /* the song's cover (an http(s) image), "" = none */
} player_now_t;

player_t *player_new(library_t *lib, engine_t *eng);
void      player_free(player_t *p);

/* Play `ids` (a copy is kept) starting at ids[start]. With shuffle on, the
 * chosen track plays first and the rest of the context is shuffled after it. */
void player_play(player_t *p, const int64_t *ids, int n, int start);
/* Load a queue and its current track without starting playback (resume, previews). */
void player_cue(player_t *p, const int64_t *ids, int n, int start);
/* Shuffle every song in the library. */
void player_shuffle_all(player_t *p);

void player_toggle_pause(player_t *p);
void player_pause(player_t *p, int on);     /* on = 0 also starts a cued track */
void player_next(player_t *p);
void player_prev(player_t *p);              /* >3 s in: restart; else previous */
void player_seek_by(player_t *p, int delta_ms);
void player_set_shuffle(player_t *p, int on);
/* Shuffle's preferences (Settings > Playback), from the next order built:
 * albums (whole albums in order) rather than songs, favouring long-unheard
 * songs, leaving out short interludes and audiobooks or podcasts, keeping a
 * work's parts together. */
typedef struct { int albums, freshness, skip_interludes, exclude_spoken, keep_works; } player_shuffle_prefs_t;
void player_set_shuffle_prefs(player_t *p, const player_shuffle_prefs_t *prefs);
int  player_shuffle(const player_t *p);
void player_set_repeat(player_t *p, repeat_t r);
repeat_t player_repeat(const player_t *p);
void player_set_stop_after(player_t *p, int on);
int  player_stop_after(const player_t *p);
/* Sleep timer: pause after `minutes` (fading out over the last 10 s); 0 = off;
 * PLAYER_SLEEP_END_ALBUM: stop when the album playing now ends. */
#define PLAYER_SLEEP_END_ALBUM (-1)
void player_set_sleep(player_t *p, int minutes);
int  player_sleep_minutes(const player_t *p);   /* the setting, 0 = off */

/* Volume levelling: from the next track on (the one playing keeps its level). */
void    player_set_levelling(player_t *p, levelling_t mode);
levelling_t player_levelling(const player_t *p);

/* The queue: the tracks after the current one, in play order. */
void    player_play_next(player_t *p, const int64_t *ids, int n);    /* right after the current track */
void    player_add_to_queue(player_t *p, const int64_t *ids, int n); /* after the ones already added */
int     player_queue_len(const player_t *p);
int64_t player_queue_at(const player_t *p, int i);
void    player_queue_move(player_t *p, int i, int dir);   /* swap with the next (+1) or previous (-1) */
void    player_queue_remove(player_t *p, int i);
void    player_queue_clear(player_t *p);                  /* everything after the current track */
void    player_queue_jump(player_t *p, int i);            /* play upcoming track i now */

/* The queue, modes and position, kept across restarts. Restoring cues the
 * track paused at its position: Play continues from there. */
int  player_save(player_t *p, const char *path);
int  player_restore(player_t *p, const char *path);

/* Internet radio: play station i of `st` (a copy is kept). The queue is kept
 * as it was, cued where it was left; playing anything from the library leaves
 * the radio. On the radio, Play/pause stops (the stream is live) and Play
 * connects again; Next and Previous change station. */
void player_radio_play(player_t *p, const radio_station_t *st, int n, int i);
int  player_radio(const player_t *p);
const radio_station_t *player_radio_station(const player_t *p);   /* NULL off the radio */
/* What's playing, from somewhere other than the stream (radio_station_t.info):
 * used over what the stream says until the station changes; NULL = clear.
 * art_url: its cover ("" or NULL = none). */
void player_radio_set_info(player_t *p, const char *artist, const char *title, const char *art_url);

void player_tick(player_t *p);
/* Nothing for player_tick() to follow until a command (a key) starts
 * something: not playing or starting, no sleep timer, no stream to reconnect. */
int player_idle(player_t *p);
void player_now(player_t *p, player_now_t *now);
uint32_t player_seq(const player_t *p);     /* changes when the track changes */

#endif
