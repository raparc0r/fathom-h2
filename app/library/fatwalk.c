/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
#include "fatwalk.h"

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
        if (rd(f, (uint64_t)(f->fat_start + sec) * f->bps, f->fat_sec, f->bps)) return 0x0FFFFFFF;
        f->fat_sec_no = sec;
    }
    uint32_t v;
    memcpy(&v, f->fat_sec + byte % f->bps, 4);
    return v & 0x0FFFFFFF;
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
    int n;
    if (u < 0x80) { b[0] = (char)u; n = 1; }
    else if (u < 0x800) { b[0] = (char)(0xc0 | u >> 6); b[1] = (char)(0x80 | (u & 0x3f)); n = 2; }
    else if (u < 0x10000) { b[0] = (char)(0xe0 | u >> 12); b[1] = (char)(0x80 | ((u >> 6) & 0x3f)); b[2] = (char)(0x80 | (u & 0x3f)); n = 3; }
    else { b[0] = (char)(0xf0 | u >> 18); b[1] = (char)(0x80 | ((u >> 12) & 0x3f)); b[2] = (char)(0x80 | ((u >> 6) & 0x3f)); b[3] = (char)(0x80 | (u & 0x3f)); n = 4; }
    if (*w + (size_t)n < cap) { memcpy(out + *w, b, (size_t)n); *w += (size_t)n; }
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

static void walk_dir(fw_t *f, uint32_t cluster, const char *path, int depth);

#define DIR_PIECE 4096

static fatwalk_entry_fn g_entry_fn;
static void *g_entry_ctx;

void fatwalk_set_entry_hook(fatwalk_entry_fn fn, void *ctx)
{
    g_entry_fn = fn;
    g_entry_ctx = ctx;
}

