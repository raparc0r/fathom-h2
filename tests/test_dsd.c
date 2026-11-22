/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/*
 * DSD: DSF and DFF files made here (sines through a second-order sigma-delta
 * modulator) decode to PCM at the right rate and length, the tones at their
 * level, the modulator's noise above the audio band filtered out; DSF and DFF
 * of the same bits give the same samples; seeking and gapless joins are exact
 * (the same samples as one continuous decode); tags; DoP; DST refused.
 * `test_dsd --bench`: the converter's cost here, and an estimate for the H2.
 */
#include "audio/decoder.h"
#include "audio/dsd.h"
#include "audio/engine.h"
#include "audio/eq.h"
#include "audio/output.h"
#include "library/tags.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "tmpdir.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

#define DSD64 2822400u
#define FS 176400.0

/* ---------- making DSD ---------- */

typedef struct { double f, a; } tone_t;

/* A sum of sines, second-order sigma-delta (STF 1, NTF (1 - z^-1)^2): n bytes, MSB first. */
static uint8_t *modulate(uint32_t rate, const tone_t *t, int nt, size_t n, double phase0)
{
    uint8_t *b = calloc(n, 1);
    double i1 = 0, i2 = 0, y = 0;
    for (size_t k = 0; k < n * 8; k++) {
        double x = 0, tt = (double)k / rate;
        for (int j = 0; j < nt; j++) x += t[j].a * sin(2 * M_PI * t[j].f * tt + phase0);
        i1 += x - y;
        i2 += i1 - y;
        y = i2 >= 0 ? 1 : -1;
        if (y > 0) b[k / 8] |= (uint8_t)(0x80 >> (k % 8));
    }
    return b;
}

static void put_le(FILE *f, uint64_t v, int n) { for (int i = 0; i < n; i++) fputc((int)(v >> (8 * i) & 0xff), f); }
static void put_be(FILE *f, uint64_t v, int n) { for (int i = n - 1; i >= 0; i--) fputc((int)(v >> (8 * i) & 0xff), f); }

static uint8_t rev8(uint8_t v)
{
    uint8_t r = 0;
    for (int i = 0; i < 8; i++) r |= (uint8_t)((v >> i & 1) << (7 - i));
    return r;
}

/* An ID3v2.3 tag: title, artist, album, and a small "picture" when pic. */
static size_t make_id3(uint8_t *out, const char *title, const char *artist, int pic)
{
    size_t n = 10;
    const char *ids[3] = {"TIT2", "TPE1", "TALB"};
    const char *vals[3] = {title, artist, "Test Tones"};
    for (int i = 0; i < 3; i++) {
        size_t l = strlen(vals[i]) + 1;
        memcpy(out + n, ids[i], 4);
        out[n + 4] = 0; out[n + 5] = 0; out[n + 6] = (uint8_t)(l >> 8); out[n + 7] = (uint8_t)l;
        out[n + 8] = out[n + 9] = 0;
        out[n + 10] = 0;                              /* Latin-1 */
        memcpy(out + n + 11, vals[i], l - 1);
        n += 10 + l;
    }
    if (pic) {
        static const uint8_t jpeg[] = {0xFF, 0xD8, 0xFF, 0xE0, 1, 2, 3, 4, 0xFF, 0xD9};
        size_t l = 1 + 11 + 1 + 1 + sizeof jpeg;       /* enc, "image/jpeg\0", type, "\0", data */
        memcpy(out + n, "APIC", 4);
        out[n + 4] = 0; out[n + 5] = 0; out[n + 6] = 0; out[n + 7] = (uint8_t)l;
        out[n + 8] = out[n + 9] = 0;
        uint8_t *p = out + n + 10;
        p[0] = 0;
        memcpy(p + 1, "image/jpeg", 11);
        p[12] = 3;
        p[13] = 0;
        memcpy(p + 14, jpeg, sizeof jpeg);
        n += 10 + l;
    }
    size_t body = n - 10;
    memcpy(out, "ID3\3\0\0", 6);
    out[6] = (uint8_t)(body >> 21 & 0x7f); out[7] = (uint8_t)(body >> 14 & 0x7f);
    out[8] = (uint8_t)(body >> 7 & 0x7f); out[9] = (uint8_t)(body & 0x7f);
    return n;
}

/* DSF: planar blocks of 4096 bytes per channel, LSB first, the last block
 * padded; the ID3 tag at the end. */
