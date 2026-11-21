/* SPDX-License-Identifier: GPL-3.0-or-later */
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

static int fetches, slow;

static long fake_fetch(const char *url, char *buf, size_t cap, const volatile int *abort)
{
    (void)abort;
    fetches++;
    const char *file = strstr(url, "itunes.apple.com") ? "tests/data/podcasts/search.json"
                     : strstr(url, "chapters.json") ? "tests/data/podcasts/chapters.json"
                     : strstr(url, "broken") ? NULL : "tests/data/podcasts/feed.xml";
    if (!file) return -1;
    FILE *f = fopen(file, "rb");
    if (!f) return -1;
    size_t n = fread(buf, 1, cap - 1, f);
    fclose(f);
    buf[n] = '\0';
    return (long)n;
}

static int64_t fake_download(const char *url, const char *path, http_progress_fn progress, void *ctx,
                             const volatile int *abort)
{
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

    /* unsubscribe */
    podsync_unsubscribe(b.url);
    settle();
    CHECK(pod_shows_load(dir, shows, 4) == 3, "three left");
    podsync_stop();
    if (fails) { printf("%d failed\n", fails); return 1; }
    printf("podsync: ok\n");
    return 0;
}
