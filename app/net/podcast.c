/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* Podcasts: feeds, OPML, Apple's search and the files on the card (podcast.h). */
#define _GNU_SOURCE   /* memmem */
#include "net/podcast.h"
#include "core/fsafe.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "net/json.h"
#include "net/sha256.h"

#ifdef FATHOM_HAVE_FFMPEG
#include <libavformat/avformat.h>
#include "audio/ff_util.h"
#endif

/* ---------- reading XML, tolerantly ----------
 * Feeds are written by many tools, some carelessly: no tree is built; tags
 * are found as they come, a value is the text up to its closing tag (CDATA
 * and entities decoded, any markup inside dropped, white space collapsed).
 * Nothing reads past `end`. */

typedef struct { const char *p, *end; } xr_t;

/* The next tag: its name (without the prefix's colon removed: "itunes:image")
 * into name, *attrs and *attrs_end around its attributes, *closing for </...>,
 * *empty for <.../>. 0 at the end. Comments, CDATA, <?...?> and <!...> are skipped. */
static int xr_tag(xr_t *x, char *name, size_t cap, const char **attrs, const char **attrs_end, int *closing, int *empty)
{
    for (;;) {
        const char *lt = memchr(x->p, '<', (size_t)(x->end - x->p));
        if (!lt || lt + 1 >= x->end) { x->p = x->end; return 0; }
        x->p = lt + 1;
        if (x->end - x->p >= 3 && !memcmp(x->p, "!--", 3)) {
            const char *e = x->p;
            while (e + 2 < x->end && memcmp(e, "-->", 3)) e++;
            x->p = e + 2 < x->end ? e + 3 : x->end;
            continue;
        }
        if (x->end - x->p >= 8 && !memcmp(x->p, "![CDATA[", 8)) {
            const char *e = x->p;
            while (e + 2 < x->end && memcmp(e, "]]>", 3)) e++;
            x->p = e + 2 < x->end ? e + 3 : x->end;
            continue;
        }
        if (*x->p == '?' || *x->p == '!') {
            const char *gt = memchr(x->p, '>', (size_t)(x->end - x->p));
            x->p = gt ? gt + 1 : x->end;
            continue;
        }
        *closing = *x->p == '/';
        if (*closing) x->p++;
        size_t n = 0;
        while (x->p < x->end && !isspace((unsigned char)*x->p) && *x->p != '>' && *x->p != '/') {
            if (n + 1 < cap) name[n++] = *x->p;
            x->p++;
        }
        name[n] = '\0';
        *attrs = x->p;
        /* the tag's end: a '>' outside quotes */
        char q = 0;
        while (x->p < x->end && (q || *x->p != '>')) {
            if (q) { if (*x->p == q) q = 0; }
            else if (*x->p == '"' || *x->p == '\'') q = *x->p;
            x->p++;
        }
        if (x->p >= x->end) return 0;      /* cut off mid-tag */
        *attrs_end = x->p;
        *empty = x->p > *attrs && x->p[-1] == '/';
        x->p++;
        if (n) return 1;
    }
}

static void put_utf8(char *out, size_t cap, size_t *n, unsigned long c)
{
    char b[4];
    int k;
    if (c < 0x80) { b[0] = (char)c; k = 1; }
    else if (c < 0x800) { b[0] = (char)(0xC0 | c >> 6); b[1] = (char)(0x80 | (c & 0x3F)); k = 2; }
    else if (c < 0x10000) {
        b[0] = (char)(0xE0 | c >> 12); b[1] = (char)(0x80 | ((c >> 6) & 0x3F)); b[2] = (char)(0x80 | (c & 0x3F)); k = 3;
    } else if (c < 0x110000) {
        b[0] = (char)(0xF0 | c >> 18); b[1] = (char)(0x80 | ((c >> 12) & 0x3F));
        b[2] = (char)(0x80 | ((c >> 6) & 0x3F)); b[3] = (char)(0x80 | (c & 0x3F)); k = 4;
    } else return;
    if (*n + (size_t)k < cap) { memcpy(out + *n, b, (size_t)k); *n += (size_t)k; }
}

/* Text with its entities decoded, markup dropped and white space collapsed
 * (to one space, trimmed), cut at cap - 1 bytes on a whole character. */
