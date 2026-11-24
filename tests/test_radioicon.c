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
#include "net/radiodir.h"
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

/* ---- an .ico: one picture w x w, a 32- or 24-bit bitmap (its mask clearing
 * the left half when masked), or a PNG inside ---- */

static void le16(unsigned char *p, unsigned v) { p[0] = (unsigned char)v, p[1] = (unsigned char)(v >> 8); }
static void le32(unsigned char *p, unsigned long v) { le16(p, (unsigned)(v & 0xffff)), le16(p + 2, (unsigned)(v >> 16)); }

static long make_ico(unsigned char *out, int w, int bpp, int masked, int png)
{
    memset(out, 0, 22);
    le16(out + 2, 1), le16(out + 4, 1);
    out[6] = (unsigned char)w, out[7] = (unsigned char)w;
    le16(out + 10, 1), le16(out + 12, (unsigned)bpp);
    unsigned char *d = out + 22;
    long n;
    if (png) {
        n = make_png(d, w, w, 10, 200, 10);
    } else {
        size_t stride = ((size_t)w * (size_t)bpp + 31) / 32 * 4, mstride = ((size_t)w + 31) / 32 * 4;
        memset(d, 0, 40 + (stride + mstride) * (size_t)w);
        le32(d, 40), le32(d + 4, (unsigned long)w), le32(d + 8, (unsigned long)w * 2), le16(d + 12, 1), le16(d + 14, (unsigned)bpp);
        for (int y = 0; y < w; y++)
            for (int x = 0; x < w; x++) {
                unsigned char *c = d + 40 + (size_t)y * stride + (size_t)x * (size_t)(bpp / 8);
                c[0] = 250, c[1] = 20, c[2] = 20;                 /* blue, green, red: a blue square */
                if (bpp == 32) c[3] = masked ? 0 : 255;           /* (masked: no alpha at all, the mask decides) */
                if (masked && x < w / 2) d[40 + stride * (size_t)w + (size_t)y * mstride + (size_t)(x / 8)] |= (unsigned char)(0x80 >> (x % 8));
            }
        n = 40 + (long)((stride + mstride) * (size_t)w);
    }
    le32(out + 14, (unsigned long)n), le32(out + 18, 22);
    return 22 + n;
}

/* ...with a PNG inside that has one more chunk, before its pixels: one that
 * says it's `len` bytes long, and has `has` after its 8 (what it holds and
 * its CRC, or less) */
static long make_ico_chunk(unsigned char *out, uint32_t len, size_t has)
{
    long n = make_ico(out, 16, 32, 0, 1);
    unsigned char *at = out + 22 + 8 + 25;           /* past the directory, the PNG's 8 bytes and its IHDR */
    memmove(at + 8 + has, at, (size_t)(out + n - at));
    memset(at, 0, 8 + has);
    put32(at, len);
    memcpy(at + 4, "tEXt", 4);
    n += 8 + (long)has;
    le32(out + 14, (unsigned long)(n - 22));
    return n;
}

/* ---- the fetch: what an address gives is in its name ---- */

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static int calls, offline, slow_ms, reach;   /* reach: the "unreachable" servers answer now */
static char asked[32][96];

