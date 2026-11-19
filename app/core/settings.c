/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "core/settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_ENTRIES 128

static struct { char key[48]; char value[160]; } g_e[MAX_ENTRIES];
static int g_n, g_dirty;
static unsigned g_changed_ms;
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

int settings_load(const char *path)
{
    snprintf(g_path, sizeof g_path, "%s", path);
    g_n = 0;
    g_dirty = 0;
    FILE *f = fopen(path, "re");
    if (!f) return 0;
    char line[256];
    while (fgets(line, sizeof line, f) && g_n < MAX_ENTRIES) {
        char *s = trim(line);
        if (!*s || *s == ';' || *s == '#' || *s == '[') continue;
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = '\0';
        char *k = trim(s), *v = trim(eq + 1);
        if (!*k) continue;
        int i = find(k);
        if (i < 0) i = g_n++;
        snprintf(g_e[i].key, sizeof g_e[i].key, "%s", k);
        snprintf(g_e[i].value, sizeof g_e[i].value, "%s", v);
    }
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
    if (i < 0) {
        if (g_n == MAX_ENTRIES) return;
        i = g_n++;
        snprintf(g_e[i].key, sizeof g_e[i].key, "%s", key);
    }
    snprintf(g_e[i].value, sizeof g_e[i].value, "%s", value);
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
    char tmp[600];
    snprintf(tmp, sizeof tmp, "%s.tmp", g_path);
    FILE *f = fopen(tmp, "we");
    if (!f) return -1;
    fprintf(f, "; Fathom settings. Edit with the player off; unknown keys are kept.\n");
    for (int i = 0; i < g_n; i++) fprintf(f, "%s = %s\n", g_e[i].key, g_e[i].value);
    int ok = fflush(f) == 0 && fsync(fileno(f)) == 0;
    ok = fclose(f) == 0 && ok;
    if (!ok || rename(tmp, g_path)) { unlink(tmp); return -1; }
    g_dirty = 0;
    return 0;
}

int settings_reset(void)
{
    g_n = 0;
    g_dirty = 1;
    return settings_save();
}

void settings_autosave(unsigned now_ms, unsigned quiet_ms)
{
    if (!g_dirty) return;
    if (!g_changed_ms) { g_changed_ms = now_ms ? now_ms : 1; return; }
    if (now_ms - g_changed_ms >= quiet_ms) settings_save();
}
