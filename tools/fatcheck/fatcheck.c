/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * fathom-fatcheck (on the H2): list a folder with fatwalk (reading the FAT
 * directly) and with readdir + stat (as the kernel shows it), and compare
 * every path, date and size. Checks fatwalk on real cards.
 *
 *   fathom-fatcheck /dev/mmcblk0p1 /mnt/sd Music
 */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "library/fatwalk.h"

typedef struct { char *p; long long mt, sz; } ent_t;
typedef struct { ent_t *v; int n, cap; } list_t;

static void add(list_t *l, const char *p, long long mt, long long sz)
{
    if (l->n == l->cap) { l->cap = l->cap ? l->cap * 2 : 4096; l->v = realloc(l->v, (size_t)l->cap * sizeof *l->v); }
    l->v[l->n].p = strdup(p); l->v[l->n].mt = mt; l->v[l->n].sz = sz; l->n++;
}

static int on_file(const char *p, int64_t mt, int64_t sz, void *ctx) { add(ctx, p, mt, sz); return 0; }

static void walk(const char *dir, list_t *l)
{
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    char p[2048];
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(p, &st)) continue;
        if (S_ISDIR(st.st_mode)) walk(p, l);
        else add(l, p, (long long)st.st_mtime, (long long)st.st_size);
    }
    closedir(d);
}

static int cmp(const void *a, const void *b) { return strcmp(((const ent_t *)a)->p, ((const ent_t *)b)->p); }

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
    if (argc != 4) { fprintf(stderr, "usage: %s DEV MOUNT FOLDER\n", argv[0]); return 2; }
    list_t a = {0}, b = {0};
    char root[1024];
    snprintf(root, sizeof root, "%s/%s", argv[2], argv[3]);
    double t0 = now();
    int rc = fatwalk(argv[1], argv[2], argv[3], on_file, &a);
    double t1 = now();
    walk(root, &b);
    double t2 = now();
    printf("fatwalk: rc %d, %d files, %.2f s; readdir: %d files, %.2f s\n", rc, a.n, t1 - t0, b.n, t2 - t1);
    qsort(a.v, (size_t)a.n, sizeof *a.v, cmp);
    qsort(b.v, (size_t)b.n, sizeof *b.v, cmp);
    int i = 0, j = 0, diffs = 0;
    while (i < a.n || j < b.n) {
        int c = i >= a.n ? 1 : j >= b.n ? -1 : strcmp(a.v[i].p, b.v[j].p);
        if (c == 0) {
            if (a.v[i].mt != b.v[j].mt || a.v[i].sz != b.v[j].sz)
                if (diffs++ < 20) printf("differs: %s  fat %lld %lld  stat %lld %lld\n", a.v[i].p, a.v[i].mt, a.v[i].sz, b.v[j].mt, b.v[j].sz);
            i++; j++;
        } else if (c < 0) {
            if (diffs++ < 20) printf("only fatwalk: %s\n", a.v[i].p);
            i++;
        } else {
            if (diffs++ < 20) printf("only readdir: %s\n", b.v[j].p);
            j++;
        }
    }
    printf("%d differences\n", diffs);
    return diffs ? 1 : 0;
}
