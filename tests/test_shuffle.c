/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* Property tests for the shuffle engine and grouping keys. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../app/core/rng.h"
#include "../app/library/keys.h"
#include "../app/shuffle/shuffle.h"
#include "../tools/shuffle-sim/simlib.h"

static int failures, checks;
#define CHECK(cond) do { checks++; if (!(cond)) { failures++; \
    fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static const int64_t NOW = 1794700000;

static int is_permutation_of_eligible(const simlib_t *L, const shuf_opts_t *o,
                                      const uint32_t *out, long c)
{
    sim_metrics_t m;
    sim_metrics(L, o, out, c, &m);
    return m.coverage_ok;
}

static void test_empty(void)
{
    shuf_opts_t o;
    shuf_default_opts(&o, NOW);
    uint32_t out[1];
    CHECK(shuf_build(NULL, 0, &o, 1, NULL, 0, out) == 0);
}

static void test_permutation_and_determinism(void)
{
    simlib_t L;
    CHECK(simlib_synthetic(&L, 3000, 42, NOW) == 0);
    shuf_opts_t o;
    shuf_default_opts(&o, NOW);
    uint32_t *a = malloc(L.n * sizeof *a), *b = malloc(L.n * sizeof *b);
    long ca = shuf_build(L.t, L.n, &o, 7, NULL, 0, a);
    long cb = shuf_build(L.t, L.n, &o, 7, NULL, 0, b);
    CHECK(ca > 0 && ca == cb);
    CHECK(memcmp(a, b, (size_t)ca * sizeof *a) == 0);           /* same seed, same order */
    CHECK(is_permutation_of_eligible(&L, &o, a, ca));
    long cc = shuf_build(L.t, L.n, &o, 8, NULL, 0, b);
    CHECK(cc == ca && memcmp(a, b, (size_t)ca * sizeof *a) != 0); /* different seed differs */

    /* interludes are excluded by default, included when the option is off */
    for (long i = 0; i < ca; i++) CHECK(L.t[a[i]].duration_ms >= o.interlude_ms);
    o.skip_interludes = 0;
    CHECK(shuf_build(L.t, L.n, &o, 7, NULL, 0, a) == (long)L.n);
    free(a); free(b);
    simlib_free(&L);
}

static void test_spread_beats_random(void)
{
    simlib_t L;
    CHECK(simlib_synthetic(&L, 8000, 5, NOW) == 0);
    shuf_opts_t o;
    shuf_default_opts(&o, NOW);
    uint32_t *a = malloc(L.n * sizeof *a);
    long c = shuf_build(L.t, L.n, &o, 11, NULL, 0, a);
    sim_metrics_t f, r;
    sim_metrics(&L, &o, a, c, &f);
    c = sim_naive(&L, &o, 11, a);
    sim_metrics(&L, &o, a, c, &r);
    CHECK(f.adj_artist <= f.forced_adj + r.adj_artist / 10);
    CHECK(f.album_within8 < r.album_within8 / 5);
    CHECK(f.first50_diversity >= 0.95);
    free(a);
    simlib_free(&L);
}

static void test_flags_and_first(void)
{
    shuf_track_t t[6];
    memset(t, 0, sizeof t);
    for (int i = 0; i < 6; i++) {
        t[i].id = (uint32_t)(100 + i);
        t[i].artist = (uint32_t)(1 + i % 3);
        t[i].album = (uint32_t)(10 + i);
        t[i].duration_ms = 200000;
    }
    t[1].flags = SHUF_F_NEVER;
    t[2].flags = SHUF_F_SPOKEN;
    t[3].duration_ms = 20000;  /* interlude */
    shuf_opts_t o;
    shuf_default_opts(&o, NOW);
    uint32_t out[6];
    long c = shuf_build(t, 6, &o, 3, NULL, 0, out);
    CHECK(c == 4);  /* NEVER and interlude dropped, spoken kept by default */
    o.exclude_spoken = 1;
    CHECK(shuf_build(t, 6, &o, 3, NULL, 0, out) == 3);
    o.first = 3;    /* explicitly picked interlude plays first */
    c = shuf_build(t, 6, &o, 3, NULL, 0, out);
    CHECK(c == 4 && out[0] == 3);
    o.first = 5;
    for (uint64_t s = 0; s < 20; s++) {
        c = shuf_build(t, 6, &o, s, NULL, 0, out);
        CHECK(c == 3 && out[0] == 5);
    }
    o.first = 2;    /* a picked spoken track plays, spoken ones left out or not */
    c = shuf_build(t, 6, &o, 3, NULL, 0, out);
    CHECK(c == 4 && out[0] == 2);

    /* by albums: a song picked part way through its album plays first, then
     * the rest of that album in order */
    shuf_track_t b[8];
    memset(b, 0, sizeof b);
    for (int i = 0; i < 8; i++) {
        b[i].id = (uint32_t)(200 + i);
        b[i].artist = (uint32_t)(1 + i / 4);
        b[i].album = (uint32_t)(20 + i / 4);   /* two albums of four */
        b[i].track = (uint16_t)(1 + i % 4);
        b[i].duration_ms = 200000;
    }
    shuf_default_opts(&o, NOW);
    o.mode = SHUF_MODE_ALBUMS;
    o.first = 2;                               /* album 20's third song */
    for (uint64_t s = 0; s < 10; s++) {
        c = shuf_build(b, 8, &o, s, NULL, 0, out);
        CHECK(out[0] == 2 && out[1] == 3 && c == 6);   /* 3, 4, then the other album */
    }
}

