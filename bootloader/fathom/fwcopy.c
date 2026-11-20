/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Files out of the H2's stock system, read straight from the flash (see
 * fwcopy.h). The stock root filesystem is UBIFS on UBI in partition 4. Like
 * tools/bluetooth/extract-firmware.py, this doesn't follow UBI's or UBIFS's
 * indexes: it scans every eraseblock for UBIFS nodes (magic, 8-byte aligned,
 * a good CRC), and keeps the newest (highest sequence number) directory entry
 * for each wanted name, inode for each inode number and data node for each
 * block. Nodes never cross an eraseblock, so one eraseblock in memory at a
 * time is enough; data nodes are only indexed (where they are), and the ones
 * a file needs are read again when it's copied.
 *
 * The LZO1X decompressor follows Linux's lib/lzo/lzo1x_decompress_safe.c (by
 * way of the Python script), with every read and write bounds-checked.
 */
#include "fwcopy.h"

#include <stdio.h>
#include <string.h>

#define UBIFS_MAGIC 0x06101831u
#define NODE_INODE  0
#define NODE_DATA   1
#define NODE_DENT   2
#define BLOCK_SIZE  4096u

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t le64(const uint8_t *p) { return le32(p) | (uint64_t)le32(p + 4) << 32; }

/* UBIFS's CRC32: the usual reflected one, seeded with ~0 and not inverted after. */
static uint32_t crc_table[256];

static uint32_t crc32_ubifs(const uint8_t *p, uint32_t n)
{
    if (!crc_table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            crc_table[i] = c;
        }
    uint32_t c = 0xFFFFFFFFu;
    while (n--) c = crc_table[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return c;
}

/* A node at p (at most room bytes there): its length, or 0 if it isn't one. */
static uint32_t node_at(const uint8_t *p, uint32_t room)
{
    if (room < 24 || le32(p) != UBIFS_MAGIC) return 0;
    uint32_t len = le32(p + 16);
    if (len < 24 || len > BLOCK_SIZE + 64 || len > room) return 0;
    if (crc32_ubifs(p + 8, len - 8) != le32(p + 4)) return 0;
    return len;
}

int fwc_scan(struct fwc *f, struct fwc_file *files, int n)
{
    f->data_n = f->inodes_n = f->bad_pebs = 0;
    for (int i = 0; i < n; i++) files[i].sqnum = files[i].inum = files[i].size = 0;

    for (uint32_t peb = 0; peb < FWC_ROOTFS_SIZE / FWC_PEB_SIZE; peb++) {
        uint32_t base = FWC_ROOTFS_ADDR + peb * FWC_PEB_SIZE;
        if (f->read(f->ctx, base, f->peb, FWC_PEB_SIZE) < 0) {
            if (peb == 0) return -1;
            f->bad_pebs++;
            continue;
        }
        if (peb == 0 && memcmp(f->peb, "UBI#", 4)) return -1;   /* no stock system here */

        for (uint32_t i = 0; i + 24 <= FWC_PEB_SIZE;) {
            const uint8_t *p = f->peb + i;
            uint32_t len = node_at(p, FWC_PEB_SIZE - i);
            if (!len) { i += 8; continue; }
            uint64_t sqnum = le64(p + 8);
            switch (p[20]) {
            case NODE_DENT: {
                uint32_t nlen = le16(p + 50);
                if (56 + nlen > len) break;
                for (int k = 0; k < n; k++)
                    if (strlen(files[k].name) == nlen && !memcmp(p + 56, files[k].name, nlen) &&
                        sqnum > files[k].sqnum) {
                        files[k].sqnum = sqnum;
                        files[k].inum = (uint32_t)le64(p + 40);
                    }
                break;
            }
            case NODE_INODE:
                if (len >= 56 && f->inodes_n < f->inodes_max)
                    f->inodes[f->inodes_n++] = (struct fwc_inode){sqnum, le32(p + 24), (uint32_t)le64(p + 48)};
                break;
            case NODE_DATA:
                if (len >= 48 && f->data_n < f->data_max)
                    f->data[f->data_n++] = (struct fwc_data){sqnum, le32(p + 24), le32(p + 28) & 0x1FFFFFFFu, base + i};
                break;
            }
            i += (len + 7) & ~7u;
        }
    }

    /* Each file's size: its newest inode node. */
    for (int k = 0; k < n; k++) {
        if (!files[k].inum) continue;
        uint64_t best = 0;
        int found = 0;
        for (uint32_t j = 0; j < f->inodes_n; j++)
            if (f->inodes[j].ino == files[k].inum && (!found || f->inodes[j].sqnum > best)) {
                best = f->inodes[j].sqnum;
                files[k].size = f->inodes[j].size;
                found = 1;
            }
        if (!found) files[k].inum = 0;
    }
    return 0;
}

/* LZO1X: src (n bytes) into out (exactly cap bytes expected). The length made, or -1. */
static int lzo1x_decompress(const uint8_t *src, uint32_t n, uint8_t *out, uint32_t cap)
{
    uint32_t ip = 0, op = 0, state = 0;

#define NEED(k) do { if (ip + (k) > n) return -1; } while (0)
    /* A length: base + the low bits, or base + a run of zero bytes (255 each) and one more. */
#define LENGTH(var, base, bits) do {                                        \
        if (bits) var = (base) + (bits);                                     \
        else {                                                               \
            uint32_t extra_ = 0;                                             \
            NEED(1);                                                         \
            while (src[ip] == 0) { extra_ += 255; ip++; NEED(1); }           \
            extra_ += src[ip++];                                             \
            var = (base) + extra_;                                           \
        }                                                                    \
    } while (0)
#define LITERALS(k) do {                                                    \
        uint32_t k_ = (k);                                                   \
        NEED(k_);                                                            \
        if (op + k_ > cap) return -1;                                        \
        memcpy(out + op, src + ip, k_); op += k_; ip += k_;                   \
    } while (0)
