/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/*
 * Fathom shuffle engine: builds one shuffle "cycle" (a permutation of the
 * eligible tracks in a context) that spreads artists and albums, favours
 * long-unheard songs, keeps musical units together, and avoids replaying the
 * end of the previous cycle.
 *
 * Pure C, no allocation outside shuf_build(), no global state between calls.
 */
#ifndef FATHOM_SHUFFLE_H
#define FATHOM_SHUFFLE_H

#include <stddef.h>
#include <stdint.h>

#define SHUF_F_NEVER  0x01u  /* user flag: never include in shuffle */
#define SHUF_F_SPOKEN 0x02u  /* audiobook / podcast (by genre or folder) */

typedef struct {
    uint32_t id;           /* stable track id */
    uint32_t artist;       /* normalized artist key (album artist, "feat." stripped) */
    uint32_t album;        /* album key, unique per album */
    uint32_t unit;         /* 0 = standalone; tracks sharing a non-zero value form a unit */
    uint16_t disc;
    uint16_t track;
    uint32_t duration_ms;
    int64_t  last_played;  /* unix seconds, 0 = never played */
    uint16_t play_count;
    uint16_t skip_count;   /* skipped within the first 30 s */
    uint8_t  flags;        /* SHUF_F_* */
} shuf_track_t;

typedef enum {
    SHUF_MODE_SONGS = 0,   /* unit = track (or kept-together unit), spread by artist then album */
    SHUF_MODE_ALBUMS = 1,  /* unit = whole album in track order, spread by artist */
} shuf_mode_t;

typedef struct {
    shuf_mode_t mode;
    int      freshness;       /* favour long-unheard songs (default on) */
    double   w_fresh;         /* weight of freshness in the within-artist ordering */
    double   w_skip;          /* weight of the early-skip rate */
    double   w_recent;        /* how far (0..1 of the cycle) a song heard today is pushed back */
    int      skip_interludes; /* drop tracks shorter than interlude_ms (default on) */
    uint32_t interlude_ms;
    int      keep_units;      /* honour shuf_track_t.unit (default off) */
    int      exclude_spoken;  /* drop SHUF_F_SPOKEN tracks (default off) */
    int64_t  now;             /* unix seconds, for freshness */
    uint32_t seam;            /* first `seam` units avoid the previous cycle's tail */
    uint32_t max_artist_gap;  /* cap on required units between same-artist units */
    uint32_t max_album_gap;   /* cap on required units between same-album units */
    uint32_t repair_window;   /* how far ahead the repair pass looks for a swap */
    int64_t  first;           /* index of a track that must play first, or -1 */
} shuf_opts_t;

void shuf_default_opts(shuf_opts_t *o, int64_t now);

/*
 * Build a cycle over tracks[0..n). Writes indices into `tracks` to `out`
 * (capacity n) in play order and returns how many were written (tracks that
 * are excluded do not appear). prev_tail lists the track ids that ended the
 * previous cycle (may be NULL). Returns -1 on allocation failure.
 * The same inputs and seed always give the same result.
 */
long shuf_build(const shuf_track_t *tracks, size_t n, const shuf_opts_t *o,
                uint64_t seed, const uint32_t *prev_tail, size_t prev_tail_n,
                uint32_t *out);

#endif
