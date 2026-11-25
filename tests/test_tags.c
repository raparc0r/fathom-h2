/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* MP3 tags (app/library/tags.c) on generated files: ID3v2.2's three-letter
 * frames and its PIC, ID3v2.3 and v2.4 extended headers, ID3v1 (and v1.1's
 * track number) alone or filling what a v2 tag left out, numbered genres. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "library/tags.h"
#include "tmpdir.h"

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)
#define CHECK_STR(got, want) do { if (strcmp((got), (want))) { \
    fprintf(stderr, "%s:%d: got \"%s\", want \"%s\"\n", __FILE__, __LINE__, (got), (want)); failures++; } } while (0)

typedef struct { unsigned char b[4096]; size_t n; } buf_t;

static void put(buf_t *b, const void *p, size_t n) { memcpy(b->b + b->n, p, n); b->n += n; }
static void put_be(buf_t *b, uint32_t v, int bytes)
{
    for (int i = bytes - 1; i >= 0; i--) b->b[b->n++] = (unsigned char)(v >> (8 * i));
}
static void put_syncsafe(buf_t *b, uint32_t v)
{
    for (int i = 3; i >= 0; i--) b->b[b->n++] = (unsigned char)((v >> (7 * i)) & 0x7f);
}

/* A text frame: encoding byte, then the text. */
static void frame(buf_t *b, int version, const char *id, int enc, const void *body, size_t n)
{
    if (version == 2) {
        put(b, id, 3);
        put_be(b, (uint32_t)n + 1, 3);
    } else {
        put(b, id, 4);
        if (version == 4) put_syncsafe(b, (uint32_t)n + 1);
        else put_be(b, (uint32_t)n + 1, 4);
        put_be(b, 0, 2);                               /* flags */
    }
    b->b[b->n++] = (unsigned char)enc;
    put(b, body, n);
}
static void text(buf_t *b, int version, const char *id, const char *s) { frame(b, version, id, 0, s, strlen(s)); }

/* The tag (header, `ext` an extended header's bytes, frames) and some
 * silent MPEG-1 Layer III frames, then a v1 tag if given. */
static void write_mp3(const char *path, int version, int flags, const buf_t *ext, const buf_t *frames, const unsigned char *v1)
{
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    if (frames) {
        buf_t h = {.n = 0};
        put(&h, "ID3", 3);
        h.b[h.n++] = (unsigned char)version;
        h.b[h.n++] = 0;
        h.b[h.n++] = (unsigned char)flags;
        put_syncsafe(&h, (uint32_t)((ext ? ext->n : 0) + frames->n + 64));   /* (and some padding) */
        fwrite(h.b, 1, h.n, f);
        if (ext) fwrite(ext->b, 1, ext->n, f);
        fwrite(frames->b, 1, frames->n, f);
        static const unsigned char pad[64];
        fwrite(pad, 1, sizeof pad, f);
    }
    static unsigned char mp3[417] = {0xFF, 0xFB, 0x90, 0x00};   /* 128 kb/s, 44.1 kHz */
    for (int i = 0; i < 40; i++) fwrite(mp3, 1, sizeof mp3, f);
    if (v1) fwrite(v1, 1, 128, f);
    fclose(f);
}

static void v1_field(unsigned char *at, const char *s, size_t n) { memcpy(at, s, strlen(s) < n ? strlen(s) : n); }

/* A tag with a picture of `pic` bytes (pic[i] = i * 7 + 3) between a title
 * and an album, written straight to the file: megabytes of it. `ext`: with
 * an extended header (v2.3, v2.4). `before`: bytes of some other big frame
 * ahead of the picture (GEOB, GEO). */