static long fake_fetch(const char *url, char *buf, size_t cap, const volatile int *abort)
{
    pthread_mutex_lock(&mu);
    if (calls < 32) snprintf(asked[calls], sizeof asked[calls], "%s", url);
    calls++;
    pthread_mutex_unlock(&mu);
    for (int t = 0; t < slow_ms && !(abort && *abort); t += 10) nap(10);
    if (abort && *abort) return -1;
    if (offline || (strstr(url, "unreachable") && !reach)) return -1;
    /* stations' own sites: their pages, and what the pages name */
    if (!strcmp(url, "https://touch.example/"))
        return snprintf(buf, cap, "<!doctype html><html><head><title>T</title>\n"
                                  "<link rel=\"mask-icon\" href=\"/pin.svg\" color=\"#000\">\n"
                                  "<link rel=\"icon\" href=\"/fav-px16.png\" sizes=\"16x16\">\n"
                                  "<LINK HREF='img/touch-px180.png?v=1&amp;x=2' REL='apple-touch-icon'>\n"
                                  "<link rel=\"icon\" type=\"image/svg+xml\" href=\"/logo.svg\" sizes=\"any\">\n"
                                  "</head><body>radio</body></html>");
    if (!strcmp(url, "https://touch.example/img/touch-px180.png?v=1&x=2")) return make_png((unsigned char *)buf, 180, 180, 9, 9, 200);
    if (!strcmp(url, "https://rel.example/a/b/page.html?x=1"))
        return snprintf(buf, cap, "<head><link rel=icon href=//cdn.example/tiny-px16.png sizes=16x16>"
                                  "<link rel=\"shortcut icon\" sizes=\"96x96\" type=\"image/png\" href=\"icons/px96.png\"></head>");
    if (!strcmp(url, "https://rel.example/a/b/icons/px96.png")) return make_png((unsigned char *)buf, 96, 96, 9, 200, 9);
    if (!strcmp(url, "https://long.example/")) {                      /* a page longer than is read: its head is enough */
        int k = snprintf(buf, cap, "<head><link rel=\"apple-touch-icon\" sizes=\"120x120\" href=\"/t120.png\"></head>");
        memset(buf + k, ' ', cap - 1 - (size_t)k);
        buf[cap - 1] = '\0';
        return -2;
    }
    if (!strcmp(url, "https://long.example/t120.png")) return make_png((unsigned char *)buf, 120, 120, 200, 200, 9);
    if (!strcmp(url, "https://ico.example/radio/index.html")) return snprintf(buf, cap, "<html><head></head><body>no icons named</body></html>");
    if (!strcmp(url, "https://ico.example/favicon.ico")) return make_ico((unsigned char *)buf, 32, 32, 0, 0);
    if (!strcmp(url, "https://ico24.example/favicon.ico")) return make_ico((unsigned char *)buf, 48, 24, 1, 0);
    if (!strcmp(url, "https://icomask.example/favicon.ico")) return make_ico((unsigned char *)buf, 16, 32, 1, 0);
    if (!strcmp(url, "https://icopng.example/favicon.ico")) return make_ico((unsigned char *)buf, 64, 32, 0, 1);
    if (!strcmp(url, "https://badico.example/favicon.ico")) { long k = make_ico((unsigned char *)buf, 32, 32, 0, 0); return k - 600; }
    if (!strcmp(url, "https://smallnamed.example/"))                  /* names only its small favicon; a big icon at the usual address */
        return snprintf(buf, cap, "<head><link rel=\"icon\" href=\"/favicon.ico\" sizes=\"32x32\"></head>");
    if (!strcmp(url, "https://smallnamed.example/favicon.ico")) return make_ico((unsigned char *)buf, 32, 32, 0, 0);
    if (!strcmp(url, "https://down.example/")) return -1;
    if (strstr(url, "ico.example/") || strstr(url, "ico24.example/") || strstr(url, "icomask.example/") ||
        strstr(url, "icopng.example/") || strstr(url, "badico.example/") || strstr(url, "bare.example/") ||
        strstr(url, "touch.example/") || strstr(url, "rel.example/") || strstr(url, "long.example/"))
        return -3;                                                    /* anything else there: not found */
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

/* ---- the directory, asked what it has for a stream (by its address, in the
 * request): no answer until dir_on, then what the stream's name says ---- */

static int dir_on, dir_calls;
static char dir_asked[16][160];

static long fake_dir(const char *url, char *buf, size_t cap, const volatile int *abort)
{
    (void)abort;
    pthread_mutex_lock(&mu);
    if (dir_calls < 16) snprintf(dir_asked[dir_calls], sizeof dir_asked[dir_calls], "%s", url);
    dir_calls++;
    pthread_mutex_unlock(&mu);
    if (!dir_on || strstr(url, "dirdown")) return -1;
    if (!strstr(url, "/json/stations/byurl?url=http%3A%2F%2Fstream.example%2F")) return snprintf(buf, cap, "<html>not the directory</html>");
    if (strstr(url, "%2Fknown-site")) return snprintf(buf, cap, "[{\"name\":\"K\",\"homepage\":\"https://touch.example/\",\"favicon\":\"\"}]");
    if (strstr(url, "%2Fknown-logo"))
        return snprintf(buf, cap, "[{\"name\":\"L\",\"homepage\":\"\",\"favicon\":\"null\"},"
                                  "{\"name\":\"L2\",\"homepage\":\"https://bare.example/\",\"favicon\":\"https://img.example/px68.png\"}]");
    if (strstr(url, "%2Fknown-both"))                /* its logo is no use: its site, then */
        return snprintf(buf, cap, "[{\"homepage\":\"https://long.example/\",\"favicon\":\"https://img.example/svg-dir.png\"}]");
    if (strstr(url, "%2Fknown-nothing")) return snprintf(buf, cap, "[{\"name\":\"N\",\"homepage\":\"\",\"favicon\":\"\"}]");
    if (strstr(url, "%2Fmemo")) return snprintf(buf, cap, "[{\"homepage\":\"\",\"favicon\":\"https://unreachable.example/memo.png\"}]");
    if (strstr(url, "%2Fsame-logo")) return snprintf(buf, cap, "[{\"homepage\":\"\",\"favicon\":\"https://img.example/svg-own.png\"}]");
    return snprintf(buf, cap, "[]");                 /* a stream the directory doesn't have */
}

static long size_of(const char *path);

static int dir_asked_for(const char *stream_tail)
{
    int k = 0;
    pthread_mutex_lock(&mu);
    for (int i = 0; i < dir_calls && i < 16; i++) k += strstr(dir_asked[i], stream_tail) != NULL;
    pthread_mutex_unlock(&mu);
    return k;
}

static long tpic(const char *dir, const char *stream)
{
    char id[13], p[700];
    ricon_stream_id(stream, id);
    snprintf(p, sizeof p, "%s/%s.565", dir, id);
    return id[0] ? size_of(p) : -1;
}

static int tmarked(const char *dir, const char *stream)
{
    char id[13], p[700];
    ricon_stream_id(stream, id);
    snprintf(p, sizeof p, "%s/%s.none", dir, id);
    return id[0] && size_of(p) == 0;
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

/* the same for a station's site */
static long spic(const char *dir, const char *site)
{
    char id[13], p[700];
    ricon_site_id(site, id);
    snprintf(p, sizeof p, "%s/%s.565", dir, id);
    return id[0] ? size_of(p) : -1;
}

static int smarked(const char *dir, const char *site)
{
    char id[13], p[700];
    ricon_site_id(site, id);
    snprintf(p, sizeof p, "%s/%s.none", dir, id);
    return id[0] && size_of(p) == 0;
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
    rdir_set_fetch(fake_dir);                       /* (silent until the part about it, below) */
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

    /* ---- stations with no logo of their own: the icon their site names ---- */
    char sdir[600];
    snprintf(sdir, sizeof sdir, "%s/site-icons", tmpl);
    CHECK(ricon_start(sdir) == 0);
    radio_station_t ss[12];
    int m = 0;
    #define SITE(nm, ic, st_) do { ss[m] = station(nm, ic); snprintf(ss[m].site, sizeof ss[m].site, "%s", st_); m++; } while (0)
    SITE("touch", "", "https://touch.example/");                       /* its page names a touch icon (and smaller ones) */
    SITE("rel", "", "https://rel.example/a/b/page.html?x=1");          /* a path beside the page, and one on another host */
    SITE("long", "", "https://long.example/");                         /* a page longer than is read */
    SITE("ico", "", "https://ico.example/radio/index.html");           /* names none: /favicon.ico, a 32-bit bitmap */
    SITE("ico24", "", "https://ico24.example/");                       /* no page: a 24-bit bitmap with a mask */
    SITE("icomask", "", "https://icomask.example/");                   /* 32 bits, no alpha: the mask */
    SITE("icopng", "", "https://icopng.example/");                     /* a PNG inside the .ico */
    SITE("badico", "", "https://badico.example/");                     /* an .ico cut short */
    SITE("bare", "", "https://bare.example/");                         /* nothing at all */
    SITE("down", "", "https://down.example/");                         /* not reached */
    SITE("own", "https://img.example/logo.jpg", "https://touch.example/");       /* a logo of its own: the site isn't asked */
    SITE("fallback", "https://img.example/svg-logo2.png", "https://long.example/");   /* its logo is no use: the site's */
    #undef SITE
    calls = 0;
    seq = ricon_seq();
    ricon_sync(ss, m, 1);
    settle();
    CHECK(spic(sdir, ss[0].site) == BOTH && !smarked(sdir, ss[0].site));
    CHECK(asked_for("https://touch.example/img/touch-px180.png?v=1&x=2") == 1 && asked_for("https://touch.example/fav-px16.png") == 0 &&
          asked_for("https://touch.example/logo.svg") == 0 && asked_for("https://touch.example/pin.svg") == 0);
    CHECK(spic(sdir, ss[1].site) == BOTH && asked_for("https://rel.example/a/b/icons/px96.png") == 1 &&
          asked_for("https://cdn.example/tiny-px16.png") == 0);
    CHECK(spic(sdir, ss[2].site) == BOTH && asked_for("https://long.example/t120.png") == 1);
    CHECK(spic(sdir, ss[3].site) == SMALL && asked_for("https://ico.example/apple-touch-icon.png") == 1 &&
          asked_for("https://ico.example/favicon.ico") == 1);
    CHECK(spic(sdir, ss[4].site) == SMALL);
    CHECK(spic(sdir, ss[5].site) == SMALL);
    CHECK(spic(sdir, ss[6].site) == SMALL);                            /* 64 pixels: under the large size's need */
    CHECK(spic(sdir, ss[7].site) == -1 && smarked(sdir, ss[7].site));
    CHECK(spic(sdir, ss[8].site) == -1 && smarked(sdir, ss[8].site) && asked_for("https://bare.example/favicon.ico") == 1);
    CHECK(spic(sdir, ss[9].site) == -1 && !smarked(sdir, ss[9].site));
    CHECK(pic(ss[10].icon) == BOTH && asked_for("https://touch.example/") == 1);          /* (once: for "touch") */
    CHECK(marked(ss[11].icon) && asked_for("https://long.example/") == 1);                /* (shared with "long") */
    CHECK(ricon_seq() - seq == 8);                                     /* 7 sites' icons and one logo */
    /* what the pixels are: the bitmap's blue, and white where its mask clears it */
    {
        static uint16_t px[LIB_ART_SMALL * LIB_ART_SMALL];
        char sid[13];
        ricon_site_id(ss[4].site, sid);
        CHECK(ricon_read(sid, LIB_ART_SMALL, px) == 1);
        /* (red 20, green 20, blue 250, as RGB565; the last bit of each is dithered) */
        #define BLUE(v) (((v) & 0x1f) >= 30 && ((v) >> 11) <= 3 && (((v) >> 5) & 0x3f) <= 6)
        CHECK(px[17 * LIB_ART_SMALL + 4] == 0xFFFF && BLUE(px[17 * LIB_ART_SMALL + 28]));
        ricon_site_id(ss[5].site, sid);
        CHECK(ricon_read(sid, LIB_ART_SMALL, px) == 1 && px[17 * LIB_ART_SMALL + 4] == 0xFFFF && BLUE(px[17 * LIB_ART_SMALL + 28]));
        ricon_site_id(ss[3].site, sid);
        CHECK(ricon_read(sid, LIB_ART_SMALL, px) == 1 && BLUE(px[17 * LIB_ART_SMALL + 4]));
        #undef BLUE
        CHECK(ricon_read(sid, LIB_ART_LARGE, (uint16_t *)q) == 0);    /* (no large part to read) */
    }
    /* the picture a station shows: its own logo's, or its site's when that's the one there */
    char own[13], want[13];
    ricon_station_id(&ss[10], own);
    ricon_id(ss[10].icon, want);
    CHECK(!strcmp(own, want));
    ricon_station_id(&ss[11], own);
    ricon_site_id(ss[11].site, want);
    CHECK(!strcmp(own, want));
    ricon_station_id(&ss[0], own);
    ricon_site_id(ss[0].site, want);
    CHECK(!strcmp(own, want) && strlen(own) == 12);
    ricon_station_id(&st[6], own);                                     /* neither: what the directory may lead to */
    ricon_stream_id(st[6].url, want);
    CHECK(!strcmp(own, want) && strlen(own) == 12);
    ricon_site_id("ftp://x.example/", own);
    CHECK(!own[0]);
    /* nothing is asked for twice; a site that's left the list loses its files */
    int calls0 = calls;
    ricon_sync(ss, m, 1);
    settle();
    CHECK(calls == calls0 + 1);                                        /* (only the one not reached) */
    ricon_sync(ss + 1, m - 1, 1);
    settle();
    CHECK(spic(sdir, ss[0].site) == BOTH);                             /* ("own" names the same site still) */
    ricon_sync(ss + 1, 9, 1);
    settle();
    CHECK(spic(sdir, ss[0].site) == -1 && spic(sdir, ss[1].site) == BOTH);
    ricon_stop();

    /* ---- stations that name neither a logo nor a site: the directory is
     * asked what it has for their stream, once; never about a stream whose
     * address looks private ---- */
    CHECK(ricon_stream_askable("http://stream.example/a") && ricon_stream_askable("https://a.b.example:8443/live.mp3") &&
          ricon_stream_askable("http://81.2.69.142:8000/stream"));
    CHECK(!ricon_stream_askable("http://user:key@stream.example/a") && !ricon_stream_askable("http://stream.example/a?token=1") &&
          !ricon_stream_askable("http://stream.example/a#x") && !ricon_stream_askable("ftp://stream.example/a") &&
          !ricon_stream_askable("") && !ricon_stream_askable(NULL));
    CHECK(!ricon_stream_askable("http://192.168.1.5:8000/s") && !ricon_stream_askable("http://10.0.0.2/s") &&
          !ricon_stream_askable("http://172.20.1.1/s") && !ricon_stream_askable("http://127.0.0.1:9/s") &&
          !ricon_stream_askable("http://169.254.3.3/s") && !ricon_stream_askable("http://100.72.0.9/s") &&
          ricon_stream_askable("http://172.32.1.1/s") && ricon_stream_askable("http://192.169.1.1/s"));
    CHECK(!ricon_stream_askable("http://nas.local/s") && !ricon_stream_askable("http://NAS.LAN:8000/s") &&
          !ricon_stream_askable("http://localhost:8000/s") && !ricon_stream_askable("http://radio/s") &&
          !ricon_stream_askable("http://[::1]:8000/s") && !ricon_stream_askable("http://pi.home.arpa/s"));
    /* private addresses written otherwise (the resolver takes short, hex and
     * octal numbers, and a dot at the end), the names home routers and
     * dynamic-DNS services give, a key in the path: none sent (the review of
     * 2026-11-24 ran each of the first three lines through as askable) */
    CHECK(!ricon_stream_askable("http://10.1/s") && !ricon_stream_askable("http://192.168.1/s") &&
          !ricon_stream_askable("http://0xc0.0xa8.1.10:8000/s") && !ricon_stream_askable("http://012.1.2.3/s") &&
          !ricon_stream_askable("http://3232235777/s") && !ricon_stream_askable("http://1.2.3.4.5/s") &&
          !ricon_stream_askable("http://256.1.1.1/s") && !ricon_stream_askable("http://192.168.1.10./s"));
    CHECK(!ricon_stream_askable("http://nas.local./s") && !ricon_stream_askable("http://nas.fritz.box:8000/s") &&
          !ricon_stream_askable("http://fritz.box/s") && !ricon_stream_askable("http://music.corp/s") &&
          !ricon_stream_askable("http://router.intranet/s") && !ricon_stream_askable("http://speedport.ip/s") &&
          !ricon_stream_askable("http://nas.tail1234.ts.net/s") && !ricon_stream_askable("https://myhome.duckdns.org:8443/radio") &&
          !ricon_stream_askable("http://me.ddns.net/s") && !ricon_stream_askable("http://box.myfritz.net/s") &&
          !ricon_stream_askable("http://stream.example../s"));
    CHECK(!ricon_stream_askable("https://stream.example/listen/a1b2c3d4e5f6a7b8c9d0e1f2/stream.mp3") &&
          !ricon_stream_askable("http://stream.example/stream;key=abc") &&
          !ricon_stream_askable("https://stream.example/0123456789abcdef0123456789abcdef.mp3"));
    /* ...and stations' own: a name that starts with a digit, a number out on
     * the internet, a Shoutcast ";", a long word that is only letters, a
     * station's short id */
    CHECK(ricon_stream_askable("http://181.fm/stream") && ricon_stream_askable("https://1live.example/a.mp3") &&
          ricon_stream_askable("http://8.8.8.8/s") && ricon_stream_askable("http://stream.example:8000/;stream.mp3") &&
          ricon_stream_askable("https://stream.example/radioparadiseworldmusicmix-flac") &&
          ricon_stream_askable("https://stream.example/0r0xa792kwzuv") &&
          ricon_stream_askable("https://stream10.example/audio15s976748-2280/stream/icecast.audio") &&
          ricon_stream_askable("http://stream.example./a"));
    char tdir[600];
    snprintf(tdir, sizeof tdir, "%s/stream-icons", tmpl);
    CHECK(ricon_start(tdir) == 0);
    radio_station_t ts[12];
    int t = 0;
    #define STREAM(nm, ic, si) do { ts[t] = station(nm, ic); snprintf(ts[t].site, sizeof ts[t].site, "%s", si); t++; } while (0)
    STREAM("known-site", "", "");                                      /* the directory has its site: the icon that names */
    STREAM("known-logo", "", "");                                      /* ...its logo (listed twice: the first that's an address) */
    STREAM("known-both", "", "");                                      /* ...a logo that's no use, and a site */
    STREAM("known-nothing", "", "");                                   /* ...neither */
    STREAM("unlisted", "", "");                                        /* the directory hasn't got the stream */
    STREAM("same-logo", "https://img.example/svg-own.png", "");        /* its own logo was no use; the directory has the same one */
    STREAM("dirdown", "", "");                                         /* no answer (and the directory isn't asked again in that run: after the others) */
    STREAM("has-logo", "https://img.example/logo.jpg", "");            /* not asked about */
    STREAM("has-site", "", "https://rel.example/a/b/page.html?x=1");   /* not asked about */
    STREAM("private", "", "");
    snprintf(ts[t - 1].url, sizeof ts[t - 1].url, "http://stream.example/private?key=secret");
    STREAM("home", "", "");
    snprintf(ts[t - 1].url, sizeof ts[t - 1].url, "http://192.168.1.20:8000/home");
    #undef STREAM
    dir_on = 1;
    dir_calls = calls = 0;
    ricon_sync(ts, t, 1);
    settle();
    CHECK(tpic(tdir, ts[0].url) == BOTH && !tmarked(tdir, ts[0].url) && dir_asked_for("%2Fknown-site") == 1);
    CHECK(tpic(tdir, ts[1].url) == BOTH && asked_for("https://img.example/px68.png") == 1 && asked_for("https://bare.example/") == 0);
    CHECK(tpic(tdir, ts[2].url) == BOTH && asked_for("https://img.example/svg-dir.png") == 1 && asked_for("https://long.example/t120.png") == 1);
    CHECK(tpic(tdir, ts[3].url) == -1 && tmarked(tdir, ts[3].url));
    CHECK(tpic(tdir, ts[4].url) == -1 && tmarked(tdir, ts[4].url) && dir_asked_for("%2Funlisted") == 1);
    CHECK(tpic(tdir, ts[6].url) == -1 && !tmarked(tdir, ts[6].url));
    CHECK(marked(ts[5].icon) && tmarked(tdir, ts[5].url) && asked_for("https://img.example/svg-own.png") == 1);
    CHECK(dir_asked_for("%2Fhas-logo") == 0 && dir_asked_for("%2Fhas-site") == 0);
    CHECK(dir_asked_for("private") == 0 && dir_asked_for("secret") == 0 && dir_asked_for("192.168") == 0 && dir_asked_for("%2Fhome") == 0);
    ricon_stream_id(ts[9].url, own);
    CHECK(!own[0]);
    ricon_station_id(&ts[9], own);                                     /* (a private stream with nothing: no picture, and no id) */
    CHECK(!own[0]);
    ricon_station_id(&ts[0], own);
    ricon_stream_id(ts[0].url, want);
    CHECK(!strcmp(own, want));
    ricon_station_id(&ts[7], own);
    ricon_id(ts[7].icon, want);
    CHECK(!strcmp(own, want));
    /* asked once: again only about the one that got no answer, and that twice at most */
    int dir0 = dir_calls;
    for (int k = 0; k < 4; k++) {
        ricon_sync(ts, t, 1);
        settle();
    }
    CHECK(dir_calls - dir0 <= 3 && dir_asked_for("%2Fknown-site") == 1 && dir_asked_for("%2Funlisted") == 1);
    ricon_stop();

    /* ---- a logo on a server that's gone; the directory asked once ---- */
    {
        char ddir[600];
        snprintf(ddir, sizeof ddir, "%s/dead-logos", tmpl);
        CHECK(ricon_start(ddir) == 0);
        ricon_test_forget_unreached();
        radio_station_t ds[8];
        int d = 0;
        #define DEAD(nm, ic, si) do { ds[d] = station(nm, ic); snprintf(ds[d].site, sizeof ds[d].site, "%s", si); d++; } while (0)
        DEAD("dead1", "https://unreachable.example/d1.png", "https://touch.example/");   /* its site's icon, in the same run */
        DEAD("dead2", "https://unreachable.example/d2.png", "https://long.example/");
        DEAD("dead3", "https://unreachable.example/d3.png", "https://rel.example/a/b/page.html?x=1");
        DEAD("after", "https://img.example/logo.jpg", "");            /* three dead logos before it: fetched all the same */
        DEAD("dead-alone", "https://unreachable.example/d4.png", ""); /* a logo that may be back: the directory isn't told of its stream */
        DEAD("memo", "", "");                                         /* the directory names a logo on a server not reached */
        DEAD("last", "https://img.example/px68.png", "");
        #undef DEAD
        dir_calls = calls = 0;
        ricon_sync(ds, d, 1);
        settle();
        CHECK(spic(ddir, ds[0].site) == BOTH && spic(ddir, ds[1].site) == BOTH && spic(ddir, ds[2].site) == BOTH);
        CHECK(!marked(ds[0].icon) && pic(ds[0].icon) == -1);          /* (not marked: its server may be back) */
        CHECK(pic(ds[3].icon) == BOTH && pic(ds[6].icon) == BOTH);
        CHECK(dir_asked_for("%2Fdead-alone") == 0 && tpic(ddir, ds[4].url) == -1 && !tmarked(ddir, ds[4].url));
        char memo[700];
        ricon_station_id(&ds[0], own);                                /* the row's picture: its site's */
        ricon_site_id(ds[0].site, want);
        CHECK(!strcmp(own, want));
        /* the directory asked once about "memo" (three servers at most), and
         * its answer kept; at the next openings the logo it named is tried
         * again, the directory isn't */
        ricon_stream_id(ds[5].url, own);
        snprintf(memo, sizeof memo, "%s/%s.dir", ddir, own);
        int asked1 = dir_asked_for("%2Fmemo");
        CHECK(asked1 >= 1 && asked1 <= 3 && size_of(memo) > 0 && tpic(ddir, ds[5].url) == -1 && !tmarked(ddir, ds[5].url));
        CHECK(asked_for("https://unreachable.example/memo.png") == 1);
        calls = 0;
        ricon_sync(ds, d, 1);
        settle();
        CHECK(dir_asked_for("%2Fmemo") == asked1 && asked_for("https://unreachable.example/memo.png") == 1);
        ricon_test_forget_unreached();                                /* (as after a restart) */
        reach = 1;                                                    /* the servers are back: the logos, and the memo's */
        ricon_sync(ds, d, 1);
        settle();
        CHECK(dir_asked_for("%2Fmemo") == asked1 && tpic(ddir, ds[5].url) == BOTH && size_of(memo) == -1);
        CHECK(pic(ds[0].icon) == BOTH && pic(ds[4].icon) == BOTH);
        ricon_station_id(&ds[0], own);                                /* its own logo again, now that it's there */
        ricon_id(ds[0].icon, want);
        CHECK(!strcmp(own, want));
        reach = 0;
        /* the directory not answering: not asked again in that run, and the
         * others' icons fetched all the same */
        ricon_stop();
        snprintf(ddir, sizeof ddir, "%s/dir-down", tmpl);
        CHECK(ricon_start(ddir) == 0);
        ricon_test_forget_unreached();
        radio_station_t dd[5];
        dd[0] = station("dirdown-a", "");
        dd[1] = station("dirdown-b", "");
        dd[2] = station("dirdown-c", "");
        dd[3] = station("dirdown-d", "");
        dd[4] = station("fine", "https://img.example/logo.jpg");
        dir_calls = calls = 0;
        ricon_sync(dd, 5, 1);
        settle();
        CHECK(dir_asked_for("dirdown-a") >= 1 && dir_asked_for("dirdown-b") == 0 && dir_asked_for("dirdown-d") == 0);
        CHECK(pic(dd[4].icon) == BOTH);
        ricon_stop();
    }
    dir_on = 0;

    /* ---- a page (or the directory) can't send the H2 to an address on the
     * home's own network: such an icon is passed over, unless the page itself
     * is on that host ---- */
    {
        char urls[1300];
        CHECK(ricon_host_public("https://img.example/a.png") && ricon_host_public("http://8.8.4.4/i.png?x=1") &&
              ricon_host_public("https://radio.duckdns.org/i.png"));      /* (a name of the internet's: fetched) */
        CHECK(!ricon_host_public("http://192.168.1.1/reboot") && !ricon_host_public("http://router/") &&
              !ricon_host_public("http://10.1/x") && !ricon_host_public("http://fritz.box/x") &&
              !ricon_host_public("http://nas.local:8080/i.png") && !ricon_host_public("http://[fe80::1]/x") &&
              !ricon_host_public("file:///etc/passwd") && !ricon_host_public("") && !ricon_host_public(NULL));
        const char *pg = "<link rel=\"apple-touch-icon\" sizes=\"180x180\" href=\"http://192.168.1.1/reboot?now=1\">"
                         "<link rel=\"icon\" sizes=\"96x96\" href=\"HTTP://router/cgi-bin/restart\">"
                         "<link rel=\"icon\" sizes=\"48x48\" href=\"/ok.png\">";
        CHECK(ricon_test_site(pg, strlen(pg), "https://tags.example/", urls, sizeof urls) == 1 &&
              !strcmp(urls, "https://tags.example/ok.png\n"));
        char j[200];                                                    /* (a site's address with a query and no path) */
        CHECK(ricon_test_join("https://ex.example?q=1", "/favicon.ico", j, sizeof j) == 0 && !strcmp(j, "https://ex.example/favicon.ico"));
        CHECK(ricon_test_join("https://ex.example?q=1", "i.png", j, sizeof j) == 0 && !strcmp(j, "https://ex.example/i.png"));
        CHECK(ricon_test_join("https://ex.example/a/b?x=/y", "i.png", j, sizeof j) == 0 && !strcmp(j, "https://ex.example/a/i.png"));
        const char *lanpg = "<link rel=\"icon\" sizes=\"96x96\" href=\"/logo.png\">"
                          "<link rel=\"icon\" sizes=\"48x48\" href=\"http://192.168.1.9/other.png\">";
        CHECK(ricon_test_site(lanpg, strlen(lanpg), "http://192.168.1.20:8000/radio/", urls, sizeof urls) == 1 &&
              !strcmp(urls, "http://192.168.1.20:8000/logo.png\n"));
    }

    /* ---- a site whose page names only a small favicon: the home-screen
     * icon at its usual address is asked for first (big enough for Now
     * Playing), the favicon not at all ---- */
    {
        char bdir[600];
        snprintf(bdir, sizeof bdir, "%s/small-named", tmpl);
        CHECK(ricon_start(bdir) == 0);
        ricon_test_forget_unreached();
        radio_station_t sn = station("smallnamed", "");
        snprintf(sn.site, sizeof sn.site, "https://smallnamed.example/");
        calls = 0;
        ricon_sync(&sn, 1, 1);
        settle();
        CHECK(spic(bdir, sn.site) == BOTH && asked_for("https://smallnamed.example/apple-touch-icon.png") == 1 &&
              asked_for("https://smallnamed.example/favicon.ico") == 0);
        ricon_stop();
    }

    /* ---- a page's tags as pages write them: an attribute with no value, no
     * space after a quoted one, spaces round the "=" (each lost the href) ---- */
    {
        char urls[1300];
        const char *pg = "<link rel=\"icon\" crossorigin href=\"/a-px96.png\" sizes=\"96x96\">"
                         "<link rel=\"apple-touch-icon\"href=\"/b.png\">"
                         "<link rel=icon type=image/png data-x href = \"c.png\" sizes = '48x48' />";
        CHECK(ricon_test_site(pg, strlen(pg), "https://tags.example/dir/page", urls, sizeof urls) == 3);
        CHECK(!strcmp(urls, "https://tags.example/b.png\nhttps://tags.example/a-px96.png\nhttps://tags.example/dir/c.png\n"));
        if (strcmp(urls, "https://tags.example/b.png\nhttps://tags.example/a-px96.png\nhttps://tags.example/dir/c.png\n")) fprintf(stderr, "got: %s", urls);
    }

    /* ---- an .ico made to go wrong (the readers on their own; many more, at
     * random, in tests/fuzz_radioicon.c) ---- */
    {
        static unsigned char ico[4096];
        int side = 0;
        long k = make_ico_chunk(ico, 5, 9);                /* a chunk that isn't the picture: passed over */
        CHECK(ricon_test_ico(ico, (size_t)k, &side) == 0 && side == 16);
        /* one that says it's 4 GB long, less 12 bytes. On the H2, where a
         * long is 32 bits, that was a step of 8 backwards, to the same chunk,
         * read again for ever (here, only a step past the end) */
        alarm(30);
        k = make_ico_chunk(ico, 0xFFFFFFF4u, 0);
        CHECK(ricon_test_ico(ico, (size_t)k, &side) == -1);
        k = make_ico_chunk(ico, 0xFFFFFFF0u, 4);           /* (a step back to its own length, taken for a chunk's name) */
        CHECK(ricon_test_ico(ico, (size_t)k, &side) == -1);
        k = make_ico_chunk(ico, 0x7FFFFFFCu, 0);           /* (the first that, with its CRC's 4 bytes, is more than such a long holds) */
        CHECK(ricon_test_ico(ico, (size_t)k, &side) == -1);
        alarm(0);
    }

    char cmd[700];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", tmpl);
    if (system(cmd)) {}
    if (!fails) printf("radioicon: ok\n");
    return fails != 0;
}
