/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* Podcasts' worker (net/podsync.h), with feeds and downloads from saved files. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "net/podsync.h"
#include "tmpdir.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static int fetches, slow, cut_off;   /* cut_off: the feed arrives cut off (Wi-Fi gone mid-transfer) */
static char alt_image[6];            /* the feed's image named this instead of "cover" (5 letters) */

static int offline;                  /* no server answers (Wi-Fi up, no internet) */
static char fetched[32][200];        /* the addresses asked for, in order (the first 32) */
static int in_log(const char *url)
{
    for (int i = 0; i < fetches && i < 32; i++)
        if (!strcmp(fetched[i], url)) return 1;
    return 0;
}

static long fake_fetch(const char *url, char *buf, size_t cap, const volatile int *abort)
{
    (void)abort;
    if (fetches < 32) snprintf(fetched[fetches], sizeof fetched[0], "%s", url);
    fetches++;
    if (offline) return -1;
    const char *file = strstr(url, "itunes.apple.com") ? "tests/data/podcasts/search.json"
                     : strstr(url, "chapters.json") ? "tests/data/podcasts/chapters.json"
                     : strstr(url, "broken") ? NULL : "tests/data/podcasts/feed.xml";
    if (!file) return -1;
    FILE *f = fopen(file, "rb");
    if (!f) return -1;
    size_t n = fread(buf, 1, cap - 1, f);
    fclose(f);
    buf[n] = '\0';
    if (strstr(file, "feed.xml")) {
        if (cut_off) {                                /* half of it, as http_get leaves it */
            n /= 2;
            buf[n] = '\0';
        } else {                                      /* (the fixture stops mid-item: whole, it ends) */
            n += (size_t)snprintf(buf + n, cap - n, "\n</rss>\n");
        }
        char *im = strstr(buf, "example.com/cover.jpg");
        if (im && alt_image[0]) memcpy(im + 12, alt_image, 5);   /* the show's image changed */
    }
    return (long)n;
}

/* A show's image (any .jpg): a real picture, or what img_mode says */
enum { IMG_OK, IMG_TEXT, IMG_BIG, IMG_NET, IMG_GONE };
static int img_mode, img_calls;
static char img_last[2][200];        /* the last two asked for, the latest first */

static int64_t fake_image(const char *url, const char *path, http_progress_fn progress, void *ctx,
                          const volatile int *abort)
{
    img_calls++;
    snprintf(img_last[1], sizeof img_last[1], "%s", img_last[0]);
    snprintf(img_last[0], sizeof img_last[0], "%s", url);
    if (img_mode == IMG_NET) return -1;
    if (img_mode == IMG_GONE) return -3;
    if (img_mode == IMG_BIG) {                        /* 9 MB on its way: given up once told so */
        if (progress) progress(64 * 1024, 9 * 1024 * 1024, ctx);
        return *abort ? -1 : 9 * 1024 * 1024;
    }
    FILE *in = img_mode == IMG_OK ? fopen("tests/data/covers/cover.jpg", "rb") : NULL, *out = fopen(path, "wb");
    char b[8192];
    size_t k;
    int64_t n = 0;
    if (!out) { if (in) fclose(in); return -1; }
    if (!in) n = (int64_t)fwrite("<html>not a picture</html>", 1, 26, out);
    while (in && (k = fread(b, 1, sizeof b, in)) > 0) n += (int64_t)fwrite(b, 1, k, out);
    if (in) fclose(in);
    fclose(out);
    if (progress) progress((uint64_t)n, (uint64_t)n, ctx);
    return n;
}

static int dl_calls;
static int64_t dl_answer = 1;                        /* what an episode's download answers (1: it works) */

static int64_t fake_download(const char *url, const char *path, http_progress_fn progress, void *ctx,
                             const volatile int *abort)
{
    if (strstr(url, ".jpg")) return fake_image(url, path, progress, ctx, abort);
    dl_calls++;
    if (dl_answer == 0) {                            /* an empty answer: an empty file, as http_download leaves */
        FILE *z = fopen(path, "wb");
        if (z) fclose(z);
        return 0;
    }
    if (dl_answer < 0) return dl_answer;
    for (int i = 0; i < (slow ? 200 : 2); i++) {
        if (*abort) return -1;
        if (progress) progress((uint64_t)i * 1000, 200000, ctx);
        usleep(2000);
    }
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fputs(url, f);
    fclose(f);
    return (int64_t)strlen(url);
}

