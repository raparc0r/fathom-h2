/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* Browsing the library: each view's query and its list of ids (or only how
 * many), a row's two texts, a song's details, the names of things, the files
 * not read, the songs as the shuffle wants them, and a song's favourite mark
 * and resume place. Out of library.c, as it was there. */
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE
#define _GNU_SOURCE   /* strcasestr (glibc, musl) */
#include "library/library_int.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "core/hash.h"
#include "core/lang.h"

/* ---------- queries ---------- */

static int64_t g_credit_album;   /* LIB_CREDIT_TRACKS: the album (library_list takes one parent) */

/* A view's SQL (?1 its parent, ?2 g_credit_album); *cut: a smart playlist's
 * "a while" (bound as ?1; 0 none). NULL: no such view. */
const char *list_sql(lib_view_t view, int64_t parent, int64_t *cut)
{
    const char *sql = NULL;
    int64_t smart_cut = 0;
    switch (view) {
    case LIB_ALBUMS: sql = "SELECT id FROM albums ORDER BY sort, year"; break;
    case LIB_ALBUM_ARTISTS: sql = "SELECT id FROM artists WHERE nalbums>0 ORDER BY sort"; break;
    case LIB_ARTISTS: sql = "SELECT id FROM artists WHERE ncredit_albums>0 ORDER BY sort"; break;
    case LIB_CREDIT_ALBUMS:
        sql = "SELECT id FROM albums WHERE id IN (SELECT t.album_id FROM credits c JOIN tracks t ON t.id=c.track_id"
              " WHERE c.artist_id=?1) ORDER BY year, sort";
        break;
    case LIB_CREDIT_TRACKS:   /* library_list_credit_tracks() */
        sql = "SELECT t.id FROM tracks t JOIN credits c ON c.track_id=t.id WHERE c.artist_id=?1 AND t.album_id=?2"
              " ORDER BY t.disc, t.track, t.path";
        break;
    case LIB_ARTIST_ALBUMS: sql = "SELECT id FROM albums WHERE artist_id=?1 ORDER BY year, sort"; break;
    case LIB_ALBUM_TRACKS: sql = "SELECT id FROM tracks WHERE album_id=?1 ORDER BY disc, track, path"; break;
    case LIB_SONGS: sql = "SELECT id FROM tracks ORDER BY sort, title COLLATE NOCASE"; break;
    case LIB_PLAYLISTS: sql = "SELECT id FROM playlists ORDER BY sort"; break;
    case LIB_PLAYLIST_TRACKS: sql = "SELECT track_id FROM playlist_items WHERE playlist_id=?1 ORDER BY pos"; break;
    case LIB_FAVOURITES: sql = "SELECT id FROM tracks WHERE favourite=1 ORDER BY sort, title COLLATE NOCASE"; break;
    case LIB_RECENT_PLAYED: sql = "SELECT id FROM tracks WHERE last_played>0 ORDER BY last_played DESC LIMIT 200"; break;
    case LIB_MOST_PLAYED:
        sql = "SELECT id FROM tracks WHERE play_count>0 ORDER BY play_count DESC, last_played DESC LIMIT 200";
        break;
    case LIB_RECENT_ADDED:
        sql = "SELECT album_id FROM tracks WHERE album_id IS NOT NULL GROUP BY album_id ORDER BY MAX(mtime) DESC LIMIT 100";
        break;
    case LIB_GENRES: sql = "SELECT id FROM genres ORDER BY sort"; break;
    case LIB_GENRE_ALBUMS:
        sql = "SELECT id FROM albums WHERE id IN (SELECT album_id FROM tracks WHERE genre_id=?1) ORDER BY sort, year";
        break;
    case LIB_COMPOSERS: sql = "SELECT id FROM composers ORDER BY sort"; break;
    case LIB_COMPOSER_ALBUMS:
        sql = "SELECT id FROM albums WHERE id IN (SELECT album_id FROM tracks WHERE composer_id=?1) ORDER BY year, sort";
        break;
    case LIB_COMPOSER_TRACKS:   /* library_list_composer_tracks() */
        sql = "SELECT id FROM tracks WHERE composer_id=?1 AND album_id=?2 ORDER BY disc, track, path";
        break;
    case LIB_YEARS: sql = "SELECT DISTINCT year FROM albums WHERE year>0 ORDER BY year DESC"; break;
    case LIB_YEAR_ALBUMS: sql = "SELECT id FROM albums WHERE year=?1 ORDER BY sort"; break;
    case LIB_SMART: {   /* ?1: the time before which counts as "a while" */
        int64_t now = (int64_t)time(NULL);
        switch ((int)parent) {
        case LIB_SMART_NEVER:
            /* (the albums in their order, each one's unplayed songs from tracks_unplayed:
             * both read from indexes, in order, nothing sorted. CROSS JOIN keeps the
             * albums outside; the other way round sorted every song: 2.5 s for 50,000) */
            sql = "SELECT t.id FROM albums a CROSS JOIN tracks t ON t.album_id=a.id"
                  " WHERE t.play_count=0 AND t.skip_count=0 ORDER BY a.sort, a.year, a.id, t.disc, t.track";
            break;
        case LIB_SMART_REPEAT:
            smart_cut = now - 30 * 86400;
            sql = "SELECT track_id FROM plays WHERE at>?1 AND track_id IN (SELECT id FROM tracks)"
                  " GROUP BY track_id HAVING COUNT(*)>=2 ORDER BY COUNT(*) DESC, MAX(at) DESC LIMIT 50";
            break;
        case LIB_SMART_NOT_HEARD:
            smart_cut = now - 90 * 86400;
            sql = "SELECT id FROM tracks WHERE play_count>0 AND last_played<?1 ORDER BY last_played LIMIT 200";
            break;
        case LIB_SMART_FORGOTTEN:
            smart_cut = now - 60 * 86400;
            sql = "SELECT id FROM tracks WHERE favourite=1 AND last_played<?1 ORDER BY last_played LIMIT 100";
            break;
        case LIB_SMART_SKIPPED:
            sql = "SELECT id FROM tracks WHERE skip_count>=2 ORDER BY skip_count DESC, sort LIMIT 100";
            break;
        default:
            return NULL;
        }
        break;
    }
    case LIB_FOLDER:   /* the subfolders (as -id), then the songs; 0: the top folder's */
        sql = "SELECT x FROM (SELECT -id AS x, 0 AS k, sort AS o FROM folders WHERE parent=?1"
              " UNION ALL SELECT id, 1, path FROM tracks WHERE folder_id=?1) ORDER BY k, o";
        break;
    }
    (void)parent;
    *cut = smart_cut;
    return sql;
}