static void decode(const char *s, const char *end, char *out, size_t cap, int markup)
{
    size_t n = 0;
    int space = 0;
    while (s < end) {
        unsigned long c;
        if (markup && *s == '<') {               /* (a description's HTML, or a CDATA's) */
            if (end - s >= 9 && !memcmp(s, "<![CDATA[", 9)) { s += 9; continue; }
            const char *gt = memchr(s, '>', (size_t)(end - s));
            s = gt ? gt + 1 : end;
            space = 1;
            continue;
        }
        if (markup && end - s >= 3 && !memcmp(s, "]]>", 3)) { s += 3; continue; }
        if (*s == '&') {
            const char *semi = memchr(s, ';', (size_t)(end - s) < 12 ? (size_t)(end - s) : 12);
            if (semi) {
                size_t k = (size_t)(semi - s - 1);
                const char *e = s + 1;
                c = 0;
                if (k >= 2 && e[0] == '#') {
                    c = e[1] == 'x' || e[1] == 'X' ? strtoul(e + 2, NULL, 16) : strtoul(e + 1, NULL, 10);
                } else {
                    static const struct { const char *n; unsigned long c; } ENT[] = {
                        {"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''}, {"nbsp", ' '},
                        {"rsquo", 0x2019}, {"lsquo", 0x2018}, {"rdquo", 0x201D}, {"ldquo", 0x201C},
                        {"ndash", 0x2013}, {"mdash", 0x2014}, {"hellip", 0x2026},
                    };
                    for (size_t i = 0; i < sizeof ENT / sizeof ENT[0]; i++)
                        if (strlen(ENT[i].n) == k && !memcmp(e, ENT[i].n, k)) c = ENT[i].c;
                }
                if (c) {
                    s = semi + 1;
                    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { space = 1; continue; }
                    if (space && n) put_utf8(out, cap, &n, ' ');
                    space = 0;
                    put_utf8(out, cap, &n, c);
                    continue;
                }
            }
        }
        if (isspace((unsigned char)*s)) { space = 1; s++; continue; }
        if (space && n && n + 1 < cap) out[n++] = ' ';
        space = 0;
        if (n + 1 < cap) out[n++] = *s;
        s++;
    }
    /* cut short: no half character at the end */
    size_t lead = n;
    while (lead > 0 && n - lead < 4 && ((unsigned char)out[lead - 1] & 0xC0) == 0x80) lead--;
    if (lead > 0) {
        unsigned char c0 = (unsigned char)out[lead - 1];
        size_t want = c0 >= 0xF0 ? 4 : c0 >= 0xE0 ? 3 : c0 >= 0xC0 ? 2 : 1;
        if (n - (lead - 1) < want) n = lead - 1;
    }
    out[n] = '\0';
}

/* An element's text: from just after its start tag (x->p) to its closing tag;
 * x moves past it. With a CDATA section, its contents as they are. */
static void xr_text(xr_t *x, const char *name, char *out, size_t cap)
{
    char close[72];
    snprintf(close, sizeof close, "</%s", name);
    size_t cl = strlen(close);
    const char *s = x->p, *e = s;
    /* the closing tag, not one inside a CDATA section */
    while (e < x->end) {
        if (x->end - e >= 9 && !memcmp(e, "<![CDATA[", 9)) {
            const char *c = e + 9;
            while (c + 2 < x->end && memcmp(c, "]]>", 3)) c++;
            e = c + 2 < x->end ? c + 3 : x->end;
            continue;
        }
        if ((size_t)(x->end - e) >= cl && !memcmp(e, close, cl)) break;
        e++;
    }
    decode(s, e, out, cap, 1);
    if (e < x->end) {
        const char *gt = memchr(e, '>', (size_t)(x->end - e));
        x->p = gt ? gt + 1 : x->end;
    } else {
        x->p = x->end;
    }
}

/* An attribute's value (decoded), name compared without case: 1, or 0. */
static int xr_attr(const char *a, const char *end, const char *name, char *out, size_t cap)
{
    size_t nl = strlen(name);
    while (a < end) {
        while (a < end && (isspace((unsigned char)*a) || *a == '/')) a++;
        const char *k = a;
        while (a < end && *a != '=' && !isspace((unsigned char)*a)) a++;
        size_t kl = (size_t)(a - k);
        while (a < end && isspace((unsigned char)*a)) a++;
        if (a >= end || *a != '=') { if (a < end) a++; continue; }
        a++;
        while (a < end && isspace((unsigned char)*a)) a++;
        if (a >= end) break;
        char q = *a == '"' || *a == '\'' ? *a++ : 0;
        const char *v = a;
        while (a < end && (q ? *a != q : !isspace((unsigned char)*a))) a++;
        if (kl == nl && !strncasecmp(k, name, nl)) {
            decode(v, a, out, cap, 0);
            return 1;
        }
        if (q && a < end) a++;
    }
    return 0;
}

/* ---------- dates, lengths, names ---------- */

static int64_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

int64_t pod_parse_date(const char *s)
{
    static const char *const MON[] = {"jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec"};
    if (!s) return 0;
    while (*s && (isspace((unsigned char)*s) || isalpha((unsigned char)*s) || *s == ',')) {
        if (isdigit((unsigned char)*s)) break;
        s++;                                         /* the weekday */
    }
    char *e;
    long d = strtol(s, &e, 10);
    if (e == s || d < 1 || d > 31) {
        /* ISO 8601 (some feeds): 2026-11-19T05:00:00Z */
        int y, mo, dd, hh = 0, mi = 0, ss = 0;
        if (sscanf(s, "%4d-%2d-%2dT%2d:%2d:%2d", &y, &mo, &dd, &hh, &mi, &ss) >= 3 && mo >= 1 && mo <= 12)
            return days_from_civil(y, mo, dd) * 86400 + (int64_t)hh * 3600 + mi * 60 + ss;
        return 0;
    }
    s = e;
    while (*s == ' ' || *s == '-') s++;
    int m = -1;
    for (int i = 0; i < 12; i++)
        if (!strncasecmp(s, MON[i], 3)) m = i;
    if (m < 0) return 0;
    while (*s && !isspace((unsigned char)*s) && *s != '-') s++;
    while (*s == ' ' || *s == '-') s++;
    long y = strtol(s, &e, 10);
    if (e == s) return 0;
    if (y < 100) y += y < 70 ? 2000 : 1900;
    if (y > 9999) return 0;
    s = e;
    int hh = 0, mi = 0, ss = 0;
    while (*s == ' ') s++;
    sscanf(s, "%2d:%2d:%2d", &hh, &mi, &ss);
    while (*s && !isspace((unsigned char)*s)) s++;
    while (*s == ' ') s++;
    long off = 0;                                    /* the zone, in minutes east */
    if (*s == '+' || *s == '-') {
        long z = strtol(s + 1, NULL, 10);
        if (z < 0 || z > 2400) z = 0;
        off = (z / 100) * 60 + z % 100;
        if (*s == '-') off = -off;
    } else {
        static const struct { const char *z; int h; } Z[] = {{"EST", -5}, {"EDT", -4}, {"CST", -6}, {"CDT", -5},
                                                             {"MST", -7}, {"MDT", -6}, {"PST", -8}, {"PDT", -7}};
        for (size_t i = 0; i < sizeof Z / sizeof Z[0]; i++)
            if (!strncasecmp(s, Z[i].z, 3)) off = Z[i].h * 60;
    }
    return days_from_civil((int)y, m + 1, (int)d) * 86400 + (int64_t)hh * 3600 + mi * 60 + ss - (int64_t)off * 60;
}

#define POD_MAX_DURATION (INT32_MAX / 1000)   /* seconds: ~24.8 days */

int pod_parse_duration(const char *s)
{
    if (!s) return 0;
    long part[3] = {0, 0, 0};
    int n = 0;
    while (*s == ' ') s++;
    while (*s && n < 3) {
        char *e;
        long v = strtol(s, &e, 10);
        if (e == s) break;
        part[n++] = v;
        s = e;
        if (*s != ':') break;
        s++;
    }
    /* nonsense ("-5:00", 20 digits) isn't a length: 0, and never past what
     * the UI's milliseconds (32 bits) hold */
    for (int i = 0; i < n; i++)
        if (part[i] < 0 || part[i] > POD_MAX_DURATION) return 0;
    int64_t v = n == 1 ? part[0] : n == 2 ? (int64_t)part[0] * 60 + part[1]
              : n == 3 ? (int64_t)part[0] * 3600 + (int64_t)part[1] * 60 + part[2] : 0;
    return v > POD_MAX_DURATION ? 0 : (int)v;
}

void pod_hash(const char *s, char out[13])
{
    sha256_t h;
    uint8_t d[32];
    char hex[65];
    sha256_init(&h);
    sha256_update(&h, s, strlen(s));
    sha256_final(&h, d);
    sha256_hex(d, hex);
    memcpy(out, hex, 12);
    out[12] = '\0';
}

const char *pod_ext(const pod_episode_t *e)
{
    static const struct { const char *t, *x; } BY_TYPE[] = {
        {"audio/mpeg", ".mp3"}, {"audio/mp3", ".mp3"}, {"audio/x-m4a", ".m4a"}, {"audio/mp4", ".m4a"},
        {"audio/m4a", ".m4a"}, {"audio/aac", ".aac"}, {"audio/ogg", ".ogg"}, {"audio/opus", ".opus"},
        {"audio/x-mpeg", ".mp3"}, {"audio/flac", ".flac"},
    };
    for (size_t i = 0; i < sizeof BY_TYPE / sizeof BY_TYPE[0]; i++)
        if (!strcasecmp(e->type, BY_TYPE[i].t)) return BY_TYPE[i].x;
    /* by the address: its path's extension, before any ?query */
    static const char *const EXT[] = {".mp3", ".m4a", ".aac", ".ogg", ".opus", ".flac"};
    const char *q = strchr(e->url, '?');
    size_t n = q ? (size_t)(q - e->url) : strlen(e->url);
    for (size_t i = 0; i < sizeof EXT / sizeof EXT[0]; i++) {
        size_t k = strlen(EXT[i]);
        if (n >= k && !strncasecmp(e->url + n - k, EXT[i], k)) return EXT[i];
    }
    return ".mp3";
}

/* ---------- feeds ---------- */

static int newest_first(const void *a, const void *b)
{
    int64_t x = ((const pod_episode_t *)a)->date, y = ((const pod_episode_t *)b)->date;
    return x < y ? 1 : x > y ? -1 : 0;
}

int pod_feed_complete(const char *xml, size_t len)
{
    static const char *const END[] = {"</rss", "</feed", "</rdf:rdf"};
    /* at the end, not quoted in an episode: in the last 16 KB (some feeds
     * end with a comment or whitespace after it, over a KB) */
    size_t from = len > 16384 ? len - 16384 : 0;
    for (size_t i = from; i < len; i++) {
        if (xml[i] != '<' || i + 1 >= len || xml[i + 1] != '/') continue;
        for (size_t k = 0; k < sizeof END / sizeof END[0]; k++) {
            size_t m = strlen(END[k]);
            if (i + m <= len && !strncasecmp(xml + i, END[k], m)) return 1;
        }
    }
    return 0;
}

int pod_parse_feed(const char *xml, size_t len, pod_show_t *show, pod_episode_t *eps, int max)
{
    xr_t x = {xml, xml + len};
    char name[64];
    const char *a, *ae;
    int closing, empty, in_item = 0, in_image = 0, is_feed = 0, n = 0, total = 0;
    char guid[512] = "";
    pod_episode_t cur;
    /* Feeds are mostly newest first, but not all: up to 4x max are read and
     * the newest kept */
    int cap = max * 4;
    pod_episode_t *all = cap > 0 ? calloc((size_t)cap, sizeof *all) : NULL;
    if (!all && cap > 0) return -1;
    show->title[0] = show->author[0] = show->image[0] = '\0';
    while (xr_tag(&x, name, sizeof name, &a, &ae, &closing, &empty)) {
        if (!closing && (!strcmp(name, "rss") || !strcmp(name, "channel"))) is_feed = 1;
        if (!strcmp(name, "item")) {
            if (!closing && !empty) {
                in_item = 1;
                memset(&cur, 0, sizeof cur);
                guid[0] = '\0';
            } else if (closing && in_item) {
                in_item = 0;
                if (cur.url[0] && cap > 0) {
                    pod_hash(guid[0] ? guid : cur.url, cur.key);
                    if (!cur.title[0]) snprintf(cur.title, sizeof cur.title, "%s", "Episode");
                    if (total < cap) all[total++] = cur;
                    else {                           /* full (an oldest-first feed): over the oldest, if newer */
                        int old = 0;
                        for (int i = 1; i < cap; i++) if (all[i].date < all[old].date) old = i;
                        if (cur.date > all[old].date) all[old] = cur;
                    }
                }
            }
            continue;
        }
        if (closing) {
            if (!strcmp(name, "image")) in_image = 0;
            continue;
        }
        if (in_item) {
            if (!strcmp(name, "title") && !empty) xr_text(&x, name, cur.title, sizeof cur.title);
            else if (!strcmp(name, "guid") && !empty) xr_text(&x, name, guid, sizeof guid);
            else if (!strcmp(name, "pubDate") && !empty) {
                char d[64];
                xr_text(&x, name, d, sizeof d);
                cur.date = pod_parse_date(d);
            } else if (!strcmp(name, "itunes:duration") && !empty) {
                char d[32];
                xr_text(&x, name, d, sizeof d);
                cur.dur_s = pod_parse_duration(d);
            } else if (!strcmp(name, "enclosure") && !cur.url[0]) {
                char v[32];
                xr_attr(a, ae, "url", cur.url, sizeof cur.url);
                xr_attr(a, ae, "type", cur.type, sizeof cur.type);
                if (xr_attr(a, ae, "length", v, sizeof v)) cur.size = strtoll(v, NULL, 10);
                /* video podcasts: not for the H2 */
                if (!strncasecmp(cur.type, "video/", 6)) cur.url[0] = '\0';
            } else if (!strcmp(name, "podcast:chapters")) {
                xr_attr(a, ae, "url", cur.chapters, sizeof cur.chapters);
            }
            continue;
        }
        if (!strcmp(name, "image")) { if (!empty) in_image = 1; continue; }
        if (!strcmp(name, "title") && !empty && !in_image && !show->title[0]) xr_text(&x, name, show->title, sizeof show->title);
        else if (!strcmp(name, "url") && !empty && in_image && !show->image[0]) xr_text(&x, name, show->image, sizeof show->image);
        else if (!strcmp(name, "itunes:image")) {
            char v[384];
            if (xr_attr(a, ae, "href", v, sizeof v)) snprintf(show->image, sizeof show->image, "%s", v);
        } else if (!strcmp(name, "itunes:author") && !empty && !show->author[0]) xr_text(&x, name, show->author, sizeof show->author);
    }
    if (!is_feed) { free(all); return -1; }
    qsort(all, (size_t)total, sizeof *all, newest_first);
    for (int i = 0; i < total; i++) {               /* each episode once (some feeds repeat a guid) */
        int dup = 0;
        for (int j = 0; j < n; j++) dup |= !strcmp(eps[j].key, all[i].key);
        if (!dup && n < max) eps[n++] = all[i];
    }
    free(all);
    show->latest = n ? eps[0].date : 0;
    return n;
}

/* ---------- OPML and search ---------- */

int pod_parse_opml(const char *xml, size_t len, pod_show_t *out, int max)
{
    xr_t x = {xml, xml + len};
    char name[64];
    const char *a, *ae;
    int closing, empty, n = 0;
    while (n < max && xr_tag(&x, name, sizeof name, &a, &ae, &closing, &empty)) {
        if (closing || strcasecmp(name, "outline")) continue;
        pod_show_t s;
        memset(&s, 0, sizeof s);
        if (!xr_attr(a, ae, "xmlUrl", s.url, sizeof s.url) || !s.url[0]) continue;
        if (!xr_attr(a, ae, "text", s.title, sizeof s.title) || !s.title[0]) xr_attr(a, ae, "title", s.title, sizeof s.title);
        if (!s.title[0]) snprintf(s.title, sizeof s.title, "%s", s.url);
        pod_hash(s.url, s.id);
        out[n++] = s;
    }
    return n;
}

void pod_search_url(char *out, size_t cap, const char *term)
{
    char q[256];
    size_t n = 0;
    for (const unsigned char *c = (const unsigned char *)term; *c && n + 4 < sizeof q; c++) {
        if (isalnum(*c) || *c == '-' || *c == '.' || *c == '_') q[n++] = (char)*c;
        else if (*c == ' ') q[n++] = '+';
        else n += (size_t)snprintf(q + n, sizeof q - n, "%%%02X", *c);
    }
    q[n] = '\0';
    snprintf(out, cap, "https://itunes.apple.com/search?media=podcast&entity=podcast&limit=%d&term=%s", POD_SEARCH_MAX, q);
}

static int is_http(const char *s) { return !strncmp(s, "http://", 7) || !strncmp(s, "https://", 8); }

int pod_parse_search(const char *json, size_t len, pod_show_t *out, int max)
{
    json_t root = json_root(json, len), res, r, v;
    if (!json_member(root, "results", &res) || json_type(res) != JSON_ARRAY) return -1;
    size_t at = 0;
    int n = 0;
    while (n < max && json_next(res, &at, &r)) {
        pod_show_t s;
        memset(&s, 0, sizeof s);
        if (!json_member(r, "feedUrl", &v) || json_string(v, s.url, sizeof s.url) <= 0) continue;
        if (json_member(r, "collectionName", &v)) json_string(v, s.title, sizeof s.title);
        if (json_member(r, "artistName", &v)) json_string(v, s.author, sizeof s.author);
        /* its picture, for the list until its feed is read: the 600 px one
         * when Apple gives it (the 100 px one is smaller than Now Playing's) */
        long il = json_member(r, "artworkUrl600", &v) ? json_string(v, s.image, sizeof s.image) : 0;
        if (il <= 0 || il >= (long)sizeof s.image - 1 || !is_http(s.image)) {   /* (none, or cut short) */
            s.image[0] = '\0';
            if (json_member(r, "artworkUrl100", &v)) json_string(v, s.image, sizeof s.image);
        }
        if (json_member(r, "trackCount", &v)) s.episodes = (int)json_int(v, 0);
        if (!s.title[0]) snprintf(s.title, sizeof s.title, "%s", s.url);
        pod_hash(s.url, s.id);
        out[n++] = s;
    }
    return n;
}

int pod_parse_chapters(const char *json, size_t len, pod_chapter_t *out, int max)
{
    json_t root = json_root(json, len), arr, c, v;
    if (!json_member(root, "chapters", &arr) || json_type(arr) != JSON_ARRAY) return 0;
    size_t at = 0;
    int n = 0;
    while (n < max && json_next(arr, &at, &c)) {
        if (!json_member(c, "startTime", &v)) continue;
        /* (a number, often with a fraction: seconds) */
        char num[32] = "";
        size_t k = v.n < sizeof num - 1 ? v.n : sizeof num - 1;
        memcpy(num, v.s, k);
        num[k] = '\0';
        if (json_member(c, "toc", &v) && !json_bool(v, 1)) continue;   /* not for the table of contents */
        double sec = strtod(num[0] == '"' ? num + 1 : num, NULL);
        if (!(sec >= 0 && sec < 4294967.0)) continue;             /* (negative, NaN, past 49 days: not a time) */
        out[n].start_ms = (uint32_t)(sec * 1000);
        out[n].title[0] = '\0';
        if (json_member(c, "title", &v)) json_string(v, out[n].title, sizeof out[n].title);
        n++;
    }
    return n;
}

int pod_file_chapters(const char *path, pod_chapter_t *out, int max)
{
#ifdef FATHOM_HAVE_FFMPEG
    AVFormatContext *fc = NULL;
    ff_mem_guard();                    /* (a long episode's table of frames isn't taken if it won't fit) */
    if (avformat_open_input(&fc, path, NULL, NULL) < 0) return 0;   /* (the header only: chapters come with it) */
    int n = 0;
    for (unsigned i = 0; i < fc->nb_chapters && n < max; i++) {
        const AVChapter *c = fc->chapters[i];
        int64_t ms = av_rescale_q(c->start, c->time_base, (AVRational){1, 1000});
        out[n].start_ms = ms > 0 ? (uint32_t)ms : 0;
        const AVDictionaryEntry *t = av_dict_get(c->metadata, "title", NULL, 0);
        snprintf(out[n].title, sizeof out[n].title, "%s", t ? t->value : "");
        n++;
    }
    avformat_close_input(&fc);
    return n;
#else
    (void)path; (void)out; (void)max;
    return 0;
#endif
}

/* ---------- the card ---------- */

/* path written whole: beside, then moved over */
static int write_file(const char *path, const char *text, size_t len)
{
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "we");
    if (!f) return -1;
    int ok = fwrite(text, 1, len, f) == len && fflush(f) == 0;
    ok &= fsync(fileno(f)) == 0;
    ok &= fclose(f) == 0;
    if (!ok || file_replace(tmp, path) != 0) { unlink(tmp); return -1; }
    return 0;
}

