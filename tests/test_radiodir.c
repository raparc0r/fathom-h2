/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* The station directory (app/net/radiodir.c), on answers saved from
 * radio-browser.info (tests/fixtures/radiodir, 2026-11-17). */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "net/radiodir.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static char *slurp(const char *name, size_t *len)
{
    char path[256];
    snprintf(path, sizeof path, "tests/fixtures/radiodir/%s", name);
    FILE *f = fopen(path, "rb");
    if (!f) { printf("FAIL: no %s\n", path); fails++; *len = 0; return calloc(1, 1); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    *len = fread(b, 1, (size_t)n, f);
    b[*len] = '\0';
    fclose(f);
    return b;
}

static void nap(int ms)
{
    struct timespec t = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&t, NULL);
}

/* ---- fetching from the fixtures, as the simulator does ---- */

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static int calls, fail_all, slow;
static char last_url[512];

static long fixture_fetch(const char *url, char *buf, size_t cap, const volatile int *abort)
{
    pthread_mutex_lock(&mu);
    calls++;
    snprintf(last_url, sizeof last_url, "%s", url);
    pthread_mutex_unlock(&mu);
    if (slow) {                                   /* a server that doesn't answer: until abort */
        for (int i = 0; i < 300 && !(abort && *abort); i++) nap(10);
        return -1;
    }
    if (fail_all) return -1;
    const char *name = strstr(url, "/json/countries") ? "countries.json"
                     : strstr(url, "tag=jazz") && strstr(url, "order=random") ? "stations-jazz-random.json"
                     : strstr(url, "tag=jazz") ? "stations-jazz.json"
                     : strstr(url, "countrycode=GB") ? "stations-gb.json"
                     : strstr(url, "name=fip") ? "stations-fip.json"
                     : strstr(url, "/json/url/") ? NULL : "stations-popular.json";
    if (!name) { snprintf(buf, cap, "{\"ok\":true}"); return (long)strlen(buf); }
    size_t n;
    char *t = slurp(name, &n);
    long r = (long)n;
    if (n >= cap) { n = cap - 1; r = -2; }
    memcpy(buf, t, n);
    buf[n] = '\0';
    free(t);
    return r;
}

static int wait_done(rdir_result_t *r)
{
    for (int i = 0; i < 500; i++) {
        int p = rdir_poll(r);
        if (p != 0) return p;
        nap(10);
    }
    return 0;
}

