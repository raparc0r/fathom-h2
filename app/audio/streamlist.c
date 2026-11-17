/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Reading a station's playlist (.m3u, .pls, HLS) for its stream's URL: see streamlist.h. */
#include "audio/streamlist.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static int is_http(const char *s, size_t n)
{
    return (n >= 7 && !strncasecmp(s, "http://", 7)) || (n >= 8 && !strncasecmp(s, "https://", 8));
}

int streamlist_looks_like(const char *url)
{
    if (!url || !is_http(url, strlen(url))) return 0;
    size_t n = strcspn(url, "?#");
    return (n > 4 && !strncasecmp(url + n - 4, ".m3u", 4)) || (n > 4 && !strncasecmp(url + n - 4, ".pls", 4)) ||
           (n > 5 && !strncasecmp(url + n - 5, ".m3u8", 5));
}

/* entry (n bytes) as an absolute URL, relative to base: 1 if it's http(s). */
static int resolve(const char *entry, size_t n, const char *base, char *out, size_t cap)
{
    if (!n || cap < 2) return 0;
    if (is_http(entry, n)) {
        if (n >= cap) return 0;
        memcpy(out, entry, n);
        out[n] = '\0';
        return 1;
    }
    if (!base || !is_http(base, strlen(base)) || memchr(entry, ':', n < 12 ? n : 12)) return 0;   /* another scheme */
    const char *host = strstr(base, "://") + 3;
    size_t scheme_end = (size_t)(host - base);                  /* "https://" */
    size_t host_end = scheme_end + strcspn(host, "/?#");        /* "https://host:port" */
    size_t keep;
    if (n >= 2 && entry[0] == '/' && entry[1] == '/') keep = scheme_end - 2;   /* "//host/x": the scheme's */
    else if (entry[0] == '/') keep = host_end;                                  /* "/x": the host's */
    else {                                                                       /* "x": the playlist's folder */
        size_t path_end = strcspn(base, "?#");
        keep = path_end;
        while (keep > host_end && base[keep - 1] != '/') keep--;
    }
    int slash = !(n >= 1 && entry[0] == '/') && keep == host_end;               /* "host" + "/" + "x" */
    if (keep + (size_t)slash + n >= cap) return 0;
    memcpy(out, base, keep);
    if (slash) out[keep++] = '/';
    memcpy(out + keep, entry, n);
    out[keep + n] = '\0';
    return 1;
}

/* The next line of text: [*s, its end) without the line break; 0 at the end. */
static int next_line(const char **s, const char *end, const char **line, size_t *len)
{
    if (*s >= end) return 0;
    const char *p = *s;
    while (p < end && *p != '\n' && *p != '\r') p++;
    *line = *s;
    *len = (size_t)(p - *s);
    while (p < end && (*p == '\n' || *p == '\r')) p++;
    *s = p;
    return 1;
}

static void trim(const char **s, size_t *n)
{
    while (*n && isspace((unsigned char)**s)) { (*s)++; (*n)--; }
    while (*n && isspace((unsigned char)(*s)[*n - 1])) (*n)--;
}

int streamlist_first(const char *text, size_t n, const char *base, char *out, size_t cap)
{
    if (!text || !out || !cap) return STREAMLIST_NONE;
    out[0] = '\0';
    const char *s = text, *end = text + n, *line;
    size_t len;
    if (n >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF) s += 3;
    /* Only text: a stream's bytes are no playlist */
    for (const char *p = s; p < end && p < s + 512; p++)
        if (*p == '\0' || ((unsigned char)*p < 0x20 && *p != '\n' && *p != '\r' && *p != '\t')) return STREAMLIST_NONE;

    const char *first = s;
    size_t first_len = 0;
    while (next_line(&s, end, &line, &len)) {          /* the first line with something on it */
        trim(&line, &len);
        if (len) { first = line; first_len = len; break; }
    }
    s = text + (first - text);
    if (first_len >= 10 && !strncasecmp(first, "[playlist]", 10)) {
        long best = -1;
        while (next_line(&s, end, &line, &len)) {
            trim(&line, &len);
            if (len < 6 || strncasecmp(line, "File", 4) || !isdigit((unsigned char)line[4])) continue;
            char *e;
            long k = strtol(line + 4, &e, 10);
            if (e >= line + len || *e != '=') continue;
            const char *v = e + 1;
            size_t vn = len - (size_t)(v - line);
            trim(&v, &vn);
            char cand[1024];
            if ((best < 0 || k < best) && resolve(v, vn, base, cand, sizeof cand) && strlen(cand) < cap) {
                best = k;
                snprintf(out, cap, "%s", cand);
            }
        }
        return best >= 0 ? STREAMLIST_FOUND : STREAMLIST_NONE;
    }

    /* M3U: HLS if it has segment tags, else the first entry */
    const char *m = s;
    while (next_line(&m, end, &line, &len))
        if (len >= 7 && !strncmp(line, "#EXT-X-", 7)) return STREAMLIST_HLS;
    while (next_line(&s, end, &line, &len)) {
        trim(&line, &len);
        if (!len || line[0] == '#') continue;
        return resolve(line, len, base, out, cap) ? STREAMLIST_FOUND : STREAMLIST_NONE;
    }
    return STREAMLIST_NONE;
}

/* ---------- HLS ---------- */

/* An attribute's value in an HLS tag's list (KEY=VALUE,KEY="a,b",...), without
 * its quotes: 1 if it's there */
