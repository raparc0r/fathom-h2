/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* Translations of the UI (lang.h): an open-addressing hash table over one
 * buffer holding the file's text. */
#include "core/lang.h"
#include "core/hash.h"

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static struct {
    char *buf;                 /* the file, rewritten in place: "en\0tr\0" pairs */
    const char **key, **val;   /* the table: cap slots, NULL = empty */
    size_t cap;
    int n;
} L;

static uint32_t hash(const char *s) { return fnv32(FNV32_START, s, strlen(s)); }   /* FNV-1a */

static void clear(void)
{
    free(L.buf);
    free(L.key);
    free(L.val);
    memset(&L, 0, sizeof L);
}

/* "\n" -> a line break, in place; trailing spaces off. */
static void unescape(char *s)
{
    char *w = s;
    for (char *r = s; *r; r++) {
        if (r[0] == '\\' && r[1] == 'n') { *w++ = '\n'; r++; }
        else *w++ = *r;
    }
    while (w > s && (w[-1] == ' ' || w[-1] == '\t' || w[-1] == '\r')) w--;
    *w = '\0';
}

static void put(const char *k, const char *v)
{
    size_t i = hash(k) & (L.cap - 1);
    while (L.key[i] && strcmp(L.key[i], k)) i = (i + 1) & (L.cap - 1);
    if (!L.key[i]) L.n++;
    L.key[i] = k;
    L.val[i] = v;
}

int lang_load(const char *path)
{
    clear();
    if (!path || !*path) return 0;
    FILE *f = fopen(path, "rbe");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len < 0 || len > 1 << 20 || !(L.buf = malloc((size_t)len + 1)) ||
        fread(L.buf, 1, (size_t)len, f) != (size_t)len) {
        fclose(f);
        clear();
        return -1;
    }
    fclose(f);
    L.buf[len] = '\0';
    size_t lines = 1;
    for (long i = 0; i < len; i++) lines += L.buf[i] == '\n';
    for (L.cap = 16; L.cap < lines * 2; L.cap *= 2) {}
    L.key = calloc(L.cap, sizeof *L.key);
    L.val = calloc(L.cap, sizeof *L.val);
    if (!L.key || !L.val) { clear(); return -1; }
    char *p = L.buf;
    if (!strncmp(p, "\xEF\xBB\xBF", 3)) p += 3;             /* a byte-order mark */
    while (*p) {
        char *line = p, *end = strchr(p, '\n');
        if (end) { *end = '\0'; p = end + 1; } else p += strlen(p);
        while (*line == ' ' || *line == '\t') line++;
        if (*line == '#' || !*line) continue;
        char *eq = strstr(line, " = ");
        if (!eq) continue;
        *eq = '\0';
        char *v = eq + 3;
        while (*v == ' ') v++;
        unescape(line);
        unescape(v);
        if (*line && *v) put(line, v);
    }
    return 0;
}

const char *tr(const char *en)
{
    if (!L.n || !en || !*en) return en;
    size_t i = hash(en) & (L.cap - 1);
    while (L.key[i]) {
        if (!strcmp(L.key[i], en)) return L.val[i];
        i = (i + 1) & (L.cap - 1);
    }
    return en;
}

int lang_count(void) { return L.n; }

void lang_fill(char *out, size_t cap, const char *fmt, const char *a, const char *b)
{
    size_t n = 0;
    if (!cap) return;
    for (const char *p = fmt; *p && n + 1 < cap; p++) {
        const char *arg = p[0] == '%' && p[1] == 's' ? (a ? a : b ? b : "") : NULL;
        if (!arg) { out[n++] = *p; continue; }
        if (a) a = NULL; else b = NULL;
        while (*arg && n + 1 < cap) out[n++] = *arg++;
        p++;
    }
    out[n] = '\0';
}

void tr_fill(char *out, size_t cap, const char *en, const char *a, const char *b)
{
    lang_fill(out, cap, tr(en), a, b);
}

static int by_name(const void *a, const void *b) { return strcasecmp((const char *)a, (const char *)b); }

int lang_list(const char *dir, char names[][64], int max)
{
    DIR *d = opendir(dir);
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < max) {
        size_t len = strlen(e->d_name);
        if (e->d_name[0] == '.' || len < 5 || len - 4 > 63 || strcasecmp(e->d_name + len - 4, ".txt")) continue;
        snprintf(names[n], 64, "%.*s", (int)(len - 4), e->d_name);
        n++;
    }
    closedir(d);
    qsort(names, (size_t)n, 64, by_name);
    return n;
}
