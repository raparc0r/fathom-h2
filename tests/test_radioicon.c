/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* Radio stations' icons (app/net/radioicon.c), with a fetch of its own: a
 * saved JPEG (tests/data/covers), PNGs made here, and every way a logo's
 * address can fail. */
#include <dirent.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <zlib.h>

#include "library/library.h"
#include "net/radioicon.h"
#include "tmpdir.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static void nap(int ms)
{
    struct timespec t = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&t, NULL);
}

/* ---- a small PNG (RGB, 8 bits, rows unfiltered), one colour ---- */

static void put32(unsigned char *p, uint32_t v) { p[0] = (unsigned char)(v >> 24), p[1] = (unsigned char)(v >> 16), p[2] = (unsigned char)(v >> 8), p[3] = (unsigned char)v; }

static size_t chunk(unsigned char *out, const char *type, const unsigned char *d, uint32_t n)
{
    put32(out, n);
    memcpy(out + 4, type, 4);
    if (n) memcpy(out + 8, d, n);
    put32(out + 8 + n, (uint32_t)crc32(crc32(0, (const Bytef *)type, 4), d, n));
    return 12 + n;
}

static long make_png(unsigned char *out, int w, int h, int r, int g, int b)
{
    size_t row = (size_t)w * 3 + 1;
    unsigned char *raw = calloc((size_t)h, row), hd[13];
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            unsigned char *p = raw + (size_t)y * row + 1 + (size_t)x * 3;
            p[0] = (unsigned char)r, p[1] = (unsigned char)g, p[2] = (unsigned char)b;
        }
    uLongf zn = compressBound((uLong)(row * (size_t)h));
    unsigned char *z = malloc(zn);
    compress2(z, &zn, raw, (uLong)(row * (size_t)h), 6);
    size_t n = 8;
    memcpy(out, "\x89PNG\r\n\x1a\n", 8);
    put32(hd, (uint32_t)w), put32(hd + 4, (uint32_t)h);
    hd[8] = 8, hd[9] = 2, hd[10] = hd[11] = hd[12] = 0;
    n += chunk(out + n, "IHDR", hd, 13);
    n += chunk(out + n, "IDAT", z, (uint32_t)zn);
    n += chunk(out + n, "IEND", NULL, 0);
    free(raw);
    free(z);
    return (long)n;
}

/* ---- the fetch: what an address gives is in its name ---- */

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static int calls, offline, slow_ms;
static char asked[32][96];

static long fake_fetch(const char *url, char *buf, size_t cap, const volatile int *abort)
{
    pthread_mutex_lock(&mu);
    if (calls < 32) snprintf(asked[calls], sizeof asked[calls], "%s", url);
    calls++;
    pthread_mutex_unlock(&mu);
    for (int t = 0; t < slow_ms && !(abort && *abort); t += 10) nap(10);
    if (abort && *abort) return -1;
    if (offline || strstr(url, "unreachable")) return -1;
    if (strstr(url, "gone")) return -3;
    if (strstr(url, "huge")) { memset(buf, 'x', cap - 1); buf[cap - 1] = '\0'; return -2; }
    if (strstr(url, "empty")) { buf[0] = '\0'; return 0; }
    if (strstr(url, "svg")) return snprintf(buf, cap, "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 1 1\"/>");
    if (strstr(url, "broken")) { long n = make_png((unsigned char *)buf, 100, 100, 1, 2, 3); return n / 2; }   /* half a PNG */
    if (strstr(url, "px16")) return make_png((unsigned char *)buf, 16, 16, 200, 30, 30);
    if (strstr(url, "px8")) return make_png((unsigned char *)buf, 8, 8, 200, 30, 30);
    if (strstr(url, "px67")) return make_png((unsigned char *)buf, 200, 67, 0, 0, 250);
    if (strstr(url, "px68")) return make_png((unsigned char *)buf, 68, 300, 0, 0, 250);
    FILE *f = fopen("tests/data/covers/cover.jpg", "rb");            /* anything else: a 300 x 300 JPEG */
    if (!f) return -1;
    size_t n = fread(buf, 1, cap - 1, f);
    fclose(f);
    return (long)n;
}

