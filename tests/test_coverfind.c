/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* Covers found online (net/coverfind.h): names compared, answers read and
 * rated, and the worker, with MusicBrainz's and Apple's answers from saved
 * files (tests/data/covers) and a made-up picture. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "net/coverfind.h"
#include "tmpdir.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    fclose(f);
    if (b) { b[n] = '\0'; *len = (size_t)n; }
    return b;
}

static cf_album_t album(int64_t id, const char *artist, const char *title, int year)
{
    cf_album_t a;
    memset(&a, 0, sizeof a);
    a.id = id;
    snprintf(a.artist, sizeof a.artist, "%s", artist);
    snprintf(a.title, sizeof a.title, "%s", title);
    snprintf(a.key, sizeof a.key, "%s\x1f%s", artist, title);
    a.year = year;
    return a;
}

static void norm_is(const char *in, int title, const char *want)
{
    char out[256];
    cf_norm(in, title, out, sizeof out);
    CHECK(!strcmp(out, want), "cf_norm(\"%s\", %d) = \"%s\", not \"%s\"", in, title, out, want);
}

static void test_norm(void)
{
    norm_is("The Beatles", 0, "beatles");
    norm_is("Sigur R\xc3\xb3s", 0, "sigur ros");
    norm_is("Simon & Garfunkel", 0, "simon and garfunkel");
    norm_is("  AC/DC ", 0, "ac dc");
    norm_is("Bj\xc3\xb6rk", 0, "bjork");
    norm_is("Stra\xc3\x9f" "e", 0, "strasse");
    norm_is("Dvo\xc5\x99\xc3\xa1k", 0, "dvorak");
    norm_is("Don\xe2\x80\x99t Stop", 1, "dont stop");
    norm_is("OK Computer (Remastered 2011)", 1, "ok computer");
    norm_is("Abbey Road [Super Deluxe Edition]", 1, "abbey road");
    norm_is("Blue - EP", 1, "blue");
    norm_is("Hello - Single", 1, "hello");
    norm_is("Alive (Live)", 1, "alive live");          /* (not an edition: kept) */
    norm_is("Epic (Expanded)", 1, "epic");             /* ("ep" only as a word) */
    norm_is("\xe6\x9d\xb1\xe4\xba\xac", 0, "\xe6\x9d\xb1\xe4\xba\xac");   /* another script: as it is */
}

static void test_urls(void)
{
    char u[1600];
    cf_album_t a = album(1, "Radiohead", "OK Computer", 1997);
    cf_mb_url(&a, u, sizeof u);
    CHECK(strstr(u, "https://musicbrainz.org/ws/2/release-group/?query=releasegroup%3A%22OK%20Computer%22"
                    "%20AND%20artist%3A%22Radiohead%22&fmt=json&limit=8"), "%s", u);
    cf_apple_url(&a, u, sizeof u);
    CHECK(strstr(u, "itunes.apple.com/search?media=music&entity=album&limit=8&term=Radiohead+OK+Computer"), "%s", u);
    cf_album_t q = album(1, "Say \"Hi\"", "A\\B", 0);
    cf_mb_url(&q, u, sizeof u);
    CHECK(strstr(u, "%22A%5C%5CB%22") && strstr(u, "%22Say%20%5C%22Hi%5C%22%22"), "escaped: %s", u);
    cf_album_t v = album(1, "Various Artists", "Now 10", 0);
    cf_apple_url(&v, u, sizeof u);
    CHECK(strstr(u, "&term=Now+10"), "a compilation: its title alone: %s", u);
}