int library_list(library_t *lib, lib_view_t view, int64_t parent, lib_list_t *out)
{
    int64_t smart_cut = 0;
    const char *sql = list_sql(view, parent, &smart_cut);
    if (!sql) {
        memset(out, 0, sizeof *out);
        return -1;
    }
    if (view == LIB_FOLDER && parent == 0)     /* (0: the top folder's) */
        parent = db_int(lib->db, "SELECT id FROM folders WHERE parent=0 ORDER BY length(path) LIMIT 1", 0, 0);
    memset(out, 0, sizeof *out);
    out->view = view;
    sqlite3_stmt *s = prep(lib->db, sql);
    if (!s) return -1;
    if (view == LIB_ARTIST_ALBUMS || view == LIB_ALBUM_TRACKS || view == LIB_PLAYLIST_TRACKS ||
        view == LIB_CREDIT_ALBUMS || view == LIB_CREDIT_TRACKS || view == LIB_GENRE_ALBUMS ||
        view == LIB_COMPOSER_ALBUMS || view == LIB_COMPOSER_TRACKS || view == LIB_YEAR_ALBUMS || view == LIB_FOLDER)
        sqlite3_bind_int64(s, 1, parent);
    if (view == LIB_CREDIT_TRACKS || view == LIB_COMPOSER_TRACKS) sqlite3_bind_int64(s, 2, g_credit_album);
    if (view == LIB_SMART && smart_cut) sqlite3_bind_int64(s, 1, smart_cut);
    int cap = 0;
    while (sqlite3_step(s) == SQLITE_ROW) {
        int64_t *ni = vec_room(out->ids, out->n, &cap, 256, sizeof *ni);
        if (!ni) break;
        out->ids = ni;
        out->ids[out->n++] = sqlite3_column_int64(s, 0);
    }
    sqlite3_finalize(s);
    return out->n;
}