static void write_big_mp3(const char *path, int version, int ext, size_t pic, size_t before)
{
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    buf_t a = {.n = 0}, z = {.n = 0}, ph = {.n = 0}, bh = {.n = 0}, x = {.n = 0};
    text(&a, version, version == 2 ? "TT2" : "TIT2", "Big Tag Song");
    text(&z, version, version == 2 ? "TAL" : "TALB", "Big Tag Album");
    text(&z, version, version == 2 ? "TP1" : "TPE1", "Big Tag Artist");
    /* the picture frame's header and the start of its body */
    unsigned char lead[32];
    size_t ln = 0;
    lead[ln++] = 0;                                              /* Latin-1 */
    if (version == 2) { memcpy(lead + ln, "JPG", 3); ln += 3; }
    else { memcpy(lead + ln, "image/jpeg", 11); ln += 11; }
    lead[ln++] = 3;                                              /* front cover */
    lead[ln++] = 0;                                              /* no description */
    uint32_t psz = (uint32_t)(ln + pic), bsz = (uint32_t)before;
    if (version == 2) { put(&ph, "PIC", 3); put_be(&ph, psz, 3); put(&bh, "GEO", 3); put_be(&bh, bsz, 3); }
    else {
        put(&ph, "APIC", 4);
        if (version == 4) put_syncsafe(&ph, psz); else put_be(&ph, psz, 4);
        put_be(&ph, 0, 2);
        put(&bh, "GEOB", 4);
        if (version == 4) put_syncsafe(&bh, bsz); else put_be(&bh, bsz, 4);
        put_be(&bh, 0, 2);
    }
    if (ext) {
        if (version == 3) { put_be(&x, 6, 4); put_be(&x, 0, 2); put_be(&x, 0, 4); }
        else { put_syncsafe(&x, 6); x.b[x.n++] = 1; x.b[x.n++] = 0; }
    }
    size_t total = x.n + a.n + (before ? bh.n + before : 0) + ph.n + psz + z.n + 64;
    buf_t h = {.n = 0};
    put(&h, "ID3", 3);
    h.b[h.n++] = (unsigned char)version;
    h.b[h.n++] = 0;
    h.b[h.n++] = ext ? 0x40 : 0;
    put_syncsafe(&h, (uint32_t)total);
    fwrite(h.b, 1, h.n, f);
    fwrite(x.b, 1, x.n, f);
    fwrite(a.b, 1, a.n, f);
    if (before) {
        fwrite(bh.b, 1, bh.n, f);
        for (size_t i = 0; i < before; i++) fputc('g', f);
    }
    fwrite(ph.b, 1, ph.n, f);
    fwrite(lead, 1, ln, f);
    for (size_t i = 0; i < pic; i++) fputc((int)((i * 7 + 3) & 0xff), f);
    fwrite(z.b, 1, z.n, f);
    static const unsigned char pad[64];
    fwrite(pad, 1, sizeof pad, f);
    static unsigned char mp3[417] = {0xFF, 0xFB, 0x90, 0x00};
    for (int i = 0; i < 40; i++) fwrite(mp3, 1, sizeof mp3, f);
    fclose(f);
}

/* A FLAC with no audio: STREAMINFO, then a comment block of a title, a
 * comment of `big` bytes (a rip's log, lyrics, a cover as text), a sheet if
 * given, and an album after them. */
static void put_le32(FILE *f, uint32_t v) { for (int i = 0; i < 4; i++) fputc((int)((v >> (8 * i)) & 0xff), f); }
static void put_entry(FILE *f, const char *e) { put_le32(f, (uint32_t)strlen(e)); fwrite(e, 1, strlen(e), f); }

static void write_big_flac(const char *path, size_t big, const char *sheet)
{
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    unsigned char si[34] = {0};
    si[0] = 0x10; si[2] = 0x10;                                  /* block sizes 4096 */
    si[10] = 0x0A; si[11] = 0xC4; si[12] = 0x42; si[13] = 0xF0;  /* 44.1 kHz, stereo, 16-bit */
    si[15] = 0x02; si[16] = 0xB1; si[17] = 0x10;                 /* 176400 samples: 4 s */
    fwrite("fLaC", 1, 4, f);
    fputc(0x00, f); fputc(0, f); fputc(0, f); fputc(34, f);
    fwrite(si, 1, 34, f);
    const char *vendor = "test", *title = "TITLE=Big Comment Song", *album = "ALBUM=Big Comment Album";
    char sh[600] = "";
    if (sheet) snprintf(sh, sizeof sh, "CUESHEET=%s", sheet);
    uint32_t n = 3 + (sheet != NULL);
    uint32_t len = 4 + (uint32_t)strlen(vendor) + 4 + 4 + (uint32_t)strlen(title) + 4 + 4 + (uint32_t)big +
                   (sheet ? 4 + (uint32_t)strlen(sh) : 0) + 4 + (uint32_t)strlen(album);
    fputc(0x84, f); fputc((int)(len >> 16), f); fputc((int)((len >> 8) & 0xff), f); fputc((int)(len & 0xff), f);   /* the last block */
    put_entry(f, vendor);
    put_le32(f, n);
    put_entry(f, title);
    put_le32(f, 4 + (uint32_t)big);
    fwrite("LOG=", 1, 4, f);
    for (size_t i = 0; i < big; i++) fputc('x', f);
    if (sheet) put_entry(f, sh);
    put_entry(f, album);
    fclose(f);
}

/* `mp3` (a file that starts with an ID3 tag) as the ID3 chunk of a WAV or an
 * AIFF, after a little sound; `tag` 0: the same file without that chunk */
