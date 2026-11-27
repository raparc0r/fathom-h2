/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* A song's lyrics: the text's lines and their times (library/lyrics.c), the
 * lyrics in a file's tags (tags_lyrics), where a song's lyrics are looked for
 * and kept, and the lookup online with its answers saved here (net/lyricfind.c).
 * The words are made up for the test. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "library/lyrics.h"
#include "library/tags.h"
#include "net/lyricfind.h"
#include "test_flac.h"
#include "test_util.h"

static int parse(const char *s, lyrics_t *l) { return lyrics_parse(s, strlen(s), l); }

static int write_bytes(const char *path, const void *b, size_t n)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    int ok = fwrite(b, 1, n, f) == n;
    return fclose(f) == 0 && ok ? 0 : -1;
}

static void test_plain(void)
{
    lyrics_t l;
    CHECK(parse("\n\nA line of the song\n\n\n\nAnother one  \r\n[Chorus: both]\n\tand a third\n\n", &l) == 0);
    CHECK(l.n == 5 && !l.synced);
    if (l.n == 5) {
        CHECK_STR(lyrics_line(&l, 0), "A line of the song");
        CHECK_STR(lyrics_line(&l, 1), "");                      /* (four empty lines: one) */
        CHECK_STR(lyrics_line(&l, 2), "Another one");
        CHECK_STR(lyrics_line(&l, 3), "[Chorus: both]");        /* (words in brackets are words) */
        CHECK_STR(lyrics_line(&l, 4), "and a third");
        CHECK(l.line[0].ms == -1);
    }
    CHECK(lyrics_at(&l, 5000) == -1);
    lyrics_free(&l);
    CHECK(l.n == 0 && !l.text);
    /* nothing to show */
    CHECK(parse("", &l) == -1);
    CHECK(parse("\n \n\t\n", &l) == -1);
    CHECK(parse("[ar:Somebody]\n[ti:Something]\n[length:03:10]\n", &l) == -1);
    CHECK(lyrics_parse(NULL, 0, &l) == -1);
    /* longer than any song's */
    char *big = malloc(LYRICS_MAX_BYTES + 2);
    if (big) {
        memset(big, 'a', LYRICS_MAX_BYTES + 1);
        big[LYRICS_MAX_BYTES + 1] = '\0';
        CHECK(lyrics_parse(big, LYRICS_MAX_BYTES + 1, &l) == -1);
        big[LYRICS_MAX_BYTES] = '\0';
        CHECK(lyrics_parse(big, LYRICS_MAX_BYTES, &l) == 0 && l.n == 1);
        lyrics_free(&l);
        /* more lines than are kept: the first LYRICS_MAX_LINES */
        size_t w = 0;
        for (int i = 0; i < LYRICS_MAX_LINES + 50; i++) w += (size_t)sprintf(big + w, "line %d\n", i);
        CHECK(lyrics_parse(big, w, &l) == 0 && l.n == LYRICS_MAX_LINES);
        lyrics_free(&l);
        free(big);
    }
}