const char *library_smart_name(int which)
{
    /* (N_: listed for translators by tools/lang/strings.py; the UI translates the name it's given) */
    static const char *const N[LIB_SMART_N] = {"", N_("Never played"), N_("On repeat"), N_("Not heard in a while"),
                                               N_("Forgotten favourites"), N_("Often skipped")};
    return which > 0 && which < LIB_SMART_N ? N[which] : "";
}

/* A view of one album's songs (LIB_CREDIT_TRACKS, LIB_COMPOSER_TRACKS): parent's, on album_id */
static int list_on_album(library_t *lib, lib_view_t view, int64_t parent, int64_t album_id, lib_list_t *out)
{
    g_credit_album = album_id;
    int n = library_list(lib, view, parent, out);
    g_credit_album = 0;
    return n;
}

int library_list_credit_tracks(library_t *lib, int64_t artist_id, int64_t album_id, lib_list_t *out)
{
    return list_on_album(lib, LIB_CREDIT_TRACKS, artist_id, album_id, out);
}

int library_artist_tracks(library_t *lib, int64_t artist_id, int credited, lib_list_t *out)
{
    memset(out, 0, sizeof *out);
    lib_list_t albums;
    if (library_list(lib, credited ? LIB_CREDIT_ALBUMS : LIB_ARTIST_ALBUMS, artist_id, &albums) < 0) return -1;
    for (int i = 0; i < albums.n; i++) {
        lib_list_t t;
        int n = credited ? library_list_credit_tracks(lib, artist_id, albums.ids[i], &t)
                         : library_list(lib, LIB_ALBUM_TRACKS, albums.ids[i], &t);
        if (n > 0) {
            int64_t *ni = realloc(out->ids, (size_t)(out->n + t.n) * sizeof *ni);
            if (ni) {
                out->ids = ni;
                memcpy(out->ids + out->n, t.ids, (size_t)t.n * sizeof *ni);
                out->n += t.n;
            }
        }
        library_list_free(&t);
    }
    library_list_free(&albums);
    out->view = credited ? LIB_CREDIT_TRACKS : LIB_ALBUM_TRACKS;
    return out->n;
}

int library_needs_tags(library_t *lib)
{
    return get_int_col(lib, NEEDS_TAGS_SQL, 0) > 0;
}

int library_count(library_t *lib, lib_view_t view, int64_t parent)
{
    /* as many as library_list(view, parent) gives, without listing them */
    if (view == LIB_SMART && parent == LIB_SMART_NEVER)
        return (int)get_int_col(lib, NEVER_PLAYED_COUNT_SQL, 0);
    int64_t cut = 0;
    const char *sql = list_sql(view, parent, &cut);
    if (!sql) return -1;
    if (view == LIB_FOLDER && parent == 0)     /* (0: the top folder's, as library_list) */
        parent = db_int(lib->db, "SELECT id FROM folders WHERE parent=0 ORDER BY length(path) LIMIT 1", 0, 0);
    char *q = sqlite3_mprintf("SELECT COUNT(*) FROM (%s)", sql);
    sqlite3_stmt *s = q ? prep(lib->db, q) : NULL;
    sqlite3_free(q);
    if (!s) return -1;
    if (view == LIB_SMART) {
        if (cut) sqlite3_bind_int64(s, 1, cut);
    } else {
        sqlite3_bind_int64(s, 1, parent);      /* (a view with no parent has no ?1: nothing bound) */
        sqlite3_bind_int64(s, 2, g_credit_album);
    }
    int n = sqlite3_step(s) == SQLITE_ROW ? sqlite3_column_int(s, 0) : -1;
    sqlite3_finalize(s);
    return n;
}

int library_smart_count(library_t *lib, int which) { return library_count(lib, LIB_SMART, which); }

