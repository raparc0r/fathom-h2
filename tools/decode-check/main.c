/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/*
 * decode-check: a file or a stream opened and decoded as the player does,
 * with no sound made. For checking on the H2 what a change to the decoder or
 * to FFmpeg does to a radio station or a podcast episode without playing it.
 *
 *   fathom-decode-check PATH_OR_URL [SECONDS]      (default 20)
 *   DC_VERBOSE=1: FFmpeg's own messages too
 *
 * 1. what FFmpeg makes of it: the container, its streams (a cover's size),
 *    its chapters (as podcast.c's pod_file_chapters reads them)
 * 2. the player's decoder on it for SECONDS of sound: the format, how much
 *    was decoded and how long that took, each title the stream announces,
 *    and the most memory this took
 * Not installed; copied to the card by hand.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>

#include "audio/decoder.h"
#include "audio/ff_util.h"

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

static long peak_kb(void)
{
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    return ru.ru_maxrss;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s PATH_OR_URL [SECONDS]\n", argv[0]); return 2; }
    const char *path = argv[1];
    int secs = argc > 2 ? atoi(argv[2]) : 20;
    int net = decoder_is_stream(path);
    setvbuf(stdout, NULL, _IOLBF, 0);

    if (!getenv("DC_VERBOSE")) ff_quiet();             /* (DC_VERBOSE=1: FFmpeg's own messages, to see why something fails) */
    ff_set_ca_file("/mnt/sd/fathom/share/ca-certificates.crt");   /* (certificates checked, as the player's are) */
    ff_mem_guard();
    AVFormatContext *fc = NULL;
    AVDictionary *opt = NULL;
    if (net) {
        ff_tls_options(&opt);
        av_dict_set(&opt, "rw_timeout", "8000000", 0);
        av_dict_set(&opt, "user_agent", "Fathom (HiFi Walker H2)", 0);
        av_dict_set(&opt, "icy", "1", 0);
    }
    double t0 = now_s();
    int r = avformat_open_input(&fc, path, NULL, &opt);
    av_dict_free(&opt);
    if (r < 0) {
        char e[128];
        av_strerror(r, e, sizeof e);
        printf("container: can't open: %s\n", e);
    } else {
        printf("container: %s, %u stream(s), %u chapter(s), opened in %.1f s, peak %ld kB\n",
               fc->iformat ? fc->iformat->name : "?", fc->nb_streams, fc->nb_chapters, now_s() - t0, peak_kb());
        for (unsigned i = 0; i < fc->nb_streams; i++) {
            const AVStream *s = fc->streams[i];
            printf("  stream %u: %s %s%s", i, av_get_media_type_string(s->codecpar->codec_type),
                   avcodec_get_name(s->codecpar->codec_id), s->disposition & AV_DISPOSITION_ATTACHED_PIC ? ", a picture" : "");
            if (s->disposition & AV_DISPOSITION_ATTACHED_PIC) printf(" of %d bytes", s->attached_pic.size);
            printf("\n");
        }
        for (unsigned i = 0; i < fc->nb_chapters && i < 5; i++) {
            const AVChapter *c = fc->chapters[i];
            const AVDictionaryEntry *t = av_dict_get(c->metadata, "title", NULL, 0);
            printf("  chapter %u at %lld ms: %s\n", i + 1,
                   (long long)av_rescale_q(c->start, c->time_base, (AVRational){1, 1000}), t ? t->value : "");
        }
        static const char *const keys[] = {"title", "artist", "album", "icy-name", "icy-genre", "StreamTitle"};
        for (size_t k = 0; k < sizeof keys / sizeof keys[0]; k++) {
            const AVDictionaryEntry *e = av_dict_get(fc->metadata, keys[k], NULL, 0);
            for (unsigned i = 0; !e && i < fc->nb_streams; i++) e = av_dict_get(fc->streams[i]->metadata, keys[k], NULL, 0);
            if (e) printf("  %s: %.100s\n", keys[k], e->value);
        }
        avformat_close_input(&fc);
    }

    audio_format_t fmt;
    t0 = now_s();
    decoder_t *d = decoder_open(path, &fmt);
    if (!d) { printf("decoder: can't open\n"); return 1; }
    printf("decoder: %s, %u Hz, %u channel(s), %u bits, opened in %.1f s\n", fmt.codec, (unsigned)fmt.rate,
           (unsigned)fmt.channels, (unsigned)fmt.bits, now_s() - t0);
    static int32_t buf[4096 * 2];
    long long frames = 0, want = (long long)secs * fmt.rate;
    long peak = 0;
    unsigned seen = 0;
    double t1 = now_s();
    while (frames < want && now_s() - t1 < secs + 30) {
        long n = decoder_read(d, buf, 4096);
        if (n <= 0) { printf("decoder: read ended (%ld) after %.1f s of sound\n", n, (double)frames / fmt.rate); break; }
        for (long i = 0; i < n * fmt.channels; i++) {
            long a = buf[i] < 0 ? -(long)(buf[i] / 256) : buf[i] / 256;
            if (a > peak) peak = a;
        }
        frames += n;
        char title[256] = "", url[256] = "";
        unsigned m = decoder_meta(d, title, sizeof title, url, sizeof url);
        if (m != seen) {
            seen = m;
            printf("  title at %.0f s: %.120s\n", (double)frames / fmt.rate, title);
        }
    }
    printf("decoder: %.1f s of sound in %.1f s, loudest sample %ld of 8388607, peak memory %ld kB\n",
           (double)frames / fmt.rate, now_s() - t1, peak, peak_kb());
    decoder_close(d);
    return frames > 0 ? 0 : 1;
}
