/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Podcasts: feeds, OPML, search answers, chapters, and the files on the card (net/podcast.h). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "net/podcast.h"
#include "tmpdir.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) { printf("FAIL: can't read %s\n", path); exit(1); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    *len = fread(b, 1, (size_t)n, f);
    b[*len] = '\0';
    fclose(f);
    return b;
}

static void dates(void)
{
    CHECK(pod_parse_date("Tue, 19 Nov 2026 05:00:00 +0000") == 1795064400LL, "rfc822 %lld",
          (long long)pod_parse_date("Tue, 19 Nov 2026 05:00:00 +0000"));
    CHECK(pod_parse_date("19 Nov 2026 05:00:00 GMT") == 1795064400LL, "no weekday");
    CHECK(pod_parse_date("Tue, 19 Nov 2026 00:00:00 -0500") == 1795064400LL, "offset");
    CHECK(pod_parse_date("Mon, 18 Nov 2026 21:00:00 PST") == 1795064400LL, "PST");
    CHECK(pod_parse_date("2026-11-19T05:00:00Z") == 1795064400LL, "ISO 8601");
    CHECK(pod_parse_date("Tue, 19 Nov 26 05:00:00 +0000") == 1795064400LL, "two-digit year");
    CHECK(pod_parse_date("") == 0 && pod_parse_date("soon") == 0 && pod_parse_date(NULL) == 0, "not a date");
    CHECK(pod_parse_duration("3600") == 3600 && pod_parse_duration("59:12") == 3552 &&
          pod_parse_duration("1:02:03") == 3723 && pod_parse_duration("") == 0 && pod_parse_duration("x") == 0,
          "durations");
}

static void feed(void)
{
    size_t len;
    char *xml = slurp("tests/data/podcasts/feed.xml", &len);
    pod_show_t s;
    memset(&s, 0, sizeof s);
    pod_episode_t e[10];
    int n = pod_parse_feed(xml, len, &s, e, 10);
    CHECK(n == 3, "episodes: %d", n);
    CHECK(!strcmp(s.title, "The Slow & Steady Show"), "title [%s]", s.title);
    CHECK(!strcmp(s.author, "Ana & Bo"), "author [%s]", s.author);
    CHECK(!strcmp(s.image, "https://example.com/cover.jpg"), "itunes:image wins [%s]", s.image);
    if (n == 3) {
        CHECK(!strcmp(e[0].title, "Episode 3 (newest, listed out of order)"), "newest first [%s]", e[0].title);
        CHECK(e[0].date == pod_parse_date("Wed, 20 Nov 2026 09:30:00 PST") && e[0].dur_s == 3600, "ep3 date/length");
        CHECK(!strcmp(e[0].url, "https://cdn.example.com/ep3.m4a") && !strcmp(pod_ext(&e[0]), ".m4a"), "ep3 url [%s]", e[0].url);
        CHECK(!strcmp(e[1].title, "Episode 2: Caf\xc3\xa9 \xe2\x80\x94 \"Live\""), "entities [%s]", e[1].title);
        CHECK(!strcmp(e[1].url, "https://cdn.example.com/ep2.mp3?src=rss&x=1"), "url entity [%s]", e[1].url);
        CHECK(e[1].size == 61234567 && e[1].dur_s == 3723, "size/duration");
        CHECK(!strcmp(e[1].chapters, "https://example.com/ep2/chapters.json"), "chapters [%s]", e[1].chapters);
        char k[13];
        pod_hash("ep-2", k);
        CHECK(!strcmp(e[1].key, k), "key from the guid");
        pod_hash("https://cdn.example.com/ep1.mp3", k);
        CHECK(!strcmp(e[2].key, k) && e[2].dur_s == 3552, "no guid: key from the url");
        CHECK(!strcmp(pod_ext(&e[2]), ".mp3"), "ext");
    }
    /* cut anywhere: never a crash, whole episodes only */
    for (size_t cut = 0; cut < len; cut += 7) {
        int m = pod_parse_feed(xml, cut, &s, e, 10);
        CHECK(m <= 3, "cut %zu: %d", cut, m);
    }
    /* at most max, the newest */
    n = pod_parse_feed(xml, len, &s, e, 1);
    CHECK(n == 1 && !strncmp(e[0].title, "Episode 3", 9), "max 1");
    CHECK(pod_parse_feed("<html><body>Not a feed</body></html>", 36, &s, e, 10) == -1, "not a feed");
    free(xml);
}