int library_scripts(library_t *lib, lib_scripts_t *s)
{
    memset(s, 0, sizeof *s);
    if (!lib) return -1;
    sqlite3_stmt *st = prep(lib->db, "SELECT title, artist, album, album_artist FROM tracks");
    if (!st) return -1;
    int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        const char *artist = (const char *)sqlite3_column_text(st, 1), *aa = (const char *)sqlite3_column_text(st, 3);
        char both[512];
        /* (the artist and the album's artist as one name: either may be the one in another writing) */
        snprintf(both, sizeof both, "%s %s", artist ? artist : "", aa && (!artist || strcmp(aa, artist)) ? aa : "");
        scripts_add(s, (const char *)sqlite3_column_text(st, 0), both, (const char *)sqlite3_column_text(st, 2));
    }
    sqlite3_finalize(st);
    /* a pass that stopped before its end is not the library's count */
    if (rc != SQLITE_DONE) { memset(s, 0, sizeof *s); return -1; }
    return 0;
}

int library_empty(library_t *lib)
{
    return get_int_col(lib, NO_SONGS_SQL, 1) != 0;
}

int library_needs_art(library_t *lib)
{
    /* (art=0: never looked at, a scan's cover pass cut short by a stop or a restart) */
    return get_int_col(lib, "SELECT EXISTS(SELECT 1 FROM albums WHERE art=0 OR (art=2 AND COALESCE(art_sig, 0)=0))", 0) > 0;
}

int library_list_composer_tracks(library_t *lib, int64_t composer_id, int64_t album_id, lib_list_t *out)
{
    return list_on_album(lib, LIB_COMPOSER_TRACKS, composer_id, album_id, out);
}

void library_artist_counts(library_t *lib, int64_t artist_id, int *nalbums, int *ncredit_albums)
{
    *nalbums = *ncredit_albums = 0;
    sqlite3_stmt *s = lib->artist_counts;
    sqlite3_reset(s);
    sqlite3_bind_int64(s, 1, artist_id);
    if (sqlite3_step(s) == SQLITE_ROW) {
        *nalbums = sqlite3_column_int(s, 0);
        *ncredit_albums = sqlite3_column_int(s, 1);
    }
    sqlite3_reset(s);   /* (not left on its row: see library_row) */
}

/* The music files a scan couldn't read: damaged ones, and those set aside
 * because the player stopped while reading them */
int library_unread_count(library_t *lib)
{
    return lib ? (int)get_int_col(lib, "SELECT COUNT(*) FROM cues WHERE kind=3", 0) : 0;
}

int library_unread(library_t *lib, int i, char *path, size_t cap)
{
    sqlite3_stmt *q = lib && i >= 0 ? lib->unread_at : NULL;
    int r = -1;
    if (!q) return -1;
    sqlite3_reset(q);
    sqlite3_bind_int(q, 1, i);
    if (sqlite3_step(q) == SQLITE_ROW && sqlite3_column_text(q, 0)) {
        snprintf(path, cap, "%s", (const char *)sqlite3_column_text(q, 0));
        r = 0;
    }
    sqlite3_reset(q);   /* (not left on its row: see library_row) */
    return r;
}

void library_list_free(lib_list_t *l)
{
    free(l->ids);
    memset(l, 0, sizeof *l);
}

