/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE                 /* (musl: mincore, madvise, usleep) */
#endif
#include "ui/glyphpack.h"
#include "core/clock.h"
#include "core/bytes.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define PAGE 4096u
#define HEADER 24u
#define MAX_COUNT 32767u            /* (a number is 15 bits) */
#define MAX_FILE (64u << 20)
#define QUEUE 512u                  /* pages waiting for the thread: a screenful is 90 */

struct glyphpack {
    const uint8_t *map;
    size_t len;
    uint32_t count, index_off;
    int nsizes;
    struct { int px, row, foot; uint32_t cell, off; } z[GLYPHPACK_MAX_SIZES];

    /* never waiting (the header): the memory's pages (not the file's 4096: the
     * system's); which of them were in memory when the kernel was last asked
     * (seen: the drawing thread's alone), which the pack's thread has fetched
     * since (fetched), and which are with it (asked) */
    size_t psz, npages;
    unsigned char *seen;
    atomic_uchar *fetched, *asked;
    long seen_at_ms;
    int warmed[GLYPHPACK_MAX_SIZES];
    pthread_t thread;
    int thread_on, stop;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    uint32_t queue[QUEUE];
    unsigned qhead, qn;
    atomic_uint arrivals;
    atomic_int pending;
    int test_forget, test_delay_ms;
};

#define SEEN_FOR_MS 1000            /* how long the kernel's answer is taken for true */
#define WARM_CELLS 600              /* kana and symbols: brought in at a size's first use */


static glyphpack_t *refuse(glyphpack_t *p, const char *path, const char *why)
{
    fprintf(stderr, "glyphpack: %s: %s\n", path, why);
    glyphpack_close(p);
    return NULL;
}

glyphpack_t *glyphpack_open(const char *path)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (errno != ENOENT && errno != ENOTDIR) fprintf(stderr, "glyphpack: %s: %s\n", path, strerror(errno));
        return NULL;
    }
    glyphpack_t *p = calloc(1, sizeof *p);
    struct stat st;
    if (!p || fstat(fd, &st) || st.st_size < (off_t)(2 * PAGE) || st.st_size > (off_t)MAX_FILE) {
        close(fd);
        free(p);
        fprintf(stderr, "glyphpack: %s: not a pack (its size)\n", path);
        return NULL;
    }
    p->len = (size_t)st.st_size;
    void *m = mmap(NULL, p->len, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (m == MAP_FAILED) {
        p->map = NULL;
        return refuse(p, path, "cannot be mapped");
    }
    p->map = m;
    /* one character's page at a time: the kernel's read-around would bring in
     * 27 pages for each (7 ms where one is under 1: measured on the H2) */
    madvise(m, p->len, MADV_RANDOM);

    const uint8_t *d = p->map;
    if (memcmp(d, "FATHGLY1", 8)) return refuse(p, path, "not a pack (or one for a newer Fathom)");
    p->count = le32(d + 8);
    uint32_t nsizes = le16(d + 12);
    p->index_off = le32(d + 16);
    if (le16(d + 14) || le32(d + 20) != p->len) return refuse(p, path, "cut short or damaged (its length)");
    if (!p->count || p->count > MAX_COUNT || !nsizes || nsizes > GLYPHPACK_MAX_SIZES) return refuse(p, path, "damaged (its counts)");
    if (p->index_off % PAGE || p->index_off < PAGE || p->index_off > p->len ||
        (uint64_t)p->count * 4 > p->len - p->index_off)
        return refuse(p, path, "damaged (its index)");
    if (HEADER + nsizes * 16 > PAGE) return refuse(p, path, "damaged (its header)");
    p->nsizes = (int)nsizes;
    for (int s = 0; s < p->nsizes; s++) {
        const uint8_t *h = d + HEADER + (size_t)s * 16;
        uint32_t px = le16(h), row = le16(h + 2), cell = le32(h + 4), off = le32(h + 8);
        int foot = (int8_t)h[12];
        if (px < 8 || px > 64 || row != (px + 1) / 2 || cell != row * px || h[13] != 4 || le16(h + 14) ||
            foot > 0 || foot < -(int)px || (s && (int)px <= p->z[s - 1].px))
            return refuse(p, path, "damaged (a size)");
        if (off % PAGE || off < p->index_off + p->count * 4 || off > p->len || (uint64_t)p->count * cell > p->len - off)
            return refuse(p, path, "damaged (a size's cells)");
        p->z[s].px = (int)px;
        p->z[s].row = (int)row;
        p->z[s].cell = cell;
        p->z[s].off = off;
        p->z[s].foot = foot;
    }
    /* the index: in order (the search needs it), every number within the cells.
     * Its pages are the ones that stay in memory anyway. */
    uint32_t last = 0;
    for (uint32_t i = 0; i < p->count; i++) {
        const uint8_t *e = d + p->index_off + (size_t)i * 4;
        uint32_t cp = le16(e);
        if ((i && cp <= last) || (le16(e + 2) & 0x7fff) >= p->count) return refuse(p, path, "damaged (its index's order)");
        last = cp;
    }
    long psz = sysconf(_SC_PAGESIZE);
    p->psz = psz > 0 ? (size_t)psz : PAGE;
    p->npages = (p->len + p->psz - 1) / p->psz;
    p->seen = calloc(p->npages, 1);
    p->fetched = calloc(p->npages, sizeof *p->fetched);
    p->asked = calloc(p->npages, sizeof *p->asked);
    if (!p->seen || !p->fetched || !p->asked) {
        free(p->seen); free(p->fetched); free(p->asked);
        p->seen = NULL; p->fetched = p->asked = NULL;
        return refuse(p, path, "no memory for it");
    }
    p->seen_at_ms = (long)(clock_ms() - 2 * SEEN_FOR_MS);
    pthread_mutex_init(&p->mu, NULL);
    pthread_cond_init(&p->cv, NULL);
    return p;
}

