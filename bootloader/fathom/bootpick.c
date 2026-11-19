/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * h2boot (Fathom): going back to the previous kernel when an update's won't
 * start.
 *
 * An update over Wi-Fi (app/net/update.c) keeps the kernel it replaces as
 * /uImage.prev and its Wi-Fi modules as /fathom/wifi/modules.prev (the old
 * kernel can load only its own), and leaves /fathom/boot-new. At each start
 * that marker moves on, boot-new to boot-try1 to boot-try2; the player
 * deletes it once it's up. So a third start that finds boot-try2 means the
 * new kernel twice didn't get as far as the player (a panic restarts the H2
 * by itself; a hang needs Power held): the kernel and modules from before go
 * back in place, the new ones kept as .failed, and /fathom/boot-fell-back
 * tells the player to say so. Two tries, so that the power going during the
 * first start doesn't undo a good update.
 *
 * Without the markers (a card copied by hand) nothing changes: /uImage.
 *
 * Written against the calls Rockbox's file API and POSIX share, so the same
 * code runs in tests/test_bootpick.c (BOOTPICK_HOST).
 */
#include "bootpick.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#ifdef BOOTPICK_HOST
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#define BP_CREATE(p) open((p), O_WRONLY | O_CREAT | O_TRUNC, 0644)
static char bp_root[200];
void fathom_boot_root(const char *root) { snprintf(bp_root, sizeof bp_root, "%s", root); }
#else
#include "file.h"
#include "dir.h"
#define BP_CREATE(p) open((p), O_WRONLY | O_CREAT | O_TRUNC)
static const char bp_root[] = "";
#endif

#define NEW   "/fathom/boot-new"
#define TRY1  "/fathom/boot-try1"
#define TRY2  "/fathom/boot-try2"
#define FELL  "/fathom/boot-fell-back"
#define MODS  "/fathom/wifi/modules"

static void at(char *out, size_t cap, const char *rel) { snprintf(out, cap, "%s%s", bp_root, rel); }

static bool has_file(const char *rel)
{
    char p[256];
    at(p, sizeof p, rel);
    int fd = open(p, O_RDONLY);
    if(fd < 0)
        return false;
    close(fd);
    return true;
}

static bool has_dir(const char *rel)
{
    char p[256];
    at(p, sizeof p, rel);
    DIR *d = opendir(p);
    if(!d)
        return false;
    closedir(d);
    return true;
}

static int mv(const char *from, const char *to)
{
    char a[256], b[256];
    at(a, sizeof a, from);
    at(b, sizeof b, to);
    return rename(a, b);
}

static void rm(const char *rel)
{
    char p[256];
    at(p, sizeof p, rel);
    remove(p);
}

/* A folder of files (the modules: no folders inside), and the folder */
static void rm_dir(const char *rel)
{
    char p[256], f[320];
    at(p, sizeof p, rel);
    DIR *d = opendir(p);
    if(!d)
        return;
    struct dirent *e;
    while((e = readdir(d))) {
        if(!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        snprintf(f, sizeof f, "%s/%s", p, e->d_name);
        remove(f);
    }
    closedir(d);
    rmdir(p);
}

static void mark(const char *rel, const char *text)
{
    char p[256];
    at(p, sizeof p, rel);
    int fd = BP_CREATE(p);
    if(fd < 0)
        return;
    if(write(fd, text, strlen(text)) < 0) { /* (the fallback itself is done) */ }
    close(fd);
}

/* The kernel and its modules from before the update back in place */
static int fall_back(void)
{
    if(!has_file("/uImage.prev"))
        return -1;
    rm("/uImage.failed");
    if(mv("/uImage", "/uImage.failed") < 0 && has_file("/uImage"))
        return -1;
    if(mv("/uImage.prev", "/uImage") < 0) {
        mv("/uImage.failed", "/uImage");          /* (as it was) */
        return -1;
    }
    if(has_dir(MODS ".prev")) {
        rm_dir(MODS ".failed");
        if(mv(MODS, MODS ".failed") == 0 && mv(MODS ".prev", MODS) < 0)
            mv(MODS ".failed", MODS);
    }
    mark(FELL, "the new kernel didn't start twice: the previous one is back\n");
    return 0;
}

int fathom_boot_pick(void)
{
    if(!has_dir("/fathom"))                       /* not a Fathom card */
        return 0;
    if(has_file(NEW)) {
        rm(TRY1);
        mv(NEW, TRY1);
        return 0;
    }
    if(has_file(TRY1)) {
        rm(TRY2);
        mv(TRY1, TRY2);
        return 0;
    }
    if(has_file(TRY2)) {
        rm(TRY2);
        return fall_back() == 0;
    }
    return 0;
}

int fathom_boot_fall_back(void)
{
    if(!has_dir("/fathom"))
        return -1;
    rm(NEW);
    rm(TRY1);
    rm(TRY2);
    return fall_back();
}
