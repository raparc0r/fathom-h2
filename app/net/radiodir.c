/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* The station directory: radio-browser.info (radiodir.h). */
#include "net/radiodir.h"

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "net/http.h"
#include "net/json.h"

/* The directory's servers, tried in turn from the one that answered last.
 * all.api is the project's own round-robin name for whichever servers are up
 * (https://api.radio-browser.info), so it comes first: a list of named ones
 * goes stale (fi1 and nl1 no longer existed by 2026-11, each costing a failed
 * lookup before an answer). The two named ones that were still up follow, in
 * case all.api's answer is the one that's down. */
static const char *const SERVERS[] = {
    "all.api.radio-browser.info", "de1.api.radio-browser.info", "de2.api.radio-browser.info",
};
#define N_SERVERS ((int)(sizeof SERVERS / sizeof SERVERS[0]))

/* Genres: the directory's tags with the most working stations (2026-11),
 * as the name in lower case, but for these (tags are free text: "hiphop"
 * has more stations than "hip hop") */
static const char *const TAGS[][2] = {{"Hip hop", "hiphop"}, {"R&B", "rnb"}};
const char *const RDIR_GENRES[] = {
    "Pop", "Rock", "Jazz", "Classical", "Electronic", "Dance", "Hip hop", "Indie",
    "Alternative", "Ambient", "Chillout", "Lounge", "Blues", "Soul", "Funk", "R&B",
    "Reggae", "Country", "Folk", "Metal", "Punk", "House", "Techno", "Trance",
    "Drum and bass", "Disco", "Latin", "Salsa", "World music", "Oldies", "60s", "70s",
    "80s", "90s", "Classic rock", "Easy listening", "Smooth jazz", "Christian", "Gospel",
    "News", "Talk", "Sports", "Public radio", "Community radio", "College radio", "Kids",
    "Christmas",
};
const int RDIR_N_GENRES = (int)(sizeof RDIR_GENRES / sizeof RDIR_GENRES[0]);

void rdir_genre_tag(int i, char *out, size_t cap)
{
    snprintf(out, cap, "%s", RDIR_GENRES[i]);
    for (size_t t = 0; t < sizeof TAGS / sizeof TAGS[0]; t++)
        if (!strcmp(RDIR_GENRES[i], TAGS[t][0])) snprintf(out, cap, "%s", TAGS[t][1]);
    for (char *c = out; *c; c++) *c = (char)tolower((unsigned char)*c);
}

/* ---------- parsing ---------- */

int rdir_playable(const char *codec, int hls)
{
    static const char *const OK[] = {"MP3", "AAC", "AAC+", "OGG", "FLAC", "OPUS", "VORBIS"};
    if (!codec || !codec[0]) return 0;
    if (!strcasecmp(codec, "UNKNOWN")) return hls;          /* HLS: the H2 picks the variant it decodes */
    /* "AAC,H.264": a TV stream; each part must be audio the H2 plays */
    const char *p = codec;
    while (*p) {
        const char *e = strchr(p, ',');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        int ok = 0;
        for (size_t i = 0; i < sizeof OK / sizeof OK[0]; i++)
            if (strlen(OK[i]) == n && !strncasecmp(p, OK[i], n)) ok = 1;
        if (!ok) return 0;
        p = e ? e + 1 : p + n;
    }
    return 1;
}

/* At most cap - 1 bytes of s, never half a UTF-8 character. */
static void cut_utf8(char *s, size_t cap)
{
    size_t n = strlen(s);
    if (n < cap) return;
    n = cap - 1;
    while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--;
    s[n] = '\0';
}

/* A name as one line: runs of spaces, tabs and line breaks become one space, trimmed. */
static void tidy(char *s)
{
    char *w = s;
    int space = 1;
    for (const char *r = s; *r; r++) {
        if ((unsigned char)*r < 0x20 || *r == ' ') {
            if (!space) *w++ = ' ';
            space = 1;
        } else {
            *w++ = *r;
            space = 0;
        }
    }
    while (w > s && w[-1] == ' ') w--;
    *w = '\0';
}