int library_row(library_t *lib, lib_view_t view, int64_t id,
                char *main, size_t main_cap, char *meta, size_t meta_cap)
{
    if (view == LIB_FOLDER && id >= 0) view = LIB_SONGS;      /* a song in a folder */
    sqlite3_stmt *s = view == LIB_ALBUM_ARTISTS ? lib->row_artist
                    : view == LIB_ARTISTS ? lib->row_credited
                    : view == LIB_GENRES ? lib->row_genre
                    : view == LIB_COMPOSERS ? lib->row_composer
                    : view == LIB_YEARS ? lib->row_year
                    : view == LIB_FOLDER ? lib->row_folder
                    : (view == LIB_ALBUM_TRACKS || view == LIB_SONGS || view == LIB_CREDIT_TRACKS ||
                       view == LIB_COMPOSER_TRACKS) ? lib->row_track
                    : view == LIB_PLAYLISTS ? lib->row_playlist
                    : view == LIB_MOST_PLAYED ? lib->row_plays
                    : (view == LIB_PLAYLIST_TRACKS || view == LIB_FAVOURITES || view == LIB_RECENT_PLAYED ||
                       view == LIB_SMART) ? lib->row_pl_track
                    : lib->row_album;   /* albums, artist albums, recently added */
    sqlite3_reset(s);
    sqlite3_bind_int64(s, 1, view == LIB_FOLDER ? -id : id);
    main[0] = meta[0] = '\0';
    if (sqlite3_step(s) != SQLITE_ROW) { sqlite3_reset(s); return -1; }
    copy_col(s, 0, main, main_cap);
    int v = sqlite3_column_int(s, 1);
    int track_list = view == LIB_ALBUM_TRACKS || view == LIB_SONGS || view == LIB_PLAYLIST_TRACKS || view == LIB_CREDIT_TRACKS ||
                     view == LIB_FAVOURITES || view == LIB_RECENT_PLAYED || view == LIB_MOST_PLAYED ||
                     view == LIB_COMPOSER_TRACKS || view == LIB_SMART;
    if (view == LIB_ARTISTS || view == LIB_ALBUM_ARTISTS || view == LIB_PLAYLISTS || view == LIB_GENRES ||
        view == LIB_COMPOSERS || view == LIB_YEARS)
        snprintf(meta, meta_cap, "%d", v);                                                   /* a count */
    else if (view == LIB_FOLDER || view == LIB_MOST_PLAYED) {                               /* (the UI's words) */
        char n[16];
        snprintf(n, sizeof n, "%d", v);
        if (view == LIB_FOLDER) tr_fill(meta, meta_cap, v == 1 ? "1 song" : "%s songs", n, NULL);
        else tr_fill(meta, meta_cap, v == 1 ? "1 play" : "%s plays", n, NULL);
    }
    else if (track_list) snprintf(meta, meta_cap, "%d:%02d", v / 60000, (v / 1000) % 60);   /* length */
    else if (library_row_shows_artist(view) && !lib->no_album_artists && sqlite3_column_text(s, 2) &&
             *sqlite3_column_text(s, 2))
        copy_col(s, 2, meta, meta_cap);                                                      /* album artist */
    else if (v > 0) snprintf(meta, meta_cap, "%d", v);                                       /* album year */
    if (view == LIB_PLAYLIST_TRACKS || view == LIB_FAVOURITES || view == LIB_RECENT_PLAYED ||
        view == LIB_MOST_PLAYED || view == LIB_SMART) {   /* a mix of artists: "Title · Artist" */
        const unsigned char *a = sqlite3_column_text(s, 2);
        size_t l = strlen(main);
        if (a && *a && l + 4 < main_cap) snprintf(main + l, main_cap - l, " \xC2\xB7 %s", (const char *)a);
    }
    /* Done with the row: reset now. A kept statement left on its row keeps
     * its read transaction open, which in WAL pins this connection to that
     * snapshot: another connection's commits (a scan, levelling) went unseen,
     * and this one's own writes failed with SQLITE_BUSY_SNAPSHOT (favourites,
     * play counts, playlists lost without a word). */
    sqlite3_reset(s);
    return 0;
}

void library_set_album_artists(library_t *lib, int on)
{
    if (lib) lib->no_album_artists = !on;
}

int64_t library_track_album(library_t *lib, int64_t track_id)
{
    sqlite3_stmt *s = lib->track_album;
    sqlite3_reset(s);
    sqlite3_bind_int64(s, 1, track_id);
    int64_t a = sqlite3_step(s) == SQLITE_ROW ? sqlite3_column_int64(s, 0) : 0;
    sqlite3_reset(s);
    return a;
}

int library_track(library_t *lib, int64_t id, lib_track_t *t)
{
    sqlite3_stmt *s = lib->track_full;
    sqlite3_reset(s);
    sqlite3_bind_int64(s, 1, id);
    memset(t, 0, sizeof *t);
    if (sqlite3_step(s) != SQLITE_ROW) return -1;
    t->id = id;
    copy_col(s, 0, t->path, sizeof t->path);
    copy_col(s, 1, t->title, sizeof t->title);
    copy_col(s, 2, t->artist, sizeof t->artist);
    copy_col(s, 3, t->album, sizeof t->album);
    t->album_id = sqlite3_column_int64(s, 4);
    t->artist_id = sqlite3_column_int64(s, 5);
    t->disc = sqlite3_column_int(s, 6);
    t->track = sqlite3_column_int(s, 7);
    t->year = sqlite3_column_int(s, 8);
    copy_col(s, 9, t->codec, sizeof t->codec);
    t->sample_rate = (uint32_t)sqlite3_column_int(s, 10);
    t->bits = (uint8_t)sqlite3_column_int(s, 11);
    t->channels = (uint8_t)sqlite3_column_int(s, 12);
    t->duration_ms = (uint32_t)sqlite3_column_int64(s, 13);
    sqlite3_reset(s);   /* (see library_row) */
    return 0;
}

