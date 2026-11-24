/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* Downloads (net/http.h) at the same time: the podcasts' worker and the
 * covers' each download in their own thread. Needs FFmpeg: the files come
 * from a small HTTP server of the test's own on 127.0.0.1 (FFmpeg's file:
 * ends a read with EIO under the read timeout http.c sets). */
#include <stdio.h>
#include <stdlib.h>

#include "net/http.h"

/* ---- tries (http_get_retry, http_download_retry): fake single tries ---- */

static int t_fails, t_calls, t_abort_after;
static long t_last;                                  /* what a try returns once it stops failing */
static volatile int t_abort;

static long fake_get(const char *url, char *buf, size_t cap, const volatile int *abort)
{
    (void)url; (void)buf; (void)cap; (void)abort;
    t_calls++;
    if (t_abort_after && t_calls >= t_abort_after) t_abort = 1;
    return t_calls <= t_fails ? -1 : t_last;
}

static int64_t fake_download(const char *url, const char *path, http_progress_fn progress, void *ctx,
                             const volatile int *abort)
{
    (void)url; (void)path; (void)progress; (void)ctx; (void)abort;
    t_calls++;
    if (t_abort_after && t_calls >= t_abort_after) t_abort = 1;
    return t_calls <= t_fails ? -1 : t_last;
}

static int tries_failed;
#define TCHECK(c, ...) do { if (!(c)) { tries_failed++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void t_set(int fails, long last, int abort_after)
{
    t_fails = fails;
    t_last = last;
    t_calls = 0;
    t_abort = 0;
    t_abort_after = abort_after;
}

static int test_tries(void)
{
    char buf[16];
    http_test_tries(fake_get, fake_download, 1, 1);
    t_set(0, 5, 0);
    TCHECK(http_get_retry("u", buf, sizeof buf, &t_abort) == 5 && t_calls == 1, "first try: %d calls", t_calls);
    t_set(1, 5, 0);                                  /* a lookup that failed once */
    TCHECK(http_get_retry("u", buf, sizeof buf, &t_abort) == 5 && t_calls == 2, "second try: %d calls", t_calls);
    t_set(2, 5, 0);
    TCHECK(http_get_retry("u", buf, sizeof buf, NULL) == 5 && t_calls == 3, "third try: %d calls", t_calls);
    t_set(9, 5, 0);                                  /* no network: three tries, then -1 */
    TCHECK(http_get_retry("u", buf, sizeof buf, &t_abort) == -1 && t_calls == 3, "gave up after %d", t_calls);
    t_set(9, 5, 1);                                  /* cancelled during the first: no more */
    TCHECK(http_get_retry("u", buf, sizeof buf, &t_abort) == -1 && t_calls == 1, "after abort: %d calls", t_calls);
    t_set(0, -3, 0);                                 /* not there: the server answered */
    TCHECK(http_get_retry("u", buf, sizeof buf, &t_abort) == -3 && t_calls == 1, "-3 tried %d times", t_calls);
    t_set(0, -2, 0);                                 /* too long: the same next time */
    TCHECK(http_get_retry("u", buf, sizeof buf, &t_abort) == -2 && t_calls == 1, "-2 tried %d times", t_calls);
    t_set(2, 14831595, 0);                           /* the update's download */
    TCHECK(http_download_retry("u", "p", NULL, NULL, &t_abort) == 14831595 && t_calls == 3,
           "download: %d calls", t_calls);
    t_set(9, 7, 2);
    TCHECK(http_download_retry("u", "p", NULL, NULL, &t_abort) == -1 && t_calls == 2,
           "download cancelled: %d calls", t_calls);
    t_set(0, -3, 0);
    TCHECK(http_download_retry("u", "p", NULL, NULL, &t_abort) == -3 && t_calls == 1,
           "download -3 tried %d times", t_calls);
    t_set(0, HTTP_ERR_FULL, 0);                      /* the card nearly full: the same next time */
    TCHECK(http_download_retry("u", "p", NULL, NULL, &t_abort) == HTTP_ERR_FULL && t_calls == 1,
           "card full tried %d times", t_calls);
    t_set(0, HTTP_ERR_WRITE, 0);
    TCHECK(http_download_retry("u", "p", NULL, NULL, &t_abort) == HTTP_ERR_WRITE && t_calls == 1,
           "card not written tried %d times", t_calls);
    t_set(1, HTTP_ERR_FULL, 0);                      /* (after a try the network failed) */
    TCHECK(http_download_retry("u", "p", NULL, NULL, &t_abort) == HTTP_ERR_FULL && t_calls == 2,
           "card full after a failed try: %d calls", t_calls);
    TCHECK(HTTP_ERR_FULL < 0 && HTTP_ERR_WRITE < 0 && HTTP_ERR_FULL != -1 && HTTP_ERR_WRITE != -1 &&
           HTTP_ERR_FULL != -3 && HTTP_ERR_WRITE != -3 && HTTP_ERR_FULL != HTTP_ERR_WRITE, "their own codes");
    http_test_tries(NULL, NULL, 2000, 5000);

    /* what a body read comes to (http_get): an answer cut short isn't a whole one */
    TCHECK(http_get_result(5000, 5000, 0, 0, 0) == 5000, "whole");
    TCHECK(http_get_result(5000, 0, 0, 0, 0) == 5000 && http_get_result(5000, -38, 0, 0, 0) == 5000, "no length given");
    TCHECK(http_get_result(1460, 5000, 0, 1, 0) == -1, "cut short after its first bytes");
    TCHECK(http_get_result(1460, 5000, 0, 0, 0) == -1, "cut short, the server just hanging up");
    TCHECK(http_get_result(5000, 5000, 0, 1, 0) == 5000, "all of it, then an error (an untidy hang-up)");
    TCHECK(http_get_result(0, 5000, 0, 1, 0) == -1 && http_get_result(0, 0, 0, 1, 0) == -1, "nothing");
    TCHECK(http_get_result(0, 0, 0, 0, 0) == 0, "an empty answer");
    TCHECK(http_get_result(4095, 9000, 1, 0, 0) == -2 && http_get_result(4095, 0, 1, 0, 0) == -2, "longer than the buffer");
    TCHECK(http_get_result(5000, 5000, 0, 0, 1) == -1, "cancelled");
    TCHECK(http_get_result(1460, 0, 0, 1, 0) == 1460, "no length and an error after some: as before");
    return tries_failed;
}

