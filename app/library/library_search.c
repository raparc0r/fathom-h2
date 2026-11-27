/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* Search: the key each artist, album and song is found by (and the filling
 * of those columns), how close a name is to what was typed, and the search
 * itself, with the SQL functions it runs through. Out of library.c, as it
 * was there; search_register is new, and holds what library_open did. */
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE
#define _GNU_SOURCE
#include "library/library_int.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "library/playlist.h"

/* SQL fold(text): words for search (text_search_fold). */
static void sql_fold(sqlite3_context *ctx, int argc, sqlite3_value **argv)
{
    (void)argc;
    const unsigned char *in = sqlite3_value_text(argv[0]);
    char out[1024];
    text_search_fold(in ? (const char *)in : "", out, sizeof out);
    sqlite3_result_text(ctx, out, -1, SQLITE_TRANSIENT);
}

/* ---- Search keys, and how close a name is to what was typed ----
 * Each artist, album and song has a key in its `search` column: its name, its
 * artist's and its album's, each folded (text_search_fold: lower case, no
 * accents, words a space apart), with '|' between them (no fold has one):
 *     exodus|bob marley the wailers|exodus
 * A search reads only these (the songs' from an index of their own,
 * tracks_search: 4 MB for 50,000 songs, where the table's rows are 30 MB and
 * reading them from the card took a second by itself), and what's typed is
 * matched against them as they are: nothing folded or copied per row. */
#define SKEY_SEP '|'

/* The key of a name, its artist's and its album's (NULL: none), into out
 * (SKEY_CAP bytes): its length. */
#define SKEY_CAP 1600

static size_t skey_make(const char *const part[3], char *out)
{
    size_t w = 0;
    for (int i = 0; i < 3; i++) {
        if (i) out[w++] = SKEY_SEP;
        text_search_fold(part[i] ? part[i] : "", out + w, 500);
        w += strlen(out + w);
    }
    return w;
}

/* SQL skey(name, artist, album): the key. */
static void sql_skey(sqlite3_context *ctx, int argc, sqlite3_value **argv)
{
    char out[SKEY_CAP];
    const char *part[3];
    for (int i = 0; i < 3; i++) part[i] = i < argc ? (const char *)sqlite3_value_text(argv[i]) : NULL;
    sqlite3_result_text(ctx, out, (int)skey_make(part, out), SQLITE_TRANSIENT);
}

static void sql_search_keep(sqlite3_context *ctx, int argc, sqlite3_value **argv);

typedef struct { const char *p; int n; } tok_t;

/* [s, e) cut into its words (at most max: a longer name's last ones aren't
 * looked at). */
static int toks_of(const char *s, const char *e, tok_t *t, int max)
{
    int n = 0;
    while (s < e && n < max) {
        const char *sp = memchr(s, ' ', (size_t)(e - s));
        if (!sp) sp = e;
        if (sp > s) t[n++] = (tok_t){s, (int)(sp - s)};
        s = sp + 1;
    }
    return n;
}

static int tok_is(tok_t t, const char *w, int wl, int whole)
{
    return (whole ? t.n == wl : t.n >= wl) && !memcmp(t.p, w, (size_t)wl);
}

/* The words typed, in order, are the tokens from `at` on (the last one whole
 * only if asked). */
static int phrase_at(const tok_t *t, int nt, int at, const char *const *w, const int *wl, int n, int last_whole)
{
    if (at + n > nt) return 0;
    for (int k = 0; k < n; k++)
        if (!tok_is(t[at + k], w[k], wl[k], k < n - 1 || last_whole)) return 0;
    return 1;
}

/* A word of [s, e) starts with w. */
static int span_has(const char *s, const char *e, const char *w, int wl)
{
    while (s < e) {
        if (e - s >= wl && !memcmp(s, w, (size_t)wl)) return 1;
        const char *sp = memchr(s, ' ', (size_t)(e - s));
        if (!sp) break;
        s = sp + 1;
    }
    return 0;
}

