/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* The FAT32 reader behind the quick "nothing changed" check (app/library/fatwalk.c),
 * on small card images made here: a sound one, and ones damaged the ways a card
 * pulled out mid-write (or made on purpose) can be. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "library/fatwalk.h"
#include "test_util.h"

/* The card: 32 KB clusters as the H2's 128 GB card has (1,024 entries a cluster) */
enum { SPC = 64, RESERVED = 32, FATSZ = 8, DATA = RESERVED + FATSZ, CLUSTERS = 80, PER = SPC * 512 / 32 };
#define EOC 0x0FFFFFFFu

static unsigned char *g_img;
static const size_t g_len = (size_t)(DATA + CLUSTERS * SPC) * 512;

static void le16(unsigned char *p, uint32_t v) { p[0] = (unsigned char)v, p[1] = (unsigned char)(v >> 8); }
static void le32(unsigned char *p, uint32_t v) { le16(p, v), le16(p + 2, v >> 16); }

static void blank(void)
{
    memset(g_img, 0, g_len);
    unsigned char *bs = g_img;
    le16(bs + 11, 512);
    bs[13] = SPC;
    le16(bs + 14, RESERVED);
    bs[16] = 1;                                   /* one FAT */
    le32(bs + 32, DATA + CLUSTERS * SPC);         /* sectors in all */
    le32(bs + 36, FATSZ);
    le32(bs + 44, 2);                             /* the root folder's cluster */
    memcpy(bs + 82, "FAT32   ", 8);
    bs[510] = 0x55, bs[511] = 0xAA;
}

static void fat(uint32_t cluster, uint32_t next) { le32(g_img + RESERVED * 512 + 4 * (size_t)cluster, next); }

/* Entry i of the folder cluster c: an 8.3 name (11 bytes), a folder or a file of `size` */
static void entry(uint32_t c, int i, const char *name11, int folder, uint32_t first, uint32_t size)
{
    unsigned char *e = g_img + (size_t)(DATA + (c - 2) * SPC) * 512 + 32 * (size_t)i;
    memcpy(e, name11, 11);
    e[11] = folder ? 0x10 : 0x20;
    le16(e + 20, first >> 16);
    le16(e + 26, first);
    le32(e + 28, size);
}

/* Entry i of the folder cluster c: one piece of a long name, the only one (up
 * to 13 UTF-16 units), for the 8.3 entry that follows it */
