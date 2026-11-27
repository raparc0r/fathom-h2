/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
#include "fatwalk.h"
#include "core/bytes.h"
#include "core/utf8.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    int fd;
    uint32_t bps, spc, cluster_bytes, fat_start, data_start, root, nclusters;
    uint8_t fat_sec[512];         /* the FAT sector last read */
    int64_t fat_sec_no;
    fatwalk_cb cb;
    void *ctx;
    int stop;
    int failed;                   /* a folder not read whole (its chain of clusters loops, a read failed,
                                   * no memory): the card is not ours to report on this way */
} fw_t;

static int rd(fw_t *f, uint64_t off, void *buf, size_t n)
{
    return pread(f->fd, buf, n, (off_t)off) == (ssize_t)n ? 0 : -1;
}

static uint32_t next_cluster(fw_t *f, uint32_t c)
{
    uint64_t byte = (uint64_t)c * 4;
    int64_t sec = (int64_t)(byte / f->bps);
    if (sec != f->fat_sec_no) {
        if (rd(f, (uint64_t)(f->fat_start + sec) * f->bps, f->fat_sec, f->bps)) {
            /* (not the chain's end: the folder would look short, and its other files gone) */
            f->fat_sec_no = -1;
            f->failed = f->stop = 1;
            return 0x0FFFFFFF;
        }
        f->fat_sec_no = sec;
    }
    return le32(f->fat_sec + byte % f->bps) & 0x0FFFFFFF;
}

/* FAT date and time (local, 2 s steps) as the kernel shows them with no tz=
 * option and the kernel's timezone at UTC: taken as UTC. */
static int64_t fat_time(uint16_t date, uint16_t time)
{
    struct tm tm = {0};
    tm.tm_year = ((date >> 9) & 0x7f) + 80;
    tm.tm_mon = ((date >> 5) & 0x0f) - 1;
    tm.tm_mday = date & 0x1f;
    tm.tm_hour = (time >> 11) & 0x1f;
    tm.tm_min = (time >> 5) & 0x3f;
    tm.tm_sec = (time & 0x1f) * 2;
    if (tm.tm_mon < 0) tm.tm_mon = 0;
    if (tm.tm_mday < 1) tm.tm_mday = 1;
    return (int64_t)timegm(&tm);
}

static void utf8_put(char *out, size_t cap, size_t *w, uint32_t u)
{
    char b[4];
    size_t n = utf8_encode(u, b);
    if (*w + n < cap) { memcpy(out + *w, b, n); *w += n; }
}

static uint8_t sfn_checksum(const uint8_t *name)
{
    uint8_t s = 0;
    for (int i = 0; i < 11; i++) s = (uint8_t)(((s & 1) << 7) + (s >> 1) + name[i]);
    return s;
}

/* An 8.3 name as Linux shows it (no long name): case from the NT flags, the
 * rest as stored; the bytes as Latin-1 (codepage 437 differs only above 0x7f,
 * where names without a long name are rare). */
static void short_name(const uint8_t *e, char *out, size_t cap)
{
    size_t w = 0;
    int base_lower = e[12] & 0x08, ext_lower = e[12] & 0x10;
    int n = 8;
    while (n > 0 && e[n - 1] == ' ') n--;
    for (int i = 0; i < n; i++) {
        uint8_t c = (i == 0 && e[0] == 0x05) ? 0xe5 : e[i];
        if (base_lower && c >= 'A' && c <= 'Z') c += 32;
        utf8_put(out, cap, &w, c);
    }
    int x = 3;
    while (x > 0 && e[8 + x - 1] == ' ') x--;
    if (x) {
        utf8_put(out, cap, &w, '.');
        for (int i = 0; i < x; i++) {
            uint8_t c = e[8 + i];
            if (ext_lower && c >= 'A' && c <= 'Z') c += 32;
            utf8_put(out, cap, &w, c);
        }
    }
    out[w] = '\0';
}

/* A long name as its pieces come before the entry they name (the last piece
 * first). */
typedef struct {
    uint16_t u[20 * 13 + 1];
    int n;                        /* pieces still expected; -1 = none collected */
    int max;
    uint8_t sum;
} lfn_t;