static void test_timed(void)
{
    lyrics_t l;
    CHECK(parse("[ti:Made up]\n[ar:Nobody]\n[by:the test]\n"
                "[00:01.00]First line\n"
                "[00:05.50] Second line \n"
                "[00:03.00][00:09.00]The chorus\n"
                "[00:07]\n"
                "[00:11.00]\n", &l) == 0);
    CHECK(l.n == 5 && l.synced);
    if (l.n == 5) {
        CHECK(l.line[0].ms == 1000 && l.line[1].ms == 3000 && l.line[2].ms == 5500 && l.line[3].ms == 7000 && l.line[4].ms == 9000);
        CHECK_STR(lyrics_line(&l, 0), "First line");
        CHECK_STR(lyrics_line(&l, 1), "The chorus");
        CHECK_STR(lyrics_line(&l, 2), "Second line");
        CHECK_STR(lyrics_line(&l, 3), "");                      /* (a pause) */
        CHECK_STR(lyrics_line(&l, 4), "The chorus");
        CHECK(l.line[1].at == l.line[4].at);                    /* (written once) */
    }
    CHECK(lyrics_at(&l, 0) == -1 && lyrics_at(&l, 999) == -1);
    CHECK(lyrics_at(&l, 1000) == 0 && lyrics_at(&l, 2999) == 0 && lyrics_at(&l, 3000) == 1);
    CHECK(lyrics_at(&l, 7500) == 3 && lyrics_at(&l, 9000) == 4 && lyrics_at(&l, 3600000) == 4);
    lyrics_free(&l);

    /* the ways a time is written */
    CHECK(parse("[1:02]a\n[01:03.3]b\n[01:04.345]c\n[01:05:34]d\n[61:00.00]e\n[00:59.999999]f\n", &l) == 0);
    CHECK(l.n == 6 && l.synced);
    if (l.n == 6) {
        CHECK_STR(lyrics_line(&l, 0), "f");
        CHECK(l.line[0].ms == 59999 && l.line[1].ms == 62000 && l.line[2].ms == 63300 && l.line[3].ms == 64345);
        CHECK(l.line[4].ms == 65340 && l.line[5].ms == 3660000);
    }
    lyrics_free(&l);
    /* not times: words */
    CHECK(parse("[12:99.00]a\n[ab:cd]b\n[1:2]c\n[00:10.]d\n", &l) == 0);
    CHECK(l.n == 4 && !l.synced);
    if (l.n == 4) CHECK_STR(lyrics_line(&l, 0), "[12:99.00]a");
    lyrics_free(&l);

    /* the file's offset: over 0, the words come sooner; never before the start */
    CHECK(parse("[offset:+500]\n[00:00.20]a\n[00:02.00]b\n", &l) == 0);
    CHECK(l.n == 2 && l.line[0].ms == 0 && l.line[1].ms == 1500);
    lyrics_free(&l);
    CHECK(parse("[offset: -250]\n[00:02.00]b\n", &l) == 0);
    CHECK(l.n == 1 && l.line[0].ms == 2250);
    lyrics_free(&l);

    /* times word by word are dropped; a line with no time is sung with the one before */
    CHECK(parse("[00:01.00]<00:01.00>Word <00:01.40>by <00:01.80>word\nand its second half\n\n[00:04.00]Next\n", &l) == 0);
    CHECK(l.n == 3 && l.synced);
    if (l.n == 3) {
        CHECK_STR(lyrics_line(&l, 0), "Word by word");
        CHECK_STR(lyrics_line(&l, 1), "and its second half");
        CHECK(l.line[1].ms == 1000 && l.line[2].ms == 4000);
    }
    lyrics_free(&l);

    CHECK(lyrics_looks_synced("[00:01.00]a", 11) == 1);
    CHECK(lyrics_looks_synced("words\n[00:01.00]a", 17) == 1);
    CHECK(lyrics_looks_synced("words [00:01.00] a", 18) == 0);
    CHECK(lyrics_looks_synced("[Chorus]\nla la", 14) == 0);
}

static void test_encodings(void)
{
    lyrics_t l;
    /* UTF-8 with its mark */
    CHECK(parse("\xef\xbb\xbf[00:01.00]caf\xc3\xa9\n", &l) == 0);
    CHECK(l.n == 1 && l.synced);
    if (l.n == 1) CHECK_STR(lyrics_line(&l, 0), "caf\xc3\xa9");
    lyrics_free(&l);
    /* Latin-1 */
    CHECK(parse("caf\xe9 ol\xe9\n", &l) == 0);
    if (l.n == 1) CHECK_STR(lyrics_line(&l, 0), "caf\xc3\xa9 ol\xc3\xa9");
    lyrics_free(&l);
    /* UTF-16, both ways round, with a character outside the first plane */
    static const unsigned char le[] = {0xff, 0xfe, '[', 0, '0', 0, '0', 0, ':', 0, '0', 0, '2', 0, ']', 0, 'a', 0, 0xe9, 0, 0x3c, 0xd8, 0xb5, 0xdf, '\n', 0, 'b', 0};
    static const unsigned char be[] = {0xfe, 0xff, 0, 'x', 0x30, 0x42, 0, '\r', 0, '\n', 0, 'y'};
    CHECK(lyrics_parse((const char *)le, sizeof le, &l) == 0);
    CHECK(l.n == 2 && l.synced && l.line[0].ms == 2000);
    if (l.n == 2) {
        CHECK_STR(lyrics_line(&l, 0), "a\xc3\xa9\xf0\x9f\x8e\xb5");
        CHECK_STR(lyrics_line(&l, 1), "b");
    }
    lyrics_free(&l);
    CHECK(lyrics_parse((const char *)be, sizeof be, &l) == 0);
    CHECK(l.n == 2 && !l.synced);
    if (l.n == 2) CHECK_STR(lyrics_line(&l, 0), "x\xe3\x81\x82");
    lyrics_free(&l);
}