static void opml_search_chapters(void)
{
    size_t len;
    char *x = slurp("tests/data/podcasts/subs.opml", &len);
    pod_show_t s[8];
    int n = pod_parse_opml(x, len, s, 8);
    CHECK(n == 2, "opml shows: %d", n);
    if (n == 2) {
        CHECK(!strcmp(s[0].title, "First & Best") && !strcmp(s[0].url, "https://feeds.example.com/first.xml"), "opml 1");
        CHECK(!strcmp(s[1].title, "Only a title") && !strcmp(s[1].url, "http://feeds.example.com/second"), "opml 2");
    }
    free(x);

    x = slurp("tests/data/podcasts/search.json", &len);
    n = pod_parse_search(x, len, s, 8);
    CHECK(n == 2, "search: %d", n);
    if (n == 2) {
        CHECK(!strcmp(s[0].title, "Radiolab") && !strcmp(s[0].author, "WNYC Studios") && s[0].episodes == 575, "search 1");
        CHECK(!strcmp(s[1].title, "Caf\xc3\xa9 Talk") && s[1].episodes == 12, "search 2 [%s]", s[1].title);
    }
    CHECK(pod_parse_search("{}", 2, s, 8) == -1, "not an answer");
    free(x);
    char url[512];
    pod_search_url(url, sizeof url, "slow & steady");
    CHECK(!strcmp(url, "https://itunes.apple.com/search?media=podcast&entity=podcast&limit=25&term=slow+%26+steady"),
          "search url [%s]", url);

    x = slurp("tests/data/podcasts/chapters.json", &len);
    pod_chapter_t c[8];
    n = pod_parse_chapters(x, len, c, 8);
    CHECK(n == 3, "chapters: %d (toc:false left out)", n);
    if (n == 3) {
        CHECK(c[0].start_ms == 0 && !strcmp(c[0].title, "Intro"), "chapter 1");
        CHECK(c[1].start_ms == 95500 && !strcmp(c[1].title, "The \xe2\x80\x9c" "big\xe2\x80\x9d story"), "chapter 2 [%s]", c[1].title);
        CHECK(c[2].start_ms == 1800000, "chapter 3");
    }
    free(x);
}

static void card(void)
{
    char tmpl[512], dir[600];
    test_tmp(tmpl, sizeof tmpl, "podcast");
    if (!mkdtemp(tmpl)) { printf("FAIL: mkdtemp\n"); fails++; return; }
    snprintf(dir, sizeof dir, "%s/podcasts", tmpl);

    pod_show_t a = {0}, b = {0}, got[8];
    snprintf(a.title, sizeof a.title, "Show = A");          /* (an " = " in a title) */
    snprintf(a.url, sizeof a.url, "https://a.example/feed");
    snprintf(b.title, sizeof b.title, "Show B");
    snprintf(b.url, sizeof b.url, "http://b.example/rss?x=1");
    CHECK(pod_shows_load(dir, got, 8) == 0, "no shows yet");
    CHECK(pod_shows_add(dir, &a) == 1 && pod_shows_add(dir, &b) == 1, "added");
    CHECK(pod_shows_add(dir, &a) == 0, "already there");
    int n = pod_shows_load(dir, got, 8);
    CHECK(n == 2 && !strcmp(got[0].title, "Show = A") && !strcmp(got[0].url, a.url) && !strcmp(got[1].url, b.url),
          "loaded in order: %d [%s] [%s]", n, n > 0 ? got[0].title : "", n > 1 ? got[1].url : "");

    /* details and episodes */
    pod_show_t d = got[0];
    snprintf(d.author, sizeof d.author, "Someone");
    d.refreshed = 1795064400;
    d.seen = 1795054400;
    CHECK(pod_show_save(dir, &d) == 0 && pod_show_seen(dir, d.id, d.seen) == 0, "show saved");
    n = pod_shows_load(dir, got, 8);
    CHECK(n == 2 && !strcmp(got[0].author, "Someone") && got[0].refreshed == 1795064400 && got[0].seen == 1795054400,
          "details read back");

    size_t len;
    char *xml = slurp("tests/data/podcasts/feed.xml", &len);
    pod_show_t f;
    pod_episode_t e[10], back[10];
    int ne = pod_parse_feed(xml, len, &f, e, 10);
    free(xml);
    CHECK(pod_episodes_save(dir, d.id, e, ne) == 0, "episodes saved");
    CHECK(pod_episodes_load(dir, d.id, back, 10) == ne, "episodes read back");
    for (int i = 0; i < ne; i++)
        CHECK(!memcmp(&e[i], &back[i], sizeof e[i]), "episode %d the same", i);

    /* state */
    pod_state_t s = {0}, st[8], one;
    snprintf(s.key, sizeof s.key, "%s", e[1].key);
    s.pos_ms = 123456;
    s.len_ms = 3723000;
    CHECK(pod_state_put(dir, d.id, &s) == 0, "state saved");
    s.pos_ms = 0;
    s.played = 1;
    s.played_at = 1795064400;
    snprintf(s.key, sizeof s.key, "%s", e[0].key);
    CHECK(pod_state_put(dir, d.id, &s) == 0, "second state");
    n = pod_state_load(dir, d.id, st, 8);
    CHECK(n == 2, "states: %d", n);
    pod_state_find(st, n, e[1].key, &one);
    CHECK(one.pos_ms == 123456 && one.len_ms == 3723000 && !one.played, "position kept");
    pod_state_find(st, n, e[0].key, &one);
    CHECK(one.played && one.played_at == 1795064400, "played kept");
    pod_state_find(st, n, e[2].key, &one);
    CHECK(!one.played && !one.pos_ms && !strcmp(one.key, e[2].key), "none: zero, its key");

    /* a download */
    char p[800];
    pod_episode_path(dir, d.id, &e[0], p, sizeof p);
    CHECK(!pod_downloaded(dir, d.id, &e[0]), "not downloaded");
    FILE *w = fopen(p, "wb");
    if (w) { fputs("ID3", w); fclose(w); }
    CHECK(pod_downloaded(dir, d.id, &e[0]), "downloaded");
    CHECK(pod_delete_download(dir, d.id, &e[0]) == 0 && !pod_downloaded(dir, d.id, &e[0]), "deleted");

    /* unsubscribe: its line and folder go, the other stays */
    CHECK(pod_shows_remove(dir, a.url) == 0, "removed");
    n = pod_shows_load(dir, got, 8);
    CHECK(n == 1 && !strcmp(got[0].url, b.url), "one left");
    struct stat sb;
    char sd[700];
    snprintf(sd, sizeof sd, "%s/%s", dir, d.id);
    CHECK(stat(sd, &sb) != 0, "its folder gone");

    /* a hand-edited list: notes, blank lines, odd spacing */
    char path[700];
    snprintf(path, sizeof path, "%s/shows.txt", dir);
    w = fopen(path, "w");
    if (w) {
        fputs("# mine\n\n  Hand Added=https://h.example/feed  \nnot a show\nhttps://bare.example/rss\n", w);
        fclose(w);
    }
    n = pod_shows_load(dir, got, 8);
    CHECK(n == 2 && !strcmp(got[0].title, "Hand Added") && !strcmp(got[0].url, "https://h.example/feed") &&
          !strcmp(got[1].url, "https://bare.example/rss") && !strcmp(got[1].title, "https://bare.example/rss"),
          "hand-edited: %d [%s] [%s]", n, n > 0 ? got[0].title : "", n > 1 ? got[1].title : "");
}

