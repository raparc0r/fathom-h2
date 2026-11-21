/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Covers found online: MusicBrainz and the Cover Art Archive, then Apple (coverfind.h). */
#include "net/coverfind.h"

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "library/keys.h"   /* key_is_various */
#include "net/json.h"

#define CF_MAX 8                     /* matches a find shows */
#define ANSWER_MAX (256 * 1024)      /* a search's answer */
#define PICTURE_MAX (4 * 1024 * 1024)

/* ---------- comparing names ---------- */

/* U+00C0..U+00FF and U+0100..U+017F as plain letters: 3 = "ae", 4 = "th",
 * 5 = "ss", 1 = "ij", 2 = "oe", a space for x and division */
static const char LATIN1[] = "aaaaaa3ceeeeiiiidnooooo ouuuuy45aaaaaa3ceeeeiiiidnooooo ouuuuy4y";
static const char LATIN_A[] = "aaaaaa" "cccccccc" "dddd" "eeeeeeeeee" "gggggggg" "hhhh" "iiiiiiiiii" "11" "jj"
                              "kkk" "llllllllll" "nnnnnnnnn" "oooooo" "22" "rrrrrr" "ssssssss" "tttttt"
                              "uuuuuuuuuuuu" "ww" "yyy" "zzzzzz" "s";
_Static_assert(sizeof LATIN1 == 64 + 1 && sizeof LATIN_A == 128 + 1, "a letter for each code point");

