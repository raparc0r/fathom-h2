/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
#include "library/lyrics.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "core/fsafe.h"
#include "core/hash.h"
#include "core/path.h"
#include "core/utf8.h"
#include "library/cue.h"
#include "library/tags.h"

/* ---------- the text as UTF-8 ---------- */

static int utf8_valid(const unsigned char *b, size_t n)
{
    for (size_t i = 0; i < n;) {
        unsigned c = b[i];
        size_t k = c < 0x80 ? 1 : (c & 0xe0) == 0xc0 ? 2 : (c & 0xf0) == 0xe0 ? 3 : (c & 0xf8) == 0xf0 ? 4 : 0;
        if (!k || i + k > n || (k == 2 && c < 0xc2)) return 0;
        for (size_t j = 1; j < k; j++)
            if ((b[i + j] & 0xc0) != 0x80) return 0;
        i += k;
    }
    return 1;
}

/* in as UTF-8, malloc'd and NUL-ended (its length in *out_len): as it is if
 * it is UTF-8 (a mark at its start dropped), from UTF-16 if it starts with
 * that mark, else its bytes taken as Latin-1. */
static char *to_utf8(const char *in, size_t len, size_t *out_len)
{
    const unsigned char *b = (const unsigned char *)in;
    char *o;
    size_t w = 0;
    if (len >= 2 && ((b[0] == 0xff && b[1] == 0xfe) || (b[0] == 0xfe && b[1] == 0xff))) {
        int le = b[0] == 0xff;
        if (!(o = malloc(len / 2 * 3 + 5))) return NULL;
        for (size_t i = 2; i + 1 < len; i += 2) {
            uint32_t u = le ? (uint32_t)(b[i] | b[i + 1] << 8) : (uint32_t)(b[i] << 8 | b[i + 1]);
            if (u >= 0xd800 && u < 0xdc00 && i + 3 < len) {
                uint32_t lo = le ? (uint32_t)(b[i + 2] | b[i + 3] << 8) : (uint32_t)(b[i + 2] << 8 | b[i + 3]);
                if (lo >= 0xdc00 && lo < 0xe000) { u = 0x10000 + ((u - 0xd800) << 10) + (lo - 0xdc00); i += 2; }
            }
            if (u >= 0xd800 && u < 0xe000) u = 0xfffd;   /* (half a pair) */
            if (u) w += utf8_encode(u, o + w);
        }
    } else if (utf8_valid(b, len)) {
        size_t skip = len >= 3 && b[0] == 0xef && b[1] == 0xbb && b[2] == 0xbf ? 3 : 0;
        if (!(o = malloc(len - skip + 1))) return NULL;
        for (size_t i = skip; i < len; i++)
            if (b[i]) o[w++] = (char)b[i];
    } else {
        if (!(o = malloc(len * 2 + 1))) return NULL;
        for (size_t i = 0; i < len; i++)
            if (b[i]) w += utf8_encode(b[i], o + w);
    }
    o[w] = '\0';
    *out_len = w;
    return o;
}

/* ---------- LRC ---------- */

/* A time at p ("1:23", "01:23.4", "01:23.45", "01:23.456", "01:23:45"), up to
 * the `close` that ends it: its milliseconds and the character after `close`,
 * or NULL (not a time). */
static const char *lrc_time(const char *p, char close, int32_t *ms)
{
    const char *q = p;
    long min = 0, sec = 0, frac = 0;
    int nd = 0, nf = 0;
    while (isdigit((unsigned char)*q) && nd < 4) { min = min * 10 + (*q++ - '0'); nd++; }
    if (!nd || *q != ':') return NULL;
    q++;
    if (!isdigit((unsigned char)q[0]) || !isdigit((unsigned char)q[1])) return NULL;
    sec = (q[0] - '0') * 10 + (q[1] - '0');
    q += 2;
    if (sec > 59) return NULL;
    if (*q == '.' || *q == ':') {
        q++;
        while (isdigit((unsigned char)*q) && nf < 3) { frac = frac * 10 + (*q++ - '0'); nf++; }
        if (!nf) return NULL;
        while (isdigit((unsigned char)*q)) q++;       /* (finer than a millisecond) */
        for (; nf < 3; nf++) frac *= 10;
    }
    if (*q != close) return NULL;
    *ms = (int32_t)(min * 60000 + sec * 1000 + frac);
    return q + 1;
}

/* An LRC's line about itself at p ("[ar:...]", "[offset:+250]"): the character
 * after its ']' (and the offset, when that is what it gives), or NULL. Only
 * the names such files use: "[Chorus: both]" is a line of the song. */