static int asked_for(const char *url)
{
    int k = 0;
    pthread_mutex_lock(&mu);
    for (int i = 0; i < calls && i < 32; i++) k += !strcmp(asked[i], url);
    pthread_mutex_unlock(&mu);
    return k;
}

static void settle(void)
{
    for (int i = 0; i < 1000 && ricon_busy(); i++) nap(10);
    CHECK(!ricon_busy());
}

static long size_of(const char *path)
{
    struct stat sb;
    return stat(path, &sb) == 0 ? (long)sb.st_size : -1;
}

/* the size of an icon's picture file (-1: none), and whether it's marked as no use */
static long pic(const char *url)
{
    char p[700];
    return ricon_path(url, p, sizeof p) == 0 ? size_of(p) : -1;
}

static int marked(const char *url)
{
    char p[700];
    if (ricon_path(url, p, sizeof p)) return 0;
    memcpy(p + strlen(p) - 3, "none", 5);
    return size_of(p) == 0;
}

static int files_in(const char *dir)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    int n = 0;
    while (d && (e = readdir(d))) n += e->d_name[0] != '.';
    if (d) closedir(d);
    return n;
}

static radio_station_t station(const char *name, const char *icon)
{
    radio_station_t s;
    memset(&s, 0, sizeof s);
    snprintf(s.name, sizeof s.name, "%s", name);
    snprintf(s.url, sizeof s.url, "http://stream.example/%s", name);
    snprintf(s.icon, sizeof s.icon, "%s", icon);
    return s;
}