static int stream_url(const char *u)
{
    if (strncmp(u, "http://", 7) && strncmp(u, "https://", 8)) return 0;
    for (const char *c = u; *c; c++)
        if ((unsigned char)*c <= ' ') return 0;
    return 1;
}

static void member_str(json_t o, const char *name, char *out, size_t cap)
{
    json_t v;
    out[0] = '\0';
    if (json_member(o, name, &v)) json_string(v, out, cap);
}

/* Walks the elements of the top-level array one by one, so an answer cut short
 * still gives those before the cut. -1 if it isn't an array at all. */
typedef struct { const char *p, *end; } walk_t;

static int walk_begin(walk_t *w, const char *text, size_t len)
{
    w->p = text;
    w->end = text + len;
    while (w->p < w->end && isspace((unsigned char)*w->p)) w->p++;
    if (w->p >= w->end || *w->p != '[') return -1;
    w->p++;
    return 0;
}

static int walk_next(walk_t *w, json_t *out)
{
    while (w->p < w->end && (isspace((unsigned char)*w->p) || *w->p == ',')) w->p++;
    if (w->p >= w->end || *w->p == ']') return 0;
    *out = json_root(w->p, (size_t)(w->end - w->p));
    if (!out->s) return 0;                   /* cut short (or malformed) here */
    w->p = out->s + out->n;
    return 1;
}

int rdir_parse_stations(const char *text, size_t len, radio_station_t *st, rdir_meta_t *meta, int max)
{
    walk_t w;
    if (!text || walk_begin(&w, text, len)) return -1;
    int n = 0;
    json_t o, v;
    char codec[32], url[1024], name[256];
    while (n < max && walk_next(&w, &o)) {
        if (json_type(o) != JSON_OBJECT) continue;
        member_str(o, "codec", codec, sizeof codec);
        int hls = json_member(o, "hls", &v) ? json_bool(v, 0) : 0;
        if (!rdir_playable(codec, hls)) continue;
        if (json_member(o, "lastcheckok", &v) && !json_bool(v, 1)) continue;
        member_str(o, "url_resolved", url, sizeof url);   /* the stream, playlists resolved */
        if (!stream_url(url)) member_str(o, "url", url, sizeof url);
        if (!stream_url(url) || strlen(url) >= sizeof st[0].url) continue;
        int dup = 0;
        for (int i = 0; i < n && !dup; i++) dup = !strcmp(st[i].url, url);
        if (dup) continue;                   /* the directory lists some stations twice */
        member_str(o, "name", name, sizeof name);
        tidy(name);
        if (!name[0]) continue;
        radio_station_t *s = &st[n];
        memset(s, 0, sizeof *s);
        cut_utf8(name, sizeof s->name);
        snprintf(s->name, sizeof s->name, "%s", name);
        snprintf(s->url, sizeof s->url, "%s", url);
        char icon[512];                      /* its logo, when the directory has one that can be kept */
        member_str(o, "favicon", icon, sizeof icon);
        if (radio_icon_ok(icon)) snprintf(s->icon, sizeof s->icon, "%s", icon);
        if (meta) {
            rdir_meta_t *m = &meta[n];
            memset(m, 0, sizeof *m);
            long long br = json_member(o, "bitrate", &v) ? json_int(v, 0) : 0;
            for (char *c = codec; *c; c++) *c = (char)toupper((unsigned char)*c);
            if (!strcmp(codec, "UNKNOWN")) snprintf(m->detail, sizeof m->detail, "HLS");
            else if (br > 0 && br < 10000) snprintf(m->detail, sizeof m->detail, "%.8s %lld", codec, br);
            else snprintf(m->detail, sizeof m->detail, "%.15s", codec);
            char uuid[64];
            member_str(o, "stationuuid", uuid, sizeof uuid);
            int ok = uuid[0] && strlen(uuid) < sizeof m->uuid;
            for (const char *c = uuid; *c && ok; c++) ok = isxdigit((unsigned char)*c) || *c == '-';
            if (ok) memcpy(m->uuid, uuid, strlen(uuid) + 1);
        }
        n++;
    }
    return n;
}