static const char *lrc_about(const char *p, long *offset)
{
    static const char *const NAME[] = {"ar", "ti", "al", "au", "by", "re", "ve", "la", "lr", "id", "length", "offset", "tool", "#", NULL};
    if (*p != '[') return NULL;
    const char *end = strchr(p, ']');
    if (!end) return NULL;
    for (int i = 0; NAME[i]; i++) {
        size_t n = strlen(NAME[i]);
        if (strncasecmp(p + 1, NAME[i], n) || p[1 + n] != ':') continue;
        if (!strcmp(NAME[i], "offset")) *offset = strtol(p + 1 + n + 1, NULL, 10);
        return end + 1;
    }
    return NULL;
}

int lyrics_looks_synced(const char *text, size_t len)
{
    for (size_t i = 0; i + 6 < len; i++) {
        if (text[i] != '[' || (i && text[i - 1] != '\n' && text[i - 1] != '\r' && text[i - 1] != ']')) continue;
        char t[16];
        size_t n = len - i - 1 < sizeof t - 1 ? len - i - 1 : sizeof t - 1;
        memcpy(t, text + i + 1, n);
        t[n] = '\0';
        int32_t ms;
        if (lrc_time(t, ']', &ms)) return 1;
    }
    return 0;
}

typedef struct { int32_t ms; uint32_t at; int order; } ent_t;

static int by_time(const void *a, const void *b)
{
    const ent_t *x = a, *y = b;
    if (x->ms != y->ms) return x->ms < y->ms ? -1 : 1;
    return x->order - y->order;                   /* (as written, where the time is the same) */
}

int lyrics_parse(const char *in, size_t len, lyrics_t *out)
{
    memset(out, 0, sizeof *out);
    if (!in || !len || len > LYRICS_MAX_BYTES) return -1;
    size_t n;
    char *t = to_utf8(in, len, &n);
    if (!t) return -1;
    ent_t *e = malloc(LYRICS_MAX_LINES * sizeof *e);
    /* each line's text is written back over the text itself (never longer),
     * so one buffer holds them all */
    if (!e) { free(t); return -1; }
    int ne = 0, timed = 0;
    long offset = 0;
    size_t w = 0;                                  /* where the next line's text goes in t */
    for (char *line = t, *next; line && ne < LYRICS_MAX_LINES; line = next) {
        char *nl = strchr(line, '\n');
        next = nl ? nl + 1 : NULL;
        if (nl) *nl = '\0';
        size_t ll = strlen(line);
        if (ll && line[ll - 1] == '\r') line[--ll] = '\0';
        const char *p = line;
        int32_t times[8];
        int nt = 0, about = 0;
        for (;;) {
            while (*p == ' ' || *p == '\t') p++;
            int32_t ms;
            const char *q = *p == '[' ? lrc_time(p + 1, ']', &ms) : NULL;
            if (q) {
                if (nt < 8) times[nt++] = ms;
                p = q;
                continue;
            }
            q = lrc_about(p, &offset);
            if (!q) break;
            about = 1;
            p = q;
        }
        /* the line's text: times inside it dropped, spaces at its ends too */
        size_t at = w, start = w;
        for (; *p; p++) {
            int32_t ms;
            const char *q = *p == '<' ? lrc_time(p + 1, '>', &ms) : NULL;
            if (q) { p = q - 1; continue; }
            if (w == start && (*p == ' ' || *p == '\t')) continue;
            t[w++] = *p == '\t' ? ' ' : *p;
        }
        while (w > start && t[w - 1] == ' ') w--;
        int blank = w == start;
        if (about && !nt && blank) continue;        /* (a line about the file, and nothing else) */
        /* (w never passes p: the line's text is no longer than the line, and
         * its NUL goes where the line's own end was, at the latest) */
        t[w++] = '\0';
        if (nt) timed += nt;
        for (int k = 0; k < (nt ? nt : 1) && ne < LYRICS_MAX_LINES; k++) {
            e[ne].ms = nt ? times[k] : -1;
            e[ne].at = (uint32_t)at;
            e[ne].order = ne;
            ne++;
        }
    }
    int synced = timed > 0;
    if (synced) {
        /* a line without a time among timed ones: empty, it is dropped; with
         * words, it is sung with the line before it */
        int k = 0;
        int32_t last = 0;                           /* (the time as written, before the offset) */
        for (int i = 0; i < ne; i++) {
            int32_t raw = e[i].ms;
            if (raw < 0) {
                if (!t[e[i].at]) continue;
                raw = last;
            }
            last = raw;
            long ms = (long)raw - offset;           /* (an offset over 0: the words come sooner) */
            e[i].ms = ms < 0 ? 0 : (int32_t)ms;
            e[k++] = e[i];
        }
        ne = k;
        qsort(e, (size_t)ne, sizeof *e, by_time);
    }
    /* empty lines: none at the start or the end, one at a time between */
    int k = 0, words = 0;
    for (int i = 0; i < ne; i++) {
        int blank = !t[e[i].at];
        if (blank && (k == 0 || !t[e[k - 1].at])) continue;
        if (!blank) words = 1;
        e[k++] = e[i];
    }
    while (k > 0 && !t[e[k - 1].at]) k--;
    ne = k;
    if (!ne || !words || !(out->line = malloc((size_t)ne * sizeof *out->line))) {
        free(e);
        free(t);
        return -1;
    }
    for (int i = 0; i < ne; i++) {
        out->line[i].ms = e[i].ms;
        out->line[i].at = e[i].at;
    }
    free(e);
    out->text = t;
    out->n = ne;
    out->synced = synced;
    return 0;
}