/* ---------- tags ---------- */

static int flac(const char *path, const char *k1, const char *v1, const char *k2, const char *v2)
{
    return t_write_flac(path, &(t_flac_t){.frames = 441, .tags = {{"TITLE", "A song"}, {k1, v1}, {"ARTIST", "Somebody"}, {k2, v2}}});
}

static size_t syncsafe(unsigned char *p, size_t n)
{
    p[0] = (unsigned char)(n >> 21 & 0x7f), p[1] = (unsigned char)(n >> 14 & 0x7f), p[2] = (unsigned char)(n >> 7 & 0x7f), p[3] = (unsigned char)(n & 0x7f);
    return 4;
}

/* An MP3's tag alone (no sound after it: tags_lyrics reads the tag), version
 * 2, 3 or 4, with a title and a lyrics frame whose body is given */
static int mp3(const char *path, int version, const unsigned char *body, size_t bn)
{
    unsigned char t[4096], *w = t + 10;
    static const unsigned char title[] = {0, 'A', ' ', 's', 'o', 'n', 'g'};
    for (int k = 0; k < 2; k++) {
        const unsigned char *b = k ? body : title;
        size_t n = k ? bn : sizeof title;
        if (version == 2) {
            memcpy(w, k ? "ULT" : "TT2", 3);
            w[3] = (unsigned char)(n >> 16), w[4] = (unsigned char)(n >> 8), w[5] = (unsigned char)n;
            w += 6;
        } else {
            memcpy(w, k ? "USLT" : "TIT2", 4);
            if (version == 4) syncsafe(w + 4, n);
            else w[4] = (unsigned char)(n >> 24), w[5] = (unsigned char)(n >> 16), w[6] = (unsigned char)(n >> 8), w[7] = (unsigned char)n;
            w[8] = w[9] = 0;
            w += 10;
        }
        memcpy(w, b, n);
        w += n;
    }
    memcpy(t, "ID3", 3);
    t[3] = (unsigned char)version, t[4] = 0, t[5] = 0;
    syncsafe(t + 6, (size_t)(w - t) - 10);
    return write_bytes(path, t, (size_t)(w - t));
}

