/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* What the decoder and the tag reader share about FFmpeg (formats other than FLAC and MP3). */
#ifndef FATHOM_FF_UTIL_H
#define FATHOM_FF_UTIL_H

#include <stddef.h>
#include <stdint.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>

/* FFmpeg logs nothing (a library scan opens thousands of files). */
void ff_quiet(void);
/* The source's bit depth for a lossless codec (16, 24...), 0 for a lossy one. */
int ff_bits(const AVCodecParameters *par);
/* The stream's length in sample frames, 0 if unknown. */
uint64_t ff_frames(const AVFormatContext *fc, const AVStream *st);
/* "ALAC", "AAC", "WAV", "AIFF", "WavPack", "APE", "Opus"... (at most cap - 1 chars). */
void ff_codec_name(const AVFormatContext *fc, const AVCodecParameters *par, char *out, size_t cap);
/* A decoded frame as interleaved, left-justified int32 (the engine's samples). -1 for
 * a sample format we don't take. Integer formats are exact; float is clipped. */
int ff_to_s32(const AVFrame *f, int channels, int32_t *out);

/* HTTPS: certificates are checked against the authorities in path (a PEM
 * bundle) for every network fetch that gets ff_tls_options(), redirects
 * included. NULL or a missing file: not checked (the simulator, a card without it). */
void ff_set_ca_file(const char *path);
/* Called first by ff_tls_options(), so before every network connection (on
 * the connecting thread, never the UI's): the player gets the time there when
 * the clock isn't set, or every certificate would look out of date. NULL: none. */
void ff_set_before_net(void (*fn)(void));
void ff_tls_options(AVDictionary **opt);
/* The bundle set, or NULL. (The player parses it once for all connections:
 * app/net/tls_wrap.c.) */
const char *ff_ca_file(void);

#endif
