/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* Radio stations' icons, fetched and kept small on the card (radioicon.h). */
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE
#define _GNU_SOURCE   /* syscall (glibc, musl) */
#include "net/radioicon.h"
#include "core/bytes.h"
#include "core/thread.h"

#include <ctype.h>
#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/resource.h>
#include <sys/syscall.h>
#endif

#include "core/fsafe.h"
#include "library/library.h"   /* library_logo_decode, LIB_ART_* */
#include "net/http.h"
#include "net/podcast.h"       /* pod_hash */
#include "net/radiodir.h"      /* rdir_lookup_stream */

#define SMALL_BYTES ((size_t)LIB_ART_SMALL * LIB_ART_SMALL * 2)
#define LARGE_BYTES ((size_t)LIB_ART_LARGE * LIB_ART_LARGE * 2)
#define OFFLINE_AFTER 3        /* fetches in a row that reached no server: the network's gone, the rest wait */

#define SITE_PAGE_MAX (96 * 1024)   /* of a station's web page: its head, where the icons are named */
#define SITE_ICONS 3                /* the icons its page names that are tried, the biggest first */

/* A run's work: the icon addresses, and the sites, of the stations as they
 * were when asked */
typedef struct {
    int n, net;
    char (*icon)[sizeof ((radio_station_t *)0)->icon];
    char (*site)[sizeof ((radio_station_t *)0)->site];
    char (*url)[sizeof ((radio_station_t *)0)->url];
} job_t;

static struct {
    pthread_mutex_t mu;
    char dir[512];
    int started;
    int running;               /* a thread is at work (under mu) */
    job_t *pending;            /* the list given while it was: its turn next (under mu) */
    volatile int abort;        /* ricon_stop: the fetch under way and the run give up */
    unsigned seq;
} R = {.mu = PTHREAD_MUTEX_INITIALIZER};

static http_get_fn g_fetch = http_get;
void ricon_set_fetch(http_get_fn fn) { g_fetch = fn ? fn : http_get; }

void ricon_id(const char *icon_url, char id[13])
{
    id[0] = '\0';
    if (radio_icon_ok(icon_url)) pod_hash(icon_url, id);
}

static void id_path(const char *id, const char *ext, char *out, size_t cap)
{
    snprintf(out, cap, "%s/%s.%s", R.dir, id, ext);
}

/* A mark of "no use" (ID.none) still stands: one over a week old doesn't,
 * and its address is asked again. A logo marked so for a network's sign-in
 * page answered in its place, or a server's "too many requests" (which reads
 * as "not there"), stayed without its picture for good. For a mark that was
 * right: one fetch a week. (A clock set back: it stands.) */
#define NONE_DAYS 7
static int marked(const char *none)
{
    struct stat st;
    return stat(none, &st) == 0 && (long long)time(NULL) - (long long)st.st_mtime <= NONE_DAYS * 86400LL;
}

/* A picture's file with both sizes in it. One with the list's alone is a
 * small icon kept before those had Now Playing's too: still shown in the
 * list, and fetched once more for the rest. */
static int whole(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && (size_t)st.st_size > SMALL_BYTES;
}

void ricon_site_id(const char *site_url, char id[13])
{
    id[0] = '\0';
    if (!radio_icon_ok(site_url)) return;
    char key[sizeof ((radio_station_t *)0)->site + 8];   /* (apart from the same address given as an icon) */
    snprintf(key, sizeof key, "site:%s", site_url);
    pod_hash(key, id);
}

/* A stream's address that may be given to the directory: http or https, on a
 * host out on the internet by its name or number, with nothing in it that
 * looks like a key. Refused, each because a private stream has it:
 *   - a login, a query, a fragment, or "=" anywhere (";key=abc" in a path)
 *   - a host with no dot, an IPv6 number, a name of a home network's own
 *     (.local, fritz.box, ...) or of a dynamic-DNS service (a server at home
 *     with a public name), a trailing dot ignored
 *   - a number that isn't four plain decimal parts out on the internet: the
 *     resolver also takes "10.1", "0xc0.0xa8.1.10" and "012.1.2.3" (octal),
 *     which are private addresses written otherwise
 *   - a part of the path that looks like a key: 24 or more letters and
 *     digits, both among them
 * (A server of one's own under a name of one's own can't be told from a
 * station's: the guide says so.) */
static int key_like(const char *seg, size_t n)
{
    int letters = 0, digits = 0;
    if (n < 24) return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)seg[i];
        if (isdigit(c)) digits = 1;
        else if (isalpha(c)) letters = 1;
        else if (c != '-' && c != '_') return 0;
    }
    return letters && digits;
}

/* The host of an http(s) address is out on the internet, by its name or its
 * number: 1, else 0 (see ricon_stream_askable; dyn: a dynamic-DNS name is
 * refused too). */
