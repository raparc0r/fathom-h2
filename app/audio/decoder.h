/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/*
 * Decoders produce interleaved signed 32-bit samples, left-justified: a 16-bit
 * source sample s becomes s << 16, a 24-bit one s << 8. So the original bits are
 * preserved exactly and an output stage can return to the source bit depth with
 * a plain shift (bit-perfect), or keep 32 bits for DSP.
 */
#ifndef FATHOM_DECODER_H
#define FATHOM_DECODER_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t rate;
    uint8_t  channels;
    uint8_t  bits;          /* source resolution (16, 24...); 0 = lossy/float source, or DSD made PCM */
    uint8_t  dop;           /* DSD over PCM: must reach the DAC untouched, at 24 bits or more */
    uint8_t  stream;        /* for the output: an internet stream (no gapless: it may buffer more) */
    uint64_t total_frames;  /* 0 if unknown */
    char     codec[8];      /* "FLAC", ..., "DSD64" */
} audio_format_t;

typedef struct decoder decoder_t;

/* Why the last decoder_open* on this thread gave no decoder: 0 (the file, its
 * format), or DECODER_WHY_MEMORY: the file needs more memory to open than
 * there is now (an MP4 hours long: its table of frames). */
#define DECODER_WHY_MEMORY 1
int decoder_open_why(void);

/* A CUE track's path (library/cue.h) opens its file and plays just that
 * track: total_frames, reads and seeks are the track's own. */
decoder_t *decoder_open(const char *path, audio_format_t *fmt);
/* The same; a stream gives up (open or read) as soon as *abort becomes nonzero,
 * which another thread may set at any time. */
decoder_t *decoder_open_ex(const char *path, audio_format_t *fmt, const volatile int *abort);
/* DSD (.dsf, .dff) is converted to PCM (176.4 or 192 kHz, 24 bits); with
 * DECODER_DOP, packed as DoP instead (fmt.dop, at the DSD rate / 16), for an
 * output that takes it to a DAC that knows it. Other files: as decoder_open. */
#define DECODER_DOP 1u
decoder_t *decoder_open_flags(const char *path, audio_format_t *fmt, unsigned flags);
/* (tests: every decoder_open_flags takes this long first, as a file slow to open does; 0: none) */
void decoder_test_open_delay(int ms);
/* A mono file or stream is given as two equal channels (fmt.channels says 2).
 * Up to max_frames frames into out (max_frames * channels samples).
 * Returns frames decoded, 0 at end of stream, -1 on error. */
long decoder_read(decoder_t *d, int32_t *out, long max_frames);
int  decoder_seek(decoder_t *d, uint64_t frame);
/* A gapless join to the next CUE track of the same file: if `path` is a part
 * of d's file starting exactly where d is now (the end of the track it was
 * playing), d carries on into it, with no reopening or seeking. 0 (and *fmt
 * the new track's), or -1 with d unchanged. */
int  decoder_continue(decoder_t *d, const char *path, audio_format_t *fmt);
void decoder_close(decoder_t *d);

/* Gapless: the track queued to follow d (NULL: none), told before each read.
 * A DSD track followed by another keeps its filter's last frames for it. */
void decoder_set_next(decoder_t *d, const char *next_path);
/* d follows prev (just ended) without a gap: it takes over what prev kept, so
 * the join sounds as one file would. Call before closing prev. */
void decoder_join(decoder_t *d, const decoder_t *prev);

/* Internet radio: a path starting with http:// or https:// is a stream (FFmpeg;
 * no length, no seeking). */
int  decoder_is_stream(const char *path);
/* What a stream says is playing ("Artist - Title", from ICY metadata or the
 * stream's own tags) and the URL it gives with it (ICY StreamUrl: a cover on
 * some stations, else ""), and a counter that changes with them. On the
 * thread that reads. */
unsigned decoder_meta(decoder_t *d, char *out, size_t cap, char *url, size_t ucap);

#endif
