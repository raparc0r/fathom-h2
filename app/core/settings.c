/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
#define _DEFAULT_SOURCE   /* getline, strdup (musl, glibc) */
#include "core/settings.h"
#include "core/fsafe.h"

#include <errno.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* The file is a few KB. One far over that is not ours as we wrote it (a size
 * damaged on the card: hundreds of MB, and no line break in them, were read
 * into memory at every start): not read, and never saved over. */
#define SETTINGS_MAX (256 * 1024)

/* As many entries, keys and values as long, as the file has: a fixed table
 * once dropped what didn't fit, and the next save erased it from the file. */
typedef struct { char *key, *value; } entry_t;
static entry_t *g_e;
static int g_n, g_cap, g_dirty;
static int g_short;          /* an entry couldn't be held (out of memory): never save over the file */
static unsigned g_changed_ms;
static unsigned g_retry_ms;  /* after an autosave that failed: no other try sooner than this (0: none failed) */
static char g_path[512];

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    size_t l = strlen(s);
    while (l && (s[l - 1] == ' ' || s[l - 1] == '\t' || s[l - 1] == '\r' || s[l - 1] == '\n')) s[--l] = '\0';
    return s;
}

static int find(const char *key)
{
    for (int i = 0; i < g_n; i++)
        if (!strcmp(g_e[i].key, key)) return i;
    return -1;
}

static void clear(void)
{
    for (int i = 0; i < g_n; i++) free(g_e[i].key), free(g_e[i].value);
    g_n = 0;
}

/* key = value: replaced, or added. 0, or -1 (out of memory: unchanged). */
static int put(const char *key, const char *value)
{
    char *v = strdup(value);
    if (!v) return -1;
    int i = find(key);
    if (i >= 0) { free(g_e[i].value); g_e[i].value = v; return 0; }
    if (g_n == g_cap) {
        int cap = g_cap ? g_cap * 2 : 64;
        entry_t *e = realloc(g_e, (size_t)cap * sizeof *e);
        if (!e) { free(v); return -1; }
        g_e = e, g_cap = cap;
    }
    char *k = strdup(key);
    if (!k) { free(v); return -1; }
    g_e[g_n++] = (entry_t){k, v};
    return 0;
}

int settings_load(const char *path)
{
    snprintf(g_path, sizeof g_path, "%s", path);
    clear();
    g_dirty = g_short = 0;
    g_retry_ms = 0;
    FILE *f = fopen(path, "re");
    if (!f) {
        /* None yet (a first start): the defaults. There but unreadable (the
         * card's reads failing, no memory, no handles): not to be taken for
         * none, or the next save would write a file with only what was set
         * since, over the owner's settings. Nothing is saved this session. */
        if (errno != ENOENT) g_short = 1;
        return 0;
    }
    struct stat st;
    if (fstat(fileno(f), &st) || st.st_size > SETTINGS_MAX) {
        fclose(f);
        g_short = 1;
        return 0;
    }
    char *line = NULL;
    size_t cap = 0;
    int first = 1;
    while (getline(&line, &cap, f) >= 0) {
        /* (a byte-order mark, as Notepad's "UTF-8" saves one: not part of the first key) */
        char *s = trim(first && !strncmp(line, "\xEF\xBB\xBF", 3) ? line + 3 : line);
        first = 0;
        if (!*s || *s == ';' || *s == '#' || *s == '[') continue;
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = '\0';
        char *k = trim(s), *v = trim(eq + 1);
        if (!*k) continue;
        if (put(k, v)) g_short = 1;
    }
    /* Ended by a read that failed (the card, no memory for a line), not by
     * the file's end: what was read is not all of it, so it is not saved over. */
    if (ferror(f) || !feof(f)) g_short = 1;
    free(line);
    fclose(f);
    return 0;
}

const char *settings_get(const char *key, const char *def)
{
    int i = find(key);
    return i >= 0 ? g_e[i].value : def;
}

int settings_get_int(const char *key, int def)
{
    const char *v = settings_get(key, NULL);
    if (!v || !*v) return def;
    char *end;
    long n = strtol(v, &end, 10);
    return *end ? def : (int)n;
}

void settings_set(const char *key, const char *value)
{
    int i = find(key);
    if (i >= 0 && !strcmp(g_e[i].value, value)) return;
    if (put(key, value)) return;
    g_dirty = 1;
    g_changed_ms = 0;   /* settings_autosave() starts its quiet period from here */
}

void settings_set_int(const char *key, int value)
{
    char v[16];
    snprintf(v, sizeof v, "%d", value);
    settings_set(key, v);
}

int settings_save(void)
{
    if (!g_dirty || !g_path[0]) return 0;
    if (g_short) return -1;
    char tmp[600];
    snprintf(tmp, sizeof tmp, "%s.tmp", g_path);
    FILE *f = fopen(tmp, "we");
    if (!f) return -1;
    fprintf(f, "; Fathom settings. Edit with the player off; unknown keys are kept.\n");
    for (int i = 0; i < g_n; i++) fprintf(f, "%s = %s\n", g_e[i].key, g_e[i].value);
    if (file_commit(f, 1, tmp, g_path)) return -1;
    g_dirty = 0;
    g_retry_ms = 0;
    return 0;
}

int settings_reset(void)
{
    clear();
    g_dirty = 1;
    g_short = 0;
    return settings_save();
}

/* A save that fails (a full card, one gone read-only) is not tried again at
 * every tick: a file made and removed on the UI thread each second, for good.
 * The quiet period starts again, and is this long at least. */
#define SAVE_RETRY_MS 30000u

void settings_autosave(unsigned now_ms, unsigned quiet_ms)
{
    if (!g_dirty) return;
    if (!g_changed_ms) { g_changed_ms = now_ms ? now_ms : 1; return; }
    if (now_ms - g_changed_ms < (quiet_ms > g_retry_ms ? quiet_ms : g_retry_ms)) return;
    if (settings_save()) {
        g_changed_ms = now_ms ? now_ms : 1;
        g_retry_ms = SAVE_RETRY_MS;
    }
}