static int host_public(const char *url, int dyn)
{
    const char *h = !strncmp(url, "http://", 7) ? url + 7 : !strncmp(url, "https://", 8) ? url + 8 : NULL;
    if (!h) return 0;
    size_t al = strcspn(h, "/?#");                   /* the authority: [login@]host[:port] */
    if (memchr(h, '@', al) || h[0] == '[') return 0; /* a login; an IPv6 number (as likely a home's as not) */
    char host[128];
    size_t hl = strcspn(h, ":/?#");
    if (hl == 0 || hl >= sizeof host) return 0;
    memcpy(host, h, hl);
    host[hl] = '\0';
    if (host[hl - 1] == '.') host[--hl] = '\0';      /* "nas.local.": the same host */
    if (hl == 0 || host[hl - 1] == '.') return 0;
    for (char *c = host; *c; c++) *c = (char)tolower((unsigned char)*c);
    const char *dot = strrchr(host, '.');
    if (!dot) return 0;                              /* "localhost", "nas": a name of the home's own */
    /* a home or office network's own names; then (dyn) a private network
     * with public names and dynamic DNS: a server at home */
    static const char *const HOME[] = {
        ".local", ".lan", ".home", ".internal", ".localdomain", ".localhost", ".home.arpa", ".test", ".invalid",
        ".corp", ".intranet", ".private", ".box", ".ip", ".router", ".gateway", ".wlan", ".mshome.net",
        ".onion", ".i2p",
        ".ts.net", ".duckdns.org", ".ddns.net", ".no-ip.org", ".no-ip.com", ".no-ip.biz", ".noip.me", ".hopto.org",
        ".zapto.org", ".sytes.net", ".dyndns.org", ".dynu.net", ".dynv6.net", ".freeddns.org", ".mooo.com",
        ".synology.me", ".myds.me", ".dscloud.me", ".myfritz.net", ".myqnapcloud.com", ".tplinkdns.com",
        ".asuscomm.com", ".selfhost.de", ".selfhost.eu", ".spdns.de", ".goip.de", ".dedyn.io"};
    enum { HOME_OWN = 20 };                          /* (the first so many: the network's own) */
    for (size_t i = 0; i < (dyn ? sizeof HOME / sizeof *HOME : (size_t)HOME_OWN); i++) {
        size_t n = strlen(HOME[i]);
        if (hl >= n && !strcmp(host + hl - n, HOME[i])) return 0;
        if (hl == n - 1 && !strcmp(host, HOME[i] + 1)) return 0;   /* ("fritz.box" has ".box"; "duckdns.org" itself) */
    }
    if (isdigit((unsigned char)dot[1])) {
        /* a number (its last part starts with a digit, as no name's does:
         * "181.fm" is a name): only four plain decimal parts will do */
        unsigned part[4];
        const char *c = host;
        for (int k = 0; k < 4; k++) {
            if (!isdigit((unsigned char)*c) || (c[0] == '0' && isdigit((unsigned char)c[1]))) return 0;   /* (no octal) */
            unsigned v = 0;
            int nd = 0;
            while (isdigit((unsigned char)*c) && nd < 4) v = v * 10 + (unsigned)(*c++ - '0'), nd++;
            if (nd > 3 || v > 255) return 0;
            if (*c != (k < 3 ? '.' : '\0')) return 0;
            c++;
            part[k] = v;
        }
        unsigned a = part[0], b = part[1];
        if (a == 10 || a == 127 || a == 0 || (a == 192 && b == 168) || (a == 172 && b >= 16 && b <= 31) ||
            (a == 169 && b == 254) || (a == 100 && b >= 64 && b <= 127) || a >= 224)
            return 0;
    }
    return 1;
}

int ricon_host_public(const char *url) { return url && host_public(url, 0); }

/* The same host (and port), whatever the paths: an address a page names on
 * its own server. */
static int same_host(const char *a, const char *b)
{
    const char *ha = strstr(a, "://"), *hb = strstr(b, "://");
    if (!ha || !hb) return 0;
    ha += 3, hb += 3;
    size_t la = strcspn(ha, "/?#"), lb = strcspn(hb, "/?#");
    return la == lb && !strncasecmp(ha, hb, la);
}

int ricon_stream_askable(const char *url)
{
    if (!url) return 0;
    const char *h = !strncmp(url, "http://", 7) ? url + 7 : !strncmp(url, "https://", 8) ? url + 8 : NULL;
    if (!h || strchr(url, '?') || strchr(url, '#') || strchr(h, '=')) return 0;
    size_t al = strcspn(h, "/");
    if (!host_public(url, 1)) return 0;
    for (const char *seg = h + al; *seg;) {          /* the path, part by part */
        while (*seg == '/') seg++;
        size_t n = strcspn(seg, "/");
        size_t stem = n;                             /* ("abc...xyz.mp3": the name before its ending) */
        for (size_t k = 0; k < n; k++)
            if (seg[k] == '.' || seg[k] == ';') { stem = k; break; }
        if (key_like(seg, stem)) return 0;
        seg += n;
    }
    return 1;
}

