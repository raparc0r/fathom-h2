/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* Updates over Wi-Fi: versions, GitHub's answer, staging, installing (update.h). */
#include "net/update.h"
#include "core/fsafe.h"
#include "core/lang.h"   /* N_(): the texts the UI translates */

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "net/json.h"
#include "net/sha256.h"
#include "net/unzip.h"

/* ---------- versions ---------- */

int update_parse_version(const char *s, int v[3])
{
    if (!s) return -1;
    if (*s == 'v' || *s == 'V') s++;
    for (int i = 0; i < 3; i++) {
        if (!isdigit((unsigned char)*s)) return -1;
        long n = 0;
        while (isdigit((unsigned char)*s)) {
            n = n * 10 + (*s++ - '0');
            if (n > 99999) return -1;
        }
        v[i] = (int)n;
        if (i < 2 && *s++ != '.') return -1;
    }
    if (!*s || *s == '+') return 0;                 /* (build metadata changes nothing) */
    if (*s != '-') return -1;
    if (!strcmp(s, "-dirty")) return 0;
    /* git describe past the tag: -N-gHASH, perhaps -dirty after it */
    const char *p = s + 1;
    if (isdigit((unsigned char)*p)) {
        while (isdigit((unsigned char)*p)) p++;
        if (p[0] == '-' && p[1] == 'g' && isxdigit((unsigned char)p[2])) {
            p += 2;
            while (isxdigit((unsigned char)*p)) p++;
            if (!*p || !strcmp(p, "-dirty")) return 0;
        }
    }
    return 1;                                       /* -rc1, -beta...: a pre-release */
}

int update_is_newer(const char *latest, const char *running)
{
    int l[3], r[3];
    if (update_parse_version(latest, l) != 0) return 0;      /* never offer a pre-release */
    int pr = update_parse_version(running, r);
    if (pr < 0) return 1;
    int cmp = 0;
    for (int i = 0; i < 3 && !cmp; i++) cmp = (l[i] > r[i]) - (l[i] < r[i]);
    /* 0.8.0 is newer than 0.8.0-rc1 (running a pre-release) */
    return cmp > 0 || (cmp == 0 && pr == 1);
}

int update_auto_due(long now, long last_ok, long last_try)
{
    if (now < last_ok || now < last_try) return 1;           /* the clock went back */
    return now - last_ok >= UPDATE_AUTO_EVERY && now - last_try >= UPDATE_AUTO_RETRY;
}

/* ---------- GitHub's answer ---------- */

static int has_suffix(const char *s, const char *suf)
{
    size_t a = strlen(s), b = strlen(suf);
    return a >= b && !strcmp(s + a - b, suf);
}

