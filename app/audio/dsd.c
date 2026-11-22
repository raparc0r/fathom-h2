/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/*
 * DSD to PCM in two stages, both linear-phase FIRs (Kaiser-windowed sinc,
 * designed when a file opens; integers from there on: Fathom is built soft-float).
 *
 *  1. At the DSD rate, decimating to twice the output rate (352.8 kHz): a
 *     6 x ratio tap filter evaluated a byte at a time from tables of the sums
 *     of each byte's eight +-coefficients (256 entries per byte position), so
 *     a byte costs one look-up and one add, not eight multiplies. The taps
 *     are symmetric, so the older half's tables are the newer half's read
 *     with the byte's bits reversed: half the tables (6 KB at DSD64, 12 KB at
 *     DSD128), which keeps them in the X1000's 16 KB data cache. It only has
 *     to stop what would fold into the 0-60 kHz that stage 2 keeps: flat to
 *     24 kHz, 133 dB down from 292.8 kHz.
 *  2. At 352.8 kHz, decimating by 2: 77 taps (symmetric: 39 multiplies), flat
 *     to 28 kHz, 116 dB down from 60 kHz, where the DSD noise is climbing.
 *
 * Stage 1's output is Q28 (headroom for a pair sum in stage 2's fold, with the
 * filter's sum of |taps| at 1.34), stage 2's taps Q30, a 64-bit accumulator
 * (MIPS madd), one rounding to 24 bits at the end.
 */
#define _FILE_OFFSET_BITS 64
#include "dsd.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>

/* ---------- the filters ---------- */

#define S1_BYTES_PER_16 12   /* stage 1: 12 bytes (96 taps) at ratio 16, more in step with it */
#define S1_BETA 13.9
#define S2_TAPS 77
#define S2_BETA 12.2
#define PASS 0.136           /* 24 kHz at 176.4 kHz out, as a fraction of the output rate */
#define STOP 0.340           /* 60 kHz */
#define SILENCE 0x69         /* DSD's idle pattern: as many ones as zeros */

struct dsd2pcm {
    unsigned ratio, ch, group;   /* group: bytes per channel per output frame (ratio / 8) */
    unsigned l1;                 /* stage 1's length in bytes */
    unsigned delay, span;
    int32_t *t1;                 /* l1 / 2 tables of 256: byte j back from the newest */
    int32_t h2[(S2_TAPS + 1) / 2];
    uint8_t *b[2];               /* l1 - 1 bytes of history, then the run's input */
    uint8_t *r[2];               /* the same bytes bit-reversed (the older half's index) */
    uint8_t rev[256];
    int32_t *s[2];               /* S2_TAPS - 1 stage-1 outputs of history, then the run's */
};

static double bessel_i0(double x)
{
    double sum = 1, term = 1;
    for (int k = 1; k < 64; k++) {
        term *= x / 2 / k;
        sum += term * term;
        if (term * term < 1e-21 * sum) break;
    }
    return sum;
}

/* Low-pass, cut-off fc in cycles per sample, unity gain at DC. */
static void kaiser_lowpass(double *h, int n, double fc, double beta)
{
    double sum = 0, w0 = bessel_i0(beta);
    for (int i = 0; i < n; i++) {
        double x = i - (n - 1) / 2.0, r = 2.0 * i / (n - 1) - 1;
        double sinc = x == 0 ? 2 * fc : sin(2 * M_PI * fc * x) / (M_PI * x);
        h[i] = sinc * bessel_i0(beta * sqrt(r * r < 1 ? 1 - r * r : 0)) / w0;
        sum += h[i];
    }
    for (int i = 0; i < n; i++) h[i] /= sum;
}