void ricon_stream_id(const char *stream_url, char id[13])
{
    id[0] = '\0';
    if (!ricon_stream_askable(stream_url)) return;
    char key[sizeof ((radio_station_t *)0)->url + 8];
    snprintf(key, sizeof key, "stream:%s", stream_url);
    pod_hash(key, id);
}

void ricon_station_id(const radio_station_t *st, char id[13])
{
    char ids[3][13], p[700];
    id[0] = '\0';
    if (!st) return;
    ricon_id(st->icon, ids[0]);                     /* its own logo; its site's icon; what the directory led to */
    ricon_site_id(st->site, ids[1]);
    ricon_stream_id(st->url, ids[2]);
    int which = ids[0][0] ? 0 : ids[1][0] ? 1 : 2;  /* the first it names, unless a later one is the one on the card */
    for (int k = 0; k < 3 && R.dir[0]; k++) {
        if (!ids[k][0]) continue;
        id_path(ids[k], "565", p, sizeof p);
        if (file_exists(p)) { which = k; break; }
    }
    memcpy(id, ids[which], 13);
}

int ricon_path(const char *icon_url, char *out, size_t cap)
{
    char id[13];
    ricon_id(icon_url, id);
    if (cap) out[0] = '\0';
    if (!id[0] || !R.dir[0]) return -1;
    id_path(id, "565", out, cap);
    return 0;
}

int ricon_read(const char *id, int size, uint16_t *px)
{
    if (!id || strlen(id) != 12 || !R.dir[0] || (size != LIB_ART_SMALL && size != LIB_ART_LARGE)) return 0;
    char p[700];
    id_path(id, "565", p, sizeof p);
    FILE *f = fopen(p, "rbe");
    if (!f) return 0;
    size_t want = size == LIB_ART_SMALL ? SMALL_BYTES : LARGE_BYTES;
    int ok = (size == LIB_ART_SMALL || fseek(f, (long)SMALL_BYTES, SEEK_SET) == 0) && fread(px, 1, want, f) == want;
    fclose(f);
    return ok;
}

unsigned ricon_seq(void)
{
    pthread_mutex_lock(&R.mu);
    unsigned s = R.seq;
    pthread_mutex_unlock(&R.mu);
    return s;
}

int ricon_busy(void)
{
    pthread_mutex_lock(&R.mu);
    int b = R.running;
    pthread_mutex_unlock(&R.mu);
    return b;
}

/* ---------- a run ---------- */

/* "0123456789ab.565", ".none" or ".dir": the id into id, else 0 */
static int icon_file(const char *name, char id[13])
{
    size_t n = strlen(name);
    if (n < 14 || name[12] != '.' || (strcmp(name + 13, "565") && strcmp(name + 13, "none") && strcmp(name + 13, "dir"))) return 0;
    for (int i = 0; i < 12; i++)
        if (!isxdigit((unsigned char)name[i])) return 0;
    memcpy(id, name, 12);
    id[12] = '\0';
    return 1;
}

/* What a run cut short left (*.tmp: only a run writes them, and there's one
 * at a time), and the icons of stations that aren't in the list any more
 * (ids: each station's icon's, then each one's site's, then each one's stream's) */
static void sweep(const job_t *j, char (*ids)[13])
{
    DIR *d = opendir(R.dir);
    struct dirent *e;
    char p[900], id[13];
    while (d && (e = readdir(d))) {
        size_t n = strlen(e->d_name);
        int drop = n > 4 && !strcmp(e->d_name + n - 4, ".tmp");
        if (!drop && icon_file(e->d_name, id)) {
            drop = 1;
            for (int i = 0; i < 3 * j->n && drop; i++) drop = strcmp(ids[i], id) != 0;
        }
        if (drop) {
            snprintf(p, sizeof p, "%s/%s", R.dir, e->d_name);
            remove(p);
        }
    }
    if (d) closedir(d);
}

/* path written whole: beside (path.tmp), on the card, then moved over. 0, -1 */
static int put(const char *path, const void *a, size_t an, const void *b, size_t bn)
{
    mkdir(R.dir, 0755);
    return file_write_whole(path, a, an, b, bn);
}

/* ---- an .ico file: the classic favicon ----
 * A directory of pictures of several sizes, each a PNG or a Windows bitmap
 * without its file header (twice as tall as it says: the picture, then a mask
 * of 1 bit a pixel). The biggest is taken; bitmaps of 32 or 24 bits a pixel,
 * not compressed (what favicons are; the old 256-colour ones are left). Over
 * white, as a logo's transparent parts are. 0 and *side, or -1. */