/* The directory's country names are the long forms ("The United Kingdom Of
 * Great Britain And Northern Ireland"): the usual short ones, a leading "The"
 * dropped and "And", "Of"... in lower case. */
static void country_name(const char *code, char *name, size_t cap)
{
    static const char *const SHORT[][2] = {
        {"GB", "United Kingdom"}, {"US", "United States"}, {"RU", "Russia"}, {"KR", "South Korea"},
        {"KP", "North Korea"}, {"IR", "Iran"}, {"LA", "Laos"}, {"MD", "Moldova"}, {"MK", "North Macedonia"},
        {"TW", "Taiwan"}, {"TZ", "Tanzania"}, {"VE", "Venezuela"}, {"BO", "Bolivia"}, {"CD", "DR Congo"},
        {"VA", "Vatican City"}, {"SY", "Syria"}, {"VN", "Vietnam"}, {"UM", "US Minor Outlying Islands"},
        {"FK", "Falkland Islands"}, {"CC", "Cocos Islands"}, {"SH", "Saint Helena"}, {"SX", "Sint Maarten"},
        {"PS", "Palestine"}, {"FM", "Micronesia"}, {"BN", "Brunei"}, {"CZ", "Czechia"},
    };
    for (size_t i = 0; i < sizeof SHORT / sizeof SHORT[0]; i++)
        if (!strcmp(code, SHORT[i][0])) { snprintf(name, cap, "%s", SHORT[i][1]); return; }
    if (!strncmp(name, "The ", 4)) memmove(name, name + 4, strlen(name + 4) + 1);
    static const char *const SMALL[] = {" And ", " Of ", " The ", " Da ", " De "};
    for (size_t i = 0; i < sizeof SMALL / sizeof SMALL[0]; i++)
        for (char *p = strstr(name, SMALL[i]); p; p = strstr(p + 1, SMALL[i])) p[1] = (char)tolower((unsigned char)p[1]);
}

static int by_name(const void *a, const void *b)
{
    return strcasecmp(((const rdir_country_t *)a)->name, ((const rdir_country_t *)b)->name);
}

int rdir_parse_countries(const char *text, size_t len, rdir_country_t *out, int max)
{
    walk_t w;
    if (!text || walk_begin(&w, text, len)) return -1;
    int n = 0;
    json_t o, v;
    char name[128], code[8];
    while (n < max && walk_next(&w, &o)) {
        if (json_type(o) != JSON_OBJECT) continue;
        member_str(o, "iso_3166_1", code, sizeof code);
        member_str(o, "name", name, sizeof name);
        int count = json_member(o, "stationcount", &v) ? (int)json_int(v, 0) : 0;
        if (strlen(code) != 2 || !isalpha((unsigned char)code[0]) || !isalpha((unsigned char)code[1])) continue;
        if (count < 1) continue;
        tidy(name);
        if (!name[0]) continue;
        code[0] = (char)toupper((unsigned char)code[0]);
        code[1] = (char)toupper((unsigned char)code[1]);
        rdir_country_t *c = &out[n++];
        memset(c, 0, sizeof *c);
        snprintf(c->code, sizeof c->code, "%s", code);
        country_name(code, name, sizeof name);
        cut_utf8(name, sizeof c->name);
        snprintf(c->name, sizeof c->name, "%s", name);
        c->count = count;
    }
    qsort(out, (size_t)n, sizeof *out, by_name);
    return n;
}

