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
#include <sys/stat.h>
#include <unistd.h>

#include "core/settings.h"
#include "test_util.h"

static char g_path[600];

static void write_text(const char *text)
{
    t_write_text(g_path, text);
}

/* 300 keys (a fixed table once held 128: the rest dropped, then erased by the next save) */
static void test_many(void)
{
    FILE *f = fopen(g_path, "w");
    if (!f) { t_fails++; return; }
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

/* A file saved with a byte-order mark (Notepad's "UTF-8"): its first key is
 * the key, not three bytes and the key (the setting lost, the other kept for good) */
static void test_bom(void)
{
    write_text("\xEF\xBB\xBF" "display.dim = 40\nwifi.on = 1\n");
    settings_load(g_path);
    CHECK(settings_get_int("display.dim", -1) == 40);
    CHECK(settings_get_int("wifi.on", -1) == 1);
    CHECK(settings_get("\xEF\xBB\xBF" "display.dim", NULL) == NULL);
    write_text("\xEF\xBB\xBF" "; a comment first\ndisplay.dim = 50\n\xEF\xBB\xBFlater = 1\n");
    settings_load(g_path);
    CHECK(settings_get_int("display.dim", -1) == 50);
    CHECK(settings_get_int("\xEF\xBB\xBFlater", -1) == 1);   /* (only at the file's start is it a mark) */
}

static long file_size(void)
{
    FILE *f = fopen(g_path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fclose(f);
    return n;
}

/* A file far bigger than settings are (a size damaged on the card): not read
 * into memory at every start, and not written over either */
static void test_huge(void)
{
    FILE *f = fopen(g_path, "w");
    if (!f) { t_fails++; return; }
    fprintf(f, "display.dim = 40\njunk = ");
    for (int i = 0; i < 300 * 1024; i++) fputc('x', f);    /* (and no line break in it) */
    fclose(f);
    long size = file_size();
    settings_load(g_path);
    CHECK(settings_get("junk", NULL) == NULL && settings_get_int("display.dim", -1) == -1);
    settings_set("display.dim", "30");                      /* (works for the session) */
    CHECK(settings_get_int("display.dim", -1) == 30);
    CHECK(settings_save() == -1 && file_size() == size);
    /* the most a file may be is still read, and saved */
    f = fopen(g_path, "w");
    if (!f) { t_fails++; return; }
    int head = fprintf(f, "display.dim = 40\nlong = ");
    for (int i = 0; i < 256 * 1024 - head; i++) fputc('x', f);
    fclose(f);
    CHECK(file_size() == 256 * 1024);
    settings_load(g_path);
    CHECK(settings_get_int("display.dim", -1) == 40 && (int)strlen(settings_get("long", "")) == 256 * 1024 - head);
    settings_set("display.dim", "30");
    CHECK(settings_save() == 0);
}

/* A save that can't be written (a full card; here a folder that isn't there)
 * is tried again after half a minute, not at every tick */
static void test_retry(void)
{
    char dir[640], path[700];
    snprintf(dir, sizeof dir, "%s.dir", g_path);
    snprintf(path, sizeof path, "%s/settings.ini", dir);
    rmdir(dir);
    settings_load(path);
    settings_set("display.dim", "30");
    settings_autosave(1000, 1500);                   /* the quiet period starts */
    settings_autosave(2000, 1500);
    settings_autosave(3000, 1500);                   /* a try: nowhere to write */
    CHECK(mkdir(dir, 0755) == 0);                    /* (from here a try would write the file) */
    for (unsigned t = 4000; t < 33000; t += 1000) settings_autosave(t, 1500);
    CHECK(access(path, F_OK) != 0);                  /* no try in the 30 s after the one that failed */
    settings_autosave(33000, 1500);
    CHECK(access(path, F_OK) == 0);
    settings_load(path);
    CHECK(settings_get_int("display.dim", -1) == 30);

    /* a change after a failed save: the wait starts again from the change */
    unlink(path);
    CHECK(rmdir(dir) == 0);
    settings_set("display.dim", "40");
    settings_autosave(100000, 1500);
    settings_autosave(102000, 1500);                 /* fails */
    settings_set("display.dim", "50");
    settings_autosave(103000, 1500);                 /* the quiet period, from this change */
    CHECK(mkdir(dir, 0755) == 0);
    for (unsigned t = 104000; t < 133000; t += 1000) settings_autosave(t, 1500);
    CHECK(access(path, F_OK) != 0);
    settings_autosave(133000, 1500);
    CHECK(access(path, F_OK) == 0);

    /* once a save has worked, the next change is saved after the quiet period as ever */
    unlink(path);
    settings_set("display.dim", "60");
    settings_autosave(200000, 1500);
    settings_autosave(201000, 1500);
    CHECK(access(path, F_OK) != 0);
    settings_autosave(201500, 1500);
    CHECK(access(path, F_OK) == 0);
    /* and a save asked for outright (a setting that must be on the card now) is tried whenever */
    CHECK(rmdir(dir) != 0);
    unlink(path);
    CHECK(rmdir(dir) == 0);
    settings_set("display.dim", "70");
    settings_autosave(300000, 1500);
    settings_autosave(302000, 1500);                 /* fails */
    CHECK(mkdir(dir, 0755) == 0);
    CHECK(settings_save() == 0 && access(path, F_OK) == 0);
    unlink(path);
    rmdir(dir);
}

/* A load ended by a read that failed is not the whole file: nothing is saved
 * over it. (A folder stands in for the file: it opens, and reading it fails.
 * A save tried writes its temporary file first, and removes it when the
 * rename fails: that file, there before, shows whether one was tried.) */
static void test_read_error(void)
{
    char dir[640], tmp[700], b[16] = "";
    snprintf(dir, sizeof dir, "%s.unread", g_path);
    snprintf(tmp, sizeof tmp, "%s.tmp", dir);
    CHECK(mkdir(dir, 0755) == 0);
    FILE *f = fopen(tmp, "w");
    if (f) fputs("keep", f), fclose(f);
    settings_load(dir);
    settings_set("display.dim", "30");
    CHECK(settings_get_int("display.dim", -1) == 30);       /* (works for the session) */
    CHECK(settings_save() == -1);
    f = fopen(tmp, "r");
    CHECK(f && fgets(b, sizeof b, f) && !strcmp(b, "keep"));
    if (f) fclose(f);
    unlink(tmp);
    rmdir(dir);
    /* a file read to its end, with or without a last line break, is saved as ever */
    write_text("a.b = 1\nc.d = 2");
    settings_load(g_path);
    settings_set("a.b", "3");
    CHECK(settings_save() == 0);
    settings_load(g_path);
    CHECK(settings_get_int("a.b", -1) == 3 && settings_get_int("c.d", -1) == 2);
    write_text("");
    settings_load(g_path);
    settings_set("a.b", "4");
    CHECK(settings_save() == 0);
}

int main(void)
{
    snprintf(g_path, sizeof g_path, "%s/fathom-settings-%d.ini", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", (int)getpid());
    test_many();
    test_long();
    test_rest();
    test_bom();
    test_huge();
    test_retry();
    test_read_error();
    unlink(g_path);
    return t_result();
}