void lyrics_free(lyrics_t *l)
{
    free(l->text);
    free(l->line);
    memset(l, 0, sizeof *l);
}

int lyrics_at(const lyrics_t *l, int32_t ms)
{
    if (!l->synced || !l->n || ms < l->line[0].ms) return -1;
    int lo = 0, hi = l->n - 1;                     /* the last line whose time has come */
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (l->line[mid].ms <= ms) lo = mid;
        else hi = mid - 1;
    }
    return lo;
}

/* ---------- where a song's lyrics are ---------- */

/* A file's bytes (malloc'd, its length in *len), if it has any and no more
 * than LYRICS_MAX_BYTES. */
static char *file_text(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rbe");
    if (!f) return NULL;
    char *b = malloc(LYRICS_MAX_BYTES + 1);
    size_t n = b ? fread(b, 1, LYRICS_MAX_BYTES + 1, f) : 0;
    int bad = ferror(f);
    fclose(f);
    if (!b || !n || n > LYRICS_MAX_BYTES || bad) { free(b); return NULL; }
    *len = n;
    return b;
}

static int from_file(const char *path, lyrics_t *out)
{
    size_t len;
    char *b = file_text(path, &len);
    if (!b) return -1;
    int r = lyrics_parse(b, len, out);
    free(b);
    return r;
}

static void fold(uint64_t *h, const char *s)
{
    for (; s && *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c < 0x80) c = (unsigned char)tolower(c);
        *h = fnv64(*h, &c, 1);
    }
    *h = fnv64(*h, "\x1f", 1);
}

void lyrics_store_path(const char *store, const lyrics_song_t *s, const char *ext, char *out, size_t cap)
{
    uint64_t h = FNV64_START;
    fold(&h, s->artist);
    fold(&h, s->title);
    fold(&h, s->album);
    snprintf(out, cap, "%s/%016llx.%s", store, (unsigned long long)h, ext);
}

int lyrics_store_save(const char *store, const lyrics_song_t *s, const char *text, size_t len)
{
    char path[1100], none[1100];
    if (!store || !store[0] || !len || len > LYRICS_MAX_BYTES) return -1;
    mkdir(store, 0755);
    lyrics_store_path(store, s, "lrc", path, sizeof path);
    if (file_write_whole(path, text, len, NULL, 0)) return -1;
    lyrics_store_path(store, s, "none", none, sizeof none);
    remove(none);
    return 0;
}

int lyrics_store_none(const char *store, const lyrics_song_t *s)
{
    char none[1100];
    if (!store || !store[0]) return -1;
    mkdir(store, 0755);
    lyrics_store_path(store, s, "none", none, sizeof none);
    return file_write_whole(none, "\n", 1, NULL, 0);
}

int lyrics_store_tried(const char *store, const lyrics_song_t *s, int days)
{
    char none[1100];
    struct stat st;
    if (!store || !store[0]) return 0;
    lyrics_store_path(store, s, "none", none, sizeof none);
    if (stat(none, &st)) return 0;
    time_t now = time(NULL);
    /* (a clock not set yet, or set back: the note is from "later", and stands) */
    return now < st.st_mtime || now - st.st_mtime < (time_t)days * 86400;
}

int lyrics_local(const char *store, const lyrics_song_t *s, lyrics_t *out, int *from)
{
    memset(out, 0, sizeof *out);
    char file[1024], lrc[1040];
    const char *of = s->path && s->path[0] ? cue_part_file(s->path, file, sizeof file) : NULL;
    if (of && of == s->path) {                      /* (a file of its own, not a track of an album in one file) */
        const char *ext = path_ext(of);
        size_t stem = *ext ? (size_t)(ext - of) - 1 : strlen(of);
        if (stem + 5 < sizeof lrc) {
            memcpy(lrc, of, stem);
            memcpy(lrc + stem, ".lrc", 5);
            if (from_file(lrc, out) == 0) { if (from) *from = LYRICS_FROM_FILE; return 0; }
        }
        char *text;
        size_t len;
        if (tags_lyrics(of, &text, &len) == 0) {
            int r = lyrics_parse(text, len, out);
            free(text);
            if (r == 0) { if (from) *from = LYRICS_FROM_TAG; return 0; }
        }
    }
    if (store && store[0] && s->title && s->title[0]) {
        lyrics_store_path(store, s, "lrc", lrc, sizeof lrc);
        if (from_file(lrc, out) == 0) { if (from) *from = LYRICS_FROM_STORE; return 0; }
    }
    return -1;
}
