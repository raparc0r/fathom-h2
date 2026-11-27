/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* The FLAC files the tests make to test with: one writer. (Nine tests had
 * fourteen, the same but for the tags, the length, the format and what the
 * samples are.) A test says what its file is and keeps a function of its own
 * with the parameters it varies:
 *
 *     return t_write_flac(path, &(t_flac_t){.frames = 441, .tags = {{"TITLE", title}, {"ALBUM", album}}});
 *
 * What the library and the players read from these files (durations, tags
 * and their order) is what the tests check: a change here changes every one. */
#ifndef FATHOM_TEST_FLAC_H
#define FATHOM_TEST_FLAC_H

#include <stdbool.h>
#include <stdlib.h>

#include <FLAC/metadata.h>
#include <FLAC/stream_encoder.h>

enum { T_FLAC_TAGS = 10, T_FLAC_BLOCK = 4096 };

typedef struct {
    unsigned frames;                  /* its length, in sample frames */
    unsigned channels, bits, rate;    /* 0: two channels, 16 bits, 44100 Hz */
    /* The samples, T_FLAC_BLOCK frames at a time: frames at .. at + n - 1,
     * interleaved, into pcm (which holds the block before, zeros at first).
     * NULL: silence. */
    void (*fill)(FLAC__int32 *pcm, unsigned at, unsigned n);
    /* Its Vorbis comments, written in this order, up to the first without a
     * name; one whose value is NULL is left out ("" is written, empty). None
     * named: the file has only the encoder's own, empty, block of them. */
    struct { const char *name, *value; } tags[T_FLAC_TAGS];
} t_flac_t;

/* 0, or -1: the file couldn't be written. */
static inline int t_write_flac(const char *path, const t_flac_t *f)
{
    unsigned channels = f->channels ? f->channels : 2;
    FLAC__StreamEncoder *e = FLAC__stream_encoder_new();
    FLAC__StreamMetadata *vc = NULL;
    FLAC__int32 *pcm = calloc((size_t)T_FLAC_BLOCK * channels, sizeof *pcm);
    int rc = -1;
    if (e && pcm) {
        FLAC__stream_encoder_set_channels(e, channels);
        FLAC__stream_encoder_set_bits_per_sample(e, f->bits ? f->bits : 16);
        FLAC__stream_encoder_set_sample_rate(e, f->rate ? f->rate : 44100);
        if (f->tags[0].name) {
            vc = FLAC__metadata_object_new(FLAC__METADATA_TYPE_VORBIS_COMMENT);
            for (int i = 0; i < T_FLAC_TAGS && f->tags[i].name; i++) {
                if (!f->tags[i].value) continue;
                FLAC__StreamMetadata_VorbisComment_Entry en;
                FLAC__metadata_object_vorbiscomment_entry_from_name_value_pair(&en, f->tags[i].name, f->tags[i].value);
                FLAC__metadata_object_vorbiscomment_append_comment(vc, en, false);
            }
            FLAC__stream_encoder_set_metadata(e, &vc, 1);
        }
        if (FLAC__stream_encoder_init_file(e, path, NULL, NULL) == FLAC__STREAM_ENCODER_INIT_STATUS_OK) {
            rc = 0;
            for (unsigned at = 0; at < f->frames && rc == 0;) {
                unsigned n = f->frames - at < T_FLAC_BLOCK ? f->frames - at : T_FLAC_BLOCK;
                if (f->fill) f->fill(pcm, at, n);
                if (!FLAC__stream_encoder_process_interleaved(e, pcm, n)) rc = -1;
                at += n;
            }
            if (!FLAC__stream_encoder_finish(e)) rc = -1;
        }
    }
    if (e) FLAC__stream_encoder_delete(e);
    if (vc) FLAC__metadata_object_delete(vc);
    free(pcm);
    return rc;
}

/* For a tag that is left out when it's empty: the value, or NULL. */
static inline const char *t_flac_some(const char *value) { return value && *value ? value : NULL; }

#endif