/* A query's text, percent-encoded. */
static void encode(char *out, size_t cap, const char *s)
{
    static const char HEX[] = "0123456789ABCDEF";
    size_t n = 0;
    for (; *s && n + 4 < cap; s++) {
        unsigned char c = (unsigned char)*s;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out[n++] = (char)c;
        else { out[n++] = '%'; out[n++] = HEX[c >> 4]; out[n++] = HEX[c & 15]; }
    }
    out[n] = '\0';
}

void rdir_stations_url(char *out, size_t cap, const char *server, const rdir_query_t *q)
{
    char arg[200], by[240] = "";
    encode(arg, sizeof arg, q->arg);
    switch (q->by) {
    case RDIR_NAME: snprintf(by, sizeof by, "name=%s&", arg); break;
    case RDIR_TAG: snprintf(by, sizeof by, "tag=%s&tagExact=true&", arg); break;
    case RDIR_COUNTRY: snprintf(by, sizeof by, "countrycode=%s&", arg); break;
    case RDIR_POPULAR: break;
    }
    snprintf(out, cap, "https://%s/json/stations/search?%shidebroken=true&order=%s&limit=%d", server, by,
             q->random ? "random" : "clickcount&reverse=true", RDIR_ASK_STATIONS);
}

void rdir_countries_url(char *out, size_t cap, const char *server)
{
    snprintf(out, cap, "https://%s/json/countries?order=name&hidebroken=true", server);
}

/* ---------- requests ---------- */

typedef struct {
    rdir_query_t q;
    int stations;                /* else the countries */
    volatile int abort;
    int done, orphan;            /* (under mu) orphan: nobody waits; the thread frees it */
    rdir_result_t res;
} job_t;

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static rdir_fetch_fn g_fetch = http_get;
static int g_server = -1;        /* the server that answered last (under mu); -1: none yet */
static job_t *g_job;             /* the request the UI waits for (the UI's thread only) */
static int g_clicking;           /* (under mu) the click worker is running */
static char *g_click_url;        /* (under mu) the play it counts next */

void rdir_set_fetch(rdir_fetch_fn fn) { g_fetch = fn ? fn : http_get; }

void rdir_result_free(rdir_result_t *r)
{
    if (!r) return;
    free(r->st);
    free(r->meta);
    free(r->countries);
    memset(r, 0, sizeof *r);
}

static int first_server(void)
{
    pthread_mutex_lock(&mu);
    int s = g_server;
    pthread_mutex_unlock(&mu);
    /* none has answered yet: all.api (it shares the load across the servers itself) */
    return s >= 0 ? s : 0;
}

static void *job_run(void *arg)
{
    job_t *j = arg;
    rdir_result_t r = {.n = -1};
    char *buf = malloc(RDIR_MAX_ANSWER);
    if (j->stations) {
        r.st = malloc(RDIR_MAX_STATIONS * sizeof *r.st);
        r.meta = malloc(RDIR_MAX_STATIONS * sizeof *r.meta);
    } else {
        r.countries = malloc(RDIR_MAX_COUNTRIES * sizeof *r.countries);
    }
    if (buf && (j->stations ? r.st && r.meta : r.countries != NULL)) {
        int s0 = first_server();
        for (int k = 0; k < N_SERVERS && !j->abort; k++) {
            int s = (s0 + k) % N_SERVERS;
            char url[512];
            if (j->stations) rdir_stations_url(url, sizeof url, SERVERS[s], &j->q);
            else rdir_countries_url(url, sizeof url, SERVERS[s]);
            long len = g_fetch(url, buf, RDIR_MAX_ANSWER, &j->abort);
            if (len == -2) len = (long)strlen(buf);       /* cut: the whole stations before the cut */
            if (len < 0) continue;
            int n = j->stations ? rdir_parse_stations(buf, (size_t)len, r.st, r.meta, RDIR_MAX_STATIONS)
                                : rdir_parse_countries(buf, (size_t)len, r.countries, RDIR_MAX_COUNTRIES);
            if (n < 0) continue;                           /* not the directory's answer (an error page) */
            r.n = n;
            pthread_mutex_lock(&mu);
            g_server = s;
            pthread_mutex_unlock(&mu);
            break;
        }
    }
    free(buf);
    if (r.n <= 0) {                                        /* nothing to keep */
        int n = r.n;
        rdir_result_free(&r);
        r.n = n;
    }
    pthread_mutex_lock(&mu);
    j->res = r;
    j->done = 1;
    int orphan = j->orphan;
    pthread_mutex_unlock(&mu);
    if (orphan) {
        rdir_result_free(&j->res);
        free(j);
    }
    return NULL;
}