/* library_search_tier, of a key. */
static int tier_of_key(const char *key, const char *const *words, int n, int last_whole)
{
    if (n <= 0 || n > 8 || !key) return 0;
    const char *own_e = strchr(key, SKEY_SEP);
    if (!own_e) own_e = key + strlen(key);
    const char *art = *own_e ? own_e + 1 : own_e, *art_e = strchr(art, SKEY_SEP);
    if (!art_e) art_e = art + strlen(art);
    const char *alb = *art_e ? art_e + 1 : art_e, *alb_e = alb + strlen(alb);
    int wl[8], in_own[8], begin = 1, any_own = 0;
    for (int k = 0; k < n; k++) {
        wl[k] = (int)strlen(words[k]);
        in_own[k] = span_has(key, own_e, words[k], wl[k]);
        /* (most rows end here: a word that's nowhere in them) */
        if (!in_own[k]) {
            begin = 0;
            if (!span_has(art, art_e, words[k], wl[k]) && !span_has(alb, alb_e, words[k], wl[k])) return 0;
        } else {
            any_own = 1;
        }
    }
    if (!any_own) return 0;                                  /* an artist's or an album's name alone */
    if (begin) {                                             /* every word begins a word of the name */
        tok_t own[48];
        int no = toks_of(key, own_e, own, 48);
        int the = no > 1 && own[0].n == 3 && !memcmp(own[0].p, "the", 3);   /* (a leading The passed over, as in the lists) */
        if (no - the == n && phrase_at(own, no, the, words, wl, n, 1)) return 1;
        if (no == n && phrase_at(own, no, 0, words, wl, n, 1)) return 1;
        if (phrase_at(own, no, the, words, wl, n, last_whole) || phrase_at(own, no, 0, words, wl, n, last_whole)) return 2;
        for (int at = 1; at + n <= no; at++)
            if (phrase_at(own, no, at, words, wl, n, last_whole)) return 3;
        int all = 1;
        for (int k = 0; k < n && all; k++) {
            int whole = k < n - 1 || last_whole, hit = 0;
            for (int i = 0; i < no && !hit; i++) hit = tok_is(own[i], words[k], wl[k], whole);
            all = hit;
        }
        return all ? 4 : 5;
    }
    /* split between the name and its artist's or album's: one of the words
     * at least in the name and not in the artist's */
    for (int k = 0; k < n; k++)
        if (in_own[k] && !span_has(art, art_e, words[k], wl[k])) return 6;
    return 0;
}

int library_search_tier(const char *name, const char *artist, const char *album, const char *const *words, int n,
                        int last_whole)
{
    char key[SKEY_CAP];
    const char *part[3] = {name, artist, album};
    skey_make(part, key);
    return tier_of_key(key, words, n, last_whole);
}

/* Fill the search columns (only the empty ones, or all after a scan; 2: the
 * empty ones of the songs a scan wrote, temp.g_c, and of albums and artists:
 * not a read of every song for a few new ones, the write lock held). */
void index_search(library_t *lib, int only_missing)
{
    const char *w = only_missing == 2 ? " WHERE search IS NULL AND id IN (SELECT id FROM temp.g_c)"
                  : only_missing ? " WHERE search IS NULL" : "";
    char sql[600];
    snprintf(sql, sizeof sql, "UPDATE tracks SET search=" SKEY_TRACK("title", "artist", "album_artist", "album") "%s;", w);
    sqlite3_exec(lib->db, sql, NULL, NULL, NULL);
    if (only_missing) w = " WHERE search IS NULL";
    snprintf(sql, sizeof sql,
             "UPDATE albums SET search=skey(title, (SELECT name FROM artists a WHERE a.id=albums.artist_id), NULL)%s;", w);
    sqlite3_exec(lib->db, sql, NULL, NULL, NULL);
    snprintf(sql, sizeof sql, "UPDATE artists SET search=" SKEY_ARTIST "%s;", w);
    sqlite3_exec(lib->db, sql, NULL, NULL, NULL);
}

/* ---------- search ---------- */

/* One kind's names that what was typed finds, each with how close it is
 * (tier_of_key), the closest first and then by name (a leading The passed
 * over): up to LIB_SEARCH_MAX of them, added to hits. Every key is read (the
 * songs' from their index, tracks_search: library_check_plans) and given to
 * search_keep(), which keeps the closest so far: the whole pass in one
 * sqlite3_step (a step a row took the H2 twice as long: 0.55 s for 50,000
 * songs, where this takes a quarter of a second whatever is typed). */
const char *search_sql(const char *table, const char *where, char *sql, size_t cap)
{
    snprintf(sql, cap, "SELECT count(search_keep(id, search)) FROM %s WHERE %s", table, where);
    return sql;
}

typedef struct { int64_t id; int tier, len; char ord[56]; } found_t;
typedef struct {
    const char *words[8];
    int nw, last_whole, n;
    found_t top[LIB_SEARCH_MAX];
} search_run_t;

/* SQL search_keep(id, key): the search under way (the library's), told of a
 * name. */