static int ico_decode(const unsigned char *b, size_t n, uint16_t *large, uint16_t *small, int *side)
{
    if (n < 6 + 16 || le16(b) != 0 || le16(b + 2) != 1) return -1;
    unsigned count = le16(b + 4);
    if (count < 1 || count > 64 || 6 + (size_t)count * 16 > n) return -1;
    const unsigned char *best = NULL;
    int best_w = 0, best_bpp = 0;
    for (unsigned i = 0; i < count; i++) {
        const unsigned char *e = b + 6 + i * 16;
        int w = e[0] ? e[0] : 256, bpp = (int)le16(e + 6);
        unsigned long size = le32(e + 8), off = le32(e + 12);
        if (off > n || size > n - off || size < 40) continue;
        if (w > best_w || (w == best_w && bpp > best_bpp)) { best = e; best_w = w; best_bpp = bpp; }
    }
    if (!best) return -1;
    const unsigned char *d = b + le32(best + 12);
    size_t dn = (size_t)le32(best + 8);
    if (dn >= 8 && !memcmp(d, "\x89PNG\r\n\x1a\n", 8)) return library_logo_decode(d, dn, large, small, side);
    unsigned long hs = le32(d);
    long w = (long)le32(d + 4), h2 = (long)le32(d + 8);
    unsigned bpp = le16(d + 14);
    if (hs < 40 || hs > dn || w < 1 || w > 256 || h2 < 2 || h2 > 512 || (h2 & 1) || le32(d + 16) != 0 || (bpp != 32 && bpp != 24))
        return -1;
    long h = h2 / 2;
    size_t stride = ((size_t)w * bpp + 31) / 32 * 4, mstride = ((size_t)w + 31) / 32 * 4;
    if (hs + stride * (size_t)h > dn) return -1;
    const unsigned char *px = d + hs, *mask = px + stride * (size_t)h;
    int have_mask = hs + stride * (size_t)h + mstride * (size_t)h <= dn;
    int alpha = 0;                                   /* (32 bits with every alpha 0: the mask says what shows) */
    for (long y = 0; bpp == 32 && y < h && !alpha; y++)
        for (long x = 0; x < w && !alpha; x++) alpha = px[(size_t)y * stride + (size_t)x * 4 + 3] != 0;
    unsigned char *rgb = malloc((size_t)w * (size_t)h * 3);
    if (!rgb) return -1;
    for (long y = 0; y < h; y++) {
        const unsigned char *row = px + (size_t)(h - 1 - y) * stride, *mrow = mask + (size_t)(h - 1 - y) * mstride;
        for (long x = 0; x < w; x++) {
            const unsigned char *c = row + (size_t)x * (bpp / 8);
            unsigned a = alpha ? c[3] : have_mask && (mrow[x / 8] >> (7 - x % 8) & 1) ? 0 : 255;
            unsigned char *o = rgb + ((size_t)y * (size_t)w + (size_t)x) * 3;
            o[0] = (unsigned char)((c[2] * a + 255 * (255 - a) + 127) / 255);
            o[1] = (unsigned char)((c[1] * a + 255 * (255 - a) + 127) / 255);
            o[2] = (unsigned char)((c[0] * a + 255 * (255 - a) + 127) / 255);
        }
    }
    int r = library_logo_rgb(rgb, (int)w, (int)h, large, small);
    free(rgb);
    if (side) *side = (int)(w < h ? w : h);
    return r;
}

/* The picture at url, kept as path: 1 it's on the card now, 0 it's no use
 * (not there, too big, not a picture that can be read, too small), -1 its
 * server wasn't reached. */
static int get_picture(const char *url, const char *path)
{
    /* (one buffer a fetch, given back after it: nothing held between icons) */
    unsigned char *buf = malloc(RICON_MAX_FILE + 1);
    uint16_t *large = malloc(LARGE_BYTES), *small = malloc(SMALL_BYTES);
    int r = -1;
    if (buf && large && small) {
        long n = g_fetch(url, (char *)buf, RICON_MAX_FILE + 1, &R.abort);
        int side = 0, bad;
        if (R.abort || n == -1) {
            r = -1;
        } else if (n < 8 || n == -2 || n == -3) {
            r = 0;                                   /* not there, or over the cap */
        } else {
            if (!memcmp(buf, "\xff\xd8\xff", 3) || !memcmp(buf, "\x89PNG\r\n\x1a\n", 8))
                bad = library_logo_decode(buf, (size_t)n, large, small, &side);
            else if (!memcmp(buf, "\0\0\1\0", 4))
                bad = ico_decode(buf, (size_t)n, large, small, &side);
            else
                bad = 1;                             /* what it is, not what its name says: SVG, WebP, a web page */
            if (bad || side < RICON_SMALL_MIN) {
                r = 0;
            } else {
                /* (both sizes always: a favicon has Now Playing's too, sharp in the
                 * square's middle: library_logo_decode. It had the list's alone.) */
                r = put(path, small, SMALL_BYTES, large, LARGE_BYTES) == 0 ? 1 : -1;
            }
        }
    }
    free(buf);
    free(large);
    free(small);
    return r;
}

/* One icon: 1 its picture is on the card now, 0 it's no use (marked), -1 the
 * server wasn't reached (nothing kept: another time) */
static int fetch_one(const char *url, const char *id)
{
    char path[700], none[700];
    id_path(id, "565", path, sizeof path);
    id_path(id, "none", none, sizeof none);
    int r = get_picture(url, path);
    if (r == 0) put(none, NULL, 0, NULL, 0);
    return r;
}