int main(void)
{
    char tmpl[512], dir[600], p[700], q[720];
    test_tmp(tmpl, sizeof tmpl, "fathom-radioicon");
    if (!mkdtemp(tmpl)) return 1;
    snprintf(dir, sizeof dir, "%s/radio-icons", tmpl);
    enum { SMALL = LIB_ART_SMALL * LIB_ART_SMALL * 2, BOTH = SMALL + LIB_ART_LARGE * LIB_ART_LARGE * 2 };
    CHECK(SMALL == 2312 && BOTH == 39304);

    /* not started: nothing happens */
    radio_station_t st[16];
    st[0] = station("jpeg", "https://img.example/logo.jpg");
    ricon_set_fetch(fake_fetch);
    ricon_sync(st, 1, 1);
    CHECK(!ricon_busy() && calls == 0 && ricon_path(st[0].icon, p, sizeof p) == -1);

    CHECK(ricon_start(dir) == 0);
    char id[13], id2[13];
    ricon_id(st[0].icon, id);
    CHECK(strlen(id) == 12 && ricon_path(st[0].icon, p, sizeof p) == 0);
    snprintf(q, sizeof q, "%s/%s.565", dir, id);
    CHECK(!strcmp(p, q));
    ricon_id("", id2);
    CHECK(!id2[0] && ricon_path("", p, sizeof p) == -1 && ricon_path("ftp://x.example/i.png", p, sizeof p) == -1);

    /* one of each */
    int n = 0;
    st[n++] = station("jpeg", "https://img.example/logo.jpg");          /* a JPEG: both sizes */
    st[n++] = station("small", "https://img.example/px16.png");         /* a favicon: the list's size only */
    st[n++] = station("svg", "https://img.example/svg-logo.png");       /* SVG text under a .png name */
    st[n++] = station("huge", "https://img.example/huge.jpg");          /* over the cap (-2) */
    st[n++] = station("gone", "https://img.example/gone.png");          /* the server says no (-3) */
    st[n++] = station("unreachable", "https://unreachable.example/l.png");   /* no answer (-1) */
    st[n++] = station("none", "");                                      /* a station with no icon */
    st[n++] = station("same", "https://img.example/logo.jpg");          /* the first one's logo again */
    st[n++] = station("broken", "https://img.example/broken.png");      /* a PNG cut short */
    st[n++] = station("tiny", "https://img.example/px8.png");           /* too small to be a logo */
    st[n++] = station("wide", "https://img.example/px67.png");          /* 200 x 67: just under the large size's need */
    st[n++] = station("tall", "https://img.example/px68.png");          /* 68 x 300: just enough */
    st[n++] = station("empty", "https://img.example/empty.png");
    unsigned seq = ricon_seq();
    ricon_sync(st, n, 1);
    settle();
    CHECK(pic(st[0].icon) == BOTH && !marked(st[0].icon));
    CHECK(pic(st[1].icon) == SMALL && !marked(st[1].icon));
    CHECK(pic(st[2].icon) == -1 && marked(st[2].icon));
    CHECK(pic(st[3].icon) == -1 && marked(st[3].icon));
    CHECK(pic(st[4].icon) == -1 && marked(st[4].icon));
    CHECK(pic(st[5].icon) == -1 && !marked(st[5].icon));                 /* nothing kept: tried again later */
    CHECK(pic(st[8].icon) == -1 && marked(st[8].icon));
    CHECK(pic(st[9].icon) == -1 && marked(st[9].icon));
    CHECK(pic(st[10].icon) == SMALL && pic(st[11].icon) == BOTH);
    CHECK(pic(st[12].icon) == -1 && marked(st[12].icon));
    CHECK(ricon_seq() == seq + 4);                                       /* four pictures written */
    CHECK(asked_for(st[0].icon) == 1);                                   /* one fetch for the logo two stations share */
    CHECK(calls == 11);
    CHECK(files_in(dir) == 10);                                          /* 4 pictures, 6 marks, no .tmp left */

    /* the pixels: the list's from the start of the file, Now Playing's after it */
    static uint16_t small[LIB_ART_SMALL * LIB_ART_SMALL], large[LIB_ART_LARGE * LIB_ART_LARGE];
    static uint16_t want_s[LIB_ART_SMALL * LIB_ART_SMALL], want_l[LIB_ART_LARGE * LIB_ART_LARGE];
    static unsigned char enc[RICON_MAX_FILE + 1];
    long en = fake_fetch("https://img.example/logo.jpg", (char *)enc, sizeof enc, NULL);
    calls--;
    CHECK(en > 0 && library_logo_decode(enc, (size_t)en, want_l, want_s, NULL) == 0);
    CHECK(ricon_read(id, LIB_ART_SMALL, small) == 1 && !memcmp(small, want_s, sizeof small));
    CHECK(ricon_read(id, LIB_ART_LARGE, large) == 1 && !memcmp(large, want_l, sizeof large));
    ricon_id(st[1].icon, id2);
    CHECK(ricon_read(id2, LIB_ART_SMALL, small) == 1 && ricon_read(id2, LIB_ART_LARGE, large) == 0);
    CHECK((small[0] >> 11) >= 23 && (small[0] & 31) <= 5 && (small[SMALL / 2 - 1] >> 11) >= 23);   /* red, corner to corner */
    ricon_id(st[2].icon, id2);
    CHECK(ricon_read(id2, LIB_ART_SMALL, small) == 0 && ricon_read("", LIB_ART_SMALL, small) == 0 &&
          ricon_read(id, 50, small) == 0);

    /* again: only the one that wasn't reached is asked for; it answers now */
    int before = calls;
    snprintf(st[5].icon, sizeof st[5].icon, "https://reachable-now.example/l.png");
    ricon_sync(st, n, 1);
    settle();
    CHECK(calls == before + 1 && pic(st[5].icon) == BOTH && ricon_seq() == seq + 5);
    before = calls;
    ricon_sync(st, n, 1);
    settle();
    CHECK(calls == before && ricon_seq() == seq + 5);                    /* all have a file or a mark: nothing fetched */

    /* without the network: nothing asked for, nothing marked */
    st[n++] = station("later", "https://img.example/later.jpg");
    ricon_sync(st, n, 0);
    settle();
    CHECK(calls == before && pic(st[n - 1].icon) == -1 && !marked(st[n - 1].icon));

    /* the network gone part way: after three in a row unreached, the rest wait */
    offline = 1;
    radio_station_t off[6];
    for (int i = 0; i < 6; i++) {
        char u[64];
        snprintf(u, sizeof u, "https://img.example/off%d.jpg", i);
        off[i] = station("off", u);
    }
    before = calls;
    ricon_sync(off, 6, 1);
    settle();
    CHECK(calls == before + 3 && files_in(dir) == 0);                    /* (and the others' files gone: not in this list) */
    offline = 0;

    /* stations taken out of the list: their pictures and marks go; a run's
     * leftovers too; anything else in the folder is left alone */
    ricon_sync(st, n, 1);
    settle();
    CHECK(pic(st[0].icon) == BOTH && pic(st[1].icon) == SMALL && marked(st[2].icon) && pic(st[n - 1].icon) == BOTH);
    snprintf(p, sizeof p, "%s/0123456789ab.565.tmp", dir);
    snprintf(q, sizeof q, "%s/notes.txt", dir);
    FILE *f = fopen(p, "wb");
    if (f) { fputs("x", f); fclose(f); }
    f = fopen(q, "wb");
    if (f) { fputs("x", f); fclose(f); }
    radio_station_t keep[2] = {st[1], st[2]};
    before = calls;
    ricon_sync(keep, 2, 0);
    settle();
    CHECK(pic(st[1].icon) == SMALL && marked(st[2].icon));               /* still in the list */
    CHECK(pic(st[0].icon) == -1 && !marked(st[3].icon) && pic(st[n - 1].icon) == -1);
    CHECK(size_of(p) == -1 && size_of(q) == 1 && calls == before);
    CHECK(files_in(dir) == 3);
    ricon_sync(NULL, -1, 1);                                             /* no list (it couldn't be read): nothing touched */
    settle();
    CHECK(files_in(dir) == 3);
    ricon_sync(NULL, 0, 1);                                              /* an empty list: none are anyone's */
    settle();
    CHECK(files_in(dir) == 1 && size_of(q) == 1);

    /* a list given while a run is under way gets its own run after it */
    slow_ms = 300;
    before = calls;
    ricon_sync(st, 1, 1);
    CHECK(ricon_busy());
    ricon_sync(st, 2, 1);
    radio_station_t two[2] = {st[0], st[10]};
    ricon_sync(two, 2, 1);                                               /* (the one waiting replaced) */
    settle();
    slow_ms = 0;
    CHECK(pic(st[0].icon) == BOTH && pic(st[10].icon) == SMALL && pic(st[1].icon) == -1 && calls == before + 2);

    /* stopped mid-fetch: the run ends, nothing half-written; started again, it's fetched */
    slow_ms = 3000;
    ricon_sync(st, 2, 1);
    nap(30);
    time_t t0 = time(NULL);
    ricon_stop();
    CHECK(!ricon_busy() && time(NULL) - t0 <= 2 && pic(st[1].icon) == -1);
    slow_ms = 0;
    ricon_sync(st, 2, 1);                                                /* stopped: nothing */
    CHECK(!ricon_busy());
    CHECK(ricon_start(dir) == 0);
    ricon_sync(st, 2, 1);
    settle();
    CHECK(pic(st[1].icon) == SMALL && !marked(st[1].icon));
    /* one whose server is never reached (it refuses every connection): asked
     * for twice since the player started, not at every opening of Radio */
    radio_station_t never = station("never", "https://unreachable.example/never.png");
    int asked0 = calls;
    for (int k = 0; k < 5; k++) {
        ricon_sync(&never, 1, 1);
        settle();
    }
    CHECK(calls == asked0 + 2 && pic(never.icon) == -1 && !marked(never.icon));
    ricon_stop();

    char cmd[700];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", tmpl);
    if (system(cmd)) {}
    if (!fails) printf("radioicon: ok\n");
    return fails != 0;
}
