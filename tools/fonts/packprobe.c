/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/*
 * What the plan for Japanese names assumes, measured on
 * the H2:
 *
 *   packprobe pages FILE      a file on the card, mapped as the pack would be: what a
 *                             page not in memory costs to touch (one page asked for,
 *                             and with the kernel's read-around), what asking "is it
 *                             in memory" costs, and how long a page asked for ahead
 *                             takes to come
 *   packprobe drop FILE       FILE's pages put out of memory (for a cold start);
 *   packprobe held FILE       how many of them are in memory
 *   packprobe outline FONT PX [FIRST LAST]
 *                             what filling a character from its outline costs, with
 *                             the outline code the player uses (LVGL's tiny_ttf)
 *
 * Built by hand, as the player is (soft float):
 *   mipsel-buildroot-linux-musl-gcc -Os -static -I LVGL/src/libs/tiny_ttf \
 *       -o packprobe tools/fonts/packprobe.c -lm
 * It only reads FILE and FONT. FILE's pages are dropped from memory first
 * (this file's alone: posix_fadvise, not the system's drop_caches).
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype_htcw.h"

static long us_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)(ts.tv_sec * 1000000L + ts.tv_nsec / 1000);
}

static int cmp_long(const void *a, const void *b)
{
    long x = *(const long *)a, y = *(const long *)b;
    return x < y ? -1 : x > y;
}

static void say(const char *what, long *t, int n)
{
    long sum = 0;
    qsort(t, (size_t)n, sizeof *t, cmp_long);
    for (int i = 0; i < n; i++) sum += t[i];
    printf("%-46s n %3d  median %6ld us  mean %6ld  90%% %6ld  max %6ld\n", what, n, t[n / 2], sum / n, t[n * 9 / 10], t[n - 1]);
}

static int resident(const unsigned char *map, size_t len, long page)
{
    size_t pages = (len + (size_t)page - 1) / (size_t)page;
    unsigned char *vec = malloc(pages);
    int n = 0;
    if (vec && mincore((void *)map, len, (void *)vec) == 0)
        for (size_t i = 0; i < pages; i++) n += vec[i] & 1;
    free(vec);
    return n;
}

/* The file's pages out of memory, and a new mapping of it */
static unsigned char *fresh(int fd, size_t len, int random)
{
    fsync(fd);
#ifdef POSIX_FADV_DONTNEED                       /* (Linux: the H2. Elsewhere the pages stay) */
    posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
#endif
    unsigned char *m = mmap(NULL, len, PROT_READ, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) { perror("mmap"); exit(1); }
    if (random) madvise(m, len, MADV_RANDOM);
    return m;
}

static int pages_cmd(const char *path)
{
    int fd = open(path, O_RDONLY);
    struct stat st;
    if (fd < 0 || fstat(fd, &st)) { perror(path); return 1; }
    size_t len = (size_t)st.st_size;
    long page = sysconf(_SC_PAGESIZE);
    long npages = (long)(len / (size_t)page);
    if (npages < 256) { fprintf(stderr, "%s: under 256 pages\n", path); return 1; }
    printf("%s: %ld pages of %ld bytes\n", path, npages, page);
    enum { N = 64 };
    long t[N], pick[N];
    volatile unsigned sink = 0;
    srand(12345);

    /* 1. one page asked for at a time */
    unsigned char *m = fresh(fd, len, 1);
    printf("in memory after dropping: %d pages\n", resident(m, len, page));
    for (int i = 0; i < N; i++) {
        pick[i] = (long)((unsigned)rand() % (unsigned)npages);
        long t0 = us_now();
        sink += m[pick[i] * page];
        t[i] = us_now() - t0;
    }
    int got = resident(m, len, page);
    say("a page not in memory (asked for alone)", t, N);
    printf("  in memory afterwards: %d pages\n", got);
    for (int i = 0; i < N; i++) {
        long t0 = us_now();
        sink += m[pick[i] * page + 7];
        t[i] = us_now() - t0;
    }
    say("the same pages again (in memory)", t, N);
    munmap(m, len);

    /* 2. the kernel's own read-around */
    m = fresh(fd, len, 0);
    for (int i = 0; i < 16; i++) {
        long p = (long)((unsigned)rand() % (unsigned)npages), t0 = us_now();
        sink += m[p * page];
        t[i] = us_now() - t0;
    }
    got = resident(m, len, page);
    say("a page not in memory (with read-around)", t, 16);
    printf("  in memory afterwards: %d pages for 16 touched\n", got);
    munmap(m, len);

    /* 3. asking whether a page is in memory */
    m = fresh(fd, len, 1);
    unsigned char v;
    long t0 = us_now();
    for (int i = 0; i < 20000; i++) mincore(m + (size_t)((i % npages) * page), (size_t)page, (void *)&v);
    printf("%-46s %ld ns a call\n", "asking \"is this page in memory\"", (us_now() - t0) * 1000 / 20000);

    /* 4. a page asked for ahead: the call, and how long until it is there */
    long call[N];
    for (int i = 0; i < N; i++) {
        unsigned char *p = m + (size_t)(((unsigned)rand() % (unsigned)npages) * page);
        long a = us_now();
        madvise(p, (size_t)page, MADV_WILLNEED);
        long b = us_now();
        call[i] = b - a;
        for (;;) {
            mincore(p, (size_t)page, (void *)&v);
            if (v & 1) break;
            if (us_now() - b > 2000000) break;
            usleep(100);
        }
        t[i] = us_now() - b;
    }
    say("asking for a page ahead: the call itself", call, N);
    say("...and until the page is there", t, N);

    /* 5. a screenful asked for ahead at once (90 pages), until all are there */
    munmap(m, len);
    m = fresh(fd, len, 1);
    enum { SCREEN = 90 };
    unsigned char *want[SCREEN];
    long a = us_now();
    for (int i = 0; i < SCREEN; i++) {
        want[i] = m + (size_t)(((unsigned)rand() % (unsigned)npages) * page);
        madvise(want[i], (size_t)page, MADV_WILLNEED);
    }
    long asked = us_now() - a;
    for (;;) {
        int left = 0;
        for (int i = 0; i < SCREEN; i++) { mincore(want[i], (size_t)page, (void *)&v); left += !(v & 1); }
        if (!left || us_now() - a > 5000000) break;
        usleep(500);
    }
    printf("%-46s asked in %ld us, all there after %ld us\n", "90 pages asked for ahead at once", asked, us_now() - a);
    munmap(m, len);
    close(fd);
    return (int)(sink & 0);
}