#ifndef FATHOM_HAVE_FFMPEG
int main(void)
{
    if (test_tries()) { printf("%d failed\n", tries_failed); return 1; }
    printf("http: tries ok; downloads skipped (no FFmpeg in this build)\n");
    return 0;
}
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define SIZE (3 * 1024 * 1024 + 123)

/* the files' contents: each byte from its own seed, so a mixed-up chunk shows */
static unsigned char byte_at(int seed, long i) { return (unsigned char)((i * 31 + seed * 7 + (i >> 13)) ^ seed); }

/* One request: GET /0 or /1, the file with its length, in pieces */
static void *serve(void *arg)
{
    int c = (int)(long)arg;
    char req[1024];
    size_t n = 0;
    while (n < sizeof req - 1) {
        ssize_t k = read(c, req + n, sizeof req - 1 - n);
        if (k <= 0) break;
        n += (size_t)k;
        req[n] = '\0';
        if (strstr(req, "\r\n\r\n")) break;
    }
    req[n] = '\0';
    int which = strstr(req, "GET /1") ? 1 : 0;
    char head[160];
    int hl = snprintf(head, sizeof head, "HTTP/1.1 200 OK\r\nContent-Length: %d\r\nConnection: close\r\n\r\n", SIZE);
    if (write(c, head, (size_t)hl) != hl) { close(c); return NULL; }
    unsigned char piece[7000];                       /* (pieces that don't line up with the reader's) */
    for (long at = 0; at < SIZE;) {
        long k = SIZE - at < (long)sizeof piece ? SIZE - at : (long)sizeof piece;
        for (long i = 0; i < k; i++) piece[i] = byte_at(which + 1, at + i);
        for (long o = 0; o < k;) {
            ssize_t w = write(c, piece + o, (size_t)(k - o));
            if (w <= 0) { close(c); return NULL; }
            o += w;
        }
        at += k;
    }
    close(c);
    return NULL;
}

