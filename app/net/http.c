/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* Fetching over HTTP(S) (http.h): FFmpeg's avio, as the radio uses it. */
#include "net/http.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <unistd.h>

/* Who's asking: Fathom, its version and where it's from (MusicBrainz asks
 * apps to say so, and is stricter with those that don't) */
static char g_ua[160] = "Fathom (HiFi Walker H2; https://github.com/raparc0r/fathom-h2)";

void http_set_version(const char *version)
{
    snprintf(g_ua, sizeof g_ua, "Fathom/%s (HiFi Walker H2; https://github.com/raparc0r/fathom-h2)",
             version && *version ? version : "dev");
}

#ifdef FATHOM_HAVE_FFMPEG
#include <libavformat/avio.h>
#include <libavutil/dict.h>

#include "audio/ff_util.h"

#define HTTP_TIMEOUT_US "15000000"

static int interrupted(void *ctx)
{
    const volatile int *abort = ctx;
    return abort && *abort;
}

/* *gone: the server said it isn't there (HTTP 4xx: not found, forbidden...),
 * as opposed to not reaching it (a name that didn't resolve, a timeout) */
static AVIOContext *open_url(const char *url, const volatile int *abort, int *gone)
{
    AVIOContext *pb = NULL;
    AVIOInterruptCB icb = {interrupted, (void *)abort};
    AVDictionary *opt = NULL;
    av_dict_set(&opt, "rw_timeout", HTTP_TIMEOUT_US, 0);
    av_dict_set(&opt, "user_agent", g_ua, 0);
    /* (FFmpeg follows redirects by itself.) Only the network: a podcast
     * feed's enclosure or image is a URL from anyone, and file:, a device
     * path or a bare tcp: to a LAN host would be opened as readily */
    av_dict_set(&opt, "protocol_whitelist", "http,https,tls,tcp,httpproxy", 0);
    ff_tls_options(&opt);
    int r = avio_open2(&pb, url, AVIO_FLAG_READ, &icb, &opt);
    av_dict_free(&opt);
    if (gone) *gone = r == AVERROR_HTTP_BAD_REQUEST || r == AVERROR_HTTP_UNAUTHORIZED || r == AVERROR_HTTP_FORBIDDEN ||
                      r == AVERROR_HTTP_NOT_FOUND || r == AVERROR_HTTP_OTHER_4XX;
    return r < 0 ? NULL : pb;
}

long http_get(const char *url, char *buf, size_t cap, const volatile int *abort)
{
    if (!buf || cap < 1) return -1;
    buf[0] = '\0';
    int gone = 0;
    AVIOContext *pb = open_url(url, abort, &gone);
    if (!pb) return gone ? -3 : -1;
    size_t n = 0;
    int cut = 0;
    for (;;) {
        if (n + 1 >= cap) {                          /* full: is there more? */
            unsigned char extra;
            cut = avio_read(pb, &extra, 1) > 0;
            break;
        }
        int k = avio_read(pb, (unsigned char *)buf + n, (int)(cap - 1 - n));
        if (k <= 0) break;
        n += (size_t)k;
    }
    int failed = pb->error < 0 && pb->error != AVERROR_EOF;
    avio_closep(&pb);
    buf[n] = '\0';
    if (abort && *abort) return -1;
    if (failed && !n) return -1;
    return cut ? -2 : (long)n;
}

/* A download stops before the card has less than this free: a server that
 * never stops sending (a live stream given as an episode or a cover, with no
 * length) would fill it, and the library, settings and an update with it */
#define DOWNLOAD_FLOOR (32ULL * 1024 * 1024)

static int card_nearly_full(FILE *f)
{
    struct statvfs v;
    return fstatvfs(fileno(f), &v) == 0 && (unsigned long long)v.f_bavail * v.f_frsize < DOWNLOAD_FLOOR;
}

int64_t http_download(const char *url, const char *path, http_progress_fn progress, void *ctx,
                      const volatile int *abort)
{
    int gone = 0;
    AVIOContext *pb = open_url(url, abort, &gone);
    if (!pb) return gone ? -3 : -1;
    char part[1024];
    snprintf(part, sizeof part, "%s.part", path);
    /* (each call its own: the podcasts' and the covers' workers download at once) */
    enum { CHUNK = 32768 };
    unsigned char *chunk = malloc(CHUNK);
    FILE *f = chunk ? fopen(part, "wbe") : NULL;
    if (!f) { free(chunk); avio_closep(&pb); return -1; }
    int64_t total = avio_size(pb);
    uint64_t done = 0;
    int ok = 1;
    for (;;) {
        int k = avio_read(pb, chunk, CHUNK);
        if (k == AVERROR_EOF || k == 0) break;
        if (k < 0 || fwrite(chunk, 1, (size_t)k, f) != (size_t)k || (abort && *abort)) { ok = 0; break; }
        done += (uint64_t)k;
        /* Each MB to the card and out of the page cache: a 16 MB download
         * would otherwise push out the player's own code (run from the card,
         * in 24 MB of RAM) and leave it all to write at once. */
        if ((done & 0xFFFFF) < (uint64_t)k && (fflush(f) || fdatasync(fileno(f)) || card_nearly_full(f))) {
            if (card_nearly_full(f)) fprintf(stderr, "http: download stopped: the card is nearly full\n");
            ok = 0;
            break;
        }
#ifdef POSIX_FADV_DONTNEED
        if ((done & 0xFFFFF) < (uint64_t)k) posix_fadvise(fileno(f), 0, 0, POSIX_FADV_DONTNEED);
#endif
        if (progress) progress(done, total > 0 ? (uint64_t)total : 0, ctx);
    }
    avio_closep(&pb);
    free(chunk);
    if (fclose(f)) ok = 0;
    if (ok && total > 0 && done != (uint64_t)total) ok = 0;   /* cut short */
    if (!ok || rename(part, path)) { remove(part); return -1; }
    return (int64_t)done;
}

#else   /* no FFmpeg (the desktop build): no network */

long http_get(const char *url, char *buf, size_t cap, const volatile int *abort)
{
    (void)url; (void)abort;
    if (buf && cap) buf[0] = '\0';
    return -1;
}

int64_t http_download(const char *url, const char *path, http_progress_fn progress, void *ctx,
                      const volatile int *abort)
{
    (void)url; (void)path; (void)progress; (void)ctx; (void)abort;
    return -1;
}

#endif