/* The next character of s (UTF-8): its code point, *len its bytes (a stray byte: itself) */
static unsigned utf8_next(const unsigned char *s, int *len)
{
    unsigned c = s[0];
    if (c < 0x80) { *len = 1; return c; }
    int n = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
    for (int i = 1; i < n; i++)
        if ((s[i] & 0xC0) != 0x80) { *len = 1; return c; }
    *len = n;
    if (n == 2) return ((c & 0x1F) << 6) | (s[1] & 0x3F);
    if (n == 3) return ((c & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
    if (n == 4) return ((c & 0x07) << 18) | ((s[1] & 0x3F) << 12) | ((s[2] & 0x3F) << 6) | (s[3] & 0x3F);
    return c;
}

/* An edition's note, not part of the title: "Remastered 2011", "Deluxe Edition"... */
static int edition_note(const char *s, size_t n)
{
    static const char *const WORDS[] = {"remaster", "deluxe", "edition", "expanded", "bonus", "anniversary",
                                        "version", "reissue", "mono", "stereo", "explicit", "special",
                                        "collector", "single", "ep"};
    char low[160];
    size_t k = n < sizeof low - 1 ? n : sizeof low - 1;
    for (size_t i = 0; i < k; i++) low[i] = (char)tolower((unsigned char)s[i]);
    low[k] = '\0';
    for (size_t w = 0; w < sizeof WORDS / sizeof WORDS[0]; w++) {
        const char *p = low;
        size_t wl = strlen(WORDS[w]);
        while ((p = strstr(p, WORDS[w]))) {                 /* a word of its own (or its start) */
            int before = p == low || !isalpha((unsigned char)p[-1]);
            int after = wl > 2 || !isalpha((unsigned char)p[wl]);   /* ("ep": the whole word) */
            if (before && after) return 1;
            p += wl;
        }
    }
    return 0;
}

/* The title without its edition's notes: (...) and [...] groups, and a
 * " - Remastered" or " - Single" at the end */
static void strip_editions(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    for (const char *p = in; *p && o + 1 < cap;) {
        if (*p == '(' || *p == '[') {
            char close = *p == '(' ? ')' : ']';
            const char *e = strchr(p + 1, close);
            if (e && edition_note(p + 1, (size_t)(e - p - 1))) { p = e + 1; continue; }
        }
        if (!strncmp(p, " - ", 3) && edition_note(p + 3, strlen(p + 3))) break;
        out[o++] = *p++;
    }
    out[o] = '\0';
}

void cf_norm(const char *in, int title, char *out, size_t cap)
{
    char buf[512];
    if (title) strip_editions(in ? in : "", buf, sizeof buf);
    else snprintf(buf, sizeof buf, "%s", in ? in : "");
    size_t o = 0;
    int space = 1;                            /* (no space first) */
#define PUT(ch) do { if (o + 1 < cap) out[o++] = (char)(ch); } while (0)
    for (const unsigned char *s = (const unsigned char *)buf; *s;) {
        int len;
        unsigned c = utf8_next(s, &len);
        const char *plain = NULL;
        char one[2] = {0, 0};
        if (c < 0x80) {
            if (isalnum((int)c)) { one[0] = (char)tolower((int)c); plain = one; }
            else if (c == '&') plain = " and ";
            else if (c == '\'') plain = "";
            else plain = " ";
        } else if (c >= 0xC0 && c <= 0x17F) {
            char m = c <= 0xFF ? LATIN1[c - 0xC0] : LATIN_A[c - 0x100];
            plain = m == '1' ? "ij" : m == '2' ? "oe" : m == '3' ? "ae" : m == '4' ? "th" : m == '5' ? "ss"
                  : (one[0] = m, one);
        } else if (c == 0x2018 || c == 0x2019 || c == 0x02BC) {
            plain = "";                       /* apostrophes */
        } else if ((c >= 0x2000 && c <= 0x206F) || (c >= 0x80 && c < 0xC0)) {
            plain = " ";                      /* dashes, quotes, spaces, Latin-1's signs */
        }
        if (plain) {
            for (const char *q = plain; *q; q++) {
                if (*q == ' ') { if (!space) { PUT(' '); space = 1; } }
                else { PUT(*q); space = 0; }
            }
        } else {                              /* another script: as it is */
            for (int i = 0; i < len; i++) PUT(s[i]);
            space = 0;
        }
        s += len;
    }
    while (o && out[o - 1] == ' ') o--;
    out[o] = '\0';
    if (!strncmp(out, "the ", 4)) memmove(out, out + 4, strlen(out + 4) + 1);
#undef PUT
}

/* ---------- the searches ---------- */

static size_t url_put(char *out, size_t o, size_t cap, const char *s, int plus)
{
    for (const unsigned char *c = (const unsigned char *)s; *c && o + 4 < cap; c++) {
        if (isalnum(*c) || *c == '-' || *c == '.' || *c == '_') out[o++] = (char)*c;
        else if (*c == ' ' && plus) out[o++] = '+';
        else o += (size_t)snprintf(out + o, cap - o, "%%%02X", *c);
    }
    out[o < cap ? o : cap - 1] = '\0';
    return o;
}

/* A phrase for MusicBrainz's search ("..." with " and \ escaped) */
static void lucene_phrase(const char *s, char *out, size_t cap)
{
    size_t o = 0;
    if (cap < 3) { if (cap) out[0] = '\0'; return; }
    out[o++] = '"';
    for (; *s && o + 3 < cap; s++) {
        if (*s == '"' || *s == '\\') out[o++] = '\\';
        out[o++] = *s;
    }
    out[o++] = '"';
    out[o] = '\0';
}

void cf_mb_url(const cf_album_t *a, char *out, size_t cap)
{
    char t[600], ar[600], q[1400];
    lucene_phrase(a->title, t, sizeof t);
    lucene_phrase(a->artist[0] ? a->artist : "Various Artists", ar, sizeof ar);
    snprintf(q, sizeof q, "releasegroup:%s AND artist:%s", t, ar);
    size_t o = (size_t)snprintf(out, cap, "https://musicbrainz.org/ws/2/release-group/?query=");
    if (o >= cap) return;
    o = url_put(out, o, cap, q, 0);
    snprintf(out + o, cap - o, "&fmt=json&limit=%d", CF_MAX);
}

void cf_apple_url(const cf_album_t *a, char *out, size_t cap)
{
    char term[600];
    snprintf(term, sizeof term, "%s %s", key_is_various(a->artist) ? "" : a->artist, a->title);
    size_t o = (size_t)snprintf(out, cap, "https://itunes.apple.com/search?media=music&entity=album&limit=%d&term=", CF_MAX);
    if (o >= cap) return;
    url_put(out, o, cap, term[0] == ' ' ? term + 1 : term, 1);
}

static int year_of(const char *date) { return date && isdigit((unsigned char)date[0]) ? atoi(date) : 0; }

static int by_score(const void *x, const void *y)
{
    const cf_match_t *a = x, *b = y;
    if (a->score != b->score) return b->score - a->score;
    if (a->source != b->source) return a->source - b->source;   /* (MusicBrainz first) */
    return b->relevance - a->relevance;
}

int cf_parse_mb(const char *json, size_t len, const cf_album_t *a, cf_match_t *out, int max)
{
    json_t root = json_root(json, len), groups, g, v, credits, c;
    if (!json_member(root, "release-groups", &groups) || json_type(groups) != JSON_ARRAY) return -1;
    size_t at = 0;
    int n = 0;
    while (n < max && json_next(groups, &at, &g)) {
        cf_match_t m;
        char id[64] = "", date[32] = "";
        memset(&m, 0, sizeof m);
        if (!json_member(g, "id", &v) || json_string(v, id, sizeof id) <= 0) continue;
        if (json_member(g, "title", &v)) json_string(v, m.title, sizeof m.title);
        if (json_member(g, "first-release-date", &v)) json_string(v, date, sizeof date);
        if (json_member(g, "score", &v)) m.relevance = (int)json_int(v, 0);
        if (json_member(g, "artist-credit", &credits) && json_type(credits) == JSON_ARRAY) {
            size_t ca = 0;
            while (json_next(credits, &ca, &c)) {     /* "Name" + "joinphrase", each */
                char part[256] = "", join[64] = "";
                if (json_member(c, "name", &v)) json_string(v, part, sizeof part);
                if (json_member(c, "joinphrase", &v)) json_string(v, join, sizeof join);
                size_t l = strlen(m.artist);
                snprintf(m.artist + l, sizeof m.artist - l, "%s%s", part, join);
            }
        }
        m.year = year_of(date);
        m.source = CF_MUSICBRAINZ;
        snprintf(m.image, sizeof m.image, "https://coverartarchive.org/release-group/%s/front-500", id);
        cf_rate(a, &m);
        out[n++] = m;
    }
    qsort(out, (size_t)n, sizeof *out, by_score);
    return n;
}

int cf_parse_apple(const char *json, size_t len, const cf_album_t *a, cf_match_t *out, int max)
{
    json_t root = json_root(json, len), res, r, v;
    if (!json_member(root, "results", &res) || json_type(res) != JSON_ARRAY) return -1;
    size_t at = 0;
    int n = 0, order = 0;
    while (n < max && json_next(res, &at, &r)) {
        cf_match_t m;
        char date[40] = "", img[512] = "";
        memset(&m, 0, sizeof m);
        order++;
        if (!json_member(r, "artworkUrl100", &v) || json_string(v, img, sizeof img) <= 0) continue;
        if (json_member(r, "collectionName", &v)) json_string(v, m.title, sizeof m.title);
        if (json_member(r, "artistName", &v)) json_string(v, m.artist, sizeof m.artist);
        if (json_member(r, "releaseDate", &v)) json_string(v, date, sizeof date);
        char *size = strstr(img, "100x100bb");      /* the same picture at 600 px */
        if (size) snprintf(m.image, sizeof m.image, "%.*s600x600bb%s", (int)(size - img), img, size + 9);
        else snprintf(m.image, sizeof m.image, "%s", img);
        m.year = year_of(date);
        m.source = CF_APPLE;
        m.relevance = 100 - order;
        cf_rate(a, &m);
        int dup = 0;                                 /* (the same album twice: clean and explicit...) */
        for (int i = 0; i < n && !dup; i++) dup = !strcmp(out[i].image, m.image);
        if (!dup) out[n++] = m;
    }
    qsort(out, (size_t)n, sizeof *out, by_score);
    return n;
}

void cf_rate(const cf_album_t *a, cf_match_t *m)
{
    char wa[256], wt[256], ma[256], mt[256];
    cf_norm(a->artist, 0, wa, sizeof wa);
    cf_norm(a->title, 1, wt, sizeof wt);
    cf_norm(m->artist, 0, ma, sizeof ma);
    cf_norm(m->title, 1, mt, sizeof mt);
    int various = key_is_various(a->artist) || !a->artist[0];
    int artist_same = (various && key_is_various(m->artist)) || (wa[0] && !strcmp(wa, ma));
    size_t wl = strlen(wa);
    int artist_part = !artist_same && wl && !strncmp(ma, wa, wl) && ma[wl] == ' ';   /* "A and B" for "A" */
    int title_same = wt[0] && !strcmp(wt, mt);
    int title_part = !title_same && wt[0] && mt[0] && (strstr(mt, wt) || strstr(wt, mt));
    int year_fits = !a->year || !m->year || abs(a->year - m->year) <= 1;
    int score = (artist_same ? 50 : artist_part ? 25 : 0) + (title_same ? 40 : title_part ? 15 : 0);
    if (a->year && m->year) score += a->year == m->year ? 10 : year_fits ? 5 : -15;
    score += (m->relevance > 0 ? m->relevance : 0) / 25;   /* (0-4: the source's own order, for ties) */
    m->score = score;
    m->sure = artist_same && title_same && year_fits;
}

int cf_pick(const cf_album_t *a, const cf_match_t *m, int n)
{
    char wa[256], wt[256];
    cf_norm(a->artist, 0, wa, sizeof wa);
    cf_norm(a->title, 1, wt, sizeof wt);
    if (!a->year && !strcmp(wa, wt)) return -1;      /* self-titled, no year: which one? */
    int first = -1, same = 1;
    for (int i = 0; i < n; i++) {
        if (!m[i].sure) continue;
        if (first < 0) first = i;
        else if (m[i].year != m[first].year) same = 0;
    }
    if (first < 0) return -1;
    if (same) return first;                          /* one album (maybe listed twice) */
    for (int i = 0; i < n && a->year; i++)           /* several: the one from its very year */
        if (m[i].sure && m[i].year == a->year) return i;
    return -1;
}

/* ---------- the worker ---------- */

typedef struct {
    int state;                 /* 0 not asked, 1 asked, 2 here, -1 none, -2 couldn't be fetched */
    uint16_t *large, *small;
} pic_t;

static struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    pthread_t th;
    int started, stop;
    char dir[512];
    /* the find */
    unsigned gen;              /* the find asked last */
    int find_req, find_state;
    cf_album_t album;
    cf_match_t m[CF_MAX];
    int n;
    pic_t pic[CF_MAX];
    int load_req;
    int shown;                 /* the match shown last: the next one's fetched ahead */
    volatile int find_abort;
    /* the batch */
    cf_album_t *list;
    int list_n, next;
    int in_flight;             /* an album of the batch under way */
    int64_t user_kept[32];     /* albums given a cover by Menu > Find cover since the batch began */
    int n_user_kept;
    cf_batch_t bst;
    volatile int batch_abort;
    struct { int64_t id; uint16_t *large, *small; } kept[4];
    int n_kept;
    int64_t last_mb_ms;
} C = {.mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER, .find_state = -1, .load_req = -1};

static cf_fetch_fn g_fetch_once = http_get;
static cf_download_fn g_download_once = http_download;
static int g_mb_gap_ms = 1100;
static int g_retry_ms[3] = {2000, 3000, 5000};   /* between tries (a picture's: the first two) */

void cf_set_fetch(cf_fetch_fn fn) { g_fetch_once = fn ? fn : http_get; }
void cf_set_download(cf_download_fn fn) { g_download_once = fn ? fn : http_download; }
void cf_set_mb_gap_ms(int ms) { g_mb_gap_ms = ms; }
void cf_set_retry_ms(int ms) { g_retry_ms[0] = g_retry_ms[1] = g_retry_ms[2] = ms; }

static int64_t now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static int wait_ms(int ms, const volatile int *abort)
{
    for (int t = 0; t < ms && !*abort && !C.stop; t += 50) {
        struct timespec ts = {0, 50 * 1000 * 1000L};
        nanosleep(&ts, NULL);
    }
    return *abort || C.stop;
}

/* An answer, tried up to 3 times (MusicBrainz says it's busy now and then:
 * HTTP 503; a home router's name lookup fails now and then) */
static long fetch(const char *url, char *buf, size_t cap, const volatile int *abort, int mb)
{
    long n = -1;
    int tries = mb ? 4 : 3;                      /* (MusicBrainz says it's busy more often: 503) */
    for (int i = 0; i < tries; i++) {
        if (mb && g_mb_gap_ms > 0) {             /* once a second at most, as MusicBrainz asks */
            int64_t since = now_ms() - C.last_mb_ms;
            if (since < g_mb_gap_ms && wait_ms((int)(g_mb_gap_ms - since), abort)) return -1;
        }
        n = g_fetch_once(url, buf, cap, abort);
        if (mb) C.last_mb_ms = now_ms();
        if (n >= 0 || n == -2 || n == -3 || i == tries - 1 || wait_ms(g_retry_ms[i], abort)) break;   /* (-3: not there) */
    }
    return n;
}

/* A picture, tried up to 3 times as an answer is: its size, -3 if the server
 * says it isn't there (the Cover Art Archive has none for that album), or -1 */
static int64_t download(const char *url, const char *path, const volatile int *abort)
{
    int64_t n = -1;
    for (int i = 0; i < 3; i++) {
        n = g_download_once(url, path, NULL, NULL, abort);
        if (n > 0 || n == -3 || i == 2 || wait_ms(g_retry_ms[i], abort)) break;
    }
    return n;
}

/* Both searches' matches for a, best first (at most max): how many, or -1
 * if neither could be asked */
static int search(const cf_album_t *a, cf_match_t *out, int max, const volatile int *abort, int *mb_n)
{
    char url[1600];
    char *buf = malloc(ANSWER_MAX);
    cf_match_t tmp[CF_MAX];
    int n = 0, asked = 0;
    *mb_n = 0;
    if (!buf) return -1;
    cf_mb_url(a, url, sizeof url);
    long len = fetch(url, buf, ANSWER_MAX, abort, 1);
    int k = len > 0 ? cf_parse_mb(buf, (size_t)len, a, tmp, CF_MAX) : -1;
    if (k >= 0) asked = 1;
    for (int i = 0; i < k && n < max; i++) out[n++] = tmp[i];
    *mb_n = n;
    if (!*abort && !C.stop) {
        cf_apple_url(a, url, sizeof url);
        len = fetch(url, buf, ANSWER_MAX, abort, 0);
        k = len > 0 ? cf_parse_apple(buf, (size_t)len, a, tmp, CF_MAX) : -1;
        if (k >= 0) asked = 1;
        for (int i = 0; i < k && n < max; i++) out[n++] = tmp[i];
    }
    free(buf);
    if (*abort || C.stop || !asked) return -1;
    return n;
}

/* A picture downloaded to path and made into both thumbnails (malloc'd). 0;
 * -1 there's none (or it can't be read); -2 it couldn't be fetched (no
 * network, a name that didn't resolve: worth trying again) */
static int picture(const char *url, const char *path, const volatile int *abort, uint16_t **large, uint16_t **small)
{
    *large = *small = NULL;
    int64_t got = download(url, path, abort);
    if (got <= 0) { remove(path); return got == -3 ? -1 : -2; }
    FILE *f = fopen(path, "rbe");
    long n = -1;
    unsigned char *enc = NULL;
    if (f) {
        fseek(f, 0, SEEK_END);
        n = ftell(f);
        fseek(f, 0, SEEK_SET);
        enc = n > 0 && n <= PICTURE_MAX ? malloc((size_t)n) : NULL;
        if (enc && fread(enc, 1, (size_t)n, f) != (size_t)n) n = -1;
        fclose(f);
    }
    *large = malloc((size_t)LIB_ART_LARGE * LIB_ART_LARGE * 2);
    *small = malloc((size_t)LIB_ART_SMALL * LIB_ART_SMALL * 2);
    int ok = enc && n > 0 && *large && *small &&
             library_art_decode(enc, (size_t)n, LIB_ART_LARGE, *large) == 0 &&
             library_art_decode(enc, (size_t)n, LIB_ART_SMALL, *small) == 0;
    free(enc);
    if (!ok) {
        free(*large);
        free(*small);
        *large = *small = NULL;
        remove(path);
        return -1;
    }
    return 0;
}

static void find_file(int k, char *out, size_t cap) { snprintf(out, cap, "%s/.find-%d.jpg", C.dir, k); }

static void drop_pics(void)
{
    for (int k = 0; k < CF_MAX; k++) {
        free(C.pic[k].large);
        free(C.pic[k].small);
        C.pic[k].large = C.pic[k].small = NULL;
        C.pic[k].state = 0;
    }
}

static void do_find(unsigned gen, cf_album_t a)
{
    cf_match_t m[CF_MAX * 2];
    int mb_n;
    int n = search(&a, m, CF_MAX * 2, &C.find_abort, &mb_n);
    if (n > 0) qsort(m, (size_t)n, sizeof *m, by_score);
    int keep = 0;                                      /* those with anything in common */
    for (int i = 0; i < n && keep < CF_MAX; i++)
        if (m[i].score > 0) m[keep++] = m[i];
    pthread_mutex_lock(&C.mu);
    if (gen == C.gen) {
        if (n < 0) C.find_state = -2;
        else {
            memcpy(C.m, m, (size_t)keep * sizeof *m);
            C.n = keep;
            C.find_state = 1;
            if (keep && C.pic[0].state == 0) { C.pic[0].state = 1; C.load_req = 0; C.shown = 0; }   /* the first one's picture */
        }
    }
    pthread_mutex_unlock(&C.mu);
}

static void do_load(unsigned gen, int k)
{
    char url[512], path[600];
    pthread_mutex_lock(&C.mu);
    int ok = gen == C.gen && k < C.n;
    if (ok) snprintf(url, sizeof url, "%s", C.m[k].image);
    pthread_mutex_unlock(&C.mu);
    if (!ok) return;
    find_file(k, path, sizeof path);
    uint16_t *large, *small;
    int r = picture(url, path, &C.find_abort, &large, &small);
    pthread_mutex_lock(&C.mu);
    if (gen == C.gen) {
        C.pic[k].state = r == 0 ? 2 : r == -1 ? -1 : -2;
        C.pic[k].large = large;
        C.pic[k].small = small;
        large = small = NULL;
        /* the one after the one shown, so turning to it doesn't wait */
        if (k == C.shown && k + 1 < C.n && C.load_req < 0 && C.pic[k + 1].state == 0) {
            C.pic[k + 1].state = 1;
            C.load_req = k + 1;
        }
    }
    int stale = gen != C.gen;
    pthread_mutex_unlock(&C.mu);
    if (stale) remove(path);                         /* (the find was left meanwhile: its file goes too) */
    free(large);
    free(small);
}

/* (with C.mu held) album was given a cover by Menu > Find cover since the batch began */
static int user_kept(int64_t album)
{
    for (int i = 0; i < C.n_user_kept; i++)
        if (C.user_kept[i] == album) return 1;
    return 0;
}

/* One album of the batch: kept (a sure match whose picture could be read),
 * unsure (something like it), or nothing; 0, 1 left as it is (given a cover
 * meanwhile), or -1 if nothing could be asked */
static int do_batch_one(const cf_album_t *a)
{
    /* given a cover by Menu > Find cover since the list was made (the batch
     * takes minutes), or a found one removed meanwhile (.none): left as it is.
     * (Not "a KEYHASH.jpg is there": one the batch put there before the
     * player stopped, not stored yet, would be skipped for ever.) */
    char none[1200];
    struct stat st;
    pthread_mutex_lock(&C.mu);
    int kept_meanwhile = user_kept(a->id);
    pthread_mutex_unlock(&C.mu);
    if (kept_meanwhile || (library_found_cover_path(a->key, "none", none, sizeof none) == 0 && stat(none, &st) == 0))
        return 1;
    cf_match_t m[CF_MAX * 2];
    int mb_n;
    int n = search(a, m, CF_MAX * 2, &C.batch_abort, &mb_n);
    if (n < 0) return -1;
    /* each source's sure match, MusicBrainz's first; one whose picture fails: the other */
    int picks[2] = {cf_pick(a, m, mb_n), cf_pick(a, m + mb_n, n - mb_n)};
    if (picks[1] >= 0) picks[1] += mb_n;
    int kept = 0;
    for (int s = 0; s < 2 && !kept && !C.batch_abort && !C.stop; s++) {
        if (picks[s] < 0) continue;
        char tmp[600], dest[1200];
        snprintf(tmp, sizeof tmp, "%s/.batch.jpg", C.dir);
        uint16_t *large, *small;
        if (picture(m[picks[s]].image, tmp, &C.batch_abort, &large, &small)) continue;
        /* (with the lock: a keep from Menu > Find cover for this album now
         * either came first, and this one goes, or renames over it after) */
        pthread_mutex_lock(&C.mu);
        int mine = !user_kept(a->id);
        int moved = mine && library_found_cover_path(a->key, "jpg", dest, sizeof dest) == 0 && rename(tmp, dest) == 0;
        pthread_mutex_unlock(&C.mu);
        if (!moved) {
            remove(tmp);
            free(large);
            free(small);
            if (!mine) return 1;
            continue;
        }
        pthread_mutex_lock(&C.mu);
        while (C.n_kept == (int)(sizeof C.kept / sizeof C.kept[0]) && !C.stop && !C.batch_abort)
            pthread_cond_wait(&C.cv, &C.mu);          /* (the UI takes them: cf_batch_take) */
        if (C.n_kept < (int)(sizeof C.kept / sizeof C.kept[0])) {
            C.kept[C.n_kept].id = a->id;
            C.kept[C.n_kept].large = large;
            C.kept[C.n_kept].small = small;
            C.n_kept++;
            large = small = NULL;
            kept = 1;
        }
        pthread_mutex_unlock(&C.mu);
        free(large);
        free(small);
    }
    pthread_mutex_lock(&C.mu);
    if (kept) C.bst.found++;
    else {
        int like = 0;
        for (int i = 0; i < n; i++) like |= m[i].score >= 40;
        if (like) C.bst.unsure++;
        else C.bst.none++;
    }
    pthread_mutex_unlock(&C.mu);
    return 0;
}

/* (with C.mu held) */
static void batch_end(void)
{
    C.bst.running = 0;
    C.bst.seq++;
    free(C.list);
    C.list = NULL;
    C.list_n = C.next = 0;
    fprintf(stderr, "covers: %d of %d looked at: %d found, %d unsure, %d none%s\n", C.bst.done,
            C.bst.total, C.bst.found, C.bst.unsure, C.bst.none, C.bst.failed ? " (couldn't ask)" : "");
}

static void *worker(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&C.mu);
    while (!C.stop) {
        int batch = C.bst.running && C.next < C.list_n;
        if (!C.find_req && C.load_req < 0 && !batch) { pthread_cond_wait(&C.cv, &C.mu); continue; }
        unsigned gen = C.gen;
        if (C.find_req) {
            cf_album_t a = C.album;
            C.find_req = 0;
            C.find_abort = 0;
            pthread_mutex_unlock(&C.mu);
            do_find(gen, a);
        } else if (C.load_req >= 0) {
            int k = C.load_req;
            C.load_req = -1;
            pthread_mutex_unlock(&C.mu);
            do_load(gen, k);
        } else {
            cf_album_t a = C.list[C.next++];
            C.in_flight = 1;
            pthread_mutex_unlock(&C.mu);
            int r = do_batch_one(&a);
            pthread_mutex_lock(&C.mu);
            C.in_flight = 0;
            if (r == 0) C.bst.done++;
            else if (r == 1) C.bst.total--;            /* (given a cover meanwhile: not one to look for) */
            int end = C.next >= C.list_n || r < 0 || C.batch_abort;
            if (r < 0 && !C.batch_abort) C.bst.failed = 1;
            if (end && C.bst.running) batch_end();
            pthread_mutex_unlock(&C.mu);
        }
        pthread_mutex_lock(&C.mu);
    }
    pthread_mutex_unlock(&C.mu);
    return NULL;
}

