/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * CUE sheets (app/library/cue.c): the parser (encodings, INDEX times, what's
 * missing), then a generated album as one FLAC with a .cue next to it and
 * another with its sheet in a CUESHEET tag, scanned, played (the parts, a
 * seek, the gapless join through the engine) and scanned again.
 *
 * Each sample says where it is: left = frame & 0xFFFF, right = frame >> 16,
 * so any frame out of place shows.
 */
#include <FLAC/metadata.h>
#include <FLAC/stream_encoder.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <sqlite3.h>

#include "audio/decoder.h"
#include "audio/engine.h"
#include "audio/output.h"
#include "library/cue.h"
#include "library/library.h"
#include "tmpdir.h"

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

#define RATE   44100
#define FRAMES (10 * RATE)   /* 10 s */

static void sleep_ms(int ms)
{
    struct timespec t = {ms / 1000, (long)(ms % 1000) * 1000000};
    nanosleep(&t, NULL);
}

/* ---------- the parser ---------- */

static cue_sheet_t S;

static int parse(const char *text) { return cue_parse(text, strlen(text), &S); }

static void test_parser(void)
{
    /* the usual: album fields, pregaps, unquoted values, REMs */
    CHECK(parse("REM GENRE \"Jazz Rock\"\r\n"
                "REM DATE 1999 / 2003\r\n"
                "REM DISCNUMBER 2\r\n"
                "REM REPLAYGAIN_ALBUM_GAIN -7.50 dB\r\n"
                "PERFORMER \"The Band\"\r\n"
                "TITLE \"An \"Album\"\"\r\n"
                "FILE \"Album.wav\" WAVE\r\n"
                "  TRACK 01 AUDIO\r\n"
                "    TITLE \"One\"\r\n"
                "    REM REPLAYGAIN_TRACK_GAIN -6.25 dB\r\n"
                "    INDEX 01 00:00:00\r\n"
                "  TRACK 02 AUDIO\r\n"
                "    TITLE Two Unquoted\r\n"
                "    PERFORMER \"Guest\"\r\n"
                "    SONGWRITER \"Writer\"\r\n"
                "    INDEX 00 03:25:00\r\n"
                "    INDEX 01 03:25:74\r\n"
                "  TRACK 03 AUDIO\r\n"
                "    INDEX 01 120:00:01\r\n") == 3);
    CHECK(!strcmp(S.title, "An \"Album\"") && !strcmp(S.performer, "The Band") && !strcmp(S.genre, "Jazz Rock"));
    CHECK(S.year == 1999 && S.disc == 2 && S.has_rg_album && S.rg_album_gain == -7.5f);
    CHECK(S.nfiles == 1 && !strcmp(S.files[0], "Album.wav"));
    CHECK(S.tracks[0].number == 1 && S.tracks[0].start == 0 && !strcmp(S.tracks[0].title, "One"));
    CHECK(S.tracks[0].has_rg && S.tracks[0].rg_gain == -6.25f && !S.tracks[1].has_rg);
    CHECK(!strcmp(S.tracks[1].title, "Two Unquoted") && !strcmp(S.tracks[1].performer, "Guest"));
    CHECK(!strcmp(S.tracks[1].songwriter, "Writer"));
    CHECK(S.tracks[1].start == (3 * 60 + 25) * 75 + 74);           /* INDEX 01, not the pregap */
    CHECK(S.tracks[2].start == 120 * 60 * 75 + 1);                  /* minutes past 99 */
    CHECK(!S.tracks[2].title[0] && !S.tracks[2].performer[0]);      /* missing: left empty */
    CHECK(cue_file_tracks(&S, 0) == 3);

    /* a UTF-8 BOM; no album fields at all; an unquoted FILE name with spaces */
    CHECK(parse("\xEF\xBB\xBF" "FILE My Album.flac WAVE\nTRACK 1 AUDIO\nTITLE \"Caf\xC3\xA9\"\nINDEX 1 0:0:0\n"
                "TRACK 2 AUDIO\nINDEX 01 00:01:00\n") == 2);
    CHECK(!strcmp(S.files[0], "My Album.flac") && !S.title[0] && !S.performer[0] && !S.year);
    CHECK(!strcmp(S.tracks[0].title, "Caf\xC3\xA9") && S.tracks[1].start == 75);

    /* not UTF-8: Windows-1252 / Latin-1 */
    CHECK(parse("TITLE \"Caf\xE9 \x92n\x92 Cr\xE8me\"\nFILE \"a.flac\" WAVE\nTRACK 01 AUDIO\nINDEX 01 00:00:00\n") == 1);
    CHECK(!strcmp(S.title, "Caf\xC3\xA9 \xE2\x80\x99n\xE2\x80\x99 Cr\xC3\xA8me"));
    /* a lone continuation byte, a cut-short sequence: also not UTF-8 */
    CHECK(parse("TITLE \"x\xC3\"\n") == 0 && !strcmp(S.title, "x\xC3\x83"));

    /* UTF-16 (EAC's "Unicode" sheets), little and big endian */
    const char *u = "TITLE \"\xC3\xA9t\xC3\xA9\"\nFILE \"a.flac\" WAVE\nTRACK 01 AUDIO\nINDEX 01 00:00:00\n";
    char w16[512], b16[512];
    size_t n = 2;
    w16[0] = (char)0xFF; w16[1] = (char)0xFE;
    b16[0] = (char)0xFE; b16[1] = (char)0xFF;
    for (const unsigned char *c = (const unsigned char *)u; *c; c++) {
        unsigned v = *c;
        if (*c == 0xC3) { v = 0xC0 | (c[1] & 0x3F); c++; }          /* (é only) */
        w16[n] = (char)(v & 0xFF); w16[n + 1] = (char)(v >> 8);
        b16[n] = (char)(v >> 8); b16[n + 1] = (char)(v & 0xFF);
        n += 2;
    }
    CHECK(cue_parse(w16, n, &S) == 1 && !strcmp(S.title, "\xC3\xA9t\xC3\xA9"));
    CHECK(cue_parse(b16, n, &S) == 1 && !strcmp(S.title, "\xC3\xA9t\xC3\xA9"));

    /* what can't be played is dropped: no INDEX (or a bad one), before any FILE,
     * not later than the track before; only an INDEX 00: that one */
    CHECK(parse("TRACK 01 AUDIO\nINDEX 01 00:00:00\n"            /* no FILE yet */
                "FILE \"a.flac\" WAVE\n"
                "TRACK 02 AUDIO\nINDEX 01 00:10:00\n"
                "TRACK 03 AUDIO\nTITLE \"no index\"\n"
                "TRACK 04 AUDIO\nINDEX 01 00:61:00\n"              /* 61 seconds */
                "TRACK 05 AUDIO\nINDEX 01 00:05:00\n"              /* earlier than 02 */
                "TRACK 06 AUDIO\nINDEX 00 00:20:00\n"
                "TRACK 07 AUDIO\nINDEX 01 00:30:75\n"              /* 75 frames */
                "TRACK 08 AUDIO\nINDEX 01 00:40:00\n") == 3);
    CHECK(S.tracks[0].number == 2 && S.tracks[1].number == 6 && S.tracks[1].start == 20 * 75 && S.tracks[2].number == 8);

    /* several files; EAC's "gaps appended" layout puts the next FILE between a
     * track's INDEX 00 and 01: the track is in the file its INDEX 01 is in */
    CHECK(parse("FILE \"1.flac\" WAVE\nTRACK 01 AUDIO\nINDEX 01 00:00:00\nTRACK 02 AUDIO\nINDEX 00 04:00:00\n"
                "FILE \"2.flac\" WAVE\nINDEX 01 00:00:00\nTRACK 03 AUDIO\nINDEX 01 03:00:00\n") == 3);
    CHECK(S.nfiles == 2 && S.tracks[0].file == 0 && S.tracks[1].file == 1 && S.tracks[2].file == 1);
    CHECK(cue_file_tracks(&S, 0) == 1 && cue_file_tracks(&S, 1) == 2);
    CHECK(parse("") == 0 && parse("garbage\n\n\x01\x02") == 0);

    /* a track's path */
    char p[256], f[256];
    uint32_t a, b;
    cue_part_path(p, sizeof p, "/m/A::B/x.flac", 3, 225, 900);
    CHECK(!strcmp(p, "/m/A::B/x.flac::03@225-900"));
    CHECK(cue_part_parse(p, f, sizeof f, &a, &b) == 3 && !strcmp(f, "/m/A::B/x.flac") && a == 225 && b == 900);
    cue_part_path(p, sizeof p, "/m/x.flac", 12, 225, 0);
    CHECK(!strcmp(p, "/m/x.flac::12@225-"));
    CHECK(cue_part_parse(p, f, sizeof f, &a, &b) == 12 && a == 225 && b == 0);
    CHECK(cue_part_suffix("/m/x.flac") == NULL && cue_part_parse("/m/x.flac", f, sizeof f, &a, &b) == 0);
    CHECK(cue_part_suffix("/m/x.flac::3@") == NULL && cue_part_suffix("/m/x.flac::3@9-5") == NULL);
    CHECK(cue_part_suffix("::01@0-") == NULL && cue_part_suffix("/m/x.flac::01@0-1x") == NULL);
}

