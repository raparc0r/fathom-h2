/* SPDX-License-Identifier: GPL-3.0-or-later */
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
