/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Uninstall Fathom, on the card (uninstall.h). */
#include "sys/uninstall.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "core/lang.h"

#define DIR_     "fathom/uninstall"
#define UPDATE   "update.upt"
#define ASIDE    "fathom-install.upt"
#define MAX_RENAMES 4

static void at(char *out, size_t cap, const char *card, const char *name)
{
    snprintf(out, cap, "%s/%s", card, name);
}

static int exists(const char *path)
{
    struct stat st;
    return !stat(path, &st);
}

static int is_upt(const char *name)
{
    size_t n = strlen(name);
    /* (not macOS's "._" files beside each one copied to a FAT card) */
    return n > 4 && strncmp(name, "._", 2) && !strcasecmp(name + n - 4, ".upt");
}

typedef struct {
    char found[256], bad[256];
    int n, update_is_hiby;
    upt_err_t first;
} finding_t;

/* The .upt files in card/sub ("" for the top), into f. */
static void look_in(const char *card, const char *sub, finding_t *f)
{
    char dir[600];
    if (sub[0]) snprintf(dir, sizeof dir, "%s/%s", card, sub);
    else snprintf(dir, sizeof dir, "%s", card);
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!is_upt(e->d_name)) continue;
        char rel[300], p[900];
        if (sub[0]) snprintf(rel, sizeof rel, "%s/%s", sub, e->d_name);
        else snprintf(rel, sizeof rel, "%s", e->d_name);
        at(p, sizeof p, card, rel);
        struct stat st;
        if (stat(p, &st) || !S_ISREG(st.st_mode)) continue;
        upt_t u;
        upt_err_t r = upt_open(&u, p, 0);
        upt_kind_t kind = u.kind;
        upt_close(&u);
        if (r) {
            if (!f->bad[0]) { snprintf(f->bad, sizeof f->bad, "%s", rel); f->first = r; }
            continue;
        }
        if (kind == UPT_FATHOM) continue;
        f->n++;
        if (!strcmp(rel, UPDATE)) f->update_is_hiby = 1;
        else if (!f->found[0]) snprintf(f->found, sizeof f->found, "%s", rel);
    }
    closedir(d);
}

uni_err_t uninstall_find(const char *card, char *name, size_t cap, upt_err_t *why)
{
    finding_t f = {.first = UPT_OK};
    look_in(card, "", &f);
    look_in(card, UNINSTALL_MUSIC, &f);     /* (where the network drive puts files) */
    char *found = f.found, *bad = f.bad;
    int n = f.n, update_is_hiby = f.update_is_hiby;
    upt_err_t first = f.first;
    if (why) *why = first;
    if (update_is_hiby) {
        snprintf(name, cap, "%s", UPDATE);
        return UNI_OK;
    }
    if (n > 1) return UNI_SEVERAL;
    if (n == 1) {
        snprintf(name, cap, "%s", found);
        return UNI_OK;
    }
    return bad[0] ? UNI_BAD : UNI_NONE;
}

int uninstall_pending(const char *card)
{
    char p[600];
    at(p, sizeof p, card, DIR_ "/undo");
    if (exists(p)) return 1;
    at(p, sizeof p, card, DIR_ "/renames");
    return exists(p);
}

static void forget(const char *card)
{
    char p[600];
    at(p, sizeof p, card, DIR_ "/undo");
    unlink(p);
    at(p, sizeof p, card, DIR_ "/renames");
    unlink(p);
    at(p, sizeof p, card, DIR_);
    rmdir(p);
}

int uninstall_forget_stale(const char *card)
{
    if (!uninstall_pending(card)) return 0;
    char p[600];
    at(p, sizeof p, card, UPDATE);
    upt_t u;
    upt_err_t e = upt_open(&u, p, 0);
    upt_kind_t kind = u.kind;
    upt_close(&u);
    if (!e && kind == UPT_HIBY_PATCHED) return 0;
    forget(card);
    return 1;
}

uni_err_t uninstall_space(const char *card, uint64_t file_size, int *need_mb)
{
    struct statvfs v;
    uint64_t need = file_size + (uint64_t)UNINSTALL_SPARE_MB * 1024 * 1024;
    *need_mb = (int)((need + 1024 * 1024 - 1) / (1024 * 1024));
    if (statvfs(card, &v)) return UNI_OK;       /* (can't tell: HiBy's updater will) */
    return (uint64_t)v.f_bavail * v.f_frsize < need ? UNI_NO_SPACE : UNI_OK;
}

/* A rename, recorded first (and on the card) so cancelling can undo it. */
static int journal_rename(const char *card, const char *from, const char *to)
{
    char rec[600], a[600], b[600];
    at(rec, sizeof rec, card, DIR_ "/renames");
    int fd = open(rec, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0) return -1;
    char line[600];
    int n = snprintf(line, sizeof line, "%s\t%s\n", from, to);
    int bad = n <= 0 || (size_t)n >= sizeof line || write(fd, line, (size_t)n) != n || fsync(fd);
    if (close(fd)) bad = 1;
    if (bad) return -1;
    at(a, sizeof a, card, from);
    at(b, sizeof b, card, to);
    if (rename(a, b)) return -1;
    char dir[600];
    snprintf(dir, sizeof dir, "%s", card);
    int dfd = open(dir, O_RDONLY | O_CLOEXEC);
    if (dfd >= 0) { fsync(dfd); close(dfd); }
    return 0;
}

