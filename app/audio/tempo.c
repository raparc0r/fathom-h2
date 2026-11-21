/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Faster without the pitch going up: WSOLA in whole numbers (tempo.h). */
#include "audio/tempo.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define SEQ_MS 40       /* a piece */
#define SEEK_MS 15      /* how far a piece may move to match */
#define OVERLAP_MS 8    /* the cross-fade between two pieces */
#define DECIM 4         /* the match: on every 4th sample */
#define IN_SPARE 8192   /* room for what comes in, beyond a piece and its search */

struct tempo {
    int ch, speed;
    long seq, seek, ov;
    int32_t *in;              /* what's come in, from in_pos to in_len (frames) */
    long in_cap, in_pos, in_len;
    int32_t *mid;             /* the last piece's end, to fade from */
    int16_t *mid_mono;        /* ...as the match sees it (every DECIM-th frame) */
    int16_t *cand;            /* the search's frames, as the match sees them (once per piece) */
    int32_t *fade;            /* the cross-fade's weights (of 32768) */
    int have_mid;
    uint32_t frac;            /* the skip's fraction (of 65536) */
    long skip;                /* input frames still to skip (a piece moved past what was in) */
};

tempo_t *tempo_new(unsigned rate, int channels, int speed_pct)
{
    if (channels < 1 || channels > 2 || !rate) return NULL;
    tempo_t *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->ch = channels;
    t->seq = (long)rate * SEQ_MS / 1000;
    if (t->seq > TEMPO_MAX_SEQ) t->seq = TEMPO_MAX_SEQ;
    t->seek = (long)rate * SEEK_MS / 1000;
    t->ov = (long)rate * OVERLAP_MS / 1000 / DECIM * DECIM;
    t->in_cap = t->seq + t->seek + IN_SPARE;
    t->in = malloc((size_t)t->in_cap * (size_t)channels * sizeof *t->in);
    t->mid = malloc((size_t)t->ov * (size_t)channels * sizeof *t->mid);
    t->mid_mono = malloc((size_t)(t->ov / DECIM) * sizeof *t->mid_mono);
    t->cand = malloc((size_t)(t->seek + t->ov) * sizeof *t->cand);
    t->fade = malloc((size_t)t->ov * sizeof *t->fade);
    if (!t->in || !t->mid || !t->mid_mono || !t->cand || !t->fade) { tempo_free(t); return NULL; }
    for (long i = 0; i < t->ov; i++) t->fade[i] = (int32_t)((i << 15) / t->ov);
    tempo_set_speed(t, speed_pct);
    return t;
}

void tempo_free(tempo_t *t)
{
    if (!t) return;
    free(t->in);
    free(t->mid);
    free(t->mid_mono);
    free(t->cand);
    free(t->fade);
    free(t);
}

void tempo_set_speed(tempo_t *t, int speed_pct)
{
    t->speed = speed_pct < 50 ? 50 : speed_pct > 300 ? 300 : speed_pct;
}

void tempo_reset(tempo_t *t)
{
    t->in_pos = t->in_len = 0;
    t->have_mid = 0;
    t->frac = 0;
    t->skip = 0;
}

long tempo_latency(const tempo_t *t) { return t->in_len - t->in_pos; }

/* A frame as the match sees it: mono, 12 bits (so a match's sums fit 32 bits) */
static inline int16_t mono(const int32_t *f, int ch)
{
    return (int16_t)(ch == 2 ? ((f[0] >> 21) + (f[1] >> 21)) : (f[0] >> 20));
}

static uint32_t isqrt32(uint32_t v)
{
    uint32_t r = 0, bit = 1u << 30;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; }
        else r >>= 1;
        bit >>= 2;
    }
    return r;
}

/* How well the piece at offset k continues the last one's end: correlation for its energy */
static int32_t match(const tempo_t *t, long k)
{
    const int16_t *c = t->cand + k;
    int32_t corr = 0, norm = 0;
    for (long i = 0, n = t->ov / DECIM; i < n; i++) {
        int32_t v = c[i * DECIM];
        corr += v * t->mid_mono[i];
        norm += v * v;
    }
    return (int32_t)(((int64_t)corr << 8) / ((int32_t)isqrt32((uint32_t)norm) + 1));
}

