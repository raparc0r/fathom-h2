/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* Volume levelling's side of the library: the next song to measure, what was
 * measured put with its row, how far the measuring has got, and the gain a
 * song (or its album) is played at. Out of library.c, as it was there. */
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE
#define _GNU_SOURCE
#include "library/library_int.h"

#include <math.h>

/* ---------- volume levelling (ReplayGain) ---------- */

/* (By tracks_rg_todo, which holds only the tracks left: the first of them
 * at once, where a walk in id order went past every one done.) */
int library_rg_next(library_t *lib, int64_t *id, char *path, size_t cap)
{
    sqlite3_stmt *s = prep(lib->db, "SELECT id, path FROM tracks WHERE rg_state=0 ORDER BY id LIMIT 1");
    int rc = -1;
    if (s && sqlite3_step(s) == SQLITE_ROW) {
        *id = sqlite3_column_int64(s, 0);
        copy_col(s, 1, path, cap);
        rc = 0;
    }
    sqlite3_finalize(s);
    return rc;
}

void library_rg_store(library_t *lib, int64_t id, const lib_rg_t *rg)
{
    sqlite3_stmt *s = prep(lib->db,
        "UPDATE tracks SET rg_state=?, rg_track=?, rg_track_peak=?, rg_album=?, rg_album_peak=? WHERE id=?");
    if (!s) return;
    sqlite3_bind_int(s, 1, rg->state);
    if (rg->state == 1 || rg->state == 2) sqlite3_bind_double(s, 2, rg->track_gain); else sqlite3_bind_null(s, 2);
    if (rg->track_peak > 0) sqlite3_bind_double(s, 3, rg->track_peak); else sqlite3_bind_null(s, 3);
    if (rg->has_album) sqlite3_bind_double(s, 4, rg->album_gain); else sqlite3_bind_null(s, 4);
    if (rg->has_album && rg->album_peak > 0) sqlite3_bind_double(s, 5, rg->album_peak); else sqlite3_bind_null(s, 5);
    sqlite3_bind_int64(s, 6, id);
    int nice = lock_boost();
    sqlite3_step(s);
    lock_unboost(nice);
    sqlite3_finalize(s);
}

void library_rg_progress(library_t *lib, int *done, int *total)
{
    *done = *total = 0;
    sqlite3_stmt *s = prep(lib->db, "SELECT COUNT(*) - (SELECT COUNT(*) FROM tracks WHERE rg_state=0), COUNT(*) FROM tracks");
    if (s && sqlite3_step(s) == SQLITE_ROW) {
        *done = sqlite3_column_int(s, 0);
        *total = sqlite3_column_int(s, 1);
    }
    sqlite3_finalize(s);
}

/* The gain (dB) and peak for a track: its own, or its album's (from the album
 * tag, or combined from the album's tracks, each weighted by its length, once
 * all of them are known). -1 if unknown. */
static int rg_lookup(library_t *lib, int64_t id, int album, double *gain, double *peak)
{
    sqlite3_stmt *s = prep(lib->db,
        "SELECT rg_state, rg_track, rg_track_peak, rg_album, rg_album_peak, album_id FROM tracks WHERE id=?");
    if (!s) return -1;
    sqlite3_bind_int64(s, 1, id);
    int rc = -1;
    int64_t album_id = 0;
    if (sqlite3_step(s) == SQLITE_ROW) {
        int st = sqlite3_column_int(s, 0);
        album_id = sqlite3_column_int64(s, 5);
        if (album && sqlite3_column_type(s, 3) != SQLITE_NULL) {
            *gain = sqlite3_column_double(s, 3);
            *peak = sqlite3_column_type(s, 4) != SQLITE_NULL ? sqlite3_column_double(s, 4) : 0;
            rc = 0;
        } else if (st == 1 || st == 2) {
            *gain = sqlite3_column_double(s, 1);
            *peak = sqlite3_column_type(s, 2) != SQLITE_NULL ? sqlite3_column_double(s, 2) : 0;
            rc = album ? 1 : 0;      /* album mode: try the album's tracks below */
        }
    }
    sqlite3_finalize(s);
    if (rc != 1) return rc;

    /* the album's loudness: its tracks' mean energy, weighted by length */
    s = prep(lib->db, "SELECT rg_state, rg_track, duration_ms, rg_track_peak FROM tracks WHERE album_id=?");
    if (!s) return 0;
    sqlite3_bind_int64(s, 1, album_id);
    double energy = 0, ms = 0, apeak = 0;
    int all = 1;
    while (sqlite3_step(s) == SQLITE_ROW) {
        int st = sqlite3_column_int(s, 0);
        if (st != 1 && st != 2) { all = 0; break; }
        double d = sqlite3_column_double(s, 2);
        energy += d * pow(10.0, (-18.0 - sqlite3_column_double(s, 1)) / 10.0);
        ms += d;
        if (sqlite3_column_double(s, 3) > apeak) apeak = sqlite3_column_double(s, 3);
    }
    sqlite3_finalize(s);
    if (all && ms > 0 && energy > 0) {
        *gain = -18.0 - 10.0 * log10(energy / ms);
        *peak = apeak;
    }
    return 0;
}

float library_rg_gain(library_t *lib, int64_t id, int album)
{
    double gain, peak;
    if (rg_lookup(lib, id, album, &gain, &peak) < 0) return 1.0f;
    if (peak > 0 && gain > -20.0 * log10(peak)) gain = -20.0 * log10(peak);   /* never clip */
    return (float)pow(10.0, gain / 20.0);
}
