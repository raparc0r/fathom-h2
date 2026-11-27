/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* Your own playlists: made, added to, reordered, renamed and removed on the
 * player, each change written to the playlist's file as well as its rows.
 * (How a scan reads playlists' files is still in library.c, with the scan.)
 * Out of library.c, as it was there. */
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE
#define _GNU_SOURCE
#include "library/library_int.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "core/fsafe.h"

/* ---------- your own playlists ---------- */

void library_set_playlist_dir(library_t *lib, const char *dir)
{
    snprintf(lib->pl_dir, sizeof lib->pl_dir, "%s", dir ? dir : "");
    if (lib->pl_dir[0]) mkdir(lib->pl_dir, 0755);
}

int library_playlist_writable(library_t *lib, int64_t id)
{
    return (int)get_int_col(lib, "SELECT writable FROM playlists WHERE id=?1", id);
}

int library_playlist_count_writable(library_t *lib)
{
    return (int)get_int_col(lib, "SELECT COUNT(*) FROM playlists WHERE writable=1", 0);
}

/* `to` relative to the folder `from_dir` (both absolute): "../../Music/A/B/c.flac". */
static void rel_path(const char *from_dir, const char *to, char *out, size_t cap)
{
    size_t i = 0, common = 0;             /* longest common prefix, ending at a '/' */
    while (from_dir[i] && to[i] && from_dir[i] == to[i]) {
        i++;
        if (to[i - 1] == '/') common = i;
    }
    if (!from_dir[i] && to[i] == '/') common = i + 1;   /* `to` is inside from_dir */
    int ups = 0;                          /* folders of from_dir below the common part */
    const char *rest = from_dir + common;
    if (*rest) {
        ups = 1;
        for (const char *c = rest; *c; c++) ups += *c == '/';
    }
    size_t w = 0;
    for (int k = 0; k < ups && w + 4 < cap; k++, w += 3) memcpy(out + w, "../", 3);
    snprintf(out + w, cap - w, "%s", to + common);
}

/* Write a playlist's file from its rows (atomically). */
static int write_playlist(library_t *lib, int64_t id)
{
    sqlite3_stmt *q = prep(lib->db, "SELECT path, name FROM playlists WHERE id=? AND writable=1");
    if (!q) return -1;
    sqlite3_bind_int64(q, 1, id);
    char path[1100], name[256], dir[1100];
    if (sqlite3_step(q) != SQLITE_ROW) { sqlite3_finalize(q); return -1; }
    copy_col(q, 0, path, sizeof path);
    copy_col(q, 1, name, sizeof name);
    sqlite3_finalize(q);
    dir_of(path, dir, sizeof dir);

    char tmp[1200];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "we");
    if (!f) return -1;
    fprintf(f, "#EXTM3U\n#PLAYLIST:%s\n", name);
    sqlite3_stmt *it = prep(lib->db,
        "SELECT t.path, t.artist, t.title, t.duration_ms FROM playlist_items i JOIN tracks t ON t.id=i.track_id"
        " WHERE i.playlist_id=? ORDER BY i.pos");
    /* Whole, or not at all: a query that couldn't be made, or stopped before
     * its end (no memory, a read error), is not the list's end. The file went
     * over the playlist with what had come, the header alone if nothing had,
     * and the next scan read it and dropped the songs from the library too. */
    int whole = 0;
    if (it) {
        sqlite3_bind_int64(it, 1, id);
        int rc;
        while ((rc = sqlite3_step(it)) == SQLITE_ROW) {
            char rel[1200];
            const char *tp = (const char *)sqlite3_column_text(it, 0);
            if (!tp) { rc = SQLITE_NOMEM; break; }
            rel_path(dir, tp, rel, sizeof rel);
            const unsigned char *a = sqlite3_column_text(it, 1), *ti = sqlite3_column_text(it, 2);
            fprintf(f, "#EXTINF:%lld,%s - %s\n%s\n", (long long)(sqlite3_column_int64(it, 3) / 1000),
                    a ? (const char *)a : "", ti ? (const char *)ti : "", rel);
        }
        whole = rc == SQLITE_DONE;
        sqlite3_finalize(it);
    }
    if (file_commit(f, whole, tmp, path)) return -1;
    return 0;
}