static void wrap_id3(const char *mp3, const char *out, int aiff, int tag)
{
    FILE *in = fopen(mp3, "rb"), *f = fopen(out, "wb");
    if (!in || !f) { perror(out); exit(1); }
    fseek(in, 0, SEEK_END);
    long n = ftell(in);
    rewind(in);
    unsigned char *b = malloc((size_t)n);
    if (!b || fread(b, 1, (size_t)n, in) != (size_t)n) exit(1);
    fclose(in);
    unsigned char sound[1001] = {0};                             /* (an odd size: padded) */
    uint32_t total = 4 + 8 + sizeof sound + 1 + (tag ? 8 + (uint32_t)n + ((uint32_t)n & 1) : 0);
    if (aiff) {
        fwrite("FORM", 1, 4, f);
        for (int i = 3; i >= 0; i--) fputc((int)((total >> (8 * i)) & 0xff), f);
        fwrite("AIFFSSND", 1, 8, f);
        for (int i = 3; i >= 0; i--) fputc((int)((sizeof sound >> (8 * i)) & 0xff), f);
    } else {
        fwrite("RIFF", 1, 4, f);
        put_le32(f, total);
        fwrite("WAVEdata", 1, 8, f);
        put_le32(f, sizeof sound);
    }
    fwrite(sound, 1, sizeof sound, f);
    fputc(0, f);
    if (tag) {
        fwrite(aiff ? "ID3 " : "id3 ", 1, 4, f);
        if (aiff) for (int i = 3; i >= 0; i--) fputc((int)(((uint32_t)n >> (8 * i)) & 0xff), f);
        else put_le32(f, (uint32_t)n);
        fwrite(b, 1, (size_t)n, f);
        if (n & 1) fputc(0, f);
    }
    free(b);
    fclose(f);
}

static int pic_right(const unsigned char *d, size_t n, size_t want)
{
    if (!d || n != want) return 0;
    for (size_t i = 0; i < n; i += 4099) if (d[i] != (unsigned char)((i * 7 + 3) & 0xff)) return 0;
    return d[n - 1] == (unsigned char)(((n - 1) * 7 + 3) & 0xff);
}

