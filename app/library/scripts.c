/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
#include "library/scripts.h"

#include <string.h>

#include "library/scripts_tables.h"

_Static_assert(sizeof SIMPLIFIED_ONLY / sizeof SIMPLIFIED_ONLY[0] == 1060 &&
               sizeof TRADITIONAL_ONLY / sizeof TRADITIONAL_ONLY[0] == 1389,
               "lib_scripts_t's seen_s and seen_t are sized for the tables");

/* The next character of UTF-8 text (0 at its end; U+FFFD for bytes that aren't one) */
static uint32_t next_cp(const unsigned char **p)
{
    const unsigned char *s = *p;
    uint32_t c = *s;
    if (!c) return 0;
    int more = c < 0x80 ? 0 : (c & 0xe0) == 0xc0 ? 1 : (c & 0xf0) == 0xe0 ? 2 : (c & 0xf8) == 0xf0 ? 3 : -1;
    if (more < 0) { *p = s + 1; return 0xfffd; }
    if (more) c &= 0x3f >> more;
    s++;
    for (int i = 0; i < more; i++, s++) {
        if ((*s & 0xc0) != 0x80) { *p = s; return 0xfffd; }
        c = c << 6 | (*s & 0x3f);
    }
    *p = s;
    return c;
}

static int place(const uint16_t *t, int n, uint32_t cp)
{
    int lo = 0, hi = n;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (t[mid] == cp) return mid;
        if (t[mid] < cp) lo = mid + 1;
        else hi = mid;
    }
    return -1;
}

#define N_S ((int)(sizeof SIMPLIFIED_ONLY / sizeof SIMPLIFIED_ONLY[0]))
#define N_T ((int)(sizeof TRADITIONAL_ONLY / sizeof TRADITIONAL_ONLY[0]))

/* What the text has; the telltale characters met noted in s (if any) */
static unsigned look(const char *utf8, lib_scripts_t *s)
{
    unsigned has = 0;
    if (!utf8) return 0;
    const unsigned char *p = (const unsigned char *)utf8;
    for (uint32_t cp; (cp = next_cp(&p));) {
        if (cp < 0x1100) continue;                                  /* (Latin, Greek, Cyrillic...: nearly everything) */
        if ((cp >= 0x3040 && cp <= 0x30ff) || (cp >= 0xff66 && cp <= 0xff9f)) has |= SCRIPT_KANA;
        else if ((cp >= 0xac00 && cp <= 0xd7a3) || (cp >= 0x3131 && cp <= 0x318e) || (cp >= 0x1100 && cp <= 0x11ff)) has |= SCRIPT_HANGUL;
        else if (cp >= 0x4e00 && cp <= 0x9fff) {
            has |= SCRIPT_HAN;
            int i = place(SIMPLIFIED_ONLY, N_S, cp);
            if (i >= 0) {
                has |= SCRIPT_SIMPLIFIED;
                if (s) s->seen_s[i / 8] |= (uint8_t)(1 << (i % 8));
            } else if ((i = place(TRADITIONAL_ONLY, N_T, cp)) >= 0) {
                has |= SCRIPT_TRADITIONAL;
                if (s) s->seen_t[i / 8] |= (uint8_t)(1 << (i % 8));
            }
        }
    }
    return has;
}

unsigned script_of(const char *utf8) { return look(utf8, NULL); }

static int bits(const uint8_t *b, size_t n)
{
    int c = 0;
    for (size_t i = 0; i < n; i++)
        for (uint8_t v = b[i]; v; v &= (uint8_t)(v - 1)) c++;
    return c;
}

void scripts_add(lib_scripts_t *s, const char *title, const char *artist, const char *album)
{
    /* (a song with kana is Japanese: the old forms in its names aren't Chinese's, and aren't noted) */
    unsigned plain = script_of(title) | script_of(artist) | script_of(album);
    if (plain & SCRIPT_KANA) s->kana++;
    if (plain & SCRIPT_HANGUL) s->hangul++;
    if (!(plain & SCRIPT_HAN) || (plain & SCRIPT_KANA)) return;
    s->han_only++;
    unsigned has = look(title, s) | look(artist, s) | look(album, s);
    if (has & SCRIPT_SIMPLIFIED) s->simplified++;
    if (has & SCRIPT_TRADITIONAL) s->traditional++;
    s->simplified_chars = bits(s->seen_s, sizeof s->seen_s);
    s->traditional_chars = bits(s->seen_t, sizeof s->seen_t);
}

/* How many different Traditional-only characters say "Chinese" in a library
 * that also has Japanese in it: Japanese names keep a few old forms (齋藤,
 * 櫻井, 國府田), a library of Chinese music has dozens. */
#define TRADITIONAL_AMONG_JAPANESE 5

void scripts_want(const lib_scripts_t *s, int songs[WANT_N])
{
    memset(songs, 0, WANT_N * sizeof songs[0]);
    songs[WANT_JA] = s->kana;
    songs[WANT_KO] = s->hangul;
    int simp = s->simplified_chars >= 1;
    int trad = s->traditional_chars >= (s->kana > 0 ? TRADITIONAL_AMONG_JAPANESE : 1);
    if (simp && trad) {
        songs[WANT_ZH_HANS] = s->simplified;
        songs[WANT_ZH_HANT] = s->traditional;
    } else if (simp) {
        songs[WANT_ZH_HANS] = s->han_only;
    } else if (trad) {
        songs[WANT_ZH_HANT] = s->han_only;
    } else if (s->han_only > 0 && s->kana == 0) {
        /* ideographs, nothing to say whose: either form of Chinese, for the user to choose */
        songs[WANT_ZH_HANS] = songs[WANT_ZH_HANT] = s->han_only;
    }
}

int scripts_want_of(const char *pack_id)
{
    static const char *const ID[WANT_N] = {"ja", "ko", "zh-hans", "zh-hant"};
    for (int i = 0; pack_id && i < WANT_N; i++)
        if (!strcmp(pack_id, ID[i])) return i;
    return -1;
}