static void sql_search_keep(sqlite3_context *ctx, int argc, sqlite3_value **argv)
{
    search_run_t *r = *(search_run_t **)sqlite3_user_data(ctx);
    const char *key = argc == 2 ? (const char *)sqlite3_value_text(argv[1]) : NULL;
    int tier = r ? tier_of_key(key, r->words, r->nw, r->last_whole) : 0;
    if (!tier) return;
    const char *e = strchr(key, SKEY_SEP);
    if (!e) e = key + strlen(key);
    if (e - key > 4 && !memcmp(key, "the ", 4)) key += 4;
    int len = (int)(e - key) < (int)sizeof r->top->ord ? (int)(e - key) : (int)sizeof r->top->ord;
    /* where it goes among the closest so far (after the ones as close and
     * named the same: the first read stays first) */
    int at = r->n;
    while (at > 0) {
        const found_t *f = &r->top[at - 1];
        int c = f->tier - tier;
        if (!c) c = memcmp(f->ord, key, (size_t)(f->len < len ? f->len : len));
        if (!c) c = f->len - len;
        if (c <= 0) break;
        at--;
    }
    if (at == LIB_SEARCH_MAX) return;
    if (r->n < LIB_SEARCH_MAX) r->n++;
    memmove(&r->top[at + 1], &r->top[at], (size_t)(r->n - 1 - at) * sizeof *r->top);
    r->top[at].id = sqlite3_value_int64(argv[0]);
    r->top[at].tier = tier;
    r->top[at].len = len;
    memcpy(r->top[at].ord, key, (size_t)len);
}

static int search_kind_rows(library_t *lib, int kind, const char *table, const char *where, char words[][64], int nw,
                            int last_whole, lib_hit_t *hits, int n)
{
    char sql[160];
    search_run_t *r = calloc(1, sizeof *r);
    sqlite3_stmt *s = r ? prep(lib->db, search_sql(table, where, sql, sizeof sql)) : NULL;
    if (!s) { free(r); return n; }
    for (int i = 0; i < nw; i++) r->words[i] = words[i];
    r->nw = nw;
    r->last_whole = last_whole;
    lib->search_run = r;
    sqlite3_step(s);
    lib->search_run = NULL;
    sqlite3_finalize(s);
    for (int i = 0; i < r->n && n < 3 * LIB_SEARCH_MAX; i++) hits[n++] = (lib_hit_t){kind, r->top[i].id, r->top[i].tier};
    free(r);
    return n;
}

int library_search(library_t *lib, const char *query, lib_search_t *out)
{
    memset(out, 0, sizeof *out);
    char folded[256], words[8][64];
    text_search_fold(query, folded, sizeof folded);
    int nw = 0;
    for (char *save = NULL, *t = strtok_r(folded, " ", &save); t && nw < 8; t = strtok_r(NULL, " ", &save))
        snprintf(words[nw++], sizeof words[0], "%s", t);
    if (!nw) return 0;
    size_t ql = strlen(query);
    int last_whole = ql > 0 && query[ql - 1] == ' ';         /* (a space after it: the word is finished) */
    lib_hit_t *all = malloc(3 * LIB_SEARCH_MAX * sizeof *all);
    if (!all) return 0;
    int n = 0;
    n = search_kind_rows(lib, LIB_HIT_ARTIST, "artists", "(nalbums>0 OR ncredit_albums>0)", words, nw, last_whole, all, n);
    n = search_kind_rows(lib, LIB_HIT_ALBUM, "albums", "1", words, nw, last_whole, all, n);
    n = search_kind_rows(lib, LIB_HIT_SONG, "tracks", "1", words, nw, last_whole, all, n);
    int close = 0;
    for (int i = 0; i < n; i++) close += all[i].tier <= 4;
    int top = close >= LIB_SEARCH_FEW ? 4 : 6;               /* the looser ones only when few are close */
    out->hits = malloc(LIB_SEARCH_MAX * sizeof *out->hits);
    out->songs = malloc(LIB_SEARCH_MAX * sizeof *out->songs);
    if (!out->hits || !out->songs) { free(all); library_search_free(out); return 0; }
    /* by tier, then artists, albums, songs, each in the order it came (its list's) */
    for (int tier = 1; tier <= top && out->n < LIB_SEARCH_MAX; tier++)
        for (int i = 0; i < n && out->n < LIB_SEARCH_MAX; i++) {
            if (all[i].tier != tier) continue;
            out->hits[out->n++] = all[i];
            if (all[i].kind == LIB_HIT_ARTIST) out->n_artists++;
            else if (all[i].kind == LIB_HIT_ALBUM) out->n_albums++;
            else out->songs[out->n_songs++] = all[i].id;
        }
    free(all);
    return out->n;
}

void library_search_free(lib_search_t *r)
{
    free(r->hits);
    free(r->songs);
    memset(r, 0, sizeof *r);
}

/* The search's SQL functions, on a connection (library_open) */
void search_register(library_t *lib)
{
    sqlite3_create_function(lib->db, "fold", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, NULL, sql_fold, NULL, NULL);
    sqlite3_create_function(lib->db, "skey", 3, SQLITE_UTF8 | SQLITE_DETERMINISTIC, NULL, sql_skey, NULL, NULL);
    sqlite3_create_function(lib->db, "search_keep", 2, SQLITE_UTF8, &lib->search_run, sql_search_keep, NULL, NULL);
}
