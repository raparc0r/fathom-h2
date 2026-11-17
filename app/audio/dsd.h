/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * DSD files (DSF, and DSDIFF/DFF uncompressed): their headers, and their
 * playback as PCM through a low-pass decimating filter in integer arithmetic,
 * or packed as DoP (DSD over PCM) for a USB DAC that takes it.
 *
 * To PCM: 176.4 kHz (192 kHz for the 48 kHz family) at 24 bits, whatever the
 * DSD rate, since that is the most the H2's I2S runs at. Unity gain: a
 * full-modulation DSD signal would be PCM full scale, so the SACD reference
 * level (50 % modulation, "0 dB" on a SACD) comes out at -6 dBFS and the
 * +3 dB peaks SACDs allow have headroom. Flat to 28 kHz (-0.01 dB), -3 dB at
 * 39 kHz, the DSD noise above 60 kHz 116 dB down; nothing folds into the
 * audio band above -137 dB.
 */
#ifndef FATHOM_DSD_H
#define FATHOM_DSD_H

#include <stdint.h>
#include <stdio.h>

#include "audio/decoder.h"

typedef struct {
    int      dff;            /* DSDIFF (else DSF) */
    int      dst;            /* DSDIFF compressed with DST: not playable */
    uint32_t rate;           /* DSD bits per second per channel: 2822400 = DSD64 */
    unsigned channels;
    uint64_t bytes;          /* of DSD per channel (whole bytes; a last partial one dropped) */
    uint64_t data;           /* offset of the first byte of sound */
    uint32_t block;          /* DSF: bytes per channel per block (planar); DFF: 0 (bytes interleaved) */
    int      lsb_first;      /* DSF's usual bit order (DFF is MSB first) */
    uint64_t id3, id3_len;   /* an ID3v2 tag (DSF's metadata, DFF's "ID3 " chunk), or 0 */
    char     title[256], artist[256];   /* DFF's own (DIIN: DITI, DIAR), "" if none */
} dsd_info_t;

/* Reads the headers from the start of f. Returns 0 if it is a DSD file we can
 * describe (dst set if it is compressed), -1 otherwise. */
int dsd_probe(FILE *f, dsd_info_t *info);
/* "DSD64", "DSD128", "DSD256" for a rate we play; NULL otherwise (DSD512 and up,
 * or a rate not a multiple of 64 x 44.1 or 48 kHz). */
const char *dsd_name(uint32_t rate);
/* The extension says DSD (.dsf, .dff). */
int dsd_path(const char *path);

/* The decoder (decoder.c's DSD files). dop: pack the DSD as DoP at rate / 16,
 * 24 bits, for an output that passes it to a DAC untouched. */
typedef struct dsd_dec dsd_dec_t;
dsd_dec_t *dsd_open(const char *path, int dop, audio_format_t *fmt);
long dsd_read(dsd_dec_t *d, int32_t *out, long max);
int  dsd_seek(dsd_dec_t *d, uint64_t frame);
/* Another DSD track follows without a gap: keep the filter's last few samples
 * for it (dsd_join) instead of playing them out against silence. */
void dsd_set_followed(dsd_dec_t *d, int followed);
/* next starts where prev (at its end, followed) stopped: it takes over the
 * filter, so the join is the same as one continuous file. 0 if it could
 * (same rate and channels), -1 if not (next starts afresh). */
int  dsd_join(dsd_dec_t *next, const dsd_dec_t *prev);
void dsd_close(dsd_dec_t *d);

/* The converter on its own (tests, the bench): ratio = DSD rate / PCM rate
 * (16, 32, 64), up to 2 channels. Feed it whole groups of ratio / 8 bytes per
 * channel, MSB first (the earliest bit in bit 7): one output frame each. */
#define DSD_CHUNK 256        /* frames per dsd2pcm_run, at most: small, to leave the cache to the tables */
typedef struct dsd2pcm dsd2pcm_t;
dsd2pcm_t *dsd2pcm_new(unsigned ratio, unsigned channels);
void      dsd2pcm_free(dsd2pcm_t *c);
void      dsd2pcm_reset(dsd2pcm_t *c);                 /* as after silence */
/* Where channel ch's bytes for the next run go (DSD_CHUNK * ratio / 8 of room). */
uint8_t  *dsd2pcm_in(dsd2pcm_t *c, unsigned ch);
/* n frames (n * ratio / 8 bytes per channel, already in dsd2pcm_in) into out:
 * interleaved, left-justified, 24 significant bits. */
void      dsd2pcm_run(dsd2pcm_t *c, long n, int32_t *out);
/* The filter's delay in output frames: output frame k + delay is the one
 * centred on input group k. */
unsigned  dsd2pcm_delay(const dsd2pcm_t *c);
/* Output frames that still depend on what came before a reset, after it. */
unsigned  dsd2pcm_span(const dsd2pcm_t *c);
/* dst carries on from src's history (same ratio and channels). */
void      dsd2pcm_copy(dsd2pcm_t *dst, const dsd2pcm_t *src);

#endif
