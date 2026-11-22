/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
#include "keys.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

/* Lowercase ASCII, collapse runs of whitespace, trim. UTF-8 bytes pass through. */
static void normalize(const char *in, char *out, size_t cap)
{
    size_t w = 0;
    int space = 1; /* suppress leading spaces */
    if (cap == 0) return;
    for (const unsigned char *p = (const unsigned char *)(in ? in : ""); *p && w + 1 < cap; p++) {
        unsigned char c = *p;
        if (c < 0x80 && isspace(c)) {
            if (!space) { out[w++] = ' '; space = 1; }
            continue;
        }
        out[w++] = (char)(c < 0x80 ? tolower(c) : c);
        space = 0;
    }
    while (w > 0 && out[w - 1] == ' ') w--;
    out[w] = '\0';
}

int key_is_various(const char *album_artist)
{
    char buf[64];
    normalize(album_artist, buf, sizeof buf);
    return !strcmp(buf, "various artists") || !strcmp(buf, "various") ||
           !strcmp(buf, "va") || !strcmp(buf, "v.a.") || !strcmp(buf, "various artist");
}

static void cut_at(char *s, const char *marker)
{
    char *p = strstr(s, marker);
    if (p) *p = '\0';
}

void key_artist(const char *artist, const char *album_artist, char *out, size_t cap)
{
    const char *src = artist;
    if (album_artist && *album_artist && !key_is_various(album_artist))
        src = album_artist;
    normalize(src, out, cap);

    static const char *const guests[] = {
        " feat. ", " feat ", " ft. ", " ft ", " featuring ", " (feat", " [feat", " (ft.", " [ft.",
    };
    for (size_t i = 0; i < sizeof guests / sizeof guests[0]; i++)
        cut_at(out, guests[i]);

    if (!strncmp(out, "the ", 4) && out[4])
        memmove(out, out + 4, strlen(out + 4) + 1);

    size_t len = strlen(out);
    while (len > 0 && out[len - 1] == ' ') out[--len] = '\0';
}

void key_album(const char *artist, const char *album_artist, const char *album,
               char *out, size_t cap)
{
    if (cap == 0) return;
    /* The album artist's key, exactly as browsing makes it: without an
     * ALBUMARTIST tag, "X" and "X feat. Y" (or "The X" and "X") on one album
     * were two albums; "VA" and "Various Artists" too. */
    const char *who = (album_artist && *album_artist) ? album_artist : artist;
    if (key_is_various(album_artist)) snprintf(out, cap, "various artists");
    else key_artist(who ? who : "", NULL, out, cap);
    if (!*out) snprintf(out, cap, "unknown artist");
    size_t len = strlen(out);
    if (len + 2 < cap) {
        out[len++] = '\x1f';
        normalize(album, out + len, cap - len);
    }
}

/* ---------- credits ---------- */

/* First case-insensitive (ASCII) occurrence of `needle` in `hay`. */
static const char *ci_find(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    for (; *hay; hay++)
        if (!strncasecmp(hay, needle, n)) return hay;
    return NULL;
}

static void add_name(const char *s, size_t len, char names[KEY_CREDIT_MAX][KEY_CREDIT_LEN], int *n)
{
    while (len && isspace((unsigned char)*s)) { s++; len--; }
    while (len && (isspace((unsigned char)s[len - 1]) || s[len - 1] == ')' || s[len - 1] == ']')) len--;
    if (!len || *n >= KEY_CREDIT_MAX) return;
    if (len >= KEY_CREDIT_LEN) len = KEY_CREDIT_LEN - 1;
    for (int i = 0; i < *n; i++)
        if (strlen(names[i]) == len && !strncasecmp(names[i], s, len)) return;
    memcpy(names[*n], s, len);
    names[*n][len] = '\0';
    (*n)++;
}

/* Split s[0..len) at any of `seps` and add the parts. */
static void add_split(const char *s, size_t len, const char *const *seps, int nseps,
                      char names[KEY_CREDIT_MAX][KEY_CREDIT_LEN], int *n)
{
    char buf[512];
    if (len >= sizeof buf) len = sizeof buf - 1;
    memcpy(buf, s, len);
    buf[len] = '\0';
    const char *p = buf;
    for (;;) {
        const char *best = NULL;
        size_t blen = 0;
        for (int i = 0; i < nseps; i++) {
            const char *f = ci_find(p, seps[i]);
            if (f && (!best || f < best)) { best = f; blen = strlen(seps[i]); }
        }
        if (!best) { add_name(p, strlen(p), names, n); return; }
        add_name(p, (size_t)(best - p), names, n);
        p = best + blen;
    }
}

static const char *const MAIN_SEPS[] = {";", " / "};
static const char *const GUEST_SEPS[] = {";", " / ", ", ", " & ", " and "};

/* Where the guests start in `s` (after the marker), or NULL; *cut = marker start. */
static const char *guest_start(const char *s, const char *const *markers, int nm, const char **cut)
{
    const char *best = NULL, *after = NULL;
    for (int i = 0; i < nm; i++) {
        const char *f = ci_find(s, markers[i]);
        if (f && (!best || f < best)) { best = f; after = f + strlen(markers[i]); }
    }
    *cut = best;
    return after;
}

int key_credits(const char *artist, const char *title, char names[KEY_CREDIT_MAX][KEY_CREDIT_LEN])
{
    static const char *const in_artist[] = {
        " (feat. ", " (feat ", " (ft. ", " [feat. ", " [ft. ", " (featuring ", " (with ",
        " feat. ", " feat ", " ft. ", " ft ", " featuring ", " with ",
    };
    static const char *const in_title[] = {
        "(feat. ", "(feat ", "(ft. ", "[feat. ", "[ft. ", "(featuring ", "(with ",
    };
    int n = 0;
    const char *a = artist ? artist : "";
    const char *cut, *g = guest_start(a, in_artist, (int)(sizeof in_artist / sizeof *in_artist), &cut);
    add_split(a, cut ? (size_t)(cut - a) : strlen(a), MAIN_SEPS, 2, names, &n);
    if (g) add_split(g, strlen(g), GUEST_SEPS, 5, names, &n);

    const char *t = title ? title : "";
    g = guest_start(t, in_title, (int)(sizeof in_title / sizeof *in_title), &cut);
    if (g) {
        const char *end = strpbrk(g, ")]");
        add_split(g, end ? (size_t)(end - g) : strlen(g), GUEST_SEPS, 5, names, &n);
    }
    return n;
}
