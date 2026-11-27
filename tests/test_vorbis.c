/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* Ogg Vorbis: decoding (app/audio/vorbis.c through decoder.c) and tags and
 * covers (app/library/tags.c), on files made here with libvorbisenc. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <vorbis/vorbisenc.h>

#include "audio/decoder.h"
#include "audio/vorbis.h"
#include "library/tags.h"
#include "test_util.h"

#define RATE 44100
#define FRAMES (RATE + 1234)          /* not a whole number of Vorbis blocks: the end is trimmed */

static void be32w(unsigned char *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

static char *b64(const unsigned char *in, size_t n)
{
    static const char a[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char *o = malloc(n / 3 * 4 + 8), *w = o;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < n ? (uint32_t)in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
        *w++ = a[v >> 18 & 63];
        *w++ = a[v >> 12 & 63];
        *w++ = i + 1 < n ? a[v >> 6 & 63] : '=';
        *w++ = i + 2 < n ? a[v & 63] : '=';
    }
    *w = '\0';
    return o;
}

/* METADATA_BLOCK_PICTURE's value: a FLAC PICTURE block holding img, in base64 */
static char *picture(int type, const unsigned char *img, size_t n)
{
    static const char mime[] = "image/jpeg", desc[] = "cover";
    size_t len = 4 + 4 + strlen(mime) + 4 + strlen(desc) + 16 + 4 + n;
    unsigned char *b = calloc(1, len), *p = b;
    be32w(p, (uint32_t)type); p += 4;
    be32w(p, (uint32_t)strlen(mime)); p += 4; memcpy(p, mime, strlen(mime)); p += strlen(mime);
    be32w(p, (uint32_t)strlen(desc)); p += 4; memcpy(p, desc, strlen(desc)); p += strlen(desc);
    be32w(p, 600); be32w(p + 4, 600); be32w(p + 8, 24); p += 16;
    be32w(p, (uint32_t)n); p += 4;
    memcpy(p, img, n);
    char *s = b64(b, len), *out = malloc(strlen(s) + 32);
    sprintf(out, "METADATA_BLOCK_PICTURE=%s", s);
    free(s);
    free(b);
    return out;
}

/* An Opus file written by hand, pages and all (no checksums: the tag
 * reader doesn't look at them): its two headers, the second with a title, a
 * small picture of another kind, then the front cover `front`, which spans
 * as many pages as it needs; and a page of sound. */
static void opus_page(FILE *f, int flags, uint32_t seq, long long granule, const unsigned char *body, size_t n, int ends)
{
    unsigned char h[27] = "OggS";
    size_t full = n / 255, nseg = full + (ends ? 1 : 0);
    h[5] = (unsigned char)flags;
    for (int i = 0; i < 8; i++) h[6 + i] = (unsigned char)((unsigned long long)granule >> (8 * i));
    h[14] = 0x4f; h[15] = 0x50; h[16] = 0x55; h[17] = 0x53;      /* the stream's serial */
    for (int i = 0; i < 4; i++) h[18 + i] = (unsigned char)(seq >> (8 * i));
    h[26] = (unsigned char)nseg;
    fwrite(h, 1, 27, f);
    for (size_t i = 0; i < full; i++) fputc(255, f);
    if (ends) fputc((int)(n % 255), f);
    fwrite(body, 1, n, f);
}

static void opus_packet(FILE *f, uint32_t *seq, const unsigned char *p, size_t n, int first_flags)
{
    int flags = first_flags;
    while (n >= 255 * 255) {                                     /* a page full of it, and more to come */
        opus_page(f, flags, (*seq)++, -1, p, 255 * 255, 0);
        p += 255 * 255;
        n -= 255 * 255;
        flags = 1;                                               /* continued */
    }
    opus_page(f, flags, (*seq)++, 0, p, n, 1);
}

static int make_opus(const char *path, const unsigned char *front, size_t front_n)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    uint32_t seq = 0;
    static const unsigned char head[19] = "OpusHead\x01\x02\x38\x01\x80\xbb\0\0\0\0";
    opus_packet(f, &seq, head, sizeof head, 2);
    static const unsigned char other_img[] = "\xFF\xD8\xFF\xE0 an artist's photo";
    char *other = picture(8, other_img, sizeof other_img), *cover = picture(3, front, front_n);
    const char *c[] = {"TITLE=Opus Song", other, cover, "ALBUM=Opus Album"};
    size_t n = 8 + 4 + 4 + 4;
    for (int i = 0; i < 4; i++) n += 4 + strlen(c[i]);
    unsigned char *t = malloc(n), *w = t;
    memcpy(w, "OpusTags", 8); w += 8;
    w[0] = 4; w[1] = w[2] = w[3] = 0; memcpy(w + 4, "test", 4); w += 8;
    w[0] = 4; w[1] = w[2] = w[3] = 0; w += 4;
    for (int i = 0; i < 4; i++) {
        uint32_t l = (uint32_t)strlen(c[i]);
        w[0] = (unsigned char)l; w[1] = (unsigned char)(l >> 8); w[2] = (unsigned char)(l >> 16); w[3] = (unsigned char)(l >> 24);
        memcpy(w + 4, c[i], l);
        w += 4 + l;
    }
    opus_packet(f, &seq, t, n, 0);
    static const unsigned char sound[1] = {0xfc};
    opus_page(f, 4, seq++, 960, sound, 1, 1);
    free(t);
    free(other);
    free(cover);
    return fclose(f);
}