dsd2pcm_t *dsd2pcm_new(unsigned ratio, unsigned channels)
{
    if (ratio < 16 || ratio % 16 || ratio > 128 || channels < 1 || channels > 2) return NULL;
    dsd2pcm_t *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->ratio = ratio;
    c->ch = channels;
    c->group = ratio / 8;
    c->l1 = S1_BYTES_PER_16 * ratio / 16;
    unsigned taps = 8 * c->l1;
    double *h = malloc(taps * sizeof *h);
    c->t1 = malloc((size_t)c->l1 / 2 * 256 * sizeof *c->t1);
    for (unsigned k = 0; k < channels; k++) {
        c->b[k] = malloc(c->l1 - 1 + (size_t)DSD_CHUNK * c->group);
        c->r[k] = malloc(c->l1 - 1 + (size_t)DSD_CHUNK * c->group);
        c->s[k] = malloc((S2_TAPS - 1 + 2 * (size_t)DSD_CHUNK) * sizeof *c->s[k]);
        if (!c->b[k] || !c->r[k] || !c->s[k]) { free(h); dsd2pcm_free(c); return NULL; }
    }
    for (unsigned v = 0; v < 256; v++) {
        unsigned x = 0;
        for (int i = 0; i < 8; i++) x |= (v >> i & 1) << (7 - i);
        c->rev[v] = (uint8_t)x;
    }
    if (!h || !c->t1) { free(h); dsd2pcm_free(c); return NULL; }

    /* stage 1: keep what stage 2 keeps, stop what would fold onto it */
    kaiser_lowpass(h, (int)taps, (PASS + 2 - STOP) / 2 / ratio, S1_BETA);
    for (unsigned j = 0; j < c->l1 / 2; j++)
        for (unsigned v = 0; v < 256; v++) {
            double a = 0;
            for (unsigned i = 0; i < 8; i++)          /* bit 0 is the newest: tap 8j + i */
                a += (v >> i & 1) ? h[8 * j + i] : -h[8 * j + i];
            c->t1[j * 256 + v] = (int32_t)lround(a * (1 << 28));
        }
    double h2[S2_TAPS];
    kaiser_lowpass(h2, S2_TAPS, (PASS + STOP) / 4, S2_BETA);
    for (unsigned i = 0; i < (S2_TAPS + 1) / 2; i++) c->h2[i] = (int32_t)lround(h2[i] * (1 << 30));
    free(h);

    /* Output k, made when group k is in, is centred on bit
     * (k + 1) * ratio - ratio * (S2_TAPS - 1) / 4 - 1 - (taps - 1) / 2,
     * that is on group k - delay (21 frames, 0.12 ms, at every ratio). */
    c->delay = (unsigned)lround((S2_TAPS - 1) / 4.0 - 1 + (1 + (taps - 1) / 2.0) / ratio);
    /* Frame k's oldest byte is (2k + 3 - S2_TAPS) * group / 2 - l1: from group
     * p on, frames from p + span on owe nothing to what came before p. */
    c->span = (S2_TAPS - 3) / 2 + (c->l1 + c->group - 1) / c->group;
    dsd2pcm_reset(c);
    return c;
}

void dsd2pcm_free(dsd2pcm_t *c)
{
    if (!c) return;
    for (unsigned k = 0; k < 2; k++) { free(c->b[k]); free(c->r[k]); free(c->s[k]); }
    free(c->t1);
    free(c);
}

void dsd2pcm_reset(dsd2pcm_t *c)
{
    for (unsigned k = 0; k < c->ch; k++) {
        memset(c->b[k], SILENCE, c->l1 - 1);
        memset(c->r[k], c->rev[SILENCE], c->l1 - 1);
        memset(c->s[k], 0, (S2_TAPS - 1) * sizeof *c->s[k]);
    }
}

void dsd2pcm_copy(dsd2pcm_t *dst, const dsd2pcm_t *src)
{
    for (unsigned k = 0; k < dst->ch; k++) {
        memcpy(dst->b[k], src->b[k], dst->l1 - 1);
        memcpy(dst->r[k], src->r[k], dst->l1 - 1);
        memcpy(dst->s[k], src->s[k], (S2_TAPS - 1) * sizeof *dst->s[k]);
    }
}