static char *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rbe");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0 || n > 4 * 1024 * 1024) { fclose(f); return NULL; }
    char *b = malloc((size_t)n + 1);
    if (!b) { fclose(f); return NULL; }
    size_t got = fread(b, 1, (size_t)n, f);
    fclose(f);
    b[got] = '\0';
    if (len) *len = got;
    return b;
}

/* a value on one line: tabs and line ends become spaces */
static void one_line(char *dst, size_t cap, const char *s)
{
    size_t n = 0;
    for (; *s && n + 1 < cap; s++) dst[n++] = *s == '\t' || *s == '\n' || *s == '\r' ? ' ' : *s;
    dst[n] = '\0';
}

static void show_dir(const char *dir, const char *id, char *out, size_t cap)
{
    snprintf(out, cap, "%s/%s", dir, id);
}

static void show_details(const char *dir, pod_show_t *s)
{
    char path[700];
    snprintf(path, sizeof path, "%s/%s/seen.txt", dir, s->id);
    char *seen = read_file(path, NULL);
    if (seen) { s->seen = strtoll(seen, NULL, 10); free(seen); }
    snprintf(path, sizeof path, "%s/%s/show.txt", dir, s->id);
    char *t = read_file(path, NULL);
    if (!t) return;
    for (char *save = NULL, *l = strtok_r(t, "\n", &save); l; l = strtok_r(NULL, "\n", &save)) {
        char *eq = strchr(l, '=');
        if (!eq) continue;
        *eq = '\0';
        const char *v = eq + 1;
        if (!strcmp(l, "author")) snprintf(s->author, sizeof s->author, "%s", v);
        else if (!strcmp(l, "image")) snprintf(s->image, sizeof s->image, "%s", v);
        else if (!strcmp(l, "refreshed")) s->refreshed = strtoll(v, NULL, 10);
        else if (!strcmp(l, "seen") && !s->seen) s->seen = strtoll(v, NULL, 10);
        else if (!strcmp(l, "latest")) s->latest = strtoll(v, NULL, 10);
        else if (!strcmp(l, "title") && !s->title[0]) snprintf(s->title, sizeof s->title, "%s", v);
    }
    free(t);
}