uni_err_t uninstall_apply(const char *card, const char *name, upt_t *u, upt_err_t *why)
{
    char p[600], undo[600];
    if (why) *why = UPT_OK;
    at(p, sizeof p, card, DIR_);
    if (mkdir(p, 0755) && !exists(p)) return UNI_WRITE;
    at(undo, sizeof undo, card, DIR_ "/undo");
    int other = strcmp(name, UPDATE) != 0;
    if (other) {
        at(p, sizeof p, card, UPDATE);
        if (exists(p)) {
            char aside[600];
            at(aside, sizeof aside, card, ASIDE);
            if (exists(aside)) return UNI_IN_THE_WAY;
            if (journal_rename(card, UPDATE, ASIDE)) { uninstall_cancel(card); return UNI_WRITE; }
        }
    }
    upt_err_t e = upt_patch(u, undo);
    if (e) {
        if (why) *why = e;
        uninstall_cancel(card);
        return UNI_BAD;
    }
    if (other && journal_rename(card, name, UPDATE)) { uninstall_cancel(card); return UNI_WRITE; }
    return UNI_OK;
}

int uninstall_cancel(const char *card)
{
    char rec[600], undo[600];
    at(rec, sizeof rec, card, DIR_ "/renames");
    at(undo, sizeof undo, card, DIR_ "/undo");
    char from[MAX_RENAMES][256], to[MAX_RENAMES][256];
    int n = 0, r = 0;
    FILE *f = fopen(rec, "r");
    if (f) {
        char line[600];
        while (n < MAX_RENAMES && fgets(line, sizeof line, f)) {
            char *tab = strchr(line, '\t'), *nl = strchr(line, '\n');
            if (!tab || !nl) continue;
            *tab = '\0';
            *nl = '\0';
            snprintf(from[n], sizeof from[n], "%s", line);
            snprintf(to[n], sizeof to[n], "%s", tab + 1);
            n++;
        }
        fclose(f);
    }
    /* the names back, the last first */
    const char *hiby = UPDATE;
    for (int i = n - 1; i >= 0; i--) {
        char a[600], b[600];
        at(a, sizeof a, card, to[i]);
        at(b, sizeof b, card, from[i]);
        if (exists(a) && !exists(b) && rename(a, b)) r = -1;
        if (!strcmp(to[i], UPDATE)) hiby = from[i];
    }
    /* then the file's contents */
    if (exists(undo)) {
        char p[600];
        at(p, sizeof p, card, hiby);
        if (upt_undo(p, undo)) r = -1;
    }
    forget(card);
    return r;
}

int64_t uninstall_stock_time(const char *card)
{
    char p[600], line[128];
    at(p, sizeof p, card, "fathom/stock-version");
    FILE *f = fopen(p, "r");
    if (!f) return 0;
    long long t = 0;
    while (fgets(line, sizeof line, f))
        if (!strncmp(line, "time=", 5)) t = strtoll(line + 5, NULL, 10);
    fclose(f);
    return t > 0 ? (int64_t)t : 0;
}

/* Days from 1970-01-01 to y-m-d (proleptic Gregorian). */
static int64_t days_from_civil(int64_t y, int m, int d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

int64_t uninstall_ver_time(const char *ver)
{
    int y, mo, d, h, mi, se, n = 0;
    if (sscanf(ver, "%4d-%2d-%2dT%2d:%2d:%2d%n", &y, &mo, &d, &h, &mi, &se, &n) != 6) return -1;
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || se > 60) return -1;
    int64_t t = days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60 + se;
    const char *z = ver + n;
    int oh, om;
    if ((z[0] == '+' || z[0] == '-') && sscanf(z + 1, "%2d:%2d", &oh, &om) == 2)
        t -= (z[0] == '+' ? 1 : -1) * (int64_t)(oh * 3600 + om * 60);
    return t;
}

uni_err_t uninstall_version_ok(const char *card, const char *ver)
{
    int64_t stock = uninstall_stock_time(card), rel = uninstall_ver_time(ver);
    if (!stock || rel < 0) return UNI_OK;
    return rel < stock - 86400 ? UNI_OLDER : UNI_OK;
}

const char *uninstall_error_text(uni_err_t e)
{
    switch (e) {
    case UNI_OK: return "";
    case UNI_NONE: return N_("No HiBy update file on the card");
    case UNI_SEVERAL: return N_("More than one HiBy update file on the card");
    case UNI_BAD: return N_("Couldn't use the update file");
    case UNI_IN_THE_WAY: return N_("Move update.upt and fathom-install.upt off the card first");
    case UNI_NO_SPACE: return N_("Not enough space on the card");
    case UNI_WRITE: return N_("Couldn't write to the card");
    case UNI_OLDER: return N_("Older than the H2's firmware");
    default: return N_("Couldn't use the update file");
    }
}