/* ---- a station's own site: the icons its page names ----
 * <link rel="apple-touch-icon" href=...> (what a phone puts on its home
 * screen: 180 pixels as a rule, enough for Now Playing) and <link rel="icon">
 * (the favicon: the list only, unless it's a big one), with the sizes they
 * give. Then, named or not, the two addresses sites keep them at. */
typedef struct { char url[420]; int size; } site_icon_t;

static const char *find_ci(const char *s, const char *e, const char *what)
{
    size_t n = strlen(what);
    for (; s + n <= e; s++)
        if (!strncasecmp(s, what, n)) return s;
    return NULL;
}

/* An attribute of the tag [t, e): its value into out ("" if it has none).
 * Attributes as pages write them: with or without quotes, with spaces round
 * the "=", with no value at all (crossorigin), with no space after a quoted
 * value (rel="icon"href="..."). */
static void tag_attr(const char *t, const char *e, const char *name, char *out, size_t cap)
{
    size_t nl = strlen(name);
    out[0] = '\0';
    const char *p = t;
    while (p < e && !isspace((unsigned char)*p)) p++;          /* past "<link" */
    while (p < e) {
        while (p < e && (isspace((unsigned char)*p) || *p == '/')) p++;
        if (p >= e) break;
        const char *a = p;
        while (p < e && *p != '=' && !isspace((unsigned char)*p)) p++;
        size_t al = (size_t)(p - a);
        const char *q = p;
        while (q < e && isspace((unsigned char)*q)) q++;
        if (q >= e || *q != '=') continue;                     /* one with no value: the next starts here */
        p = q + 1;
        while (p < e && isspace((unsigned char)*p)) p++;
        const char *v = p, *ve;
        if (p < e && (*p == '"' || *p == '\'')) {
            char qc = *p++;
            v = p;
            while (p < e && *p != qc) p++;
            ve = p;
            if (p < e) p++;
        } else {
            while (p < e && !isspace((unsigned char)*p)) p++;
            ve = p;
        }
        if (al == nl && !strncasecmp(a, name, nl)) {
            size_t n = (size_t)(ve - v) < cap - 1 ? (size_t)(ve - v) : cap - 1;
            memcpy(out, v, n);
            out[n] = '\0';
            return;
        }
    }
}

/* href as the page at base names it, whole: 0, or -1 (not an address to fetch) */
static int url_join(const char *base, const char *href, char *out, size_t cap)
{
    while (isspace((unsigned char)*href)) href++;
    const char *path = strstr(base, "://");
    if (!path) return -1;
    size_t origin = (size_t)(path + 3 - base) + strcspn(path + 3, "/?#");   /* the origin ends here ("https://ex.com?q=1" has no path) */
    int n;
    if (!strncasecmp(href, "http://", 7) || !strncasecmp(href, "https://", 8)) {
        n = snprintf(out, cap, "%s", href);
    } else if (!strncmp(href, "//", 2)) {
        n = snprintf(out, cap, "%.*s%s", (int)(path + 1 - base), base, href);
    } else if (href[0] == '/') {
        n = snprintf(out, cap, "%.*s%s", (int)origin, base, href);
    } else if (!href[0] || strchr(href, ':') || !strncmp(href, "../", 3)) {
        return -1;                                   /* nothing, data: and the like, or a path that climbs */
    } else {
        if (!strncmp(href, "./", 2)) href += 2;
        const char *q = base + strcspn(base, "?#");
        const char *dir = q;                         /* the page's folder: up to its last '/' past the origin */
        while (dir > base + origin && dir[-1] != '/') dir--;
        if (dir <= base + origin) n = snprintf(out, cap, "%.*s/%s", (int)origin, base, href);
        else n = snprintf(out, cap, "%.*s%s", (int)(dir - base), base, href);
    }
    if (n < 0 || (size_t)n >= cap) return -1;
    for (char *a = out; (a = strstr(a, "&amp;")); a++) memmove(a + 1, a + 5, strlen(a + 5) + 1);
    out[strcspn(out, "#")] = '\0';
    for (char *e = out + strlen(out); e > out && isspace((unsigned char)e[-1]);) *--e = '\0';
    if (strncasecmp(out, "http://", 7) && strncasecmp(out, "https://", 8)) return -1;
    for (const char *c = out; *c; c++)               /* (a page's own addresses may be longer than a line's icon) */
        if ((unsigned char)*c <= ' ') return -1;
    return 0;
}

/* The icons the page (html, n bytes, fetched from base) names, the biggest
 * first: how many (max at most). */
