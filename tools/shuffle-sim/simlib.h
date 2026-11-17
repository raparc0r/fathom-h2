/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Test libraries (synthetic or loaded from a library export) and shuffle quality metrics. */
#ifndef FATHOM_SIMLIB_H
#define FATHOM_SIMLIB_H

#include <stddef.h>
#include <stdint.h>

#include "../../app/core/intern.h"
#include "../../app/shuffle/shuffle.h"

typedef struct {
    shuf_track_t *t;
    size_t        n;
    intern_t     *artists;  /* artist key strings, id = shuf_track_t.artist */
    intern_t     *albums;   /* album key strings, id = shuf_track_t.album */
} simlib_t;

/* Realistic synthetic library: Zipf-like artist sizes, 6-16 track albums,
 * ~3% short interludes, 40% never played. Returns 0 on success. */
int  simlib_synthetic(simlib_t *L, size_t n, uint64_t seed, int64_t now);
/* Load a tab-separated export from tools/library-export. Returns 0 on success. */
int  simlib_load_tsv(simlib_t *L, const char *path);
void simlib_free(simlib_t *L);

typedef struct {
    long   count;            /* tracks in the cycle */
    int    coverage_ok;      /* every eligible track exactly once */
    long   adj_artist;       /* same artist back-to-back */
    long   forced_adj;       /* lower bound on adj_artist given the library */
    long   album_within8;    /* positions with the same album among the previous 8 */
    double first50_diversity;/* distinct artists in first 50 / proportional ideal */
} sim_metrics_t;

void sim_metrics(const simlib_t *L, const shuf_opts_t *o, const uint32_t *order,
                 long count, sim_metrics_t *m);

/* Baseline: plain Fisher-Yates over the same eligible tracks. */
long sim_naive(const simlib_t *L, const shuf_opts_t *o, uint64_t seed, uint32_t *out);

/* Number of tracks shuf_build would consider eligible. */
long sim_eligible(const simlib_t *L, const shuf_opts_t *o);

#endif
