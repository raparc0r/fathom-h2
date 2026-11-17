/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Ogg Vorbis files. On the H2 through Tremor (libvorbisidec: integer, as
 * floating point is slow on the X1000), on a computer through Tremor or
 * libvorbisfile, whichever pkg-config finds (FATHOM_HAVE_VORBIS, and
 * FATHOM_VORBIS_TREMOR for Tremor). Both have vorbisfile's API, which does
 * the gapless trimming (granule positions), exact seeking and the length;
 * this hides where they differ (headers, ov_read's arguments, the samples).
 */
#ifndef FATHOM_VORBIS_H
#define FATHOM_VORBIS_H

#include <stddef.h>
#include <stdint.h>

/* The first n bytes of a file are an Ogg page starting a Vorbis stream (Opus
 * and FLAC in Ogg are not). 64 bytes are enough. Always built. */
int vorbis_sniff(const unsigned char *head, size_t n);
/* The same, for a file. */
int vorbis_file_is_vorbis(const char *path);

typedef struct vorbis_file vorbis_file_t;

/* NULL if it can't be opened, isn't Vorbis, isn't mono or stereo, or no
 * Vorbis library was built in. *frames is 0 when the length is unknown. */
vorbis_file_t *vorbis_open(const char *path, uint32_t *rate, uint8_t *channels, uint64_t *frames);
/* A stream (radio: no length, no seeking), its bytes from src (up to n into
 * buf; 0 at the end or on an error). */
vorbis_file_t *vorbis_open_stream(size_t (*src)(void *ctx, void *buf, size_t n), void *ctx,
                                  uint32_t *rate, uint8_t *channels);
/* When a link (a chained stream's next song) has begun since the last call:
 * 1 and its "Artist - Title" (or "Title") in out; else 0. */
int vorbis_title(vorbis_file_t *v, char *out, size_t cap);
/* Up to max frames, interleaved and left-justified (the decoder's samples):
 * frames read, 0 at the end, -1 on error. Tremor gives 16 bits. */
long vorbis_read(vorbis_file_t *v, int32_t *out, long max);
int  vorbis_seek(vorbis_file_t *v, uint64_t frame);
void vorbis_close(vorbis_file_t *v);

#endif