static int site_icons(const char *html, size_t n, const char *base, site_icon_t *out, int max)
{
    int k = 0;
    const char *end = html + n;
    for (const char *t = html; (t = find_ci(t, end, "<link")) != NULL; t += 5) {
        size_t span = (size_t)(end - t) > 2001 ? 2001 : (size_t)(end - t);   /* (a tag longer than that isn't one) */
        const char *e = memchr(t, '>', span);
        if (!e) continue;
        char rel[64], href[400], sizes[32];
        tag_attr(t, e, "rel", rel, sizeof rel);
        int touch = find_ci(rel, rel + strlen(rel), "apple-touch-icon") != NULL;
        if (!touch && (!find_ci(rel, rel + strlen(rel), "icon") || find_ci(rel, rel + strlen(rel), "mask-icon"))) continue;
        tag_attr(t, e, "href", href, sizeof href);
        tag_attr(t, e, "sizes", sizes, sizeof sizes);
        site_icon_t c;
        if (url_join(base, href, c.url, sizeof c.url)) continue;
        /* (a page can name any address: one on the home's own network is
         * fetched only when the page itself is there) */
        if (!host_public(c.url, 0) && !same_host(c.url, base)) continue;
        size_t pl = strcspn(c.url, "?");
        if (pl >= 4 && !strncasecmp(c.url + pl - 4, ".svg", 4)) continue;
        c.size = atoi(sizes);
        if (c.size <= 0) c.size = touch ? 180 : 32;  /* (what each is when it doesn't say) */
        if (c.size > 600) c.size = 1;                /* (too big to be read: the last resort) */
        int at = k < max ? k : max - 1;
        if (k >= max && out[at].size >= c.size) continue;
        while (at > 0 && out[at - 1].size < c.size) { out[at] = out[at - 1]; at--; }
        out[at] = c;
        if (k < max) k++;
    }
    return k;
}

/* ---- tests: those readers on their own (radioicon.h) ---- */

int ricon_test_ico(const unsigned char *buf, size_t n, int *side)
{
    uint16_t *large = malloc(LARGE_BYTES), *small = malloc(SMALL_BYTES);
    int r = large && small ? ico_decode(buf, n, large, small, side) : -1;
    free(large);
    free(small);
    return r;
}

int ricon_test_site(const char *html, size_t n, const char *base, char *urls, size_t cap)
{
    site_icon_t cand[SITE_ICONS];                    /* (as many as are asked for, and no more: one written past them shows) */
    int k = site_icons(html, n, base, cand, SITE_ICONS);
    size_t at = 0;
    if (urls && cap) urls[0] = '\0';
    for (int i = 0; i < k && urls && at < cap; i++) at += (size_t)snprintf(urls + at, cap - at, "%s\n", cand[i].url);
    return k;
}

int ricon_test_join(const char *base, const char *href, char *out, size_t cap)
{
    return url_join(base, href, out, cap);
}

/* A station's site, for the icon it names, kept as path: 1 on the card now,
 * 0 it has none that can be used, -1 it (or what it names) wasn't reached. */
static int site_picture(const char *site, const char *path)
{
    site_icon_t cand[SITE_ICONS + 2];
    int nc = 0;
    char *page = malloc(SITE_PAGE_MAX);
    if (!page) return -1;
    long n = g_fetch(site, page, SITE_PAGE_MAX, &R.abort);
    if (n == -2) n = SITE_PAGE_MAX - 1;              /* (a long page: its start is enough) */
    if (R.abort || n == -1) { free(page); return -1; }
    if (n > 0) nc = site_icons(page, (size_t)n, site, cand, SITE_ICONS);
    free(page);
    /* The home-screen icon at its usual address goes before the small ones
     * the page names (they're the biggest first): a page that names only its
     * 32-pixel favicon stopped there, the list's picture and none for Now
     * Playing, with a 180-pixel one beside it unasked for. */
    int big = 0;
    while (big < nc && cand[big].size >= RICON_LARGE_MIN) big++;
    memmove(&cand[big + 1], &cand[big], (size_t)(nc - big) * sizeof cand[0]);
    if (url_join(site, "/apple-touch-icon.png", cand[big].url, sizeof cand[big].url) == 0) {
        cand[big].size = 0;
        nc++;
    } else {
        memmove(&cand[big], &cand[big + 1], (size_t)(nc - big) * sizeof cand[0]);
    }
    if (url_join(site, "/favicon.ico", cand[nc].url, sizeof cand[nc].url) == 0) cand[nc++].size = 0;
    int unreached = 0;
    for (int i = 0; i < nc && !R.abort; i++) {
        int dup = 0;
        for (int k = 0; k < i; k++) dup |= !strcmp(cand[k].url, cand[i].url);
        if (dup) continue;
        int r = get_picture(cand[i].url, path);
        if (r == 1) return 1;
        unreached |= r < 0;
    }
    return R.abort || unreached ? -1 : 0;
}

/* ...under the site's own id, marked when it has none: 1, 0, -1 as fetch_one */
static int fetch_site(const char *site, const char *id)
{
    char path[700], none[700];
    id_path(id, "565", path, sizeof path);
    id_path(id, "none", none, sizeof none);
    int r = site_picture(site, path);
    if (r == 0) put(none, NULL, 0, NULL, 0);
    return r;
}