/* Where (0..seek-1) the piece starting at p best continues the last one's end:
 * every 4th offset first, then those around the best */
static long best_offset(tempo_t *t, const int32_t *p)
{
    for (long i = 0; i < t->seek + t->ov; i++) t->cand[i] = mono(p + i * t->ch, t->ch);
    long best = 0;
    int32_t bs = INT32_MIN;
    for (long k = 0; k < t->seek; k += 4) {
        int32_t sc = match(t, k);
        if (sc > bs) { bs = sc; best = k; }
    }
    long lo = best - 3 < 0 ? 0 : best - 3, hi = best + 3 >= t->seek ? t->seek - 1 : best + 3, b2 = best;
    for (long k = lo; k <= hi; k++) {
        if (k == best) continue;
        int32_t sc = match(t, k);
        if (sc > bs) { bs = sc; b2 = k; }
    }
    return b2;
}

/* One piece out (seq - ov frames); 0 if there isn't enough in yet */
static long piece(tempo_t *t, int32_t *out)
{
    if (t->in_len - t->in_pos < t->seq + t->seek) return 0;
    const int32_t *p = t->in + t->in_pos * t->ch;
    long off = t->have_mid ? best_offset(t, p) : 0;
    const int32_t *s = p + off * t->ch;
    long o = 0;
    int ch = t->ch;
    if (t->have_mid) {                               /* the cross-fade */
        for (long i = 0; i < t->ov; i++)
            for (int c = 0; c < ch; c++) {
                int64_t a = t->mid[i * ch + c], b = s[i * ch + c], w = t->fade[i];
                out[o * ch + c] = (int32_t)((a * (32768 - w) + b * w) >> 15);
                if (c == ch - 1) o++;
            }
    } else {
        memcpy(out, s, (size_t)t->ov * (size_t)ch * sizeof *out);
        o = t->ov;
    }
    long mid_len = t->seq - 2 * t->ov;               /* straight through */
    memcpy(out + o * ch, s + t->ov * ch, (size_t)mid_len * (size_t)ch * sizeof *out);
    o += mid_len;
    const int32_t *tail = s + (t->seq - t->ov) * ch; /* the next fade starts from here */
    memcpy(t->mid, tail, (size_t)t->ov * (size_t)ch * sizeof *t->mid);
    for (long i = 0; i < t->ov / DECIM; i++) t->mid_mono[i] = mono(tail + i * DECIM * ch, ch);
    t->have_mid = 1;
    /* on by (seq - ov) * speed frames of the input */
    uint64_t adv = (uint64_t)(t->seq - t->ov) * (uint64_t)t->speed * 65536 / 100 + t->frac;
    t->in_pos += (long)(adv >> 16);
    t->frac = (uint32_t)(adv & 0xFFFF);
    return o;
}

long tempo_run(tempo_t *t, const int32_t *in, long n, int32_t *out, long cap)
{
    long w = 0;
    int ch = t->ch;
    while (n > 0) {
        if (t->skip > 0) {                           /* the last piece moved past what had come in */
            long d = t->skip < n ? t->skip : n;
            in += d * ch;
            n -= d;
            t->skip -= d;
            continue;
        }
        if (t->in_len + n > t->in_cap && t->in_pos > 0) {   /* room: what's been used goes */
            long keep = t->in_len - t->in_pos;
            if (keep) memmove(t->in, t->in + t->in_pos * ch, (size_t)keep * (size_t)ch * sizeof *t->in);
            t->in_len = keep;
            t->in_pos = 0;
        }
        long take = t->in_cap - t->in_len;
        if (take > n) take = n;
        if (take <= 0) break;
        memcpy(t->in + t->in_len * ch, in, (size_t)take * (size_t)ch * sizeof *in);
        t->in_len += take;
        in += take * ch;
        n -= take;
        while (cap - w >= t->seq) {
            long got = piece(t, out + w * ch);
            if (!got) break;
            w += got;
            if (t->in_pos > t->in_len) {
                t->skip = t->in_pos - t->in_len;
                t->in_pos = t->in_len = 0;
                break;
            }
        }
    }
    return w;
}