int main(void)
{
    char dir[200], path[300];
    test_tmp(dir, sizeof dir, "fathom-test-tags");
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; }
    tags_t t;

    /* ID3v2.2 (iTunes until 2005 or so): no tags at all before */
    buf_t fr = {.n = 0};
    text(&fr, 2, "TT2", "Caf\xE9 Song");                 /* (Latin-1) */
    text(&fr, 2, "TP1", "Old Artist");
    text(&fr, 2, "TP2", "Old Album Artist");
    text(&fr, 2, "TAL", "Old Album");
    text(&fr, 2, "TRK", "3/10");
    text(&fr, 2, "TPA", "2/2");
    text(&fr, 2, "TYE", "1999");
    text(&fr, 2, "TCO", "(17)");
    text(&fr, 2, "TCM", "Some Composer");
    static const unsigned char pic[] = "JPG\x03" "\0" "\xFF\xD8\xFF\xE0 not really a JPEG";
    frame(&fr, 2, "PIC", 0, pic, sizeof pic - 1);
    snprintf(path, sizeof path, "%s/v22.mp3", dir);
    write_mp3(path, 2, 0, NULL, &fr, NULL);
    CHECK(tags_read(path, &t) == 0);
    CHECK_STR(t.title, "Caf\xC3\xA9 Song");
    CHECK_STR(t.artist, "Old Artist");
    CHECK_STR(t.album_artist, "Old Album Artist");
    CHECK_STR(t.album, "Old Album");
    CHECK_STR(t.genre, "Rock");
    CHECK_STR(t.composer, "Some Composer");
    CHECK(t.track == 3 && t.disc == 2 && t.year == 1999 && t.has_picture);
    CHECK(!strcmp(t.codec, "MP3") && t.sample_rate == 44100);
    unsigned char *data = NULL;
    size_t len = 0;
    CHECK(tags_picture(path, &data, &len) == 0 && len == 22 && data && !memcmp(data, "\xFF\xD8\xFF\xE0", 4));
    free(data);

    /* ID3v2.3 with an extended header (6 bytes, its size not counting itself):
     * read as a frame, it ended the tag */
    fr.n = 0;
    text(&fr, 3, "TIT2", "Ext Three");
    text(&fr, 3, "TPE1", "Artist Three");
    text(&fr, 3, "TCON", "(8)Free Jazz");             /* (its own words win) */
    buf_t ext = {.n = 0};
    put_be(&ext, 6, 4);
    put_be(&ext, 0, 2);                                /* flags */
    put_be(&ext, 64, 4);                               /* padding size */
    snprintf(path, sizeof path, "%s/v23ext.mp3", dir);
    write_mp3(path, 3, 0x40, &ext, &fr, NULL);
    CHECK(tags_read(path, &t) == 0);
    CHECK_STR(t.title, "Ext Three");
    CHECK_STR(t.artist, "Artist Three");
    CHECK_STR(t.genre, "Free Jazz");

    /* ID3v2.4 with an extended header (syncsafe, counting itself), UTF-8 text */
    fr.n = 0;
    frame(&fr, 4, "TIT2", 3, "Ext F\xC3\xBCnf", 9);
    frame(&fr, 4, "TALB", 3, "Album Four", 10);
    frame(&fr, 4, "TCON", 3, "Electronic", 10);
    ext.n = 0;
    put_syncsafe(&ext, 6);
    ext.b[ext.n++] = 1;                                /* one byte of flags */
    ext.b[ext.n++] = 0;
    snprintf(path, sizeof path, "%s/v24ext.mp3", dir);
    write_mp3(path, 4, 0x40, &ext, &fr, NULL);
    CHECK(tags_read(path, &t) == 0);
    CHECK_STR(t.title, "Ext F\xC3\xBCnf");
    CHECK_STR(t.album, "Album Four");
    CHECK_STR(t.genre, "Electronic");

    /* ID3v1.1 only: the file name was all there was */
    unsigned char v1[128] = "TAG";
    v1_field(v1 + 3, "V1 Title", 30);
    v1_field(v1 + 33, "V1 Artist \xC9t\xE9", 30);     /* (Latin-1) */
    v1_field(v1 + 63, "V1 Album", 30);
    v1_field(v1 + 93, "2001", 4);
    v1_field(v1 + 97, "a comment", 28);
    v1[125] = 0;
    v1[126] = 7;                                       /* v1.1: track 7 */
    v1[127] = 8;                                       /* Jazz */
    snprintf(path, sizeof path, "%s/v1only.mp3", dir);
    write_mp3(path, 0, 0, NULL, NULL, v1);
    CHECK(tags_read(path, &t) == 0);
    CHECK_STR(t.title, "V1 Title");
    CHECK_STR(t.artist, "V1 Artist \xC3\x89t\xC3\xA9");
    CHECK_STR(t.album, "V1 Album");
    CHECK_STR(t.genre, "Jazz");
    CHECK(t.year == 2001 && t.track == 7);

    /* a v2.3 tag with just a title (and a numbered genre), v1 for the rest */
    fr.n = 0;
    text(&fr, 3, "TIT2", "From V2");
    text(&fr, 3, "TCON", "32");
    v1[126] = 0;                                       /* (v1.0: no track number) */
    v1[127] = 255;                                     /* (no genre) */
    snprintf(path, sizeof path, "%s/both.mp3", dir);
    write_mp3(path, 3, 0, NULL, &fr, v1);
    CHECK(tags_read(path, &t) == 0);
    CHECK_STR(t.title, "From V2");
    CHECK_STR(t.artist, "V1 Artist \xC3\x89t\xC3\xA9");
    CHECK_STR(t.genre, "Classical");
    CHECK(t.track == 0 && t.year == 2001);

    /* none at all: the file's name */
    snprintf(path, sizeof path, "%s/Bare Name.mp3", dir);
    write_mp3(path, 0, 0, NULL, NULL, NULL);
    CHECK(tags_read(path, &t) == 0);
    CHECK_STR(t.title, "Bare Name");
    CHECK(!t.artist[0] && !t.genre[0]);

    /* A tag too big to hold whole (a cover of megabytes in each song: read
     * whole, twice over, it took the H2's memory and the player with it):
     * the texts on both sides of the picture, and the picture itself from
     * the file, in each version; with an extended header; with another big
     * frame ahead of it */
    {
        static const struct { int version, ext; size_t pic, before; } big[] = {
            {3, 0, 3000000, 0}, {4, 0, 3000000, 0}, {2, 0, 3000000, 0},
            {3, 1, 2500000, 0}, {4, 1, 2500000, 0}, {3, 0, 1500000, 2000000}, {4, 0, 12000000, 0},
        };
        /* (on the H2, the limit its player sets: a picture of 12 MB held
         * whole is more than the H2 has, for this test as for the player) */
        size_t most = getenv("FATHOM_TEST_ON_H2") ? (size_t)4 * 1024 * 1024 : (size_t)-1;
        tags_set_max_picture(most);
        for (size_t i = 0; i < sizeof big / sizeof big[0]; i++) {
            snprintf(path, sizeof path, "%s/big%d.mp3", dir, (int)i);
            write_big_mp3(path, big[i].version, big[i].ext, big[i].pic, big[i].before);
            CHECK(tags_read(path, &t) == 0);
            CHECK_STR(t.title, "Big Tag Song");
            CHECK_STR(t.album, "Big Tag Album");
            CHECK_STR(t.artist, "Big Tag Artist");
            CHECK(t.has_picture == 1 && t.sample_rate == 44100);
            unsigned char *bd = NULL;
            size_t bn = 0;
            if (big[i].pic <= most) CHECK(tags_picture(path, &bd, &bn) == 0 && pic_right(bd, bn, big[i].pic));
            else CHECK(tags_picture(path, &bd, &bn) != 0 && bd == NULL);
            free(bd);
            /* a picture over the limit set (4 MB on the H2) is left unread */
            tags_set_max_picture(1024 * 1024);
            bd = NULL;
            CHECK(tags_picture(path, &bd, &bn) != 0 && bd == NULL);
            tags_set_max_picture(most);
            unlink(path);
        }
        /* a small picture in a big tag (behind a big frame of something else) is found as ever */
        snprintf(path, sizeof path, "%s/bigsmall.mp3", dir);
        write_big_mp3(path, 3, 0, 30000, 2000000);
        unsigned char *bd = NULL;
        size_t bn = 0;
        CHECK(tags_read(path, &t) == 0 && t.has_picture == 1);
        CHECK_STR(t.album, "Big Tag Album");
        CHECK(tags_picture(path, &bd, &bn) == 0 && pic_right(bd, bn, 30000));
        free(bd);
        unlink(path);
        /* a WAV's and an AIFF's cover, from the ID3 chunk they keep it in
         * (read from the file, as an MP3's: not through FFmpeg, twice over) */
        {
            char src[300], out[300];
            snprintf(src, sizeof src, "%s/forwav.mp3", dir);
            write_big_mp3(src, 3, 0, 3000001, 0);
            for (int aiff = 0; aiff < 2; aiff++) {
                snprintf(out, sizeof out, "%s/cover.%s", dir, aiff ? "aiff" : "wav");
                wrap_id3(src, out, aiff, 1);
                bd = NULL;
                CHECK(tags_picture(out, &bd, &bn) == 0 && pic_right(bd, bn, 3000001));
                free(bd);
                wrap_id3(src, out, aiff, 0);
                bd = NULL;
                CHECK(tags_picture(out, &bd, &bn) != 0 && bd == NULL);
                unlink(out);
            }
            unlink(src);
        }
        /* a FLAC whose comment block is megabytes (read whole before): the
         * tags on both sides of the big comment, and a sheet among them */
        snprintf(path, sizeof path, "%s/bigvc.flac", dir);
        write_big_flac(path, 12000000, NULL);
        CHECK(tags_read(path, &t) == 0 && t.sample_rate == 44100 && t.bits == 16 && t.duration_ms == 4000);
        CHECK_STR(t.title, "Big Comment Song");
        CHECK_STR(t.album, "Big Comment Album");
        unlink(path);
        snprintf(path, sizeof path, "%s/bigvcsheet.flac", dir);
        write_big_flac(path, 3000000, "FILE \"x.flac\" WAVE\n  TRACK 01 AUDIO\n    INDEX 01 00:00:00\n");
        char *sheet = NULL;
        size_t sn = 0;
        CHECK(tags_read(path, &t) == 0 && t.has_cuesheet == 1);
        CHECK_STR(t.album, "Big Comment Album");
        CHECK(tags_cuesheet(path, &sheet, &sn) == 0 && sheet && sn > 20 && !strncmp(sheet, "FILE \"x.flac\"", 13));
        free(sheet);
        unlink(path);
        /* and a tag just under the size held whole reads as before */
        snprintf(path, sizeof path, "%s/whole.mp3", dir);
        write_big_mp3(path, 3, 0, 900000, 0);
        bd = NULL;
        CHECK(tags_read(path, &t) == 0 && t.has_picture == 1);
        CHECK_STR(t.title, "Big Tag Song");
        CHECK(tags_picture(path, &bd, &bn) == 0 && pic_right(bd, bn, 900000));
        free(bd);
        unlink(path);
    }

    const char *names[] = {"v22", "v23ext", "v24ext", "v1only", "both", "Bare Name"};
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        snprintf(path, sizeof path, "%s/%s.mp3", dir, names[i]);
        unlink(path);
    }
    rmdir(dir);
    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    printf("tags: all good\n");
    return 0;
}