#define MATCH(dist, k) do {                                                 \
        uint32_t d_ = (dist), k_ = (k);                                      \
        if (d_ > op || op + k_ > cap) return -1;                             \
        for (uint32_t j_ = 0; j_ < k_; j_++, op++) out[op] = out[op - d_];   \
    } while (0)

    NEED(1);
    if (src[0] > 17) {
        uint32_t k = src[0] - 17u;
        ip = 1;
        LITERALS(k);
        state = k >= 4 ? 4 : k;
    }
    while (ip < n) {
        uint32_t t = src[ip++], s, k;
        if (t < 16) {
            if (state == 0) {
                LENGTH(k, t & 15 ? 3 : 18, t & 15);
                LITERALS(k);
                state = 4;
                continue;
            }
            NEED(1);
            uint32_t h = src[ip++];
            if (state < 4) MATCH((h << 2) + ((t >> 2) & 3) + 1, 2);
            else MATCH((h << 2) + ((t >> 2) & 3) + 2049, 3);
            s = t & 3;
        } else if (t < 32) {
            LENGTH(k, t & 7 ? 2 : 9, t & 7);
            NEED(2);
            uint32_t d = le16(src + ip);
            ip += 2;
            uint32_t dist = 16384 + ((t >> 3) & 1) * 16384 + (d >> 2);
            if (dist == 16384) break;                 /* the end of the stream */
            MATCH(dist, k);
            s = d & 3;
        } else if (t < 64) {
            LENGTH(k, t & 31 ? 2 : 33, t & 31);
            NEED(2);
            uint32_t d = le16(src + ip);
            ip += 2;
            MATCH((d >> 2) + 1, k);
            s = d & 3;
        } else {
            NEED(1);
            uint32_t h = src[ip++];
            MATCH((h << 3) + ((t >> 2) & 7) + 1, t < 128 ? 3 + ((t >> 5) & 1) : 5 + ((t >> 5) & 3));
            s = t & 3;
        }
        if (s) LITERALS(s);
        state = s;
    }
    return (int)op;