/* ---------- the generated library ---------- */

static int write_flac(const char *path, int frames, const char *title, const char *cuesheet)
{
    FLAC__StreamEncoder *e = FLAC__stream_encoder_new();
    FLAC__stream_encoder_set_channels(e, 2);
    FLAC__stream_encoder_set_bits_per_sample(e, 16);
    FLAC__stream_encoder_set_sample_rate(e, RATE);
    FLAC__StreamMetadata *vc = FLAC__metadata_object_new(FLAC__METADATA_TYPE_VORBIS_COMMENT);
    FLAC__StreamMetadata_VorbisComment_Entry en;
    FLAC__metadata_object_vorbiscomment_entry_from_name_value_pair(&en, "TITLE", title);
    FLAC__metadata_object_vorbiscomment_append_comment(vc, en, false);
    FLAC__metadata_object_vorbiscomment_entry_from_name_value_pair(&en, "ARTIST", "File Artist");
    FLAC__metadata_object_vorbiscomment_append_comment(vc, en, false);
    FLAC__metadata_object_vorbiscomment_entry_from_name_value_pair(&en, "ALBUM", "File Album");
    FLAC__metadata_object_vorbiscomment_append_comment(vc, en, false);
    FLAC__metadata_object_vorbiscomment_entry_from_name_value_pair(&en, "REPLAYGAIN_TRACK_GAIN", "-4.00 dB");
    FLAC__metadata_object_vorbiscomment_append_comment(vc, en, false);
    if (cuesheet) {
        FLAC__metadata_object_vorbiscomment_entry_from_name_value_pair(&en, "CUESHEET", cuesheet);
        FLAC__metadata_object_vorbiscomment_append_comment(vc, en, false);
    }
    FLAC__stream_encoder_set_metadata(e, &vc, 1);
    if (FLAC__stream_encoder_init_file(e, path, NULL, NULL) != FLAC__STREAM_ENCODER_INIT_STATUS_OK) return -1;
    FLAC__int32 *pcm = malloc((size_t)frames * 2 * sizeof *pcm);
    if (!pcm) return -1;
    for (int i = 0; i < frames; i++) {
        pcm[2 * i] = (int16_t)(i & 0xFFFF);
        pcm[2 * i + 1] = (int16_t)(i >> 16);
    }
    FLAC__stream_encoder_process_interleaved(e, pcm, (unsigned)frames);
    FLAC__stream_encoder_finish(e);
    FLAC__stream_encoder_delete(e);
    FLAC__metadata_object_delete(vc);
    free(pcm);
    return 0;
}

