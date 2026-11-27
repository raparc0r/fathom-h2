/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* Uninstall Fathom, on the card (uninstall.h). */
#include "sys/uninstall.h"
#include "core/datetime.h"
#include "core/fsafe.h"

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
    if (file_exists(p)) return 1;
    at(p, sizeof p, card, DIR_ "/renames");
    if (file_exists(p)) return 1;
    at(p, sizeof p, card, DIR_ "/target");       /* (an Apply cut before it had changed anything: its note alone) */
    return file_exists(p);
}

static void forget(const char *card)
{
    char p[600];
    at(p, sizeof p, card, DIR_ "/undo");
    unlink(p);
    at(p, sizeof p, card, DIR_ "/renames");
    unlink(p);
    at(p, sizeof p, card, DIR_ "/target");
    unlink(p);
    at(p, sizeof p, card, DIR_);
    rmdir(p);
}

/* The HiBy file an Apply set out to change, by the name it had then (noted
 * before anything is written: target_note). "" if no note (an Apply by an
 * older Fathom). Until the last rename it still has that name, and nothing
 * else says which file the undo is for. */
static void target_read(const char *card, char *name, size_t cap)
{
    char p[600];
    name[0] = '\0';
    at(p, sizeof p, card, DIR_ "/target");
    FILE *f = fopen(p, "r");
    if (!f) return;
    if (fgets(name, (int)cap, f)) name[strcspn(name, "\r\n")] = '\0';
    else name[0] = '\0';
    fclose(f);
}

static int target_note(const char *card, const char *name)
{
    char p[600];
    at(p, sizeof p, card, DIR_ "/target");
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return -1;
    size_t n = strlen(name);
    int bad = write(fd, name, n) != (ssize_t)n || write(fd, "\n", 1) != 1 || fsync(fd);
    if (close(fd)) bad = 1;
    return bad ? -1 : 0;
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
    /* Not the file made ready. Either that was long ago (HiBy's recovery
     * installed it, the files were moved by hand: forgotten, below, the
     * files left as they are), or the Apply itself was cut short (the player
     * stopped in the second it takes): then HiBy's file is half changed, or
     * changed and not yet named update.upt, and the undo is the only way
     * back. Forgotten like the first, it was lost: the file couldn't be
     * put back, and Fathom's installer stayed set aside under another name.
     * The second is told by HiBy's file still being there under the name it
     * had, and either update.upt not there yet or that file not whole. */
    char name[256], hp[600];
    target_read(card, name, sizeof name);
    at(hp, sizeof hp, card, name[0] ? name : UPDATE);
    if (file_exists(hp)) {
        upt_err_t he = upt_open(&u, hp, 0);
        upt_close(&u);
        int renamed = strcmp(name[0] ? name : UPDATE, UPDATE) != 0;
        if (he || (renamed && !file_exists(p))) {
            uninstall_cancel(card);
            return 1;
        }
    }
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
    if (mkdir(p, 0755) && !file_exists(p)) return UNI_WRITE;
    at(undo, sizeof undo, card, DIR_ "/undo");
    int other = strcmp(name, UPDATE) != 0;
    if (other) {
        at(p, sizeof p, card, UPDATE);
        if (file_exists(p)) {
            char aside[600];
            at(aside, sizeof aside, card, ASIDE);
            if (file_exists(aside)) return UNI_IN_THE_WAY;
            if (journal_rename(card, UPDATE, ASIDE)) { uninstall_cancel(card); return UNI_WRITE; }
        }
    }
    if (target_note(card, name)) { uninstall_cancel(card); return UNI_WRITE; }   /* (which file the undo will be for) */
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
    /* the names back, the last first. HiBy's file: the one that was renamed
     * to update.upt, or, if the Apply never got that far, the one it noted
     * (it was taken for update.upt then: the undo was tried on Fathom's own
     * installer, refused, and thrown away with the rest). */
    char noted[256];
    target_read(card, noted, sizeof noted);
    const char *hiby = noted[0] ? noted : UPDATE;
    for (int i = n - 1; i >= 0; i--) {
        char a[600], b[600];
        at(a, sizeof a, card, to[i]);
        at(b, sizeof b, card, from[i]);
        if (file_exists(a) && !file_exists(b) && rename(a, b)) r = -1;
        if (!strcmp(to[i], UPDATE)) hiby = from[i];
    }
    /* then the file's contents */
    if (file_exists(undo)) {
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
int64_t uninstall_ver_time(const char *ver)
{
    int y, mo, d, h, mi, se, n = 0;
    if (sscanf(ver, "%4d-%2d-%2dT%2d:%2d:%2d%n", &y, &mo, &d, &h, &mi, &se, &n) != 6) return -1;
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || se > 60) return -1;
    int64_t t = dt_days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60 + se;
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
    case UNI_OLDER: return N_("Older than the player's firmware");
    default: return N_("Couldn't use the update file");
    }
}
