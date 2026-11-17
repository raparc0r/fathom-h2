/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * CUE sheets: an album ripped as one file, with a .cue next to it (or the
 * sheet in the file's own CUESHEET tag), becomes the sheet's tracks.
 *
 * Each track is a part of the file, from its INDEX 01 to the next track's
 * (its pregap, INDEX 00, belongs to the track before, as on the CD), and
 * has a path of its own that says so: "<file>::NN@START-END", START and END
 * in CD frames (1/75 s), END empty for the file's end. The library keeps
 * tracks by that path (it stays the same while the sheet's times do), and
 * decoder_open() plays just that part of the file.
 */
#ifndef FATHOM_CUE_H
#define FATHOM_CUE_H

#include <stddef.h>
#include <stdint.h>

#define CUE_FPS        75     /* CD frames per second */
#define CUE_MAX_TRACKS 99
#define CUE_MAX_FILES  16
#define CUE_MAX_BYTES  (256 * 1024)   /* a sheet bigger than this is not a sheet */

typedef struct {
    int      number;          /* TRACK nn */
    int      file;            /* the FILE its INDEX 01 is in (files[]) */
    uint32_t start;           /* INDEX 01, CD frames */
    char     title[256], performer[256], songwriter[128];
    int      has_rg;          /* REM REPLAYGAIN_TRACK_GAIN (dB, -18 LUFS) */
    float    rg_gain, rg_peak;
} cue_track_t;

typedef struct {
    char  title[256], performer[256], songwriter[128], genre[64];
    int   year, disc;         /* REM DATE, REM DISCNUMBER; 0 = none */
    int   has_rg_album;
    float rg_album_gain, rg_album_peak;
    char  files[CUE_MAX_FILES][256];   /* as written (UTF-8) */
    int   nfiles;
    cue_track_t tracks[CUE_MAX_TRACKS];   /* in sheet order; each file's in time order */
    int   n;
} cue_sheet_t;   /* ~65 KB: on the heap */

/* A sheet's text (UTF-8 with or without a BOM, UTF-16 with a BOM, else
 * Windows-1252/Latin-1): its tracks with an INDEX 01, each later than the one
 * before in its file. Returns the number of tracks, or -1. */
int cue_parse(const char *text, size_t len, cue_sheet_t *out);
int cue_read(const char *path, cue_sheet_t *out);
/* The text as UTF-8 (malloc'd, NUL-terminated; *out_len without the NUL), or NULL. */
char *cue_to_utf8(const char *text, size_t len, size_t *out_len);

int cue_supported(const char *path);   /* a .cue file */
/* How many of the sheet's tracks are in file f. */
int cue_file_tracks(const cue_sheet_t *s, int f);

/* A track's path: file, track number, INDEX 01 and the next track's (0: the end). */
void cue_part_path(char *out, size_t cap, const char *file, int track, uint32_t start, uint32_t end);
/* Where a track's path has its "::NN@..." (the file is what's before), or
 * NULL if it's an ordinary file's path. */
const char *cue_part_suffix(const char *path);
/* A track's path taken apart: the track number (> 0), or 0 if it's an
 * ordinary file's path (then nothing is written). */
int cue_part_parse(const char *path, char *file, size_t cap, uint32_t *start, uint32_t *end);

#endif
