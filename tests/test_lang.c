/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* Translations of the UI (app/core/lang.c). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "core/lang.h"
#include "test_util.h"

int main(void)
{
    char dir[200];
    t_mkdtemp(dir, sizeof dir, "fathom-test-lang");
    char path[300], other[300];
    snprintf(path, sizeof path, "%s/Espa\xC3\xB1ol.txt", dir);
    snprintf(other, sizeof other, "%s/Deutsch.txt", dir);
    FILE *f = fopen(path, "w");
    fputs("\xEF\xBB\xBF# a comment\n"
          "Now playing = Reproduciendo\n"
          "  Settings = Ajustes   \r\n"
          "Two\\nlines = Dos\\nl\xC3\xADneas\n"
          "no separator here\n"
          "A = B = C\n"                   /* the first \" = \" splits */
          "Empty = \n"
          "%s songs = %s canciones\n"
          "%s of %s = %s de %s\n"
          "Bad %s = %d %n %s\n", f);
    fclose(f);
    f = fopen(other, "w");
    fputs("Settings = Einstellungen\n", f);
    fclose(f);

    CHECK(!strcmp(tr("Settings"), "Settings"));            /* English to start with */
    CHECK(lang_load(path) == 0);
    CHECK(!strcmp(tr("Now playing"), "Reproduciendo"));
    CHECK(!strcmp(tr("Settings"), "Ajustes"));
    CHECK(!strcmp(tr("Two\nlines"), "Dos\nl\xC3\xADneas"));
    CHECK(!strcmp(tr("A"), "B = C"));
    CHECK(!strcmp(tr("Empty"), "Empty"));                 /* no translation: English */
    CHECK(!strcmp(tr("Albums"), "Albums"));
    CHECK(tr(NULL) == NULL && !strcmp(tr(""), ""));
    CHECK(lang_count() == 7);

    char names[8][64];
    CHECK(lang_list(dir, names, 8) == 2);
    CHECK(!strcmp(names[0], "Deutsch") && !strcmp(names[1], "Espa\xC3\xB1ol"));

    /* Texts with numbers or names: each %s filled in, in the translation's
     * own order; nothing else in a translation is a format (a "%d" stays) */
    char out[64];
    tr_fill(out, sizeof out, "%s songs", "12", NULL);
    CHECK(!strcmp(out, "12 canciones"));
    tr_fill(out, sizeof out, "%s of %s", "3", "40");
    CHECK(!strcmp(out, "3 de 40"));
    tr_fill(out, sizeof out, "Untranslated %s", "x", NULL);
    CHECK(!strcmp(out, "Untranslated x"));                /* English, filled in */
    tr_fill(out, sizeof out, "Bad %s", "x", NULL);
    CHECK(!strcmp(out, "%d %n x"));                       /* (never handed to printf) */
    lang_fill(out, 8, "%s and %s", "abcdef", "ghi");
    CHECK(!strcmp(out, "abcdef "));                       /* cut to the buffer */
    lang_fill(out, sizeof out, "%s %s %s", "a", NULL);
    CHECK(!strcmp(out, "a  "));                           /* a missing one: empty */

    CHECK(lang_load(other) == 0 && !strcmp(tr("Settings"), "Einstellungen") && !strcmp(tr("Now playing"), "Now playing"));
    CHECK(lang_load(NULL) == 0 && !strcmp(tr("Settings"), "Settings") && lang_count() == 0);
    CHECK(lang_load("/nonexistent/x.txt") == -1 && !strcmp(tr("Settings"), "Settings"));

    unlink(path);
    unlink(other);
    rmdir(dir);
    return t_result();
}
