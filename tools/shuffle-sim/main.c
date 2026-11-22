/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/*
 * shuffle-sim: measures Fathom's shuffle against a plain random shuffle.
 *
 *   shuffle-sim [--synthetic N | --tsv FILE] [--trials T] [--seed S]
 *               [--albums] [--no-freshness] [--keep-interludes] [--show K]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../../app/core/rng.h"
#include "simlib.h"

#define HEAD 30

typedef struct {
    double adj, forced, album8, div50, overlap_lib, overlap_small, seam;
    int    coverage_ok;
} agg_t;

static long build(int naive, const simlib_t *L, const shuf_opts_t *o, uint64_t seed,
                  const uint32_t *tail, size_t tail_n, uint32_t *out)
{
    return naive ? sim_naive(L, o, seed, out) : shuf_build(L->t, L->n, o, seed, tail, tail_n, out);
}

/* Overlap of the first HEAD tracks between "today" and "tomorrow" for library L. */
static double day_overlap(int naive, simlib_t *L, const shuf_opts_t *o0, uint64_t seed, uint32_t *buf)
{
    shuf_opts_t o = *o0;
    long c1 = build(naive, L, &o, seed, NULL, 0, buf);
    long h = c1 < HEAD ? c1 : HEAD;
    if (h == 0) return 0;
    uint32_t first[HEAD];
    int64_t saved[HEAD];
    for (long i = 0; i < h; i++) {
        first[i] = buf[i];
        saved[i] = L->t[buf[i]].last_played;
        L->t[buf[i]].last_played = o.now;          /* listened today */
    }
    o.now += 86400;
    long c2 = build(naive, L, &o, seed ^ 0x9e3779b97f4a7c15ULL, NULL, 0, buf);
    long h2 = c2 < HEAD ? c2 : HEAD, same = 0;
    for (long i = 0; i < h2; i++)
        for (long j = 0; j < h; j++)
            if (buf[i] == first[j]) { same++; break; }
    for (long i = 0; i < h; i++) L->t[first[i]].last_played = saved[i];
    return (double)same / h;
}

static void run(int naive, simlib_t *L, simlib_t *small, const shuf_opts_t *o,
                uint64_t seed, int trials, agg_t *a)
{
    uint32_t *buf = malloc(L->n * sizeof *buf);
    uint32_t *buf2 = malloc(L->n * sizeof *buf2);
    memset(a, 0, sizeof *a);
    a->coverage_ok = 1;
    for (int k = 0; k < trials; k++) {
        uint64_t s = seed + (uint64_t)k * 7919;
        long c = build(naive, L, o, s, NULL, 0, buf);
        sim_metrics_t m;
        sim_metrics(L, o, buf, c, &m);
        a->adj += m.adj_artist;
        a->forced += m.forced_adj;
        a->album8 += m.album_within8;
        a->div50 += m.first50_diversity;
        a->coverage_ok &= m.coverage_ok;

        /* seam: tail of this cycle must not reappear early in the next */
        uint32_t tail[HEAD];
        long tn = c < HEAD ? c : HEAD;
        for (long i = 0; i < tn; i++) tail[i] = L->t[buf[c - tn + i]].id;
        long c2 = build(naive, L, o, s + 1, tail, (size_t)tn, buf2);
        long early = 0, lim = c2 < HEAD ? c2 : HEAD;
        for (long i = 0; i < lim; i++)
            for (long j = 0; j < tn; j++)
                if (L->t[buf2[i]].id == tail[j]) { early++; break; }
        a->seam += early;

        a->overlap_lib += day_overlap(naive, L, o, s + 2, buf);
        a->overlap_small += day_overlap(naive, small, o, s + 3, buf);
    }
    a->adj /= trials; a->forced /= trials; a->album8 /= trials; a->div50 /= trials;
    a->seam /= trials; a->overlap_lib /= trials; a->overlap_small /= trials;
    free(buf); free(buf2);
}

/* A 100-track "playlist" drawn from L (keeps the original artist/album ids). */
static int make_small(const simlib_t *L, simlib_t *S, uint64_t seed)
{
    memset(S, 0, sizeof *S);
    size_t n = L->n < 100 ? L->n : 100;
    S->t = malloc((n ? n : 1) * sizeof *S->t);
    S->artists = intern_new();
    S->albums = intern_new();
    if (!S->t || !S->artists || !S->albums) return -1;
    /* share string tables by re-interning so ids stay valid in metrics */
    for (uint32_t i = 1; i <= intern_count(L->artists); i++) intern_id(S->artists, intern_str(L->artists, i));
    for (uint32_t i = 1; i <= intern_count(L->albums); i++) intern_id(S->albums, intern_str(L->albums, i));
    rng_t r;
    rng_seed(&r, seed, 3);
    uint8_t *taken = calloc(L->n ? L->n : 1, 1);
    for (size_t k = 0; k < n;) {
        uint32_t i = rng_below(&r, (uint32_t)L->n);
        if (taken[i]) continue;
        taken[i] = 1;
        S->t[k++] = L->t[i];
    }
    free(taken);
    S->n = n;
    return 0;
}

