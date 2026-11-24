/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
#include "radio.h"
#include "core/fsafe.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void trim_copy(char *dst, size_t cap, const char *a, const char *b)
{
    while (a < b && isspace((unsigned char)*a)) a++;
    while (b > a && isspace((unsigned char)b[-1])) b--;
    size_t n = (size_t)(b - a);
    if (n >= cap) n = cap - 1;
    memcpy(dst, a, n);
    dst[n] = '\0';
}

int radio_icon_ok(const char *url)
{
    if (!url || (strncmp(url, "http://", 7) && strncmp(url, "https://", 8))) return 0;
    size_t n = 0;
    for (const char *c = url; *c; c++, n++)
        if ((unsigned char)*c <= ' ' || *c == '|') return 0;
    return n < sizeof ((radio_station_t *)0)->icon;
}

int radio_parse(const char *text, radio_station_t *out, int max)
{
    int n = 0;
    const char *line = text;
    while (line && *line && n < max) {
        const char *end = strchr(line, '\n');
        const char *stop = end ? end : line + strlen(line);
        const char *s = line;
        while (s < stop && isspace((unsigned char)*s)) s++;
        if (s < stop && *s != '#') {
            /* Name = URL [| info: SOURCE] [| icon: URL]; the name is up to the first
             * '=' that is followed by a URL (names may hold '='; URLs do too). */
            const char *eq = NULL;
            for (const char *q = s; q < stop; q++) {
                if (*q != '=') continue;
                const char *u = q + 1;
                while (u < stop && isspace((unsigned char)*u)) u++;
                if (!strncmp(u, "http://", 7) || !strncmp(u, "https://", 8)) { eq = q; break; }
            }
            if (eq) {
                radio_station_t *r = &out[n];
                memset(r, 0, sizeof *r);
                trim_copy(r->name, sizeof r->name, s, eq);
                const char *u = eq + 1, *bar = NULL;
                for (const char *q = u; q < stop; q++)
                    if (*q == '|') { bar = q; break; }
                trim_copy(r->url, sizeof r->url, u, bar ? bar : stop);
                for (char *c = r->url; *c; c++)
                    if (isspace((unsigned char)*c)) { *c = '\0'; break; }   /* a URL has no spaces */
                while (bar) {                      /* what follows the stream, each part up to the next '|' */
                    const char *i = bar + 1, *next = memchr(i, '|', (size_t)(stop - i));
                    const char *e = next ? next : stop;
                    while (i < e && isspace((unsigned char)*i)) i++;
                    if (!strncmp(i, "info:", 5)) {
                        trim_copy(r->info, sizeof r->info, i + 5, e);
                    } else if (!strncmp(i, "icon:", 5)) {
                        char icon[sizeof r->icon + 8];   /* (room to see one that's too long: left out, not cut) */
                        trim_copy(icon, sizeof icon, i + 5, e);
                        if (radio_icon_ok(icon)) memcpy(r->icon, icon, strlen(icon) + 1);
                    } else if (!strncmp(i, "site:", 5)) {
                        char site[sizeof r->site + 8];
                        trim_copy(site, sizeof site, i + 5, e);
                        if (radio_icon_ok(site)) memcpy(r->site, site, strlen(site) + 1);
                    }
                    bar = next;
                }
                if (!r->name[0]) snprintf(r->name, sizeof r->name, "Station %d", n + 1);
                n++;
            }
        }
        line = end ? end + 1 : NULL;
    }
    return n;
}

int radio_load(const char *path, radio_station_t *out, int max)
{
    FILE *f = fopen(path, "rbe");
    if (!f) return -1;
    char *buf = malloc(65536);
    if (!buf) { fclose(f); return -1; }
    size_t n = fread(buf, 1, 65535, f);
    fclose(f);
    buf[n] = '\0';
    char *t = buf;
    if (n >= 3 && (unsigned char)t[0] == 0xef && (unsigned char)t[1] == 0xbb && (unsigned char)t[2] == 0xbf) t += 3;
    for (char *c = t; *c; c++)
        if (*c == '\r') *c = '\n';                   /* Windows and old Mac line ends */
    int r = radio_parse(t, out, max);
    free(buf);
    return r;
}

