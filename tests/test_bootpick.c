/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * h2boot's fallback to the previous kernel (bootloader/fathom/bootpick.c),
 * the same code on a folder standing in for the card: an update's kernel
 * tried twice, then the one before put back with its Wi-Fi modules.
 */
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../bootloader/fathom/bootpick.h"

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static char g_root[400];

static const char *at(const char *rel)
{
    static char p[4][600];
    static int k;
    k = (k + 1) % 4;
    snprintf(p[k], sizeof p[k], "%s%s", g_root, rel);
    return p[k];
}

static void put(const char *rel, const char *text)
{
    FILE *f = fopen(at(rel), "wb");
    fputs(text, f);
    fclose(f);
}

static int is(const char *rel, const char *text)
{
    char b[200] = "";
    FILE *f = fopen(at(rel), "rb");
    if (!f) return 0;
    size_t n = fread(b, 1, sizeof b - 1, f);
    fclose(f);
    b[n] = '\0';
    return !strcmp(b, text);
}

static int has(const char *rel) { struct stat st; return stat(at(rel), &st) == 0; }

/* A card just updated: new kernel and modules, the old ones beside them */
static void card(int with_prev, int with_mods_prev)
{
    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf '%s' && mkdir -p '%s/fathom/wifi/modules'", g_root, g_root);
    if (system(cmd)) {}
    put("/uImage", "new kernel");
    put("/fathom/wifi/modules/brcmfmac.ko", "new module");
    put("/fathom/wifi/modules/cfg80211.ko", "new module 2");
    if (with_prev) put("/uImage.prev", "old kernel");
    if (with_mods_prev) {
        mkdir(at("/fathom/wifi/modules.prev"), 0755);
        put("/fathom/wifi/modules.prev/brcmfmac.ko", "old module");
        put("/fathom/wifi/modules.prev/cfg80211.ko", "old module 2");
    }
    put("/fathom/boot-new", "a new kernel\n");
}

int main(void)
{
    snprintf(g_root, sizeof g_root, "%s/fathom-bootpick-%d", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", (int)getpid());
    fathom_boot_root(g_root);

    /* the new kernel starts and the player gets up (deleting the marker) */
    card(1, 1);
    CHECK(fathom_boot_pick() == 0 && is("/uImage", "new kernel") && has("/fathom/boot-try1") && !has("/fathom/boot-new"));
    unlink(at("/fathom/boot-try1"));               /* (the player) */
    CHECK(fathom_boot_pick() == 0 && is("/uImage", "new kernel") && !has("/fathom/boot-fell-back"));

    /* the power went during the first start: a second try, then it works */
    card(1, 1);
    CHECK(fathom_boot_pick() == 0);
    CHECK(fathom_boot_pick() == 0 && has("/fathom/boot-try2") && !has("/fathom/boot-try1") && is("/uImage", "new kernel"));
    unlink(at("/fathom/boot-try2"));
    CHECK(fathom_boot_pick() == 0 && is("/uImage", "new kernel"));

    /* it never gets up: the third start goes back to the kernel and modules before */
    card(1, 1);
    CHECK(fathom_boot_pick() == 0);
    CHECK(fathom_boot_pick() == 0);
    CHECK(fathom_boot_pick() == 1);
    CHECK(is("/uImage", "old kernel") && is("/uImage.failed", "new kernel") && !has("/uImage.prev"));
    CHECK(is("/fathom/wifi/modules/brcmfmac.ko", "old module") && is("/fathom/wifi/modules/cfg80211.ko", "old module 2"));
    CHECK(is("/fathom/wifi/modules.failed/brcmfmac.ko", "new module") && !has("/fathom/wifi/modules.prev"));
    CHECK(has("/fathom/boot-fell-back") && !has("/fathom/boot-try1") && !has("/fathom/boot-try2"));
    /* and the starts after it: the old kernel, nothing more moved */
    unlink(at("/fathom/boot-fell-back"));
    CHECK(fathom_boot_pick() == 0 && is("/uImage", "old kernel") && is("/fathom/wifi/modules/brcmfmac.ko", "old module"));

    /* an update from before modules were kept: the kernel alone goes back */
    card(1, 0);
    fathom_boot_pick(), fathom_boot_pick();
    CHECK(fathom_boot_pick() == 1 && is("/uImage", "old kernel") && is("/fathom/wifi/modules/brcmfmac.ko", "new module"));

    /* no kernel to go back to: the new one stays, tried every time */
    card(0, 1);
    fathom_boot_pick(), fathom_boot_pick();
    CHECK(fathom_boot_pick() == 0 && is("/uImage", "new kernel") && !has("/fathom/boot-fell-back"));

    /* /uImage won't load at all: back at once */
    card(1, 1);
    CHECK(fathom_boot_pick() == 0);
    CHECK(fathom_boot_fall_back() == 0 && is("/uImage", "old kernel") && !has("/fathom/boot-try1"));
    CHECK(is("/fathom/wifi/modules/brcmfmac.ko", "old module") && has("/fathom/boot-fell-back"));
    /* a second fallback later replaces the first one's leftovers */
    put("/uImage.prev", "older kernel");
    mkdir(at("/fathom/wifi/modules.prev"), 0755);
    put("/fathom/wifi/modules.prev/brcmfmac.ko", "older module");
    CHECK(fathom_boot_fall_back() == 0 && is("/uImage", "older kernel") && is("/uImage.failed", "old kernel"));
    CHECK(is("/fathom/wifi/modules.failed/brcmfmac.ko", "old module") && is("/fathom/wifi/modules.failed/cfg80211.ko", "old module 2"));

    /* a card copied by hand: no markers, nothing touched */
    card(1, 1);
    unlink(at("/fathom/boot-new"));
    CHECK(fathom_boot_pick() == 0 && is("/uImage", "new kernel") && is("/uImage.prev", "old kernel"));
    /* not a Fathom card */
    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf '%s' && mkdir -p '%s'", g_root, g_root);
    if (system(cmd)) {}
    put("/uImage", "someone's kernel");
    CHECK(fathom_boot_pick() == 0 && fathom_boot_fall_back() == -1 && is("/uImage", "someone's kernel"));

    snprintf(cmd, sizeof cmd, "rm -rf '%s'", g_root);
    if (system(cmd)) {}
    if (failures) { fprintf(stderr, "%d failures\n", failures); return 1; }
    printf("bootpick: ok\n");
    return 0;
}