static int name_query(sqlite3_stmt *s, int64_t id, char *out, size_t cap);

int library_genre_name(library_t *lib, int64_t id, char *out, size_t cap) { return name_query(lib->genre_name, id, out, cap); }
int library_composer_name(library_t *lib, int64_t id, char *out, size_t cap) { return name_query(lib->composer_name, id, out, cap); }

int library_folder_name(library_t *lib, int64_t id, char *out, size_t cap)
{
    if (id == 0) { snprintf(out, cap, "Folders"); return 0; }
    return name_query(lib->folder_name, id, out, cap);
}

static int name_query(sqlite3_stmt *s, int64_t id, char *out, size_t cap)
{
    sqlite3_reset(s);
    sqlite3_bind_int64(s, 1, id);
    out[0] = '\0';
    int ok = sqlite3_step(s) == SQLITE_ROW;
    if (ok) copy_col(s, 0, out, cap);
    sqlite3_reset(s);   /* (a statement left on its row keeps a read open: see library_row) */
    return ok ? 0 : -1;
}

int library_album_name(library_t *lib, int64_t id, char *out, size_t cap) { return name_query(lib->album_name, id, out, cap); }
int library_artist_name(library_t *lib, int64_t id, char *out, size_t cap) { return name_query(lib->artist_name, id, out, cap); }
int library_playlist_name(library_t *lib, int64_t id, char *out, size_t cap) { return name_query(lib->playlist_name, id, out, cap); }

int64_t library_album_artist(library_t *lib, int64_t album_id)
{
    return get_int_col(lib, "SELECT artist_id FROM albums WHERE id=?", album_id);
}

/* s's row (SHUF_COLS) in shuffle form; a work's title prefix hashed into unit for shuf_works */
static void shuf_row(sqlite3_stmt *s, shuf_track_t *t)
{
    memset(t, 0, sizeof *t);
    t->id = (uint32_t)sqlite3_column_int64(s, 0);
    t->artist = (uint32_t)sqlite3_column_int64(s, 1);
    t->album = (uint32_t)sqlite3_column_int64(s, 2);
    t->disc = (uint16_t)sqlite3_column_int(s, 3);
    t->track = (uint16_t)sqlite3_column_int(s, 4);
    t->duration_ms = (uint32_t)sqlite3_column_int64(s, 5);
    t->last_played = sqlite3_column_int64(s, 6);
    t->play_count = (uint16_t)sqlite3_column_int(s, 7);
    t->skip_count = (uint16_t)sqlite3_column_int(s, 8);
    const char *g = (const char *)sqlite3_column_text(s, 9);
    if (g && (strcasestr(g, "audiobook") || strcasestr(g, "podcast") || strcasestr(g, "spoken")))
        t->flags |= SHUF_F_SPOKEN;
    /* A work in several parts ("Symphony No. 5 in C minor: I. Allegro"): the
     * title before ": ", for shuf_works (kept in unit for now) */
    const char *ti = (const char *)sqlite3_column_text(s, 10);
    const char *c = ti ? strstr(ti, ": ") : NULL;
    if (c && c - ti >= 4) {
        uint32_t h = fnv32(FNV32_START, ti, (size_t)(c - ti));
        t->unit = h ? h : 1;
    }
}

/* Works: consecutive tracks of an album with the same title prefix, two or
 * more of them, share a unit (shuffle keeps them together when asked to);
 * a prefix on its own is no work. t is in album, disc, track order. */
