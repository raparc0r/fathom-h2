/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* Reading a zip file, an entry at a time (unzip.h). */
#include "net/unzip.h"
#include "core/bytes.h"
#include "core/fsafe.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <zlib.h>

#define CHUNK 16384


static int read_at(FILE *f, uint64_t off, void *buf, size_t n)
{
    return fseeko(f, (off_t)off, SEEK_SET) == 0 && fread(buf, 1, n, f) == n ? 0 : -1;
}

int zip_open(zip_t *z, const char *path)
{
    memset(z, 0, sizeof *z);
    z->f = fopen(path, "rbe");
    if (!z->f) return -1;
    if (fseeko(z->f, 0, SEEK_END) || ftello(z->f) < 22) goto fail;
    z->file_size = (uint64_t)ftello(z->f);
    /* The end of central directory record: the last 22 bytes, unless the zip
     * has a comment (up to 64 KB) after it. */
    size_t tail = z->file_size < 22 + 65535 ? (size_t)z->file_size : 22 + 65535;
    unsigned char *b = malloc(tail);
    if (!b || read_at(z->f, z->file_size - tail, b, tail)) { free(b); goto fail; }
    long at = -1;
    for (long i = (long)tail - 22; i >= 0; i--)
        if (b[i] == 'P' && b[i + 1] == 'K' && b[i + 2] == 5 && b[i + 3] == 6 &&
            i + 22 + (long)le16(b + i + 20) == (long)tail) { at = i; break; }
    if (at < 0) { free(b); goto fail; }
    const unsigned char *e = b + at;
    unsigned disk = le16(e + 4), cd_disk = le16(e + 6), here = le16(e + 8);
    z->n = le16(e + 10);
    z->cd_size = le32(e + 12);
    z->cd_off = le32(e + 16);
    free(b);
    /* one disk; no zip64 (its markers are all-ones) */
    if (disk || cd_disk || here != z->n || z->n == 0xFFFF || z->cd_off == 0xFFFFFFFF ||
        (uint64_t)z->cd_off + z->cd_size > z->file_size)
        goto fail;
    z->next = z->cd_off;
    return 0;
fail:
    fclose(z->f);
    z->f = NULL;
    return -1;
}

void zip_close(zip_t *z)
{
    if (z->f) fclose(z->f);
    z->f = NULL;
}

void zip_rewind(zip_t *z)
{
    z->next = z->cd_off;
    z->seen = 0;
}

int zip_next(zip_t *z, zip_entry_t *e)
{
    if (!z->f) return -1;
    if (z->seen >= z->n) return 0;
    unsigned char h[46];
    if ((uint64_t)z->next + 46 > (uint64_t)z->cd_off + z->cd_size || read_at(z->f, z->next, h, 46) ||
        le32(h) != 0x02014b50)
        return -1;
    memset(e, 0, sizeof *e);
    e->flags = le16(h + 8);
    e->method = le16(h + 10);
    e->crc = le32(h + 16);
    e->csize = le32(h + 20);
    e->usize = le32(h + 24);
    unsigned nl = le16(h + 28), xl = le16(h + 30), cl = le16(h + 32);
    e->local_off = le32(h + 42);
    if (nl == 0 || nl >= sizeof e->name || (uint64_t)z->next + 46 + nl + xl + cl > (uint64_t)z->cd_off + z->cd_size)
        return -1;
    if (fread(e->name, 1, nl, z->f) != nl) return -1;
    e->name[nl] = '\0';
    if (memchr(e->name, '\0', nl)) return -1;
    e->dir = e->name[nl - 1] == '/';
    z->next += 46 + nl + xl + cl;
    z->seen++;
    return 1;
}

int zip_extract(zip_t *z, const zip_entry_t *e, zip_sink_fn sink, void *ctx, const volatile int *abort)
{
    unsigned char h[30];
    if (!z->f || (e->flags & 1) || (e->method != 0 && e->method != 8)) return -1;   /* encrypted, or unknown */
    if (e->csize == 0xFFFFFFFF || e->usize == 0xFFFFFFFF) return -1;               /* zip64 */
    if (read_at(z->f, e->local_off, h, 30) || le32(h) != 0x04034b50) return -1;
    /* The sizes and CRC come from the central directory: a local header may
     * leave them to a descriptor after the data (flag bit 3). */
    uint64_t data = (uint64_t)e->local_off + 30 + le16(h + 26) + le16(h + 28);
    if (data + e->csize > z->file_size || fseeko(z->f, (off_t)data, SEEK_SET)) return -1;

    unsigned char *in = malloc(CHUNK), *out = e->method == 8 ? malloc(CHUNK) : NULL;
    if (!in || (e->method == 8 && !out)) { free(in); free(out); return -1; }
    uLong crc = crc32(0L, Z_NULL, 0);
    uint64_t left = e->csize, produced = 0;
    int ok = 0;
    if (e->method == 0) {
        if (e->csize != e->usize) goto done;
        while (left) {
            size_t k = left < CHUNK ? (size_t)left : CHUNK;
            if (fread(in, 1, k, z->f) != k || (abort && *abort)) goto done;
            crc = crc32(crc, in, (uInt)k);
            if (sink(in, k, ctx)) goto done;
            left -= k;
            produced += k;
        }
        ok = 1;
    } else {
        z_stream s;
        memset(&s, 0, sizeof s);
        if (inflateInit2(&s, -MAX_WBITS) != Z_OK) goto done;   /* raw deflate, as zip has it */
        int r = Z_OK;
        while (r != Z_STREAM_END) {
            if (abort && *abort) break;
            if (s.avail_in == 0) {
                if (!left) break;                              /* the data ended before the stream */
                size_t k = left < CHUNK ? (size_t)left : CHUNK;
                if (fread(in, 1, k, z->f) != k) break;
                s.next_in = in;
                s.avail_in = (uInt)k;
                left -= k;
            }
            s.next_out = out;
            s.avail_out = CHUNK;
            r = inflate(&s, Z_NO_FLUSH);
            if (r != Z_OK && r != Z_STREAM_END) break;
            size_t k = CHUNK - s.avail_out;
            if (k) {
                produced += k;
                if (produced > e->usize) break;                 /* more than it says */
                crc = crc32(crc, out, (uInt)k);
                if (sink(out, k, ctx)) break;
            }
        }
        inflateEnd(&s);
        ok = r == Z_STREAM_END;
    }
done:
    /* What was read leaves the page cache: 16 MB of zip would otherwise push
     * out the player's own code (run from the card) on the H2. */
    file_drop_cache(fileno(z->f), (off_t)data, (off_t)e->csize);
    free(in);
    free(out);
    return ok && produced == e->usize && crc == e->crc ? 0 : -1;
}