static void *server(void *arg)
{
    int s = (int)(long)arg;
    for (;;) {
        int c = accept(s, NULL, NULL);
        if (c < 0) return NULL;
        pthread_t t;
        if (pthread_create(&t, NULL, serve, (void *)(long)c) == 0) pthread_detach(t);
        else close(c);
    }
}

typedef struct { char url[100], out[512]; int64_t got; } job_t;

static void *run(void *arg)
{
    job_t *j = arg;
    j->got = http_download(j->url, j->out, NULL, NULL, NULL);
    return NULL;
}

/* ---- a radio stream (audio/decoder.h) and a server that isn't there ----
 * A connection that fails must fail when it does: the player has its own
 * tries, a minute of them (player.c), and FFmpeg trying each one again by
 * itself (reconnect_on_network_error: 11 s for a refused connection, and
 * nested in every reconnect of a stream that dropped) made a dead station
 * take minutes to be given up. A stream whose connection closes is still
 * picked up again (reconnect, reconnect_streamed). */
#include <time.h>
#include "audio/decoder.h"

static volatile int rs_conns, rs_gone;

static double mono_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

/* A WAV with no length (44.1 kHz, 16 bits, stereo, silent): the first
 * connection gets a second of it and is closed; the ones after go on until
 * rs_gone. */
static void *radio_serve(void *arg)
{
    int c = (int)((long)arg & 0xffff), nth = (int)((long)arg >> 16);
    char req[1024];
    size_t n = 0;
    while (n < sizeof req - 1) {
        ssize_t k = read(c, req + n, sizeof req - 1 - n);
        if (k <= 0) break;
        n += (size_t)k;
        req[n] = '\0';
        if (strstr(req, "\r\n\r\n")) break;
    }
    static const unsigned char wav[44] = {
        'R', 'I', 'F', 'F', 0xff, 0xff, 0xff, 0x7f, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0,
        1, 0, 2, 0, 0x44, 0xac, 0, 0, 0x10, 0xb1, 2, 0, 4, 0, 16, 0, 'd', 'a', 't', 'a', 0, 0, 0, 0};
    static const char head[] = "HTTP/1.0 200 OK\r\nContent-Type: audio/wav\r\n\r\n";
    static const unsigned char quiet[4096];
    long sent = 0;
    if (write(c, head, sizeof head - 1) == (ssize_t)sizeof head - 1 && write(c, wav, sizeof wav) == (ssize_t)sizeof wav)
        while (!rs_gone && !(nth == 1 && sent >= 44100 * 4)) {
            ssize_t w = write(c, quiet, sizeof quiet);
            if (w <= 0) break;
            sent += w;
        }
    close(c);
    return NULL;
}

static void *radio_server(void *arg)
{
    int s = (int)(long)arg;
    for (;;) {
        int c = accept(s, NULL, NULL);
        if (c < 0) return NULL;
        long a = (long)c | ((long)++rs_conns << 16);
        pthread_t t;
        if (pthread_create(&t, NULL, radio_serve, (void *)a) == 0) pthread_detach(t);
        else close(c);
    }
}

static int local_listener(int *port)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t al = sizeof a;
    if (s < 0 || bind(s, (struct sockaddr *)&a, sizeof a) || listen(s, 8) || getsockname(s, (struct sockaddr *)&a, &al)) return -1;
    *port = ntohs(a.sin_port);
    return s;
}