int radio_load_list(const char *path, const char *example, radio_station_t *out, int max)
{
    int n = radio_load(path, out, max);
    if (n < 0) return example ? radio_load(example, out, max) : -1;
    int missing = 0;
    for (int i = 0; i < n; i++) missing |= !out[i].icon[0] || !out[i].site[0];
    radio_station_t *ex = missing && example ? malloc(RADIO_MAX * sizeof *ex) : NULL;
    int nex = ex ? radio_load(example, ex, RADIO_MAX) : 0;
    for (int i = 0; i < n; i++)
        for (int k = 0; k < nex && (!out[i].icon[0] || !out[i].site[0]); k++) {
            if (strcmp(ex[k].url, out[i].url)) continue;
            if (!out[i].icon[0] && ex[k].icon[0]) memcpy(out[i].icon, ex[k].icon, sizeof out[i].icon);
            if (!out[i].site[0] && ex[k].site[0]) memcpy(out[i].site, ex[k].site, sizeof out[i].site);
        }
    free(ex);
    return n;
}

/* ---------- changing the list ---------- */

#define LIST_MAX 65536

/* A file's text (NUL-terminated, a BOM dropped), or NULL. */
static char *read_text(const char *path)
{
    FILE *f = path ? fopen(path, "rbe") : NULL;
    if (!f) return NULL;
    char *buf = malloc(LIST_MAX);
    size_t n = buf ? fread(buf, 1, LIST_MAX - 1, f) : 0;
    fclose(f);
    if (!buf) return NULL;
    buf[n] = '\0';
    if (n >= 3 && (unsigned char)buf[0] == 0xef && (unsigned char)buf[1] == 0xbb && (unsigned char)buf[2] == 0xbf)
        memmove(buf, buf + 3, n - 2);
    return buf;
}

/* The station on one line of the list (a, up to b), if it is one. */
static int line_station(const char *a, const char *b, radio_station_t *st)
{
    char line[1536];                 /* (a full line is under 1,200: radio_line) */
    size_t n = (size_t)(b - a);
    if (n >= sizeof line) n = sizeof line - 1;
    memcpy(line, a, n);
    line[n] = '\0';
    for (char *c = line; *c; c++)
        if (*c == '\r') *c = '\0';
    return radio_parse(line, st, 1) == 1;
}

static const char NEW_LIST[] =
    "# Your internet radio stations (Home > Radio), in this order. One per line:\n"
    "#   Name = stream URL\n"
    "# Lines starting with # are notes. Find stations on the H2 adds stations here;\n"
    "# Menu on one in Home > Radio removes it.\n\n";

/* The list's text: the file, or until there's one, a new list with the example's stations. */
static char *list_text(const char *path, const char *example)
{
    char *t = read_text(path);
    if (t) return t;
    t = malloc(LIST_MAX);
    if (!t) return NULL;
    size_t n = (size_t)snprintf(t, LIST_MAX, "%s", NEW_LIST);
    char *ex = read_text(example);
    for (const char *line = ex; line && *line;) {
        const char *end = strchr(line, '\n'), *stop = end ? end : line + strlen(line);
        radio_station_t st;
        if (line_station(line, stop, &st) && n + (size_t)(stop - line) + 2 < LIST_MAX) {
            memcpy(t + n, line, (size_t)(stop - line));
            n += (size_t)(stop - line);
            while (n && t[n - 1] == '\r') n--;
            t[n++] = '\n';
        }
        line = end ? end + 1 : NULL;
    }
    t[n] = '\0';
    free(ex);
    return t;
}

static int write_text(const char *path, const char *text)
{
    char tmp[600];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wbe");
    if (!f) return -1;
    size_t n = strlen(text);
    int ok = fwrite(text, 1, n, f) == n;
    ok = fflush(f) == 0 && ok;
    ok = fsync(fileno(f)) == 0 && ok;
    ok = fclose(f) == 0 && ok;
    if (!ok || file_replace(tmp, path)) { unlink(tmp); return -1; }
    return 0;
}