uint8_t *dsd2pcm_in(dsd2pcm_t *c, unsigned ch) { return c->b[ch] + c->l1 - 1; }
unsigned dsd2pcm_delay(const dsd2pcm_t *c) { return c->delay; }
unsigned dsd2pcm_span(const dsd2pcm_t *c) { return c->span; }

/* Stage 1 for m outputs, `half` bytes apart; byte l1 - 1 - j back from the
 * newest is table j's, bit-reversed. Inlined with l1 and half constant, so the
 * compiler unrolls it: a look-up is then a byte load, a shift, an add, a load
 * and an add. */
static inline __attribute__((always_inline)) void stage1(const int32_t *t1, const uint8_t *b, const uint8_t *r,
                                                          int32_t *s, long m, unsigned half, unsigned l1)
{
    for (long i = 0; i < m; i++) {
        const uint8_t *p = b + (i + 1) * half - 1, *q = r + (i + 1) * half - l1;
        int32_t a = 0;
#pragma GCC unroll 24
        for (unsigned j = 0; j < l1 / 2; j++) a += t1[j * 256 + p[-(long)j]] + t1[j * 256 + q[j]];
        s[i] = a;
    }
}

void dsd2pcm_run(dsd2pcm_t *c, long n, int32_t *out)
{
    const unsigned l1 = c->l1, ch = c->ch;
    const int32_t *t1 = c->t1;
    const long bytes = n * c->group;
    for (unsigned k = 0; k < ch; k++) {
        /* stage 1: two outputs per group, each from the l1 newest bytes */
        const uint8_t *b = c->b[k] + l1 - 1;      /* the run's first byte */
        uint8_t *r = c->r[k] + l1 - 1;
        for (long i = 0; i < bytes; i++) r[i] = c->rev[b[i]];
        int32_t *s = c->s[k] + S2_TAPS - 1;
        switch (c->ratio) {
        case 16: stage1(t1, b, r, s, 2 * n, 1, 12); break;
        case 32: stage1(t1, b, r, s, 2 * n, 2, 24); break;
        default: stage1(t1, b, r, s, 2 * n, c->group / 2, l1); break;
        }
        /* stage 2: one output per pair, the symmetric taps folded */
        const int32_t *h2 = c->h2;
        for (long i = 0; i < n; i++) {
            const int32_t *o = c->s[k] + 2 * i + 1;   /* the oldest of S2_TAPS */
            int64_t a = (int64_t)h2[S2_TAPS / 2] * o[S2_TAPS / 2];
#pragma GCC unroll 38
            for (unsigned t = 0; t < S2_TAPS / 2; t++) a += (int64_t)h2[t] * (o[t] + o[S2_TAPS - 1 - t]);
            /* Q58 -> 24 bits, rounded, saturated */
            int64_t v = (a + ((int64_t)1 << 34)) >> 35;
            if (v > 0x7fffff) v = 0x7fffff;
            if (v < -0x800000) v = -0x800000;
            out[i * ch + k] = (int32_t)((uint32_t)v << 8);
        }
        memmove(c->b[k], c->b[k] + bytes, l1 - 1);
        memmove(c->r[k], c->r[k] + bytes, l1 - 1);
        memmove(c->s[k], c->s[k] + 2 * n, (S2_TAPS - 1) * sizeof *c->s[k]);
    }
}