int pod_shows_load(const char *dir, pod_show_t *out, int max)
{
    char path[700];
    snprintf(path, sizeof path, "%s/shows.txt", dir);
    char *t = read_file(path, NULL);
    if (!t) return 0;
    int n = 0;
    for (char *save = NULL, *l = strtok_r(t, "\n", &save); l && n < max; l = strtok_r(NULL, "\n", &save)) {
        while (*l == ' ' || *l == '\t') l++;
        if (*l == '#' || !*l) continue;
        /* "Title = URL": the last " = " before an address (titles can have
         * one, or be the address itself: an OPML outline without a name); a
         * hand-written "Title=URL" at an '=' before the line's first address;
         * else a bare address from its start (an address has no spaces, but
         * can have "=http" in its query) */
        char *h1 = strstr(l, "http://"), *h2 = strstr(l, "https://");
        char *first = !h1 ? h2 : !h2 ? h1 : h1 < h2 ? h1 : h2;
        if (!first) continue;
        char *u = NULL;
        for (char *q = strstr(l, " = "); q; q = strstr(q + 1, " = "))
            if (is_http(q + 3)) u = q + 3;
        for (char *q = strchr(l, '='); !u && q && q < first; q = strchr(q + 1, '=')) {
            char *v = q + 1;
            while (*v == ' ' || *v == '\t') v++;
            if (is_http(v)) u = v;
        }
        if (!u) u = first;
        pod_show_t s;
        memset(&s, 0, sizeof s);
        char *e = u + strlen(u);
        while (e > u && isspace((unsigned char)e[-1])) *--e = '\0';
        snprintf(s.url, sizeof s.url, "%s", u);
        char *te = u;
        while (te > l && (te[-1] == ' ' || te[-1] == '=' || te[-1] == '\t')) te--;
        snprintf(s.title, sizeof s.title, "%.*s", (int)(te - l), l);
        int dup = 0;
        for (int i = 0; i < n; i++) dup |= !strcmp(out[i].url, s.url);
        if (dup) continue;
        pod_hash(s.url, s.id);
        /* Before 0.13 the line's first http:// was taken for the address (an
         * address with another inside it: the inner one) and the show's
         * folder named after that: kept if it's the one there */
        const char *old = h1 ? h1 : h2;
        if (strcmp(old, s.url)) {
            char oid[13], od[700], nd[700];
            struct stat sb;
            pod_hash(old, oid);
            show_dir(dir, oid, od, sizeof od);
            show_dir(dir, s.id, nd, sizeof nd);
            if (stat(nd, &sb) != 0 && stat(od, &sb) == 0) memcpy(s.id, oid, sizeof oid);
        }
        show_details(dir, &s);
        if (!s.title[0]) snprintf(s.title, sizeof s.title, "%s", s.url);
        out[n++] = s;
    }
    free(t);
    return n;
}