static void test_parse(void)
{
    size_t len;
    cf_match_t m[16];
    char *j = slurp("tests/data/covers/mb_okcomputer.json", &len);
    CHECK(j, "mb_okcomputer.json");
    cf_album_t okc = album(1, "Radiohead", "OK Computer", 1997);
    int n = j ? cf_parse_mb(j, len, &okc, m, 16) : 0;
    CHECK(n >= 1 && !strcmp(m[0].title, "OK Computer") && !strcmp(m[0].artist, "Radiohead") && m[0].year == 1997 &&
          m[0].sure && strstr(m[0].image, "coverartarchive.org/release-group/b1392450-e666-3926-a536-22c65f834433/front-250"),
          "MusicBrainz: OK Computer (%d: %s / %s %d)", n, n ? m[0].title : "", n ? m[0].artist : "", n ? m[0].year : 0);
    CHECK(cf_pick(&okc, m, n) == 0, "picked");
    cf_album_t remaster = album(1, "Radiohead", "OK Computer (Remastered)", 0);
    n = j ? cf_parse_mb(j, len, &remaster, m, 16) : 0;
    CHECK(n >= 1 && m[0].sure && cf_pick(&remaster, m, n) == 0, "an edition's note doesn't matter");
    cf_album_t other = album(1, "Radiohead", "OK Computer", 2017);
    n = j ? cf_parse_mb(j, len, &other, m, 16) : 0;
    CHECK(n >= 1 && !m[0].sure && cf_pick(&other, m, n) < 0, "another year (2017): not sure");
    free(j);

    j = slurp("tests/data/covers/apple_okcomputer.json", &len);
    n = j ? cf_parse_apple(j, len, &okc, m, 16) : 0;
    CHECK(n == 5 && !strcmp(m[0].title, "OK Computer") && m[0].sure && strstr(m[0].image, "/300x300bb.jpg"),
          "Apple: OK Computer first, at 600 px (%d)", n);
    int sure = 0;
    for (int i = 0; i < n; i++) sure += m[i].sure;
    CHECK(sure == 1, "the tributes aren't sure (%d sure)", sure);
    CHECK(cf_pick(&okc, m, n) == 0, "picked");
    free(j);

    j = slurp("tests/data/covers/mb_weezer.json", &len);
    cf_album_t green = album(1, "Weezer", "Weezer", 2001), blue = album(1, "Weezer", "Weezer", 0);
    n = j ? cf_parse_mb(j, len, &green, m, 16) : 0;
    int k = cf_pick(&green, m, n);
    CHECK(k >= 0 && m[k].year == 2001, "Weezer (2001): the Green Album (%d)", k >= 0 ? m[k].year : 0);
    n = j ? cf_parse_mb(j, len, &blue, m, 16) : 0;
    CHECK(n >= 7 && cf_pick(&blue, m, n) < 0, "Weezer, no year: not taken (%d)", n);
    free(j);

    j = slurp("tests/data/covers/mb_nevermind.json", &len);
    cf_album_t nm = album(1, "Nirvana", "Nevermind", 1991);
    n = j ? cf_parse_mb(j, len, &nm, m, 16) : 0;
    k = cf_pick(&nm, m, n);
    CHECK(k >= 0 && !strcmp(m[k].title, "Nevermind") && m[k].year == 1991, "Nevermind (1991)");
    cf_album_t nm0 = album(1, "Nirvana", "Nevermind", 0);
    n = j ? cf_parse_mb(j, len, &nm0, m, 16) : 0;
    k = cf_pick(&nm0, m, n);
    CHECK(k >= 0 && m[k].year == 1991, "Nevermind, no year: the one album called that");
    free(j);

    CHECK(cf_parse_mb("{\"error\": \"busy\"}", 17, &okc, m, 16) < 0, "not an answer");
    CHECK(cf_parse_apple("{\"resultCount\":0,\"results\":[]}", 30, &okc, m, 16) == 0, "no results");
}

/* ---------- the worker ---------- */

static int net_down;

static volatile int gate, gated;   /* gate: the next question waits (gated: one is waiting) */

static int mb_asked, apple_asked;   /* questions put to each */