#undef NEED
#undef LENGTH
#undef LITERALS
#undef MATCH
}

int fwc_extract(struct fwc *f, const struct fwc_file *file, uint8_t *out, uint8_t *node)
{
    static uint8_t block[BLOCK_SIZE];
    if (!file->inum) return -1;
    uint32_t nblocks = (file->size + BLOCK_SIZE - 1) / BLOCK_SIZE;
    for (uint32_t b = 0; b < nblocks; b++) {
        const struct fwc_data *best = NULL;
        for (uint32_t j = 0; j < f->data_n; j++)
            if (f->data[j].ino == file->inum && f->data[j].block == b && (!best || f->data[j].sqnum > best->sqnum))
                best = &f->data[j];
        if (!best) return -2;                          /* a block is missing */

        /* The node again (it never crosses its eraseblock) */
        uint32_t room = FWC_PEB_SIZE - (best->addr - FWC_ROOTFS_ADDR) % FWC_PEB_SIZE;
        if (room > FWC_NODE_MAX) room = FWC_NODE_MAX;
        if (f->read(f->ctx, best->addr, node, room) < 0) return -3;
        uint32_t len = node_at(node, room);
        if (!len || len < 48) return -4;
        uint32_t usize = le32(node + 40), compr = le16(node + 44);
        if (usize > BLOCK_SIZE) return -5;
        if (compr == 0) {
            if (48 + usize > len) return -5;
            memcpy(block, node + 48, usize);
        } else if (compr == 1) {
            if (lzo1x_decompress(node + 48, len - 48, block, usize) != (int)usize) return -6;
        } else {
            return -7;                                 /* zlib or zstd: the stock system has none */
        }
        uint32_t at = b * BLOCK_SIZE, want = file->size - at < BLOCK_SIZE ? file->size - at : BLOCK_SIZE;
        if (usize < want) return -8;
        memcpy(out + at, block, want);
    }
    return 0;
}

int fwc_check_hcd(const uint8_t *d, uint32_t n)
{
    uint32_t i = 0;
    while (i < n) {
        if (i + 3 > n) return 0;
        i += 3u + d[i + 2];
    }
    return n >= 3 && i == n && d[1] == 0xFC;
}

static int contains(const uint8_t *d, uint32_t n, const char *s)
{
    size_t l = strlen(s);
    for (uint32_t i = 0; i + l <= n; i++)
        if (!memcmp(d + i, s, l)) return 1;
    return 0;
}

int fwc_check_wifi_fw(const uint8_t *d, uint32_t n) { return n > 200000 && contains(d, n, "43430a1-roml"); }

int fwc_check_nvram(const uint8_t *d, uint32_t n) { return contains(d, n, "manfid=0x2d0") && contains(d, n, "boardtype="); }

/* ---------- the stock kernel's header ---------- */

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

int fwc_kernel_note(const uint8_t *h, char *out, uint32_t cap)
{
    if (be32(h) != 0x27051956u) return -1;              /* IH_MAGIC */
    uint8_t c[FWC_UIMAGE_HDR];
    memcpy(c, h, sizeof c);
    memset(c + 4, 0, 4);                                /* the CRC is over the header with it 0 */
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < sizeof c; i++) {
        crc ^= c[i];
        for (int k = 0; k < 8; k++) crc = crc & 1 ? 0xEDB88320u ^ (crc >> 1) : crc >> 1;
    }
    if (~crc != be32(h + 4)) return -1;
    char name[33];
    uint32_t n = 0;
    for (uint32_t i = 32; i < 64 && h[i]; i++)
        name[n++] = h[i] >= 0x20 && h[i] < 0x7f ? (char)h[i] : '?';
    name[n] = '\0';
    int len = snprintf(out, cap, "name=%s\ntime=%lu\n", name, (unsigned long)be32(h + 8));
    return len < 0 || (uint32_t)len >= cap ? -1 : len;
}