static double tone(long i, int c) { return 0.5 * sin(2 * M_PI * (c ? 660.0 : 440.0) * (double)i / RATE); }

/* A stereo file of FRAMES frames: a tone, tags, and two pictures (an "other"
 * first, then the front cover, which spans pages) */
static int make_ogg(const char *path, const unsigned char *front, size_t front_n)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    vorbis_info vi;
    vorbis_comment vc;
    vorbis_dsp_state vd;
    vorbis_block vb;
    ogg_stream_state os;
    ogg_page og;
    ogg_packet op, oc, oh;
    vorbis_info_init(&vi);
    if (vorbis_encode_init_vbr(&vi, 2, RATE, 0.3f)) return -1;
    vorbis_comment_init(&vc);
    vorbis_comment_add_tag(&vc, "TITLE", "Ogg Title");
    vorbis_comment_add_tag(&vc, "ARTIST", "Someone");
    vorbis_comment_add_tag(&vc, "ALBUM", "Some Album");
    vorbis_comment_add_tag(&vc, "TRACKNUMBER", "7/12");
    vorbis_comment_add_tag(&vc, "DATE", "1999-04-01");
    vorbis_comment_add_tag(&vc, "REPLAYGAIN_TRACK_GAIN", "-6.50 dB");
    unsigned char *other = malloc(9000);
    for (int i = 0; i < 9000; i++) other[i] = (unsigned char)(i * 7);
    char *p0 = picture(0, other, 9000), *p3 = picture(3, front, front_n);
    vorbis_comment_add(&vc, p0);
    vorbis_comment_add(&vc, p3);
    vorbis_comment_add_tag(&vc, "GENRE", "Test");   /* after the pictures: read past them */
    free(other); free(p0); free(p3);

    vorbis_analysis_init(&vd, &vi);
    vorbis_block_init(&vd, &vb);
    ogg_stream_init(&os, 1234);
    vorbis_analysis_headerout(&vd, &vc, &op, &oc, &oh);
    ogg_stream_packetin(&os, &op);
    ogg_stream_packetin(&os, &oc);
    ogg_stream_packetin(&os, &oh);
    while (ogg_stream_flush(&os, &og)) { fwrite(og.header, 1, og.header_len, f); fwrite(og.body, 1, og.body_len, f); }
    for (long at = 0, eos = 0; !eos;) {
        long n = FRAMES - at > 1024 ? 1024 : FRAMES - at;
        if (n > 0) {
            float **buf = vorbis_analysis_buffer(&vd, (int)n);
            for (long i = 0; i < n; i++) { buf[0][i] = (float)tone(at + i, 0); buf[1][i] = (float)tone(at + i, 1); }
            vorbis_analysis_wrote(&vd, (int)n);
            at += n;
        } else {
            vorbis_analysis_wrote(&vd, 0);
        }
        while (vorbis_analysis_blockout(&vd, &vb) == 1) {
            vorbis_analysis(&vb, NULL);
            vorbis_bitrate_addblock(&vb);
            while (vorbis_bitrate_flushpacket(&vd, &op)) {
                ogg_stream_packetin(&os, &op);
                while (ogg_stream_pageout(&os, &og)) {
                    fwrite(og.header, 1, og.header_len, f);
                    fwrite(og.body, 1, og.body_len, f);
                    if (ogg_page_eos(&og)) eos = 1;
                }
            }
        }
        if (n <= 0 && !eos) {   /* the last page */
            while (ogg_stream_flush(&os, &og)) {
                fwrite(og.header, 1, og.header_len, f);
                fwrite(og.body, 1, og.body_len, f);
                if (ogg_page_eos(&og)) eos = 1;
            }
        }
    }
    ogg_stream_clear(&os);
    vorbis_block_clear(&vb);
    vorbis_dsp_clear(&vd);
    vorbis_comment_clear(&vc);
    vorbis_info_clear(&vi);
    return fclose(f);
}

static int32_t pcm[(FRAMES + 4096) * 2];