int cf_start(const char *dir)
{
    if (C.started) return 0;
    snprintf(C.dir, sizeof C.dir, "%s", dir);
    mkdir(C.dir, 0755);
    char p[600];
    char part[620];
    for (int k = 0; k < CF_MAX; k++) {           /* (a find left behind, and a picture's download cut short) */
        find_file(k, p, sizeof p);
        remove(p);
        snprintf(part, sizeof part, "%s.part", p);
        remove(part);
    }
    snprintf(p, sizeof p, "%s/.batch.jpg", C.dir);
    remove(p);
    snprintf(part, sizeof part, "%s.part", p);
    remove(part);
    C.stop = 0;
    if (pthread_create(&C.th, NULL, worker, NULL)) return -1;
    C.started = 1;
    return 0;
}

void cf_stop(void)
{
    if (!C.started) return;
    pthread_mutex_lock(&C.mu);
    C.stop = 1;
    C.find_abort = C.batch_abort = 1;
    pthread_cond_broadcast(&C.cv);
    pthread_mutex_unlock(&C.mu);
    pthread_join(C.th, NULL);
    C.started = 0;
    drop_pics();
    for (int i = 0; i < C.n_kept; i++) { free(C.kept[i].large); free(C.kept[i].small); }
    C.n_kept = 0;
    free(C.list);
    C.list = NULL;
    C.list_n = C.next = 0;
    C.bst.running = 0;
    C.find_state = -1;
}

