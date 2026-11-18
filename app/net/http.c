/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Fetching over HTTP(S) (http.h): FFmpeg's avio, as the radio uses it. */
#include "net/http.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef FATHOM_HAVE_FFMPEG
#include <libavformat/avio.h>
#include <libavutil/dict.h>

#include "audio/ff_util.h"

#define HTTP_TIMEOUT_US "15000000"
#define HTTP_UA "Fathom (HiFi Walker H2)"

static int interrupted(void *ctx)
{
    const volatile int *abort = ctx;
    return abort && *abort;
}

static AVIOContext *open_url(const char *url, const volatile int *abort)
{
    AVIOContext *pb = NULL;
    AVIOInterruptCB icb = {interrupted, (void *)abort};
    AVDictionary *opt = NULL;
    av_dict_set(&opt, "rw_timeout", HTTP_TIMEOUT_US, 0);
    av_dict_set(&opt, "user_agent", HTTP_UA, 0);
    av_dict_set(&opt, "follow", "1", 0);          /* (FFmpeg follows redirects by default) */
    ff_tls_options(&opt);
    int r = avio_open2(&pb, url, AVIO_FLAG_READ, &icb, &opt);
    av_dict_free(&opt);
    return r < 0 ? NULL : pb;
}

long http_get(const char *url, char *buf, size_t cap, const volatile int *abort)
{
    if (!buf || cap < 1) return -1;
    buf[0] = '\0';
    AVIOContext *pb = open_url(url, abort);
    if (!pb) return -1;
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

int64_t http_download(const char *url, const char *path, http_progress_fn progress, void *ctx,
                      const volatile int *abort)
{
    AVIOContext *pb = open_url(url, abort);
    if (!pb) return -1;
    char part[1024];
    snprintf(part, sizeof part, "%s.part", path);
    FILE *f = fopen(part, "wb");
    if (!f) { avio_closep(&pb); return -1; }
    int64_t total = avio_size(pb);
    uint64_t done = 0;
    static unsigned char chunk[32768];
    int ok = 1;
    for (;;) {
        int k = avio_read(pb, chunk, sizeof chunk);
        if (k == AVERROR_EOF || k == 0) break;
        if (k < 0 || fwrite(chunk, 1, (size_t)k, f) != (size_t)k || (abort && *abort)) { ok = 0; break; }
        done += (uint64_t)k;
        /* Each MB to the card and out of the page cache: a 16 MB download
         * would otherwise push out the player's own code (run from the card,
         * in 24 MB of RAM) and leave it all to write at once. */
        if ((done & 0xFFFFF) < (uint64_t)k && (fflush(f) || fdatasync(fileno(f)))) { ok = 0; break; }
#ifdef POSIX_FADV_DONTNEED
        if ((done & 0xFFFFF) < (uint64_t)k) posix_fadvise(fileno(f), 0, 0, POSIX_FADV_DONTNEED);
#endif
        if (progress) progress(done, total > 0 ? (uint64_t)total : 0, ctx);
    }
    avio_closep(&pb);
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