static void write_text(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    if (f) { fputs(text, f); fclose(f); }
}

/* Which frame of the file a decoded frame is. */
static uint32_t frame_of(const int32_t *fr)
{
    return ((uint32_t)fr[0] >> 16) | ((uint32_t)fr[1] >> 16) << 16;
}

/* Read d to its end: frames read, and how many weren't the file's frame
 * `first` onwards in order. */
static long read_all(decoder_t *d, uint32_t first, long *bad)
{
    static int32_t buf[4096 * 2];
    long n, total = 0;
    *bad = 0;
    while ((n = decoder_read(d, buf, 4096)) > 0)
        for (long i = 0; i < n; i++, total++) *bad += frame_of(buf + 2 * i) != first + (uint32_t)total;
    return total;
}

/* An output that checks every frame it's given follows the one before. */
typedef struct {
    uint32_t next;
    long bad;
    unsigned long long frames;
    int opens;
    volatile int gate;   /* writes wait until it opens: the test lines up the next track first */
} cap_t;

static int c_open(void *ctx, const audio_format_t *f) { (void)f; ((cap_t *)ctx)->opens++; return 0; }
static long c_write(void *ctx, const int32_t *fr, long n)
{
    cap_t *c = ctx;
    while (!c->gate) sleep_ms(1);
    for (long i = 0; i < n; i++) c->bad += frame_of(fr + 2 * i) != c->next++;
    c->frames += (unsigned long long)n;
    return n;
}
static long c_delay(void *ctx) { (void)ctx; return 0; }
static void c_nop(void *ctx) { (void)ctx; }
static void c_pause(void *ctx, int on) { (void)ctx; (void)on; }
static void c_describe(void *ctx, char *out, int cap) { (void)ctx; snprintf(out, (size_t)cap, "capture"); }