int main(void)
{
    radio_station_t st[RDIR_MAX_STATIONS];
    rdir_meta_t meta[RDIR_MAX_STATIONS];
    size_t len;

    /* the most listened stations: a full answer (64 asked for) */
    char *t = slurp("stations-popular.json", &len);
    int n = rdir_parse_stations(t, len, st, meta, RDIR_MAX_STATIONS);
    CHECK(n == 64);
    CHECK(!strcmp(st[0].name, "Christmas FM Non-stop!"));
    CHECK(!strncmp(st[0].url, "https://", 8));
    CHECK(st[0].info[0] == '\0');
    CHECK(!strcmp(meta[0].detail, "AAC 128"));
    CHECK(strlen(meta[0].uuid) == 36);
    CHECK(!strcmp(st[1].name, "Radio Armisa") && !strcmp(meta[1].detail, "MP3"));   /* no bitrate given */
    CHECK(!strcmp(st[2].name, "RTL") && !strcmp(meta[2].detail, "AAC 64"));          /* HLS, codec known */
    CHECK(rdir_parse_stations(t, len, st, meta, 10) == 10);
    /* cut short (a longer answer than the buffer): the stations before the cut */
    int cut = rdir_parse_stations(t, 40000, st, meta, RDIR_MAX_STATIONS);
    CHECK(cut > 20 && cut < 64);
    CHECK(cut > 0 && st[cut - 1].name[0] && !strncmp(st[cut - 1].url, "http", 4));
    free(t);

    /* names tidied: the directory's " CAPITAL - ..." */
    t = slurp("stations-gb.json", &len);
    n = rdir_parse_stations(t, len, st, NULL, RDIR_MAX_STATIONS);
    CHECK(n == 24);
    CHECK(!strcmp(st[1].name, "CAPITAL - The UK's No.1 Hit Music Station"));
    free(t);

    t = slurp("stations-fip.json", &len);
    n = rdir_parse_stations(t, len, st, meta, RDIR_MAX_STATIONS);
    CHECK(n == 30);
    CHECK(!strcmp(st[0].name, "FIP") && !strcmp(meta[0].detail, "AAC 107"));
    free(t);

    /* what's left out, and what's kept */
    static const char odd[] =
        "[{\"name\":\"WMA\",\"url\":\"http://a.example/1\",\"url_resolved\":\"\",\"codec\":\"WMA\",\"bitrate\":64,\"hls\":0},"
        " {\"name\":\"TV\",\"url_resolved\":\"https://a.example/2.m3u8\",\"codec\":\"AAC,H.264\",\"hls\":1},"
        " {\"name\":\"Unknown\",\"url_resolved\":\"https://a.example/3\",\"codec\":\"UNKNOWN\",\"hls\":0},"
        " {\"name\":\"HLS\",\"url_resolved\":\"https://a.example/4.m3u8\",\"codec\":\"UNKNOWN\",\"hls\":1,\"stationuuid\":\"9617a958-0601-11e8-ae97-52543be04c81\"},"
        " {\"name\":\"Twice\",\"url_resolved\":\"https://a.example/4.m3u8\",\"codec\":\"AAC\",\"hls\":1},"
        " {\"name\":\" Line\\nbreak\\t\\t here \",\"url\":\"http://a.example/5\",\"url_resolved\":\"\",\"codec\":\"mp3\",\"bitrate\":128},"
        " {\"name\":\"FTP\",\"url_resolved\":\"ftp://a.example/6\",\"codec\":\"MP3\"},"
        " {\"name\":\"Spaces\",\"url_resolved\":\"http://a.example/a b\",\"codec\":\"MP3\"},"
        " {\"name\":\"\",\"url_resolved\":\"http://a.example/7\",\"codec\":\"MP3\"},"
        " {\"name\":\"Dead\",\"url_resolved\":\"http://a.example/8\",\"codec\":\"MP3\",\"lastcheckok\":0},"
        " {\"name\":\"\\u00c9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9"
        "\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9x\","
        "  \"url_resolved\":\"https://a.example/9\",\"codec\":\"OGG\",\"bitrate\":0,\"stationuuid\":\"bad id!\"},"
        " {\"name\":\"Opus\",\"url_resolved\":\"https://a.example/10\",\"codec\":\"OPUS\",\"bitrate\":96},"
        " \"not an object\", 3]";
    n = rdir_parse_stations(odd, strlen(odd), st, meta, RDIR_MAX_STATIONS);
    CHECK(n == 4);
    CHECK(n > 0 && !strcmp(st[0].name, "HLS") && !strcmp(meta[0].detail, "HLS") && meta[0].uuid[0]);
    CHECK(n > 1 && !strcmp(st[1].name, "Line break here") && !strcmp(st[1].url, "http://a.example/5") &&
          !strcmp(meta[1].detail, "MP3 128"));
    CHECK(n > 2 && strlen(st[2].name) == 62 && (unsigned char)st[2].name[60] == 0xC3);   /* 31 whole É/é */
    CHECK(n > 2 && !strcmp(meta[2].detail, "OGG") && meta[2].uuid[0] == '\0');
    CHECK(n > 3 && !strcmp(st[3].name, "Opus") && !strcmp(meta[3].detail, "OPUS 96"));
    CHECK(rdir_parse_stations("<html>Bad gateway</html>", 24, st, meta, 8) == -1);
    CHECK(rdir_parse_stations("[]", 2, st, meta, 8) == 0);
    CHECK(rdir_parse_stations("", 0, st, meta, 8) == -1);

    CHECK(rdir_playable("MP3", 0) && rdir_playable("aac+", 0) && rdir_playable("FLAC", 0));
    CHECK(rdir_playable("UNKNOWN", 1) && !rdir_playable("UNKNOWN", 0));
    CHECK(!rdir_playable("WMA", 0) && !rdir_playable("MP3,H.264", 1) && !rdir_playable("", 0));

    /* countries: by their short names */
    rdir_country_t c[RDIR_MAX_COUNTRIES];
    t = slurp("countries.json", &len);
    n = rdir_parse_countries(t, len, c, RDIR_MAX_COUNTRIES);
    CHECK(n == 223);                              /* (one has no name) */
    int gb = -1, sorted = 1, bahamas = 0, antigua = 0, the = 0;
    for (int i = 0; i < n; i++) {
        if (!strcmp(c[i].code, "GB")) gb = i;
        if (i && strcasecmp(c[i - 1].name, c[i].name) > 0) sorted = 0;
        if (!strcmp(c[i].name, "Bahamas")) bahamas = 1;
        if (!strcmp(c[i].name, "Antigua and Barbuda")) antigua = 1;
        if (!strncmp(c[i].name, "The ", 4)) the = 1;
    }
    CHECK(gb >= 0 && !strcmp(c[gb].name, "United Kingdom") && c[gb].count == 1939);
    CHECK(sorted && bahamas && antigua && !the);
    CHECK(rdir_parse_countries(t, len, c, 10) == 10);
    free(t);

    /* the requests */
    char url[512];
    rdir_query_t q = {RDIR_NAME, "fip jazz \xC3\xA9&x", 0};
    rdir_stations_url(url, sizeof url, "de1.api.radio-browser.info", &q);
    CHECK(!strcmp(url, "https://de1.api.radio-browser.info/json/stations/search?name=fip%20jazz%20%C3%A9%26x&"
                       "hidebroken=true&order=clickcount&reverse=true&limit=80"));
    q = (rdir_query_t){RDIR_TAG, "drum and bass", 1};
    rdir_stations_url(url, sizeof url, "x", &q);
    CHECK(!strcmp(url, "https://x/json/stations/search?tag=drum%20and%20bass&tagExact=true&hidebroken=true&order=random&limit=80"));
    q = (rdir_query_t){RDIR_POPULAR, "", 0};
    rdir_stations_url(url, sizeof url, "x", &q);
    CHECK(!strcmp(url, "https://x/json/stations/search?hidebroken=true&order=clickcount&reverse=true&limit=80"));
    rdir_countries_url(url, sizeof url, "x");
    CHECK(!strcmp(url, "https://x/json/countries?order=name&hidebroken=true"));
    char tag[64];
    rdir_genre_tag(0, tag, sizeof tag);
    CHECK(!strcmp(RDIR_GENRES[0], "Pop") && !strcmp(tag, "pop"));
    for (int i = 0; i < RDIR_N_GENRES; i++) {
        rdir_genre_tag(i, tag, sizeof tag);
        if (!strcmp(RDIR_GENRES[i], "Hip hop")) CHECK(!strcmp(tag, "hiphop"));
        if (!strcmp(RDIR_GENRES[i], "Drum and bass")) CHECK(!strcmp(tag, "drum and bass"));
    }

    /* on the worker thread */
    rdir_result_t r;
    rdir_set_fetch(fixture_fetch);
    CHECK(rdir_poll(&r) == -1 && !rdir_busy());
    q = (rdir_query_t){RDIR_TAG, "jazz", 0};
    CHECK(rdir_start(&q) == 0 && rdir_busy());
    CHECK(wait_done(&r) == 1);
    CHECK(r.n == 24 && r.st && r.meta && !r.countries && !strcmp(r.st[0].name, "Classic Vinyl HD"));
    CHECK(!rdir_busy());
    rdir_result_free(&r);
    q.random = 1;
    CHECK(rdir_start(&q) == 0 && wait_done(&r) == 1 && r.n == 16);
    rdir_result_free(&r);
    CHECK(rdir_start(NULL) == 0 && wait_done(&r) == 1 && r.n == 223 && r.countries && !r.st);
    rdir_result_free(&r);

    /* no server answers: each is tried once, then it fails */
    fail_all = 1;
    calls = 0;
    CHECK(rdir_start(&q) == 0 && wait_done(&r) == 1 && r.n == -1 && !r.st);
    CHECK(calls == 3);                            /* (all.api, de1, de2) */
    fail_all = 0;

    /* cancelled while it waits: the answer is dropped, the thread ends by itself */
    slow = 1;
    calls = 0;
    CHECK(rdir_start(&q) == 0);
    nap(50);
    rdir_cancel();
    CHECK(rdir_poll(&r) == -1 && !rdir_busy());
    nap(200);
    CHECK(calls == 1);                            /* aborted: no other server tried */
    slow = 0;
    /* a new request replaces one that's running */
    CHECK(rdir_start(&q) == 0 && rdir_start(NULL) == 0 && wait_done(&r) == 1 && r.countries && r.n == 223);
    rdir_result_free(&r);

    /* counting a play */
    calls = 0;
    rdir_count_click("9617a958-0601-11e8-ae97-52543be04c81");
    for (int i = 0; i < 100 && !calls; i++) nap(10);
    pthread_mutex_lock(&mu);
    CHECK(calls == 1 && strstr(last_url, "/json/url/9617a958-0601-11e8-ae97-52543be04c81"));
    pthread_mutex_unlock(&mu);
    nap(50);

    if (!fails) printf("radiodir: ok\n");
    return fails != 0;
}
