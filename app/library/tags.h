/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* Read tags, stream format and embedded cover art from audio files. */
#ifndef FATHOM_TAGS_H
#define FATHOM_TAGS_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    char title[256];
    char artist[256];
    char album_artist[256];
    char album[256];
    char genre[64];
    char composer[256];
    int  year, disc, track;

    char     codec[8];      /* "FLAC", "MP3", ... */
    uint32_t sample_rate;
    uint8_t  bits;          /* 0 for lossy codecs */
    uint8_t  channels;
    uint32_t duration_ms;
    int      has_picture;   /* embedded front cover present */
    int      has_cuesheet;  /* a CUE sheet in a tag (FLAC's CUESHEET, APEv2's Cuesheet): tags_cuesheet() */

    /* ReplayGain (dB, referred to -18 LUFS; peaks 0..1 of full scale), when
     * tagged: REPLAYGAIN_* (FLAC, MP3 TXXX) or R128_* (moved to -18 LUFS). */
    int      has_rg_track, has_rg_album;
    float    rg_track_gain, rg_track_peak, rg_album_gain, rg_album_peak;
} tags_t;

/* Fill `t` from the file. Returns 0 on success, -1 if the file can't be read
 * (a CUE track's path, library/cue.h, isn't a file: -1). */
int tags_read(const char *path, tags_t *t);
/* The CUE sheet in the file's tags, malloc'd and NUL-terminated (its length
 * in *len), or -1. */
int tags_cuesheet(const char *path, char **text, size_t *len);
/* The lyrics in the file's tags (FLAC's LYRICS and UNSYNCEDLYRICS, an MP3's
 * USLT, an M4A's...), malloc'd and NUL-terminated, as tagged: plain text, or
 * an LRC's with its times (library/lyrics.h reads either). Of several, one
 * with times. -1: none (or a CUE track's path: the tags are its album's). */
int tags_lyrics(const char *path, char **text, size_t *len);
/* A FLAC through libFLAC, as tags_read() did before its own one-pass reader:
 * to check that reader against (tools/tags-compare). */
int tags_read_flac_reference(const char *path, tags_t *t);
/* MP3 length: 1 (default) walks every frame header for an exact length; 0 uses
 * the Xing/Info header or the bitrate (fast on slow storage, VBR files without
 * a Xing header come out approximate). */
void tags_set_exact_mp3_length(int exact);

/* Embedded cover (JPEG/PNG bytes), malloc'd into *data: the front cover, else
 * the first other picture (not a file icon). Returns 0 if found. A CUE
 * track's path: its file's. */
int tags_picture(const char *path, unsigned char **data, size_t *len);
/* Where that cover's bytes are in a FLAC (the same choice), to be read from
 * the file rather than memory: 0 with *off and *len, or -1 (none, or not a
 * FLAC: tags_picture then). */
int tags_picture_at(const char *path, long *off, long *len);
/* Skip embedded pictures larger than this (default: no limit). */
void tags_set_max_picture(size_t bytes);

/* True if the file extension is one we can play. */
int tags_supported(const char *path);

#endif