/* The long-name piece at e, taken into l (or what l held given up: a piece out of turn) */
static void lfn_piece(lfn_t *l, const uint8_t *e)
{
    static const int at[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
    int ord = e[0] & 0x1f;
    if (e[0] & 0x40) { l->n = ord; l->max = ord; l->sum = e[13]; memset(l->u, 0, sizeof l->u); }
    if (l->n < 1 || ord != l->n || e[13] != l->sum || ord > 20) { l->n = -1; return; }
    for (int k = 0; k < 13; k++) l->u[(ord - 1) * 13 + k] = (uint16_t)le16(e + at[k]);
    l->n = ord - 1;
}

/* The name of the entry at e: its long name when l holds all of it and it is
 * this entry's, else its 8.3 name. A surrogate pair is written as the one
 * character it stands for, as the kernel names it (find wrote each half as it
 * came: a start folder with an emoji in its name was never found, and every
 * scan went the slow way). */
static void entry_name(const lfn_t *l, const uint8_t *e, char *out, size_t cap)
{
    if (l->n != 0 || sfn_checksum(e) != l->sum) { short_name(e, out, cap); return; }
    size_t w = 0;
    for (int k = 0; k < l->max * 13; k++) {
        uint32_t u = l->u[k];
        if (u == 0 || u == 0xffff) break;
        if (u >= 0xd800 && u < 0xdc00 && k + 1 < l->max * 13) {
            uint32_t lo = l->u[k + 1];
            if (lo >= 0xdc00 && lo < 0xe000) { u = 0x10000 + ((u - 0xd800) << 10) + (lo - 0xdc00); k++; }
        }
        utf8_put(out, cap, &w, u);
    }
    out[w] = '\0';
}

static uint32_t entry_cluster(const uint8_t *e) { return le16(e + 20) << 16 | le16(e + 26); }

static void walk_dir(fw_t *f, uint32_t cluster, const char *path, int depth);

#define DIR_PIECE 4096
#define DIR_MAX (65536u * 32)     /* FAT32's own limit: 65,536 entries a folder (2 MB) */

static fatwalk_entry_fn g_entry_fn;
static void *g_entry_ctx;

void fatwalk_set_entry_hook(fatwalk_entry_fn fn, void *ctx)
{
    g_entry_fn = fn;
    g_entry_ctx = ctx;
}

/* Read a directory (all its clusters) into memory; caller frees. No more of
 * it than a folder can be, and none at all of one whose chain of clusters
 * comes round again (the walk is given up: the kernel's takes over): a loop
 * was followed 4,096 clusters round, 128 MB at 32 KB a cluster, and its files
 * reported as many times. Given up too when a read fails or there's no
 * memory: the folder ended there, looked short, and fatwalk still said 0, so
 * the songs past that point were later dropped as gone. */
static uint8_t *read_dir(fw_t *f, uint32_t cluster, size_t *len)
{
    uint8_t *buf = NULL;
    size_t n = 0;
    uint32_t mark = cluster, steps = 0, lap = 1;   /* a cluster passed, to meet again in a loop (moved on by doubling steps) */
    *len = 0;
    while (cluster >= 2 && cluster < 0x0FFFFFF8 && n + f->cluster_bytes <= DIR_MAX) {
        if (cluster - 2 >= f->nclusters) goto fail;   /* (a chain that leaves the card: damage, not the folder's end) */
        uint8_t *nb = realloc(buf, n + f->cluster_bytes);
        if (!nb) goto fail;
        buf = nb;
        /* A piece at a time, up to the entry that ends the folder: an album's
         * dozen files take a kilobyte or two of a 32 KB cluster, and reading
         * all of it, for each of thousands of folders, was most of a walk. */
        uint64_t at = (uint64_t)(f->data_start + (cluster - 2) * f->spc) * f->bps;
        uint32_t got = 0;
        int ended = 0;
        while (got < f->cluster_bytes && !ended) {
            uint32_t piece = f->cluster_bytes - got < DIR_PIECE ? f->cluster_bytes - got : DIR_PIECE;
            if (rd(f, at + got, buf + n + got, piece)) goto fail;
            for (uint32_t o = 0; o + 32 <= piece; o += 32)
                if (buf[n + got + o] == 0x00) { ended = 1; break; }
            got += piece;
        }
        if (got < f->cluster_bytes) memset(buf + n + got, 0, f->cluster_bytes - got);   /* (past its end: none) */
        n += f->cluster_bytes;
        if (ended) break;
        cluster = next_cluster(f, cluster);
        if (f->failed || cluster == mark) goto fail;
        if (++steps == lap) { mark = cluster; steps = 0; lap *= 2; }
    }
    *len = n;
    return buf;
fail:
    f->failed = f->stop = 1;
    free(buf);
    return NULL;
}

/* A folder's subfolders, gone into once its own files are given, in name
 * order: files then reach the caller in nearly the order of their paths
 * whatever order the folders were made in (the library keeps the names it's
 * given in blocks and lets a block go once the scan is past it: with folders
 * in the card's own order, a block held names from all over the alphabet and
 * none could go before the end). And the folder's own entries (a cluster,
 * 32 KB) aren't held while its subfolders are read. */
typedef struct { char *name; uint32_t cluster; } subdir_t;

static int cmp_subdir(const void *a, const void *b)
{
    return strcmp(((const subdir_t *)a)->name, ((const subdir_t *)b)->name);
}

static void walk_dir(fw_t *f, uint32_t cluster, const char *path, int depth)
{
    if (depth > 16 || f->stop) return;
    size_t len;
    uint8_t *d = read_dir(f, cluster, &len);
    if (!d) return;
    subdir_t *sub = NULL;
    int nsub = 0, capsub = 0;
    lfn_t lfn;
    lfn.n = -1;
    lfn.max = 0;
    lfn.sum = 0;
    for (size_t off = 0; off + 32 <= len && !f->stop; off += 32) {
        const uint8_t *e = d + off;
        if (e[0] == 0x00) break;                 /* end of directory */
        if (e[0] == 0xe5) { lfn.n = -1; continue; }
        if (e[11] == 0x0f) { lfn_piece(&lfn, e); continue; }   /* a long-name piece */
        if (e[11] & 0x08) { lfn.n = -1; continue; }   /* volume label */
        char name[1024];
        entry_name(&lfn, e, name, sizeof name);
        lfn.n = -1;
        if (name[0] == '.' || !name[0]) continue;     /* ".", "..", hidden: as walk() */
        char p[2048];
        snprintf(p, sizeof p, "%s/%s", path, name);
        uint32_t c = entry_cluster(e);
        if (e[11] & 0x10) {
            if (nsub == capsub) {
                int nc = capsub ? capsub * 2 : 32;
                subdir_t *ns = realloc(sub, (size_t)nc * sizeof *ns);
                if (!ns) { walk_dir(f, c, p, depth + 1); continue; }   /* (no memory to note it: gone into now) */
                sub = ns;
                capsub = nc;
            }
            if ((sub[nsub].name = strdup(name))) sub[nsub++].cluster = c;
            else walk_dir(f, c, p, depth + 1);
        } else {
            uint32_t size = le32(e + 28);
            int64_t mt = fat_time((uint16_t)le16(e + 24), (uint16_t)le16(e + 22));
            if (g_entry_fn) g_entry_fn(p, c, c >= 2 && c - 2 < f->nclusters ? next_cluster(f, c) : 0xFFFFFFFFu, size, g_entry_ctx);
            if (f->cb(p, mt, size, f->ctx)) f->stop = 1;
        }
    }
    free(d);
    if (nsub > 1) qsort(sub, (size_t)nsub, sizeof *sub, cmp_subdir);
    for (int i = 0; i < nsub; i++) {
        if (!f->stop) {
            char p[2048];
            snprintf(p, sizeof p, "%s/%s", path, sub[i].name);
            walk_dir(f, sub[i].cluster, p, depth + 1);
        }
        free(sub[i].name);
    }
    free(sub);
}

/* The first cluster of `name` in the directory at `cluster`, or 0. */
static uint32_t find(fw_t *f, uint32_t cluster, const char *name)
{
    size_t len;
    uint8_t *d = read_dir(f, cluster, &len);
    if (!d) return 0;
    uint32_t found = 0;
    lfn_t lfn;
    lfn.n = -1;
    lfn.max = 0;
    lfn.sum = 0;
    for (size_t off = 0; off + 32 <= len && !found; off += 32) {
        const uint8_t *e = d + off;
        if (e[0] == 0x00) break;
        if (e[0] == 0xe5) { lfn.n = -1; continue; }
        if (e[11] == 0x0f) { lfn_piece(&lfn, e); continue; }
        char nm[1024];
        entry_name(&lfn, e, nm, sizeof nm);
        lfn.n = -1;
        if ((e[11] & 0x10) && !strcmp(nm, name)) found = entry_cluster(e);
    }
    free(d);
    return found;
}

int fatwalk(const char *dev, const char *mount, const char *rel, fatwalk_cb cb, void *ctx)
{
    fw_t f = {.fd = open(dev, O_RDONLY | O_CLOEXEC), .fat_sec_no = -1, .cb = cb, .ctx = ctx};
    if (f.fd < 0) return -1;
#ifdef POSIX_FADV_RANDOM
    posix_fadvise(f.fd, 0, 0, POSIX_FADV_RANDOM);   /* (folders lie all over the card: nothing read ahead) */
#endif
    uint8_t bs[512];
    int rc = -1;
    if (rd(&f, 0, bs, sizeof bs) || bs[510] != 0x55 || bs[511] != 0xaa || memcmp(bs + 82, "FAT32", 5)) goto out;
    f.bps = le16(bs + 11);
    f.spc = bs[13];
    uint32_t reserved = le16(bs + 14), nfats = bs[16], fatsz = le32(bs + 36), total = le32(bs + 32);
    f.root = le32(bs + 44);
    if (f.bps != 512 || !f.spc || !nfats || !fatsz) goto out;
    f.fat_start = reserved;
    f.data_start = reserved + nfats * fatsz;
    if (total <= f.data_start) goto out;        /* (no room for data: the count of clusters would wrap) */
    f.cluster_bytes = f.bps * f.spc;
    f.nclusters = (total - f.data_start) / f.spc;
    uint32_t c = f.root;
    char base[1024];
    snprintf(base, sizeof base, "%s", mount);
    if (rel && *rel) {                          /* down to the start folder */
        char tmp[512];
        snprintf(tmp, sizeof tmp, "%s", rel);
        for (char *save = NULL, *part = strtok_r(tmp, "/", &save); part; part = strtok_r(NULL, "/", &save)) {
            c = find(&f, c, part);
            if (!c) goto out;
            size_t l = strlen(base);
            snprintf(base + l, sizeof base - l, "/%s", part);
        }
    }
    walk_dir(&f, c, base, 0);
    rc = f.failed ? -1 : 0;
out:
    close(f.fd);
    return rc;
}