void glyphpack_close(glyphpack_t *p)
{
    if (!p) return;
    if (p->thread_on) {
        pthread_mutex_lock(&p->mu);
        p->stop = 1;
        pthread_cond_signal(&p->cv);
        pthread_mutex_unlock(&p->mu);
        pthread_join(p->thread, NULL);
    }
    if (p->seen) {                                /* (made last, after everything was checked) */
        pthread_cond_destroy(&p->cv);
        pthread_mutex_destroy(&p->mu);
    }
    free(p->seen);
    free(p->fetched);
    free(p->asked);
    if (p->map) munmap((void *)p->map, p->len);
    free(p);
}

int glyphpack_count(const glyphpack_t *p) { return p ? (int)p->count : 0; }

int glyphpack_sizes(const glyphpack_t *p, int *px)
{
    if (!p) return 0;
    for (int s = 0; s < p->nsizes; s++) px[s] = p->z[s].px;
    return p->nsizes;
}

int glyphpack_size_for(const glyphpack_t *p, int want)
{
    int best = 0;
    for (int s = 1; s < p->nsizes; s++) {
        if (p->z[s].px > want + 1) break;                     /* (smallest first) */
        if (abs(p->z[s].px - want) < abs(p->z[best].px - want)) best = s;
    }
    return best;
}

int glyphpack_foot(const glyphpack_t *p, int size)
{
    return p && size >= 0 && size < p->nsizes ? p->z[size].foot : 0;
}

int glyphpack_find(const glyphpack_t *p, uint32_t cp, int *half)
{
    if (!p || cp > 0xffff) return -1;
    const uint8_t *ix = p->map + p->index_off;
    uint32_t lo = 0, hi = p->count;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2, at = le16(ix + (size_t)mid * 4);
        if (at == cp) {
            uint32_t num = le16(ix + (size_t)mid * 4 + 2);
            if (half) *half = (num & 0x8000) != 0;
            return (int)(num & 0x7fff);
        }
        if (at < cp) lo = mid + 1;
        else hi = mid;
    }
    return -1;
}

const uint8_t *glyphpack_cell(const glyphpack_t *p, int size, int number, int *row_bytes)
{
    if (!p || size < 0 || size >= p->nsizes || number < 0 || (uint32_t)number >= p->count) return NULL;
    if (row_bytes) *row_bytes = p->z[size].row;
    return p->map + p->z[size].off + (size_t)number * p->z[size].cell;
}

/* ---------- never waiting for the card ---------- */

/* The pack's thread: each page handed to it read (one touch: the mapping is
 * "read at random", so the kernel brings that page alone), then counted. */
static void *fetcher(void *arg)
{
    glyphpack_t *p = arg;
    volatile unsigned sink = 0;
    pthread_mutex_lock(&p->mu);
    for (;;) {
        while (!p->qn && !p->stop) pthread_cond_wait(&p->cv, &p->mu);
        if (p->stop) break;
        uint32_t page = p->queue[p->qhead];
        p->qhead = (p->qhead + 1) % QUEUE;
        p->qn--;
        int delay = p->test_delay_ms;
        pthread_mutex_unlock(&p->mu);
        if (delay) usleep((useconds_t)delay * 1000);
        sink += p->map[(size_t)page * p->psz];
        atomic_store(&p->fetched[page], 1);
        atomic_store(&p->asked[page], 0);
        atomic_fetch_sub(&p->pending, 1);
        atomic_fetch_add(&p->arrivals, 1);
        pthread_mutex_lock(&p->mu);
    }
    pthread_mutex_unlock(&p->mu);
    (void)sink;
    return NULL;
}

