/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
#include "rfmeta.h"
#include "core/utf8.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int rf_station(const char *info)
{
    if (!info || strncmp(info, "radiofrance:", 12)) return -1;
    char *end;
    long n = strtol(info + 12, &end, 10);
    return end != info + 12 && n >= 0 && n < 100000 ? (int)n : -1;
}

void rf_url(int n, char *out, size_t cap)
{
    snprintf(out, cap, "https://api.radiofrance.fr/livemeta/pull/%d", n);
}

/* ---- just enough JSON: walk to a value, skip values, read strings ---- */

static const char *ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

static const char *skip_string(const char *p)   /* p at '"': past the closing quote, or NULL */
{
    for (p++; *p; p++) {
        if (*p == '\\') { if (!*++p) return NULL; }
        else if (*p == '"') return p + 1;
    }
    return NULL;
}

static const char *skip_value(const char *p)
{
    p = ws(p);
    if (*p == '"') return skip_string(p);
    if (*p == '{' || *p == '[') {
        int depth = 0;
        while (*p) {
            if (*p == '"') { p = skip_string(p); if (!p) return NULL; continue; }
            if (*p == '{' || *p == '[') depth++;
            else if (*p == '}' || *p == ']') { if (--depth == 0) return p + 1; }
            p++;
        }
        return NULL;
    }
    while (*p && *p != ',' && *p != '}' && *p != ']') p++;   /* number, true, false, null */
    return p;
}

/* Four hex digits' value, or -1 (stops at the first that isn't one: a NUL too) */
static int hex4(const char *p)
{
    int v = 0;
    for (int i = 0; i < 4; i++) {
        char h = p[i];
        int d = h >= '0' && h <= '9' ? h - '0' : h >= 'a' && h <= 'f' ? h - 'a' + 10 : h >= 'A' && h <= 'F' ? h - 'A' + 10 : -1;
        if (d < 0) return -1;
        v = v * 16 + d;
    }
    return v;
}

/* Reads a string value into out (\uXXXX as UTF-8, other escapes as themselves). */
static int read_string(const char *p, char *out, size_t cap)
{
    p = ws(p);
    if (*p != '"' || !cap) return -1;
    size_t n = 0;
    for (p++; *p && *p != '"'; p++) {
        unsigned c = (unsigned char)*p;
        if (c == '\\') {
            p++;
            if (!*p) return -1;
            switch (*p) {
            case 'n': case 'r': case 't': c = ' '; break;
            case 'b': case 'f': continue;
            case 'u': {
                int v = hex4(p + 1);
                if (v < 0) return -1;                  /* (not 4 hex digits: never past the text's end) */
                p += 4;
                if (v >= 0xd800 && v < 0xdc00 && p[1] == '\\' && p[2] == 'u') {   /* surrogate pair */
                    int lo = hex4(p + 3);
                    if (lo >= 0xdc00 && lo < 0xe000) { v = 0x10000 + ((v - 0xd800) << 10) + (lo - 0xdc00); p += 6; }
                }
                char u[4];
                int k = (int)utf8_encode((uint32_t)v, u);
                for (int i = 0; i < k && n + 1 < cap; i++) out[n++] = u[i];
                continue;
            }
            default: c = (unsigned char)*p; break;
            }
        }
        if (n + 1 < cap) out[n++] = (char)c;
    }
    out[n] = '\0';
    return *p == '"' ? 0 : -1;
}

/* In the object at p: the value of `key`, or NULL. */
static const char *member(const char *p, const char *key)
{
    p = ws(p);
    if (*p != '{') return NULL;
    p++;
    size_t kl = strlen(key);
    for (;;) {
        p = ws(p);
        if (*p != '"') return NULL;
        const char *k = p + 1, *e = skip_string(p);
        if (!e) return NULL;
        int match = (size_t)(e - 1 - k) == kl && !strncmp(k, key, kl);
        p = ws(e);
        if (*p != ':') return NULL;
        p = ws(p + 1);
        if (match) return p;
        p = skip_value(p);
        if (!p) return NULL;
        p = ws(p);
        if (*p != ',') return NULL;
        p++;
    }
}

/* In the array at p: element i, or NULL. */
static const char *element(const char *p, int i)
{
    p = ws(p);
    if (*p != '[') return NULL;
    p = ws(p + 1);
    if (*p == ']') return NULL;
    for (int k = 0;; k++) {
        if (k == i) return p;
        p = skip_value(p);
        if (!p) return NULL;
        p = ws(p);
        if (*p != ',') return NULL;
        p = ws(p + 1);
    }
}

/* The step on now (levels[0]'s position among its items), or NULL. */
static const char *step_now(const char *json)
{
    const char *lv = element(member(json, "levels") ? member(json, "levels") : "", 0);
    if (!lv) return NULL;
    const char *pos = member(lv, "position"), *items = member(lv, "items");
    if (!pos || !items) return NULL;
    int i = atoi(pos);
    const char *it = element(items, i);
    char uid[128];
    if (!it || read_string(it, uid, sizeof uid)) return NULL;
    const char *steps = member(json, "steps");
    return steps ? member(steps, uid) : NULL;
}

int rf_parse(const char *json, char *artist, size_t an, char *title, size_t tn, char *visual, size_t vn)
{
    if (visual && vn) visual[0] = '\0';
    const char *step = step_now(json);
    if (!step) return -1;
    const char *t = member(step, "title");
    if (!t || read_string(t, title, tn) || !title[0]) return -1;
    artist[0] = '\0';
    const char *h = member(step, "highlightedArtists");
    const char *h0 = h ? element(h, 0) : NULL;
    if (!h0 || read_string(h0, artist, an) || !artist[0]) {
        const char *a = member(step, "authors");
        if (!a || read_string(a, artist, an)) artist[0] = '\0';
    }
    const char *v = member(step, "visual");
    if (visual && vn && (!v || read_string(v, visual, vn))) visual[0] = '\0';
    /* (it goes to a fetch as it is: only an address on the network, not a
     * file's path or another scheme) */
    if (visual && vn && strncmp(visual, "http://", 7) && strncmp(visual, "https://", 8)) visual[0] = '\0';
    return 0;
}

/* A whole number of seconds at p (a time): 0 if it isn't one. */
static int64_t read_time(const char *p)
{
    if (!p) return 0;
    p = ws(p);
    if (*p < '0' || *p > '9') return 0;
    char *end;
    long long v = strtoll(p, &end, 10);
    return v > 0 && v < 100000000000LL ? (int64_t)v : 0;
}

int rf_step_times(const char *json, int64_t *start, int64_t *end)
{
    *start = *end = 0;
    const char *step = step_now(json);
    if (!step) return -1;
    int64_t s = read_time(member(step, "start")), e = read_time(member(step, "end"));
    if (!s || !e) return -1;
    *start = s;
    *end = e;
    return 0;
}

int rf_next_fetch_s(int64_t now, int64_t start, int64_t end)
{
    /* believable by this clock: begun (give or take), not over, and no longer than a long programme */
    if (start <= 0 || end <= start || end - start > 4 * 3600 || now < start - 30 || now >= end) return RF_POLL_S;
    int64_t in = end + 3 - now;
    return in < RF_POLL_S ? RF_POLL_S : in > RF_POLL_MAX_S ? RF_POLL_MAX_S : (int)in;
}

int rf_retry_s(int fails)
{
    return fails <= 1 ? 5 : fails == 2 ? 15 : fails == 3 ? 60 : RF_POLL_MAX_S;
}
