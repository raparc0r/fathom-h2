/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* Stereo width (audio/width.h): the arithmetic, and through the engine. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "audio/engine.h"
#include "audio/output.h"
#include "audio/width.h"
#include "test_flac.h"
#include "test_util.h"

static void run(int pct, int32_t l, int32_t r, int32_t *ol, int32_t *or_)
{
    int32_t ka, kb, s[2] = {l, r};
    width_coeffs(pct, &ka, &kb);
    width_run(s, 1, ka, kb);
    *ol = s[0];
    *or_ = s[1];
}

static long near(int64_t a, int64_t b) { return labs((long)(a - b)); }

static void test_arithmetic(void)
{
    int32_t l, r;
    const int32_t F = INT32_MAX, H = 1 << 30;
    /* mono: both channels their mean */
    run(WIDTH_MONO, H, 0, &l, &r);
    CHECK(l == r && near(l, H / 2) <= 1);
    run(WIDTH_MONO, F, INT32_MIN, &l, &r);
    CHECK(l == r && near(l, 0) <= 1);
    /* 100: as it was (the engine doesn't call it, but it would change nothing) */
    run(WIDTH_NORMAL, 123456789, -987654321, &l, &r);
    CHECK(l == 123456789 && r == -987654321);
    /* what both channels share stays put at every width below 100, and is
     * only turned down above it (by 100/width) */
    for (int p = 0; p <= 100; p += 25) {
        run(p, H, H, &l, &r);
        CHECK(near(l, H) <= 1 && near(r, H) <= 1);
    }
    run(150, H, H, &l, &r);
    CHECK(near(l, (int64_t)H * 100 / 150) <= 2 && l == r);
    run(125, H, H, &l, &r);
    CHECK(near(l, (int64_t)H * 100 / 125) <= 2 && l == r);
    /* narrower: a sound on one side moves toward the middle */
    run(50, H, 0, &l, &r);
    CHECK(near(l, (int64_t)H * 3 / 4) <= 1 && near(r, H / 4) <= 1);
    /* wider: it leaves the other side in antiphase, and nothing clips: a
     * full-scale sound on one side, and full scale out of phase */
    run(150, H, 0, &l, &r);
    CHECK(l > 0 && r < 0 && near(l, (int64_t)H * 250 / 300) <= 2 && near(r, -(int64_t)H * 50 / 300) <= 2);
    run(150, F, 0, &l, &r);
    CHECK(l > 0 && l < F && r < 0);
    run(150, F, INT32_MIN, &l, &r);
    CHECK(l > 0 && r < 0 && l >= F - 4 && r <= INT32_MIN + 4);              /* the loudest it gets: full scale */
    run(125, INT32_MIN, F, &l, &r);
    CHECK(l < 0 && r > 0);
    run(WIDTH_MAX, F, INT32_MIN, &l, &r);                                    /* the widest too */
    CHECK(l > 0 && r < 0 && l >= F - 4 && r <= INT32_MIN + 4);
    run(WIDTH_MAX, H, H, &l, &r);
    CHECK(near(l, H / 2) <= 2 && l == r);                                    /* (what's shared: half, 6 dB down) */
    run(WIDTH_MAX, F, 0, &l, &r);
    CHECK(l > 0 && l < F && r < 0);
    /* out of range: held to the ends */
    int32_t ka, kb, k2a, k2b;
    width_coeffs(400, &ka, &kb);
    width_coeffs(WIDTH_MAX, &k2a, &k2b);
    CHECK(ka == k2a && kb == k2b);
    width_coeffs(-5, &ka, &kb);
    CHECK(ka == kb);
}

/* ---- through the engine: an output that keeps what it's given ---- */

typedef struct { int32_t first[2]; unsigned long long frames; unsigned bits; int opens; } keep_t;

static int k_open(void *c, const audio_format_t *f) { keep_t *k = c; k->bits = f->bits; k->opens++; return 0; }
static long k_write(void *c, const int32_t *x, long n)
{
    keep_t *k = c;
    if (!k->frames && n > 0) { k->first[0] = x[0]; k->first[1] = x[1]; }
    k->frames += (unsigned long long)n;
    return n;
}
static long k_delay(void *c) { (void)c; return 0; }
static void k_nop(void *c) { (void)c; }
static void k_pause(void *c, int on) { (void)c; (void)on; }
static void k_describe(void *c, char *o, int cap) { (void)c; snprintf(o, (size_t)cap, "keep"); }

/* A stereo 16-bit FLAC, left at +0.5 of full scale and right silent. */
static void left_only(FLAC__int32 *pcm, unsigned at, unsigned n)
{
    (void)at;
    for (unsigned i = 0; i < n; i++) { pcm[2 * i] = 16384; pcm[2 * i + 1] = 0; }
}

static int write_left(const char *path, unsigned frames)
{
    return t_write_flac(path, &(t_flac_t){.frames = frames, .fill = left_only});
}

static void play_at(const char *path, int width, keep_t *k, eng_status_t *st)
{
    memset(k, 0, sizeof *k);
    audio_output_t out = {k_open, k_write, k_delay, k_nop, k_nop, k_pause, k_nop, k_describe, k, NULL, NULL, NULL, NULL};
    engine_t *e = engine_new(&out);
    engine_set_width(e, width);
    engine_play(e, path, 1);
    for (int i = 0; i < 400 && !k->frames; i++) t_sleep_ms(5);
    engine_status(e, st);
    engine_stop(e);
    engine_free(e);
}

static void test_engine(const char *dir)
{
    char path[300];
    snprintf(path, sizeof path, "%s/left.flac", dir);
    CHECK(write_left(path, 44100) == 0);
    keep_t k;
    eng_status_t st;
    const int32_t half = 16384 << 16;
    play_at(path, WIDTH_NORMAL, &k, &st);                 /* as recorded: untouched, at the file's 16 bits */
    CHECK(k.frames > 0 && k.first[0] == half && k.first[1] == 0);
    CHECK(st.width == WIDTH_NORMAL && st.untouched && k.bits == 16);
    play_at(path, WIDTH_MONO, &k, &st);                   /* mono: the same in both, and no longer bit-perfect */
    CHECK(k.frames > 0 && k.first[0] == k.first[1] && near(k.first[0], half / 2) <= 1);
    CHECK(st.width == WIDTH_MONO && !st.untouched && k.bits == 24);   /* (changed samples: a 24-bit output) */
    play_at(path, 150, &k, &st);                          /* wider: more left, the right in antiphase */
    CHECK(k.first[0] > 0 && k.first[1] < 0 && near(k.first[0], (int64_t)half * 250 / 300) <= 2);
    play_at(path, WIDTH_MAX, &k, &st);                    /* the widest: 3/4 left, -1/4 right */
    CHECK(near(k.first[0], (int64_t)half * 3 / 4) <= 2 && near(k.first[1], -(int64_t)half / 4) <= 2);
    play_at(path, 900, &k, &st);                          /* (held to the widest) */
    CHECK(st.width == WIDTH_MAX);
    unlink(path);
}

int main(void)
{
    char dir[200];
    t_mkdtemp(dir, sizeof dir, "fathom-test-width");
    test_arithmetic();
    test_engine(dir);
    rmdir(dir);
    return t_result();
}
