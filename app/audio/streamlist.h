/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Radio stations often publish a playlist (.m3u, .pls) that names the stream
 * rather than the stream itself. These read one (no network here: the decoder
 * fetches it) and give the stream's URL.
 */
#ifndef FATHOM_STREAMLIST_H
#define FATHOM_STREAMLIST_H

#include <stddef.h>

enum {
    STREAMLIST_NONE = 0,     /* not a playlist (or nothing playable in it) */
    STREAMLIST_FOUND = 1,    /* out holds the first stream's URL */
    STREAMLIST_HLS = 2,      /* an HLS playlist (#EXT-X-...): segments, not a stream URL */
};

/* The URL's path ends in .m3u, .m3u8 or .pls (before any ?query or #fragment):
 * read it first (an .m3u8 is HLS, or now and then a plain list). */
int streamlist_looks_like(const char *url);

/* The first stream named by a playlist's text (n bytes, not NUL-terminated):
 * PLS ([playlist], the lowest FileN=) or M3U (the first line that isn't a
 * comment). Relative entries are resolved against base, the playlist's own
 * URL. Only http and https results are given. */
int streamlist_first(const char *text, size_t n, const char *base, char *out, size_t cap);

/* An HLS playlist (STREAMLIST_HLS above), for FFmpeg's hls demuxer. A master
 * playlist (#EXT-X-STREAM-INF) gives the one media playlist to play in out
 * (STREAMLIST_FOUND), so FFmpeg fetches only that one: of the audio-only
 * variants, the lowest bandwidth that's still good (about 96 kb/s and up),
 * else the highest; failing those an audio rendition (#EXT-X-MEDIA,
 * TYPE=AUDIO, the default one), then the cheapest variant with video. Codecs
 * the H2 doesn't decode (AC-3, E-AC-3) are passed over. A media playlist
 * (segments) gives STREAMLIST_HLS: it's played as it is. */
int streamlist_hls_pick(const char *text, size_t n, const char *base, char *out, size_t cap);

#endif
