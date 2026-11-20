/* SPDX-License-Identifier: GPL-3.0-or-later */
/* HiBy's update files (hibyupt.h). */
#include "sys/hibyupt.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "core/lang.h"
#include "sys/md5.h"

#define SECTOR 2048u
#define CHUNK  (32u * 1024)
#define UNDO_MAGIC "FUPTUND1"

/* ---------- small helpers ---------- */

static uint32_t le32(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static void put_both32(uint8_t *p, uint32_t v)      /* ISO 9660's both-byte-order field */
{
    for (int i = 0; i < 4; i++) {
        p[i] = (uint8_t)(v >> (8 * i));
        p[7 - i] = (uint8_t)(v >> (8 * i));
    }
}

static int both32(const uint8_t *p, uint32_t *v)
{
    *v = le32(p);
    return *v == be32(p + 4);
}

static int read_at(int fd, void *buf, size_t len, uint64_t off)
{
    uint8_t *b = buf;
    while (len) {
        ssize_t n = pread(fd, b, len, (off_t)off);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        b += n;
        len -= (size_t)n;
        off += (uint64_t)n;
    }
    return 0;
}

static int write_at(int fd, const void *buf, size_t len, uint64_t off)
{
    const uint8_t *b = buf;
    while (len) {
        ssize_t n = pwrite(fd, b, len, (off_t)off);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        b += n;
        len -= (size_t)n;
        off += (uint64_t)n;
    }
    return 0;
}

/* What was read leaves the page cache: a 60 MB image would otherwise push out
 * the music's cached pages and other programs' code, in 24 MB of RAM. */
static void drop_cache(int fd, uint64_t off, uint64_t len)
{
#ifdef POSIX_FADV_DONTNEED
    posix_fadvise(fd, (off_t)off, (off_t)len, POSIX_FADV_DONTNEED);
#else
    (void)fd; (void)off; (void)len;
#endif
}

const upt_file_t *upt_file(const upt_t *u, const char *name)
{
    for (int i = 0; i < u->nfiles; i++)
        if (!strcmp(u->files[i].name, name)) return &u->files[i];
    return NULL;
}

static uint32_t sectors(uint32_t size) { return (size + SECTOR - 1) / SECTOR; }

/* ---------- the directory trees ---------- */

/* A record's name: ISO 9660's "UBOOT.BIN;1", or Joliet's UCS-2, as "uboot.bin". */
static void record_name(const uint8_t *r, int joliet, char *out, size_t cap)
{
    int len = r[32];
    const uint8_t *s = r + 33;
    size_t n = 0;
    if (joliet) {
        for (int i = 0; i + 1 < len && n + 1 < cap; i += 2)
            out[n++] = s[i] ? '?' : (char)tolower(s[i + 1]);
    } else {
        for (int i = 0; i < len && n + 1 < cap; i++) out[n++] = (char)tolower(s[i]);
    }
    out[n] = '\0';
    char *semi = strchr(out, ';');
    if (semi) *semi = '\0';
    n = strlen(out);
    if (n && out[n - 1] == '.') out[n - 1] = '\0';
}

/* The files at the root of one tree, merged into u->files. */
static upt_err_t read_tree(upt_t *u, const uint8_t *root, int joliet)
{
    uint32_t lba, len;
    if (!both32(root + 2, &lba) || !both32(root + 10, &len)) return UPT_ERR_NOT_UPDATE;
    if (!len || len > 16 * SECTOR || lba + sectors(len) > u->blocks) return UPT_ERR_NOT_UPDATE;
    uint8_t sec[SECTOR];
    for (uint32_t s = 0; s < sectors(len); s++) {
        uint64_t base = (uint64_t)(lba + s) * SECTOR;
        if (read_at(u->fd, sec, SECTOR, base)) return UPT_ERR_DAMAGED;
        uint32_t end = len - s * SECTOR < SECTOR ? len - s * SECTOR : SECTOR;
        for (uint32_t i = 0; i < end;) {
            uint8_t rl = sec[i];
            if (!rl) break;                             /* the rest of the sector is unused */
            if (rl < 34 || i + rl > end || 33u + sec[i + 32] > rl) return UPT_ERR_NOT_UPDATE;
            const uint8_t *r = sec + i;
            uint32_t flba, fsize;
            int dot = r[32] == 1 && r[33] <= 1;         /* "." or ".." */
            if (!dot && !(r[25] & 2)) {                 /* a file, not a folder */
                if (!both32(r + 2, &flba) || !both32(r + 10, &fsize)) return UPT_ERR_NOT_UPDATE;
                char name[32];
                record_name(r, joliet, name, sizeof name);
                upt_file_t *f = (upt_file_t *)upt_file(u, name);
                if (!f) {
                    if (u->nfiles == UPT_MAX_FILES) return UPT_ERR_NOT_UPDATE;
                    f = &u->files[u->nfiles++];
                    memset(f, 0, sizeof *f);
                    snprintf(f->name, sizeof f->name, "%s", name);
                    f->lba = flba;
                    f->size = fsize;
                } else if (f->lba != flba || f->size != fsize) {
                    return UPT_ERR_LAYOUT;              /* the two trees disagree */
                }
                if (f->nrec < UPT_MAX_TREES) f->rec[f->nrec++] = base + i;
            }
            i += rl;
        }
    }
    return UPT_OK;
}

static upt_err_t read_layout(upt_t *u, uint64_t file_size)
{
    uint8_t vd[SECTOR], primary[34], joliet[34];
    int have_primary = 0, have_joliet = 0;
    for (uint32_t n = 16; n < 32; n++) {
        if (read_at(u->fd, vd, SECTOR, (uint64_t)n * SECTOR)) return UPT_ERR_NOT_UPDATE;
        if (memcmp(vd + 1, "CD001", 5)) return UPT_ERR_NOT_UPDATE;
        if (vd[0] == 255) break;
        if (vd[0] == 1 && !have_primary) {
            if ((vd[128] | vd[129] << 8) != SECTOR) return UPT_ERR_NOT_UPDATE;
            if (!both32(vd + 80, &u->blocks)) return UPT_ERR_NOT_UPDATE;
            memcpy(primary, vd + 156, 34);
            have_primary = 1;
        } else if (vd[0] == 2 && vd[88] == '%' && vd[89] == '/' && strchr("@CE", vd[90]) && !have_joliet) {
            memcpy(joliet, vd + 156, 34);
            have_joliet = 1;
        }
    }
    if (!have_primary) return UPT_ERR_NOT_UPDATE;
    if ((uint64_t)u->blocks * SECTOR > file_size) return UPT_ERR_DAMAGED;   /* cut short */
    upt_err_t e = read_tree(u, primary, 0);
    if (!e && have_joliet) e = read_tree(u, joliet, 1);
    if (e) return e;
    for (int i = 0; i < u->nfiles; i++)
        if (u->files[i].lba + sectors(u->files[i].size) > u->blocks) return UPT_ERR_DAMAGED;
    return UPT_OK;
}

/* ---------- update.txt and version.txt ---------- */

static const char *skip_space(const char *p, const char *end)
{
    while (p < end && isspace((unsigned char)*p)) p++;
    return p;
}

/* The next "key={ ... }" from *p: 1 (key, body), 0 at the end, -1 not that. */
static int next_block(const char **p, const char *end, char *key, size_t kcap,
                      const char **body, size_t *blen)
{
    const char *s = skip_space(*p, end);
    if (s == end) return 0;
    size_t n = 0;
    while (s < end && (isalnum((unsigned char)*s) || *s == '_')) {
        if (n + 1 < kcap) key[n++] = *s;
        s++;
    }
    key[n] = '\0';
    if (!n) return -1;
    s = skip_space(s, end);
    if (s == end || *s++ != '=') return -1;
    s = skip_space(s, end);
    if (s == end || *s++ != '{') return -1;
    const char *close = memchr(s, '}', (size_t)(end - s));
    if (!close) return -1;
    *body = s;
    *blen = (size_t)(close - s);
    *p = close + 1;
    return 1;
}

/* A "field=value" line's value in a block's body ("" if none). */
static void block_field(const char *body, size_t blen, const char *field, char *out, size_t cap)
{
    const char *p = body, *end = body + blen;
    size_t fl = strlen(field);
    out[0] = '\0';
    while (p < end) {
        p = skip_space(p, end);
        const char *eol = memchr(p, '\n', (size_t)(end - p));
        if (!eol) eol = end;
        if ((size_t)(eol - p) > fl && !memcmp(p, field, fl) && p[fl] == '=') {
            const char *v = p + fl + 1, *ve = v;
            while (ve < eol && !isspace((unsigned char)*ve)) ve++;
            size_t n = (size_t)(ve - v) < cap - 1 ? (size_t)(ve - v) : cap - 1;
            memcpy(out, v, n);
            out[n] = '\0';
            return;
        }
        p = eol;
    }
}

static int is_md5(const char *s)
{
    if (strlen(s) != 32) return 0;
    for (; *s; s++)
        if (!isxdigit((unsigned char)*s)) return 0;
    return 1;
}

/* The image an entry names ("autoupdate/uboot.bin" -> uboot.bin), or NULL. */
static const upt_file_t *entry_file(const upt_t *u, const upt_entry_t *e)
{
    const char *base = strrchr(e->path, '/');
    char name[64];
    snprintf(name, sizeof name, "%s", base ? base + 1 : e->path);
    for (char *c = name; *c; c++) *c = (char)tolower(*c);
    return upt_file(u, name);
}

static upt_err_t parse_manifest(upt_t *u)
{
    const char *p = u->manifest, *end = u->manifest + u->manifest_len, *body;
    size_t blen;
    char key[16];
    int r, kernel = 0, rootfs = 0, uboot = 0;
    u->nentries = 0;
    while ((r = next_block(&p, end, key, sizeof key, &body, &blen)) == 1) {
        if (u->nentries == UPT_MAX_ENTRIES) return UPT_ERR_NOT_UPDATE;
        upt_entry_t *e = &u->entries[u->nentries++];
        snprintf(e->key, sizeof e->key, "%s", key);
        block_field(body, blen, "name", e->name, sizeof e->name);
        block_field(body, blen, "file_path", e->path, sizeof e->path);
        block_field(body, blen, "md5", e->md5, sizeof e->md5);
        for (char *c = e->md5; *c; c++) *c = (char)tolower(*c);
        if (strncmp(e->path, "autoupdate/", 11) || !is_md5(e->md5) || !entry_file(u, e))
            return UPT_ERR_NOT_UPDATE;
        if (!strcmp(e->name, "kernel")) kernel = 1;
        else if (!strcmp(e->name, "rootfs")) rootfs = 1;
        else if (!strcmp(e->name, "uboot")) uboot = 1;
    }
    if (r < 0) return UPT_ERR_NOT_UPDATE;
    if (kernel && rootfs) u->kind = uboot ? UPT_HIBY_PATCHED : UPT_HIBY;
    else if (uboot && u->nentries == 1) u->kind = UPT_FATHOM;
    else return UPT_ERR_NOT_UPDATE;
    return UPT_OK;
}

static upt_err_t read_small(upt_t *u, const char *name, char *out, size_t cap, size_t *len)
{
    const upt_file_t *f = upt_file(u, name);
    if (!f) return UPT_ERR_NOT_UPDATE;
    if (f->size >= cap) return UPT_ERR_NOT_UPDATE;
    if (read_at(u->fd, out, f->size, (uint64_t)f->lba * SECTOR)) return UPT_ERR_DAMAGED;
    out[f->size] = '\0';
    if (memchr(out, '\0', f->size)) return UPT_ERR_NOT_UPDATE;
    if (len) *len = f->size;
    return UPT_OK;
}

/* ---------- open ---------- */

upt_err_t upt_open(upt_t *u, const char *path, int writable)
{
    memset(u, 0, sizeof *u);
    u->fd = open(path, (writable ? O_RDWR : O_RDONLY) | O_CLOEXEC);
    if (u->fd < 0) return UPT_ERR_OPEN;
    u->writable = writable;
    struct stat st;
    if (fstat(u->fd, &st) || !S_ISREG(st.st_mode)) return UPT_ERR_OPEN;
    upt_err_t e = read_layout(u, (uint64_t)st.st_size);
    if (e) return e;

    char version[1024];
    if ((e = read_small(u, "version.txt", version, sizeof version, NULL))) return e;
    const char *p = version, *body;
    size_t blen;
    char key[16];
    int r;
    while ((r = next_block(&p, version + strlen(version), key, sizeof key, &body, &blen)) == 1)
        if (!strcmp(key, "version")) {
            block_field(body, blen, "name", u->model, sizeof u->model);
            block_field(body, blen, "ver", u->ver, sizeof u->ver);
            break;
        }
    if (r != 1 || !u->model[0]) return UPT_ERR_NOT_UPDATE;
    if (strcmp(u->model, UPT_MODEL)) return UPT_ERR_MODEL;

    if ((e = read_small(u, "update.txt", u->manifest, sizeof u->manifest, &u->manifest_len))) return e;
    return parse_manifest(u);
}

void upt_close(upt_t *u)
{
    if (u->fd >= 0) close(u->fd);
    u->fd = -1;
}

/* ---------- check ---------- */

typedef struct {
    void (*progress)(void *ctx, uint64_t done, uint64_t total);
    void *ctx;
    uint64_t done, total;
    uint8_t *buf;                       /* 64 + CHUNK bytes, only while checking */
} prog_t;

static const char BANNER[] = "U-Boot SPL ";

/* The image's MD5; if banner, also U-Boot's build date ("" if none). */
static upt_err_t hash_file(upt_t *u, const upt_file_t *f, char md5[33], char *banner, size_t bcap, prog_t *pg)
{
    uint8_t *buf = pg->buf;             /* 64 bytes carried over, for a banner across chunks */
    md5_t m;
    md5_init(&m);
    size_t carry = 0;
    if (banner) banner[0] = '\0';
    uint64_t base = (uint64_t)f->lba * SECTOR, last_drop = 0;
    for (uint32_t off = 0; off < f->size;) {
        uint32_t n = f->size - off < CHUNK ? f->size - off : CHUNK;
        if (read_at(u->fd, buf + carry, n, base + off)) return UPT_ERR_DAMAGED;
        md5_update(&m, buf + carry, n);
        if (banner && !banner[0]) {
            size_t have = carry + n;
            for (size_t i = 0; i + sizeof BANNER + 40 <= have; i++)
                if (!memcmp(buf + i, BANNER, sizeof BANNER - 1)) {
                    const char *open = memchr(buf + i, '(', 40), *close;
                    if (open && (close = memchr(open, ')', (size_t)(buf + have - (const uint8_t *)open))) &&
                        close - open < (long)bcap) {
                        memcpy(banner, open + 1, (size_t)(close - open - 1));
                        banner[close - open - 1] = '\0';
                    }
                    break;
                }
            carry = have < 64 ? have : 64;
            memmove(buf, buf + have - carry, carry);
        }
        off += n;
        pg->done += n;
        if (off - last_drop >= 1024 * 1024 || off == f->size) {
            drop_cache(u->fd, base + last_drop, off - last_drop);
            last_drop = off;
        }
        if (pg->progress) pg->progress(pg->ctx, pg->done, pg->total);
    }
    uint8_t d[16];
    md5_final(&m, d);
    md5_hex(d, md5);
    return UPT_OK;
}

static upt_err_t check(upt_t *u, prog_t *pgp)
{
    prog_t pg = *pgp;
    const upt_file_t *uboot = upt_file(u, "uboot.bin");
    int uboot_listed = 0;
    for (int i = 0; i < u->nentries; i++) {
        pg.total += entry_file(u, &u->entries[i])->size;
        if (entry_file(u, &u->entries[i]) == uboot) uboot_listed = 1;
    }
    if (u->kind != UPT_FATHOM) {
        if (!uboot || uboot->size < 16 * 1024 || uboot->size > 512 * 1024) return UPT_ERR_NO_BOOTLOADER;
        if (!uboot_listed) pg.total += uboot->size;
    }
    upt_err_t e;
    for (int i = 0; i < u->nentries; i++) {
        const upt_file_t *f = entry_file(u, &u->entries[i]);
        char md5[33];
        if (f == uboot && u->kind != UPT_FATHOM) continue;     /* below, with its banner */
        if ((e = hash_file(u, f, md5, NULL, 0, &pg))) return e;
        if (strcmp(md5, u->entries[i].md5)) return UPT_ERR_DAMAGED;
        if (f == uboot) snprintf(u->uboot_md5, sizeof u->uboot_md5, "%s", md5);
    }
    if (u->kind != UPT_FATHOM) {
        /* the X1000's boot header, and HiBy's U-Boot (not Fathom's or Rockbox's bootloader) */
        static const uint8_t head[8] = {0x06, 0x05, 0x04, 0x03, 0x02, 0x55, 0xaa, 0x55};
        uint8_t h[8];
        if (read_at(u->fd, h, sizeof h, (uint64_t)uboot->lba * SECTOR)) return UPT_ERR_DAMAGED;
        if (memcmp(h, head, sizeof h)) return UPT_ERR_NO_BOOTLOADER;
        if ((e = hash_file(u, uboot, u->uboot_md5, u->uboot_built, sizeof u->uboot_built, &pg))) return e;
        if (!u->uboot_built[0]) return UPT_ERR_NO_BOOTLOADER;
        for (int i = 0; i < u->nentries; i++)
            if (entry_file(u, &u->entries[i]) == uboot && strcmp(u->entries[i].md5, u->uboot_md5))
                return UPT_ERR_DAMAGED;
    }
    u->checked = 1;
    return UPT_OK;
}

upt_err_t upt_check(upt_t *u, void (*progress)(void *ctx, uint64_t done, uint64_t total), void *ctx)
{
    prog_t pg = {progress, ctx, 0, 0, malloc(64 + CHUNK)};
    if (!pg.buf) return UPT_ERR_OPEN;
    upt_err_t e = check(u, &pg);
    free(pg.buf);
    return e;
}

/* ---------- patch and undo ---------- */

/* The undo file: magic, sector, old and new sizes, the records, then the
 * sector as it was. Little-endian fields. */
typedef struct {
    uint32_t lba, old_size, new_size, nrec;
    uint64_t rec[UPT_MAX_TREES];
    uint8_t sector[SECTOR];
} undo_t;

#define UNDO_SIZE (8 + 4 * 4 + 8 * UPT_MAX_TREES + SECTOR)

static void put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void put64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static uint64_t le64(const uint8_t *p) { return le32(p) | (uint64_t)le32(p + 4) << 32; }

static int sync_dir_of(const char *path)
{
    char dir[1024];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) *slash = '\0';
    else snprintf(dir, sizeof dir, ".");
    int fd = open(slash && !dir[0] ? "/" : dir, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    int r = fsync(fd);
    close(fd);
    return r;
}

static upt_err_t undo_save(const char *undo_path, const undo_t *d)
{
    uint8_t b[UNDO_SIZE] = {0};
    memcpy(b, UNDO_MAGIC, 8);
    put32(b + 8, d->lba);
    put32(b + 12, d->old_size);
    put32(b + 16, d->new_size);
    put32(b + 20, d->nrec);
    for (int i = 0; i < UPT_MAX_TREES; i++) put64(b + 24 + 8 * i, d->rec[i]);
    memcpy(b + 24 + 8 * UPT_MAX_TREES, d->sector, SECTOR);

    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s.part", undo_path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return UPT_ERR_WRITE;
    int bad = write_at(fd, b, sizeof b, 0) || fsync(fd);
    if (close(fd)) bad = 1;
    if (bad || rename(tmp, undo_path) || sync_dir_of(undo_path)) {
        unlink(tmp);
        return UPT_ERR_WRITE;
    }
    return UPT_OK;
}

static upt_err_t undo_load(const char *undo_path, undo_t *d)
{
    uint8_t b[UNDO_SIZE + 1];
    int fd = open(undo_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return UPT_ERR_UNDO;
    ssize_t n = read(fd, b, sizeof b);
    close(fd);
    if (n != UNDO_SIZE || memcmp(b, UNDO_MAGIC, 8)) return UPT_ERR_UNDO;
    d->lba = le32(b + 8);
    d->old_size = le32(b + 12);
    d->new_size = le32(b + 16);
    d->nrec = le32(b + 20);
    if (!d->nrec || d->nrec > UPT_MAX_TREES) return UPT_ERR_UNDO;
    for (int i = 0; i < UPT_MAX_TREES; i++) d->rec[i] = le64(b + 24 + 8 * i);
    memcpy(d->sector, b + 24 + 8 * UPT_MAX_TREES, SECTOR);
    return UPT_OK;
}

/* Each record still names update.txt's sector, with one of the sizes. */
static int records_match(int fd, const undo_t *d)
{
    for (uint32_t i = 0; i < d->nrec; i++) {
        uint8_t r[34];
        uint32_t lba, size;
        if (read_at(fd, r, sizeof r, d->rec[i])) return 0;
        if (!both32(r + 2, &lba) || !both32(r + 10, &size)) return 0;
        if (lba != d->lba || (size != d->old_size && size != d->new_size)) return 0;
    }
    return 1;
}

static int write_sizes(int fd, const undo_t *d, uint32_t size)
{
    uint8_t v[8];
    put_both32(v, size);
    for (uint32_t i = 0; i < d->nrec; i++)
        if (write_at(fd, v, sizeof v, d->rec[i] + 10)) return -1;
    return fsync(fd);
}

/* The same entry as Rockbox's patch_manifest.pl, before the rest. */
static int patched_manifest(const upt_t *u, char *out, size_t cap)
{
    return snprintf(out, cap,
                    "bootloader={\n"
                    "        name=uboot\n"
                    "        file_path=autoupdate/uboot.bin\n"
                    "        md5=%s\n"
                    "}\n"
                    "%s",
                    u->uboot_md5, u->manifest);
}

upt_err_t upt_patch(upt_t *u, const char *undo_path)
{
    if (!u->checked) return UPT_ERR_NOT_UPDATE;
    if (u->kind == UPT_HIBY_PATCHED) return UPT_OK;
    if (u->kind != UPT_HIBY) return UPT_ERR_NOT_UPDATE;
    if (!u->writable) return UPT_ERR_WRITE;

    upt_file_t *f = (upt_file_t *)upt_file(u, "update.txt");
    char text[SECTOR + 1];
    int len = patched_manifest(u, text, sizeof text);
    if (!f || f->size > SECTOR || len < 0 || (size_t)len > SECTOR) return UPT_ERR_LAYOUT;
    for (int i = 0; i < u->nfiles; i++)         /* the sector is update.txt's alone */
        if (&u->files[i] != f && u->files[i].size && u->files[i].lba == f->lba) return UPT_ERR_LAYOUT;

    undo_t d = {f->lba, f->size, (uint32_t)len, (uint32_t)f->nrec, {0}, {0}};
    for (int i = 0; i < f->nrec; i++) d.rec[i] = f->rec[i];
    uint64_t at = (uint64_t)f->lba * SECTOR;
    if (read_at(u->fd, d.sector, SECTOR, at)) return UPT_ERR_DAMAGED;
    if (memcmp(d.sector, u->manifest, u->manifest_len) || !records_match(u->fd, &d)) return UPT_ERR_LAYOUT;

    upt_err_t e = undo_save(undo_path, &d);
    if (e) return e;
    /* the text, then its length: cut short in between, the undo file puts it back */
    uint8_t sec[SECTOR] = {0};
    memcpy(sec, text, (size_t)len);
    if (write_at(u->fd, sec, SECTOR, at) || fsync(u->fd) || write_sizes(u->fd, &d, (uint32_t)len))
        return UPT_ERR_WRITE;

    f->size = (uint32_t)len;
    memcpy(u->manifest, text, (size_t)len + 1);
    u->manifest_len = (size_t)len;
    return parse_manifest(u) ? UPT_ERR_LAYOUT : UPT_OK;
}

upt_err_t upt_undo(const char *path, const char *undo_path)
{
    undo_t d;
    upt_err_t e = undo_load(undo_path, &d);
    if (e) return e;
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) return UPT_ERR_OPEN;
    if (!records_match(fd, &d)) {
        close(fd);
        return UPT_ERR_UNDO;
    }
    int bad = write_at(fd, d.sector, SECTOR, (uint64_t)d.lba * SECTOR) || fsync(fd) ||
              write_sizes(fd, &d, d.old_size);
    if (close(fd)) bad = 1;
    if (bad) return UPT_ERR_WRITE;
    unlink(undo_path);
    sync_dir_of(undo_path);
    return UPT_OK;
}

const char *upt_error_text(upt_err_t e)
{
    switch (e) {
    case UPT_OK: return "";
    case UPT_ERR_OPEN: return N_("Couldn't open the update file");
    case UPT_ERR_NOT_UPDATE: return N_("Not a HiBy update file");
    case UPT_ERR_MODEL: return N_("This update is for another player");
    case UPT_ERR_DAMAGED: return N_("The update file is damaged");
    case UPT_ERR_NO_BOOTLOADER: return N_("The update file has no HiBy bootloader");
    case UPT_ERR_LAYOUT: return N_("Fathom can't change this update file");
    case UPT_ERR_WRITE: return N_("Couldn't write to the card");
    case UPT_ERR_UNDO: return N_("Couldn't undo the change to the update file");
    default: return N_("Couldn't use the update file");
    }
}
