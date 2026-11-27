/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/*
 * Character packs (ui/glyphpack.h), fetched: the list of those there are, and
 * one downloaded into the card's packs folder. A pack is not part of a
 * release: it is published once, as a zip of its one file, and fetched when
 * the user asks (Settings > Interface > Language packs). What comes is
 * checked against the size and SHA-256 built in here before it is given its
 * name, so a pack in the folder under its name is whole.
 */
#ifndef FATHOM_PACKFETCH_H
#define FATHOM_PACKFETCH_H

#include <stdint.h>

typedef struct {
    const char *id;         /* "ja" */
    const char *name;       /* "Japanese": the row in Settings */
    const char *file;       /* "glyphs-ja.bin": its name in the folder, and inside the zip */
    const char *url;        /* the zip */
    int64_t zip_size;       /* about: shown before the download, and for the room needed */
    int64_t size;           /* the file's, exactly */
    const char *sha256;     /* the file's */
    int han;                /* it has the ideographs Chinese and Japanese share (each draws
                             * them its own way: the user says whose are used, packs.first) */
    const char *brief;      /* ...and its name where that is said, beside the others': "Simplified" */
} pack_info_t;

int                packs_count(void);
const pack_info_t *packs_info(int i);      /* NULL past the last */

enum { PACK_OK, PACK_ERR_NET, PACK_ERR_SPACE, PACK_ERR_DOWNLOAD, PACK_ERR_DAMAGED, PACK_ERR_WRITE, PACK_ERR_CANCELLED };
/* For the screen, in English (the UI translates it); "" for PACK_OK */
const char *pack_error_text(int err);

/* 1: the pack's file is in dir under its name */
int pack_installed(const char *dir, const pack_info_t *p);

/* The pack downloaded into dir (made if need be), checked, and named. progress:
 * 0 to 100 of the whole (the download to 90, the unpacking and checking the
 * rest). Nothing is left behind unless it is the whole pack. A pack there
 * already is replaced only once the new one has passed its check. */
typedef void (*pack_progress_fn)(int percent, void *ctx);
int pack_fetch(const char *dir, const pack_info_t *p, pack_progress_fn progress, void *ctx, const volatile int *abort);

/* The pack's file removed (and anything a cut-short fetch left); 0, -1 */
int pack_remove(const char *dir, const pack_info_t *p);

/* ---- one fetch at a time, on a thread of its own ----
 * packs_init: the folder (the card's fathom/packs). Then for each pack of the
 * list where it stands; packs_get starts fetching one (0; -1: another is being
 * fetched, or no such pack); packs_cancel stops the fetch under way;
 * packs_delete removes one that is there (-1: it is being fetched);
 * packs_refuse: the fetch that can't start now, and why (no Wi-Fi), as a
 * failure. packs_seq changes whenever anything of that does (progress too);
 * packs_gen changes when a pack came or went: the text is to be drawn again.
 * packs_busy: fetching now. At the player's end: packs_stop_ask, then
 * packs_stop_wait (1: the thread has ended; 0: not within ms, and it is left). */
enum { PACK_ABSENT, PACK_GETTING, PACK_THERE, PACK_FAILED };
typedef struct {
    int state;              /* PACK_ABSENT... */
    int percent;            /* PACK_GETTING */
    char why[64];           /* PACK_FAILED: for the screen, in English */
} pack_status_t;

void     packs_init(const char *dir);
void     packs_status(int i, pack_status_t *out);
int      packs_get(int i);
void     packs_cancel(void);
int      packs_delete(int i);
void     packs_refuse(int i, const char *why);
unsigned packs_seq(void);
unsigned packs_gen(void);
int      packs_busy(void);
void     packs_stop_ask(void);
int      packs_stop_wait(int ms);

/* Another place to fetch the zips from than their own addresses ("" or NULL:
 * their own): BASE/glyphs-ja.zip. For trying a pack before it is published
 * (the setting packs.url: a server on the bench). What comes is checked
 * against the built-in size and digest all the same. */
void packs_url_base(const char *base);

/* Tests: another list than the built-in one (NULL, 0: the built-in again) */
void packs_test_list(const pack_info_t *list, int n);

#endif
