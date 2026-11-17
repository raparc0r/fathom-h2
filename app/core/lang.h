/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Translations of the UI's own text (not the music's). A language is a UTF-8
 * file, fathom/lang/NAME.txt on the card, its name the one shown in Settings >
 * Display > Language, with one line per text:
 *
 *     # comments start with '#'
 *     Now playing = Reproduciendo
 *     Settings = Ajustes
 *
 * The English text, " = ", the translation; "\n" in either is a line break.
 * Text with no line stays in English, so a partial translation works.
 * tools/lang/strings.py lists the texts to translate.
 */
#ifndef FATHOM_LANG_H
#define FATHOM_LANG_H

#include <stddef.h>

/* The languages in dir (the files' names without ".txt"), sorted; how many. */
int  lang_list(const char *dir, char names[][64], int max);
/* Use the translations in path; NULL or "" goes back to English. 0 on success. */
int  lang_load(const char *path);
/* The translation of en, or en itself. */
const char *tr(const char *en);
/* How many texts the loaded language translates (0: English). */
int  lang_count(void);

#endif