static void write_dsf(const char *path, uint8_t *const *ch, unsigned nch, size_t n, uint32_t rate, int tag)
{
    enum { BLOCK = 4096 };
    size_t blocks = (n + BLOCK - 1) / BLOCK, data = blocks * BLOCK * nch;
    uint8_t id3[512];
    size_t id3n = tag ? make_id3(id3, "Sine", "Fathom", 1) : 0;
    FILE *f = fopen(path, "wb");
    uint64_t total = 28 + 52 + 12 + data + id3n;
    fwrite("DSD ", 1, 4, f); put_le(f, 28, 8); put_le(f, total, 8); put_le(f, id3n ? 28 + 52 + 12 + data : 0, 8);
    fwrite("fmt ", 1, 4, f); put_le(f, 52, 8); put_le(f, 1, 4); put_le(f, 0, 4);
    put_le(f, nch == 1 ? 1 : 2, 4); put_le(f, nch, 4); put_le(f, rate, 4); put_le(f, 1, 4);
    put_le(f, (uint64_t)n * 8, 8); put_le(f, BLOCK, 4); put_le(f, 0, 4);
    fwrite("data", 1, 4, f); put_le(f, 12 + data, 8);
    for (size_t b = 0; b < blocks; b++)
        for (unsigned c = 0; c < nch; c++)
            for (size_t i = 0; i < BLOCK; i++) {
                size_t at = b * BLOCK + i;
                fputc(at < n ? rev8(ch[c][at]) : 0, f);
            }
    fwrite(id3, 1, id3n, f);
    fclose(f);
}

/* DFF: bytes interleaved, MSB first; DIIN's title, an "ID3 " chunk (tag). dst: a
 * DST chunk instead of the sound (the content doesn't matter: it's refused). */
static void write_dff(const char *path, uint8_t *const *ch, unsigned nch, size_t n, uint32_t rate, int tag, int dst)
{
    uint8_t id3[512];
    size_t id3n = tag ? make_id3(id3, "Sine", "Fathom", 0) : 0;
    static const char cname[] = "not compressed";     /* type 4, count 1, name 14: 19, padded */
    uint64_t prop = 4 + (12 + 4) + (12 + 2 + 4 * nch) + (12 + 19 + 1);
    uint64_t snd = (uint64_t)n * nch;
    static const char title[] = "Title in DIIN";      /* 13: + count 4 = 17, padded */
    uint64_t diti = 4 + sizeof title - 1, diin = 12 + diti + (diti & 1);
    uint64_t form = 4 + (12 + 4) + (12 + prop) + (12 + snd + (snd & 1)) + (12 + diin) + (id3n ? 12 + id3n + (id3n & 1) : 0);
    FILE *f = fopen(path, "wb");
    fwrite("FRM8", 1, 4, f); put_be(f, form, 8); fwrite("DSD ", 1, 4, f);
    fwrite("FVER", 1, 4, f); put_be(f, 4, 8); put_be(f, 0x01050000, 4);
    fwrite("PROP", 1, 4, f); put_be(f, prop, 8); fwrite("SND ", 1, 4, f);
    fwrite("FS  ", 1, 4, f); put_be(f, 4, 8); put_be(f, rate, 4);
    fwrite("CHNL", 1, 4, f); put_be(f, 2 + 4 * nch, 8); put_be(f, nch, 2);
    for (unsigned c = 0; c < nch; c++) fwrite(c ? "SRGT" : "SLFT", 1, 4, f);
    fwrite("CMPR", 1, 4, f); put_be(f, 19, 8); fwrite(dst ? "DST " : "DSD ", 1, 4, f);
    fputc(14, f); fwrite(cname, 1, 14, f); fputc(0, f);
    fwrite(dst ? "DST " : "DSD ", 1, 4, f); put_be(f, snd, 8);
    for (size_t i = 0; i < n; i++)
        for (unsigned c = 0; c < nch; c++) fputc(ch[c][i], f);
    if (snd & 1) fputc(0, f);
    fwrite("DIIN", 1, 4, f); put_be(f, diin, 8);
    fwrite("DITI", 1, 4, f); put_be(f, diti, 8); put_be(f, sizeof title - 1, 4); fwrite(title, 1, sizeof title - 1, f);
    if (diti & 1) fputc(0, f);
    if (id3n) {
        fwrite("ID3 ", 1, 4, f); put_be(f, id3n, 8); fwrite(id3, 1, id3n, f);
        if (id3n & 1) fputc(0, f);
    }
    fclose(f);
}

/* ---------- measuring ---------- */

static int32_t *decode_all(const char *path, unsigned flags, audio_format_t *fmt, long *frames)
{
    decoder_t *d = decoder_open_flags(path, fmt, flags);
    if (!d) { *frames = -1; return NULL; }
    long cap = (long)fmt->total_frames + 4096, got = 0, n;
    int32_t *buf = malloc((size_t)cap * fmt->channels * sizeof *buf);
    while (got < cap && (n = decoder_read(d, buf + got * fmt->channels, cap - got > 2048 ? 2048 : cap - got)) > 0) got += n;
    decoder_close(d);
    *frames = got;
    return buf;
}