int cf_find(const cf_album_t *a)
{
    if (!C.started) return -1;
    pthread_mutex_lock(&C.mu);
    C.gen++;
    C.find_abort = 1;                                 /* (the last one, if it's still going) */
    drop_pics();
    C.album = *a;
    C.n = 0;
    C.find_state = 0;
    C.find_req = 1;
    C.load_req = -1;
    pthread_cond_broadcast(&C.cv);
    pthread_mutex_unlock(&C.mu);
    return 0;
}

int cf_find_state(int *n)
{
    pthread_mutex_lock(&C.mu);
    int st = C.find_state;
    if (n) *n = st == 1 ? C.n : 0;
    pthread_mutex_unlock(&C.mu);
    return st;
}

int cf_find_show(int k, cf_match_t *m, uint16_t *large)
{
    int r;
    pthread_mutex_lock(&C.mu);
    if (C.find_state != 1 || k < 0 || k >= C.n) r = -1;
    else {
        if (m) *m = C.m[k];
        pic_t *p = &C.pic[k];
        C.shown = k;
        if (p->state == 2) {
            if (large) memcpy(large, p->large, (size_t)LIB_ART_LARGE * LIB_ART_LARGE * 2);
            r = 1;
        } else if (p->state < 0) r = p->state;
        else {
            if (p->state == 0) {                      /* asked now, before one fetched ahead */
                if (C.load_req >= 0) C.pic[C.load_req].state = 0;
                p->state = 1;
                C.load_req = k;
                pthread_cond_broadcast(&C.cv);
            }
            r = 0;
        }
    }
    pthread_mutex_unlock(&C.mu);
    return r;
}