static void test_tags(const char *dir)
{
    char path[700], *text;
    size_t len;
    snprintf(path, sizeof path, "%s/plain.flac", dir);
    CHECK(flac(path, "LYRICS", "The first line\nThe second line\n", NULL, NULL) == 0);
    CHECK(tags_lyrics(path, &text, &len) == 0 && text && len == strlen("The first line\nThe second line"));
    if (text) CHECK_STR(text, "The first line\nThe second line");
    free(text);
    /* of two, the one with times, whichever comes first */
    CHECK(flac(path, "UNSYNCEDLYRICS", "plain words", "lyrics", "[00:01.00]timed words") == 0);
    CHECK(tags_lyrics(path, &text, &len) == 0);
    if (text) CHECK_STR(text, "[00:01.00]timed words");
    free(text);
    CHECK(flac(path, "SYNCEDLYRICS", "[00:01.00]timed words", "UNSYNCEDLYRICS", "plain words") == 0);
    CHECK(tags_lyrics(path, &text, &len) == 0);
    if (text) CHECK_STR(text, "[00:01.00]timed words");
    free(text);
    /* none, or an empty one; a tag whose name only starts the same */
    CHECK(flac(path, "LYRICIST", "Somebody Else", "LYRICS", "") == 0);
    CHECK(tags_lyrics(path, &text, &len) == -1 && !text);
    /* a track of an album in one file: not its file's */
    char part[800];
    CHECK(flac(path, "LYRICS", "The whole album's words", NULL, NULL) == 0);
    snprintf(part, sizeof part, "%s::02@1000-2000", path);
    CHECK(tags_lyrics(part, &text, &len) == -1 && !text);
    CHECK(tags_lyrics("/nowhere/at-all.flac", &text, &len) == -1);
    remove(path);

    /* an MP3's: Latin-1 (v2.3), UTF-8 (v2.4), UTF-16 with a description (v2.3), v2.2's */
    snprintf(path, sizeof path, "%s/song.mp3", dir);
    static const unsigned char latin[] = {0, 'e', 'n', 'g', 0, 'c', 'a', 'f', 0xe9, '\n', 't', 'w', 'o'};
    CHECK(mp3(path, 3, latin, sizeof latin) == 0);
    CHECK(tags_lyrics(path, &text, &len) == 0);
    if (text) CHECK_STR(text, "caf\xc3\xa9\ntwo");
    free(text);
    static const unsigned char utf8[] = {3, 'e', 'n', 'g', 'd', 'e', 's', 'c', 0, '[', '0', '0', ':', '0', '2', '.', '0', '0', ']', 0xc3, 0xa9};
    CHECK(mp3(path, 4, utf8, sizeof utf8) == 0);
    CHECK(tags_lyrics(path, &text, &len) == 0);
    if (text) CHECK_STR(text, "[00:02.00]\xc3\xa9");
    free(text);
    static const unsigned char utf16[] = {1, 'e', 'n', 'g', 0xff, 0xfe, 'd', 0, 0, 0, 0xff, 0xfe, 'h', 0, 'i', 0, '\n', 0, 0x42, 0x30};
    CHECK(mp3(path, 3, utf16, sizeof utf16) == 0);
    CHECK(tags_lyrics(path, &text, &len) == 0);
    if (text) CHECK_STR(text, "hi\n\xe3\x81\x82");
    free(text);
    CHECK(mp3(path, 2, latin, sizeof latin) == 0);
    CHECK(tags_lyrics(path, &text, &len) == 0);
    if (text) CHECK_STR(text, "caf\xc3\xa9\ntwo");
    free(text);
    /* a frame with nothing after its description, and one cut short */
    static const unsigned char empty[] = {0, 'e', 'n', 'g', 'x', 0};
    CHECK(mp3(path, 3, empty, sizeof empty) == 0);
    CHECK(tags_lyrics(path, &text, &len) == -1);
    CHECK(mp3(path, 3, latin, 3) == 0);
    CHECK(tags_lyrics(path, &text, &len) == -1);
    remove(path);
}

/* ---------- where they are looked for, and kept ---------- */

static void test_local(const char *dir)
{
    char song[700], lrc[700], store[700], p1[1100], p2[1100];
    snprintf(song, sizeof song, "%s/01 A song.flac", dir);
    snprintf(lrc, sizeof lrc, "%s/01 A song.lrc", dir);
    snprintf(store, sizeof store, "%s/lyrics", dir);
    lyrics_song_t s = {song, "Somebody", "A song", "An album"};
    lyrics_t l;
    int from = 0;
    CHECK(flac(song, NULL, NULL, NULL, NULL) == 0);
    CHECK(lyrics_local(store, &s, &l, &from) == -1 && l.n == 0);
    CHECK(lyrics_local(NULL, &s, &l, &from) == -1);
    CHECK(lyrics_store_tried(store, &s, 7) == 0);

    /* found before: kept under the song's names, whatever their case */
    CHECK(lyrics_store_save(store, &s, "[00:01.00]From the store", 24) == 0);
    lyrics_song_t same = {"/elsewhere/other name.mp3", "SOMEBODY", "a song", "An Album"}, other = {song, "Somebody", "A song", "Another album"};
    lyrics_store_path(store, &s, "lrc", p1, sizeof p1);
    lyrics_store_path(store, &same, "lrc", p2, sizeof p2);
    CHECK_STR(p1, p2);
    lyrics_store_path(store, &other, "lrc", p2, sizeof p2);
    CHECK(strcmp(p1, p2) != 0);
    CHECK(lyrics_local(store, &s, &l, &from) == 0 && from == LYRICS_FROM_STORE && l.synced);
    lyrics_free(&l);
    CHECK(lyrics_local(store, &other, &l, &from) == -1);
    /* a track of an album in one file has the store alone */
    char part[800];
    snprintf(part, sizeof part, "%s::02@1000-2000", song);
    lyrics_song_t cue = {part, "Somebody", "A song", "An album"};
    CHECK(lyrics_local(store, &cue, &l, &from) == 0 && from == LYRICS_FROM_STORE);
    lyrics_free(&l);

    /* its tags come before that, and a .lrc beside it before its tags */
    CHECK(flac(song, "LYRICS", "From the tags", NULL, NULL) == 0);
    CHECK(lyrics_local(store, &s, &l, &from) == 0 && from == LYRICS_FROM_TAG && !l.synced);
    if (l.n) CHECK_STR(lyrics_line(&l, 0), "From the tags");
    lyrics_free(&l);
    CHECK(write_bytes(lrc, "[00:02.00]From the file beside it\n", 34) == 0);
    CHECK(lyrics_local(store, &s, &l, &from) == 0 && from == LYRICS_FROM_FILE && l.synced);
    if (l.n) CHECK_STR(lyrics_line(&l, 0), "From the file beside it");
    lyrics_free(&l);
    CHECK(lyrics_local(store, &cue, &l, &from) == 0 && from == LYRICS_FROM_STORE);   /* (not the album's .lrc) */
    lyrics_free(&l);
    /* a .lrc with nothing in it is no lyrics: the next place */
    CHECK(write_bytes(lrc, "[ar:Somebody]\n", 14) == 0);
    CHECK(lyrics_local(store, &s, &l, &from) == 0 && from == LYRICS_FROM_TAG);
    lyrics_free(&l);
    remove(lrc);

    /* looked for and not found: said for a week, gone once some are saved */
    CHECK(lyrics_store_none(store, &other) == 0);
    CHECK(lyrics_store_tried(store, &other, 7) == 1 && lyrics_store_tried(store, &other, 0) == 0);
    CHECK(lyrics_store_save(store, &other, "words", 5) == 0);
    CHECK(lyrics_store_tried(store, &other, 7) == 0);
    CHECK(lyrics_store_save(store, &s, "", 0) == -1);
    CHECK(lyrics_store_save("", &s, "words", 5) == -1);
    lyrics_store_path(store, &other, "lrc", p2, sizeof p2);
    remove(p1);
    remove(p2);
    remove(song);
    rmdir(store);
}