/* Amplitude (of full scale) and phase of f in channel c over n frames from
 * `from` (n holding a whole number of periods). */
static double tone(const int32_t *s, int ch, int c, long from, long n, double f, double *phase)
{
    double re = 0, im = 0;
    for (long i = 0; i < n; i++) {
        double v = s[(from + i) * ch + c] / 2147483648.0, w = 2 * M_PI * f * (double)(from + i) / FS;
        re += v * cos(w);
        im += v * sin(w);
    }
    if (phase) *phase = atan2(re, im);              /* of sin(w + phase) */
    return 2 * sqrt(re * re + im * im) / n;
}

/* The same of the bitstream itself (+-1), over nbits from bit `from`. */
static double bits_tone(const uint8_t *b, uint32_t rate, size_t from, size_t nbits, double f)
{
    double re = 0, im = 0;
    for (size_t k = from; k < from + nbits; k++) {
        double v = b[k / 8] & (0x80 >> (k % 8)) ? 1 : -1, w = 2 * M_PI * f * (double)k / rate;
        re += v * cos(w);
        im += v * sin(w);
    }
    return 2 * sqrt(re * re + im * im) / (double)nbits;
}

static void fft(double *re, double *im, int n)
{
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { double t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; }
    }
    for (int len = 2; len <= n; len <<= 1) {
        double a = -2 * M_PI / len;
        for (int i = 0; i < n; i += len)
            for (int k = 0; k < len / 2; k++) {
                double wr = cos(a * k), wi = sin(a * k);
                double xr = re[i + k + len / 2] * wr - im[i + k + len / 2] * wi;
                double xi = re[i + k + len / 2] * wi + im[i + k + len / 2] * wr;
                re[i + k + len / 2] = re[i + k] - xr; im[i + k + len / 2] = im[i + k] - xi;
                re[i + k] += xr; im[i + k] += xi;
            }
    }
}

/* Power (dB of full scale squared) in [f0, f1] Hz of channel c, 16384 frames
 * from `from`, Blackman-Harris window. */
static double band_db(const int32_t *s, int ch, int c, long from, double f0, double f1)
{
    enum { N = 16384 };
    static double re[N], im[N];
    double w2 = 0;
    for (int i = 0; i < N; i++) {
        double x = 2 * M_PI * i / (N - 1);
        double w = 0.35875 - 0.48829 * cos(x) + 0.14128 * cos(2 * x) - 0.01168 * cos(3 * x);
        re[i] = w * s[(from + i) * ch + c] / 2147483648.0;
        im[i] = 0;
        w2 += w * w;
    }
    fft(re, im, N);
    double p = 0;
    for (int k = (int)(f0 * N / FS); k <= (int)(f1 * N / FS) && k <= N / 2; k++) p += re[k] * re[k] + im[k] * im[k];
    return 10 * log10(2 * p / (N * w2) + 1e-30);
}

/* ---------- an output that keeps what it's given (the engine test) ---------- */

typedef struct { int32_t *buf; long n, cap; int ch, opens; audio_format_t fmt; } keep_t;
static int k_open(void *c, const audio_format_t *f) { keep_t *k = c; k->fmt = *f; k->ch = f->channels; k->opens++; return 0; }
static long k_write(void *c, const int32_t *x, long n)
{
    keep_t *k = c;
    if (k->n + n > k->cap) n = k->cap - k->n;
    memcpy(k->buf + k->n * k->ch, x, (size_t)n * k->ch * sizeof *x);
    k->n += n;
    return n;
}
static long k_delay(void *c) { (void)c; return 0; }
static void k_nop(void *c) { (void)c; }
static void k_pause(void *c, int on) { (void)c; (void)on; }
static void k_describe(void *c, char *o, int cap) { (void)c; snprintf(o, (size_t)cap, "keep"); }

/* ---------- the bench ---------- */

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

