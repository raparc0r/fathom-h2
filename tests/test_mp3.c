/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/*
 * MP3 files as the decoder opens them: the LAME/Info frame's encoder delay
 * and padding are taken off (gapless) whether or not an ID3v2 tag stands in
 * front of the audio, and a tag (a big cover in it) changes nothing of what
 * is decoded. The decoder passes over the tag without reading it (tags are
 * the library's: library/tags.c). Files written here by hand: an Info frame
 * and silent frames, 128 kb/s, 44.1 kHz stereo.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "audio/decoder.h"
#include "test_util.h"

#define FRAME_BYTES 417           /* 144 * 128000 / 44100 */
#define SPF 1152                  /* samples a frame */

static void be32(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}

/* An ID3v2.3 tag: a title, and a picture of `cover` bytes. */
static void write_id3(FILE *f, size_t cover)
{
    static const char mime[] = "image/jpeg";
    size_t apic = 1 + sizeof mime + 1 + 1 + cover;   /* encoding, mime and its NUL, type, description's NUL, data */
    size_t body = 10 + 6 + 10 + apic;
    unsigned char h[10] = {'I', 'D', '3', 3, 0, 0};
    h[6] = (unsigned char)(body >> 21 & 0x7f);
    h[7] = (unsigned char)(body >> 14 & 0x7f);
    h[8] = (unsigned char)(body >> 7 & 0x7f);
    h[9] = (unsigned char)(body & 0x7f);
    fwrite(h, 1, sizeof h, f);
    unsigned char fh[10] = {'T', 'I', 'T', '2'};
    be32(fh + 4, 6);
    fwrite(fh, 1, sizeof fh, f);
    fwrite("\0Title", 1, 6, f);
    memcpy(fh, "APIC", 4);
    be32(fh + 4, (unsigned long)apic);
    fwrite(fh, 1, sizeof fh, f);
    fputc(0, f);
    fwrite(mime, 1, sizeof mime, f);
    fputc(3, f);
    fputc(0, f);
    for (size_t i = 0; i < cover; i++) fputc((int)(i * 7 & 0xfe), f);   /* (never 0xFF: nothing that looks like a frame) */
}

/* `frames` silent frames after an Info frame that says how many there are
 * and what the encoder added: `delay` samples before, `padding` after. */
static int write_mp3(const char *path, size_t cover, unsigned frames, unsigned delay, unsigned padding)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    if (cover) write_id3(f, cover);
    unsigned char fr[FRAME_BYTES] = {0xFF, 0xFB, 0x90, 0x00};
    unsigned char *x = fr + 4 + 32;                  /* after the header and the (empty) side info */
    memcpy(x, "Info", 4);
    be32(x + 4, 0xF);                                /* frames, bytes, a seek table, quality */
    be32(x + 8, frames);
    be32(x + 12, (unsigned long)frames * FRAME_BYTES);
    for (int i = 0; i < 100; i++) x[16 + i] = (unsigned char)(i * 256 / 100);
    be32(x + 116, 50);
    unsigned char *l = x + 120;
    memcpy(l, "LAME3.100", 9);
    l[9] = 0x01;                                     /* tag revision 0, constant bitrate */
    l[21] = (unsigned char)(delay >> 4);
    l[22] = (unsigned char)((delay & 15) << 4 | padding >> 8);
    l[23] = (unsigned char)padding;
    fwrite(fr, 1, sizeof fr, f);
    memset(fr + 4, 0, sizeof fr - 4);
    for (unsigned i = 0; i < frames; i++) fwrite(fr, 1, sizeof fr, f);
    return fclose(f);
}

/* Everything the decoder gives for `path` from frame `from`: how many frames
 * (-1: it didn't open or seek), and a sum over the samples with their place. */
static long decode(const char *path, uint64_t from, audio_format_t *fmt, uint64_t *sum)
{
    decoder_t *d = decoder_open(path, fmt);
    if (!d) return -1;
    if (from && decoder_seek(d, from)) { decoder_close(d); return -1; }
    static int32_t buf[2048 * 2];
    long total = 0, n;
    uint64_t h = 1469598103934665603ULL;
    while ((n = decoder_read(d, buf, 2048)) > 0) {
        for (long i = 0; i < n * fmt->channels; i++) h = (h ^ (uint32_t)buf[i]) * 1099511628211ULL;
        total += n;
    }
    decoder_close(d);
    if (sum) *sum = h;
    return n < 0 ? -1 : total;
}

int main(void)
{
    char dir[200], bare[300], tagged[300];
    t_mkdtemp(dir, sizeof dir, "fathom-test-mp3");
    snprintf(bare, sizeof bare, "%s/bare.mp3", dir);
    snprintf(tagged, sizeof tagged, "%s/tagged.mp3", dir);
    const unsigned frames = 60, delay = 576, padding = 1105;
    const long want = (long)frames * SPF - delay - padding;
    CHECK(write_mp3(bare, 0, frames, delay, padding) == 0);
    CHECK(write_mp3(tagged, 300 * 1024, frames, delay, padding) == 0);   /* a 300 KB cover in front */

    audio_format_t fb, ft;
    uint64_t sb = 0, st = 0;
    long nb = decode(bare, 0, &fb, &sb), nt = decode(tagged, 0, &ft, &st);
    /* the length is the music's: the encoder's delay and padding are off it,
     * said at the open and true of what's read */
    CHECK(nb == want && (long)fb.total_frames == want);
    CHECK(nt == want && (long)ft.total_frames == want);
    if (nb != want || nt != want || (long)fb.total_frames != want || (long)ft.total_frames != want)
        fprintf(stderr, "want %ld frames: bare %ld (says %llu), tagged %ld (says %llu)\n", want, nb,
                (unsigned long long)fb.total_frames, nt, (unsigned long long)ft.total_frames);
    CHECK(fb.rate == 44100 && fb.channels == 2 && !strcmp(fb.codec, "MP3"));
    CHECK(ft.rate == 44100 && ft.channels == 2 && !strcmp(ft.codec, "MP3"));
    CHECK(sb == st);                                 /* the same samples, tag or no tag */

    /* a seek lands on the same frame of the music in both */
    long kb = decode(bare, 20000, &fb, &sb), kt = decode(tagged, 20000, &ft, &st);
    CHECK(kb == want - 20000 && kt == want - 20000 && sb == st);

    /* no Info frame's worth of knowledge lost with a tag so big that it is
     * most of the file (a cover of 3 MB) */
    CHECK(write_mp3(tagged, 3 * 1024 * 1024, frames, delay, padding) == 0);
    nt = decode(tagged, 0, &ft, &st);
    CHECK(nt == want && (long)ft.total_frames == want);

    unlink(bare);
    unlink(tagged);
    rmdir(dir);
    return t_result();
}
