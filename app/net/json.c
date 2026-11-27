/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* Reading JSON in place (json.h). */
#include "net/json.h"
#include "core/utf8.h"

#include <stdlib.h>
#include <string.h>

static const char *ws(const char *p, const char *end)
{
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    return p;
}

/* The end of the value starting at p (one past it), or NULL. */
static const char *skip(const char *p, const char *end, int depth)
{
    p = ws(p, end);
    if (p >= end || depth > 64) return NULL;
    if (*p == '"') {
        for (p++; p < end; p++) {
            if (*p == '\\') { p++; continue; }
            if (*p == '"') return p + 1;
        }
        return NULL;
    }
    if (*p == '{' || *p == '[') {
        char close = *p == '{' ? '}' : ']';
        p = ws(p + 1, end);
        if (p < end && *p == close) return p + 1;
        for (;;) {
            if (close == '}') {                     /* "name": */
                if (p >= end || *p != '"' || !(p = skip(p, end, depth + 1))) return NULL;
                p = ws(p, end);
                if (p >= end || *p != ':') return NULL;
                p++;
            }
            if (!(p = skip(p, end, depth + 1))) return NULL;
            p = ws(p, end);
            if (p >= end) return NULL;
            if (*p == close) return p + 1;
            if (*p != ',') return NULL;
            p = ws(p + 1, end);
        }
    }
    /* number, true, false, null */
    const char *q = p;
    while (q < end && (strchr("+-.0123456789eE", *q) || (*q >= 'a' && *q <= 'z'))) q++;
    return q > p ? q : NULL;
}

json_t json_root(const char *text, size_t len)
{
    json_t v = {NULL, 0};
    if (!text) return v;
    const char *end = text + len, *p = ws(text, end), *q = skip(p, end, 0);
    if (q) { v.s = p; v.n = (size_t)(q - p); }
    return v;
}

json_type_t json_type(json_t v)
{
    if (!v.s || !v.n) return JSON_NONE;
    switch (v.s[0]) {
    case '{': return JSON_OBJECT;
    case '[': return JSON_ARRAY;
    case '"': return JSON_STRING;
    case 't': case 'f': return JSON_BOOL;
    case 'n': return JSON_NULL;
    default: return JSON_NUMBER;
    }
}

/* Inside an object: the member at p: its name's span and value. */
static int member_at(const char **pp, const char *end, json_t *name, json_t *val)
{
    const char *p = ws(*pp, end);
    if (p >= end || *p != '"') return 0;
    const char *q = skip(p, end, 1);
    if (!q) return 0;
    name->s = p; name->n = (size_t)(q - p);
    p = ws(q, end);
    if (p >= end || *p != ':') return 0;
    p = ws(p + 1, end);
    q = skip(p, end, 1);
    if (!q) return 0;
    val->s = p; val->n = (size_t)(q - p);
    p = ws(q, end);
    if (p < end && *p == ',') p++;
    *pp = p;
    return 1;
}

int json_member(json_t obj, const char *name, json_t *out)
{
    if (json_type(obj) != JSON_OBJECT) return 0;
    const char *p = obj.s + 1, *end = obj.s + obj.n - 1;
    size_t len = strlen(name);
    json_t k, v;
    while (member_at(&p, end, &k, &v)) {
        /* names with escapes aren't looked up by these callers: compare raw */
        if (k.n == len + 2 && !memcmp(k.s + 1, name, len)) { *out = v; return 1; }
    }
    return 0;
}

int json_next(json_t arr, size_t *at, json_t *out)
{
    json_type_t t = json_type(arr);
    if (t != JSON_ARRAY && t != JSON_OBJECT) return 0;
    const char *end = arr.s + arr.n - 1, *p = arr.s + 1 + *at;
    if (p >= end) return 0;
    if (t == JSON_OBJECT) {
        json_t k;
        if (!member_at(&p, end, &k, out)) return 0;
    } else {
        p = ws(p, end);
        if (p >= end) return 0;
        const char *q = skip(p, end, 1);
        if (!q) return 0;
        out->s = p; out->n = (size_t)(q - p);
        p = ws(q, end);
        if (p < end && *p == ',') p++;
    }
    *at = (size_t)(p - (arr.s + 1));
    return 1;
}

static int hex4(const char *p, unsigned *u)
{
    *u = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0) return -1;
        *u = *u << 4 | (unsigned)d;
    }
    return 0;
}

long json_string(json_t v, char *out, size_t cap)
{
    if (json_type(v) != JSON_STRING || !out || !cap) { if (out && cap) out[0] = '\0'; return -1; }
    const char *p = v.s + 1, *end = v.s + v.n - 1;
    size_t n = 0;
    while (p < end) {
        char ch[4];
        size_t k = 1;
        if (*p != '\\') { ch[0] = *p++; }
        else {
            p++;
            if (p >= end) break;
            char e = *p++;
            switch (e) {
            case 'n': ch[0] = '\n'; break;
            case 't': ch[0] = '\t'; break;
            case 'r': ch[0] = '\r'; break;
            case 'b': ch[0] = '\b'; break;
            case 'f': ch[0] = '\f'; break;
            case 'u': {
                unsigned u, lo;
                if (end - p < 4 || hex4(p, &u)) { ch[0] = '?'; break; }
                p += 4;
                if (u >= 0xD800 && u < 0xDC00 && end - p >= 6 && p[0] == '\\' && p[1] == 'u' && !hex4(p + 2, &lo) &&
                    lo >= 0xDC00 && lo < 0xE000) {
                    u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
                    p += 6;
                }
                if (u >= 0xD800 && u < 0xE000) u = 0xFFFD;
                k = utf8_encode(u, ch);
                break;
            }
            default: ch[0] = e;                      /* \" \\ \/ */
            }
        }
        if (n + k + 1 > cap) break;                 /* never half a character */
        memcpy(out + n, ch, k);
        n += k;
    }
    out[n] = '\0';
    return (long)n;
}

long long json_int(json_t v, long long def)
{
    char buf[32];
    json_type_t t = json_type(v);
    if (t == JSON_STRING) {
        if (json_string(v, buf, sizeof buf) < 1) return def;
    } else if (t == JSON_NUMBER && v.n < sizeof buf) {
        memcpy(buf, v.s, v.n);
        buf[v.n] = '\0';
    } else {
        return def;
    }
    char *e;
    long long x = strtoll(buf, &e, 10);
    return e == buf ? def : x;
}

int json_bool(json_t v, int def)
{
    if (json_type(v) == JSON_BOOL) return v.s[0] == 't';
    if (json_type(v) == JSON_NUMBER) return json_int(v, def) != 0;
    return def;
}
