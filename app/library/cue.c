/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "library/cue.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ---------- text encodings ---------- */

static size_t put_utf8(char *o, uint32_t u)
{
    if (u < 0x80) { o[0] = (char)u; return 1; }
    if (u < 0x800) { o[0] = (char)(0xC0 | u >> 6); o[1] = (char)(0x80 | (u & 0x3F)); return 2; }
    if (u < 0x10000) {
        o[0] = (char)(0xE0 | u >> 12); o[1] = (char)(0x80 | ((u >> 6) & 0x3F)); o[2] = (char)(0x80 | (u & 0x3F));
        return 3;
    }
    o[0] = (char)(0xF0 | u >> 18); o[1] = (char)(0x80 | ((u >> 12) & 0x3F));
    o[2] = (char)(0x80 | ((u >> 6) & 0x3F)); o[3] = (char)(0x80 | (u & 0x3F));
    return 4;
}

/* Well-formed UTF-8 (no overlong forms, surrogates or values past U+10FFFF):
 * a Latin-1 sheet almost never passes by chance. */
static int valid_utf8(const unsigned char *s, size_t n)
{
    for (size_t i = 0; i < n;) {
        unsigned c = s[i];
        if (c < 0x80) { i++; continue; }
        size_t k = c >= 0xC2 && c <= 0xDF ? 1 : c >= 0xE0 && c <= 0xEF ? 2 : c >= 0xF0 && c <= 0xF4 ? 3 : 0;
        if (!k || i + k >= n) return 0;          /* not a lead byte, or cut short */
        unsigned lo = 0x80, hi = 0xBF;
        if (c == 0xE0) lo = 0xA0;
        else if (c == 0xED) hi = 0x9F;
        else if (c == 0xF0) lo = 0x90;
        else if (c == 0xF4) hi = 0x8F;
        if (s[i + 1] < lo || s[i + 1] > hi) return 0;
        for (size_t j = 2; j <= k; j++)
            if ((s[i + j] & 0xC0) != 0x80) return 0;
        i += k + 1;
    }
    return 1;
}

/* Windows-1252's 0x80-0x9F (what most 8-bit sheets are written in: EAC on a
 * Western Windows); the rest is Latin-1. Undefined ones stay as Latin-1's. */
static const uint16_t CP1252[32] = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
    0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178,
};

char *cue_to_utf8(const char *text, size_t len, size_t *out_len)
{
    const unsigned char *s = (const unsigned char *)text;
    char *o;
    size_t w = 0;
    if (len >= 2 && ((s[0] == 0xFF && s[1] == 0xFE) || (s[0] == 0xFE && s[1] == 0xFF))) {   /* UTF-16 */
        int le = s[0] == 0xFF;
        if (!(o = malloc(len / 2 * 3 + 1))) return NULL;
        for (size_t i = 2; i + 1 < len; i += 2) {
            uint32_t u = le ? (uint32_t)(s[i] | s[i + 1] << 8) : (uint32_t)(s[i] << 8 | s[i + 1]);
            if (u >= 0xD800 && u < 0xDC00 && i + 3 < len) {
                uint32_t lo = le ? (uint32_t)(s[i + 2] | s[i + 3] << 8) : (uint32_t)(s[i + 2] << 8 | s[i + 3]);
                if (lo >= 0xDC00 && lo < 0xE000) { u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00); i += 2; }
            }
            if (u >= 0xD800 && u < 0xE000) u = 0xFFFD;   /* an unpaired surrogate */
            if (u == 0) break;
            w += put_utf8(o + w, u);
        }
    } else if (len >= 3 && s[0] == 0xEF && s[1] == 0xBB && s[2] == 0xBF) {   /* UTF-8 with a BOM */
        if (!(o = malloc(len - 2))) return NULL;
        memcpy(o, s + 3, len - 3);
        w = len - 3;
    } else if (valid_utf8(s, len)) {
        if (!(o = malloc(len + 1))) return NULL;
        memcpy(o, s, len);
        w = len;
    } else {                                     /* 8-bit */
        if (!(o = malloc(len * 3 + 1))) return NULL;
        for (size_t i = 0; i < len; i++)
            w += put_utf8(o + w, s[i] >= 0x80 && s[i] < 0xA0 ? CP1252[s[i] - 0x80] : s[i]);
    }
    o[w] = '\0';
    if (out_len) *out_len = w;
    return o;
}

/* ---------- parsing ---------- */