static int hls_attr(const char *s, size_t n, const char *key, char *out, size_t cap)
{
    size_t kl = strlen(key), i = 0;
    while (i < n) {
        while (i < n && (s[i] == ',' || s[i] == ' ')) i++;
        size_t k = i;
        while (i < n && s[i] != '=' && s[i] != ',') i++;
        int match = i - k == kl && !strncasecmp(s + k, key, kl);
        if (i >= n || s[i] != '=') continue;
        size_t v = ++i, ve;
        if (i < n && s[i] == '"') {
            v = ++i;
            while (i < n && s[i] != '"') i++;
            ve = i;
            if (i < n) i++;
        } else {
            while (i < n && s[i] != ',') i++;
            ve = i;
        }
        if (match) {
            size_t len = ve - v < cap - 1 ? ve - v : cap - 1;
            memcpy(out, s + v, len);
            out[len] = '\0';
            return 1;
        }
    }
    return 0;
}

enum { HLS_AUDIO = 1, HLS_VIDEO = 2, HLS_UNPLAYABLE = 4 };

/* What a variant holds, from its CODECS (and RESOLUTION): audio we decode
 * (AAC, MP3, FLAC, Opus, ALAC), video, or something we don't (AC-3...) */
static int hls_kind(const char *attrs, size_t n)
{
    char codecs[256], res[32];
    int kind = hls_attr(attrs, n, "RESOLUTION", res, sizeof res) ? HLS_VIDEO : 0;
    if (!hls_attr(attrs, n, "CODECS", codecs, sizeof codecs)) return kind | HLS_AUDIO;   /* unsaid: likely */
    static const char *const audio[] = {"mp4a", "mp3", "flac", "fLaC", "opus", "Opus", "alac", NULL};
    static const char *const video[] = {"avc1", "avc3", "hvc1", "hev1", "vp08", "vp09", "av01", "mp4v", "dvh1", "dvhe", NULL};
    for (char *c = codecs; *c;) {
        while (*c == ',' || *c == ' ') c++;
        size_t len = strcspn(c, ", ");
        if (!len) break;
        int known = 0;
        for (int i = 0; audio[i] && !known; i++)
            if (!strncmp(c, audio[i], strlen(audio[i]))) { kind |= HLS_AUDIO; known = 1; }
        for (int i = 0; video[i] && !known; i++)
            if (!strncmp(c, video[i], strlen(video[i]))) { kind |= HLS_VIDEO; known = 1; }
        if (!known) kind |= HLS_UNPLAYABLE;      /* ac-3, ec-3, subtitles (stpp, wvtt)... */
        c += len;
    }
    return kind;
}

int streamlist_hls_pick(const char *text, size_t n, const char *base, char *out, size_t cap)
{
    if (!text || !out || !cap) return STREAMLIST_NONE;
    out[0] = '\0';
    const char *s = text, *end = text + n, *line;
    size_t len;
    int media = 0, variants = 0;
    /* the best so far: of the audio-only variants the best one; an audio rendition of its own (the default one);
     * of variants with video, the cheapest (its audio is all we play) */
    long a_bw = -1, v_bw = -1;
    char a_url[1024] = "", v_url[1024] = "", r_url[1024] = "";
    int r_default = 0;
    while (next_line(&s, end, &line, &len)) {
        trim(&line, &len);
        if (len >= 18 && !strncmp(line, "#EXT-X-STREAM-INF:", 18)) {
            const char *attrs = line + 18, *u;
            size_t an = len - 18, un;
            char bw_s[24], cand[1024];
            long bw = hls_attr(attrs, an, "BANDWIDTH", bw_s, sizeof bw_s) ? atol(bw_s) : 0;
            int kind = hls_kind(attrs, an);
            do {                                      /* its URI: the next line that isn't a tag */
                if (!next_line(&s, end, &u, &un)) { un = 0; break; }
                trim(&u, &un);
            } while (!un || u[0] == '#');
            if (!un || !resolve(u, un, base, cand, sizeof cand) || strlen(cand) >= cap) continue;
            variants++;
            if (kind & HLS_UNPLAYABLE || !(kind & HLS_AUDIO)) continue;
            if (!(kind & HLS_VIDEO)) {
                if (a_bw < 0 || bw > a_bw) {          /* the best sound: a hi-fi player (AAC at 320 kb/s is ~10 % CPU) */
                    a_bw = bw;
                    snprintf(a_url, sizeof a_url, "%s", cand);
                }
            } else if (v_bw < 0 || bw < v_bw) {
                v_bw = bw;
                snprintf(v_url, sizeof v_url, "%s", cand);
            }
        } else if (len >= 13 && !strncmp(line, "#EXT-X-MEDIA:", 13)) {
            const char *attrs = line + 13;
            size_t an = len - 13;
            char type[16], uri[1024], def[8], cand[1024];
            if (!hls_attr(attrs, an, "TYPE", type, sizeof type) || strcasecmp(type, "AUDIO") ||
                !hls_attr(attrs, an, "URI", uri, sizeof uri))
                continue;
            int dflt = hls_attr(attrs, an, "DEFAULT", def, sizeof def) && !strcasecmp(def, "YES");
            if ((!r_url[0] || (dflt && !r_default)) && resolve(uri, strlen(uri), base, cand, sizeof cand) &&
                strlen(cand) < cap) {
                snprintf(r_url, sizeof r_url, "%s", cand);
                r_default = dflt;
            }
        } else if ((len >= 8 && !strncmp(line, "#EXTINF:", 8)) ||
                   (len >= 22 && !strncmp(line, "#EXT-X-TARGETDURATION:", 22))) {
            media = 1;
        }
    }
    const char *pick = a_url[0] ? a_url : r_url[0] ? r_url : v_url[0] ? v_url : NULL;
    if (pick) {
        snprintf(out, cap, "%s", pick);
        return STREAMLIST_FOUND;
    }
    return media && !variants ? STREAMLIST_HLS : STREAMLIST_NONE;
}