int update_parse_release(const char *json, size_t len, update_release_t *r)
{
    memset(r, 0, sizeof *r);
    json_t root = json_root(json, len), v, assets, a;
    if (json_type(root) != JSON_OBJECT) return -1;
    if (!json_member(root, "tag_name", &v) || json_string(v, r->tag, sizeof r->tag) <= 0) return -1;
    if (json_member(root, "draft", &v) && json_bool(v, 0)) return -1;
    if (json_member(root, "prerelease", &v) && json_bool(v, 0)) return -1;
    int ver[3];
    if (update_parse_version(r->tag, ver) != 0) return -1;
    if (!json_member(root, "assets", &assets) || json_type(assets) != JSON_ARRAY) return -1;
    /* Each release carries the zip twice, byte for byte: fathom-h2-VERSION.zip
     * for people, OTA_fathom-h2-VERSION for this updater, so GitHub's download
     * counts tell the two apart. (0.12.0 and older know only the zip.) */
    const char *num = r->tag[0] == 'v' || r->tag[0] == 'V' ? r->tag + 1 : r->tag;
    char want[128], ota[128];
    snprintf(want, sizeof want, "fathom-h2-%s.zip", num);
    snprintf(ota, sizeof ota, "OTA_fathom-h2-%s", num);
    int best = 0;                                   /* 3: OTA_; 2: the exact zip; 1: some fathom-h2-*.zip */
    size_t at = 0;
    while (json_next(assets, &at, &a)) {
        char name[128], url[512], state[32] = "", digest[80] = "";
        if (!json_member(a, "name", &v) || json_string(v, name, sizeof name) <= 0) continue;
        if (strchr(name, '/')) continue;                              /* (it names a file in the stage) */
        int score = !strcmp(name, ota) ? 3 : !strcmp(name, want) ? 2 : !strncmp(name, "fathom-h2-", 10) && has_suffix(name, ".zip") ? 1 : 0;
        if (score <= best) continue;
        if (json_member(a, "state", &v)) json_string(v, state, sizeof state);
        if (state[0] && strcmp(state, "uploaded")) continue;          /* still uploading */
        if (!json_member(a, "browser_download_url", &v) || json_string(v, url, sizeof url) <= 0) continue;
        long long size = json_member(a, "size", &v) ? json_int(v, -1) : -1;
        if (size <= 0) continue;
        if (json_member(a, "digest", &v)) json_string(v, digest, sizeof digest);
        int has_digest = !strncmp(digest, "sha256:", 7) && strlen(digest) == 7 + 64;
        /* Over HTTPS; plain http only with GitHub's digest to check it by
         * (a release served from a computer, for testing) */
        if (strncmp(url, "https://", 8) && !(has_digest && !strncmp(url, "http://", 7))) continue;
        best = score;
        snprintf(r->asset, sizeof r->asset, "%s", name);
        snprintf(r->url, sizeof r->url, "%s", url);
        r->size = size;
        r->sha256[0] = '\0';
        if (has_digest) {
            for (int i = 0; i < 64; i++) r->sha256[i] = (char)tolower((unsigned char)digest[7 + i]);
            r->sha256[64] = '\0';
        }
    }
    return best ? 0 : -1;
}

const char *update_error_text(int err)
{
    switch (err) {
    case UPDATE_OK: return "";
    case UPDATE_ERR_NET: return N_("Couldn't reach GitHub");
    case UPDATE_ERR_ANSWER: return N_("No update file in the latest release");
    case UPDATE_ERR_SPACE: return N_("Not enough space on the card");
    case UPDATE_ERR_DOWNLOAD: return N_("The download failed");
    case UPDATE_ERR_DAMAGED: return N_("The download is damaged");
    case UPDATE_ERR_ZIP: return N_("Couldn't unpack the update");
    case UPDATE_ERR_WRITE: return N_("Couldn't write to the card");
    case UPDATE_ERR_INSTALL: return N_("Couldn't install the update");
    case UPDATE_ERR_CANCELLED: return N_("Update cancelled");
    default: return N_("Couldn't update");
    }
}

/* ---------- files ---------- */

int update_file_sha256(const char *path, char hex[65], http_progress_fn progress, void *ctx,
                       const volatile int *abort)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    struct stat st;
    uint64_t total = fstat(fd, &st) == 0 ? (uint64_t)st.st_size : 0, done = 0;
    unsigned char *b = malloc(32768);
    if (!b) { close(fd); return -1; }
    sha256_t s;
    sha256_init(&s);
    ssize_t k;
    int ok = 1;
    while ((k = read(fd, b, 32768)) > 0) {
        if (abort && *abort) { ok = 0; break; }
        sha256_update(&s, b, (size_t)k);
        done += (uint64_t)k;
        if ((done & 0xFFFFF) < 32768) {             /* each MB */
            file_drop_cache(fd, 0, 0);
            if (progress) progress(done, total, ctx);
        }
    }
    if (k < 0) ok = 0;
    file_drop_cache(fd, 0, 0);
    close(fd);
    free(b);
    if (!ok) return -1;
    uint8_t d[32];
    sha256_final(&s, d);
    sha256_hex(d, hex);
    if (progress) progress(done, total, ctx);
    return 0;
}

int64_t update_free_space(const char *path, long *cluster)
{
    struct statvfs st;
    if (statvfs(path, &st)) return -1;
    if (cluster) *cluster = st.f_bsize ? (long)st.f_bsize : 32768;
    return (int64_t)st.f_bavail * (int64_t)st.f_frsize;
}

/* The names in a folder (not . and ..), malloc'd: their count, -1. Read whole
 * first, so renaming and removing never happens under readdir. */