/* ---------- the files ---------- */

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t le64(const uint8_t *p) { return le32(p) | (uint64_t)le32(p + 4) << 32; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint64_t be64(const uint8_t *p) { return (uint64_t)be32(p) << 32 | be32(p + 4); }

static int read_at(FILE *f, uint64_t at, void *buf, size_t n)
{
    return fseeko(f, (off_t)at, SEEK_SET) == 0 && fread(buf, 1, n, f) == n ? 0 : -1;
}

/* rate / (64 x 44.1 kHz or 64 x 48 kHz): 1, 2, 4 (DSD64, 128, 256); 0 otherwise */
static unsigned rate_mult(uint32_t rate)
{
    unsigned m = rate % 2822400 == 0 ? rate / 2822400 : rate % 3072000 == 0 ? rate / 3072000 : 0;
    return m == 1 || m == 2 || m == 4 ? m : 0;
}

const char *dsd_name(uint32_t rate)
{
    switch (rate_mult(rate)) {
    case 1: return "DSD64";
    case 2: return "DSD128";
    case 4: return "DSD256";
    default: return NULL;
    }
}

int dsd_path(const char *path)
{
    const char *dot = strrchr(path, '.'), *slash = strrchr(path, '/');
    return dot && (!slash || dot > slash) && (!strcasecmp(dot, ".dsf") || !strcasecmp(dot, ".dff"));
}

static int probe_dsf(FILE *f, dsd_info_t *in, const uint8_t *h)
{
    uint64_t at = le64(h + 4);                        /* the "DSD " chunk's size: 28 */
    uint64_t meta = le64(h + 20);
    uint8_t c[52];
    if (at < 28 || read_at(f, at, c, sizeof c) || memcmp(c, "fmt ", 4)) return -1;
    uint64_t fsz = le64(c + 4);
    if (le32(c + 16) != 0) return -1;                 /* format id 0: DSD raw */
    in->channels = le32(c + 24);
    in->rate = le32(c + 28);
    unsigned bits = le32(c + 32);
    uint64_t samples = le64(c + 36);
    in->block = le32(c + 44);
    if (bits != 1 && bits != 8) return -1;
    in->lsb_first = bits == 1;
    uint8_t d[12];
    at += fsz;
    if (fsz < 52 || read_at(f, at, d, sizeof d) || memcmp(d, "data", 4) || le64(d + 4) < 12) return -1;
    in->data = at + 12;
    in->bytes = samples / 8;
    if (in->channels < 1 || in->channels > 8 || !in->block || in->block > (1u << 20)) return -1;
    /* the data chunk holds whole blocks; a count beyond it is a damaged file */
    uint64_t room = (le64(d + 4) - 12) / in->channels;
    if (in->bytes > room) in->bytes = room;
    if (meta) {
        uint8_t id[10];
        if (read_at(f, meta, id, sizeof id) == 0 && !memcmp(id, "ID3", 3)) {
            in->id3 = meta;
            in->id3_len = 10 + ((uint64_t)(id[6] & 0x7f) << 21 | (uint64_t)(id[7] & 0x7f) << 14 |
                                (uint64_t)(id[8] & 0x7f) << 7 | (id[9] & 0x7f));
        }
    }
    return 0;
}

/* DIIN's DITI and DIAR: a 32-bit count, then the text (the format names no
 * encoding: taken as it is) */
static void dff_text(FILE *f, uint64_t at, uint64_t size, char *out, size_t cap)
{
    uint8_t n[4];
    if (size < 4 || read_at(f, at, n, 4)) return;
    uint32_t len = be32(n);
    if (len > size - 4) len = (uint32_t)(size - 4);
    if (len >= cap) len = (uint32_t)cap - 1;
    if (fread(out, 1, len, f) != len) len = 0;
    out[len] = '\0';
}

static int probe_dff(FILE *f, dsd_info_t *in, const uint8_t *h)
{
    in->dff = 1;
    uint64_t end = 12 + be64(h + 4), at = 16;
    int have_cmpr = 0;
    for (uint8_t c[12]; at + 12 <= end && read_at(f, at, c, 12) == 0;) {
        uint64_t size = be64(c + 4), body = at + 12;
        if (size > end - body) size = end - body;      /* (a damaged size must not wrap round) */
        if (!memcmp(c, "PROP", 4)) {
            uint8_t t[4];
            if (size < 4 || read_at(f, body, t, 4) || memcmp(t, "SND ", 4)) return -1;
            for (uint64_t p = body + 4; p + 12 <= body + size;) {
                uint8_t s[12], v[4];
                if (read_at(f, p, s, 12)) return -1;
                uint64_t sz = be64(s + 4);
                if (sz > body + size - p - 12) sz = body + size - p - 12;
                if (!memcmp(s, "FS  ", 4) && sz >= 4 && fread(v, 1, 4, f) == 4) in->rate = be32(v);
                else if (!memcmp(s, "CHNL", 4) && sz >= 2 && fread(v, 1, 2, f) == 2) in->channels = (unsigned)v[0] << 8 | v[1];
                else if (!memcmp(s, "CMPR", 4) && sz >= 4 && fread(v, 1, 4, f) == 4) {
                    have_cmpr = 1;
                    in->dst = memcmp(v, "DSD ", 4) != 0;
                }
                p += 12 + sz + (sz & 1);
            }
        } else if (!memcmp(c, "DSD ", 4)) {
            in->data = body;
            in->bytes = in->channels ? size / in->channels : 0;
        } else if (!memcmp(c, "DST ", 4)) {
            in->dst = 1;
        } else if (!memcmp(c, "DIIN", 4)) {
            for (uint64_t p = body; p + 12 <= body + size;) {
                uint8_t s[12];
                if (read_at(f, p, s, 12)) break;
                uint64_t sz = be64(s + 4);
                if (sz > body + size - p - 12) sz = body + size - p - 12;
                if (!memcmp(s, "DITI", 4)) dff_text(f, p + 12, sz, in->title, sizeof in->title);
                else if (!memcmp(s, "DIAR", 4)) dff_text(f, p + 12, sz, in->artist, sizeof in->artist);
                p += 12 + sz + (sz & 1);
            }
        } else if (!strncasecmp((const char *)c, "ID3 ", 4)) {
            in->id3 = body;
            in->id3_len = size;
        }
        at = body + size + (size & 1);
    }
    if (!have_cmpr || !in->rate || in->channels < 1 || in->channels > 8) return -1;
    return in->dst || in->data ? 0 : -1;
}

int dsd_probe(FILE *f, dsd_info_t *in)
{
    memset(in, 0, sizeof *in);
    uint8_t h[28];
    if (read_at(f, 0, h, sizeof h)) return -1;
    int r = !memcmp(h, "DSD ", 4) ? probe_dsf(f, in, h)
          : !memcmp(h, "FRM8", 4) && !memcmp(h + 12, "DSD ", 4) ? probe_dff(f, in, h) : -1;
    if (r || in->dst || fseeko(f, 0, SEEK_END)) return r;
    /* a file cut short (a copy that stopped): as long as what's there, not
     * what the header promised (in whole DSF blocks) */
    off_t end = ftello(f);
    uint64_t have = end > 0 && (uint64_t)end > in->data ? (uint64_t)end - in->data : 0;
    uint64_t most = in->dff ? have / in->channels : have / ((uint64_t)in->block * in->channels) * in->block;
    if (in->bytes > most) in->bytes = most;
    return 0;
}

/* ---------- the decoder ---------- */

struct dsd_dec {
    FILE *f;
    dsd_info_t in;
    int dop;
    unsigned group;          /* bytes per channel per output frame */
    uint64_t frames;         /* the track's */
    uint64_t pos;            /* the next group to read */
    /* To PCM: the converter's output y[k] (k counted from the track's first
     * group) is the track's frame z = k - delay. Frames below zmin are dropped
     * (before a seek target; a predecessor's, after a join). */
    dsd2pcm_t *conv;
    int64_t yk, zmin;
    unsigned delay;
    int followed;
    unsigned dop_marker;     /* DoP: the next frame's marker is 0xFA (else 0x05) */
    uint8_t *plane[2];       /* DoP: each channel's bytes for a run */
    uint8_t *buf;            /* DSF: a block for every channel; DFF: interleaved bytes */
    uint64_t blk;            /* the DSF block in buf, + 1 (0: none) */
    uint8_t rev[256];        /* bit reversal (DSF is LSB first) */
};

dsd_dec_t *dsd_open(const char *path, int dop, audio_format_t *fmt)
{
    FILE *f = fopen(path, "rbe");
    if (!f) return NULL;
    dsd_dec_t *d = calloc(1, sizeof *d);
    if (!d) { fclose(f); return NULL; }
    d->f = f;
    unsigned mult;
    if (dsd_probe(f, &d->in) || d->in.dst || d->in.channels > 2 || !(mult = rate_mult(d->in.rate))) {
        dsd_close(d);                 /* DST, multichannel (a stereo DAC, no downmix), DSD512 */
        return NULL;
    }
    d->dop = dop;
    unsigned ch = d->in.channels, ratio = 16 * mult;
    d->group = dop ? 2 : ratio / 8;
    d->frames = d->in.bytes / d->group;
    if (d->in.dff) d->in.block = 4096;        /* (for reading: DFF has no blocks) */
    d->buf = malloc((size_t)d->in.block * ch);
    if (!d->buf) { dsd_close(d); return NULL; }
    if (dop) {
        for (unsigned k = 0; k < ch; k++)
            if (!(d->plane[k] = malloc((size_t)DSD_CHUNK * d->group))) { dsd_close(d); return NULL; }
    } else {
        if (!(d->conv = dsd2pcm_new(ratio, ch))) { dsd_close(d); return NULL; }
        for (unsigned k = 0; k < ch; k++) d->plane[k] = dsd2pcm_in(d->conv, k);
        d->delay = dsd2pcm_delay(d->conv);
    }
    for (unsigned v = 0; v < 256; v++) {
        unsigned r = 0;
        for (int i = 0; i < 8; i++) r |= (v >> i & 1) << (7 - i);
        d->rev[v] = (uint8_t)r;
    }
    memset(fmt, 0, sizeof *fmt);
    fmt->rate = d->in.rate / (dop ? 16 : ratio);
    fmt->channels = (uint8_t)ch;
    fmt->bits = dop ? 24 : 0;          /* PCM made from DSD: not a source depth */
    fmt->dop = (uint8_t)dop;
    fmt->total_frames = d->frames;
    snprintf(fmt->codec, sizeof fmt->codec, "%s", dsd_name(d->in.rate));
    return d;
}

void dsd_close(dsd_dec_t *d)
{
    if (!d) return;
    if (d->f) fclose(d->f);
    if (d->conv) dsd2pcm_free(d->conv);
    else for (unsigned k = 0; k < 2; k++) free(d->plane[k]);
    free(d->buf);
    free(d);
}

/* n groups from d->pos into the planes, MSB first. Read a block at a time
 * (DSF's own blocks, a channel after another; DFF's bytes interleaved, in
 * blocks of the same size). A short file reads as silence. */
static void fetch(dsd_dec_t *d, long n)
{
    unsigned ch = d->in.channels;
    size_t len = (size_t)n * d->group;
    uint64_t q = d->pos * d->group;               /* per-channel byte offset */
    uint32_t bs = d->in.block;
    for (size_t done = 0; done < len;) {
        uint64_t b = (q + done) / bs;
        uint32_t off = (uint32_t)((q + done) % bs);
        if (d->blk != b + 1) {                    /* whole blocks: one read for every channel */
            size_t want = (size_t)bs * ch, got = 0;
            if (fseeko(d->f, (off_t)(d->in.data + b * want), SEEK_SET) == 0) got = fread(d->buf, 1, want, d->f);
            memset(d->buf + got, d->in.lsb_first ? d->rev[SILENCE] : SILENCE, want - got);
            d->blk = b + 1;
        }
        size_t m = bs - off;
        if (m > len - done) m = len - done;
        if (d->in.dff) {
            const uint8_t *src = d->buf + (size_t)off * ch;
            for (size_t i = 0; i < m; i++)
                for (unsigned k = 0; k < ch; k++) d->plane[k][done + i] = src[i * ch + k];
            done += m;
            continue;
        }
        for (unsigned k = 0; k < ch; k++) {
            const uint8_t *src = d->buf + (size_t)k * bs + off;
            uint8_t *dst = d->plane[k] + done;
            if (d->in.lsb_first) for (size_t i = 0; i < m; i++) dst[i] = d->rev[src[i]];
            else memcpy(dst, src, m);
        }
        done += m;
    }
}

static long read_dop(dsd_dec_t *d, int32_t *out, long max)
{
    unsigned ch = d->in.channels;
    long got = 0;
    while (got < max && d->pos < d->frames) {
        long n = max - got;
        if (n > DSD_CHUNK) n = DSD_CHUNK;
        if ((uint64_t)n > d->frames - d->pos) n = (long)(d->frames - d->pos);
        fetch(d, n);
        /* 24 bits: the marker, then 16 bits of DSD, earliest first */
        for (long i = 0; i < n; i++, d->dop_marker ^= 1) {
            uint32_t mk = d->dop_marker ? 0xFA : 0x05;
            for (unsigned k = 0; k < ch; k++)
                out[(got + i) * ch + k] = (int32_t)((mk << 24) | (uint32_t)d->plane[k][2 * i] << 16 |
                                                    (uint32_t)d->plane[k][2 * i + 1] << 8);
        }
        d->pos += (uint64_t)n;
        got += n;
    }
    return got;
}

long dsd_read(dsd_dec_t *d, int32_t *out, long max)
{
    if (d->dop) return read_dop(d, out, max);
    unsigned ch = d->in.channels;
    int64_t frames = (int64_t)d->frames;
    long got = 0;
    while (got < max) {
        long n = max - got;
        if (n > DSD_CHUNK) n = DSD_CHUNK;
        int64_t z = d->yk - d->delay;                 /* the frame the next group makes */
        if (d->pos < d->frames) {
            if ((uint64_t)n > d->frames - d->pos) n = (long)(d->frames - d->pos);
            fetch(d, n);
            d->pos += (uint64_t)n;
        } else if (!d->followed && z < frames) {
            /* the end: silence through the filter for its last frames */
            if (n > frames - z) n = (long)(frames - z);
            for (unsigned k = 0; k < ch; k++) memset(d->plane[k], SILENCE, (size_t)n * d->group);
        } else {
            break;
        }
        dsd2pcm_run(d->conv, n, out + got * ch);
        d->yk += n;
        int64_t drop = d->zmin - z;
        if (drop > n) drop = n;
        if (drop > 0)
            memmove(out + got * ch, out + (got + drop) * ch, (size_t)(n - drop) * ch * sizeof *out);
        got += n - (drop > 0 ? (long)drop : 0);
    }
    return got;
}

int dsd_seek(dsd_dec_t *d, uint64_t frame)
{
    if (frame > d->frames) return -1;
    if (d->dop) { d->pos = frame; return 0; }
    /* start far enough back that the history is all real by the target */
    int64_t p = (int64_t)frame + d->delay - dsd2pcm_span(d->conv);
    if (p < 0) p = 0;
    dsd2pcm_reset(d->conv);
    d->pos = (uint64_t)p;
    d->yk = p;
    d->zmin = (int64_t)frame;
    return 0;
}

void dsd_set_followed(dsd_dec_t *d, int followed) { d->followed = followed; }

int dsd_join(dsd_dec_t *next, const dsd_dec_t *prev)
{
    if (next->dop != prev->dop || next->in.rate != prev->in.rate || next->in.channels != prev->in.channels ||
        next->pos || prev->pos != prev->frames)
        return -1;
    if (next->dop) {                             /* the markers keep alternating */
        next->dop_marker = prev->dop_marker;
        return 0;
    }
    if (next->yk || prev->yk != (int64_t)prev->frames) return -1;   /* prev played out its end already */
    dsd2pcm_copy(next->conv, prev->conv);
    /* next's first groups make prev's last frames: those prev didn't give out */
    int64_t pz = prev->yk - prev->delay;
    if (pz < prev->zmin) pz = prev->zmin;
    next->zmin = pz - (int64_t)prev->frames;
    return 0;
}