int cf_find_retry(int k)
{
    pthread_mutex_lock(&C.mu);
    int ok = C.find_state == 1 && k >= 0 && k < C.n && C.pic[k].state == -2;
    if (ok) {
        if (C.load_req >= 0 && C.load_req != k) C.pic[C.load_req].state = 0;   /* (before one fetched ahead) */
        C.pic[k].state = 1;
        C.load_req = k;
        C.shown = k;
        pthread_cond_broadcast(&C.cv);
    }
    pthread_mutex_unlock(&C.mu);
    return ok ? 0 : -1;
}

int cf_find_keep(int k, uint16_t *large, uint16_t *small)
{
    char key[512], from[600], to[1200], none[1200];
    int64_t id = 0;
    pthread_mutex_lock(&C.mu);
    int ok = C.find_state == 1 && k >= 0 && k < C.n && C.pic[k].state == 2;
    if (ok) {
        memcpy(large, C.pic[k].large, (size_t)LIB_ART_LARGE * LIB_ART_LARGE * 2);
        memcpy(small, C.pic[k].small, (size_t)LIB_ART_SMALL * LIB_ART_SMALL * 2);
        snprintf(key, sizeof key, "%s", C.album.key);
        id = C.album.id;
    }
    pthread_mutex_unlock(&C.mu);
    if (!ok) return -1;
    find_file(k, from, sizeof from);
    /* (with the lock, and noted for a batch under way: it won't put its own over this) */
    pthread_mutex_lock(&C.mu);
    int r = library_found_cover_path(key, "jpg", to, sizeof to) || rename(from, to) ? -1 : 0;
    if (r == 0 && !user_kept(id)) {
        if (C.n_user_kept == (int)(sizeof C.user_kept / sizeof C.user_kept[0])) C.n_user_kept = 0;   /* (32 in one batch: start over) */
        C.user_kept[C.n_user_kept++] = id;
    }
    pthread_mutex_unlock(&C.mu);
    if (r) return -1;
    if (library_found_cover_path(key, "none", none, sizeof none) == 0) remove(none);
    return 0;
}

