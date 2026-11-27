/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* A character pack as the player reads it (app/ui/glyphpack.c): a small one
 * made here by hand, and the same damaged in each way a file from outside
 * can be. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "core/clock.h"
#include "ui/glyphpack.h"
#include "test_util.h"

enum { PAGE = 4096, N = 5, LEN = 4 * PAGE };
/* five characters, by code point; their cells kept in another order */
static const unsigned CP[N] = {0x3042, 0x4E00, 0x6771, 0xFF71, 0xFF9F};
static const unsigned NUM[N] = {1, 0, 4, 2 | 0x8000, 3 | 0x8000};

static void p16(unsigned char *p, unsigned v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static void p32(unsigned char *p, unsigned long v) { p16(p, (unsigned)(v & 0xffff)); p16(p + 2, (unsigned)(v >> 16)); }

static unsigned char img[LEN];

/* sizes 8 (4 bytes a row, 32 a cell) at page 2 and 12 (6, 72) at page 3; index at page 1 */
static void whole(void)
{
    memset(img, 0, sizeof img);
    memcpy(img, "FATHGLY1", 8);
    p32(img + 8, N);
    p16(img + 12, 2);
    p32(img + 16, PAGE);
    p32(img + 20, LEN);
    unsigned char *h = img + 24;
    p16(h, 8); p16(h + 2, 4); p32(h + 4, 32); p32(h + 8, 2 * PAGE); h[12] = (unsigned char)(signed char)-1; h[13] = 4;
    h += 16;
    p16(h, 12); p16(h + 2, 6); p32(h + 4, 72); p32(h + 8, 3 * PAGE); h[12] = (unsigned char)(signed char)-2; h[13] = 4;
    for (int i = 0; i < N; i++) {
        p16(img + PAGE + i * 4, CP[i]);
        p16(img + PAGE + i * 4 + 2, NUM[i]);
    }
    /* each cell filled with its own number + 1 in both halves of every byte */
    for (int k = 0; k < N; k++) {
        memset(img + 2 * PAGE + k * 32, (k + 1) * 0x11, 32);
        memset(img + 3 * PAGE + k * 72, (k + 1) * 0x11, 72);
    }
}

static char g_path[512];

static glyphpack_t *written(size_t len)
{
    FILE *f = fopen(g_path, "wb");
    if (!f || fwrite(img, 1, len, f) != len || fclose(f)) { printf("FAIL cannot write %s\n", g_path); t_fails++; return NULL; }
    return glyphpack_open(g_path);
}

static long ms_now(void) { return (long)clock_ms(); }

/* Until the pack's thread has nothing left to fetch (0: it still had, after `ms`) */
static int wait_fetched(glyphpack_t *p, long ms)
{
    long t0 = ms_now();
    while (glyphpack_pending(p) > 0) {
        if (ms_now() - t0 > ms) return 0;
        usleep(2000);
    }
    return 1;
}

/* The pack with one thing wrong is refused (and nothing else happens) */
#define REFUSED(change) do { whole(); change; glyphpack_t *bad = written(LEN); CHECK(bad == NULL); glyphpack_close(bad); } while (0)

int main(void)
{
    const char *tmp = getenv("TMPDIR");
    snprintf(g_path, sizeof g_path, "%s/fathom-test-glyphpack-%d.bin", tmp && *tmp ? tmp : "/tmp", (int)getpid());

    /* ---- no file: quietly nothing ---- */
    CHECK(glyphpack_open(g_path) == NULL);
    CHECK(glyphpack_count(NULL) == 0 && glyphpack_find(NULL, 0x3042, NULL) == -1 && glyphpack_cell(NULL, 0, 0, NULL) == NULL);
    glyphpack_close(NULL);

    /* ---- a whole pack ---- */
    whole();
    glyphpack_t *p = written(LEN);
    CHECK(p != NULL);
    if (p) {
        int px[GLYPHPACK_MAX_SIZES], half = -1, row = 0;
        CHECK(glyphpack_count(p) == N);
        CHECK(glyphpack_sizes(p, px) == 2 && px[0] == 8 && px[1] == 12);
        CHECK(glyphpack_foot(p, 0) == -1 && glyphpack_foot(p, 1) == -2 && glyphpack_foot(p, 2) == 0);
        /* the nearest size, the smaller of two as near, never more than a pixel over */
        CHECK(glyphpack_size_for(p, 4) == 0 && glyphpack_size_for(p, 8) == 0 && glyphpack_size_for(p, 9) == 0);
        CHECK(glyphpack_size_for(p, 10) == 0);              /* (8 and 12 as near: the smaller; and 12 is 2 over) */
        CHECK(glyphpack_size_for(p, 11) == 1 && glyphpack_size_for(p, 12) == 1 && glyphpack_size_for(p, 40) == 1);
        /* every character, its number and its width */
        for (int i = 0; i < N; i++) {
            half = -1;
            CHECK(glyphpack_find(p, CP[i], &half) == (int)(NUM[i] & 0x7fff));
            CHECK(half == (NUM[i] >> 15 ? 1 : 0));
        }
        CHECK(glyphpack_find(p, CP[1], NULL) == 0);
        /* those not in it: before, between, after, and past 16 bits */
        CHECK(glyphpack_find(p, 0x41, NULL) == -1 && glyphpack_find(p, 0x3043, NULL) == -1);
        CHECK(glyphpack_find(p, 0xFFA0, NULL) == -1 && glyphpack_find(p, 0x13042, NULL) == -1 && glyphpack_find(p, 0x20BB7, NULL) == -1);
        /* the cells, where the numbers say */
        for (int k = 0; k < N; k++) {
            const uint8_t *c = glyphpack_cell(p, 0, k, &row);
            CHECK(c && row == 4 && c[0] == (k + 1) * 0x11 && c[31] == (k + 1) * 0x11);
            c = glyphpack_cell(p, 1, k, &row);
            CHECK(c && row == 6 && c[0] == (k + 1) * 0x11 && c[71] == (k + 1) * 0x11);
        }
        CHECK(glyphpack_cell(p, 0, N, NULL) == NULL && glyphpack_cell(p, 0, -1, NULL) == NULL);
        CHECK(glyphpack_cell(p, 2, 0, NULL) == NULL && glyphpack_cell(p, -1, 0, NULL) == NULL);
        glyphpack_close(p);
    }

    /* ---- damaged, each way ---- */
    REFUSED(img[0] = 'X');                                   /* not a pack */
    REFUSED(img[7] = '2');                                   /* a later layout */
    REFUSED(p32(img + 20, LEN - 1));                         /* its length isn't the file's */
    REFUSED(p32(img + 8, 0));                                /* no characters */
    REFUSED(p32(img + 8, 40000));                            /* more than a number can say */
    REFUSED(p32(img + 8, 0xffffffffUL));
    REFUSED(p16(img + 12, 0));                               /* no sizes */
    REFUSED(p16(img + 12, 9));                               /* too many */
    REFUSED(p16(img + 14, 1));
    REFUSED(p32(img + 16, PAGE + 4));                        /* the index off its page */
    REFUSED(p32(img + 16, 0));                               /* ...over the header */
    REFUSED(p32(img + 16, 8 * PAGE));                        /* ...past the end */
    REFUSED(p32(img + 16, 0xfffff000UL));
    REFUSED(p16(img + 24, 7));                               /* a size too small */
    REFUSED(p16(img + 24, 65));                              /* ...too large */
    REFUSED(p16(img + 24 + 2, 5));                           /* rows of the wrong length */
    REFUSED(p32(img + 24 + 4, 33));                          /* cells of the wrong length */
    REFUSED(p32(img + 24 + 8, 2 * PAGE + 1));                /* cells off their page */
    REFUSED(p32(img + 24 + 8, 0));                           /* ...over the header and the index */
    REFUSED(p32(img + 24 + 8, 4 * PAGE));                    /* ...at the very end: no room */
    REFUSED(p32(img + 24 + 8, 0xfffff000UL));
    REFUSED(p32(img + 40 + 8, 3 * PAGE + PAGE));             /* the second size's past the end */
    REFUSED(img[24 + 12] = 1);                               /* a foot over the baseline */
    REFUSED(img[24 + 13] = 8);                               /* not 4 bits a pixel */
    REFUSED(p16(img + 40, 8));                               /* sizes not in order (8 then 8) */
    REFUSED(p16(img + PAGE + 4, 0x3042));                    /* the index not in order (twice the same) */
    REFUSED(p16(img + PAGE + 8, 0x4000));                    /* ...going back */
    REFUSED(p16(img + PAGE + 2, N));                         /* a number past the cells */
    REFUSED(p16(img + PAGE + 2, 0x7fff));

    /* cut short: every length under the whole, a page at a time and by one byte */
    whole();
    for (size_t len = 0; len < LEN; len += len < PAGE ? 512 : PAGE) {
        glyphpack_t *bad = written(len);
        CHECK(bad == NULL);
        glyphpack_close(bad);
    }
    glyphpack_t *bad = written(LEN - 1);
    CHECK(bad == NULL);
    glyphpack_close(bad);

    /* ---- never waiting for the card ---- */
    whole();
    p = written(LEN);
    CHECK(p != NULL);
    if (p) {
        int row = 0;
        /* nothing in memory (as far as the pack may know): the cell isn't given,
         * its pages are fetched by the pack's thread, and then it is */
        glyphpack_test(p, 1, 0);
        CHECK(glyphpack_arrivals(p) == 0 && glyphpack_pending(p) == 0);
        CHECK(glyphpack_cell_ready(p, 0, 0, &row) == NULL);
        CHECK(wait_fetched(p, 2000));
        CHECK(glyphpack_arrivals(p) > 0);
        const uint8_t *c = glyphpack_cell_ready(p, 0, 0, &row);
        CHECK(c && row == 4 && c[0] == 0x11 && c[31] == 0x11);

        /* a slow card (300 ms a page): asking returns at once, the same page is
         * asked for once however often it is wanted, and the cell comes */
        glyphpack_test(p, 1, 300);
        long t0 = ms_now();
        CHECK(glyphpack_cell_ready(p, 1, 4, &row) == NULL);
        CHECK(ms_now() - t0 < 100);
        int pending = glyphpack_pending(p);
        CHECK(pending > 0);
        t0 = ms_now();
        for (int i = 0; i < 1000; i++) {
            CHECK(glyphpack_cell_ready(p, 1, 4, &row) == NULL || ms_now() - t0 >= 250);
            glyphpack_want(p, 1, 4);
            if (ms_now() - t0 >= 250) break;
        }
        CHECK(glyphpack_pending(p) <= pending);
        CHECK(wait_fetched(p, 5000));
        c = glyphpack_cell_ready(p, 1, 4, &row);
        CHECK(c && row == 6 && c[0] == 0x55 && c[71] == 0x55);

        /* cells that aren't: nothing asked for, nothing given */
        pending = glyphpack_pending(p);
        glyphpack_want(p, 0, N);
        glyphpack_want(p, 5, 0);
        CHECK(glyphpack_cell_ready(p, 0, -1, NULL) == NULL && glyphpack_cell_ready(p, 2, 0, NULL) == NULL);
        CHECK(glyphpack_pending(p) == pending);

        /* closed while its thread is at a page: it ends, without the page's wait being cut short more than once */
        glyphpack_test(p, 1, 300);
        CHECK(glyphpack_cell_ready(p, 0, 2, &row) == NULL);
        t0 = ms_now();
        glyphpack_close(p);
        CHECK(ms_now() - t0 < 2000);
    }
    CHECK(glyphpack_arrivals(NULL) == 0 && glyphpack_pending(NULL) == 0 && glyphpack_cell_ready(NULL, 0, 0, NULL) == NULL);
    glyphpack_want(NULL, 0, 0);
    glyphpack_test(NULL, 1, 1);

    unlink(g_path);
    return t_result();
}