static void long_name(uint32_t c, int i, const uint16_t *u, int n, const char *name11)
{
    static const int at[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
    unsigned char *e = g_img + (size_t)(DATA + (c - 2) * SPC) * 512 + 32 * (size_t)i, sum = 0;
    for (int k = 0; k < 11; k++) sum = (unsigned char)(((sum & 1) << 7) + (sum >> 1) + (unsigned char)name11[k]);
    e[0] = 0x41;                                  /* piece 1, the last */
    e[11] = 0x0f;
    e[13] = sum;
    for (int k = 0; k < 13; k++) le16(e + at[k], k < n ? u[k] : k == n ? 0 : 0xffff);
}

/* Folder clusters first..last full of files (each its own name), chained in order */
static void fill(uint32_t first, uint32_t last)
{
    for (uint32_t c = first; c <= last; c++) {
        for (int i = 0; i < PER; i++) {
            char name[16];
            snprintf(name, sizeof name, "F%02u%05dTXT", (unsigned)c, i);
            entry(c, i, name, 0, 0, 1);
        }
        fat(c, c < last ? c + 1 : EOC);
    }
}

static char g_path[512];

static void save(void)
{
    FILE *f = fopen(g_path, "wb");
    if (!f || fwrite(g_img, 1, g_len, f) != g_len) { perror(g_path); exit(1); }
    fclose(f);
}

static long g_files, g_one;

static int on_file(const char *path, int64_t mtime, int64_t size, void *ctx)
{
    (void)mtime, (void)ctx;
    g_files++;
    if (!strcmp(path, "/card/ONE.TXT") && size == 5) g_one++;
    return 0;
}

static int walk(void)
{
    g_files = g_one = 0;
    return fatwalk(g_path, "/card", "", on_file, NULL);
}

int main(void)
{
    char dir[400];
    t_mkdtemp(dir, sizeof dir, "fathom-test-fatwalk");
    snprintf(g_path, sizeof g_path, "%s/card.img", dir);
    g_img = malloc(g_len);
    if (!g_img) return 1;

    /* a sound card: a file in the root, and a folder of two clusters (one full, the next ending early) */
    blank();
    fat(2, EOC);
    entry(2, 0, "ONE     TXT", 0, 0, 5);
    entry(2, 1, "SUB        ", 1, 3, 0);
    fill(3, 4);
    memset(g_img + (size_t)(DATA + 2 * SPC) * 512 + 32 * 2, 0, 32);   /* (cluster 4 ends after two files) */
    save();
    CHECK(walk() == 0);
    CHECK(g_one == 1 && g_files == 1 + PER + 2);
    if (g_files != 1 + PER + 2) fprintf(stderr, "a sound card: %ld files\n", g_files);

    /* the folder's second cluster points back at its first: followed round 4,096
     * clusters (128 MB held, each file reported 2,048 times) before; now the walk
     * is given up, and the kernel's own takes over */
    blank();
    fat(2, EOC);
    entry(2, 0, "ONE     TXT", 0, 0, 5);
    entry(2, 1, "SUB        ", 1, 3, 0);
    fill(3, 4);
    fat(4, 3);
    save();
    CHECK(walk() == -1);
    CHECK(g_files <= 1 + 3 * PER);
    if (g_files > 1 + 3 * PER) fprintf(stderr, "a folder that loops: %ld files\n", g_files);
    /* (and one that points at itself; and the folder the walk starts from) */
    fat(3, 3);
    save();
    CHECK(walk() == -1);
    CHECK(fatwalk(g_path, "/card", "SUB/DEEPER", on_file, NULL) == -1);

    /* a folder longer than any can be (65,536 entries, 2 MB: 64 clusters here): read that far and no further */
    blank();
    fat(2, EOC);
    entry(2, 0, "ONE     TXT", 0, 0, 5);
    entry(2, 1, "SUB        ", 1, 3, 0);
    fill(3, 72);
    save();
    CHECK(walk() == 0);
    CHECK(g_one == 1 && g_files == 1 + 65536);
    if (g_files != 1 + 65536) fprintf(stderr, "a folder of 70 clusters: %ld files\n", g_files);

    /* a folder that can't be read (here the card's file ends before it, or in
     * the middle of it): the walk is given up, not the folder taken for empty
     * or short (rc 0 then, and its songs dropped as gone) */
    blank();
    fat(2, EOC);
    entry(2, 0, "ONE     TXT", 0, 0, 5);
    entry(2, 1, "SUB        ", 1, 3, 0);
    fill(3, 4);
    save();
    CHECK(walk() == 0 && g_files == 1 + 2 * PER);
    CHECK(truncate(g_path, (off_t)(DATA + 2 * SPC) * 512 + 4096) == 0);    /* (in the folder's second cluster) */
    CHECK(walk() == -1);
    CHECK(truncate(g_path, (off_t)(DATA + 1 * SPC) * 512) == 0);           /* (before its first) */
    CHECK(walk() == -1);
    CHECK(fatwalk(g_path, "/card", "SUB", on_file, NULL) == -1);

    /* the folder the walk starts from has an emoji in its name ("Music" and a
     * note, U+1F3B5: two UTF-16 units on the card, one character in the path
     * asked for): never found before, so every scan went the slow way */
    blank();
    fat(2, EOC);
    static const uint16_t note[] = {'M', 'u', 's', 'i', 'c', 0xd83c, 0xdfb5};
    long_name(2, 0, note, 7, "MUSIC~1    ");
    entry(2, 1, "MUSIC~1    ", 1, 3, 0);
    fill(3, 3);
    save();
    g_files = 0;
    CHECK(fatwalk(g_path, "/card", "Music\xf0\x9f\x8e\xb5", on_file, NULL) == 0);
    CHECK(g_files == PER);
    CHECK(fatwalk(g_path, "/card", "Music", on_file, NULL) == -1);    /* (and not by a part of its name) */

    /* a boot sector with no sectors left for data: not a card to read (the count of clusters wrapped) */
    blank();
    fat(2, EOC);
    entry(2, 0, "ONE     TXT", 0, 0, 5);
    le32(g_img + 32, DATA);
    save();
    CHECK(walk() == -1);
    le32(g_img + 32, DATA - 1);
    save();
    CHECK(walk() == -1);

    free(g_img);
    unlink(g_path);
    rmdir(dir);
    return t_result();
}