int main(void)
{
    char dir[200];
    t_mkdtemp(dir, sizeof dir, "fathom-test-vorbis");
    char path[256];
    snprintf(path, sizeof path, "%s/tone.ogg", dir);
    unsigned char *front = malloc(100000);
    for (int i = 0; i < 100000; i++) front[i] = (unsigned char)(i * 31 + 5);
    front[0] = 0xFF; front[1] = 0xD8;
    if (make_ogg(path, front, 100000)) { fprintf(stderr, "can't write %s\n", path); return 1; }

    /* which files are Vorbis */
    CHECK(vorbis_file_is_vorbis(path));
    static const unsigned char opus[] = "OggS\0\x02\0\0\0\0\0\0\0\0\1\0\0\0\0\0\0\0\0\0\0\0\x01\x13OpusHead\x01\x02";
    CHECK(!vorbis_sniff(opus, sizeof opus - 1));
    CHECK(!vorbis_sniff((const unsigned char *)"fLaC", 4));

    /* tags */
    tags_t t;
    CHECK(tags_supported(path));
    CHECK(tags_read(path, &t) == 0);
    CHECK(!strcmp(t.codec, "Vorbis"));
    CHECK(t.sample_rate == RATE && t.channels == 2 && t.bits == 0);
    CHECK(t.duration_ms == (uint32_t)((uint64_t)FRAMES * 1000 / RATE));
    CHECK(!strcmp(t.title, "Ogg Title") && !strcmp(t.artist, "Someone") && !strcmp(t.album, "Some Album"));
    CHECK(!strcmp(t.genre, "Test") && t.track == 7 && t.year == 1999);
    CHECK(t.has_rg_track && fabsf(t.rg_track_gain + 6.5f) < 0.001f);
    CHECK(t.has_picture);

    /* the front cover, not the first picture */
    unsigned char *img = NULL;
    size_t n = 0;
    CHECK(tags_picture(path, &img, &n) == 0);
    CHECK(n == 100000 && img && !memcmp(img, front, 100000));
    free(img);
    tags_set_max_picture(90000);                 /* too big: skipped */
    CHECK(tags_picture(path, &img, &n) == -1 && !img);
    tags_set_max_picture((size_t)-1);

    /* An Opus file's cover, read the same way (FFmpeg's reader holds the
     * whole comment header, and now keeps only its first 256 KB): the front
     * cover whatever its size, 100 KB and 3 MB; and none over the limit */
    {
        char op[256];
        snprintf(op, sizeof op, "%s/cover.opus", dir);
        size_t big_n = 3000000;
        unsigned char *big = malloc(big_n);
        for (size_t i = 0; i < big_n; i++) big[i] = (unsigned char)(i * 13 + 7);
        big[0] = 0xFF; big[1] = 0xD8;
        CHECK(make_opus(op, front, 100000) == 0);
        img = NULL;
        CHECK(tags_picture(op, &img, &n) == 0 && n == 100000 && img && !memcmp(img, front, 100000));
        free(img);
        CHECK(make_opus(op, big, big_n) == 0);
        img = NULL;
        CHECK(tags_picture(op, &img, &n) == 0 && n == big_n && img && !memcmp(img, big, big_n));
        free(img);
        img = NULL;
        tags_set_max_picture(1000000);
        CHECK(tags_picture(op, &img, &n) == -1 && !img);
        tags_set_max_picture((size_t)-1);
        free(big);
        unlink(op);
    }

    /* decoding: every frame (gapless: exactly what was encoded), the tone */
    audio_format_t fmt;
    decoder_t *d = decoder_open(path, &fmt);
    CHECK(d != NULL);
    if (d) {
        CHECK(!strcmp(fmt.codec, "Vorbis") && fmt.rate == RATE && fmt.channels == 2 && fmt.bits == 0);
        CHECK(fmt.total_frames == FRAMES);
        long got = 0, r;
        while ((r = decoder_read(d, pcm + got * 2, 1000)) > 0) got += r;
        CHECK(r == 0);
        CHECK(got == FRAMES);
        double err = 0, sig = 0;
        for (long i = 2048; i < FRAMES - 2048; i++)
            for (int c = 0; c < 2; c++) {
                double x = pcm[i * 2 + c] / 2147483648.0, y = tone(i, c);
                err += (x - y) * (x - y);
                sig += y * y;
            }
        CHECK(sig > 0 && err / sig < 0.01);          /* within -20 dB: it's the tone */

        /* seeking: to a frame, then the rest of the file from there */
        long at = FRAMES / 2;
        CHECK(decoder_seek(d, (uint64_t)at) == 0);
        static int32_t after[(FRAMES + 4096) * 2];
        long got2 = 0;
        while ((r = decoder_read(d, after + got2 * 2, 1000)) > 0) got2 += r;
        CHECK(got2 == FRAMES - at);
        int32_t worst = 0;
        for (long i = 0; i < 4096 && i < got2; i++)
            for (int c = 0; c < 2; c++) {
                int32_t e = abs(after[i * 2 + c] / 65536 - pcm[(at + i) * 2 + c] / 65536);
                if (e > worst) worst = e;
            }
        CHECK(worst < 64);                           /* the same samples, give or take rounding */
        decoder_close(d);
    }

    unlink(path);
    rmdir(dir);
    free(front);
    return t_result();
}
