/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
#define _POSIX_C_SOURCE 200809L
#include "simlib.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../app/core/rng.h"
#include "../../app/library/keys.h"

static int is_eligible(const shuf_track_t *x, const shuf_opts_t *o)
{
    int grouped = (o->mode == SHUF_MODE_ALBUMS) || (o->keep_units && x->unit != 0);
    if (x->flags & SHUF_F_NEVER) return 0;
    if (o->exclude_spoken && (x->flags & SHUF_F_SPOKEN)) return 0;
    if (o->skip_interludes && !grouped && x->duration_ms > 0 && x->duration_ms < o->interlude_ms)
        return 0;
    return 1;
}

long sim_eligible(const simlib_t *L, const shuf_opts_t *o)
{
    long c = 0;
    for (size_t i = 0; i < L->n; i++) c += is_eligible(&L->t[i], o);
    return c;
}

int simlib_synthetic(simlib_t *L, size_t n, uint64_t seed, int64_t now)
{
    memset(L, 0, sizeof *L);
    L->t = calloc(n ? n : 1, sizeof *L->t);
    L->artists = intern_new();
    L->albums = intern_new();
    if (!L->t || !L->artists || !L->albums) return -1;

    rng_t r;
    rng_seed(&r, seed, 7);
    size_t n_artists = n / 18 + 1;
    double *w = malloc(n_artists * sizeof *w), total = 0;
    if (!w) return -1;
    for (size_t a = 0; a < n_artists; a++) { w[a] = 1.0 / pow((double)a + 1, 0.9); total += w[a]; }

    size_t i = 0;
    uint32_t album_no = 0;
    char name[64];
    while (i < n) {
        /* pick an artist proportional to weight */
        double x = rng_unit(&r) * total, acc = 0;
        size_t a = 0;
        for (; a + 1 < n_artists; a++) { acc += w[a]; if (x < acc) break; }
        snprintf(name, sizeof name, "artist %zu", a);
        uint32_t artist = intern_id(L->artists, name);
        snprintf(name, sizeof name, "artist %zu\x1f" "album %u", a, ++album_no);
        uint32_t album = intern_id(L->albums, name);
        uint32_t tracks = 6 + rng_below(&r, 11);
        for (uint32_t k = 0; k < tracks && i < n; k++, i++) {
            shuf_track_t *t = &L->t[i];
            t->id = (uint32_t)i + 1;
            t->artist = artist;
            t->album = album;
            t->disc = 1;
            t->track = (uint16_t)(k + 1);
            t->duration_ms = rng_below(&r, 100) < 3 ? 15000 + rng_below(&r, 25000)
                                                    : 120000 + rng_below(&r, 300000);
            if (rng_below(&r, 100) >= 40) {
                t->last_played = now - (int64_t)rng_below(&r, 200 * 86400);
                t->play_count = (uint16_t)(1 + rng_below(&r, 20));
                t->skip_count = (uint16_t)rng_below(&r, 3);
            }
        }
    }
    free(w);
    L->n = n;
    return 0;
}

/* Split `line` in place on tabs; returns number of fields. */
static int split_tabs(char *line, char **f, int max)
{
    int c = 0;
    f[c++] = line;
    for (char *p = line; *p && c < max; p++)
        if (*p == '\t') { *p = '\0'; f[c++] = p + 1; }
    return c;
}

static int contains_ci(const char *hay, const char *needle)
{
    size_t nl = strlen(needle);
    for (; *hay; hay++) {
        size_t k = 0;
        while (k < nl && hay[k] && (hay[k] | 32) == needle[k]) k++;
        if (k == nl) return 1;
    }
    return 0;
}

int simlib_load_tsv(simlib_t *L, const char *path)
{
    memset(L, 0, sizeof *L);
    FILE *fp = fopen(path, "r");
    if (!fp) return -1;
    L->artists = intern_new();
    L->albums = intern_new();
    size_t cap = 4096;
    L->t = calloc(cap, sizeof *L->t);
    if (!L->artists || !L->albums || !L->t) { fclose(fp); return -1; }

    char *line = NULL;
    size_t lcap = 0;
    ssize_t len;
    int header = 1;
    char key[1024];
    while ((len = getline(&line, &lcap, fp)) > 0) {
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';
        if (header) { header = 0; if (!strncmp(line, "id\t", 3)) continue; }
        char *f[9];
        if (split_tabs(line, f, 9) < 9) continue;
        /* id, artist, album_artist, album, title, genre, disc, track, duration_ms */
        if (L->n == cap) {
            cap *= 2;
            shuf_track_t *nt = realloc(L->t, cap * sizeof *nt);
            if (!nt) { free(line); fclose(fp); return -1; }
            L->t = nt;
        }
        shuf_track_t *t = &L->t[L->n];
        memset(t, 0, sizeof *t);
        t->id = (uint32_t)L->n + 1;
        key_artist(f[1], f[2], key, sizeof key);
        t->artist = intern_id(L->artists, key[0] ? key : "(unknown artist)");
        key_album(f[1], f[2], f[3], key, sizeof key);
        t->album = intern_id(L->albums, key);
        t->disc = (uint16_t)atoi(f[6]);
        t->track = (uint16_t)atoi(f[7]);
        t->duration_ms = (uint32_t)strtoul(f[8], NULL, 10);
        if (contains_ci(f[5], "audiobook") || contains_ci(f[5], "podcast") ||
            contains_ci(f[5], "spoken"))
            t->flags |= SHUF_F_SPOKEN;
        L->n++;
    }
    free(line);
    fclose(fp);
    return 0;
}