/* A station that names neither a logo nor a site (one added before sites
 * were kept, or whose logo and site were no use): the directory is asked
 * what it has for that stream, once, and its logo, then its site's icon,
 * kept under the stream's id. 1, 0 (marked: the directory doesn't know the
 * stream, or what it gives is no use), -1 (a picture's server wasn't
 * reached), -2 (the directory wasn't).
 *
 * Once: what the directory answered is kept beside (ID.dir: the logo's
 * address, then the site's, a line each) until its pictures are had or found
 * to be no use. A logo it named on a server that wasn't reached had the
 * stream's address sent again at every try. */
static int fetch_stream(const char *stream, const char *own_icon, const char *id)
{
    char path[700], none[700], memo[700], site[256] = "", icon[256] = "", line[600];
    id_path(id, "565", path, sizeof path);
    id_path(id, "none", none, sizeof none);
    id_path(id, "dir", memo, sizeof memo);
    int known = 0;
    FILE *f = fopen(memo, "re");
    if (f) {                                          /* asked already: what it said then */
        if (fgets(icon, sizeof icon, f)) icon[strcspn(icon, "\r\n")] = '\0';
        if (fgets(site, sizeof site, f)) site[strcspn(site, "\r\n")] = '\0';
        fclose(f);
        if (!radio_icon_ok(icon) || !host_public(icon, 0)) icon[0] = '\0';
        if (!radio_icon_ok(site) || !host_public(site, 0)) site[0] = '\0';
        known = icon[0] || site[0];
    }
    if (!known) {
        known = rdir_lookup_stream(stream, site, sizeof site, icon, sizeof icon, &R.abort);
        if (R.abort || known < 0) return -2;          /* the directory itself wasn't reached */
        if (known && (icon[0] || site[0])) {
            int n = snprintf(line, sizeof line, "%s\n%s\n", icon, site);
            put(memo, line, (size_t)n, NULL, 0);
        }
    }
    int r = 0;
    if (known && icon[0] && strcmp(icon, own_icon)) r = get_picture(icon, path);   /* (not the one that was no use already) */
    if (r != 1 && known && site[0]) {
        int rs = site_picture(site, path);
        r = rs == 1 ? 1 : (r < 0 || rs < 0) ? -1 : 0;
    }
    if (r == 0) put(none, NULL, 0, NULL, 0);
    if (r >= 0) remove(memo);                         /* settled: the picture, or the mark, says it all */
    return r;
}

/* Icons whose server wasn't reached, and how often, since the player
 * started (the worker's own: one run at a time). One that can't be had (a
 * host that refuses the connection every time) was asked again at every
 * opening of Radio, a handshake each time: twice a session is enough. */
#define UNREACHED_MAX 2
static struct { char id[13]; int n; } g_unreached[48];

/* How often id wasn't reached (note: once more now). */
static int unreached_n(const char *id, int note)
{
    int free_at = -1;
    for (size_t i = 0; i < sizeof g_unreached / sizeof *g_unreached; i++) {
        if (!g_unreached[i].id[0]) { if (free_at < 0) free_at = (int)i; continue; }
        if (strcmp(g_unreached[i].id, id)) continue;
        if (note) g_unreached[i].n++;
        return g_unreached[i].n;
    }
    if (note && free_at >= 0) {
        snprintf(g_unreached[free_at].id, sizeof g_unreached[free_at].id, "%s", id);
        g_unreached[free_at].n = 1;
        return 1;
    }
    return 0;
}

void ricon_test_forget_unreached(void) { memset(g_unreached, 0, sizeof g_unreached); }