/* ---------- online ---------- */

static const char ANSWER[] =
    "{\"id\":1,\"name\":\"A song\",\"trackName\":\"A song\",\"artistName\":\"Somebody\",\"albumName\":\"An album\","
    "\"duration\":181.0,\"instrumental\":false,\"plainLyrics\":\"Plain first\\nPlain second\\n\","
    "\"syncedLyrics\":\"[00:01.00] Timed first\\n[00:04.00] Timed \\\"second\\\"\\n\"}";
static const char NO_WORDS[] =
    "{\"id\":2,\"trackName\":\"A song\",\"artistName\":\"Somebody\",\"duration\":180,\"instrumental\":true,"
    "\"plainLyrics\":null,\"syncedLyrics\":null}";
static const char MISSING[] = "{\"message\":\"Failed to find specified track\",\"name\":\"TrackNotFound\",\"statusCode\":404}";

static void test_answers(void)
{
    lf_song_t s = {.artist = "Somebody & Co", .title = "A song (live)?", .album = "An \xc3\xa9lbum", .duration_s = 180};
    char url[600], *text;
    size_t len;
    lf_url(&s, 1, url, sizeof url);
    CHECK_STR(url, "https://lrclib.net/api/get?artist_name=Somebody+%26+Co&track_name=A+song+%28live%29%3F&album_name=An+%C3%A9lbum&duration=180");
    lf_url(&s, 0, url, sizeof url);
    CHECK_STR(url, "https://lrclib.net/api/get?artist_name=Somebody+%26+Co&track_name=A+song+%28live%29%3F&duration=180");
    s.duration_s = 0;
    s.album[0] = '\0';
    lf_url(&s, 1, url, sizeof url);
    CHECK_STR(url, "https://lrclib.net/api/get?artist_name=Somebody+%26+Co&track_name=A+song+%28live%29%3F");
    char small[40];
    lf_url(&s, 1, small, sizeof small);            /* (no room: cut, ended) */
    CHECK(strlen(small) < sizeof small);

    /* a recording of the same length: its timed lyrics; another: its words alone */
    CHECK(lf_parse(ANSWER, sizeof ANSWER - 1, 180, &text, &len) == 1);
    if (text) CHECK_STR(text, "[00:01.00] Timed first\n[00:04.00] Timed \"second\"");
    CHECK(text && len == strlen(text));
    free(text);
    CHECK(lf_parse(ANSWER, sizeof ANSWER - 1, 184, &text, &len) == 1);
    if (text) CHECK(!strncmp(text, "[00:01.00]", 10));
    free(text);
    CHECK(lf_parse(ANSWER, sizeof ANSWER - 1, 190, &text, &len) == 1);
    if (text) CHECK_STR(text, "Plain first\nPlain second");
    free(text);
    CHECK(lf_parse(ANSWER, sizeof ANSWER - 1, 0, &text, &len) == 1);   /* (our length not known) */
    if (text) CHECK_STR(text, "Plain first\nPlain second");
    free(text);
    CHECK(lf_parse(NO_WORDS, sizeof NO_WORDS - 1, 180, &text, &len) == 2 && !text);
    CHECK(lf_parse(MISSING, sizeof MISSING - 1, 180, &text, &len) == -1 && !text);
    CHECK(lf_parse("<html>Sign in to the hotel's Wi-Fi</html>", 41, 180, &text, &len) == -1);
    static const char BLANK[] = "{\"trackName\":\"A song\",\"duration\":180,\"plainLyrics\":\"  \\n\"}";
    CHECK(lf_parse(BLANK, sizeof BLANK - 1, 180, &text, &len) == 0 && !text);
    /* a zero byte written into the words: they end there (found by the fuzzer: the length said more) */
    static const char ZERO[] = "{\"trackName\":\"A song\",\"duration\":180,\"plainLyrics\":\"One\\u0000Two\"}";
    CHECK(lf_parse(ZERO, sizeof ZERO - 1, 180, &text, &len) == 1 && text && len == 3 && !strcmp(text, "One"));
    free(text);
    CHECK(lf_parse(ANSWER, 60, 180, &text, &len) == -1 || !text);      /* (cut short) */
    free(text);
}