void simlib_free(simlib_t *L)
{
    free(L->t);
    intern_free(L->artists);
    intern_free(L->albums);
    memset(L, 0, sizeof *L);
}

static void metrics_raw(const simlib_t *L, const shuf_opts_t *o, const uint32_t *order,
                        long count, sim_metrics_t *m)
{
    memset(m, 0, sizeof *m);
    m->count = count;

    uint32_t na = intern_count(L->artists) + 1;
    uint32_t *artist_n = calloc(na, sizeof *artist_n);
    uint8_t *seen = calloc(L->n ? L->n : 1, 1);
    uint8_t *seen_a = calloc(na, 1);
    if (!artist_n || !seen || !seen_a) { free(artist_n); free(seen); free(seen_a); return; }

    int dup = 0;
    for (long i = 0; i < count; i++) {
        if (seen[order[i]]) dup = 1;
        seen[order[i]] = 1;
        artist_n[L->t[order[i]].artist]++;
    }
    m->coverage_ok = !dup && count == sim_eligible(L, o);

    uint32_t n_max = 0, distinct = 0;
    for (uint32_t a = 0; a < na; a++) {
        if (artist_n[a] > n_max) n_max = artist_n[a];
        if (artist_n[a]) distinct++;
    }
    long forced = (long)n_max - (count - (long)n_max) - 1;
    m->forced_adj = forced > 0 ? forced : 0;

    for (long i = 1; i < count; i++)
        if (L->t[order[i]].artist == L->t[order[i - 1]].artist) m->adj_artist++;

    for (long i = 1; i < count; i++)
        for (long d = 1; d <= 8 && d <= i; d++)
            if (L->t[order[i]].album == L->t[order[i - d]].album) { m->album_within8++; break; }

    long head = count < 50 ? count : 50;
    uint32_t got = 0;
    for (long i = 0; i < head; i++)
        if (!seen_a[L->t[order[i]].artist]) { seen_a[L->t[order[i]].artist] = 1; got++; }
    /* Ideal: every artist shows up in proportion to its share of the cycle, so an
     * artist with share p contributes min(1, head * p) distinct appearances. */
    double ideal = 0;
    for (uint32_t a = 0; a < na; a++)
        if (artist_n[a]) {
            double e = (double)head * artist_n[a] / (double)count;
            ideal += e < 1.0 ? e : 1.0;
        }
    (void)distinct;
    m->first50_diversity = ideal > 0 ? (double)got / ideal : 1.0;

    free(artist_n); free(seen); free(seen_a);
}

void sim_metrics(const simlib_t *L, const shuf_opts_t *o, const uint32_t *order,
                 long count, sim_metrics_t *m)
{
    metrics_raw(L, o, order, count, m);
    if (o->mode != SHUF_MODE_ALBUMS || count == 0) return;
    /* Album mode: whole albums play in order, so measure spread album by album. */
    uint32_t *firsts = malloc((size_t)count * sizeof *firsts);
    if (!firsts) return;
    long k = 0;
    for (long i = 0; i < count; i++)
        if (i == 0 || L->t[order[i]].album != L->t[order[i - 1]].album) firsts[k++] = order[i];
    sim_metrics_t a;
    metrics_raw(L, o, firsts, k, &a);
    int coverage = m->coverage_ok;
    *m = a;
    m->coverage_ok = coverage;
    m->count = count;
    free(firsts);
}

long sim_naive(const simlib_t *L, const shuf_opts_t *o, uint64_t seed, uint32_t *out)
{
    rng_t r;
    rng_seed(&r, seed, 99);
    long c = 0;
    for (size_t i = 0; i < L->n; i++)
        if (is_eligible(&L->t[i], o)) out[c++] = (uint32_t)i;
    for (long i = c - 1; i > 0; i--) {
        uint32_t j = rng_below(&r, (uint32_t)i + 1);
        uint32_t tmp = out[i]; out[i] = out[j]; out[j] = tmp;
    }
    return c;
}