/* A page for the thread, unless it is there or already asked for. Never waits:
 * with the queue full the page is left for the next time it is wanted. */
static void ask(glyphpack_t *p, size_t page)
{
    if (page >= p->npages || p->seen[page] || atomic_load(&p->fetched[page]) || atomic_load(&p->asked[page])) return;
    pthread_mutex_lock(&p->mu);
    if (!p->thread_on) {
        p->thread_on = pthread_create(&p->thread, NULL, fetcher, p) == 0;
        if (!p->thread_on) {                      /* no thread: the page is read by whoever draws */
            pthread_mutex_unlock(&p->mu);
            p->seen[page] = 1;
            return;
        }
    }
    if (p->qn < QUEUE) {
        p->queue[(p->qhead + p->qn++) % QUEUE] = (uint32_t)page;
        atomic_store(&p->asked[page], 1);
        atomic_fetch_add(&p->pending, 1);
        pthread_cond_signal(&p->cv);
    }
    pthread_mutex_unlock(&p->mu);
}

/* Which pages are in memory: the kernel asked, a second at most between two
 * askings (one call for the whole file; one for each character would cost more
 * than the drawing) */
static void look(glyphpack_t *p)
{
    long now = (long)clock_ms();
    if (p->test_forget || now - p->seen_at_ms < SEEN_FOR_MS) return;
    p->seen_at_ms = now;
    if (mincore((void *)p->map, p->len, (void *)p->seen)) memset(p->seen, 0, p->npages);
    /* (what the thread fetched before this is in the kernel's answer, or gone
     * again; a page it fetches while this runs may be asked for once more) */
    for (size_t i = 0; i < p->npages; i++) {
        p->seen[i] &= 1;
        atomic_store(&p->fetched[i], 0);
    }
}

static int cell_pages(glyphpack_t *p, int size, int number, size_t *first, size_t *last)
{
    if (!p || size < 0 || size >= p->nsizes || number < 0 || (uint32_t)number >= p->count) return 0;
    size_t at = p->z[size].off + (size_t)number * p->z[size].cell;
    *first = at / p->psz;
    *last = (at + p->z[size].cell - 1) / p->psz;
    return 1;
}

/* A size's first use: its commonest cells too, after what was asked for (the
 * thread takes pages in the order they come: with these first, the first
 * Japanese on the screen waited behind 57 pages, 113 ms on the H2). */
static void warm(glyphpack_t *p, int size)
{
    if (p->warmed[size]) return;
    p->warmed[size] = 1;
    uint32_t n = p->count < WARM_CELLS ? p->count : WARM_CELLS;
    size_t a = p->z[size].off / p->psz, b = (p->z[size].off + (size_t)n * p->z[size].cell - 1) / p->psz;
    for (size_t pg = a; pg <= b; pg++) ask(p, pg);
}

void glyphpack_want(glyphpack_t *p, int size, int number)
{
    size_t a, b;
    if (!cell_pages(p, size, number, &a, &b)) return;
    look(p);
    for (size_t pg = a; pg <= b; pg++) ask(p, pg);
    warm(p, size);
}

const uint8_t *glyphpack_cell_ready(glyphpack_t *p, int size, int number, int *row_bytes)
{
    size_t a, b;
    if (!cell_pages(p, size, number, &a, &b)) return NULL;
    look(p);
    int there = 1;
    for (size_t pg = a; pg <= b; pg++) {
        if (p->seen[pg] || atomic_load(&p->fetched[pg])) continue;
        there = 0;
        ask(p, pg);
    }
    warm(p, size);
    return there ? glyphpack_cell(p, size, number, row_bytes) : NULL;
}

unsigned glyphpack_arrivals(const glyphpack_t *p) { return p ? atomic_load(&p->arrivals) : 0; }
int glyphpack_pending(const glyphpack_t *p) { return p ? atomic_load(&p->pending) : 0; }

void glyphpack_test(glyphpack_t *p, int forget, int page_delay_ms)
{
    if (!p) return;
    pthread_mutex_lock(&p->mu);
    p->test_forget = forget;
    p->test_delay_ms = page_delay_ms;
    pthread_mutex_unlock(&p->mu);
    if (forget) {
        memset(p->seen, 0, p->npages);
        for (size_t i = 0; i < p->npages; i++) atomic_store(&p->fetched[i], 0);
    }
}