static void renumber(library_t *lib, int64_t id)
{
    /* positions 0..n-1 again after a removal */
    sqlite3_stmt *q = prep(lib->db, "SELECT rowid FROM playlist_items WHERE playlist_id=? ORDER BY pos");
    sqlite3_stmt *u = prep(lib->db, "UPDATE playlist_items SET pos=? WHERE rowid=?");
    if (q && u) {
        sqlite3_bind_int64(q, 1, id);
        int pos = 0;
        while (sqlite3_step(q) == SQLITE_ROW) {
            sqlite3_reset(u);
            sqlite3_bind_int(u, 1, pos++);
            sqlite3_bind_int64(u, 2, sqlite3_column_int64(q, 0));
            sqlite3_step(u);
        }
    }
    sqlite3_finalize(q);
    sqlite3_finalize(u);
    db_run_id(lib->db, "UPDATE playlists SET ntracks=(SELECT COUNT(*) FROM playlist_items WHERE playlist_id=?1)"
                       " WHERE id=?1", id);
}

static void playlist_path(library_t *lib, const char *name, const char *keep, char *out, size_t cap);

int64_t library_playlist_create(library_t *lib, const char *name)
{
    if (!lib->pl_dir[0]) return 0;
    char path[1400], fname[256];
    playlist_path(lib, name, NULL, path, sizeof path);   /* never over another playlist's file */
    const char *slash = strrchr(path, '/');
    snprintf(fname, sizeof fname, "%.255s", slash ? slash + 1 : path);
    sqlite3_stmt *s = prep(lib->db, "INSERT INTO playlists(path, name, sort, ntracks, seen, writable) VALUES(?,?,?,0,1,1)");
    if (!s) return 0;
    char sort[256];
    sort_key(fname, sort, sizeof sort);   /* (its file's name cut to 255 bytes first: not playlist_sort_key, which a very long name starting with punctuation would sort a byte further) */
    sqlite3_bind_text(s, 1, path, -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 2, name, -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 3, sort, -1, SQLITE_STATIC);
    int ok = sqlite3_step(s) == SQLITE_DONE;
    sqlite3_finalize(s);
    if (!ok) return 0;
    int64_t id = sqlite3_last_insert_rowid(lib->db);
    if (write_playlist(lib, id)) { library_playlist_delete(lib, id); return 0; }
    return id;
}

int library_playlist_add(library_t *lib, int64_t id, const int64_t *tracks, int n)
{
    if (!library_playlist_writable(lib, id)) return -1;
    int pos = (int)get_int_col(lib, "SELECT COUNT(*) FROM playlist_items WHERE playlist_id=?1", id);
    sqlite3_stmt *s = prep(lib->db, "INSERT INTO playlist_items(playlist_id, pos, track_id) VALUES(?,?,?)");
    if (!s) return -1;
    sqlite3_exec(lib->db, "BEGIN", NULL, NULL, NULL);
    for (int i = 0; i < n; i++) {
        sqlite3_reset(s);
        sqlite3_bind_int64(s, 1, id);
        sqlite3_bind_int(s, 2, pos++);
        sqlite3_bind_int64(s, 3, tracks[i]);
        sqlite3_step(s);
    }
    sqlite3_finalize(s);
    renumber(lib, id);
    sqlite3_exec(lib->db, "COMMIT", NULL, NULL, NULL);
    return write_playlist(lib, id);
}

int library_playlist_remove(library_t *lib, int64_t id, int pos)
{
    if (!library_playlist_writable(lib, id)) return -1;
    sqlite3_stmt *s = prep(lib->db, "DELETE FROM playlist_items WHERE playlist_id=? AND pos=?");
    if (!s) return -1;
    sqlite3_bind_int64(s, 1, id);
    sqlite3_bind_int(s, 2, pos);
    sqlite3_step(s);
    sqlite3_finalize(s);
    renumber(lib, id);
    return write_playlist(lib, id);
}