static int g_asked, g_with_album, g_without, g_hold;
static const char *g_first, *g_second;       /* the answers with the album, and without (NULL: "not there") */
static long g_fail;                          /* what the fetch returns instead, if not 0 */

static long fake_fetch(const char *url, char *buf, size_t cap, const volatile int *abort)
{
    g_asked++;
    int with = strstr(url, "&album_name=") != NULL;
    if (with) g_with_album++;
    else g_without++;
    while (g_hold && !*abort) t_sleep_ms(5);     /* (a server that doesn't answer) */
    if (*abort) return -1;
    if (g_fail) return g_fail;
    const char *a = with ? g_first : g_second;
    if (!a) return -3;
    snprintf(buf, cap, "%s", a);
    return (long)strlen(buf);
}

static int settled(int64_t *track, int *online, int *tried)
{
    int st = LF_LOOKING;
    for (int i = 0; i < 1000 && (st = lf_state(track, online, tried)) == LF_LOOKING; i++) t_sleep_ms(5);
    return st;
}

static void test_worker(const char *dir)
{
    char store[700], path[1100], none[1100];
    snprintf(store, sizeof store, "%s/found", dir);
    lf_song_t s = {.track = 7, .artist = "Somebody", .title = "A song", .album = "An album", .duration_s = 180};
    snprintf(s.path, sizeof s.path, "%s/no such file.flac", dir);
    lyrics_song_t ls = {s.path, s.artist, s.title, s.album};
    lyrics_store_path(store, &ls, "lrc", path, sizeof path);
    lyrics_store_path(store, &ls, "none", none, sizeof none);
    lyrics_t l;
    int64_t track = 0;
    int online = -1, tried = -1, from = 0;

    CHECK(lf_state(&track, NULL, NULL) == LF_IDLE);
    CHECK(lf_ask(&s, 0) == -1);                    /* (not started) */
    lf_set_fetch(fake_fetch);
    lf_start(store);

    /* what the song has by itself: nothing, and nobody is asked */
    CHECK(lf_ask(&s, 0) == 0);
    CHECK(settled(&track, &online, &tried) == LF_NONE && track == 7 && online == 0 && tried == 0);
    CHECK(g_asked == 0 && lf_take(&l, &from) == -1);

    /* online: not under its album, there without; kept, and shown */
    g_first = NULL;
    g_second = ANSWER;
    CHECK(lf_ask(&s, 1) == 0);
    CHECK(settled(&track, &online, NULL) == LF_FOUND && track == 7 && online == 1);
    CHECK(g_with_album == 1 && g_without == 1);
    CHECK(lf_take(&l, &from) == 0 && from == LYRICS_FROM_STORE && l.synced && l.n == 2);
    if (l.n == 2) CHECK_STR(lyrics_line(&l, 1), "Timed \"second\"");
    lyrics_free(&l);
    CHECK(lf_take(&l, &from) == -1);               /* (handed over once) */
    CHECK(t_file_size(path) == (long)strlen("[00:01.00] Timed first\n[00:04.00] Timed \"second\""));
    /* asked again: from the card, nobody asked */
    g_asked = 0;
    CHECK(lf_ask(&s, 1) == 0);
    CHECK(settled(NULL, NULL, NULL) == LF_FOUND && g_asked == 0);
    CHECK(lf_take(&l, &from) == 0 && from == LYRICS_FROM_STORE);
    lyrics_free(&l);
    remove(path);

    /* not there at all: said, and noted for next time */
    g_first = g_second = NULL;
    g_asked = 0;
    CHECK(lf_ask(&s, 1) == 0);
    CHECK(settled(NULL, NULL, &tried) == LF_NOT_FOUND && tried == 1 && g_asked == 2);
    CHECK(t_file_size(none) == 1);
    CHECK(lf_ask(&s, 0) == 0);
    CHECK(settled(NULL, &online, &tried) == LF_NONE && tried == 1 && online == 0);
    /* a song without words */
    g_first = NO_WORDS;
    CHECK(lf_ask(&s, 1) == 0);
    CHECK(settled(NULL, NULL, NULL) == LF_INSTRUMENTAL);
    /* an answer that isn't one (a sign-in page): not found, nothing kept */
    g_first = g_second = "<html>Sign in</html>";
    CHECK(lf_ask(&s, 1) == 0);
    CHECK(settled(NULL, NULL, NULL) == LF_NOT_FOUND && t_file_size(path) < 0);
    remove(none);

    /* no network: said so, and not noted as "not there" */
    g_fail = -1;
    CHECK(lf_ask(&s, 1) == 0);
    CHECK(settled(NULL, NULL, &tried) == LF_FAILED && tried == 0 && t_file_size(none) < 0);
    g_fail = 0;

    /* nothing to ask with: no artist */
    lf_song_t anon = s;
    anon.artist[0] = '\0';
    g_asked = 0;
    CHECK(lf_ask(&anon, 1) == 0);
    CHECK(settled(NULL, NULL, NULL) == LF_NOT_FOUND && g_asked == 0);

    /* another song asked for while the first is waited for: the first is given up */
    lf_song_t t2 = s;
    t2.track = 8;
    snprintf(t2.title, sizeof t2.title, "Another song");
    g_first = ANSWER;
    g_hold = 1;
    g_asked = 0;
    CHECK(lf_ask(&s, 1) == 0);
    for (int i = 0; i < 400 && !g_asked; i++) t_sleep_ms(5);
    CHECK(g_asked == 1 && lf_state(&track, NULL, NULL) == LF_LOOKING && track == 7);
    CHECK(lf_ask(&t2, 1) == 0);
    CHECK(lf_state(&track, NULL, NULL) == LF_LOOKING && track == 8);
    g_hold = 0;
    CHECK(settled(&track, NULL, NULL) == LF_FOUND && track == 8);
    CHECK(lf_take(&l, &from) == 0);
    lyrics_free(&l);
    CHECK(t_file_size(path) < 0);                  /* (the first song's: never kept) */
    lyrics_song_t ls2 = {t2.path, t2.artist, t2.title, t2.album};
    char path2[1100];
    lyrics_store_path(store, &ls2, "lrc", path2, sizeof path2);
    CHECK(t_file_size(path2) > 0);
    remove(path2);

    /* stopping while one is waited for: it gives up, and nothing more is taken */
    g_hold = 1;
    CHECK(lf_ask(&s, 1) == 0);
    t_sleep_ms(30);
    lf_stop_ask();
    CHECK(lf_stop_wait(2000) == 1);
    g_hold = 0;
    CHECK(lf_state(NULL, NULL, NULL) == LF_IDLE && lf_ask(&s, 0) == -1);
    remove(none);
    rmdir(store);
}

int main(void)
{
    char dir[400];
    if (!t_mkdtemp(dir, sizeof dir, "fathom-test-lyrics")) return 1;
    test_plain();
    test_timed();
    test_encodings();
    test_tags(dir);
    test_local(dir);
    test_answers();
    test_worker(dir);
    rmdir(dir);
    return t_result();
}