/* The track of an album list with this number: its row. */
static int find_track(library_t *lib, const lib_list_t *l, int number, lib_track_t *t)
{
    for (int i = 0; i < l->n; i++)
        if (library_track(lib, l->ids[i], t) == 0 && t->track == number) return 0;
    return -1;
}

static int64_t album_named(library_t *lib, const char *name)
{
    lib_list_t l;
    char n[256];
    int64_t id = 0;
    library_list(lib, LIB_ALBUMS, 0, &l);
    for (int i = 0; i < l.n && !id; i++)
        if (library_album_name(lib, l.ids[i], n, sizeof n) == 0 && !strcmp(n, name)) id = l.ids[i];
    library_list_free(&l);
    return id;
}

static int64_t db_int(const char *db, const char *sql)
{
    sqlite3 *h;
    sqlite3_stmt *s;
    int64_t v = -1;
    if (sqlite3_open(db, &h) != SQLITE_OK) return -1;
    if (sqlite3_prepare_v2(h, sql, -1, &s, NULL) == SQLITE_OK) {
        if (sqlite3_step(s) == SQLITE_ROW) v = sqlite3_column_int64(s, 0);
        sqlite3_finalize(s);
    }
    sqlite3_close(h);
    return v;
}

static void db_exec(const char *db, const char *sql)
{
    sqlite3 *h;
    if (sqlite3_open(db, &h) != SQLITE_OK) return;
    char *err = NULL;
    if (sqlite3_exec(h, sql, NULL, NULL, &err) != SQLITE_OK) { fprintf(stderr, "sql: %s\n", err); failures++; }
    sqlite3_free(err);
    sqlite3_close(h);
}

/* The sheet for Album.flac (written for the WAV: FILE names Album.wav), in
 * Latin-1 like many old ones. `t2` is track 2's title. */
static void write_sheet(const char *path, const char *t2)
{
    char text[1024];
    snprintf(text, sizeof text,
             "REM GENRE Rock\r\nREM DATE 1999\r\nPERFORMER \"Cue Band\"\r\nTITLE \"Caf\xE9 Sessions\"\r\n"
             "FILE \"Album.wav\" WAVE\r\n"
             "  TRACK 01 AUDIO\r\n    TITLE \"Opening\"\r\n    INDEX 01 00:00:00\r\n"
             "  TRACK 02 AUDIO\r\n    TITLE \"%s\"\r\n    PERFORMER \"Guest Singer\"\r\n"
             "    INDEX 00 00:02:50\r\n    INDEX 01 00:03:00\r\n"
             "  TRACK 03 AUDIO\r\n    TITLE \"Closing\"\r\n    INDEX 01 00:07:00\r\n",
             t2);
    write_text(path, text);
}