static void shuf_works(shuf_track_t *first, size_t stride, int n)
{
#define SHUF_AT(i) ((shuf_track_t *)((char *)first + (size_t)(i) * stride))   /* (each inside a larger item, or not) */
    for (int i = 0; i < n;) {
        int j = i + 1;
        while (j < n && SHUF_AT(i)->unit && SHUF_AT(j)->unit == SHUF_AT(i)->unit && SHUF_AT(j)->album == SHUF_AT(i)->album) j++;
        uint32_t u = j - i >= 2 ? (SHUF_AT(i)->id ? SHUF_AT(i)->id : 1) : 0;   /* the first part's id: unique */
        for (int k = i; k < j; k++) SHUF_AT(k)->unit = u;
        i = j;
    }
#undef SHUF_AT
}

int library_shuffle_tracks(library_t *lib, shuf_track_t **out)
{
    sqlite3_stmt *s = prep(lib->db, SHUFFLE_ALL_SQL);
    *out = NULL;
    if (!s) return -1;
    int n = 0, cap = 0;
    while (sqlite3_step(s) == SQLITE_ROW) {
        shuf_track_t *nt = vec_room(*out, n, &cap, 1024, sizeof *nt);
        if (!nt) break;
        *out = nt;
        shuf_row(s, &(*out)[n++]);
    }
    sqlite3_finalize(s);
    shuf_works(*out, sizeof **out, n);
    return n;
}

int64_t library_top_track(library_t *lib) { return get_int_col(lib, "SELECT COALESCE(MAX(id), 0) FROM tracks", 0); }
int64_t library_track_from(library_t *lib, int64_t id)
{
    return get_int_col(lib, "SELECT id FROM tracks WHERE id>=? ORDER BY id LIMIT 1", id);
}

typedef struct { shuf_track_t t; int at; } shuf_at_t;

static int cmp_shuf_album(const void *a, const void *b)
{
    const shuf_track_t *x = &((const shuf_at_t *)a)->t, *y = &((const shuf_at_t *)b)->t;
    if (x->album != y->album) return x->album < y->album ? -1 : 1;
    if (x->disc != y->disc) return x->disc < y->disc ? -1 : 1;
    if (x->track != y->track) return x->track < y->track ? -1 : 1;
    return x->id < y->id ? -1 : x->id > y->id;
}

int library_shuffle_tracks_for(library_t *lib, const int64_t *ids, int n, shuf_track_t *out)
{
    sqlite3_stmt *s = prep(lib->db, "SELECT " SHUF_COLS " FROM tracks WHERE id=?");
    shuf_at_t *w = n > 0 ? malloc((size_t)n * sizeof *w) : NULL;
    if (!s || !w) { sqlite3_finalize(s); free(w); return -1; }
    for (int i = 0; i < n; i++) {
        sqlite3_reset(s);
        sqlite3_bind_int64(s, 1, ids[i]);
        if (sqlite3_step(s) == SQLITE_ROW) shuf_row(s, &w[i].t);
        else { memset(&w[i].t, 0, sizeof w[i].t); w[i].t.id = (uint32_t)ids[i]; }   /* (gone: as a song nothing is known of) */
        w[i].at = i;
    }
    sqlite3_finalize(s);
    /* works, as library_shuffle_tracks finds them: among these songs, in album order */
    qsort(w, (size_t)n, sizeof *w, cmp_shuf_album);
    shuf_works(&w[0].t, sizeof *w, n);
    for (int i = 0; i < n; i++) out[w[i].at] = w[i].t;
    free(w);
    return n;
}

void library_set_favourite(library_t *lib, int64_t id, int on)
{
    set_int_col(lib, "UPDATE tracks SET favourite=?1 WHERE id=?2", id, on ? 1 : 0);
}

int library_favourite(library_t *lib, int64_t id)
{
    return (int)get_int_col(lib, "SELECT favourite FROM tracks WHERE id=?1", id);
}

void library_set_resume(library_t *lib, int64_t id, uint32_t ms)
{
    set_int_col(lib, "UPDATE tracks SET resume_ms=?1 WHERE id=?2", id, ms);
}

uint32_t library_resume(library_t *lib, int64_t id)
{
    return (uint32_t)get_int_col(lib, "SELECT resume_ms FROM tracks WHERE id=?1", id);
}