/* until the worker has nothing left (or 15 s) */
static void settle(void)
{
    podsync_status_t st;
    for (int i = 0; i < 1500; i++) {   /* (a failing feed is tried 3 times: 7 s) */
        podsync_status(&st);
        if (!st.busy && !st.queued) return;
        usleep(10000);
    }
    printf("FAIL: the worker didn't settle\n");
    fails++;
}

static long size_of(const char *dir, const char *id, const char *name)
{
    char p[900];
    struct stat sb;
    snprintf(p, sizeof p, "%s/%s/%s", dir, id, name);
    return stat(p, &sb) == 0 ? (long)sb.st_size : -1;
}

/* a show's cover.url: the image last dealt with */
static const char *marker(const char *dir, const char *id)
{
    static char m[400];
    char p[900];
    snprintf(p, sizeof p, "%s/%s/cover.url", dir, id);
    FILE *f = fopen(p, "rb");
    m[f ? fread(m, 1, sizeof m - 1, f) : 0] = '\0';
    if (f) fclose(f);
    return m;
}

static void drop(const char *dir, const char *id, const char *name)
{
    char p[900];
    snprintf(p, sizeof p, "%s/%s/%s", dir, id, name);
    remove(p);
}

/* A show's picture: fetched once, kept at two sizes, and an image that's no
 * use not asked for again (id: a show just subscribed to, its feed read) */
static void pictures(const char *dir, const pod_show_t *show)
{
    const char *id = show->id, *url = show->url;
    enum { COVER = 136 * 136 * 2, THUMB = 34 * 34 * 2 };
    CHECK(img_calls == 1, "the show's image fetched: %d", img_calls);
    CHECK(size_of(dir, id, "cover.565") == COVER, "its cover: %ld bytes", size_of(dir, id, "cover.565"));
    CHECK(size_of(dir, id, "thumb.565") == THUMB, "its thumbnail: %ld bytes", size_of(dir, id, "thumb.565"));
    CHECK(!strcmp(marker(dir, id), "https://example.com/cover.jpg"), "its image noted [%s]", marker(dir, id));
    CHECK(size_of(dir, id, "cover.565.img") < 0, "the download removed");
    podsync_refresh(url);
    settle();
    CHECK(img_calls == 1, "read again: the image not fetched again (%d)", img_calls);

    /* no thumbnail and no note (a show from before there were any): both made from the cover, no download */
    drop(dir, id, "thumb.565");
    drop(dir, id, "cover.url");
    podsync_refresh(url);
    settle();
    CHECK(img_calls == 1 && size_of(dir, id, "thumb.565") == THUMB, "the thumbnail made from the cover (%d fetched)", img_calls);
    CHECK(!strcmp(marker(dir, id), "https://example.com/cover.jpg"), "and its image noted [%s]", marker(dir, id));
    /* the thumbnail alone gone: made again */
    drop(dir, id, "thumb.565");
    podsync_refresh(url);
    settle();
    CHECK(img_calls == 1 && size_of(dir, id, "thumb.565") == THUMB, "the thumbnail made again");

    /* its image changed, to something that's no picture: fetched once, noted, the cover it had kept */
    snprintf(alt_image, sizeof alt_image, "nopic");
    img_mode = IMG_TEXT;
    podsync_refresh(url);
    settle();
    CHECK(img_calls == 2 && !strcmp(marker(dir, id), "https://example.com/nopic.jpg"), "not a picture: noted (%d) [%s]", img_calls, marker(dir, id));
    CHECK(size_of(dir, id, "cover.565") == COVER && size_of(dir, id, "thumb.565") == THUMB, "the cover it had stays");
    podsync_refresh(url);
    settle();
    CHECK(img_calls == 2, "not a picture: not fetched again (%d)", img_calls);

    /* one over the cap: given up at once, one try, noted */
    snprintf(alt_image, sizeof alt_image, "toobg");
    img_mode = IMG_BIG;
    podsync_refresh(url);
    settle();
    CHECK(img_calls == 3 && !strcmp(marker(dir, id), "https://example.com/toobg.jpg"), "too big: one try, noted (%d) [%s]", img_calls, marker(dir, id));
    CHECK(size_of(dir, id, "cover.565.img") < 0 && size_of(dir, id, "cover.565") == COVER, "too big: nothing kept of it");
    podsync_refresh(url);
    settle();
    CHECK(img_calls == 3, "too big: not fetched again (%d)", img_calls);

    /* one the server says isn't there: one try, noted */
    snprintf(alt_image, sizeof alt_image, "gone1");
    img_mode = IMG_GONE;
    podsync_refresh(url);
    settle();
    CHECK(img_calls == 4 && !strcmp(marker(dir, id), "https://example.com/gone1.jpg"), "not there: one try, noted (%d)", img_calls);

    /* the network gone: three tries, nothing noted, tried again at the next refresh */
    snprintf(alt_image, sizeof alt_image, "later");
    img_mode = IMG_NET;
    podsync_refresh(url);
    settle();
    CHECK(img_calls == 7 && !strcmp(marker(dir, id), "https://example.com/gone1.jpg"), "no network: nothing noted (%d) [%s]", img_calls, marker(dir, id));
    img_mode = IMG_OK;
    podsync_refresh(url);
    settle();
    CHECK(img_calls == 8 && !strcmp(marker(dir, id), "https://example.com/later.jpg"), "no network: fetched the next time (%d)", img_calls);
    CHECK(size_of(dir, id, "cover.565") == COVER && size_of(dir, id, "thumb.565") == THUMB, "and kept");
    alt_image[0] = '\0';
    podsync_refresh(url);                             /* (back to the feed's own, for the rest) */
    settle();
    CHECK(img_calls == 9, "changed back: fetched (%d)", img_calls);
}