/* Read a directory (all its clusters) into memory; caller frees. */
static uint8_t *read_dir(fw_t *f, uint32_t cluster, size_t *len)
{
    uint8_t *buf = NULL;
    size_t n = 0;
    for (int guard = 0; cluster >= 2 && cluster < 0x0FFFFFF8 && guard < 4096; guard++) {
        if (cluster - 2 >= f->nclusters) break;
        uint8_t *nb = realloc(buf, n + f->cluster_bytes);
        if (!nb) break;
        buf = nb;
        /* A piece at a time, up to the entry that ends the folder: an album's
         * dozen files take a kilobyte or two of a 32 KB cluster, and reading
         * all of it, for each of thousands of folders, was most of a walk. */
        uint64_t at = (uint64_t)(f->data_start + (cluster - 2) * f->spc) * f->bps;
        uint32_t got = 0;
        int ended = 0, bad = 0;
        while (got < f->cluster_bytes && !ended) {
            uint32_t piece = f->cluster_bytes - got < DIR_PIECE ? f->cluster_bytes - got : DIR_PIECE;
            if (rd(f, at + got, buf + n + got, piece)) { bad = 1; break; }
            for (uint32_t o = 0; o + 32 <= piece; o += 32)
                if (buf[n + got + o] == 0x00) { ended = 1; break; }
            got += piece;
        }
        if (bad) break;
        if (got < f->cluster_bytes) memset(buf + n + got, 0, f->cluster_bytes - got);   /* (past its end: none) */
        n += f->cluster_bytes;
        if (ended) break;
        cluster = next_cluster(f, cluster);
    }
    *len = n;
    return buf;
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
    uint16_t lfn[20 * 13 + 1];
    int lfn_n = -1;               /* pieces expected; -1 = none collected */
    uint8_t lfn_sum = 0;
    int lfn_max = 0;
    for (size_t off = 0; off + 32 <= len && !f->stop; off += 32) {
        const uint8_t *e = d + off;
        if (e[0] == 0x00) break;                 /* end of directory */
        if (e[0] == 0xe5) { lfn_n = -1; continue; }
        if (e[11] == 0x0f) {                     /* a long-name piece */
            int ord = e[0] & 0x1f;
            if (e[0] & 0x40) { lfn_n = ord; lfn_max = ord; lfn_sum = e[13]; memset(lfn, 0, sizeof lfn); }
            if (lfn_n < 1 || ord != lfn_n || e[13] != lfn_sum || ord > 20) { lfn_n = -1; continue; }
            static const int at[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
            for (int k = 0; k < 13; k++) lfn[(ord - 1) * 13 + k] = (uint16_t)(e[at[k]] | e[at[k] + 1] << 8);
            lfn_n = ord - 1;
            continue;
        }
        if (e[11] & 0x08) { lfn_n = -1; continue; }   /* volume label */
        char name[1024];
        size_t w = 0;
        if (lfn_n == 0 && sfn_checksum(e) == lfn_sum) {
            for (int k = 0; k < lfn_max * 13; k++) {
                uint32_t u = lfn[k];
                if (u == 0 || u == 0xffff) break;
                if (u >= 0xd800 && u < 0xdc00 && k + 1 < lfn_max * 13) {
                    uint32_t lo = lfn[k + 1];
                    if (lo >= 0xdc00 && lo < 0xe000) { u = 0x10000 + ((u - 0xd800) << 10) + (lo - 0xdc00); k++; }
                }
                utf8_put(name, sizeof name, &w, u);
            }
            name[w] = '\0';
        } else {
            short_name(e, name, sizeof name);
        }
        lfn_n = -1;
        if (name[0] == '.' || !name[0]) continue;     /* ".", "..", hidden: as walk() */
        char p[2048];
        snprintf(p, sizeof p, "%s/%s", path, name);
        uint32_t c = (uint32_t)(e[20] | e[21] << 8) << 16 | (uint32_t)(e[26] | e[27] << 8);
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
            uint32_t size;
            memcpy(&size, e + 28, 4);
            int64_t mt = fat_time((uint16_t)(e[24] | e[25] << 8), (uint16_t)(e[22] | e[23] << 8));
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
    uint16_t lfn[20 * 13 + 1];
    int lfn_n = -1, lfn_max = 0;
    uint8_t lfn_sum = 0;
    for (size_t off = 0; off + 32 <= len && !found; off += 32) {
        const uint8_t *e = d + off;
        if (e[0] == 0x00) break;
        if (e[0] == 0xe5) { lfn_n = -1; continue; }
        if (e[11] == 0x0f) {
            int ord = e[0] & 0x1f;
            if (e[0] & 0x40) { lfn_n = ord; lfn_max = ord; lfn_sum = e[13]; memset(lfn, 0, sizeof lfn); }
            if (lfn_n < 1 || ord != lfn_n || e[13] != lfn_sum || ord > 20) { lfn_n = -1; continue; }
            static const int at[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
            for (int k = 0; k < 13; k++) lfn[(ord - 1) * 13 + k] = (uint16_t)(e[at[k]] | e[at[k] + 1] << 8);
            lfn_n = ord - 1;
            continue;
        }
        char nm[1024];
        size_t w = 0;
        if (lfn_n == 0 && sfn_checksum(e) == lfn_sum) {
            for (int k = 0; k < lfn_max * 13 && lfn[k] && lfn[k] != 0xffff; k++) utf8_put(nm, sizeof nm, &w, lfn[k]);
            nm[w] = '\0';
        } else {
            short_name(e, nm, sizeof nm);
        }
        lfn_n = -1;
        if ((e[11] & 0x10) && !strcmp(nm, name))
            found = (uint32_t)(e[20] | e[21] << 8) << 16 | (uint32_t)(e[26] | e[27] << 8);
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
    f.bps = (uint32_t)(bs[11] | bs[12] << 8);
    f.spc = bs[13];
    uint32_t reserved = (uint32_t)(bs[14] | bs[15] << 8), nfats = bs[16], fatsz, total;
    memcpy(&fatsz, bs + 36, 4);
    memcpy(&total, bs + 32, 4);
    memcpy(&f.root, bs + 44, 4);
    if (f.bps != 512 || !f.spc || !nfats || !fatsz) goto out;
    f.fat_start = reserved;
    f.data_start = reserved + nfats * fatsz;
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
    rc = 0;
out:
    close(f.fd);
    return rc;
}