static int list_dir(const char *path, char ***out)
{
    *out = NULL;
    DIR *d = opendir(path);
    if (!d) return -1;
    int n = 0, cap = 0;
    char **names = NULL;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        if (n == cap) {
            char **g = realloc(names, sizeof *names * (size_t)(cap = cap ? cap * 2 : 16));
            if (!g) break;
            names = g;
        }
        if (!(names[n] = strdup(de->d_name))) break;
        n++;
    }
    closedir(d);
    *out = names;
    return n;
}

static void free_names(char **names, int n)
{
    for (int i = 0; i < n; i++) free(names[i]);
    free(names);
}

int update_remove_tree(const char *dir)
{
    struct stat st;
    if (lstat(dir, &st)) return errno == ENOENT ? 0 : -1;
    if (!S_ISDIR(st.st_mode)) return unlink(dir);
    char **names;
    int n = list_dir(dir, &names), bad = n < 0;
    for (int i = 0; i < n; i++) {
        char p[1024];
        snprintf(p, sizeof p, "%s/%s", dir, names[i]);
        if (update_remove_tree(p)) bad = 1;
    }
    free_names(names, n > 0 ? n : 0);
    return rmdir(dir) || bad ? -1 : 0;
}

/* mkdir -p of path's folders (the last component too if it ends in '/'). */
static int make_dirs(const char *path)
{
    char p[1024];
    snprintf(p, sizeof p, "%s", path);
    for (char *s = p + 1; *s; s++) {
        if (*s != '/') continue;
        *s = '\0';
        if (mkdir(p, 0755) && errno != EEXIST) return -1;
        *s = '/';
    }
    return 0;
}

static int write_marker(const char *path, const char *text)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return -1;
    size_t n = strlen(text);
    int ok = write(fd, text, n) == (ssize_t)n && fsync(fd) == 0;
    return close(fd) || !ok ? -1 : 0;
}

/* ---------- the zip's layout ---------- */

/* A name that's safe to write on the card: relative, no . or .. parts, nothing
 * FAT refuses. */
static int safe_name(const char *s)
{
    if (!*s || *s == '/') return 0;
    for (const char *c = s; *c; c++)
        if ((unsigned char)*c < 0x20 || strchr("\\:*?\"<>|", *c)) return 0;
    for (const char *c = s; *c;) {
        const char *e = strchr(c, '/');
        size_t n = e ? (size_t)(e - c) : strlen(c);
        if ((n == 0 && e && e[1]) || (n == 1 && c[0] == '.') || (n == 2 && c[0] == '.' && c[1] == '.')) return 0;
        if (!e) break;
        c = e + 1;
    }
    return 1;
}

/* The zip's top folder ("fathom-h2-0.8.0/", every entry inside it) into top. */
static int zip_top(zip_t *z, char *top, size_t cap)
{
    zip_entry_t e;
    int r;
    top[0] = '\0';
    zip_rewind(z);
    while ((r = zip_next(z, &e)) == 1) {
        if (!safe_name(e.name)) return -1;
        const char *slash = strchr(e.name, '/');
        if (!slash) return -1;                      /* a file at the top: not a release */
        size_t n = (size_t)(slash - e.name) + 1;
        if (!top[0]) {
            if (n >= cap) return -1;
            memcpy(top, e.name, n);
            top[n] = '\0';
        } else if (strncmp(e.name, top, strlen(top))) {
            return -1;
        }
    }
    zip_rewind(z);
    return r == 0 && top[0] ? 0 : -1;
}

int64_t update_stage_needs(const char *zip_path, long cluster, int64_t kernel_size)
{
    zip_t z;
    char top[160], sd[200];
    if (zip_open(&z, zip_path)) return -1;
    if (zip_top(&z, top, sizeof top)) { zip_close(&z); return -1; }
    snprintf(sd, sizeof sd, "%ssd/", top);
    size_t sl = strlen(sd);
    if (cluster <= 0) cluster = 32768;
    int64_t need = 0;
    zip_entry_t e;
    while (zip_next(&z, &e) == 1)
        if (!strncmp(e.name, sd, sl))       /* a folder takes a cluster for its entries */
            need += e.dir ? cluster : ((int64_t)e.usize + cluster - 1) / cluster * cluster;
    zip_close(&z);
    return need + (kernel_size + cluster - 1) / cluster * cluster + (1 << 20);   /* + uImage.prev, a margin */
}