/* `n` bytes of `src` into dst, cut at a character's start if too long. */
static void put_str(char *dst, size_t cap, const char *src, size_t n)
{
    if (n >= cap) {
        n = cap - 1;
        while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80) n--;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* A value: "quoted" (to the line's last quote, so a stray quote inside
 * survives) or the rest of the line. */
static void value(const char *p, const char *end, char *out, size_t cap)
{
    while (p < end && (*p == ' ' || *p == '\t')) p++;
    while (end > p && (end[-1] == ' ' || end[-1] == '\t')) end--;
    if (p < end && *p == '"') {
        const char *q = end;
        while (q > p + 1 && q[-1] != '"') q--;
        if (q > p + 1) { put_str(out, cap, p + 1, (size_t)(q - 1 - (p + 1))); return; }
        p++;
    }
    put_str(out, cap, p, (size_t)(end - p));
}

/* "mm:ss:ff" (minutes may pass 99) in CD frames, or -1. */
static long msf(const char *p, const char *end)
{
    long v[3] = {0, 0, 0};
    for (int k = 0; k < 3; k++) {
        if (p >= end || !isdigit((unsigned char)*p)) return -1;
        while (p < end && isdigit((unsigned char)*p)) {
            v[k] = v[k] * 10 + (*p++ - '0');
            if (v[k] > 100000) return -1;
        }
        if (k < 2) {
            if (p >= end || *p != ':') return -1;
            p++;
        }
    }
    if (v[1] >= 60 || v[2] >= CUE_FPS) return -1;
    return (v[0] * 60 + v[1]) * CUE_FPS + v[2];
}

static const char *word(const char *p, const char *end, const char **wend)
{
    while (p < end && (*p == ' ' || *p == '\t')) p++;
    const char *q = p;
    while (q < end && *q != ' ' && *q != '\t') q++;
    *wend = q;
    return p;
}

static int is_word(const char *w, const char *we, const char *kw)
{
    size_t n = strlen(kw);
    return (size_t)(we - w) == n && !strncasecmp(w, kw, n);
}

/* FILE "name" TYPE, or FILE name with spaces TYPE */
static void file_name(const char *p, const char *end, char *out, size_t cap)
{
    while (end > p && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r')) end--;
    const char *t = end;
    while (t > p && t[-1] != ' ' && t[-1] != '\t') t--;
    static const char *const TYPES[] = {"WAVE", "MP3", "AIFF", "BINARY", "MOTOROLA", "FLAC", "WAV", NULL};
    for (int i = 0; TYPES[i] && t > p; i++)
        if (is_word(t, end, TYPES[i])) { end = t; break; }
    value(p, end, out, cap);
}

static void rg_value(const char *p, const char *end, float *v)
{
    char b[32];
    value(p, end, b, sizeof b);
    *v = (float)strtod(b, NULL);
}

int cue_parse(const char *text, size_t len, cue_sheet_t *out)
{
    memset(out, 0, sizeof *out);
    size_t n;
    char *t = cue_to_utf8(text, len, &n);
    if (!t) return -1;
    int has1[CUE_MAX_TRACKS] = {0}, has0[CUE_MAX_TRACKS] = {0};
    uint32_t idx0[CUE_MAX_TRACKS] = {0};
    int cur_file = -1;
    cue_track_t *tr = NULL;
    for (const char *line = t; line < t + n;) {
        const char *end = memchr(line, '\n', (size_t)(t + n - line));
        if (!end) end = t + n;
        const char *next = end < t + n ? end + 1 : end;
        if (end > line && end[-1] == '\r') end--;
        const char *we, *w = word(line, end, &we);
        const char *rest = we;
        if (is_word(w, we, "FILE")) {
            if (out->nfiles < CUE_MAX_FILES) {
                file_name(rest, end, out->files[out->nfiles], sizeof out->files[0]);
                cur_file = out->nfiles++;
            } else {
                cur_file = -1;                       /* more files than we keep: their tracks are dropped */
            }
        } else if (is_word(w, we, "TRACK")) {
            tr = NULL;
            if (out->n < CUE_MAX_TRACKS) {
                tr = &out->tracks[out->n];
                memset(tr, 0, sizeof *tr);
                tr->number = atoi(rest);
                tr->file = cur_file;
                has0[out->n] = has1[out->n] = 0;
                out->n++;
            }
        } else if (is_word(w, we, "INDEX")) {
            const char *ne, *nw = word(rest, end, &ne);
            const char *te, *tw = word(ne, end, &te);
            long f = msf(tw, te);
            int k = atoi(nw);
            if (tr && f >= 0 && nw < ne) {
                int i = (int)(tr - out->tracks);
                if (k == 1 && !has1[i]) { tr->start = (uint32_t)f; tr->file = cur_file; has1[i] = 1; }
                else if (k == 0) { idx0[i] = (uint32_t)f; has0[i] = 1; if (!has1[i]) tr->file = cur_file; }
            }
        } else if (is_word(w, we, "TITLE")) {
            if (tr) value(rest, end, tr->title, sizeof tr->title);
            else value(rest, end, out->title, sizeof out->title);
        } else if (is_word(w, we, "PERFORMER")) {
            if (tr) value(rest, end, tr->performer, sizeof tr->performer);
            else value(rest, end, out->performer, sizeof out->performer);
        } else if (is_word(w, we, "SONGWRITER")) {
            if (tr) value(rest, end, tr->songwriter, sizeof tr->songwriter);
            else value(rest, end, out->songwriter, sizeof out->songwriter);
        } else if (is_word(w, we, "REM")) {
            const char *ke, *k = word(rest, end, &ke);
            char b[64];
            if (is_word(k, ke, "DATE")) {
                value(ke, end, b, sizeof b);
                const char *d = b;
                while (*d && !isdigit((unsigned char)*d)) d++;
                if (!tr) out->year = atoi(d);
            } else if (is_word(k, ke, "GENRE")) {
                if (!tr) value(ke, end, out->genre, sizeof out->genre);
            } else if (is_word(k, ke, "DISCNUMBER")) {
                value(ke, end, b, sizeof b);
                if (!tr) out->disc = atoi(b);
            } else if (is_word(k, ke, "COMPOSER")) {
                char *dst = tr ? tr->songwriter : out->songwriter;
                if (!*dst) value(ke, end, dst, sizeof out->songwriter);
            } else if (is_word(k, ke, "REPLAYGAIN_TRACK_GAIN")) {
                if (tr) { rg_value(ke, end, &tr->rg_gain); tr->has_rg = 1; }
            } else if (is_word(k, ke, "REPLAYGAIN_TRACK_PEAK")) {
                if (tr) rg_value(ke, end, &tr->rg_peak);
            } else if (is_word(k, ke, "REPLAYGAIN_ALBUM_GAIN")) {
                rg_value(ke, end, &out->rg_album_gain);
                out->has_rg_album = 1;
            } else if (is_word(k, ke, "REPLAYGAIN_ALBUM_PEAK")) {
                rg_value(ke, end, &out->rg_album_peak);
            }
        }
        /* CATALOG, CDTEXTFILE, FLAGS, ISRC, PREGAP, POSTGAP: nothing we show or need */
        line = next;
    }
    free(t);

    /* Keep the tracks that can be played: in a file, with an INDEX 01 (or
     * only an INDEX 00: then that), later than the one before in that file. */
    int kept = 0;
    uint32_t last[CUE_MAX_FILES];
    int any[CUE_MAX_FILES] = {0};
    for (int i = 0; i < out->n; i++) {
        cue_track_t *c = &out->tracks[i];
        if (!has1[i] && has0[i]) { c->start = idx0[i]; has1[i] = 1; }
        if (!has1[i] || c->file < 0) continue;
        if (any[c->file] && c->start <= last[c->file]) continue;
        any[c->file] = 1;
        last[c->file] = c->start;
        if (kept != i) out->tracks[kept] = *c;
        kept++;
    }
    out->n = kept;
    return kept;
}

int cue_read(const char *path, cue_sheet_t *out)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    char *b = malloc(CUE_MAX_BYTES);
    size_t n = b ? fread(b, 1, CUE_MAX_BYTES, f) : 0;
    int r = -1;
    if (b && n > 0 && n < CUE_MAX_BYTES) r = cue_parse(b, n, out);
    free(b);
    fclose(f);
    return r;
}

int cue_supported(const char *path)
{
    const char *dot = strrchr(path, '.');
    return dot && !strcasecmp(dot, ".cue");
}

int cue_file_tracks(const cue_sheet_t *s, int f)
{
    int n = 0;
    for (int i = 0; i < s->n; i++) n += s->tracks[i].file == f;
    return n;
}

/* ---------- a track's path ---------- */

void cue_part_path(char *out, size_t cap, const char *file, int track, uint32_t start, uint32_t end)
{
    if (end) snprintf(out, cap, "%s::%02d@%u-%u", file, track, (unsigned)start, (unsigned)end);
    else snprintf(out, cap, "%s::%02d@%u-", file, track, (unsigned)start);
}

static const char *digits(const char *p, uint32_t *v)
{
    const char *s = p;
    uint64_t x = 0;
    while (isdigit((unsigned char)*p) && x <= 0xFFFFFFFFu) x = x * 10 + (uint64_t)(*p++ - '0');
    if (p == s || x > 0xFFFFFFFFu) return NULL;
    *v = (uint32_t)x;
    return p;
}

/* The suffix's numbers, if `s` (just after "::") is one: "NN@START-END" or "NN@START-". */
static int suffix_parse(const char *s, int *track, uint32_t *start, uint32_t *end)
{
    uint32_t t;
    const char *p = digits(s, &t);
    if (!p || *p != '@' || t == 0 || t > 99) return 0;
    if (!(p = digits(p + 1, start)) || *p != '-') return 0;
    p++;
    *end = 0;
    if (*p && (!(p = digits(p, end)) || *p || *end <= *start)) return 0;
    *track = (int)t;
    return 1;
}

const char *cue_part_suffix(const char *path)
{
    const char *last = NULL;
    for (const char *p = strstr(path, "::"); p; p = strstr(p + 1, "::")) last = p;
    int t;
    uint32_t s, e;
    return last && last > path && suffix_parse(last + 2, &t, &s, &e) ? last : NULL;
}

int cue_part_parse(const char *path, char *file, size_t cap, uint32_t *start, uint32_t *end)
{
    const char *sfx = cue_part_suffix(path);
    int t;
    if (!sfx || (size_t)(sfx - path) >= cap || !suffix_parse(sfx + 2, &t, start, end)) return 0;
    memcpy(file, path, (size_t)(sfx - path));
    file[sfx - path] = '\0';
    return t;
}