static void test_units_and_albums(void)
{
    simlib_t L;
    CHECK(simlib_synthetic(&L, 1500, 9, NOW) == 0);
    /* every album's tracks 2..4 form one segue unit */
    for (size_t i = 0; i < L.n; i++)
        if (L.t[i].track >= 2 && L.t[i].track <= 4) L.t[i].unit = L.t[i].album;
    shuf_opts_t o;
    shuf_default_opts(&o, NOW);
    o.keep_units = 1;
    uint32_t *a = malloc(L.n * sizeof *a);
    long c = shuf_build(L.t, L.n, &o, 21, NULL, 0, a);
    CHECK(is_permutation_of_eligible(&L, &o, a, c));
    for (long i = 0; i < c; i++)
        if (L.t[a[i]].unit && L.t[a[i]].track == 2) {
            CHECK(i + 2 < c);
            if (i + 2 < c) {
                CHECK(L.t[a[i + 1]].unit == L.t[a[i]].unit && L.t[a[i + 1]].track == 3);
                CHECK(L.t[a[i + 2]].unit == L.t[a[i]].unit && L.t[a[i + 2]].track == 4);
            }
        }

    /* album mode: whole albums, in track order */
    o.keep_units = 0;
    o.mode = SHUF_MODE_ALBUMS;
    c = shuf_build(L.t, L.n, &o, 22, NULL, 0, a);
    CHECK(c == (long)L.n);
    for (long i = 1; i < c; i++)
        if (L.t[a[i]].album == L.t[a[i - 1]].album)
            CHECK(L.t[a[i]].track == L.t[a[i - 1]].track + 1);
        else
            CHECK(L.t[a[i]].track == 1);
    free(a);
    simlib_free(&L);
}

static void test_seam(void)
{
    simlib_t L;
    CHECK(simlib_synthetic(&L, 2000, 13, NOW) == 0);
    shuf_opts_t o;
    shuf_default_opts(&o, NOW);
    uint32_t *a = malloc(L.n * sizeof *a), tail[30];
    long c = shuf_build(L.t, L.n, &o, 1, NULL, 0, a);
    for (int i = 0; i < 30; i++) tail[i] = L.t[a[c - 30 + i]].id;
    c = shuf_build(L.t, L.n, &o, 2, tail, 30, a);
    for (long i = 0; i < 30 && i < c; i++)
        for (int j = 0; j < 30; j++) CHECK(L.t[a[i]].id != tail[j]);
    free(a);
    simlib_free(&L);
}

static void test_single_artist(void)
{
    shuf_track_t t[40];
    memset(t, 0, sizeof t);
    for (int i = 0; i < 40; i++) {
        t[i].id = (uint32_t)i + 1;
        t[i].artist = 1;
        t[i].album = (uint32_t)(1 + i / 10);
        t[i].track = (uint16_t)(i % 10 + 1);
        t[i].duration_ms = 180000;
    }
    shuf_opts_t o;
    shuf_default_opts(&o, NOW);
    uint32_t out[40];
    long c = shuf_build(t, 40, &o, 5, NULL, 0, out);
    CHECK(c == 40);
    /* four albums of ten: no two songs from one album back-to-back */
    for (long i = 1; i < c; i++) CHECK(t[out[i]].album != t[out[i - 1]].album);
}