/* An MP3 with chapters in its ID3 tag (CHAP frames, as podcast tools write
 * them): two chapters, then a second of silent frames. */
static void be32(unsigned char *p, uint32_t v) { p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16); p[2] = (unsigned char)(v >> 8); p[3] = (unsigned char)v; }

static size_t chap_frame(unsigned char *out, const char *id, uint32_t start, uint32_t end, const char *title)
{
    unsigned char sub[128];
    size_t tl = strlen(title);
    memcpy(sub, "TIT2", 4);
    be32(sub + 4, (uint32_t)(tl + 1));
    sub[8] = sub[9] = 0;
    sub[10] = 0;                                   /* ISO-8859-1 */
    memcpy(sub + 11, title, tl);
    size_t sl = 11 + tl, il = strlen(id) + 1, body = il + 16 + sl;
    memcpy(out, "CHAP", 4);
    be32(out + 4, (uint32_t)body);
    out[8] = out[9] = 0;
    memcpy(out + 10, id, il);
    be32(out + 10 + il, start);
    be32(out + 14 + il, end);
    be32(out + 18 + il, 0xFFFFFFFF);
    be32(out + 22 + il, 0xFFFFFFFF);
    memcpy(out + 26 + il, sub, sl);
    return 10 + body;
}

static void embedded_chapters(void)
{
    char tmpl[512], path[600];
    test_tmp(tmpl, sizeof tmpl, "podchap");
    if (!mkdtemp(tmpl)) return;
    snprintf(path, sizeof path, "%s/ep.mp3", tmpl);
    unsigned char tag[512];
    size_t n = 10;
    n += chap_frame(tag + n, "ch0", 0, 600, "Opening");
    n += chap_frame(tag + n, "ch1", 600, 1300, "Second part");
    size_t body = n - 10;
    memcpy(tag, "ID3\x03\x00\x00", 6);
    tag[6] = (unsigned char)((body >> 21) & 0x7F); tag[7] = (unsigned char)((body >> 14) & 0x7F);
    tag[8] = (unsigned char)((body >> 7) & 0x7F); tag[9] = (unsigned char)(body & 0x7F);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fwrite(tag, 1, n, f);
    unsigned char frame[417] = {0xFF, 0xFB, 0x90, 0xC4};   /* MPEG-1 layer III, 128 kb/s, 44.1 kHz, mono: silence */
    for (int i = 0; i < 40; i++) fwrite(frame, 1, sizeof frame, f);
    fclose(f);
    pod_chapter_t c[4];
    int k = pod_file_chapters(path, c, 4);
    printf("embedded chapters: %d%s\n", k, k ? "" : " (no FFmpeg in this build)");
    CHECK(k == 0 || (k == 2 && c[0].start_ms == 0 && !strcmp(c[0].title, "Opening") && c[1].start_ms == 600 &&
                     !strcmp(c[1].title, "Second part")), "embedded: %d", k);
    unlink(path);
    rmdir(tmpl);
}

int main(void)
{
    dates();
    feed();
    opml_search_chapters();
    card();
    embedded_chapters();
    if (fails) { printf("%d failed\n", fails); return 1; }
    printf("podcast: ok\n");
    return 0;
}
