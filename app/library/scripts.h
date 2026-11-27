/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/*
 * Which writing a library's names are in, for offering the character packs
 * that draw them (ui/glyphpack.h, net/packfetch.h): Japanese, Korean, Chinese
 * in its two forms. Kana is Japanese and Hangul is Korean, with no doubt.
 * Ideographs alone could be any of three, so: a character only Simplified
 * Chinese has settles it; the characters Traditional Chinese has and everyday
 * Japanese hasn't settle it once there are several different ones (Japanese
 * keeps a few of them for old names).
 */
#ifndef FATHOM_SCRIPTS_H
#define FATHOM_SCRIPTS_H

#include <stdint.h>

/* What one name has */
enum { SCRIPT_KANA = 1, SCRIPT_HANGUL = 2, SCRIPT_HAN = 4, SCRIPT_SIMPLIFIED = 8, SCRIPT_TRADITIONAL = 16 };
unsigned script_of(const char *utf8);

/* A library's names, counted (library_scripts fills it, song by song) */
typedef struct {
    int kana, hangul;           /* songs with kana, with Hangul, in their title, artist or album */
    int han_only;               /* songs with ideographs and no kana */
    int simplified, traditional;   /* ...of those, songs with a character only that form of Chinese has */
    int simplified_chars, traditional_chars;   /* how many different such characters, over the library */
    uint8_t seen_s[1060 / 8 + 1], seen_t[1389 / 8 + 1];   /* (which: scripts.c's own) */
} lib_scripts_t;

/* One song's names into the count */
void scripts_add(lib_scripts_t *s, const char *title, const char *artist, const char *album);

/* The packs the library's names call for, and for how many songs each. */
enum { WANT_JA, WANT_KO, WANT_ZH_HANS, WANT_ZH_HANT, WANT_N };
void scripts_want(const lib_scripts_t *s, int songs[WANT_N]);
/* The place in that list of a pack's id ("ja", "zh-hans"); -1: none */
int scripts_want_of(const char *pack_id);

#endif