static long fake_fetch(const char *url, char *buf, size_t cap, const volatile int *abort)
{
    (void)abort;
    while (gate) { gated = 1; usleep(1000); }
    if (strstr(url, "musicbrainz.org")) mb_asked++;
    else apple_asked++;
    if (net_down) return -2;                           /* (-2: no retries, so the test doesn't wait) */
    const char *file = NULL;
    if (strstr(url, "musicbrainz.org")) {
        file = strstr(url, "OK%20Computer") ? "tests/data/covers/mb_okcomputer.json"
             : strstr(url, "Weezer") ? "tests/data/covers/mb_weezer.json" : NULL;
        if (!file) { snprintf(buf, cap, "{\"release-groups\":[]}"); return (long)strlen(buf); }
    } else {
        file = strstr(url, "OK+Computer") ? "tests/data/covers/apple_okcomputer.json" : NULL;
        if (!file) { snprintf(buf, cap, "{\"resultCount\":0,\"results\":[]}"); return (long)strlen(buf); }
    }
    FILE *f = fopen(file, "rb");
    if (!f) return -1;
    size_t n = fread(buf, 1, cap - 1, f);
    fclose(f);
    buf[n] = '\0';
    return (long)n;
}

static int downloads, flaky, net_fails;
static int card_full, full_tries;   /* the card nearly full: how often the first picture asked for was tried */
static char full_url[512];

static int64_t fake_download(const char *url, const char *path, http_progress_fn progress, void *ctx,
                             const volatile int *abort)
{
    (void)progress; (void)ctx; (void)abort;
    downloads++;
    if (card_full) {
        if (!full_url[0]) snprintf(full_url, sizeof full_url, "%s", url);
        if (!strcmp(url, full_url)) full_tries++;
        return HTTP_ERR_FULL;
    }
    if (strstr(url, "coverartarchive.org") && strstr(url, "/release-group/1a5ae023")) return -3;   /* (no picture) */
    if (net_fails) { net_fails--; return -1; }       /* (a name that didn't resolve) */
    if (flaky) return -1;
    size_t len;
    char *b = slurp("tests/data/covers/cover.jpg", &len);
    FILE *f = b ? fopen(path, "wb") : NULL;
    if (f) { fwrite(b, 1, len, f); fclose(f); }
    free(b);
    return f ? (int64_t)len : -1;
}

static int exists(const char *p) { struct stat st; return stat(p, &st) == 0; }