static void test_stream(void)
{
    char url[100];
    audio_format_t f;
    volatile int stop = 0;
    static int32_t pcm[4096 * 2];
    /* nothing listening: it fails at once (it took 11 s) */
    int port = 0, s = local_listener(&port);
    CHECK(s >= 0, "a listener");
    close(s);
    snprintf(url, sizeof url, "http://127.0.0.1:%d/radio.wav", port);
    double t0 = mono_s();
    decoder_t *d = decoder_open_ex(url, &f, &stop);
    double took = mono_s() - t0;
    CHECK(!d && took < 3, "a refused stream: %s after %.1f s", d ? "opened" : "failed", took);
    if (d) decoder_close(d);

    /* a stream whose connection is closed after a second: gone on with, on a
     * new connection */
    s = local_listener(&port);
    CHECK(s >= 0, "a listener");
    pthread_t st;
    pthread_create(&st, NULL, radio_server, (void *)(long)s);
    snprintf(url, sizeof url, "http://127.0.0.1:%d/radio.wav", port);
    d = decoder_open_ex(url, &f, &stop);
    CHECK(d && f.rate == 44100 && f.channels == 2, "the stream opened (%u Hz, %u channels)", d ? f.rate : 0, d ? f.channels : 0);
    if (!d) return;
    long frames = 0, r = 1;
    t0 = mono_s();
    while (frames < 3 * 44100 && r > 0 && mono_s() - t0 < 60) {
        r = decoder_read(d, pcm, 4096);
        if (r > 0) frames += r;
    }
    CHECK(frames >= 3 * 44100 && rs_conns >= 2, "after its connection closed: %ld frames over %d connections", frames, rs_conns);

    /* its server gone for good: the stream ends once FFmpeg's own reconnects
     * are used up (11 s of waits; over a minute when each of them tried five
     * times itself), for the player to take over */
    rs_gone = 1;
    shutdown(s, SHUT_RDWR);
    close(s);
    pthread_join(st, NULL);
    t0 = mono_s();
    while ((r = decoder_read(d, pcm, 4096)) > 0 && mono_s() - t0 < 180) ;
    took = mono_s() - t0;
    CHECK(r <= 0 && took < 30, "its server gone: the stream %s after %.1f s", r <= 0 ? "ended" : "still going", took);
    printf("http: a refused stream fails at once; a dropped one reconnects; a dead one ends in %.1f s\n", took);
    decoder_close(d);
}

#include "tmpdir.h"

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    char dir[512];
    test_tmp(dir, sizeof dir, "http");
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; }
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t al = sizeof a;
    if (ls < 0 || bind(ls, (struct sockaddr *)&a, sizeof a) || listen(ls, 8) || getsockname(ls, (struct sockaddr *)&a, &al)) {
        perror("server");
        return 1;
    }
    pthread_t st;
    pthread_create(&st, NULL, server, (void *)(long)ls);
    pthread_detach(st);
    unsigned char b[8192];   /* (streamed: the H2 has ~9 MB free) */
    job_t j[2];
    for (int round = 0; round < 4; round++) {
        pthread_t t[2];
        for (int s = 0; s < 2; s++) {
            snprintf(j[s].url, sizeof j[s].url, "http://127.0.0.1:%d/%d", ntohs(a.sin_port), s);
            snprintf(j[s].out, sizeof j[s].out, "%s/out%d", dir, s);
            pthread_create(&t[s], NULL, run, &j[s]);
        }
        for (int s = 0; s < 2; s++) pthread_join(t[s], NULL);
        for (int s = 0; s < 2; s++) {
            CHECK(j[s].got == SIZE, "round %d: download %d: %lld bytes", round, s, (long long)j[s].got);
            FILE *f = fopen(j[s].out, "rb");
            long n = 0, bad = -1;
            size_t k;
            while (f && (k = fread(b, 1, sizeof b, f)) > 0) {
                for (size_t i = 0; i < k && bad < 0; i++) if (b[i] != byte_at(s + 1, n + (long)i)) bad = n + (long)i;
                n += (long)k;
            }
            if (f) fclose(f);
            CHECK(n == SIZE && bad < 0, "round %d: download %d mixed up from byte %ld", round, s, bad);
            remove(j[s].out);
        }
    }
    close(ls);
    rmdir(dir);
    test_stream();
    fails += test_tries();
    if (fails) { printf("%d failed\n", fails); return 1; }
    printf("http: ok\n");
    return 0;
}
#endif