/* ---------- staging ---------- */

typedef struct {
    char *path;             /* after "./sd/" */
    char *hash;
    int seen;
} sum_t;

typedef struct {
    int fd;
    sha256_t sha;
    int werr;
    uint64_t since_sync;
    uint64_t *done, total;
    http_progress_fn progress;
    void *ctx;
} sink_t;

static int to_file(const void *data, size_t len, void *ctx)
{
    sink_t *s = ctx;
    const char *p = data;
    for (size_t left = len; left;) {
        ssize_t k = write(s->fd, p, left);
        if (k <= 0) { s->werr = 1; return -1; }
        p += k;
        left -= (size_t)k;
    }
    sha256_update(&s->sha, data, len);
    /* A few MB at a time to the card, not 30 MB of dirty pages at once: the
     * music is read from the same card meanwhile. */
    if ((s->since_sync += len) >= (2u << 20)) {
        s->since_sync = 0;
        if (fdatasync(s->fd)) { s->werr = 1; return -1; }
        file_drop_cache(s->fd, 0, 0);
    }
    *s->done += len;
    if (s->progress) s->progress(*s->done, s->total, s->ctx);
    return 0;
}

typedef struct { char *buf; size_t n, cap; } mem_t;

static int to_mem(const void *data, size_t len, void *ctx)
{
    mem_t *m = ctx;
    if (m->n + len + 1 > m->cap) return -1;
    memcpy(m->buf + m->n, data, len);
    m->n += len;
    m->buf[m->n] = '\0';
    return 0;
}

/* SHA256SUMS ("HASH  ./PATH" lines, as shasum writes them): the sd/ ones, in
 * place in text. Their count, -1. */
static int parse_sums(char *text, sum_t **out)
{
    int n = 0, cap = 0;
    sum_t *s = NULL;
    for (char *line = text; line && *line;) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        size_t len = strlen(line);
        if (len && line[len - 1] == '\r') line[--len] = '\0';
        int hex = 1;
        for (int i = 0; i < 64 && hex; i++) hex = i < (int)len && isxdigit((unsigned char)line[i]);
        if (hex && len > 66 && line[64] == ' ' && (line[65] == ' ' || line[65] == '*')) {
            char *path = line + 66;
            if (!strncmp(path, "./", 2)) path += 2;
            if (!strncmp(path, "sd/", 3) && path[3]) {
                if (n == cap) {
                    sum_t *g = realloc(s, sizeof *s * (size_t)(cap = cap ? cap * 2 : 64));
                    if (!g) { free(s); return -1; }
                    s = g;
                }
                line[64] = '\0';
                for (int i = 0; i < 64; i++) line[i] = (char)tolower((unsigned char)line[i]);
                s[n++] = (sum_t){.path = path + 3, .hash = line};
            }
        }
        line = nl ? nl + 1 : NULL;
    }
    *out = s;
    return n;
}