static int spawn(void *(*fn)(void *), void *arg)
{
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    int r = pthread_create(&t, &a, fn, arg);
    pthread_attr_destroy(&a);
    return r == 0 ? 0 : -1;
}

int rdir_start(const rdir_query_t *q)
{
    rdir_cancel();
    job_t *j = calloc(1, sizeof *j);
    if (!j) return -1;
    j->stations = q != NULL;
    if (q) j->q = *q;
    if (spawn(job_run, j)) { free(j); return -1; }
    g_job = j;
    return 0;
}

int rdir_poll(rdir_result_t *out)
{
    if (!g_job) return -1;
    pthread_mutex_lock(&mu);
    int done = g_job->done;
    pthread_mutex_unlock(&mu);
    if (!done) return 0;
    *out = g_job->res;
    free(g_job);
    g_job = NULL;
    return 1;
}

int rdir_busy(void) { return g_job != NULL; }

void rdir_cancel(void)
{
    job_t *j = g_job;
    if (!j) return;
    g_job = NULL;
    pthread_mutex_lock(&mu);
    int done = j->done;
    if (!done) {
        j->abort = 1;
        j->orphan = 1;
    }
    pthread_mutex_unlock(&mu);
    if (done) {
        rdir_result_free(&j->res);
        free(j);
    }
}

/* A play is counted the moment a station is chosen, which is when the
 * station's own connection needs the one core: the request (a TLS handshake
 * for an answer that's thrown away) waits a few seconds first, and runs at
 * the lowest priority. A station chosen meanwhile is the one counted. */
#define CLICK_WAIT_MS 5000
static int g_click_wait_ms = CLICK_WAIT_MS;

void rdir_test_click_wait(int ms) { g_click_wait_ms = ms >= 0 ? ms : CLICK_WAIT_MS; }

static void *click_run(void *arg)
{
    (void)arg;
#ifdef __linux__
    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 19);   /* (this thread only) */
#endif
    for (;;) {
        struct timespec ts = {g_click_wait_ms / 1000, (long)(g_click_wait_ms % 1000) * 1000000L};
        nanosleep(&ts, NULL);
        pthread_mutex_lock(&mu);
        char *url = g_click_url;
        g_click_url = NULL;
        pthread_mutex_unlock(&mu);
        if (url) {
            char buf[4096];
            g_fetch(url, buf, sizeof buf, NULL);
            free(url);
        }
        pthread_mutex_lock(&mu);
        int more = g_click_url != NULL;           /* another chosen while that one was counted */
        if (!more) g_clicking = 0;
        pthread_mutex_unlock(&mu);
        if (!more) return NULL;
    }
}

void rdir_count_click(const char *uuid)
{
    if (!uuid || !uuid[0]) return;
    char *url = malloc(256);
    if (!url) return;
    char id[64];
    encode(id, sizeof id, uuid);
    snprintf(url, 256, "https://%s/json/url/%s", SERVERS[first_server()], id);
    pthread_mutex_lock(&mu);
    free(g_click_url);                            /* (one not counted yet: this one instead) */
    g_click_url = url;
    int start = !g_clicking;
    if (start) g_clicking = 1;
    pthread_mutex_unlock(&mu);
    if (start && spawn(click_run, NULL)) {
        pthread_mutex_lock(&mu);
        free(g_click_url);
        g_click_url = NULL;
        g_clicking = 0;
        pthread_mutex_unlock(&mu);
    }
}