static void test_worker(void)
{
    char dir[600], p[1200];
    if (!mkdtemp(test_tmp(dir, sizeof dir, "coverfind"))) { printf("FAIL: mkdtemp\n"); fails++; return; }
    library_set_found_covers(dir);
    cf_set_fetch(fake_fetch);
    cf_set_download(fake_download);
    cf_set_mb_gap_ms(0);
    cf_set_retry_ms(10);
    CHECK(cf_start(dir) == 0, "start");

    /* one album: its matches, the first one's picture, kept */
    cf_album_t okc = album(7, "Radiohead", "OK Computer", 1997);
    cf_find(&okc);
    int n = 0, st = 0;
    for (int i = 0; i < 500 && (st = cf_find_state(&n)) == 0; i++) usleep(10000);
    CHECK(st == 1 && n >= 2, "found (%d, %d matches)", st, n);
    static uint16_t large[LIB_ART_LARGE * LIB_ART_LARGE], small[LIB_ART_SMALL * LIB_ART_SMALL];
    cf_match_t m;
    int r = 0;
    for (int i = 0; i < 500 && (r = cf_find_show(0, &m, large)) == 0; i++) usleep(10000);
    CHECK(r == 1 && !strcmp(m.title, "OK Computer"), "its picture (%d)", r);
    int any = 0;
    for (int i = 0; i < LIB_ART_LARGE * LIB_ART_LARGE; i++) any |= large[i];
    CHECK(any, "decoded");
    library_found_cover_path(okc.key, "none", p, sizeof p);
    FILE *f = fopen(p, "w");                           /* (removed once before: asking again undoes that) */
    if (f) fclose(f);
    CHECK(cf_find_keep(0, large, small) == 0, "kept");
    library_found_cover_path(okc.key, "jpg", p, sizeof p);
    CHECK(exists(p), "saved as %s", p);
    library_found_cover_path(okc.key, "none", p, sizeof p);
    CHECK(!exists(p), "the .none is gone");
    CHECK(cf_find_keep(5, large, small) < 0, "one not fetched: not kept");
    cf_find_close();
    CHECK(cf_find_state(NULL) == -1, "closed");

    /* the network failing now and then: a picture tried again; never there: tried when asked */
    net_fails = 2;                                     /* (two failures: the third try gets it) */
    cf_find(&okc);
    for (int i = 0; i < 500 && (st = cf_find_state(&n)) == 0; i++) usleep(10000);
    for (int i = 0; i < 500 && (r = cf_find_show(0, &m, large)) == 0; i++) usleep(10000);
    CHECK(r == 1 && net_fails == 0, "fetched at the third try (%d)", r);
    cf_find_close();
    flaky = 1;
    cf_find(&okc);
    for (int i = 0; i < 500 && (st = cf_find_state(&n)) == 0; i++) usleep(10000);
    for (int i = 0; i < 500 && (r = cf_find_show(0, &m, large)) == 0; i++) usleep(10000);
    CHECK(r == -2, "couldn't be fetched: says so (%d)", r);
    flaky = 0;
    CHECK(cf_find_retry(0) == 0, "asked again");
    for (int i = 0; i < 500 && (r = cf_find_show(0, &m, large)) == 0; i++) usleep(10000);
    CHECK(r == 1, "then here (%d)", r);
    CHECK(cf_find_retry(0) < 0, "(one that's here isn't asked again)");
    cf_find_close();
    /* the card nearly full: the picture isn't here, and wasn't tried three times */
    card_full = 1;
    cf_find(&okc);
    for (int i = 0; i < 500 && (st = cf_find_state(&n)) == 0; i++) usleep(10000);
    for (int i = 0; i < 500 && (r = cf_find_show(0, &m, large)) == 0; i++) usleep(10000);
    CHECK(r == -2 && full_tries == 1, "card full: %d, tried %d times", r, full_tries);
    cf_find_close();
    card_full = 0;

    /* every album without a cover, once */
    cf_album_t list[4] = {album(1, "Radiohead", "OK Computer", 1997), album(2, "Weezer", "Weezer", 0),
                          album(3, "Nobody", "Nothing Like It", 2000), album(4, "Removed", "Its Cover", 2000)};
    library_found_cover_path(list[3].key, "none", p, sizeof p);
    f = fopen(p, "w");
    if (f) fclose(f);
    library_found_cover_path(list[0].key, "jpg", p, sizeof p);
    remove(p);                                         /* (the one kept above: as if it had none) */
    mb_asked = apple_asked = 0;
    CHECK(cf_batch(list, 4) == 0, "batch");
    cf_batch_t b;
    int taken = 0;
    int64_t id = 0;
    for (int i = 0; i < 1000; i++) {
        cf_batch_status(&b);
        while (cf_batch_take(&id, large, small)) taken++;
        if (!b.running) break;
        usleep(10000);
    }
    while (cf_batch_take(&id, large, small)) taken++;
    CHECK(!b.running && b.total == 3 && b.done == 3, "done (%d of %d)", b.done, b.total);
    CHECK(b.found == 1 && b.unsure == 1 && b.none == 1 && !b.failed, "found %d, unsure %d, none %d", b.found, b.unsure, b.none);
    CHECK(taken == 1 && id == 1, "OK Computer's to store (%d, %lld)", taken, (long long)id);
    /* Apple asked only for the two MusicBrainz had no sure picture for */
    CHECK(mb_asked == 3 && apple_asked == 2, "asked MusicBrainz %d times, Apple %d", mb_asked, apple_asked);
    library_found_cover_path(list[1].key, "jpg", p, sizeof p);
    CHECK(!exists(p), "Weezer's: not taken");

    /* an album given a cover by Menu > Find cover while the batch is on its
     * way to it: the batch leaves it (before: its own renamed over the kept one) */
    cf_find(&list[0]);
    for (int i = 0; i < 500 && (st = cf_find_state(&n)) == 0; i++) usleep(10000);
    for (int i = 0; i < 500 && (r = cf_find_show(0, &m, large)) == 0; i++) usleep(10000);
    CHECK(r == 1, "a find for the batch's album (%d)", r);
    gate = 1;                                           /* (the batch waits in its first question) */
    taken = 0;
    CHECK(cf_batch(list, 1) == 0, "batch again");
    for (int i = 0; i < 200 && !gated; i++) usleep(10000);
    CHECK(cf_find_keep(0, large, small) == 0, "kept from the find meanwhile");
    library_found_cover_path(list[0].key, "jpg", p, sizeof p);
    f = fopen(p, "w");
    if (f) { fputs("MINE", f); fclose(f); }             /* (to tell it from the batch's) */
    gate = 0;
    for (int i = 0; i < 1000; i++) {
        cf_batch_status(&b);
        while (cf_batch_take(&id, large, small)) taken++;
        if (!b.running) break;
        usleep(10000);
    }
    while (cf_batch_take(&id, large, small)) taken++;
    cf_find_close();
    char mine[8] = "";
    f = fopen(p, "r");
    if (f) { if (!fgets(mine, sizeof mine, f)) mine[0] = '\0'; fclose(f); }
    CHECK(taken == 0 && !strcmp(mine, "MINE"), "a cover kept meanwhile left as it is (%d taken, [%s])", taken, mine);
    CHECK(b.total == 0 && b.done == 0 && b.found + b.unsure + b.none == b.done, "not counted as looked at (%d of %d)",
          b.done, b.total);

    /* the batch's picture renamed in, then the player stopped before storing
     * it (the album still has none): the next batch looks again and stores it */
    f = fopen(p, "w");
    if (f) { fputs("STALE", f); fclose(f); }
    taken = 0;
    CHECK(cf_batch(list, 1) == 0, "batch after a stop");
    for (int i = 0; i < 1000; i++) {
        cf_batch_status(&b);
        while (cf_batch_take(&id, large, small)) taken++;
        if (!b.running) break;
        usleep(10000);
    }
    while (cf_batch_take(&id, large, small)) taken++;
    CHECK(taken == 1 && id == 1 && b.done == 1 && b.found == 1, "looked at again and taken (%d, %d done)", taken, b.done);

    /* no network: it stops, and says so */
    remove(p);
    net_down = 1;
    unsigned seq = b.seq;
    cf_batch(list, 3);
    for (int i = 0; i < 500; i++) {
        cf_batch_status(&b);
        if (!b.running && b.seq != seq) break;
        usleep(10000);
    }
    CHECK(!b.running && b.failed && b.done == 0, "failed (%d, %d)", b.failed, b.done);
    net_down = 0;

    /* stopped partway */
    cf_batch(list, 3);
    cf_batch_stop();
    for (int i = 0; i < 500; i++) {
        cf_batch_status(&b);
        if (!b.running) break;
        usleep(10000);
    }
    CHECK(!b.running && !b.failed && b.done <= 1, "stopped (%d done)", b.done);
    cf_stop();
    /* pictures whose download was cut short (the player killed, the power gone): gone at the next start */
    const char *left[] = {".find-0.jpg.part", ".find-3.jpg.part", ".batch.jpg.part"};
    for (int i = 0; i < 3; i++) {
        snprintf(p, sizeof p, "%s/%s", dir, left[i]);
        f = fopen(p, "wb");
        if (f) { fputs("x", f); fclose(f); }
    }
    CHECK(cf_start(dir) == 0, "started again");
    for (int i = 0; i < 3; i++) {
        struct stat sb;
        snprintf(p, sizeof p, "%s/%s", dir, left[i]);
        CHECK(stat(p, &sb) != 0, "%s removed", left[i]);
    }
    cf_stop();
    char cmd[700];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    if (system(cmd)) {}
}

int main(void)
{
    CHECK(sizeof "aaaaaa" "cccccccc" "dddd" "eeeeeeeeee" "gggggggg" "hhhh" "iiiiiiiiii" "11" "jj"
          "kkk" "llllllllll" "nnnnnnnnn" "oooooo" "22" "rrrrrr" "ssssssss" "tttttt"
          "uuuuuuuuuuuu" "ww" "yyy" "zzzzzz" "s" == 129, "Latin Extended-A: 128 letters");
    test_norm();
    test_urls();
    test_parse();
    test_worker();
    printf("%s (%d failed)\n", fails ? "FAILED" : "coverfind: ok", fails);
    return fails != 0;
}