int update_stage(const char *zip_path, const char *stage, http_progress_fn progress, void *ctx,
                 const volatile int *abort)
{
    zip_t z;
    char top[160], sd[200], name[256], path[1024];
    zip_entry_t e;
    if (zip_open(&z, zip_path)) return UPDATE_ERR_ZIP;
    if (zip_top(&z, top, sizeof top)) { zip_close(&z); return UPDATE_ERR_ZIP; }
    snprintf(sd, sizeof sd, "%ssd/", top);
    snprintf(name, sizeof name, "%sabout/SHA256SUMS", top);
    size_t sl = strlen(sd);

    /* The checksums, and how much there is to unpack */
    int err = UPDATE_ERR_ZIP, found = 0;
    uint64_t total = 0, done = 0;
    mem_t m = {malloc(256 << 10), 0, 256 << 10};   /* (0.7.0's is 16 KB) */
    sum_t *sums = NULL;
    int nsums = 0;
    if (!m.buf) goto out;
    m.buf[0] = '\0';                                /* (an empty SHA256SUMS writes nothing: it was read as it came from malloc) */
    while (zip_next(&z, &e) == 1) {
        if (!strncmp(e.name, sd, sl) && !e.dir) total += e.usize;
        if (!strcmp(e.name, name) && !found) {
            found = 1;
            zip_entry_t sums_e = e;
            uint32_t pos = z.next;
            unsigned seen = z.seen;
            if (zip_extract(&z, &sums_e, to_mem, &m, abort)) goto out;
            z.next = pos;                           /* (carry on through the directory) */
            z.seen = seen;
        }
    }
    if (!found || (nsums = parse_sums(m.buf, &sums)) <= 0) { err = UPDATE_ERR_DAMAGED; goto out; }

    /* A fresh stage */
    snprintf(path, sizeof path, "%s/sd", stage);
    update_remove_tree(path);
    snprintf(path, sizeof path, "%s/READY", stage);
    unlink(path);
    snprintf(path, sizeof path, "%s/sd/", stage);
    if (make_dirs(path)) { err = UPDATE_ERR_WRITE; goto out; }

    int have_kernel = 0, have_player = 0;
    zip_rewind(&z);
    while (zip_next(&z, &e) == 1) {
        if (strncmp(e.name, sd, sl) || !e.name[sl]) continue;
        const char *rel = e.name + sl;
        snprintf(path, sizeof path, "%s/sd/%s", stage, rel);
        if (e.dir) {
            if (make_dirs(path)) { err = UPDATE_ERR_WRITE; goto out; }
            continue;
        }
        sum_t *s = NULL;
        for (int i = 0; i < nsums && !s; i++)
            if (!strcmp(sums[i].path, rel)) s = &sums[i];
        if (!s || s->seen) { err = UPDATE_ERR_DAMAGED; goto out; }   /* a file nobody vouches for */
        if (make_dirs(path)) { err = UPDATE_ERR_WRITE; goto out; }
        sink_t k = {.fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644), .done = &done, .total = total,
                    .progress = progress, .ctx = ctx};
        if (k.fd < 0) { err = UPDATE_ERR_WRITE; goto out; }
        sha256_init(&k.sha);
        int r = zip_extract(&z, &e, to_file, &k, abort);
        if (!r && fsync(k.fd)) { r = -1; k.werr = 1; }
        file_drop_cache(k.fd, 0, 0);
        if (close(k.fd)) { r = -1; k.werr = 1; }
        if (abort && *abort) { err = UPDATE_ERR_CANCELLED; goto out; }
        if (r) { err = k.werr ? UPDATE_ERR_WRITE : UPDATE_ERR_DAMAGED; goto out; }
        uint8_t d[32];
        char hex[65];
        sha256_final(&k.sha, d);
        sha256_hex(d, hex);
        if (strcmp(hex, s->hash)) { err = UPDATE_ERR_DAMAGED; goto out; }
        s->seen = 1;
        have_kernel |= !strcmp(rel, "uImage");
        have_player |= !strcmp(rel, "fathom/bin/fathom-player");
    }
    for (int i = 0; i < nsums; i++)
        if (!sums[i].seen) { err = UPDATE_ERR_DAMAGED; goto out; }   /* listed, not in the zip */
    if (!have_kernel || !have_player) { err = UPDATE_ERR_DAMAGED; goto out; }
    snprintf(path, sizeof path, "%s/READY", stage);
    top[strlen(top) - 1] = '\0';
    err = write_marker(path, top) ? UPDATE_ERR_WRITE : UPDATE_OK;
out:
    if (err != UPDATE_OK && abort && *abort) err = UPDATE_ERR_CANCELLED;
    free(sums);
    free(m.buf);
    zip_close(&z);
    return err;
}

/* ---------- installing ---------- */

/* Every staged file under from/rel renamed to card/rel, folders made as
 * needed, the staged folders removed as they empty; the kernel (uImage at
 * the top) is left for last. */
static int move_tree(const char *from, const char *card, const char *rel)
{
    char src[1024], dst[1024];
    snprintf(src, sizeof src, "%s%s%s", from, *rel ? "/" : "", rel);
    char **names;
    int n = list_dir(src, &names), bad = n < 0;
    for (int i = 0; i < n && !bad; i++) {
        char sub[1024];
        snprintf(sub, sizeof sub, "%s%s%s", rel, *rel ? "/" : "", names[i]);
        if (!*rel && !strcmp(names[i], "uImage")) continue;
        snprintf(src, sizeof src, "%s/%s", from, sub);
        snprintf(dst, sizeof dst, "%s/%s", card, sub);
        struct stat st;
        if (lstat(src, &st)) { bad = 1; break; }
        if (S_ISDIR(st.st_mode)) {
            if (mkdir(dst, 0755) && errno != EEXIST) { bad = 1; break; }
            if (move_tree(from, card, sub)) { bad = 1; break; }
            rmdir(src);
        } else if (rename(src, dst)) {
            fprintf(stderr, "update: can't install %s: %s\n", sub, strerror(errno));
            bad = 1;
        }
    }
    free_names(names, n > 0 ? n : 0);
    return bad ? -1 : 0;
}

