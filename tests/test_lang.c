/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Translations of the UI (app/core/lang.c). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "core/lang.h"

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

int main(void)
{
    char dir[] = "/tmp/fathom-test-lang-XXXXXX";
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; }
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
          "Empty = \n", f);
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
    CHECK(lang_count() == 4);

    char names[8][64];
    CHECK(lang_list(dir, names, 8) == 2);
    CHECK(!strcmp(names[0], "Deutsch") && !strcmp(names[1], "Espa\xC3\xB1ol"));

    CHECK(lang_load(other) == 0 && !strcmp(tr("Settings"), "Einstellungen") && !strcmp(tr("Now playing"), "Now playing"));
    CHECK(lang_load(NULL) == 0 && !strcmp(tr("Settings"), "Settings") && lang_count() == 0);
    CHECK(lang_load("/nonexistent/x.txt") == -1 && !strcmp(tr("Settings"), "Settings"));

    unlink(path);
    unlink(other);
    rmdir(dir);
    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    printf("lang: all good\n");
    return 0;
}