static int bench(void)
{
    const tone_t t[] = {{1000, 0.5}};
    size_t n = DSD64 / 8;                             /* a second of DSD64 per channel */
    uint8_t *bits = modulate(DSD64, t, 1, 2 * n, 0);  /* reused as DSD128's second */
    int32_t *out = malloc(DSD_CHUNK * 2 * sizeof *out);
    double host[2];
    for (int r = 0; r < 2; r++) {
        unsigned ratio = 16 << r, g = ratio / 8;
        dsd2pcm_t *c = dsd2pcm_new(ratio, 2);
        double best = 1e9;
        for (int rep = 0; rep < 5; rep++) {
            double t0 = now_s();
            for (int sec = 0; sec < 4; sec++)
                for (size_t at = 0; at + (size_t)DSD_CHUNK * g <= n * (1u << r); at += (size_t)DSD_CHUNK * g) {
                    memcpy(dsd2pcm_in(c, 0), bits + at, (size_t)DSD_CHUNK * g);
                    memcpy(dsd2pcm_in(c, 1), bits + at, (size_t)DSD_CHUNK * g);
                    dsd2pcm_run(c, DSD_CHUNK, out);
                }
            double dt = (now_s() - t0) / 4;
            if (dt < best) best = dt;
        }
        host[r] = best * 100;
        printf("DSD%d stereo to 176.4 kHz: %.2f ms per second of audio (%.3f %% of a core here)\n",
               64 << r, best * 1e3, host[r]);
        dsd2pcm_free(c);
    }
    /* the whole decoder: reading (from the page cache here), bit order, interleaving */
    char dir[200], path[2][64];
    test_tmp(dir, sizeof dir, "fathom-bench-dsd");
    if (mkdtemp(dir)) {
        uint8_t *ch[2] = {bits, bits};
        snprintf(path[0], sizeof path[0], "%s/b.dsf", dir);
        snprintf(path[1], sizeof path[1], "%s/b.dff", dir);
        write_dsf(path[0], ch, 2, 2 * n, DSD64, 0);
        write_dff(path[1], ch, 2, 2 * n, DSD64, 0, 0);
        int32_t *buf = malloc(2048 * 2 * sizeof *buf);
        for (int k = 0; k < 2; k++) {
            double best = 1e9;
            for (int rep = 0; rep < 5; rep++) {
                audio_format_t f;
                double t0 = now_s();
                decoder_t *d = decoder_open(path[k], &f);
                while (d && decoder_read(d, buf, 2048) > 0) {}
                if (d) decoder_close(d);
                double dt = (now_s() - t0) / 2;
                if (dt < best) best = dt;
            }
            printf("decoder, DSD64 %s: %.2f ms per second of audio\n", k ? "DFF" : "DSF", best * 1e3);
            unlink(path[k]);
        }
        free(buf);
        rmdir(dir);
    }
    /* The EQ's 10 bands at 44.1 kHz cost 4 % on the H2: the same
     * kind of integer work (64-bit multiply-adds, loads), to scale by. */
    eq_profile_t p = {.preamp_db = -6, .n = 10};
    for (int i = 0; i < 10; i++) p.band[i] = (eq_band_t){EQ_PEAK, 31.25 * (1 << i), i % 2 ? 3 : -3, 1.0};
    eq_t *e = eq_create(&p, 44100, 2);
    int32_t *s = malloc(44100 * 2 * sizeof *s);
    for (int i = 0; i < 44100 * 2; i++) s[i] = (int32_t)(sin(i * 0.01) * 1e9);
    double best = 1e9;
    for (int rep = 0; rep < 5; rep++) {
        double t0 = now_s();
        for (int sec = 0; sec < 4; sec++) eq_run(e, s, 44100);
        double dt = (now_s() - t0) / 4;
        if (dt < best) best = dt;
    }
    eq_destroy(e);
    double eq_host = best * 100, scale = 4.0 / eq_host;
    printf("EQ, 10 bands at 44.1 kHz: %.3f %% here (4 %% on the H2: x%.0f)\n", eq_host, scale);
    printf("estimate for the H2: DSD64 %.1f %%, DSD128 %.1f %% of the CPU\n", host[0] * scale, host[1] * scale);
    free(s);
    free(out);
    free(bits);
    return 0;
}

/* ---------- the tests ---------- */

