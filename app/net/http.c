/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* Fetching over HTTP(S) (http.h): FFmpeg's avio, as the radio uses it. */
#include "net/http.h"
#include "net/netclock.h"
#include "core/fsafe.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

/* Who's asking: Fathom, its version and where it's from (MusicBrainz asks
 * apps to say so, and is stricter with those that don't) */
static char g_ua[160] = "Fathom (HiFi Walker H2; https://github.com/raparc0r/fathom-h2)";

void http_set_version(const char *version)
{
    snprintf(g_ua, sizeof g_ua, "Fathom/%s (HiFi Walker H2; https://github.com/raparc0r/fathom-h2)",
             version && *version ? version : "dev");
}

long http_get_result(size_t n, int64_t total, int cut, int failed, int aborted)
{
    if (aborted) return -1;
    if (failed && !n) return -1;
    if (cut) return -2;
    /* fewer bytes than the server said it had: the connection went partway.
     * (An error with all of it here is a server that hangs up untidily.) */
    if (total > 0 && (int64_t)n < total) return -1;
    return (long)n;
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
 * as opposed to not reaching it (a name that didn't resolve, a timeout).
 * file: a download to the card, which FFmpeg takes up again by itself when
 * its connection goes partway. */
static AVIOContext *open_url(const char *url, const volatile int *abort, int *gone, int file)
{
    AVIOContext *pb = NULL;
    AVIOInterruptCB icb = {interrupted, (void *)abort};
    AVDictionary *opt = NULL;
    av_dict_set(&opt, "rw_timeout", HTTP_TIMEOUT_US, 0);
    av_dict_set(&opt, "user_agent", g_ua, 0);
    if (file) {
        /* Wi-Fi gone for a moment 30 MB into an episode: FFmpeg connects
         * again and asks for the rest (a Range request from where it was),
         * waiting 0, 1, 3, 7 and 15 s between its tries, where the whole
         * file used to be started again. Only for a server that says how
         * long the file is and takes ranges; with any other the read fails
         * as before (not reconnect_streamed: that starts a live stream again
         * from its start, which here would be the file's beginning written
         * after its middle). A connection that can't be opened is tried
         * again by FFmpeg in the same way (reconnect_on_network_error),
         * before this try is given up. */
        av_dict_set(&opt, "reconnect", "1", 0);
        av_dict_set(&opt, "reconnect_on_network_error", "1", 0);
        av_dict_set(&opt, "reconnect_delay_max", "30", 0);
    }
    /* (FFmpeg follows redirects by itself.) Only the network: a podcast
     * feed's enclosure or image is a URL from anyone, and file:, a device
     * path or a bare tcp: to a LAN host would be opened as readily */
    av_dict_set(&opt, "protocol_whitelist", "http,https,tls,tcp,httpproxy", 0);
    ff_tls_options(&opt);
    int r = avio_open2(&pb, url, AVIO_FLAG_READ, &icb, &opt);
    av_dict_free(&opt);
    if (r < 0 && !(abort && *abort) && netclock_net_failed) netclock_net_failed(url);   /* (a clock days behind may be why: the time is asked for) */
    if (gone) *gone = r == AVERROR_HTTP_BAD_REQUEST || r == AVERROR_HTTP_UNAUTHORIZED || r == AVERROR_HTTP_FORBIDDEN ||
                      r == AVERROR_HTTP_NOT_FOUND || r == AVERROR_HTTP_OTHER_4XX;
    return r < 0 ? NULL : pb;
}

long http_get(const char *url, char *buf, size_t cap, const volatile int *abort)
{
    if (!buf || cap < 1) return -1;
    buf[0] = '\0';
    int gone = 0;
    AVIOContext *pb = open_url(url, abort, &gone, 0);
    if (!pb) return gone ? -3 : -1;
    int64_t total = avio_size(pb);                   /* (its Content-Length; negative when it gave none) */
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
    return http_get_result(n, total, cut, failed, abort && *abort);
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
    AVIOContext *pb = open_url(url, abort, &gone, 1);
    if (!pb) return gone ? -3 : -1;
    char part[1024];
    snprintf(part, sizeof part, "%s.part", path);
    /* (each call its own: the podcasts' and the covers' workers download at once) */
    enum { CHUNK = 32768 };
    unsigned char *chunk = malloc(CHUNK);
    if (!chunk) { avio_closep(&pb); return -1; }
    FILE *f = fopen(part, "wbe");
    if (!f) {
        fprintf(stderr, "http: download not started: %s can't be written\n", part);
        free(chunk);
        avio_closep(&pb);
        return HTTP_ERR_WRITE;
    }
    int64_t total = avio_size(pb);
    uint64_t done = 0;
    int ok = 1, card = 0;                            /* card: what failed was the card (HTTP_ERR_*), not the network */
    for (;;) {
        int k = avio_read(pb, chunk, CHUNK);
        if (k == AVERROR_EOF || k == 0) break;
        if (k < 0 || (abort && *abort)) { ok = 0; break; }
        if (fwrite(chunk, 1, (size_t)k, f) != (size_t)k) { ok = 0; card = HTTP_ERR_WRITE; break; }
        done += (uint64_t)k;
        /* Each MB to the card and out of the page cache: a 16 MB download
         * would otherwise push out the player's own code (run from the card,
         * in 24 MB of RAM) and leave it all to write at once. */
        if ((done & 0xFFFFF) < (uint64_t)k) {
            if (fflush(f) || fdatasync(fileno(f))) card = HTTP_ERR_WRITE;
            else if (card_nearly_full(f)) card = HTTP_ERR_FULL;
            if (card) {
                fprintf(stderr, "http: download stopped: %s\n",
                        card == HTTP_ERR_FULL ? "the card is nearly full" : "the card can't be written");
                ok = 0;
                break;
            }
        }
#ifdef POSIX_FADV_DONTNEED
        if ((done & 0xFFFFF) < (uint64_t)k) posix_fadvise(fileno(f), 0, 0, POSIX_FADV_DONTNEED);
#endif
        if (progress) progress(done, total > 0 ? (uint64_t)total : 0, ctx);
    }
    avio_closep(&pb);
    free(chunk);
    if (fclose(f) && ok) { ok = 0; card = HTTP_ERR_WRITE; }
    if (ok && total > 0 && done != (uint64_t)total) ok = 0;   /* cut short */
    if (ok && file_replace(part, path)) { ok = 0; card = HTTP_ERR_WRITE; }
    if (!ok) { remove(part); return card ? card : -1; }
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

/* ---- tries (http.h): the same for both builds ---- */

static http_get_fn g_get_once;
static http_download_fn g_download_once;
static int g_wait_ms[2] = {2000, 5000};

void http_test_tries(http_get_fn get, http_download_fn download, int wait1_ms, int wait2_ms)
{
    g_get_once = get;
    g_download_once = download;
    g_wait_ms[0] = wait1_ms;
    g_wait_ms[1] = wait2_ms;
}

/* 1 if *abort was set (now or while waiting) */
static int wait_or_abort(int ms, const volatile int *abort)
{
    for (int t = 0; t < ms && !(abort && *abort); t += 50) {
        struct timespec ts = {0, 50 * 1000 * 1000L};
        nanosleep(&ts, NULL);
    }
    return abort && *abort;
}

long http_get_retry(const char *url, char *buf, size_t cap, const volatile int *abort)
{
    long n = -1;
    for (int i = 0; i < 3; i++) {
        n = (g_get_once ? g_get_once : http_get)(url, buf, cap, abort);
        if (n != -1 || i == 2 || wait_or_abort(g_wait_ms[i], abort)) break;
        fprintf(stderr, "http: %s: trying again\n", url);
    }
    return n;
}

int64_t http_download_retry(const char *url, const char *path, http_progress_fn progress, void *ctx,
                            const volatile int *abort)
{
    int64_t n = -1;
    for (int i = 0; i < 3; i++) {
        n = (g_download_once ? g_download_once : http_download)(url, path, progress, ctx, abort);
        if (n != -1 || i == 2 || wait_or_abort(g_wait_ms[i], abort)) break;
        fprintf(stderr, "http: %s: trying again\n", url);
    }
    return n;
}
