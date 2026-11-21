/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Downloads (net/http.h) at the same time: the podcasts' worker and the
 * covers' each download in their own thread. Needs FFmpeg: the files come
 * from a small HTTP server of the test's own on 127.0.0.1 (FFmpeg's file:
 * ends a read with EIO under the read timeout http.c sets). */
#include <stdio.h>
#include <stdlib.h>

#include "net/http.h"

#ifndef FATHOM_HAVE_FFMPEG
int main(void)
{
    printf("http: skipped (no FFmpeg in this build)\n");
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
    if (fails) { printf("%d failed\n", fails); return 1; }
    printf("http: ok\n");
    return 0;
}
#endif