static void path_in(char *out, size_t cap, const char *dir, const char *name) { snprintf(out, cap, "%s/%s", dir, name); }

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "--bench")) return bench();
    char dir[200];
    test_tmp(dir, sizeof dir, "fathom-test-dsd");
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; }
    char dsf[256], dff[256], a[256], b[256], dst[256], mono[256], d128[256];
    path_in(dsf, sizeof dsf, dir, "sine.dsf");
    path_in(dff, sizeof dff, dir, "sine.dff");
    path_in(a, sizeof a, dir, "a.dsf");
    path_in(b, sizeof b, dir, "b.dsf");
    path_in(dst, sizeof dst, dir, "dst.dff");
    path_in(mono, sizeof mono, dir, "mono.dsf");
    path_in(d128, sizeof d128, dir, "sine128.dff");

    /* a second of DSD64: 1 kHz at the SACD reference level (50 %) on the left,
     * at a quarter on the right */
    size_t n = DSD64 / 8;
    const tone_t tl[] = {{1000, 0.5}}, tr[] = {{1000, 0.25}};
    uint8_t *ch[2] = {modulate(DSD64, tl, 1, n, 0), modulate(DSD64, tr, 1, n, 0)};
    write_dsf(dsf, ch, 2, n, DSD64, 1);
    write_dff(dff, ch, 2, n, DSD64, 1, 0);
    size_t split = 2 * 40001;                         /* a.dsf ends mid-block, on a frame */
    uint8_t *chb[2] = {ch[0] + split, ch[1] + split};
    write_dsf(a, ch, 2, split, DSD64, 0);
    write_dsf(b, chb, 2, n - split, DSD64, 0);
    write_dff(dst, ch, 2, 4096, DSD64, 0, 1);

    /* rate, length, level, noise */
    audio_format_t f;
    long frames;
    int32_t *pcm = decode_all(dsf, 0, &f, &frames);
    CHECK(pcm && f.rate == 176400 && f.channels == 2 && f.total_frames == 176400 && !f.bits && !f.dop &&
          !strcmp(f.codec, "DSD64"), "DSF: %u Hz %u ch %llu frames %u bits %s", f.rate, f.channels,
          (unsigned long long)f.total_frames, f.bits, f.codec);
    CHECK(frames == 176400, "DSF decoded %ld frames", frames);
    if (frames == 176400) {
        double ph, al = tone(pcm, 2, 0, 44100, 88200, 1000, &ph), ar = tone(pcm, 2, 1, 44100, 88200, 1000, NULL);
        double dbl = 20 * log10(al), dbr = 20 * log10(ar);
        printf("1 kHz: left %.3f dBFS (50 %% DSD), right %.3f dBFS (25 %%), phase %.2f deg\n", dbl, dbr, ph * 180 / M_PI);
        CHECK(fabs(dbl + 6.0206) < 0.02 && fabs(dbr + 12.0412) < 0.02, "levels %.4f %.4f dB", dbl, dbr);
        CHECK(fabs(ph) * 180 / M_PI < 1.0, "1 kHz delayed by %.2f degrees", ph * 180 / M_PI);  /* < half a frame */
        double noise = band_db(pcm, 2, 0, 50000, 60000, 88200), audio = band_db(pcm, 2, 0, 50000, 2000, 20000);
        printf("noise: 60-88.2 kHz %.1f dB, 2-20 kHz %.1f dB (the modulator's own)\n", noise, audio);
        CHECK(noise < -120, "noise above 60 kHz at %.1f dB", noise);
        CHECK(audio < -80, "noise in the audio band at %.1f dB", audio);
        /* the start and the end: the filter's delay taken out, nothing missing */
        double head = tone(pcm, 2, 0, 0, 882, 1000, NULL), tail = tone(pcm, 2, 0, 176400 - 882 - 30, 882, 1000, NULL);
        CHECK(fabs(20 * log10(head / al)) < 0.1 && fabs(20 * log10(tail / al)) < 0.1, "start %.3f, end %.3f", head, tail);
    }

    /* the same bits as DFF: the same samples */
    long frames2;
    audio_format_t f2;
    int32_t *pcm2 = decode_all(dff, 0, &f2, &frames2);
    CHECK(pcm2 && frames2 == frames && f2.rate == f.rate && !memcmp(pcm, pcm2, (size_t)frames * 2 * sizeof *pcm),
          "DFF differs from DSF (%ld frames)", frames2);
    free(pcm2);

    /* seeking: exactly the samples of a decode from the start, DSF and DFF */
    for (int k = 0; k < 2 && frames == 176400; k++) {
        decoder_t *d = decoder_open(k ? dff : dsf, &f);
        const long at[] = {100000, 0, 1, 21, 777, 4095, 4096, 50000, 176400 - 3000, 176400 - 5};
        int32_t got[2048 * 2];
        for (size_t i = 0; i < sizeof at / sizeof at[0]; i++) {
            CHECK(decoder_seek(d, (uint64_t)at[i]) == 0, "seek to %ld", at[i]);
            long want = 176400 - at[i] < 2048 ? 176400 - at[i] : 2048, m = 0, r;
            while (m < want && (r = decoder_read(d, got + m * 2, want - m)) > 0) m += r;
            CHECK(m == want && !memcmp(got, pcm + at[i] * 2, (size_t)m * 2 * sizeof *got), "%s: seek to %ld: %ld frames, differ",
                  k ? "DFF" : "DSF", at[i], m);
        }
        CHECK(decoder_seek(d, 176400) == 0 && decoder_read(d, got, 16) == 0, "seek to the end");
        CHECK(decoder_seek(d, 176401) != 0, "seek past the end");
        decoder_close(d);
    }

    /* gapless: a.dsf then b.dsf, joined, are the whole file */
    if (frames == 176400) {
        audio_format_t fa, fb;
        decoder_t *da = decoder_open(a, &fa), *db = NULL;
        int32_t *j = malloc((176400 + 64) * 2 * sizeof *j);
        long m = 0, r;
        decoder_set_next(da, b);
        while ((r = decoder_read(da, j + m * 2, 1000)) > 0) m += r;
        long ma = m;
        db = decoder_open(b, &fb);
        decoder_join(db, da);
        decoder_close(da);
        while (m < 176400 + 64 && (r = decoder_read(db, j + m * 2, 1000)) > 0) m += r;
        decoder_close(db);
        CHECK(fa.total_frames == 40001 && fb.total_frames == 176400 - 40001, "split lengths %llu %llu",
              (unsigned long long)fa.total_frames, (unsigned long long)fb.total_frames);
        CHECK(m == 176400 && !memcmp(j, pcm, (size_t)m * 2 * sizeof *j), "joined: %ld frames (%ld from a), not the whole", m, ma);
        /* not followed: each on its own, exactly its length */
        long la, lb;
        free(decode_all(a, 0, &fa, &la));
        free(decode_all(b, 0, &fb, &lb));
        CHECK(la == 40001 && lb == 176400 - 40001, "alone: %ld + %ld frames", la, lb);

        /* the same through the engine: one open, the whole file's samples */
        keep_t k = {.cap = 176400 + 4096};
        k.buf = calloc((size_t)k.cap * 2, sizeof *k.buf);
        audio_output_t out = {k_open, k_write, k_delay, k_nop, k_nop, k_pause, k_nop, k_describe, &k, NULL, NULL, NULL};
        engine_t *e = engine_new(&out);
        engine_play(e, a, 1);
        engine_pause(e, 1);                           /* b must be queued before a ends */
        engine_queue_next(e, b, 2);
        engine_pause(e, 0);
        eng_status_t st;
        for (int w = 0; w < 500; w++) {
            engine_status(e, &st);
            if (st.ended) break;
            usleep(10000);
        }
        engine_free(e);
        CHECK(st.ended && k.opens == 1 && k.n == 176400 && !memcmp(k.buf, pcm, (size_t)k.n * 2 * sizeof *k.buf),
              "engine: ended %d, %d opens, %ld frames", st.ended, k.opens, k.n);
        free(k.buf);
        free(j);
    }

    /* the pass and stop bands, mono: 20 kHz against what the bits hold of it (the
     * modulator is 54 dB from flat there: its own error moves the tone 0.01 dB),
     * and 100 kHz, which would fold to 76.4 kHz */
    {
        size_t mn = DSD64 / 16;
        const tone_t tm[] = {{20000, 0.25}, {100000, 0.25}};
        uint8_t *mch[1] = {modulate(DSD64, tm, 2, mn, 0)};
        write_dsf(mono, mch, 1, mn, DSD64, 0);
        long mf;
        audio_format_t fm;
        int32_t *mp = decode_all(mono, 0, &fm, &mf);
        CHECK(mp && fm.channels == 1 && mf == 88200, "mono: %ld frames, %u ch", mf, fm.channels);
        if (mp && mf == 88200) {
            double in = bits_tone(mch[0], DSD64, 22050 * 16, 44100 * 16, 20000);
            double t20 = 20 * log10(tone(mp, 1, 0, 22050, 44100, 20000, NULL) / in);
            double t76 = 20 * log10(tone(mp, 1, 0, 22050, 44100, 76400, NULL) / 0.25 + 1e-12);
            printf("20 kHz at %+.5f dB (%.4f in the bits); 100 kHz at %.1f dB\n", t20, in, t76);
            CHECK(fabs(t20) < 0.001, "20 kHz at %+.5f dB", t20);
            CHECK(t76 < -110, "100 kHz through at %.1f dB", t76);
        }
        free(mp);
        free(mch[0]);
    }

    /* DSD256 (DSF, mono): 176.4 kHz too */
    {
        char d256[256];
        path_in(d256, sizeof d256, dir, "sine256.dsf");
        size_t dn = 4 * DSD64 / 8 / 10;               /* a tenth of a second */
        uint8_t *dch[1] = {modulate(4 * DSD64, tl, 1, dn, 0)};
        write_dsf(d256, dch, 1, dn, 4 * DSD64, 0);
        long df;
        audio_format_t fd;
        int32_t *dp = decode_all(d256, 0, &fd, &df);
        CHECK(dp && fd.rate == 176400 && df == 17640 && !strcmp(fd.codec, "DSD256"), "DSD256: %u Hz, %ld frames, %s",
              fd.rate, df, fd.codec);
        if (dp && df == 17640) {
            double dbl = 20 * log10(tone(dp, 1, 0, 0, 17640, 1000, NULL));
            printf("DSD256: 1 kHz %.3f dBFS\n", dbl);
            CHECK(fabs(dbl + 6.0206) < 0.02, "DSD256: %.4f dB", dbl);
        }
        free(dp);
        free(dch[0]);
        unlink(d256);
    }

    /* DSD128 (DFF): 176.4 kHz too */
    {
        size_t dn = 2 * DSD64 / 16;                   /* half a second */
        uint8_t *dch[2] = {modulate(2 * DSD64, tl, 1, dn, 0), modulate(2 * DSD64, tr, 1, dn, 0)};
        write_dff(d128, dch, 2, dn, 2 * DSD64, 1, 0);
        long df;
        audio_format_t fd;
        int32_t *dp = decode_all(d128, 0, &fd, &df);
        CHECK(dp && fd.rate == 176400 && df == 88200 && !strcmp(fd.codec, "DSD128"), "DSD128: %u Hz, %ld frames, %s",
              fd.rate, df, fd.codec);
        if (dp && df == 88200) {
            double dbl = 20 * log10(tone(dp, 2, 0, 0, 88200, 1000, NULL));
            double noise = band_db(dp, 2, 0, 30000, 60000, 88200);
            printf("DSD128: 1 kHz %.3f dBFS, noise 60-88.2 kHz %.1f dB\n", dbl, noise);
            CHECK(fabs(dbl + 6.0206) < 0.02 && noise < -120, "DSD128: %.4f dB, noise %.1f dB", dbl, noise);
        }
        free(dp);
        tags_t t;
        CHECK(tags_read(d128, &t) == 0 && !strcmp(t.codec, "DSD128") && t.sample_rate == 2 * DSD64 && t.duration_ms == 500,
              "DSD128 tags: %s %u %u ms", t.codec, t.sample_rate, t.duration_ms);
        free(dch[0]);
        free(dch[1]);
    }

    /* DoP: the bits as they are, under alternating markers */
    {
        long df;
        audio_format_t fd;
        int32_t *dp = decode_all(dsf, DECODER_DOP, &fd, &df);
        CHECK(dp && fd.dop && fd.bits == 24 && fd.rate == 176400 && df == 176400, "DoP: %u Hz %u bits, %ld frames",
              fd.rate, fd.bits, df);
        int ok = dp != NULL;
        for (long i = 0; ok && i < df; i++)
            for (int c = 0; c < 2; c++) {
                uint32_t v = (uint32_t)dp[i * 2 + c];
                ok &= (v & 0xff) == 0 && v >> 24 == (i & 1 ? 0xFAu : 0x05u) &&
                      (v >> 16 & 0xff) == ch[c][2 * i] && (v >> 8 & 0xff) == ch[c][2 * i + 1];
            }
        CHECK(ok, "DoP frames aren't the DSD bits");

        /* through the engine: untouched (a balance set, but not for DoP), and
         * back to PCM at once when the output changes */
        keep_t k = {.cap = 176400 + 4096};
        k.buf = calloc((size_t)k.cap * 2, sizeof *k.buf);
        audio_output_t out = {k_open, k_write, k_delay, k_nop, k_nop, k_pause, k_nop, k_describe, &k, NULL, NULL, NULL};
        engine_t *e = engine_new(&out);
        engine_set_dop(e, 1);
        engine_set_balance(e, 1.0f, 0.5f);
        engine_play(e, dsf, 1);
        eng_status_t st;
        for (int w = 0; w < 500; w++) {
            engine_status(e, &st);
            if (st.ended) break;
            usleep(10000);
        }
        CHECK(st.ended && k.fmt.dop && k.fmt.bits == 24 && st.untouched && k.n == df && dp &&
              !memcmp(k.buf, dp, (size_t)df * 2 * sizeof *dp), "engine DoP: %ld frames, dop %d, untouched %d", k.n,
              k.fmt.dop, st.untouched);
        k.n = 0;
        engine_play(e, dsf, 2);
        engine_pause(e, 1);
        engine_seek_ms(e, 500);
        for (int w = 0; w < 100 && (st.starting || st.state != ENG_PAUSED || st.pos_ms < 400); w++) {
            usleep(10000);
            engine_status(e, &st);
        }
        engine_switch_output(e, NULL, NULL);          /* (another DAC, say): PCM from here */
        engine_status(e, &st);
        CHECK(!st.fmt.dop && !k.fmt.dop && st.state == ENG_PAUSED && st.pos_ms >= 400, "after the switch: dop %d %d, %u ms",
              st.fmt.dop, k.fmt.dop, st.pos_ms);
        engine_set_balance(e, 1.0f, 1.0f);
        k.n = 0;                                      /* (what's played from here: a chunk may have gone out before the pause) */
        engine_pause(e, 0);
        for (int w = 0; w < 500; w++) {
            engine_status(e, &st);
            if (st.ended) break;
            usleep(10000);
        }
        long from = 176400 - k.n;
        CHECK(st.ended && from > 0 && !memcmp(k.buf, pcm + from * 2, (size_t)k.n * 2 * sizeof *pcm),
              "after the switch: %ld frames, not the PCM from %ld", k.n, from);
        engine_free(e);
        free(k.buf);
        free(dp);
    }

    /* DST: refused, and left out of the library */
    {
        audio_format_t fd;
        tags_t t;
        decoder_t *d = decoder_open(dst, &fd);
        CHECK(!d && tags_read(dst, &t) != 0, "DST accepted");
        if (d) decoder_close(d);
    }

    /* tags: DSF's ID3 at the end, DFF's ID3 chunk; the cover */
    {
        tags_t t;
        CHECK(tags_supported(dsf) && tags_supported(dff) && tags_supported("/x/Y.DSF"), "not supported");
        CHECK(tags_read(dsf, &t) == 0, "DSF tags");
        CHECK(!strcmp(t.codec, "DSD64") && t.sample_rate == DSD64 && t.bits == 1 && t.channels == 2 &&
              t.duration_ms == 1000 && !strcmp(t.title, "Sine") && !strcmp(t.artist, "Fathom") &&
              !strcmp(t.album, "Test Tones") && t.has_picture,
              "DSF: %s %u Hz %u bits %u ch %u ms '%s' '%s' '%s' pic %d", t.codec, t.sample_rate, t.bits, t.channels,
              t.duration_ms, t.title, t.artist, t.album, t.has_picture);
        unsigned char *pic;
        size_t len;
        CHECK(tags_picture(dsf, &pic, &len) == 0 && len == 10 && pic[0] == 0xFF && pic[9] == 0xD9, "DSF cover");
        if (len) free(pic);
        CHECK(tags_read(dff, &t) == 0 && !strcmp(t.codec, "DSD64") && t.duration_ms == 1000 && !strcmp(t.title, "Sine") &&
              !strcmp(t.album, "Test Tones"), "DFF: %s %u ms '%s'", t.codec, t.duration_ms, t.title);
        CHECK(tags_read(a, &t) == 0 && !strcmp(t.title, "a"), "untagged DSF: '%s'", t.title);   /* the file's name */
        CHECK(tags_read(dst, &t) != 0, "DST tagged");
        char nodsd[256];
        path_in(nodsd, sizeof nodsd, dir, "plain.dff");
        write_dff(nodsd, ch, 2, 8192, DSD64, 0, 0);
        CHECK(tags_read(nodsd, &t) == 0 && !strcmp(t.title, "Title in DIIN"), "DIIN title: '%s'", t.title);
        unlink(nodsd);
    }

    /* cut short anywhere: refused, or plays what's there (silence after), no crash */
    for (int k = 0; k < 2; k++) {
        static const long cuts[] = {0, 3, 12, 30, 60, 85, 95, 140, 5000, 300000};
        FILE *src = fopen(k ? dff : dsf, "rb");
        uint8_t *whole = malloc(300000);
        size_t have = fread(whole, 1, 300000, src);
        fclose(src);
        for (size_t i = 0; i < sizeof cuts / sizeof cuts[0]; i++) {
            char cut[256];
            path_in(cut, sizeof cut, dir, k ? "cut.dff" : "cut.dsf");
            FILE *o = fopen(cut, "wb");
            fwrite(whole, 1, (size_t)cuts[i] < have ? (size_t)cuts[i] : have, o);
            fclose(o);
            audio_format_t fc;
            decoder_t *d = decoder_open(cut, &fc);
            long total = 0, r;
            int32_t buf[2048 * 2];
            while (d && (r = decoder_read(d, buf, 2048)) > 0) total += r;
            CHECK(!d || total == (long)fc.total_frames, "cut at %ld: %ld of %llu frames", cuts[i], total,
                  (unsigned long long)fc.total_frames);
            if (d) decoder_close(d);
            tags_t t;
            tags_read(cut, &t);
            unlink(cut);
        }
        free(whole);
    }

    free(pcm);
    free(ch[0]);
    free(ch[1]);
    const char *made[] = {dsf, dff, a, b, dst, mono, d128};
    for (size_t i = 0; i < sizeof made / sizeof made[0]; i++) unlink(made[i]);
    rmdir(dir);
    printf(fails ? "%d failures\n" : "dsd ok\n", fails);
    return fails != 0;
}
