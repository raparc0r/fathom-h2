/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "intern.h"

#include <stdlib.h>
#include <string.h>

struct intern {
    uint32_t *slots;   /* id per slot, 0 = empty */
    uint32_t  cap;     /* power of two */
    char    **strs;    /* strs[id - 1] */
    uint32_t *hashes;  /* hashes[id - 1] */
    uint32_t  count;
    uint32_t  strs_cap;
};

static uint32_t fnv1a(const char *s)
{
    uint32_t h = 2166136261u;
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 16777619u; }
    return h;
}

intern_t *intern_new(void)
{
    intern_t *in = calloc(1, sizeof *in);
    if (!in) return NULL;
    in->cap = 1024;
    in->slots = calloc(in->cap, sizeof *in->slots);
    if (!in->slots) { free(in); return NULL; }
    return in;
}

void intern_free(intern_t *in)
{
    if (!in) return;
    for (uint32_t i = 0; i < in->count; i++) free(in->strs[i]);
    free(in->strs); free(in->hashes); free(in->slots); free(in);
}

static int grow(intern_t *in)
{
    uint32_t ncap = in->cap * 2;
    uint32_t *ns = calloc(ncap, sizeof *ns);
    if (!ns) return 0;
    for (uint32_t id = 1; id <= in->count; id++) {
        uint32_t i = in->hashes[id - 1] & (ncap - 1);
        while (ns[i]) i = (i + 1) & (ncap - 1);
        ns[i] = id;
    }
    free(in->slots);
    in->slots = ns;
    in->cap = ncap;
    return 1;
}

uint32_t intern_id(intern_t *in, const char *s)
{
    uint32_t h = fnv1a(s);
    uint32_t i = h & (in->cap - 1);
    while (in->slots[i]) {
        uint32_t id = in->slots[i];
        if (in->hashes[id - 1] == h && !strcmp(in->strs[id - 1], s)) return id;
        i = (i + 1) & (in->cap - 1);
    }
    if (in->count + 1 > in->strs_cap) {
        uint32_t nc = in->strs_cap ? in->strs_cap * 2 : 256;
        char **ns = realloc(in->strs, nc * sizeof *ns);
        if (!ns) return 0;
        in->strs = ns;
        uint32_t *nh = realloc(in->hashes, nc * sizeof *nh);
        if (!nh) return 0;
        in->hashes = nh;
        in->strs_cap = nc;
    }
    char *copy = strdup(s);
    if (!copy) return 0;
    uint32_t id = ++in->count;
    in->strs[id - 1] = copy;
    in->hashes[id - 1] = h;
    in->slots[i] = id;
    if (in->count * 2 > in->cap && !grow(in)) return 0;
    return id;
}

const char *intern_str(const intern_t *in, uint32_t id)
{
    return (id && id <= in->count) ? in->strs[id - 1] : "";
}

uint32_t intern_count(const intern_t *in) { return in->count; }
