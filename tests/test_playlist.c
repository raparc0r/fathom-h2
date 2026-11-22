/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* M3U parsing, path normalisation and loose keys (app/library/playlist.c). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "library/playlist.h"
#include "tmpdir.h"

static int failures;

#define CHECK_STR(got, want) do { \
    if (strcmp((got), (want))) { fprintf(stderr, "%s:%d: got \"%s\", want \"%s\"\n", __FILE__, __LINE__, (got), (want)); failures++; } \
} while (0)
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static void norm(const char *in, const char *want)
{
    char b[512];
    snprintf(b, sizeof b, "%s", in);
    path_normalize(b);
    CHECK_STR(b, want);
}

static void key(const char *in, int tail, const char *want)
{
    char b[512];
    path_loose_key(in, tail, b, sizeof b);
    CHECK_STR(b, want);
}

int main(void)
{
    norm("/a/b/../c/./d.flac", "/a/c/d.flac");
    norm("/mnt/sd/Music/playlists/../Artist/Album/01 x.flac", "/mnt/sd/Music/Artist/Album/01 x.flac");
    norm("a//b///c", "a/b/c");
    norm("..\\Artist\\Album\\t.mp3", "../Artist/Album/t.mp3");
    norm("/../x", "/../x");

    key("/mnt/sd/Music/Artist/Album/08 - Song.flac", 3, "artist/album/08songflac");
    key("/Users/someone/Music/Artist/Album/08 - Song.flac", 2, "album/08songflac");
    /* "Français": NFC (U+00E7) and NFD (c + U+0327) fold the same */
    key("/x/Fran\xC3\xA7" "ais/t.flac", 2, "francais/tflac");
    key("/x/Franc\xCC\xA7" "ais/t.flac", 2, "francais/tflac");
    key("/x/Bj\xC3\xB6rk/\xC3\x89t\xC3\xA9.flac", 2, "bjork/eteflac");
    key("short.flac", 3, "shortflac");

    const char text[] =
        "\xEF\xBB\xBF#EXTM3U\r\n"
        "#PLAYLIST:Soul \xE2\x80\x94 Funk Revue\r\n"
        "# a comment line\r\n"
        "#EXTINF:277,Artist - Title\r\n"
        "../Artist/Album/08 - Title.flac\r\n"
        "\r\n"
        "   ../Other/Album/01 - Intro.mp3  \n"
        "http://example.invalid/stream.mp3\n"
        "/abs/path/song.flac\n"
        "C:\\Music\\Artist\\Album\\02 - Two.flac\n";
    m3u_t m;
    CHECK(m3u_parse(text, sizeof text - 1, "/mnt/sd/Music/playlists", "fallback", &m) == 0);
    CHECK_STR(m.name, "Soul \xE2\x80\x94 Funk Revue");
    CHECK(m.n == 4);
    if (m.n == 4) {
        CHECK_STR(m.paths[0], "/mnt/sd/Music/Artist/Album/08 - Title.flac");
        CHECK(m.infos[0] && !strcmp(m.infos[0], "Artist - Title") && m.secs[0] == 277);
        CHECK(m.infos[1] == NULL && m.secs[1] == -1);
        CHECK_STR(m.paths[1], "/mnt/sd/Music/Other/Album/01 - Intro.mp3");
        CHECK_STR(m.paths[2], "/abs/path/song.flac");
        CHECK_STR(m.paths[3], "C:/Music/Artist/Album/02 - Two.flac");
    }
    m3u_free(&m);

    CHECK(m3u_parse("one.flac\ntwo.flac", 17, "/d", "My List", &m) == 0);
    CHECK_STR(m.name, "My List");
    CHECK(m.n == 2 && !strcmp(m.paths[1], "/d/two.flac"));
    m3u_free(&m);

    char k[128];
    text_loose_key("Caf\xC3\xA9 Tacvba - Las Flores", k, sizeof k);
    CHECK_STR(k, "cafetacvbalasflores");

    char f[128];
    text_search_fold("Beyonc\xC3\xA9 \xE2\x80\x94 D\xC3\xA9j\xC3\xA0 Vu (Live)", f, sizeof f);
    CHECK_STR(f, "beyonce deja vu live");
    text_search_fold("  Don't Stop Me Now\xE2\x80\x99s  ", f, sizeof f);
    CHECK_STR(f, "dont stop me nows");
    text_search_fold("\xD0\x9C\xD0\xBE\xD1\x81\xD0\xBA\xD0\xB2\xD0\xB0 2000", f, sizeof f);
    CHECK_STR(f, "2000");

    CHECK(m3u_supported("/x/List.M3U8") && m3u_supported("a.m3u") && !m3u_supported("a.m3u8.txt"));

    /* An .m3u from Windows, in its code page: lines that aren't UTF-8 are
     * taken as Windows-1252 ("Café", "Don’t"); UTF-8 lines in it stay as
     * they are; an .m3u8 is UTF-8 whatever it holds */
    char dir[200], path[300];
    test_tmp(dir, sizeof dir, "fathom-test-playlist");
    if (mkdtemp(dir)) {
        const char latin[] = "#EXTINF:200,Caf\xE9 Tacvba - Don\x92t Go\r\n"
                             "Caf\xE9 Tacvba\\Album\\01.flac\r\n"
                             "Bj\xC3\xB6rk/Post/02.flac\n"
                             "plain/03.flac";
        const char *names[2] = {"list.m3u", "list.m3u8"};
        for (int i = 0; i < 2; i++) {
            snprintf(path, sizeof path, "%s/%s", dir, names[i]);
            FILE *pf = fopen(path, "wb");
            if (pf) { fwrite(latin, 1, sizeof latin - 1, pf); fclose(pf); }
            CHECK(m3u_read(path, &m) == 0 && m.n == 3);
            if (m.n == 3) {
                char want[400];
                snprintf(want, sizeof want, "%s/%s", dir, i ? "Caf\xE9 Tacvba/Album/01.flac" : "Caf\xC3\xA9 Tacvba/Album/01.flac");
                CHECK_STR(m.paths[0], want);
                CHECK_STR(m.infos[0], i ? "Caf\xE9 Tacvba - Don\x92t Go" : "Caf\xC3\xA9 Tacvba - Don\xE2\x80\x99t Go");
                snprintf(want, sizeof want, "%s/Bj\xC3\xB6rk/Post/02.flac", dir);
                CHECK_STR(m.paths[1], want);
                snprintf(want, sizeof want, "%s/plain/03.flac", dir);
                CHECK_STR(m.paths[2], want);
            }
            m3u_free(&m);
            unlink(path);
        }
        rmdir(dir);
    }

    if (failures) fprintf(stderr, "%d failure(s)\n", failures);
    else printf("playlist: all checks passed\n");
    return failures != 0;
}