int pod_shows_add(const char *dir, const pod_show_t *s)
{
    pod_show_t *have = calloc(POD_MAX_SHOWS, sizeof *have);
    if (!have) return -1;
    int n = pod_shows_load(dir, have, POD_MAX_SHOWS);
    for (int i = 0; i < n; i++)
        if (!strcmp(have[i].url, s->url)) { free(have); return 0; }
    free(have);
    if (n >= POD_MAX_SHOWS) return -2;
    mkdir(dir, 0755);
    char path[700];
    snprintf(path, sizeof path, "%s/shows.txt", dir);
    size_t len = 0;
    char *t = read_file(path, &len);
    if (!t) {
        t = strdup("# Podcasts: one show a line, Title = the feed's address (Fathom adds them here)\n");
        if (!t) return -1;
        len = strlen(t);
    }
    char title[128], line[700];
    one_line(title, sizeof title, s->title[0] ? s->title : "Podcast");
    for (char *c = title; *c; c++) if (*c == '#') *c = ' ';
    int ln = snprintf(line, sizeof line, "%s%s = %s\n", len && t[len - 1] != '\n' ? "\n" : "", title, s->url);
    char *all = malloc(len + (size_t)ln + 1);
    if (!all) { free(t); return -1; }
    memcpy(all, t, len);
    memcpy(all + len, line, (size_t)ln + 1);
    free(t);
    int r = write_file(path, all, len + (size_t)ln);
    free(all);
    if (r) return -1;
    pod_show_t copy = *s;
    pod_hash(s->url, copy.id);
    pod_show_save(dir, &copy);
    return 1;
}

