/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "radio.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void trim_copy(char *dst, size_t cap, const char *a, const char *b)
{
    while (a < b && isspace((unsigned char)*a)) a++;
    while (b > a && isspace((unsigned char)b[-1])) b--;
    size_t n = (size_t)(b - a);
    if (n >= cap) n = cap - 1;
    memcpy(dst, a, n);
    dst[n] = '\0';
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
            /* Name = URL [| info: SOURCE]; the name is up to the first '=' that
             * is followed by a URL (names may hold '='; URLs do too). */
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
                if (bar) {
                    const char *i = bar + 1;
                    while (i < stop && isspace((unsigned char)*i)) i++;
                    if (!strncmp(i, "info:", 5)) trim_copy(r->info, sizeof r->info, i + 5, stop);
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
    FILE *f = fopen(path, "rb");
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