/* from copied to to, by way of to.part (so to is whole or as it was).
 * errno is the failure's (ENOSPC for a full card). */
static int copy_file(const char *from, const char *to)
{
    char part[1024];
    snprintf(part, sizeof part, "%s.part", to);
    int in = open(from, O_RDONLY | O_CLOEXEC), out = in < 0 ? -1 : open(part, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (out < 0) { int e = errno; if (in >= 0) close(in); errno = e; return -1; }
    char *b = malloc(32768);
    ssize_t k = -1;
    int ok = b != NULL, why = b ? 0 : ENOMEM;
    while (ok && (k = read(in, b, 32768)) > 0) {
        ssize_t w = write(out, b, (size_t)k);
        ok = w == k;
        if (!ok) why = w < 0 ? errno : ENOSPC;      /* (a short write: the card is full) */
    }
    if (ok && k < 0) { ok = 0; why = errno; }
    if (ok && fsync(out)) { ok = 0; why = errno; }
    free(b);
    close(in);
    if (close(out) && ok) { ok = 0; why = errno; }
    if (ok && rename(part, to)) { ok = 0; why = errno; }
    if (!ok) { unlink(part); errno = why; return -1; }
    return 0;
}

/* The card's Wi-Fi modules copied to modules.prev, beside uImage.prev: the
 * old kernel can only load its own, and h2boot puts both back if the new
 * kernel won't start (bootloader/fathom/bootpick.c). */
static int keep_modules(const char *mods, const char *prev)
{
    update_remove_tree(prev);
    if (mkdir(prev, 0755)) return -1;
    char **names;
    int n = list_dir(mods, &names), bad = n < 0;
    for (int i = 0; i < n && !bad; i++) {
        char a[1024], b[1024];
        snprintf(a, sizeof a, "%s/%s", mods, names[i]);
        snprintf(b, sizeof b, "%s/%s", prev, names[i]);
        bad = copy_file(a, b) != 0;
    }
    free_names(names, n > 0 ? n : 0);
    return bad ? -1 : 0;
}

/* Whether the INSTALLING marker has this word as one of its lines. */
static int marker_has(const char *path, const char *word)
{
    char buf[128];
    FILE *f = fopen(path, "re");
    int has = 0;
    while (f && !has && fgets(buf, sizeof buf, f)) {
        buf[strcspn(buf, "\r\n")] = '\0';
        has = !strcmp(buf, word);
    }
    if (f) fclose(f);
    return has;
}

int update_install(const char *card, const char *stage)
{
    char sd[1024], mark[1024], kernel[1024], now[1024], prev[1024];
    char mods[1024], mods_prev[1024], new_mods[1024], boot_new[1024];
    snprintf(sd, sizeof sd, "%s/sd", stage);
    snprintf(mark, sizeof mark, "%s/INSTALLING", stage);
    snprintf(kernel, sizeof kernel, "%s/sd/uImage", stage);
    snprintf(now, sizeof now, "%s/uImage", card);
    snprintf(prev, sizeof prev, "%s/uImage.prev", card);
    snprintf(mods, sizeof mods, "%s/fathom/wifi/modules", card);
    snprintf(mods_prev, sizeof mods_prev, "%s/fathom/wifi/modules.prev", card);
    snprintf(new_mods, sizeof new_mods, "%s/sd/fathom/wifi/modules", stage);
    snprintf(boot_new, sizeof boot_new, "%s/fathom/boot-new", card);
    int kept = 0;
    if (!file_exists(mark)) {
        char ready[1024];
        snprintf(ready, sizeof ready, "%s/READY", stage);
        if (!file_exists(ready)) return UPDATE_ERR_INSTALL;
        /* The old kernel kept for h2boot's way back, and for a person to go
         * back to by hand (the bootloader always starts uImage). Copied, not
         * moved: uImage never goes missing. Before anything moves: a card
         * that filled since the download (music copied over USB) fails here,
         * with nothing installed, where the copy made after the files had
         * moved left the new player with the old kernel for good. */
        if (file_exists(kernel) && file_exists(now)) {
            if (copy_file(now, prev)) {
                int full = errno == ENOSPC || errno == EDQUOT;
                fprintf(stderr, "update: can't keep the old kernel: %s\n", strerror(errno));
                return full ? UPDATE_ERR_SPACE : UPDATE_ERR_INSTALL;
            }
            kept = 1;
        }
        /* (once, before anything moves: cut short, the card's are the new ones) */
        if (file_exists(kernel) && file_exists(new_mods) && file_exists(mods) && keep_modules(mods, mods_prev)) {
            int full = errno == ENOSPC || errno == EDQUOT;
            fprintf(stderr, "update: can't keep the old modules: %s\n", strerror(errno));
            return full ? UPDATE_ERR_SPACE : UPDATE_ERR_INSTALL;
        }
        if (write_marker(mark, kept ? "installing\nkept\n" : "installing\n")) return UPDATE_ERR_INSTALL;
    } else {
        kept = marker_has(mark, "kept");
    }
    sync();
    /* Everything but the kernel: the new modules can't be loaded by the old
     * kernel, but nothing loads them before the restart, and a cut-short
     * install is finished at the next start (update_resume). (No sd/ left:
     * cut short while the stage was being removed, all of it installed.) */
    if (file_exists(sd) && move_tree(sd, card, "")) return UPDATE_ERR_INSTALL;
    sync();
    if (file_exists(kernel)) {
        /* An install begun by a player from before the copy moved up (its
         * marker doesn't say "kept"): made here, as that player would have. */
        if (!kept && file_exists(now) && copy_file(now, prev)) return UPDATE_ERR_INSTALL;
        sync();
        /* h2boot starts it twice at most without the player getting up (which
         * deletes this), then goes back to uImage.prev and modules.prev. What
         * an earlier fallback left goes. The marker before the kernel: the
         * power cut between the two then leaves the old kernel marked (it
         * starts, and the install is finished from here), where the other
         * way round it left a new kernel with nothing to go back from. */
        char failed[1024];
        snprintf(failed, sizeof failed, "%s/uImage.failed", card);
        unlink(failed);
        snprintf(failed, sizeof failed, "%s/fathom/wifi/modules.failed", card);
        update_remove_tree(failed);
        if (write_marker(boot_new, "a new kernel: h2boot tries it twice, then the one before\n"))
            fprintf(stderr, "update: can't mark the new kernel: %s\n", strerror(errno));
        if (file_replace(kernel, now)) {
            fprintf(stderr, "update: can't install uImage: %s\n", strerror(errno));
            return UPDATE_ERR_INSTALL;
        }
    }
    sync();
    update_remove_tree(sd);
    update_remove_tree(stage);                      /* (INSTALLING with it, last: done) */
    sync();
    /* A card that can't be written any more (made read-only after an error):
     * the marker still there isn't "installed, restart". The player restarts
     * the H2 after an install finished at its start, and with the marker
     * left it did so at every start, for ever. */
    if (file_exists(mark)) {
        fprintf(stderr, "update: the stage can't be removed: %s\n", strerror(errno));
        return UPDATE_ERR_INSTALL;
    }
    return UPDATE_OK;
}

int update_install_begun(const char *stage)
{
    char mark[1024];
    snprintf(mark, sizeof mark, "%s/INSTALLING", stage);
    return file_exists(mark);
}

int update_resume(const char *card, const char *stage)
{
    char mark[1024];
    snprintf(mark, sizeof mark, "%s/INSTALLING", stage);
    if (file_exists(mark)) {
        fprintf(stderr, "update: finishing an install cut short\n");
        return update_install(card, stage) == UPDATE_OK ? 1 : -1;
    }
    if (file_exists(stage)) {
        fprintf(stderr, "update: removing an unfinished update\n");
        update_remove_tree(stage);
    }
    return 0;
}