int library_playlist_move(library_t *lib, int64_t id, int pos, int dir)
{
    if (!library_playlist_writable(lib, id)) return -1;
    int other = pos + dir, n = (int)get_int_col(lib, "SELECT COUNT(*) FROM playlist_items WHERE playlist_id=?1", id);
    if (pos < 0 || other < 0 || pos >= n || other >= n) return -1;
    /* swap the two positions through a temporary one */
    sqlite3_stmt *s = prep(lib->db, "UPDATE playlist_items SET pos=?3 WHERE playlist_id=?1 AND pos=?2");
    if (!s) return -1;
    int steps[3][2] = {{pos, -1}, {other, pos}, {-1, other}};
    for (int i = 0; i < 3; i++) {
        sqlite3_reset(s);
        sqlite3_bind_int64(s, 1, id);
        sqlite3_bind_int(s, 2, steps[i][0]);
        sqlite3_bind_int(s, 3, steps[i][1]);
        sqlite3_step(s);
    }
    sqlite3_finalize(s);
    return write_playlist(lib, id);
}

/* A file name for a playlist name, in pl_dir; " 2", " 3"... if taken by another file. */
static void playlist_path(library_t *lib, const char *name, const char *keep, char *out, size_t cap)
{
    char fname[256];
    size_t w = 0;
    for (const char *c = name; *c && w + 1 < sizeof fname; c++)
        fname[w++] = (*c == '/' || *c == '\\' || *c == ':') ? '-' : *c;
    fname[w] = '\0';
    for (int n = 1; n < 100; n++) {
        if (n == 1) snprintf(out, cap, "%s/%s.m3u8", lib->pl_dir, fname);
        else snprintf(out, cap, "%s/%s %d.m3u8", lib->pl_dir, fname, n);
        if ((keep && !strcmp(out, keep)) || access(out, F_OK) != 0) return;
    }
}

int library_playlist_rename(library_t *lib, int64_t id, const char *name)
{
    if (!library_playlist_writable(lib, id) || !name[0]) return -1;
    sqlite3_stmt *q = prep(lib->db, "SELECT path FROM playlists WHERE id=?");
    if (!q) return -1;
    sqlite3_bind_int64(q, 1, id);
    char old[1100] = "", path[1400];
    if (sqlite3_step(q) == SQLITE_ROW) copy_col(q, 0, old, sizeof old);
    sqlite3_finalize(q);
    if (!old[0]) return -1;
    playlist_path(lib, name, old, path, sizeof path);
    if (strcmp(path, old) && rename(old, path)) return -1;
    char sort[256];
    playlist_sort_key(path, sort, sizeof sort);
    sqlite3_stmt *u = prep(lib->db, "UPDATE playlists SET path=?, name=?, sort=? WHERE id=?");
    if (!u) return -1;
    sqlite3_bind_text(u, 1, path, -1, SQLITE_STATIC);
    sqlite3_bind_text(u, 2, name, -1, SQLITE_STATIC);
    sqlite3_bind_text(u, 3, sort, -1, SQLITE_STATIC);
    sqlite3_bind_int64(u, 4, id);
    sqlite3_step(u);
    sqlite3_finalize(u);
    return write_playlist(lib, id);   /* #PLAYLIST: the new name */
}

int library_playlist_delete(library_t *lib, int64_t id)
{
    sqlite3_stmt *q = prep(lib->db, "SELECT path FROM playlists WHERE id=? AND writable=1");
    if (!q) return -1;
    sqlite3_bind_int64(q, 1, id);
    char path[1100] = "";
    if (sqlite3_step(q) == SQLITE_ROW) copy_col(q, 0, path, sizeof path);
    sqlite3_finalize(q);
    if (!path[0]) return -1;
    unlink(path);
    db_run_id(lib->db, "DELETE FROM playlist_items WHERE playlist_id=?1", id);
    db_run_id(lib->db, "DELETE FROM playlists WHERE id=?1", id);
    return 0;
}