int main(void)
{
    test_parser();

    char dir[200];
    test_tmp(dir, sizeof dir, "fathom-test-cue");
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; }
    char music[300], path[700], sheet[700], emb[700], single[700], db[300], art[300];
    snprintf(music, sizeof music, "%s/music", dir);
    mkdir(music, 0755);
    snprintf(path, sizeof path, "%s/Cue Album", music);
    mkdir(path, 0755);
    snprintf(path, sizeof path, "%s/Embedded", music);
    mkdir(path, 0755);
    snprintf(path, sizeof path, "%s/Single", music);
    mkdir(path, 0755);
    /* a sheet for another rip: its tracks start past this file's end */
    char broken[700];
    snprintf(path, sizeof path, "%s/Broken", music);
    mkdir(path, 0755);
    snprintf(broken, sizeof broken, "%s/Broken/b.flac", music);
    write_flac(broken, RATE, "Broken Whole", NULL);
    snprintf(path, sizeof path, "%s/Broken/b.cue", music);
    write_text(path, "FILE \"b.flac\" WAVE\nTRACK 01 AUDIO\nINDEX 01 00:05:00\nTRACK 02 AUDIO\nINDEX 01 00:06:00\n");
    /* a playlist naming the whole file (made before the sheet, or elsewhere), and a track */
    snprintf(path, sizeof path, "%s/list.m3u8", music);
    write_text(path, "#EXTM3U\nCue Album/Album.flac\nSingle/one.flac\n");
    snprintf(path, sizeof path, "%s/Cue Album/Album.flac", music);
    snprintf(sheet, sizeof sheet, "%s/Cue Album/Album.cue", music);
    snprintf(emb, sizeof emb, "%s/Embedded/Embedded.flac", music);
    snprintf(single, sizeof single, "%s/Single/one.flac", music);
    if (write_flac(path, FRAMES, "Whole File", NULL) ||
        write_flac(emb, 4 * RATE, "Embedded Whole",
                   "TITLE \"Inside\"\nPERFORMER \"Tag Band\"\nFILE \"CDImage.wav\" WAVE\n"
                   "TRACK 01 AUDIO\nTITLE \"In One\"\nINDEX 01 00:00:00\n"
                   "TRACK 02 AUDIO\nTITLE \"In Two\"\nINDEX 01 00:01:00\n") ||
        write_flac(single, RATE, "One Alone", NULL)) {
        fprintf(stderr, "cannot write the test files\n");
        return 1;
    }
    write_sheet(sheet, "Middle");
    snprintf(db, sizeof db, "%s/lib.db", dir);
    snprintf(art, sizeof art, "%s/art", dir);

    library_t *lib = library_open(db, art);
    CHECK(lib && library_scan(lib, music, NULL, NULL) == 3 + 2 + 1 + 1);   /* the big files aren't tracks */
    CHECK(db_int(db, "SELECT COUNT(*) FROM tracks WHERE title IN ('Whole File', 'Embedded Whole')") == 0);

    /* the sheet's album: its tracks, in order, with the sheet's fields */
    int64_t alb = album_named(lib, "Caf\xC3\xA9 Sessions");
    CHECK(alb > 0);
    lib_list_t l = {0};
    CHECK(library_list(lib, LIB_ALBUM_TRACKS, alb, &l) == 3);
    lib_track_t t1, t2, t3;
    CHECK(find_track(lib, &l, 1, &t1) == 0 && find_track(lib, &l, 2, &t2) == 0 && find_track(lib, &l, 3, &t3) == 0);
    CHECK(l.n == 3 && l.ids[0] == t1.id && l.ids[1] == t2.id && l.ids[2] == t3.id);
    CHECK(!strcmp(t1.title, "Opening") && !strcmp(t2.title, "Middle") && !strcmp(t3.title, "Closing"));
    CHECK(!strcmp(t1.artist, "Cue Band") && !strcmp(t2.artist, "Guest Singer"));
    CHECK(t1.year == 1999 && !strcmp(t1.codec, "FLAC") && t1.sample_rate == RATE && t1.bits == 16);
    CHECK(t1.duration_ms == 3000 && t2.duration_ms == 4000 && t3.duration_ms == 3000);   /* to the next INDEX 01, or the end */
    CHECK(library_album_artist(lib, alb) > 0);
    char name[256];
    CHECK(library_artist_name(lib, library_album_artist(lib, alb), name, sizeof name) == 0 && !strcmp(name, "Cue Band"));
    CHECK(db_int(db, "SELECT COUNT(*) FROM tracks WHERE genre='Rock' AND cue>0") == 3);
    /* the file's own gain is the album's; each track's is measured later */
    CHECK(db_int(db, "SELECT COUNT(*) FROM tracks WHERE cue>0 AND rg_state=0 AND rg_album=-4.0 AND path LIKE '%Album.flac::%'") == 3);
    library_list_free(&l);
    CHECK(db_int(db, "SELECT COUNT(*) FROM tracks WHERE title='Broken Whole' AND cue=0") == 1);   /* stays one */

    /* the playlist: the file's tracks, then the other */
    CHECK(library_list(lib, LIB_PLAYLISTS, 0, &l) == 1);
    lib_list_t pl;
    CHECK(library_list(lib, LIB_PLAYLIST_TRACKS, l.ids[0], &pl) == 4);
    CHECK(pl.n == 4 && pl.ids[0] == t1.id && pl.ids[1] == t2.id && pl.ids[2] == t3.id);
    library_list_free(&pl);
    library_list_free(&l);

    /* the sheet in the tags */
    int64_t ealb = album_named(lib, "Inside");
    CHECK(ealb > 0 && library_list(lib, LIB_ALBUM_TRACKS, ealb, &l) == 2);
    lib_track_t e1, e2;
    CHECK(find_track(lib, &l, 1, &e1) == 0 && find_track(lib, &l, 2, &e2) == 0);
    CHECK(!strcmp(e1.title, "In One") && !strcmp(e2.artist, "Tag Band") && e1.duration_ms == 1000 && e2.duration_ms == 3000);
    library_list_free(&l);

    /* playing a part: from its INDEX 01 to the next, its own length; seeking in it */
    audio_format_t f;
    long bad;
    decoder_t *d = decoder_open(t2.path, &f);
    CHECK(d && f.total_frames == (uint64_t)4 * RATE && f.rate == RATE);
    if (d) {
        CHECK(read_all(d, 3 * RATE, &bad) == 4 * RATE && bad == 0);
        CHECK(decoder_seek(d, 1000) == 0 && read_all(d, 3 * RATE + 1000, &bad) == 4 * RATE - 1000 && bad == 0);
        decoder_close(d);
    }
    d = decoder_open(t3.path, &f);
    CHECK(d && f.total_frames == (uint64_t)3 * RATE);
    if (d) { CHECK(read_all(d, 7 * RATE, &bad) == 3 * RATE && bad == 0); decoder_close(d); }

    /* the next part carries on in the same decoder; a part elsewhere doesn't */
    d = decoder_open(t1.path, &f);
    if (d) {
        CHECK(read_all(d, 0, &bad) == 3 * RATE && bad == 0);
        CHECK(decoder_continue(d, t3.path, &f) != 0);
        CHECK(decoder_continue(d, single, &f) != 0);
        CHECK(decoder_continue(d, t2.path, &f) == 0 && f.total_frames == (uint64_t)4 * RATE);
        CHECK(read_all(d, 3 * RATE, &bad) == 4 * RATE && bad == 0);
        decoder_close(d);
    }

    /* gapless through the engine: track 1 then 2, one output opening, every
     * frame in order, the position the track's own */
    cap_t cap = {0};
    audio_output_t out = {c_open, c_write, c_delay, c_nop, c_nop, c_pause, c_nop, c_describe, &cap, NULL, NULL};
    engine_t *eng = engine_new(&out);
    engine_play(eng, t1.path, t1.id);
    engine_queue_next(eng, t2.path, t2.id);
    eng_status_t st;
    engine_status(eng, &st);
    cap.gate = 1;
    for (int w = 0; w < 500; w++) {
        engine_status(eng, &st);
        if (st.ended) break;
        sleep_ms(10);
    }
    CHECK(st.ended && st.track_id == t2.id && st.len_ms == 4000 && st.pos_ms == 4000);
    CHECK(cap.frames == (unsigned long long)7 * RATE && cap.bad == 0 && cap.opens == 1);
    engine_free(eng);

    /* a scan with nothing changed: nothing written, the same ids */
    int64_t ids[3] = {t1.id, t2.id, t3.id};
    CHECK(library_scan(lib, music, NULL, NULL) == 7 && library_scan_changes(lib) == 0);
    CHECK(library_track(lib, t2.id, &t2) == 0 && !strcmp(t2.title, "Middle"));
    CHECK(library_track(lib, e2.id, &e2) == 0 && !strcmp(e2.title, "In Two"));

    /* a measured gain, a play count and a favourite; then the sheet's title
     * edited: the same ids, the new title, the gain and counts kept */
    lib_rg_t rg = {2, -3.0f, 0.5f, 0, 0, 0};
    library_rg_store(lib, ids[1], &rg);
    lib_list_t sl;
    int64_t one = db_int(db, "SELECT id FROM tracks WHERE title='One Alone'");
    library_played(lib, one, 0, 1000);
    library_played(lib, ids[0], 0, 1000);
    library_set_favourite(lib, ids[2], 1);
    write_sheet(sheet, "Middle Eight");
    CHECK(library_scan(lib, music, NULL, NULL) == 7 && library_scan_changes(lib) == 3);
    for (int i = 0; i < 3; i++) CHECK(library_track(lib, ids[i], &t1) == 0);
    CHECK(library_track(lib, ids[1], &t2) == 0 && !strcmp(t2.title, "Middle Eight"));
    CHECK(db_int(db, "SELECT rg_state FROM tracks WHERE title='Middle Eight'") == 2);
    CHECK(library_rg_gain(lib, ids[1], 0) > 0.70f && library_rg_gain(lib, ids[1], 0) < 0.71f);   /* -3 dB */
    CHECK(db_int(db, "SELECT play_count FROM tracks WHERE title='One Alone'") == 1);
    CHECK(db_int(db, "SELECT play_count FROM tracks WHERE title='Opening'") == 1);
    CHECK(library_favourite(lib, ids[2]) == 1);
    CHECK(db_int(db, "SELECT id FROM tracks WHERE title='One Alone'") == one);

    /* levelling measures the parts: their paths are what it gets */
    int64_t rid;
    char rpath[1024];
    CHECK(library_rg_next(lib, &rid, rpath, sizeof rpath) == 0 && cue_part_suffix(rpath) != NULL);

    /* a library from before sheets in tags: the file was one track, read at
     * tags_v 1; the next scan finds the sheet in it, once */
    struct stat es;
    stat(emb, &es);
    char sql[2000];
    snprintf(sql, sizeof sql,
             "DELETE FROM tracks WHERE cue>0 AND path LIKE '%%Embedded.flac::%%';"
             "DELETE FROM cues WHERE path='%s';"
             "INSERT INTO tracks(path, mtime, size, title, tags_v) VALUES('%s', %lld, %lld, 'Embedded Whole', 1);",
             emb, emb, (long long)es.st_mtime, (long long)es.st_size);
    library_close(lib);
    db_exec(db, sql);
    lib = library_open(db, art);
    CHECK(lib && library_needs_tags(lib));
    CHECK(library_scan(lib, music, NULL, NULL) == 7 && !library_needs_tags(lib));
    CHECK(db_int(db, "SELECT COUNT(*) FROM tracks WHERE title IN ('In One', 'In Two')") == 2);
    CHECK(db_int(db, "SELECT COUNT(*) FROM tracks WHERE title='Embedded Whole'") == 0);
    CHECK(library_scan(lib, music, NULL, NULL) == 7 && library_scan_changes(lib) == 0);

    /* the sheet gone: the file is one track again */
    unlink(sheet);
    CHECK(library_scan(lib, music, NULL, NULL) == 1 + 2 + 1 + 1);
    CHECK(db_int(db, "SELECT duration_ms FROM tracks WHERE title='Whole File' AND cue=0") == 10000);
    CHECK(db_int(db, "SELECT COUNT(*) FROM cues") == 2);   /* Embedded.flac and the broken sheet */
    CHECK(library_list(lib, LIB_PLAYLISTS, 0, &l) == 1 && library_list(lib, LIB_PLAYLIST_TRACKS, l.ids[0], &sl) == 2);
    library_list_free(&sl);
    library_list_free(&l);
    library_close(lib);

    char cmd[400];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    if (system(cmd)) {}
    if (failures) fprintf(stderr, "%d failure(s)\n", failures);
    else printf("cue: all passed\n");
    return failures ? 1 : 0;
}
