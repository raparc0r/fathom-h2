/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Works for shuffle's "Keep works together" (library_shuffle_tracks): consecutive
 * tracks of an album whose titles share a prefix before ": ", two or more, form
 * one unit; a prefix on its own, or split by another track, doesn't. */
#include <FLAC/metadata.h>
#include <FLAC/stream_encoder.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "library/library.h"
#include "tmpdir.h"

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static const char *const TITLES[] = {
    "Suite in D: I. Prelude", "Suite in D: II. Air", "Suite in D: III. Gigue",   /* a work in three parts */
    "Interlude: Rain",                                                          /* a prefix on its own */
    "Song: Live", "Another Song", "Song: Live again",                           /* split: no work */
};
#define N ((int)(sizeof TITLES / sizeof TITLES[0]))

static int write_flac(const char *path, int track)
{
    FLAC__StreamEncoder *e = FLAC__stream_encoder_new();
    FLAC__stream_encoder_set_channels(e, 2);
    FLAC__stream_encoder_set_bits_per_sample(e, 16);
    FLAC__stream_encoder_set_sample_rate(e, 44100);
    FLAC__StreamMetadata *vc = FLAC__metadata_object_new(FLAC__METADATA_TYPE_VORBIS_COMMENT);
    FLAC__StreamMetadata_VorbisComment_Entry en;
    char buf[64];
    FLAC__metadata_object_vorbiscomment_entry_from_name_value_pair(&en, "TITLE", TITLES[track - 1]);
    FLAC__metadata_object_vorbiscomment_append_comment(vc, en, false);
    FLAC__metadata_object_vorbiscomment_entry_from_name_value_pair(&en, "ARTIST", "Some Ensemble");
    FLAC__metadata_object_vorbiscomment_append_comment(vc, en, false);
    FLAC__metadata_object_vorbiscomment_entry_from_name_value_pair(&en, "ALBUM", "Suites");
    FLAC__metadata_object_vorbiscomment_append_comment(vc, en, false);
    snprintf(buf, sizeof buf, "%d", track);
    FLAC__metadata_object_vorbiscomment_entry_from_name_value_pair(&en, "TRACKNUMBER", buf);
    FLAC__metadata_object_vorbiscomment_append_comment(vc, en, false);
    FLAC__stream_encoder_set_metadata(e, &vc, 1);
    if (FLAC__stream_encoder_init_file(e, path, NULL, NULL) != FLAC__STREAM_ENCODER_INIT_STATUS_OK) return -1;
    static FLAC__int32 pcm[2 * 4410];
    FLAC__stream_encoder_process_interleaved(e, pcm, 4410);
    FLAC__stream_encoder_finish(e);
    FLAC__stream_encoder_delete(e);
    FLAC__metadata_object_delete(vc);
    return 0;
}

int main(void)
{
    char dir[200];
    test_tmp(dir, sizeof dir, "fathom-test-works");
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; }
    char path[300], music[300], db[300], art[300];
    snprintf(music, sizeof music, "%s/music", dir);
    mkdir(music, 0755);
    for (int i = 1; i <= N; i++) {
        snprintf(path, sizeof path, "%s/%02d.flac", music, i);
        if (write_flac(path, i)) { fprintf(stderr, "cannot write %s\n", path); return 1; }
    }
    snprintf(db, sizeof db, "%s/lib.db", dir);
    snprintf(art, sizeof art, "%s/art", dir);
    library_t *lib = library_open(db, art);
    CHECK(lib && library_scan(lib, music, NULL, NULL) == N);

    shuf_track_t *t = NULL;
    int n = library_shuffle_tracks(lib, &t);
    CHECK(n == N);
    uint32_t unit[N + 1] = {0};
    for (int i = 0; i < n; i++) if (t[i].track >= 1 && t[i].track <= N) unit[t[i].track] = t[i].unit;
    CHECK(unit[1] && unit[1] == unit[2] && unit[2] == unit[3]);   /* the suite, together */
    CHECK(unit[4] == 0);                                          /* a lone prefix */
    CHECK(unit[5] == 0 && unit[6] == 0 && unit[7] == 0);          /* split by another track */
    free(t);
    library_close(lib);

    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    printf("works: all good\n");
    return 0;
}