static int outline_cmd(const char *path, int px, long first, long last)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return 1; }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    rewind(f);
    unsigned char *data = malloc((size_t)len);
    if (!data || fread(data, 1, (size_t)len, f) != (size_t)len) { fprintf(stderr, "cannot read %s\n", path); return 1; }
    fclose(f);
    stbtt_fontinfo info;
    if (!stbtt_InitFont(&info, data, stbtt_GetFontOffsetForIndex(data, 0))) { fprintf(stderr, "%s: not a font this code reads\n", path); return 1; }
    float scale = stbtt_ScaleForPixelHeight(&info, (float)px);
    enum { MAX = 4000 };
    static long t[MAX];
    static unsigned char bmp[96 * 96];
    int n = 0;
    long points = 0;
    for (long cp = first; cp <= last && n < MAX; cp++) {
        int g = stbtt_FindGlyphIndex(&info, (int)cp);
        if (!g) continue;
        int x0, y0, x1, y1;
        long t0 = us_now();
        stbtt_GetGlyphBitmapBox(&info, g, scale, scale, &x0, &y0, &x1, &y1);
        int w = x1 - x0, h = y1 - y0;
        if (w <= 0 || h <= 0 || w > 96 || h > 96) continue;
        stbtt_MakeGlyphBitmap(&info, bmp, w, h, w, scale, scale, g);
        t[n++] = us_now() - t0;
        stbtt_vertex *v = NULL;
        int nv = stbtt_GetGlyphShape(&info, g, &v);
        points += nv;
        STBTT_free(v, info.userdata);
    }
    if (!n) { fprintf(stderr, "no character of U+%04lX to U+%04lX in %s\n", first, last, path); return 1; }
    char what[96];
    snprintf(what, sizeof what, "%d px, U+%04lX-%04lX (%ld points each)", px, first, last, points / n);
    say(what, t, n);
    return 0;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc == 3 && !strcmp(argv[1], "pages")) return pages_cmd(argv[2]);
    if (argc == 3 && (!strcmp(argv[1], "drop") || !strcmp(argv[1], "held"))) {
        /* drop: FILE's pages out of memory (those no program has mapped in);
         * held: how many of its pages are in memory, nothing changed */
        int fd = open(argv[2], O_RDONLY);
        struct stat st;
        if (fd < 0 || fstat(fd, &st)) { perror(argv[2]); return 1; }
        unsigned char *m;
        if (!strcmp(argv[1], "drop")) m = fresh(fd, (size_t)st.st_size, 1);
        else if ((m = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_SHARED, fd, 0)) == MAP_FAILED) { perror("mmap"); return 1; }
        long page = sysconf(_SC_PAGESIZE);
        printf("%s: %d of %ld pages in memory\n", argv[2], resident(m, (size_t)st.st_size, page), (long)(((size_t)st.st_size + (size_t)page - 1) / (size_t)page));
        return 0;
    }
    if ((argc == 4 || argc == 6) && !strcmp(argv[1], "outline"))
        return outline_cmd(argv[2], atoi(argv[3]), argc == 6 ? strtol(argv[4], NULL, 16) : 0x21, argc == 6 ? strtol(argv[5], NULL, 16) : 0x24F);
    fprintf(stderr, "usage: packprobe pages FILE | drop FILE | held FILE | outline FONT PX [FIRST LAST (hex)]\n");
    return 2;
}