static void run_job(const job_t *j)
{
    /* each station's icon's id, then each one's site's, then each one's stream's */
    char (*ids)[13] = calloc((size_t)(j->n > 0 ? 3 * j->n : 1), sizeof *ids);
    if (!ids) return;
    for (int i = 0; i < j->n; i++) {
        ricon_id(j->icon[i], ids[i]);
        ricon_site_id(j->site[i], ids[j->n + i]);
        ricon_stream_id(j->url[i], ids[2 * j->n + i]);
    }
    sweep(j, ids);
    int unreached = 0, dir_down = 0;
    /* Station by station: its own logo; then, if it has none (or one that's
     * no use), the icon its site names; then, with neither, what the directory
     * has for its stream.
     *
     * "No use" is a logo marked so (its address answered, with nothing that
     * can be shown). A logo whose server wasn't reached isn't marked: it's
     * tried again another time. For the station's own site that's enough to
     * go on to it meanwhile (a logo on a server that's gone for good never
     * got its station an icon); the directory is asked only when everything
     * the station names is marked, so that a logo that didn't load for once
     * never sends the stream's address there.
     *
     * (Station by station, not source by source: a dead logo and then its
     * site's icon fetched is a network that's up, where three dead logos in
     * a row looked like one that's gone and ended the run. The directory
     * not answering is the directory's trouble, not the network's: it isn't
     * asked again in this run, and the other stations' icons carry on.) */
    for (int i = 0; i < j->n && j->net && !R.abort && unreached < OFFLINE_AFTER; i++)
        for (int pass = 0; pass < 3 && !R.abort && unreached < OFFLINE_AFTER; pass++) {
            char path[700], none[700];
            const char *id = ids[pass * j->n + i];
            if (!id[0]) continue;
            int skip = 0;
            for (int k = 0; k < pass && !skip; k++) {
                const char *was = ids[k * j->n + i];
                if (!was[0]) continue;
                id_path(was, "565", path, sizeof path);
                id_path(was, "none", none, sizeof none);
                if (whole(path)) skip = 1;                        /* it has its picture */
                else if (!marked(none)) skip = !(pass == 1 && unreached_n(was, 0) > 0);
            }
            if (skip) continue;
            id_path(id, "565", path, sizeof path);
            id_path(id, "none", none, sizeof none);
            if (whole(path) || marked(none) || unreached_n(id, 0) >= UNREACHED_MAX) continue;
            if (pass == 2 && dir_down) continue;
            int r = pass == 0 ? fetch_one(j->icon[i], id) : pass == 1 ? fetch_site(j->site[i], id)
                                                                      : fetch_stream(j->url[i], j->icon[i], id);
            /* (one that failed before in this session says nothing new
             * about the network: three such in a row, their sites' icons
             * already had, ended the second run) */
            if (r == -2) dir_down = 1;
            else if (r >= 0) unreached = 0;
            else if (unreached_n(id, 0) == 0) unreached++;
            if (r < 0 && !R.abort) unreached_n(id, 1);
            if (r == 1) {
                remove(none);                                     /* (an old mark asked again: wrong, as it turns out) */
                pthread_mutex_lock(&R.mu);
                R.seq++;
                pthread_mutex_unlock(&R.mu);
            }
        }
    free(ids);
}

static void job_free(job_t *j)
{
    if (j) free(j->icon);
    if (j) free(j->site);
    if (j) free(j->url);
    free(j);
}

static void *run(void *arg)
{
    job_t *j = arg;
    /* below the music and the UI (as the scan and the updater): a logo's
     * HTTPS handshake and its decoding are work for the H2's one core, just
     * when a station is likely to be started */
    thread_nice(19);
    while (j) {
        if (!R.abort) run_job(j);
        job_free(j);
        pthread_mutex_lock(&R.mu);
        j = R.pending;                               /* a list given meanwhile: its turn */
        R.pending = NULL;
        if (!j) R.running = 0;
        pthread_mutex_unlock(&R.mu);
    }
    return NULL;
}

void ricon_sync(const radio_station_t *st, int n, int net)
{
    if (!R.started || n < 0 || (n > 0 && !st)) return;
    job_t *j = calloc(1, sizeof *j);
    if (j) j->icon = calloc((size_t)(n > 0 ? n : 1), sizeof *j->icon);
    if (j) j->site = calloc((size_t)(n > 0 ? n : 1), sizeof *j->site);
    if (j) j->url = calloc((size_t)(n > 0 ? n : 1), sizeof *j->url);
    if (!j || !j->icon || !j->site || !j->url) { job_free(j); return; }
    j->n = n;
    j->net = net;
    for (int i = 0; i < n; i++) {
        memcpy(j->icon[i], st[i].icon, sizeof j->icon[i]);
        memcpy(j->site[i], st[i].site, sizeof j->site[i]);
        memcpy(j->url[i], st[i].url, sizeof j->url[i]);
    }
    pthread_mutex_lock(&R.mu);
    if (R.running) {                                 /* after the run under way (a list waiting already: replaced) */
        job_free(R.pending);
        R.pending = j;
        pthread_mutex_unlock(&R.mu);
        return;
    }
    R.running = 1;
    pthread_mutex_unlock(&R.mu);
    int r = thread_detached(run, j, 128 * 1024);     /* (as the radio's other fetches: device/radiometa.c) */
    if (r) {
        job_free(j);
        pthread_mutex_lock(&R.mu);
        R.running = 0;
        pthread_mutex_unlock(&R.mu);
    }
}

int ricon_start(const char *dir)
{
    pthread_mutex_lock(&R.mu);
    snprintf(R.dir, sizeof R.dir, "%s", dir ? dir : "");
    R.abort = 0;
    R.started = R.dir[0] != '\0';
    pthread_mutex_unlock(&R.mu);
    return 0;
}

void ricon_stop_ask(void)
{
    pthread_mutex_lock(&R.mu);
    R.started = 0;
    R.abort = 1;
    job_free(R.pending);
    R.pending = NULL;
    pthread_mutex_unlock(&R.mu);
}

int ricon_stop_wait(int ms)
{
    for (int t = 0; t < ms && ricon_busy(); t += 50) {   /* (its fetch ends on the flag; a decode is under a second) */
        struct timespec ts = {0, 50 * 1000 * 1000L};
        nanosleep(&ts, NULL);
    }
    /* (a run still there is inside a name lookup, which nothing interrupts:
     * left to end with the player; the next run removes the .tmp it was at) */
    return !ricon_busy();
}

void ricon_stop(void)
{
    ricon_stop_ask();
    ricon_stop_wait(5000);
}
