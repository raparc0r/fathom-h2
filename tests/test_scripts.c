/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* Which writing a library's names are in (app/library/scripts.c), and the
 * character packs that calls for. */
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "library/library.h"
#include "library/scripts.h"
#include "test_util.h"

typedef struct { const char *title, *artist, *album; } song_t;

static void want(const song_t *songs, int n, int out[WANT_N], lib_scripts_t *counts)
{
    lib_scripts_t s;
    memset(&s, 0, sizeof s);
    for (int i = 0; i < n; i++) scripts_add(&s, songs[i].title, songs[i].artist, songs[i].album);
    scripts_want(&s, out);
    if (counts) *counts = s;
}
#define WANT(arr, out, counts) want(arr, (int)(sizeof arr / sizeof arr[0]), out, counts)

int main(void)
{
    int w[WANT_N];
    lib_scripts_t c;

    /* ---- one name ---- */
    CHECK(script_of(NULL) == 0 && script_of("") == 0);
    CHECK(script_of("Hocus Pocus") == 0 && script_of("16 pi\xC3\xA8" "ces") == 0);
    CHECK(script_of("\xD0\x9C\xD0\xBE\xD1\x81\xD0\xBA\xD0\xB2\xD0\xB0") == 0);                 /* Cyrillic */
    CHECK(script_of("東京の夜") == (SCRIPT_KANA | SCRIPT_HAN));
    CHECK(script_of("ｱﾙﾊﾞﾑ") == SCRIPT_KANA);                                                   /* half-width katakana */
    CHECK(script_of("東京") == SCRIPT_HAN);                                                    /* could be anyone's */
    CHECK(script_of("서울의 밤") == SCRIPT_HANGUL && script_of("ㅋㅋㅋ") == SCRIPT_HANGUL);
    CHECK(script_of("这是我们的歌") == (SCRIPT_HAN | SCRIPT_SIMPLIFIED));
    CHECK(script_of("這是我們的歌") == (SCRIPT_HAN | SCRIPT_TRADITIONAL));
    CHECK(script_of("齋藤") == (SCRIPT_HAN | SCRIPT_TRADITIONAL));                             /* a Japanese name in old forms: told apart by the count */
    CHECK(script_of("Live in 東京 (서울)") == (SCRIPT_HAN | SCRIPT_HANGUL));
    CHECK(script_of("\xE6\x9D") == 0 && script_of("abc\xFF\xFE" "def") == 0);                  /* cut short, not UTF-8: nothing, no harm */
    CHECK(script_of("\xF0\xA0\xAE\xB7") == 0);                                                  /* past the 16 bits a pack holds */

    /* ---- no such names: nothing wanted ---- */
    static const song_t latin[] = {{"Amber", "Ann", "Alpha"}, {"\xC3\x80 mi-chemin", "Hocus Pocus", "16 pi\xC3\xA8" "ces"}, {NULL, NULL, NULL}};
    WANT(latin, w, NULL);
    CHECK(!w[WANT_JA] && !w[WANT_KO] && !w[WANT_ZH_HANS] && !w[WANT_ZH_HANT]);

    /* ---- Japanese: kana anywhere in a song; its kanji-only songs are not Chinese ---- */
    static const song_t ja[] = {{"東京の夜", "山田太郎", "春の歌"}, {"東京", "山田太郎", "白雪"}, {"桜", "齋藤", "夜空"},
                                {"Intro", "ザ・サンプルズ", "Best"}, {"Amber", "Ann", "Alpha"}};
    WANT(ja, w, &c);
    CHECK(w[WANT_JA] == 2 && !w[WANT_KO] && !w[WANT_ZH_HANS] && !w[WANT_ZH_HANT]);
    CHECK(c.han_only == 2 && c.traditional == 1 && c.traditional_chars == 1);              /* (齋: one old form) */

    /* ...even with a whole album by an artist in old forms */
    static const song_t ja_old[] = {{"東京の夜", "山田太郎", "春の歌"}, {"桜", "齋藤", "夜空"}, {"月", "齋藤", "夜空"}, {"海", "齋藤", "夜空"},
                                    {"空", "櫻井", "白雪"}, {"星", "櫻井", "白雪"}, {"風", "國府田", "白雪"}};
    WANT(ja_old, w, &c);
    CHECK(w[WANT_JA] == 1 && !w[WANT_ZH_HANS] && !w[WANT_ZH_HANT]);
    CHECK(c.traditional == 6 && c.traditional_chars == 3);                                   /* six songs, three different characters: not Chinese */

    /* ---- Korean ---- */
    static const song_t ko[] = {{"서울의 밤", "김민준", "봄의 노래"}, {"Amber", "이서연", "Alpha"}, {"Amber", "Ann", "Alpha"}};
    WANT(ko, w, NULL);
    CHECK(!w[WANT_JA] && w[WANT_KO] == 2 && !w[WANT_ZH_HANS] && !w[WANT_ZH_HANT]);

    /* ---- Simplified Chinese: one telling character is enough, and every ideograph-only song counts ---- */
    static const song_t sc[] = {{"北京的夜晚", "张伟", "春天的歌"}, {"星空", "王芳", "白雪"}, {"Amber", "Ann", "Alpha"}};
    WANT(sc, w, NULL);
    CHECK(!w[WANT_JA] && !w[WANT_KO] && w[WANT_ZH_HANS] == 2 && !w[WANT_ZH_HANT]);

    /* ---- Traditional Chinese, alone in the library: one telling character is enough ---- */
    static const song_t tc[] = {{"臺北的夜晚", "陳怡君", "春天的歌"}, {"星空", "林志明", "白雪"}};
    WANT(tc, w, NULL);
    CHECK(!w[WANT_JA] && !w[WANT_ZH_HANS] && w[WANT_ZH_HANT] == 2);

    /* ---- Japanese and Traditional Chinese in one library: Chinese once enough different characters say so ---- */
    static const song_t ja_tc[] = {{"東京の夜", "山田太郎", "春の歌"}, {"這是我們的歌", "陳怡君", "臺北"}, {"你說對嗎", "張雅婷樂團", "來學習"}};
    WANT(ja_tc, w, &c);
    CHECK(w[WANT_JA] == 1 && !w[WANT_ZH_HANS] && w[WANT_ZH_HANT] == 2 && c.traditional_chars >= 5);

    /* ---- both forms of Chinese: each its own songs ---- */
    static const song_t both[] = {{"这是我们的歌", "张伟", "北京"}, {"這是我們的歌", "陳怡君", "臺北"}, {"你說對嗎", "林志明", "來學習"}, {"星空", "王芳", "白雪"}};
    WANT(both, w, NULL);
    CHECK(w[WANT_ZH_HANS] == 1 && w[WANT_ZH_HANT] == 2);

    /* ---- ideographs and nothing to say whose: both forms of Chinese offered, for the user to choose ---- */
    static const song_t han[] = {{"星空", "山田", "白雪"}, {"東京", "山田", "白雪"}};
    WANT(han, w, NULL);
    CHECK(!w[WANT_JA] && w[WANT_ZH_HANS] == 2 && w[WANT_ZH_HANT] == 2);

    /* ---- everything at once ---- */
    static const song_t all[] = {{"東京の夜", "山田太郎", "春の歌"}, {"서울의 밤", "김민준", "봄"}, {"这是我们的歌", "张伟", "北京"},
                                 {"這是我們的歌", "陳怡君", "臺北"}, {"你說對嗎", "林志明", "來學習"}, {"Amber", "Ann", "Alpha"}};
    WANT(all, w, NULL);
    CHECK(w[WANT_JA] == 1 && w[WANT_KO] == 1 && w[WANT_ZH_HANS] == 1 && w[WANT_ZH_HANT] == 2);

    /* ---- over a library: every song's title, artist, album and album's artist ---- */
    {
        const char *tmp = getenv("TMPDIR");
        char db[512], art[512];
        snprintf(db, sizeof db, "%s/fathom-test-scripts-%d.db", tmp && *tmp ? tmp : "/tmp", (int)getpid());
        snprintf(art, sizeof art, "%s/fathom-test-scripts-%d-art", tmp && *tmp ? tmp : "/tmp", (int)getpid());
        unlink(db);
        library_t *lib = library_open(db, art);
        CHECK(lib != NULL);
        lib_scripts_t s;
        CHECK(lib && library_scripts(lib, &s) == 0 && s.kana == 0 && s.han_only == 0);      /* an empty library */
        sqlite3 *raw = NULL;
        CHECK(sqlite3_open(db, &raw) == SQLITE_OK);
        static const char *const ROWS[][4] = {
            {"東京の夜", "山田太郎", "春の歌", "山田太郎"}, {"Amber", "Ann", "Alpha", NULL},
            {"Intro", "Various", "Best", "김민준"},                    /* only the album's artist is Korean */
            {"这是我们的歌", "张伟", "北京", "张伟"}, {"星空", "王芳", "白雪", NULL},
        };
        for (size_t i = 0; raw && i < sizeof ROWS / sizeof ROWS[0]; i++) {
            sqlite3_stmt *ins = NULL;
            sqlite3_prepare_v2(raw, "INSERT INTO tracks(path, title, artist, album, album_artist) VALUES(?,?,?,?,?)", -1, &ins, NULL);
            char path[32];
            snprintf(path, sizeof path, "/m/%d.flac", (int)i);
            sqlite3_bind_text(ins, 1, path, -1, SQLITE_STATIC);
            for (int k = 0; k < 4; k++) sqlite3_bind_text(ins, 2 + k, ROWS[i][k], -1, SQLITE_STATIC);
            CHECK(sqlite3_step(ins) == SQLITE_DONE);
            sqlite3_finalize(ins);
        }
        sqlite3_close(raw);
        CHECK(lib && library_scripts(lib, &s) == 0);
        scripts_want(&s, w);
        CHECK(s.kana == 1 && s.hangul == 1 && s.han_only == 2 && s.simplified == 1);
        CHECK(w[WANT_JA] == 1 && w[WANT_KO] == 1 && w[WANT_ZH_HANS] == 2 && !w[WANT_ZH_HANT]);
        CHECK(library_scripts(NULL, &s) == -1 && s.kana == 0);
        library_close(lib);
        unlink(db);
        char side[540];
        snprintf(side, sizeof side, "%s-wal", db); unlink(side);
        snprintf(side, sizeof side, "%s-shm", db); unlink(side);
        rmdir(art);
    }

    /* ---- the packs' names for these ---- */
    CHECK(scripts_want_of("ja") == WANT_JA && scripts_want_of("ko") == WANT_KO && scripts_want_of("zh-hans") == WANT_ZH_HANS);
    CHECK(scripts_want_of("zh-hant") == WANT_ZH_HANT && scripts_want_of("xx") == -1 && scripts_want_of(NULL) == -1);

    return t_result();
}