void cf_find_close(void)
{
    pthread_mutex_lock(&C.mu);
    C.gen++;
    C.find_abort = 1;
    C.find_req = 0;
    C.load_req = -1;
    C.find_state = -1;
    C.n = 0;
    drop_pics();
    pthread_mutex_unlock(&C.mu);
    char p[600];
    for (int k = 0; k < CF_MAX; k++) { find_file(k, p, sizeof p); remove(p); }
}

int cf_batch(const cf_album_t *list, int n)
{
    if (!C.started) return -1;
    cf_album_t *copy = malloc((size_t)(n > 0 ? n : 1) * sizeof *copy);
    if (!copy) return -1;
    int k = 0;
    for (int i = 0; i < n; i++) {                     /* (a found cover removed: not again) */
        char none[1200];
        struct stat st;
        if (library_found_cover_path(list[i].key, "none", none, sizeof none) == 0 && stat(none, &st) == 0) continue;
        copy[k++] = list[i];
    }
    pthread_mutex_lock(&C.mu);
    if (C.bst.running) { pthread_mutex_unlock(&C.mu); free(copy); return -1; }
    unsigned seq = C.bst.seq;
    memset(&C.bst, 0, sizeof C.bst);
    C.bst.seq = seq;
    C.bst.total = k;
    C.list = copy;
    C.list_n = k;
    C.n_user_kept = 0;
    C.next = 0;
    C.batch_abort = 0;
    C.bst.running = 1;
    if (!k) batch_end();                              /* nothing to look for: done at once */
    pthread_cond_broadcast(&C.cv);
    pthread_mutex_unlock(&C.mu);
    return 0;
}