int main(int argc, char **argv)
{
    const char *tsv = NULL;
    size_t synthetic = 15000;
    int trials = 50, show = 0;
    uint64_t seed = 12345;
    int64_t now = 1794700000;  /* fixed "today" so results are reproducible */
    shuf_opts_t o;
    shuf_default_opts(&o, now);

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--tsv") && i + 1 < argc) tsv = argv[++i];
        else if (!strcmp(argv[i], "--synthetic") && i + 1 < argc) synthetic = strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--trials") && i + 1 < argc) trials = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) seed = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--show") && i + 1 < argc) show = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--albums")) o.mode = SHUF_MODE_ALBUMS;
        else if (!strcmp(argv[i], "--no-freshness")) o.freshness = 0;
        else if (!strcmp(argv[i], "--keep-interludes")) o.skip_interludes = 0;
        else {
            fprintf(stderr, "usage: %s [--synthetic N | --tsv FILE] [--trials T] [--seed S] "
                            "[--albums] [--no-freshness] [--keep-interludes] [--show K]\n", argv[0]);
            return 2;
        }
    }
    if (trials < 1) trials = 1;

    simlib_t L, S;
    if (tsv ? simlib_load_tsv(&L, tsv) : simlib_synthetic(&L, synthetic, seed, now)) {
        fprintf(stderr, "could not load library\n");
        return 1;
    }
    if (L.n == 0) { fprintf(stderr, "library is empty\n"); return 1; }
    if (make_small(&L, &S, seed)) { fprintf(stderr, "out of memory\n"); return 1; }

    printf("Library: %s, %zu tracks, %u artists, %u albums, %ld eligible (%s mode)\n",
           tsv ? tsv : "synthetic", L.n, intern_count(L.artists), intern_count(L.albums),
           sim_eligible(&L, &o), o.mode == SHUF_MODE_ALBUMS ? "album" : "song");

    agg_t f, nv;
    run(0, &L, &S, &o, seed, trials, &f);
    run(1, &L, &S, &o, seed, trials, &nv);

    uint32_t *buf = malloc(L.n * sizeof *buf);
    clock_t c0 = clock();
    int reps = 20;
    for (int k = 0; k < reps; k++) shuf_build(L.t, L.n, &o, seed + k, NULL, 0, buf);
    double ms = 1000.0 * (double)(clock() - c0) / CLOCKS_PER_SEC / reps;

    printf("\nAverages over %d cycles          Fathom    Random    Target\n", trials);
    if (o.mode == SHUF_MODE_ALBUMS)
        printf("(album mode: artist and variety figures count albums, not tracks)\n");
    printf("Same artist back-to-back      %9.1f %9.1f    ~%.0f (forced minimum)\n", f.adj, nv.adj, f.forced);
    printf("Same album within 8 tracks    %9.1f %9.1f    ~0 where achievable\n", f.album8, nv.album8);
    printf("Artist variety, first 50      %8.0f%% %8.0f%%    >= 95%% of proportional ideal\n", 100 * f.div50, 100 * nv.div50);
    printf("Tail of last cycle in first %d %8.1f %9.1f    0\n", HEAD, f.seam, nv.seam);
    printf("Day-to-day overlap, library   %8.1f%% %8.1f%%    <= 5%%\n", 100 * f.overlap_lib, 100 * nv.overlap_lib);
    printf("Day-to-day overlap, 100 songs %8.1f%% %8.1f%%    <= 5%% (expected random: 30%%)\n",
           100 * f.overlap_small, 100 * nv.overlap_small);
    printf("Coverage (each track once)    %9s %9s    yes\n", f.coverage_ok ? "yes" : "NO", nv.coverage_ok ? "yes" : "NO");
    printf("Build time (this machine)     %7.1f ms\n", ms);

    if (show > 0) {
        long c = shuf_build(L.t, L.n, &o, seed, NULL, 0, buf);
        printf("\nFirst %d of one cycle:\n", show);
        for (long i = 0; i < c && i < show; i++) {
            const shuf_track_t *t = &L.t[buf[i]];
            const char *al = intern_str(L.albums, t->album);
            const char *sep = strchr(al, '\x1f');
            printf("%4ld  %-28.28s  %-30.30s  %u.%02u\n", i + 1, intern_str(L.artists, t->artist),
                   sep ? sep + 1 : al, t->disc, t->track);
        }
    }

    free(buf);
    simlib_free(&L);
    simlib_free(&S);
    return f.coverage_ok ? 0 : 1;
}
