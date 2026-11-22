/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/*
 * Settings (app/core/settings.c): every entry and every value of a file kept
 * through a load and a save, however many and however long.
 */
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "core/settings.h"

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static char g_path[600];

static void write_text(const char *text)
{
    FILE *f = fopen(g_path, "w");
    if (f) fputs(text, f), fclose(f);
}

/* 300 keys (a fixed table once held 128: the rest dropped, then erased by the next save) */
static void test_many(void)
{
    FILE *f = fopen(g_path, "w");
    if (!f) { failures++; return; }
    fprintf(f, "; a comment\n[a section]\n");
    for (int i = 0; i < 300; i++) fprintf(f, "test.key%d = %d\n", i, i * 7);
    fclose(f);
    settings_load(g_path);
    CHECK(settings_get_int("test.key0", -1) == 0);
    CHECK(settings_get_int("test.key299", -1) == 299 * 7);
    settings_set("display.dim", "30");             /* a change: saved */
    CHECK(settings_save() == 0);
    settings_load(g_path);
    int kept = 0;
    for (int i = 0; i < 300; i++) {
        char k[32];
        snprintf(k, sizeof k, "test.key%d", i);
        kept += settings_get_int(k, -1) == i * 7;
    }
    printf("many: %d of 300 kept through a save\n", kept);
    CHECK(kept == 300);
    CHECK(settings_get_int("display.dim", -1) == 30);
}

/* Long values: Home's every item hidden (157 characters), a long URL, a line
 * past the old 256-byte read, a long key */
static void test_long(void)
{
    static const char home[] = "-nowplaying,-shuffle,-search,-albums,-artists,-albumartists,-songs,-playlists,"
                               "-favourites,-recent,-history,-radio,settings,-genres,-composers,-years,-folders";
    char url[1200] = "https://example.test/";
    while (strlen(url) < 1100) strcat(url, "abcdefghij");
    char text[1600];
    snprintf(text, sizeof text, "update.api = %s\nwifi.on = 1\n", url);
    write_text(text);
    settings_load(g_path);
    CHECK(!strcmp(settings_get("update.api", ""), url));
    CHECK(settings_get_int("wifi.on", 0) == 1);
    settings_set("home.items", home);
    CHECK(!strcmp(settings_get("home.items", ""), home));
    char key[100];
    memset(key, 'k', 80), key[80] = '\0';            /* (keys were cut at 47) */
    settings_set(key, "long key");
    CHECK(settings_save() == 0);
    settings_load(g_path);
    CHECK(!strcmp(settings_get("home.items", ""), home));
    CHECK(!strcmp(settings_get("update.api", ""), url));
    CHECK(!strcmp(settings_get(key, ""), "long key"));
    CHECK(settings_get_int("wifi.on", 0) == 1);
}

/* The last of a key twice wins; a value set as it was isn't a change; reset */
static void test_rest(void)
{
    write_text("a.b = 1\n  a.b=2  \nno equals here\n = nameless\nc.d = x = y\n");
    settings_load(g_path);
    CHECK(settings_get_int("a.b", 0) == 2);
    CHECK(!strcmp(settings_get("c.d", ""), "x = y"));
    CHECK(settings_get("", NULL) == NULL);
    unlink(g_path);
    settings_set("a.b", "2");                       /* (unchanged: nothing written) */
    CHECK(settings_save() == 0 && access(g_path, F_OK) != 0);
    settings_set_int("a.b", 3);
    CHECK(settings_save() == 0 && access(g_path, F_OK) == 0);
    CHECK(settings_reset() == 0);
    settings_load(g_path);
    CHECK(settings_get("a.b", NULL) == NULL && settings_get("c.d", NULL) == NULL);
}

int main(void)
{
    snprintf(g_path, sizeof g_path, "%s/fathom-settings-%d.ini", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", (int)getpid());
    test_many();
    test_long();
    test_rest();
    unlink(g_path);
    if (failures) { fprintf(stderr, "%d failures\n", failures); return 1; }
    printf("settings: ok\n");
    return 0;
}