void cf_batch_stop(void)
{
    pthread_mutex_lock(&C.mu);
    if (C.bst.running) {
        C.batch_abort = 1;
        C.list_n = C.next;                            /* (none after the one under way) */
        if (!C.in_flight) batch_end();                /* (none under way: done now) */
        pthread_cond_broadcast(&C.cv);
    }
    pthread_mutex_unlock(&C.mu);
}

void cf_batch_status(cf_batch_t *st)
{
    pthread_mutex_lock(&C.mu);
    *st = C.bst;
    pthread_mutex_unlock(&C.mu);
}

int cf_batch_take(int64_t *album, uint16_t *large, uint16_t *small)
{
    pthread_mutex_lock(&C.mu);
    int r = C.n_kept > 0;
    if (r) {
        *album = C.kept[0].id;
        memcpy(large, C.kept[0].large, (size_t)LIB_ART_LARGE * LIB_ART_LARGE * 2);
        memcpy(small, C.kept[0].small, (size_t)LIB_ART_SMALL * LIB_ART_SMALL * 2);
        free(C.kept[0].large);
        free(C.kept[0].small);
        memmove(&C.kept[0], &C.kept[1], (size_t)(C.n_kept - 1) * sizeof C.kept[0]);
        C.n_kept--;
        pthread_cond_broadcast(&C.cv);
    }
    pthread_mutex_unlock(&C.mu);
    return r;
}