int main(void)
{
    char tmpl[512], dir[600];
    test_tmp(tmpl, sizeof tmpl, "podsync");
    if (!mkdtemp(tmpl)) return 1;
    snprintf(dir, sizeof dir, "%s/podcasts", tmpl);
    podsync_set_fetch(fake_fetch);
    podsync_set_download(fake_download);
    CHECK(podsync_start(dir) == 0, "started");

    /* subscribe: the show is added, its feed read */
    pod_show_t s = {0};
    snprintf(s.title, sizeof s.title, "Slow");
    snprintf(s.url, sizeof s.url, "https://feeds.example.com/slow.xml");
    podsync_status_t st;
    podsync_status(&st);
    unsigned ch = st.changed;
    CHECK(podsync_subscribe(&s) == 0, "subscribe queued");
    settle();
    podsync_status(&st);
    CHECK(st.changed > ch, "changed");
    pod_show_t shows[4];
    int n = pod_shows_load(dir, shows, 4);
    CHECK(n == 1 && !strcmp(shows[0].title, "Slow") && shows[0].refreshed > 0, "subscribed and read: %d", n);
    pod_episode_t e[10];
    int ne = n ? pod_episodes_load(dir, shows[0].id, e, 10) : 0;
    CHECK(ne == 3, "episodes on the card: %d", ne);
    CHECK(n && shows[0].seen == e[1].date && shows[0].latest == e[0].date, "only the newest is new");
    if (n) pictures(dir, &shows[0]);

    /* a show found in the directory: the picture the search gave first, at
     * once, then its feed's own in its place */
    {
        pod_show_t d = {0}, all[4];
        snprintf(d.title, sizeof d.title, "Found");
        snprintf(d.url, sizeof d.url, "https://feeds.example.com/found.xml");
        snprintf(d.image, sizeof d.image, "https://is1.example/600x600bb.jpg");
        int before = img_calls;
        podsync_subscribe(&d);
        settle();
        int k = pod_shows_load(dir, all, 4);
        CHECK(k == 2 && img_calls == before + 2, "found: two images fetched (%d)", img_calls - before);
        CHECK(!strcmp(img_last[1], "https://is1.example/600x600bb.jpg") && !strcmp(img_last[0], "https://example.com/cover.jpg"),
              "the directory's, then the feed's [%s] [%s]", img_last[1], img_last[0]);
        if (k == 2) {
            CHECK(!strcmp(marker(dir, all[1].id), "https://example.com/cover.jpg"), "the feed's noted");
            CHECK(size_of(dir, all[1].id, "thumb.565") == 34 * 34 * 2, "its thumbnail");
        }
        podsync_subscribe(&d);                        /* again: nothing fetched */
        settle();
        CHECK(img_calls == before + 2, "subscribed already: no image fetched");
        podsync_unsubscribe(d.url);
        settle();
    }

    /* download, with progress; again: nothing */
    if (n && ne) {
        CHECK(podsync_download(shows[0].id, &e[0]) == 0, "download queued");
        CHECK(podsync_download(shows[0].id, &e[0]) == 0 && podsync_download_state(shows[0].id, e[0].key) >= 0, "once");
        settle();
        CHECK(pod_downloaded(dir, shows[0].id, &e[0]), "downloaded");
        CHECK(podsync_download_state(shows[0].id, e[0].key) == -1, "no longer queued");

        /* cancel one under way: no file left, not even its .part */
        slow = 1;
        podsync_download(shows[0].id, &e[1]);
        usleep(30000);
        int pct = podsync_download_state(shows[0].id, e[1].key);
        CHECK(pct >= 1, "under way: %d", pct);
        podsync_cancel_download(shows[0].id, e[1].key);
        settle();
        slow = 0;
        char p[800], part[820];
        struct stat sb;
        pod_episode_path(dir, shows[0].id, &e[1], p, sizeof p);
        snprintf(part, sizeof part, "%s.part", p);
        CHECK(!pod_downloaded(dir, shows[0].id, &e[1]) && stat(part, &sb) != 0, "cancelled: nothing left");
    }

    /* the feed cut off by a Wi-Fi drop: the episodes stay as they were, and
     * the refresh is said to have failed */
    if (n && ne) {
        cut_off = 1;
        podsync_refresh(shows[0].url);
        settle();
        cut_off = 0;
        pod_episode_t e2[10];
        int ne2 = pod_episodes_load(dir, shows[0].id, e2, 10);
        podsync_status(&st);
        CHECK(ne2 == ne, "cut off: %d episodes, were %d", ne2, ne);
        CHECK(st.last_failed && !strcmp(st.failed_what, "Slow"), "cut off: failure said [%s]", st.failed_what);
        podsync_refresh(shows[0].url);                /* whole again: read */
        settle();
        CHECK(pod_episodes_load(dir, shows[0].id, e2, 10) == ne, "whole again");
    }

    /* search */
    pod_show_t found[25];
    int nf = 0;
    CHECK(podsync_search_result(found, 25, &nf) == -1, "no search yet");
    podsync_search("radiolab");
    podsync_search("radio lab");     /* replaces it */
    settle();
    CHECK(podsync_search_result(found, 25, &nf) == 1 && nf == 2 && !strcmp(found[0].title, "Radiolab"), "found: %d", nf);

    /* a feed that fails: said so */
    pod_show_t b = {0};
    snprintf(b.title, sizeof b.title, "Broken");
    snprintf(b.url, sizeof b.url, "https://broken.example/feed");
    podsync_subscribe(&b);
    settle();
    podsync_status(&st);
    CHECK(st.last_failed && !strcmp(st.failed_what, "Broken"), "failure said: %d [%s]", st.last_failed, st.failed_what);

    /* auto: newest 2 unplayed downloaded while charging on Wi-Fi; a played old one cleaned up */
    if (n && ne) {
        pod_state_t ps = {0};
        snprintf(ps.key, sizeof ps.key, "%s", e[0].key);
        ps.played = 1;
        ps.played_at = (int64_t)time(NULL) - 9 * 86400;
        pod_state_put(dir, shows[0].id, &ps);       /* e[0]: played 9 days ago, on the card */
        podsync_auto((int64_t)time(NULL), 1, 1, 0, 2, 7);
        settle();
        settle();
        CHECK(!pod_downloaded(dir, shows[0].id, &e[0]), "played 9 days ago: removed");
        CHECK(pod_downloaded(dir, shows[0].id, &e[1]) && pod_downloaded(dir, shows[0].id, &e[2]), "the 2 newest unplayed");
        pod_chapter_t chs[8];
        CHECK(pod_chapters(dir, shows[0].id, &e[1], chs, 8) == 3 && chs[1].start_ms == 95500, "its chapters kept beside it");
        CHECK(pod_chapters(dir, shows[0].id, &e[2], chs, 8) == 0, "none for one without");
        /* not charging: nothing downloaded */
        pod_delete_download(dir, shows[0].id, &e[2]);
        podsync_auto((int64_t)time(NULL), 1, 0, 0, 2, 7);
        settle();
        CHECK(!pod_downloaded(dir, shows[0].id, &e[2]), "not charging: not downloaded");
        /* feeds read again once due */
        int before = fetches;
        podsync_auto((int64_t)time(NULL) + 7 * 3600, 1, 0, 6 * 3600, 2, 7);
        settle();
        CHECK(fetches > before, "feeds read again when due");
        before = fetches;
        podsync_auto((int64_t)time(NULL) + 7 * 3600 + 60, 1, 0, 6 * 3600, 2, 7);
        settle();
        CHECK(fetches == before, "not again a minute later");
        /* an automatic download the card stops: tried once (not three times,
         * filling it again), and not at the next call ten minutes later, nor
         * the one after: after an hour */
        int64_t t0 = (int64_t)time(NULL);
        dl_calls = 0;
        dl_answer = HTTP_ERR_FULL;
        podsync_auto(t0, 1, 1, 0, 2, 7);
        settle();
        settle();
        CHECK(dl_calls == 1 && !pod_downloaded(dir, shows[0].id, &e[2]), "the card full: one try (%d)", dl_calls);
        podsync_auto(t0 + 600, 1, 1, 0, 2, 7);
        settle();
        settle();
        CHECK(dl_calls == 1, "not again ten minutes later (%d)", dl_calls);
        dl_answer = 0;
        podsync_auto(t0 + 3700, 1, 1, 0, 2, 7);
        settle();
        settle();
        CHECK(dl_calls == 2 && !pod_downloaded(dir, shows[0].id, &e[2]), "after an hour: once; an empty answer isn't an episode (%d)", dl_calls);
        podsync_auto(t0 + 2 * 3700, 1, 1, 0, 2, 7);
        settle();
        settle();
        CHECK(dl_calls == 2, "a second failure: longer than an hour (%d)", dl_calls);
        /* asked for by hand: tried whatever failed before; and once it's had, forgotten */
        dl_answer = 1;
        podsync_download(shows[0].id, &e[2]);
        settle();
        CHECK(dl_calls == 3 && pod_downloaded(dir, shows[0].id, &e[2]), "by hand: tried, and there (%d)", dl_calls);
        pod_delete_download(dir, shows[0].id, &e[2]);
        podsync_auto(t0 + 2 * 3700, 1, 1, 0, 2, 7);
        settle();
        settle();
        CHECK(dl_calls == 4 && pod_downloaded(dir, shows[0].id, &e[2]), "no longer held back (%d)", dl_calls);
        pod_delete_download(dir, shows[0].id, &e[2]);
    }

    /* OPML import: the shows added, each read */
    char opml[700];
    snprintf(opml, sizeof opml, "%s/../podcasts/../subs.opml", dir);
    FILE *src = fopen("tests/data/podcasts/subs.opml", "rb"), *dst = fopen(opml, "wb");
    char buf[4096];
    size_t k;
    while (src && dst && (k = fread(buf, 1, sizeof buf, src)) > 0) fwrite(buf, 1, k, dst);
    if (src) fclose(src);
    if (dst) fclose(dst);
    podsync_import_opml(opml);
    settle();
    podsync_status(&st);
    CHECK(st.imported == 2, "imported: %d", st.imported);
    CHECK(pod_shows_load(dir, shows, 4) == 4, "four shows now");

    /* Wi-Fi up with no internet behind it: a refresh of every show stops
     * after three in a row have reached no server (each took its three tries
     * and their waits: 64 shows held the worker for many minutes), and the
     * next one starts with the shows it left */
    {
        podsync_set_retry_waits(0, 0);
        offline = 1;
        fetches = 0;
        podsync_refresh("");
        settle();
        CHECK(fetches == 9, "offline: three shows tried, three times each (%d)", fetches);
        CHECK(in_log(shows[0].url) && in_log(shows[2].url) && !in_log(shows[3].url), "offline: the fourth left");
        offline = 0;
        fetches = 0;
        podsync_refresh("");
        settle();
        CHECK(!strcmp(fetched[0], shows[3].url), "back: the one left comes first [%s]", fetched[0]);
        CHECK(in_log(shows[0].url) && in_log(shows[1].url) && in_log(shows[2].url), "back: and every other show");
        fetches = 0;
        podsync_refresh("");                          /* (a whole round: the next starts at the top again) */
        settle();
        CHECK(!strcmp(fetched[0], shows[0].url), "then from the top [%s]", fetched[0]);
        podsync_set_retry_waits(2000, 5000);
    }

    /* unsubscribe */
    podsync_unsubscribe(b.url);
    settle();
    CHECK(pod_shows_load(dir, shows, 4) == 3, "three left");

    /* a download (or a place kept) for a show unsubscribed meanwhile: its folder isn't made again */
    if (n && ne) {
        char slow_id[13], sd[700];
        snprintf(slow_id, sizeof slow_id, "%s", shows[0].id);
        podsync_unsubscribe(shows[0].url);
        podsync_download(slow_id, &e[2]);
        settle();
        snprintf(sd, sizeof sd, "%s/%s", dir, slow_id);
        struct stat sb;
        CHECK(stat(sd, &sb) != 0, "unsubscribed: its folder not made again by a download");
        pod_state_t ps = {0};
        snprintf(ps.key, sizeof ps.key, "%s", e[2].key);
        ps.pos_ms = 90000;
        pod_state_put(dir, slow_id, &ps);
        CHECK(stat(sd, &sb) != 0, "...nor by its place kept");
    }

    /* what a download cut short left: gone when the worker starts again */
    n = pod_shows_load(dir, shows, 4);
    if (n) {
        char left[6][800], ui_tmp[800];
        int had_cover = size_of(dir, shows[0].id, "cover.565") > 0;
        drop(dir, shows[0].id, "thumb.565");          /* (a show from before there were thumbnails) */
        snprintf(left[0], sizeof left[0], "%s/%s/0123456789ab.mp3.part", dir, shows[0].id);
        snprintf(left[1], sizeof left[1], "%s/%s/cover.565.tmp", dir, shows[0].id);
        snprintf(left[2], sizeof left[2], "%s/%s/cover.565.img", dir, shows[0].id);
        snprintf(left[3], sizeof left[3], "%s/%s/cover.565.img.part", dir, shows[0].id);
        snprintf(left[4], sizeof left[4], "%s/%s/thumb.565.tmp", dir, shows[0].id);
        snprintf(left[5], sizeof left[5], "%s/%s/cover.url.tmp", dir, shows[0].id);
        snprintf(ui_tmp, sizeof ui_tmp, "%s/%s/state.txt.tmp", dir, shows[0].id);   /* (the UI's, maybe being written) */
        for (int i = 0; i < 6; i++) { FILE *f = fopen(left[i], "wb"); if (f) { fputs("x", f); fclose(f); } }
        FILE *uf = fopen(ui_tmp, "wb");
        if (uf) { fputs("x", uf); fclose(uf); }
        podsync_stop();
        podsync_start(dir);
        struct stat sb;
        int gone = 0;
        for (int i = 0; i < 200 && !gone; i++) {
            gone = 1;
            for (int q = 0; q < 6; q++) gone &= stat(left[q], &sb) != 0;
            gone &= size_of(dir, shows[0].id, "thumb.565") == 34 * 34 * 2;
            if (!gone) usleep(10000);
        }
        CHECK(gone, "a cut-short download's .part and a cover's leftovers removed at the start");
        CHECK(had_cover && size_of(dir, shows[0].id, "thumb.565") == 34 * 34 * 2, "a cover without a thumbnail given one at the start");
        CHECK(stat(ui_tmp, &sb) == 0, "the UI's state.txt.tmp left alone");
        remove(ui_tmp);
    }
    podsync_stop();
    if (fails) { printf("%d failed\n", fails); return 1; }
    printf("podsync: ok\n");
    return 0;
}