static void remove_folder(const char *path)
{
    DIR *d = opendir(path);
    struct dirent *e;
    char f[1200];
    while (d && (e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(f, sizeof f, "%s/%s", path, e->d_name);
        unlink(f);
    }
    if (d) closedir(d);
    rmdir(path);
}

int pod_shows_remove(const char *dir, const char *url)
{
    char id[13];                                     /* its folder: as loaded (an older show's may be named otherwise) */
    pod_hash(url, id);
    pod_show_t *have = calloc(POD_MAX_SHOWS, sizeof *have);
    int nh = have ? pod_shows_load(dir, have, POD_MAX_SHOWS) : 0;
    for (int i = 0; i < nh; i++)
        if (!strcmp(have[i].url, url)) memcpy(id, have[i].id, sizeof id);
    free(have);
    char path[700];
    snprintf(path, sizeof path, "%s/shows.txt", dir);
    size_t len;
    char *t = read_file(path, &len);
    if (!t) return -1;
    char *out = malloc(len + 1);
    if (!out) { free(t); return -1; }
    size_t w = 0;
    int found = 0;
    for (char *l = t; *l;) {
        char *nl = strchr(l, '\n');
        size_t ll = nl ? (size_t)(nl - l + 1) : strlen(l);
        /* this line's address is url (to its end, but for trailing spaces) */
        int match = 0;
        char *u = memmem(l, ll, url, strlen(url));
        if (u && *l != '#') {
            const char *after = u + strlen(url);
            while (after < l + ll && (*after == ' ' || *after == '\r' || *after == '\t' || *after == '\n')) after++;
            match = after == l + ll;
        }
        if (match) found = 1;
        else { memcpy(out + w, l, ll); w += ll; }
        l += ll;
    }
    int r = found ? write_file(path, out, w) : 0;
    free(out);
    free(t);
    if (r) return -1;
    char sd[700];
    show_dir(dir, id, sd, sizeof sd);
    remove_folder(sd);
    return 0;
}

int pod_show_save(const char *dir, const pod_show_t *s)
{
    char sd[700], path[760], t[1400], title[128], author[96], image[384];
    show_dir(dir, s->id, sd, sizeof sd);
    mkdir(dir, 0755);
    mkdir(sd, 0755);
    snprintf(path, sizeof path, "%s/show.txt", sd);
    one_line(title, sizeof title, s->title);
    one_line(author, sizeof author, s->author);
    one_line(image, sizeof image, s->image);
    int n = snprintf(t, sizeof t, "title=%s\nurl=%s\nauthor=%s\nimage=%s\nrefreshed=%lld\nlatest=%lld\n",
                     title, s->url, author, image, (long long)s->refreshed, (long long)s->latest);
    return write_file(path, t, (size_t)(n < (int)sizeof t ? n : (int)sizeof t - 1));
}

int pod_show_seen(const char *dir, const char *id, int64_t seen)
{
    char sd[700], path[760], t[32];
    show_dir(dir, id, sd, sizeof sd);
    mkdir(dir, 0755);
    mkdir(sd, 0755);
    snprintf(path, sizeof path, "%s/seen.txt", sd);
    int n = snprintf(t, sizeof t, "%lld\n", (long long)seen);
    return write_file(path, t, (size_t)n);
}

int pod_episodes_save(const char *dir, const char *id, const pod_episode_t *eps, int n)
{
    char sd[700], path[760];
    show_dir(dir, id, sd, sizeof sd);
    mkdir(dir, 0755);
    mkdir(sd, 0755);
    snprintf(path, sizeof path, "%s/episodes.txt", sd);
    size_t cap = (size_t)(n + 1) * 1200, w = 0;
    char *t = malloc(cap);
    if (!t) return -1;
    for (int i = 0; i < n; i++) {
        char title[160], url[640], type[32], ch[256];
        one_line(title, sizeof title, eps[i].title);
        one_line(url, sizeof url, eps[i].url);
        one_line(type, sizeof type, eps[i].type);
        one_line(ch, sizeof ch, eps[i].chapters);
        w += (size_t)snprintf(t + w, cap - w, "%s\t%lld\t%d\t%lld\t%s\t%s\t%s\t%s\n", eps[i].key, (long long)eps[i].date,
                              eps[i].dur_s, (long long)eps[i].size, type, url, ch, title);
        if (w >= cap) { w = cap - 1; break; }
    }
    int r = write_file(path, t, w);
    free(t);
    return r;
}

/* the next tab-separated field of a line (moving *p past it) */
static char *field(char **p)
{
    char *s = *p;
    if (!s) return (char *)"";
    char *tab = strchr(s, '\t');
    if (tab) { *tab = '\0'; *p = tab + 1; }
    else *p = NULL;
    return s;
}

int pod_episodes_load(const char *dir, const char *id, pod_episode_t *eps, int max)
{
    char path[760];
    snprintf(path, sizeof path, "%s/%s/episodes.txt", dir, id);
    char *t = read_file(path, NULL);
    if (!t) return 0;
    int n = 0;
    for (char *save = NULL, *l = strtok_r(t, "\n", &save); l && n < max; l = strtok_r(NULL, "\n", &save)) {
        pod_episode_t e;
        memset(&e, 0, sizeof e);
        char *p = l;
        snprintf(e.key, sizeof e.key, "%s", field(&p));
        e.date = strtoll(field(&p), NULL, 10);
        e.dur_s = atoi(field(&p));
        e.size = strtoll(field(&p), NULL, 10);
        snprintf(e.type, sizeof e.type, "%s", field(&p));
        snprintf(e.url, sizeof e.url, "%s", field(&p));
        snprintf(e.chapters, sizeof e.chapters, "%s", field(&p));
        snprintf(e.title, sizeof e.title, "%s", p ? p : "");
        /* (the key names files: twelve hex digits, nothing else, from a file
         * that may have been edited by hand) */
        int hex = strlen(e.key) == 12;
        for (int k = 0; hex && k < 12; k++) hex = isxdigit((unsigned char)e.key[k]) != 0;
        if (hex && e.url[0]) eps[n++] = e;
    }
    free(t);
    return n;
}

int pod_state_load(const char *dir, const char *id, pod_state_t *st, int max)
{
    char path[760];
    snprintf(path, sizeof path, "%s/%s/state.txt", dir, id);
    char *t = read_file(path, NULL);
    if (!t) return 0;
    int n = 0;
    for (char *save = NULL, *l = strtok_r(t, "\n", &save); l && n < max; l = strtok_r(NULL, "\n", &save)) {
        pod_state_t s;
        memset(&s, 0, sizeof s);
        char *p = l;
        snprintf(s.key, sizeof s.key, "%s", field(&p));
        s.pos_ms = (uint32_t)strtoul(field(&p), NULL, 10);
        s.len_ms = (uint32_t)strtoul(field(&p), NULL, 10);
        s.played = atoi(field(&p));
        s.played_at = strtoll(field(&p), NULL, 10);
        if (strlen(s.key) == 12) st[n++] = s;
    }
    free(t);
    return n;
}

#define POD_MAX_STATES 400   /* kept per show (an old episode's goes once it's long gone) */

int pod_state_put(const char *dir, const char *id, const pod_state_t *s)
{
    pod_state_t *st = calloc(POD_MAX_STATES, sizeof *st);
    if (!st) return -1;
    int n = pod_state_load(dir, id, st, POD_MAX_STATES), i;
    for (i = 0; i < n && strcmp(st[i].key, s->key); i++) {}
    if (i < n) st[i] = *s;
    else if (n < POD_MAX_STATES) st[n++] = *s;
    else { memmove(st, st + 1, (size_t)(n - 1) * sizeof *st); st[n - 1] = *s; }   /* the oldest goes */
    char sd[700], path[760];
    show_dir(dir, id, sd, sizeof sd);
    struct stat sb;
    if (stat(sd, &sb)) { free(st); return -1; }   /* unsubscribed (its folder gone): not made again for this */
    snprintf(path, sizeof path, "%s/state.txt", sd);
    enum { LINE = 12 + 1 + 10 + 1 + 10 + 1 + 11 + 1 + 20 + 1 };   /* the longest (numbers read back from a damaged file) */
    size_t cap = (size_t)n * LINE + 1, w = 0;
    char *t = malloc(cap);
    if (!t) { free(st); return -1; }
    for (int k = 0; k < n; k++) {
        if (!st[k].pos_ms && !st[k].played) continue;          /* nothing to keep */
        int l = snprintf(t + w, cap - w, "%s\t%u\t%u\t%d\t%lld\n", st[k].key, st[k].pos_ms, st[k].len_ms,
                         st[k].played, (long long)st[k].played_at);
        if (l < 0 || (size_t)l >= cap - w) break;
        w += (size_t)l;
    }
    int r = write_file(path, t, w);
    free(t);
    free(st);
    return r;
}

void pod_state_find(const pod_state_t *st, int n, const char *key, pod_state_t *out)
{
    for (int i = 0; i < n; i++)
        if (!strcmp(st[i].key, key)) { *out = st[i]; return; }
    memset(out, 0, sizeof *out);
    snprintf(out->key, sizeof out->key, "%s", key);
}

void pod_chapters_path(const char *dir, const char *id, const char *key, char *out, size_t cap)
{
    snprintf(out, cap, "%s/%s/%s.json", dir, id, key);
}

void pod_cover_path(const char *dir, const char *id, char *out, size_t cap)
{
    snprintf(out, cap, "%s/%s/cover.565", dir, id);
}

void pod_thumb_path(const char *dir, const char *id, char *out, size_t cap)
{
    snprintf(out, cap, "%s/%s/thumb.565", dir, id);
}

int pod_chapters(const char *dir, const char *id, const pod_episode_t *e, pod_chapter_t *out, int max)
{
    char p[800];
    size_t len = 0;
    pod_chapters_path(dir, id, e->key, p, sizeof p);
    char *t = read_file(p, &len);
    int n = t ? pod_parse_chapters(t, len, out, max) : 0;
    free(t);
    if (n > 0) return n;
    pod_episode_path(dir, id, e, p, sizeof p);
    return pod_file_chapters(p, out, max);
}

void pod_episode_path(const char *dir, const char *id, const pod_episode_t *e, char *out, size_t cap)
{
    snprintf(out, cap, "%s/%s/%s%s", dir, id, e->key, pod_ext(e));
}

int pod_downloaded(const char *dir, const char *id, const pod_episode_t *e)
{
    char p[800];
    struct stat st;
    pod_episode_path(dir, id, e, p, sizeof p);
    return stat(p, &st) == 0 && st.st_size > 0;
}

int pod_delete_download(const char *dir, const char *id, const pod_episode_t *e)
{
    char p[800], part[820];
    pod_chapters_path(dir, id, e->key, p, sizeof p);
    unlink(p);
    pod_episode_path(dir, id, e, p, sizeof p);
    snprintf(part, sizeof part, "%s.part", p);
    unlink(part);
    return unlink(p);
}
