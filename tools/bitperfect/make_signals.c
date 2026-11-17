/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * make-bitperfect: FLAC test files with known samples, for checking that the
 * player hands the sound card exactly what's in the file (tools/bitperfect).
 *
 *   make-bitperfect OUTDIR
 *
 * Writes NAME.flac and NAME.raw (the same samples, interleaved, signed little
 * endian at the file's width: 2 bytes for 16-bit, 3 for 24-bit) for:
 *   noise16_44k   16-bit / 44.1 kHz, 4 s
 *   noise24_44k   24-bit / 44.1 kHz, 4 s (played after noise16_44k: the card
 *                 must leave 16-bit mode)
 *   noise24_96k   24-bit / 96 kHz, 4 s
 *   noise24_192k  24-bit / 192 kHz, 4 s
 *   gapless_a, gapless_b  one 24/96 signal cut at an odd sample count
 *                 (gapless.raw is the whole signal)
 * Every signal starts with the extreme values (most positive, most negative,
 * zero) and is otherwise random full-scale noise, so every bit is exercised.
 */
#include <FLAC/stream_encoder.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t g_rng = 0x2545F491u;
static uint32_t rnd(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }

static int32_t *make_signal(int bits, long frames)
{
    int32_t *s = malloc((size_t)frames * 2 * sizeof *s);
    int32_t max = (1 << (bits - 1)) - 1, min = -(1 << (bits - 1));
    for (long i = 0; i < frames * 2; i++) {
        int32_t v = (int32_t)(rnd() >> (32 - bits));             /* 0 .. 2^bits-1 */
        s[i] = v - (1 << (bits - 1));                             /* full signed range */
    }
    static const int32_t edge_pos[] = {1, 0, 0, 1, 2, 2};         /* max, min, 0 per channel */
    for (int k = 0; k < 6; k++) s[k] = edge_pos[k] == 1 ? max : edge_pos[k] == 0 ? min : 0;
    return s;
}

static int write_raw(const char *path, const int32_t *s, long frames, int bits)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    int bytes = bits / 8;
    for (long i = 0; i < frames * 2; i++) {
        uint32_t v = (uint32_t)s[i];
        uint8_t b[3] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16)};
        fwrite(b, 1, (size_t)bytes, f);
    }
    return fclose(f);
}

static int write_flac(const char *path, const int32_t *s, long frames, int bits, unsigned rate)
{
    FLAC__StreamEncoder *e = FLAC__stream_encoder_new();
    FLAC__stream_encoder_set_channels(e, 2);
    FLAC__stream_encoder_set_bits_per_sample(e, (unsigned)bits);
    FLAC__stream_encoder_set_sample_rate(e, rate);
    FLAC__stream_encoder_set_compression_level(e, 5);
    FLAC__stream_encoder_set_total_samples_estimate(e, (FLAC__uint64)frames);
    if (FLAC__stream_encoder_init_file(e, path, NULL, NULL) != FLAC__STREAM_ENCODER_INIT_STATUS_OK) return -1;
    int ok = FLAC__stream_encoder_process_interleaved(e, s, (unsigned)frames);
    ok = FLAC__stream_encoder_finish(e) && ok;
    FLAC__stream_encoder_delete(e);
    return ok ? 0 : -1;
}

static int make(const char *dir, const char *name, int bits, unsigned rate, double secs)
{
    long frames = (long)(rate * secs);
    int32_t *s = make_signal(bits, frames);
    char p[1024];
    snprintf(p, sizeof p, "%s/%s.flac", dir, name);
    int rc = write_flac(p, s, frames, bits, rate);
    snprintf(p, sizeof p, "%s/%s.raw", dir, name);
    rc |= write_raw(p, s, frames, bits);
    printf("%s: %d-bit, %u Hz, %ld frames\n", name, bits, rate, frames);
    free(s);
    return rc;
}

int main(int argc, char **argv)
{
    if (argc != 2) { fprintf(stderr, "usage: %s OUTDIR\n", argv[0]); return 2; }
    const char *dir = argv[1];
    int rc = make(dir, "noise16_44k", 16, 44100, 4.0);
    rc |= make(dir, "noise24_44k", 24, 44100, 4.0);   /* after noise16_44k: the format must change */
    rc |= make(dir, "noise24_96k", 24, 96000, 4.0);
    rc |= make(dir, "noise24_192k", 24, 192000, 4.0);

    /* gapless: one signal, cut at an odd frame count */
    long frames = 96000 * 6, cut = 96000 * 2 + 12345;
    int32_t *s = make_signal(24, frames);
    char p[1024];
    snprintf(p, sizeof p, "%s/gapless_a.flac", dir);
    rc |= write_flac(p, s, cut, 24, 96000);
    snprintf(p, sizeof p, "%s/gapless_b.flac", dir);
    rc |= write_flac(p, s + cut * 2, frames - cut, 24, 96000);
    snprintf(p, sizeof p, "%s/gapless.raw", dir);
    rc |= write_raw(p, s, frames, 24);
    printf("gapless: 24-bit, 96000 Hz, %ld frames, cut at %ld\n", frames, cut);
    free(s);
    return rc ? 1 : 0;
}