void radio_line(const radio_station_t *st, char *out, size_t cap)
{
    /* the name as one line that reads back the same: no line breaks, no leading '#' */
    char name[sizeof st->name];
    size_t n = 0;
    for (const char *c = st->name; *c && n + 1 < sizeof name; c++)
        name[n++] = (unsigned char)*c < 0x20 ? ' ' : *c;
    name[n] = '\0';
    /* ...and no "= http://" in it: read back, the line's stream would be the
     * one in the name (a directory's station can be named anything) */
    for (char *c = name; *c; c++) {
        if (*c != '=') continue;
        const char *u = c + 1;
        while (isspace((unsigned char)*u)) u++;
        if (!strncmp(u, "http://", 7) || !strncmp(u, "https://", 8)) *c = '-';
    }
    char *s = name;
    while (*s == '#' || isspace((unsigned char)*s)) s++;
    for (char *e = s + strlen(s); e > s && isspace((unsigned char)e[-1]);) *--e = '\0';
    if (!*s) s = "Station";
    /* (the info before the icon: a build from before icons reads the info to
     * the line's end, and takes "radiofrance:7 | icon: ..." for station 7 still) */
    int len = snprintf(out, cap, "%s = %s", s, st->url);
    if (st->info[0] && len >= 0 && (size_t)len < cap) len += snprintf(out + len, cap - (size_t)len, " | info: %s", st->info);
    if (radio_icon_ok(st->icon) && len >= 0 && (size_t)len < cap) len += snprintf(out + len, cap - (size_t)len, " | icon: %s", st->icon);
    if (radio_icon_ok(st->site) && len >= 0 && (size_t)len < cap) snprintf(out + len, cap - (size_t)len, " | site: %s", st->site);
}

int radio_add(const char *path, const char *example, const radio_station_t *st)
{
    char *t = list_text(path, example);
    if (!t) return -1;
    int count = 0, there = 0;
    for (const char *line = t; *line;) {
        const char *end = strchr(line, '\n'), *stop = end ? end : line + strlen(line);
        radio_station_t s;
        if (line_station(line, stop, &s)) {
            count++;
            if (!strcmp(s.url, st->url)) there = 1;
        }
        if (!end) break;
        line = end + 1;
    }
    char add[1536];
    radio_line(st, add, sizeof add);
    size_t n = strlen(t), k = strlen(add);
    int r;
    if (there) r = 0;
    else if (count >= RADIO_MAX) r = -2;
    else if (n + k + 2 >= LIST_MAX) r = -1;
    else {
        if (n && t[n - 1] != '\n') t[n++] = '\n';
        memcpy(t + n, add, k);
        t[n + k] = '\n';
        t[n + k + 1] = '\0';
        r = write_text(path, t) ? -1 : 1;
    }
    free(t);
    return r;
}

int radio_remove(const char *path, const char *example, const char *url)
{
    char *t = list_text(path, example);
    if (!t) return -1;
    char *w = t;
    int removed = 0;
    for (char *line = t; *line;) {
        char *end = strchr(line, '\n'), *stop = end ? end : line + strlen(line);
        char *next = end ? end + 1 : stop;
        radio_station_t s;
        if (line_station(line, stop, &s) && !strcmp(s.url, url)) {
            removed++;
        } else {
            size_t k = (size_t)(next - line);
            memmove(w, line, k);
            w += k;
        }
        line = next;
    }
    *w = '\0';
    int r = !removed ? 1 : write_text(path, t) ? -1 : 0;
    free(t);
    return r;
}

void radio_split_meta(const char *meta, char *artist, size_t an, char *title, size_t tn)
{
    const char *d = strstr(meta, " - ");
    if (d) {
        trim_copy(artist, an, meta, d);
        trim_copy(title, tn, d + 3, meta + strlen(meta));
        if (!title[0]) { snprintf(title, tn, "%s", artist); artist[0] = '\0'; }
    } else {
        if (an) artist[0] = '\0';
        trim_copy(title, tn, meta, meta + strlen(meta));
    }
}
