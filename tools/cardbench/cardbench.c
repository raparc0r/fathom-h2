/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/*
 * cardbench (on the H2): what a microSD card does in the H2's slot.
 *
 *   cardbench FILE [MB]        (default 100 MB; FILE is written, then left)
 *
 *   write   MB in 64 KB writes, then fsync: what copying onto the card costs
 *   read    the same back, 64 KB at a time (the file dropped from the page
 *           cache first, by posix_fadvise: only this file, not the player's)
 *   random  2000 reads of 4 KB at random 4 KB-aligned places: what a library
 *           scan and the player's code being read back look like; each read
 *           timed, for the median, the 99th percentile and the worst
 *
 * Copied by hand to fathom/test (not installed); the same binary on each card.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static int cmp(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static void drop(int fd)
{
    fdatasync(fd);
    posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s FILE [MB]\n", argv[0]); return 2; }
    long mb = argc > 2 ? atol(argv[2]) : 100;
    size_t chunk = 64 << 10;
    unsigned char *b = malloc(chunk);
    for (size_t i = 0; i < chunk; i++) b[i] = (unsigned char)(i * 131 + 7);
    int fd = open(argv[1], O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { perror(argv[1]); return 1; }

    double t0 = now();
    for (long i = 0; i < mb * 16; i++) {
        b[0] = (unsigned char)i;                  /* (no two chunks alike) */
        if (write(fd, b, chunk) != (ssize_t)chunk) { perror("write"); return 1; }
    }
    fsync(fd);
    double tw = now() - t0;
    printf("write   %ld MB in %.2f s: %.1f MB/s\n", mb, tw, mb / tw);

    drop(fd);
    lseek(fd, 0, SEEK_SET);
    t0 = now();
    long got = 0;
    ssize_t n;
    while ((n = read(fd, b, chunk)) > 0) got += n;
    double tr = now() - t0;
    printf("read    %ld MB in %.2f s: %.1f MB/s\n", got >> 20, tr, (got >> 20) / tr);

    drop(fd);
    enum { N = 2000 };
    double lat[N];
    long blocks = mb * 256;                       /* 4 KB blocks */
    srand(12345);                                 /* (the same places on every card) */
    t0 = now();
    for (int i = 0; i < N; i++) {
        off_t at = (off_t)(((unsigned)rand() * 2654435761u) % (unsigned)blocks) * 4096;
        double s = now();
        if (pread(fd, b, 4096, at) != 4096) { perror("pread"); return 1; }
        lat[i] = now() - s;
    }
    double ta = now() - t0;
    qsort(lat, N, sizeof *lat, cmp);
    printf("random  %d reads of 4 KB in %.2f s: %.0f reads/s; one read: median %.2f ms, 99th %.2f ms, worst %.2f ms\n",
           N, ta, N / ta, lat[N / 2] * 1e3, lat[N * 99 / 100] * 1e3, lat[N - 1] * 1e3);
    close(fd);
    free(b);
    return 0;
}