static void test_keys(void)
{
    char k[128];
    key_artist("The Beatles feat. Billy Preston", "", k, sizeof k);
    CHECK(!strcmp(k, "beatles"));
    key_artist("Daft Punk", "  DAFT   PUNK ", k, sizeof k);
    CHECK(!strcmp(k, "daft punk"));
    key_artist("Nina Simone", "Various Artists", k, sizeof k);
    CHECK(!strcmp(k, "nina simone"));
    key_artist("Björk ft. Thom Yorke", NULL, k, sizeof k);
    CHECK(!strcmp(k, "björk"));
    key_artist("The The", "", k, sizeof k);
    CHECK(!strcmp(k, "the"));
    key_album("Nina Simone", "Various Artists", "Jazz Classics", k, sizeof k);
    CHECK(!strcmp(k, "various artists\x1fjazz classics"));
    CHECK(key_is_various("VA") && !key_is_various("Vampire Weekend"));
    /* one album whatever its tracks' artists add: guests, "The", a VA spelling */
    char k2[128];
    key_album("Eminem", "", "Recovery", k, sizeof k);
    key_album("Eminem feat. Rihanna", "", "Recovery", k2, sizeof k2);
    CHECK(!strcmp(k, k2) && !strcmp(k, "eminem\x1frecovery"));
    key_album("The Beatles", "", "Abbey Road", k, sizeof k);
    key_album("x", "Beatles", "Abbey Road", k2, sizeof k2);
    CHECK(!strcmp(k, k2));
    key_album("a", "VA", "Hits", k, sizeof k);
    key_album("b", "Various Artists", "Hits", k2, sizeof k2);
    CHECK(!strcmp(k, k2));
    key_album("", "", "Untitled", k, sizeof k);
    CHECK(!strcmp(k, "unknown artist\x1funtitled"));
}

static int credits_are(const char *artist, const char *title, const char *const *want, int nwant)
{
    char names[KEY_CREDIT_MAX][KEY_CREDIT_LEN];
    int n = key_credits(artist, title, names);
    if (n != nwant) { fprintf(stderr, "  credits(%s | %s): %d names\n", artist, title, n); return 0; }
    for (int i = 0; i < n; i++)
        if (strcmp(names[i], want[i])) { fprintf(stderr, "  credits: '%s' != '%s'\n", names[i], want[i]); return 0; }
    return 1;
}

static void test_credits(void)
{
    static const char *const a[] = {"Raggasonic", "Supa John"};
    CHECK(credits_are("Raggasonic feat. Supa John", "Laisse le peuple", a, 2));
    static const char *const b[] = {"Simon & Garfunkel"};
    CHECK(credits_are("Simon & Garfunkel", "The Boxer", b, 1));
    static const char *const c[] = {"Ali Farka Touré", "Ry Cooder"};
    CHECK(credits_are("Ali Farka Touré with Ry Cooder", "Diaraby", c, 2));
    static const char *const d[] = {"Mystik", "Freeman", "Rocca"};
    CHECK(credits_are("Mystik; Freeman; Rocca", "11'30", d, 3));
    static const char *const e[] = {"Gorillaz", "De La Soul", "Gruff Rhys"};
    CHECK(credits_are("Gorillaz", "Superfast Jellyfish (feat. De La Soul & Gruff Rhys)", e, 3));
    static const char *const f[] = {"A", "B", "C", "D"};
    CHECK(credits_are("A ft. B, C and D", "x", f, 4));
    static const char *const g[] = {"Genius/GZA"};
    CHECK(credits_are("Genius/GZA", "Labels", g, 1));
    static const char *const h[] = {"Björk", "Thom Yorke"};
    CHECK(credits_are("Björk", "Song [feat. Thom Yorke] (Live)", h, 2));
    static const char *const i[] = {"X"};
    CHECK(credits_are("X feat. x", "y (feat. X)", i, 1));          /* no duplicates */
    CHECK(credits_are("", "", NULL, 0));
}

static void test_rng(void)
{
    rng_t r;
    rng_seed(&r, 123, 1);
    unsigned hist[7] = {0};
    for (int i = 0; i < 70000; i++) {
        uint32_t v = rng_below(&r, 7);
        CHECK(v < 7);
        if (v < 7) hist[v]++;
    }
    for (int i = 0; i < 7; i++) CHECK(hist[i] > 9500 && hist[i] < 10500);
    for (int i = 0; i < 1000; i++) { double d = rng_unit(&r); CHECK(d >= 0.0 && d < 1.0); }
}

int main(void)
{
    test_empty();
    test_permutation_and_determinism();
    test_spread_beats_random();
    test_flags_and_first();
    test_units_and_albums();
    test_seam();
    test_single_artist();
    test_keys();
    test_credits();
    test_rng();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
