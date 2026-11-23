/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
#include "decoder.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <FLAC/stream_decoder.h>
#include <mpg123.h>

#include "audio/dsd.h"
#include "core/lat.h"
#include "audio/vorbis.h"

#ifdef FATHOM_HAVE_FFMPEG
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include "audio/ff_util.h"
#include "audio/streamlist.h"
#include "net/netclock.h"
#endif
#include "library/cue.h"

typedef enum { DEC_FLAC, DEC_MP3, DEC_FF, DEC_DSD, DEC_VORBIS } dec_kind_t;

struct decoder {
    dec_kind_t kind;
    audio_format_t fmt;

    /* FLAC: one decoded block is buffered here and handed out across reads */
    FLAC__StreamDecoder *flac;
    int32_t *buf;
    long buf_frames, buf_pos, buf_cap;
    int flac_error;

    mpg123_handle *mp3;
    int mp3_bytes;   /* bytes per sample mpg123 actually delivers */
    int mp3_fd;      /* the file, opened here (close-on-exec), -1 none */

    /* A CUE track (cue.h): frames [part_start, part_end) of `file` (part_end
     * 0: to its end). Reads, seeks and total_frames are the part's. */
    int part;
    uint64_t part_start, part_end, file_frames;
    uint64_t at;     /* the next frame read, in the file */
    char *file;

    dsd_dec_t *dsd;
    vorbis_file_t *vorbis;   /* Ogg Vorbis: Tremor (vorbis.c) */

#ifdef FATHOM_HAVE_FFMPEG
    /* everything else (ALAC, AAC, WAV, AIFF, WavPack, APE, Opus...): one
     * decoded frame is buffered in buf, as for FLAC */
    AVFormatContext *fc;
    AVCodecContext *cc;
    AVPacket *pkt;
    AVFrame *frm;
    int stream, ff_eof;
    int64_t ff_target;   /* after a seek: the first frame to hand out, or -1 */
    /* internet radio */
    int net;
    const volatile int *abort;
    AVIOContext *vpb;    /* Ogg Vorbis radio: Tremor's connection */
    char meta[256], meta_url[512], icy_last[512];
    unsigned meta_seq;
#endif
};

/* ---------- FLAC ---------- */

static FLAC__StreamDecoderWriteStatus flac_write(const FLAC__StreamDecoder *dec, const FLAC__Frame *frame,
                                                 const FLAC__int32 *const buffer[], void *client)
{
    (void)dec;
    decoder_t *d = client;
    unsigned ch = frame->header.channels, n = frame->header.blocksize;
    unsigned shift = 32 - frame->header.bits_per_sample;
    if ((long)n > d->buf_cap) {
        int32_t *nb = realloc(d->buf, (size_t)n * ch * sizeof *nb);
        if (!nb) return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
        d->buf = nb;
        d->buf_cap = n;
    }
    /* stereo and mono on their own: the general loop took 11 MIPS instructions a
     * sample, a tenth of decoding (levelling reads whole tracks) */
    int32_t *o = d->buf;
    if (ch == 2) {
        const FLAC__int32 *l = buffer[0], *r = buffer[1];
        for (unsigned i = 0; i < n; i++) {
            o[2 * i] = (int32_t)((uint32_t)l[i] << shift);
            o[2 * i + 1] = (int32_t)((uint32_t)r[i] << shift);
        }
    } else if (ch == 1) {
        const FLAC__int32 *m = buffer[0];
        for (unsigned i = 0; i < n; i++) o[i] = (int32_t)((uint32_t)m[i] << shift);
    } else {
        for (unsigned i = 0; i < n; i++)
            for (unsigned c = 0; c < ch; c++) o[i * ch + c] = (int32_t)((uint32_t)buffer[c][i] << shift);
    }
    d->buf_frames = n;
    d->buf_pos = 0;
    return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

static void flac_meta(const FLAC__StreamDecoder *dec, const FLAC__StreamMetadata *m, void *client)
{
    (void)dec;
    decoder_t *d = client;
    if (m->type != FLAC__METADATA_TYPE_STREAMINFO) return;
    d->fmt.rate = m->data.stream_info.sample_rate;
    d->fmt.channels = (uint8_t)m->data.stream_info.channels;
    d->fmt.bits = (uint8_t)m->data.stream_info.bits_per_sample;
    d->fmt.total_frames = m->data.stream_info.total_samples;
}

static void flac_err(const FLAC__StreamDecoder *dec, FLAC__StreamDecoderErrorStatus st, void *client)
{
    (void)dec; (void)st;
    ((decoder_t *)client)->flac_error = 1;
}

static int open_flac(decoder_t *d, const char *path)
{
    d->flac = FLAC__stream_decoder_new();
    if (!d->flac) return -1;
    FLAC__stream_decoder_set_md5_checking(d->flac, false);
    /* Opened here, close-on-exec, rather than by libFLAC: a program the player
     * starts meanwhile (the SSH server, Wi-Fi's) would otherwise keep the file
     * open for good, and the card couldn't be lent to a computer. libFLAC
     * closes it at finish. */
    FILE *f = fopen(path, "rbe");
    if (!f) return -1;
    if (FLAC__stream_decoder_init_FILE(d->flac, f, flac_write, flac_meta, flac_err, d) !=
        FLAC__STREAM_DECODER_INIT_STATUS_OK) {
        fclose(f);
        return -1;
    }
    if (!FLAC__stream_decoder_process_until_end_of_metadata(d->flac) || !d->fmt.rate) return -1;
    strcpy(d->fmt.codec, "FLAC");
    return 0;
}

static long read_flac(decoder_t *d, int32_t *out, long max)
{
    long got = 0;
    int ch = d->fmt.channels;
    while (got < max) {
        if (d->buf_pos >= d->buf_frames) {
            if (FLAC__stream_decoder_get_state(d->flac) == FLAC__STREAM_DECODER_END_OF_STREAM) break;
            d->buf_frames = d->buf_pos = 0;
            if (!FLAC__stream_decoder_process_single(d->flac)) return got ? got : -1;
            if (d->buf_frames == 0) {
                if (FLAC__stream_decoder_get_state(d->flac) == FLAC__STREAM_DECODER_END_OF_STREAM) break;
                continue;
            }
        }
        long n = d->buf_frames - d->buf_pos;
        if (n > max - got) n = max - got;
        memcpy(out + got * ch, d->buf + d->buf_pos * ch, (size_t)n * ch * sizeof *out);
        d->buf_pos += n;
        got += n;
    }
    return got;
}

/* ---------- MP3 (mpg123 handles LAME/Xing gapless trimming) ---------- */

static int mp3_ready;

/* Allow `enc` at every sample rate mpg123 knows. Must happen before mpg123_open:
 * the output format is fixed when the first frame is parsed. */
static int allow_encoding(mpg123_handle *mh, int enc)
{
    const long *rates;
    size_t n;
    mpg123_rates(&rates, &n);
    mpg123_format_none(mh);
    int ok = 0;
    for (size_t i = 0; i < n; i++)
        ok |= mpg123_format(mh, rates[i], MPG123_MONO | MPG123_STEREO, enc) == MPG123_OK;
    return ok ? 0 : -1;
}

static int open_mp3(decoder_t *d, const char *path)
{
    d->mp3_fd = -1;
    if (!mp3_ready) { mpg123_init(); mp3_ready = 1; }   /* no-op on recent mpg123, needed on old ones */
    d->mp3 = mpg123_new(NULL, NULL);
    if (!d->mp3) return -1;
    /* ADD_FLAGS keeps mpg123's defaults; MPG123_FLAGS would replace them.
     * SKIP_ID3V2: the tag at the front is passed over, not read into memory
     * and parsed (all of it, a cover of a megabyte or more included, at every
     * open: on the playback thread at a gapless join). Nothing here asks
     * mpg123 for tags (the library reads them: library/tags.c); the encoder's
     * delay and padding are in the first audio frame (LAME/Info), not the tag. */
    mpg123_param(d->mp3, MPG123_ADD_FLAGS, MPG123_QUIET | MPG123_GAPLESS | MPG123_SKIP_ID3V2, 0);
    /* damaged files: keep searching for the next valid frame instead of giving up after 1 KiB */
    mpg123_param(d->mp3, MPG123_RESYNC_LIMIT, -1, 0);
    /* 32-bit output if this mpg123 build has it, else 16-bit; we widen to int32 ourselves */
    if (allow_encoding(d->mp3, MPG123_ENC_SIGNED_32) && allow_encoding(d->mp3, MPG123_ENC_SIGNED_16)) return -1;
    d->mp3_fd = open(path, O_RDONLY | O_CLOEXEC);     /* (as FLAC's, above) */
    if (d->mp3_fd < 0 || mpg123_open_fd(d->mp3, d->mp3_fd) != MPG123_OK) return -1;
    long rate;
    int ch, enc;
    if (mpg123_getformat(d->mp3, &rate, &ch, &enc) != MPG123_OK) return -1;
    if (enc == MPG123_ENC_SIGNED_32) d->mp3_bytes = 4;
    else if (enc == MPG123_ENC_SIGNED_16) d->mp3_bytes = 2;
    else return -1;
    d->fmt.rate = (uint32_t)rate;
    d->fmt.channels = (uint8_t)ch;
    d->fmt.bits = 0;
    off_t len = mpg123_length(d->mp3);
    d->fmt.total_frames = len > 0 ? (uint64_t)len : 0;
    strcpy(d->fmt.codec, "MP3");
    return 0;
}

static long read_mp3(decoder_t *d, int32_t *out, long max)
{
    int ch = d->fmt.channels;
    size_t want = (size_t)max * ch * d->mp3_bytes;   /* fits in `out`: 2 or 4 bytes per sample */
    unsigned char *raw = (unsigned char *)out;
    size_t done = 0;
    for (int tries = 0; tries < 4 && done == 0; tries++) {
        int r = mpg123_read(d->mp3, raw, want, &done);
        if (r == MPG123_NEW_FORMAT) {
            long rate;
            int c, e;
            mpg123_getformat(d->mp3, &rate, &c, &e);
            if (c != ch || (uint32_t)rate != d->fmt.rate) return -1;   /* format change mid-stream */
            d->mp3_bytes = (e == MPG123_ENC_SIGNED_32) ? 4 : 2;
            want = (size_t)max * ch * d->mp3_bytes;
            continue;
        }
        if (r == MPG123_DONE && done == 0) return 0;
        if (r != MPG123_OK && r != MPG123_DONE && done == 0) return -1;
    }
    long frames = (long)(done / ((size_t)ch * d->mp3_bytes));
    if (d->mp3_bytes == 2) {   /* widen in place, back to front: s16 -> left-justified s32 */
        const int16_t *s16 = (const int16_t *)raw;
        for (long i = frames * ch - 1; i >= 0; i--) out[i] = (int32_t)((uint32_t)(uint16_t)s16[i] << 16);
    }
    return frames;
}

/* ---------- Ogg Vorbis (Tremor: FFmpeg's decoder is floating point) ---------- */

#ifdef FATHOM_HAVE_VORBIS
static int open_vorbis(decoder_t *d, const char *path)
{
    d->vorbis = vorbis_open(path, &d->fmt.rate, &d->fmt.channels, &d->fmt.total_frames);
    if (!d->vorbis) return -1;
    d->fmt.bits = 0;
    strcpy(d->fmt.codec, "Vorbis");
    return 0;
}
#endif

/* ---------- FFmpeg: the other formats ---------- */

#ifdef FATHOM_HAVE_FFMPEG
static int ff_interrupted(void *opaque)
{
    const decoder_t *d = opaque;
    return d->abort && *d->abort;
}

#define NET_UA "Fathom (HiFi Walker H2)"
/* µs: a read that waits this long gives up, and the stream reconnects. 8 s
 * (it was 15): a live stream that has sent nothing for that long isn't coming
 * back on that connection, and the silence is heard for all of it. */
#define NET_TIMEOUT "8000000"

/* The hls demuxer's own fetches (the playlist, its reloads, segments, keys):
 * with the certificates checked as for every other fetch (it passes on only
 * a few of the options it was opened with, and not the TLS ones), and only
 * over the network: a station's playlist can't name the card's files. */
static int hls_io_open(AVFormatContext *s, AVIOContext **pb, const char *url, int flags, AVDictionary **options)
{
    AVDictionary *o = NULL;
    if (options) av_dict_copy(&o, *options, 0);
    ff_tls_options(&o);
    if (!av_dict_get(o, "rw_timeout", NULL, 0)) av_dict_set(&o, "rw_timeout", NET_TIMEOUT, 0);
    if (!av_dict_get(o, "user_agent", NULL, 0)) av_dict_set(&o, "user_agent", NET_UA, 0);
    av_dict_set(&o, "protocol_whitelist", "http,https,tls,tcp,crypto,httpproxy", 0);
    int r = avio_open2(pb, url, flags, &s->interrupt_callback, &o);
    if (options) {
        av_dict_free(options);
        *options = o;                   /* what wasn't taken, as FFmpeg's own io_open leaves it */
    } else {
        av_dict_free(&o);
    }
    return r;
}

/* hls: the URL is an HLS media playlist, for FFmpeg's hls demuxer. -2 for
 * a stream in Vorbis, which FFmpeg reads but (on the H2) can't decode. */
static int open_ff(decoder_t *d, const char *path, int hls)
{
    ff_quiet();
    d->ff_target = -1;
    AVDictionary *opt = NULL;
    const AVInputFormat *ifmt = NULL;
    if (d->net) {
        /* a stream: titles (ICY), reconnects, no endless waits, a quick start */
        d->fc = avformat_alloc_context();
        if (!d->fc) return -1;
        d->fc->interrupt_callback = (AVIOInterruptCB){ff_interrupted, d};
        /* (the hls demuxer gives its segments' demuxer these too) */
        d->fc->probesize = 64 * 1024;
        d->fc->max_analyze_duration = 1000000;
        if (hls) {
            ifmt = av_find_input_format("hls");
            d->fc->io_open = hls_io_open;
            /* one segment at a time: one connection's buffers, not two (the
             * next segment opened early); a failed segment tried again */
            av_dict_set(&opt, "http_multiple", "0", 0);
            av_dict_set(&opt, "seg_max_retry", "3", 0);
        } else {
            av_dict_set(&opt, "icy", "1", 0);   /* (HLS has none: titles come in ID3 tags, if at all) */
        }
        av_dict_set(&opt, "reconnect", "1", 0);
        av_dict_set(&opt, "reconnect_streamed", "1", 0);
        av_dict_set(&opt, "reconnect_on_network_error", "1", 0);
        av_dict_set(&opt, "reconnect_delay_max", "8", 0);
        av_dict_set(&opt, "rw_timeout", NET_TIMEOUT, 0);
        av_dict_set(&opt, "user_agent", NET_UA, 0);
        ff_tls_options(&opt);              /* https (and a redirect to it): certificates checked */
    }
    LAT("decoder: opening %s", path);
    int r = avformat_open_input(&d->fc, path, ifmt, &opt);
    av_dict_free(&opt);
    LAT("decoder: open_input %d (%s)", r, d->fc && d->fc->iformat ? d->fc->iformat->name : "-");
    if (r < 0) return -1;
    if (avformat_find_stream_info(d->fc, NULL) < 0) return -1;
    LAT("decoder: stream info (%lld bytes read)", d->fc->pb ? (long long)avio_tell(d->fc->pb) : -1LL);
    const AVCodec *codec = NULL;
    d->stream = av_find_best_stream(d->fc, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
    if (d->stream < 0 || !codec) {
        int s = av_find_best_stream(d->fc, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
        return d->net && !hls && s >= 0 && d->fc->streams[s]->codecpar->codec_id == AV_CODEC_ID_VORBIS ? -2 : -1;
    }
    AVStream *st = d->fc->streams[d->stream];
    if (d->net)                          /* the rest unread: video, ID3 timestamps, other variants */
        for (unsigned i = 0; i < d->fc->nb_streams; i++)
            if ((int)i != d->stream) d->fc->streams[i]->discard = AVDISCARD_ALL;
    d->cc = avcodec_alloc_context3(codec);
    if (!d->cc || avcodec_parameters_to_context(d->cc, st->codecpar) < 0) return -1;
    d->cc->request_sample_fmt = AV_SAMPLE_FMT_S16;   /* libopus: its integer path (soft-float build) */
    if (avcodec_open2(d->cc, codec, NULL) < 0) return -1;
    d->pkt = av_packet_alloc();
    d->frm = av_frame_alloc();
    if (!d->pkt || !d->frm) return -1;

    int ch = d->cc->ch_layout.nb_channels;
    if (ch < 1 || ch > 2 || d->cc->sample_rate <= 0) return -1;   /* stereo DAC; no downmixing */
    d->fmt.rate = (uint32_t)d->cc->sample_rate;
    d->fmt.channels = (uint8_t)ch;
    d->fmt.bits = (uint8_t)ff_bits(st->codecpar);
    d->fmt.total_frames = d->net ? 0 : ff_frames(d->fc, st);
    ff_codec_name(d->fc, st->codecpar, d->fmt.codec, sizeof d->fmt.codec);
    return 0;
}

/* The decoded frame, interleaved and left-justified into buf (from the seek
 * target on, after a seek). Returns its frames. */
static long ff_take(decoder_t *d, const AVFrame *f)
{
    int ch = d->fmt.channels;
    long n = f->nb_samples, skip = 0;
    if (d->ff_target >= 0) {
        if (f->pts != AV_NOPTS_VALUE) {
            AVStream *st = d->fc->streams[d->stream];
            int64_t at = av_rescale_q(f->pts, st->time_base, (AVRational){1, (int)d->fmt.rate});
            if (at + n <= d->ff_target) return 0;              /* before the target */
            if (at < d->ff_target) skip = (long)(d->ff_target - at);
        }
        d->ff_target = -1;
    }
    if (n > d->buf_cap) {
        int32_t *nb = realloc(d->buf, (size_t)n * ch * sizeof *nb);
        if (!nb) return -1;
        d->buf = nb;
        d->buf_cap = n;
    }
    if (ff_to_s32(f, ch, d->buf) < 0) return -1;
    d->buf_frames = n;
    d->buf_pos = skip;
    return n - skip;
}

/* A stream's "now playing": ICY (StreamTitle='Artist - Title';...) where the
 * station sends it, else the stream's tags when they change (chained Ogg). */
static void meta_set(decoder_t *d, const char *t, const char *url)
{
    if (!t || !*t) return;
    if (!url) url = "";
    if (!strcmp(t, d->meta) && !strcmp(url, d->meta_url)) return;
    snprintf(d->meta, sizeof d->meta, "%s", t);
    snprintf(d->meta_url, sizeof d->meta_url, "%s", url);
    d->meta_seq++;
}

/* An ICY field's value (StreamTitle='...';), or "" */
static void icy_field(const char *icy, const char *key, char *out, size_t cap)
{
    out[0] = '\0';
    char k[32];
    snprintf(k, sizeof k, "%s='", key);
    const char *t = strstr(icy, k);
    if (!t) return;
    t += strlen(k);
    const char *e = strstr(t, "';");
    size_t n = e ? (size_t)(e - t) : strlen(t);
    if (n >= cap) n = cap - 1;
    memcpy(out, t, n);
    out[n] = '\0';
}

static void net_meta(decoder_t *d)
{
    uint8_t *icy = NULL;
    if (d->fc->pb && av_opt_get(d->fc->pb, "icy_metadata_packet", AV_OPT_SEARCH_CHILDREN, &icy) >= 0 && icy) {
        if (*icy && strcmp((const char *)icy, d->icy_last)) {
            snprintf(d->icy_last, sizeof d->icy_last, "%s", (const char *)icy);
            char title[256], url[512];
            icy_field((const char *)icy, "StreamTitle", title, sizeof title);
            icy_field((const char *)icy, "StreamUrl", url, sizeof url);   /* a cover, on some stations */
            meta_set(d, title, url);
        }
        av_free(icy);
    }
    if (d->fc->event_flags & AVFMT_EVENT_FLAG_METADATA_UPDATED || d->fc->streams[d->stream]->event_flags & AVSTREAM_EVENT_FLAG_METADATA_UPDATED) {
        d->fc->event_flags &= ~AVFMT_EVENT_FLAG_METADATA_UPDATED;
        d->fc->streams[d->stream]->event_flags &= ~AVSTREAM_EVENT_FLAG_METADATA_UPDATED;
        AVDictionary *m = d->fc->streams[d->stream]->metadata;
        AVDictionaryEntry *ar = av_dict_get(m, "artist", NULL, 0), *ti = av_dict_get(m, "title", NULL, 0);
        if (!ti) { m = d->fc->metadata; ar = av_dict_get(m, "artist", NULL, 0); ti = av_dict_get(m, "title", NULL, 0); }
        if (ti) {
            char t[256];
            if (ar) snprintf(t, sizeof t, "%s - %s", ar->value, ti->value);
            else snprintf(t, sizeof t, "%s", ti->value);
            meta_set(d, t, NULL);
        }
    }
}

/* Decodes until a frame is buffered: its frame count, 0 at the end, -1 on error. */
static long ff_fill(decoder_t *d)
{
    for (;;) {
        int r = avcodec_receive_frame(d->cc, d->frm);
        if (r == 0) {
            long n = ff_take(d, d->frm);
            av_frame_unref(d->frm);
            if (n != 0) return n;
            continue;
        }
        if (r == AVERROR_EOF) return 0;
        if (r != AVERROR(EAGAIN)) return -1;
        if (d->ff_eof) return 0;
        if (av_read_frame(d->fc, d->pkt) < 0) {
            d->ff_eof = 1;
            avcodec_send_packet(d->cc, NULL);                   /* drain the decoder */
            continue;
        }
        if (d->net) net_meta(d);
        if (d->pkt->stream_index == d->stream) avcodec_send_packet(d->cc, d->pkt);   /* a bad packet: skipped */
        av_packet_unref(d->pkt);
    }
}

static long read_ff(decoder_t *d, int32_t *out, long max)
{
    long got = 0;
    int ch = d->fmt.channels;
    while (got < max) {
        if (d->buf_pos >= d->buf_frames) {
            d->buf_frames = d->buf_pos = 0;
            long r = ff_fill(d);
            if (r < 0) return got ? got : -1;
            if (r == 0) break;
        }
        long n = d->buf_frames - d->buf_pos;
        if (n > max - got) n = max - got;
        memcpy(out + got * ch, d->buf + d->buf_pos * ch, (size_t)n * ch * sizeof *out);
        d->buf_pos += n;
        got += n;
    }
    return got;
}

static int seek_ff(decoder_t *d, uint64_t frame)
{
    AVStream *st = d->fc->streams[d->stream];
    int64_t ts = av_rescale_q((int64_t)frame, (AVRational){1, (int)d->fmt.rate}, st->time_base);
    if (av_seek_frame(d->fc, d->stream, ts, AVSEEK_FLAG_BACKWARD) < 0) return -1;
    avcodec_flush_buffers(d->cc);
    d->ff_eof = 0;
    d->buf_frames = d->buf_pos = 0;
    d->ff_target = (int64_t)frame;    /* the demuxer lands on a packet at or before it */
    return 0;
}
#endif

#ifdef FATHOM_HAVE_FFMPEG
/* A station's link to a playlist (.m3u, .pls) rather than the stream: up to
 * cap - 1 bytes of what's there (NUL-terminated), or -1. Same options as the
 * stream: a timeout, the abort flag, checked certificates. */
static long fetch_text(decoder_t *d, const char *url, char *buf, size_t cap)
{
    AVIOContext *pb = NULL;
    AVIOInterruptCB icb = {ff_interrupted, d};
    AVDictionary *opt = NULL;
    av_dict_set(&opt, "rw_timeout", NET_TIMEOUT, 0);
    av_dict_set(&opt, "user_agent", NET_UA, 0);
    ff_tls_options(&opt);
    LAT("decoder: fetching %s", url);
    int r = avio_open2(&pb, url, AVIO_FLAG_READ, &icb, &opt);
    av_dict_free(&opt);
    LAT("decoder: fetch open %d", r);
    if (r < 0) return -1;
    size_t n = 0;
    while (n + 1 < cap) {
        int k = avio_read(pb, (unsigned char *)buf + n, (int)(cap - 1 - n));
        if (k <= 0) break;
        n += (size_t)k;
    }
    avio_closep(&pb);
    buf[n] = '\0';
    LAT("decoder: fetched %zu bytes", n);
    return (long)n;
}

#define LIST_MAX   65536
#define LIST_DEPTH 3

#ifdef FATHOM_HAVE_VORBIS
/* Ogg Vorbis radio: FFmpeg's decoder is floating point, so the H2 has none;
 * Tremor reads the stream instead, over a connection of its own (the same
 * options as FFmpeg's, but no ICY: Ogg streams send their own tags). */
static size_t net_vorbis_src(void *ctx, void *buf, size_t n)
{
    decoder_t *d = ctx;
    int r = avio_read(d->vpb, buf, n > 65536 ? 65536 : (int)n);
    return r > 0 ? (size_t)r : 0;
}

static int open_net_vorbis(decoder_t *d, const char *url)
{
    AVIOInterruptCB icb = {ff_interrupted, d};
    AVDictionary *opt = NULL;
    av_dict_set(&opt, "reconnect", "1", 0);
    av_dict_set(&opt, "reconnect_streamed", "1", 0);
    av_dict_set(&opt, "reconnect_on_network_error", "1", 0);
    av_dict_set(&opt, "reconnect_delay_max", "8", 0);
    av_dict_set(&opt, "rw_timeout", NET_TIMEOUT, 0);
    av_dict_set(&opt, "user_agent", NET_UA, 0);
    ff_tls_options(&opt);
    int r = avio_open2(&d->vpb, url, AVIO_FLAG_READ, &icb, &opt);
    av_dict_free(&opt);
    if (r < 0) return -1;
    d->vorbis = vorbis_open_stream(net_vorbis_src, d, &d->fmt.rate, &d->fmt.channels);
    if (!d->vorbis) return -1;
    d->kind = DEC_VORBIS;
    d->fmt.bits = 0;
    d->fmt.total_frames = 0;
    strcpy(d->fmt.codec, "Vorbis");
    return 0;
}
#endif

/* A stream's URL (no playlist): FFmpeg, or Tremor for Vorbis */
static int open_stream(decoder_t *d, const char *url)
{
    int r = open_ff(d, url, 0);
#ifdef FATHOM_HAVE_VORBIS
    if (r == -2) {
        avformat_close_input(&d->fc);
        fprintf(stderr, "radio: %s is Vorbis: Tremor\n", url);
        return open_net_vorbis(d, url);
    }
#endif
    return r ? -1 : 0;
}

/* An HLS playlist (url, its text in buf): a master playlist's chosen variant,
 * or the media playlist itself, through FFmpeg's hls demuxer. */
static int open_hls(decoder_t *d, const char *url, const char *buf, long n)
{
    char media[1024];
    int k = streamlist_hls_pick(buf, (size_t)n, url, media, sizeof media);
    if (k == STREAMLIST_FOUND) {
        fprintf(stderr, "radio: HLS %s, playing %s\n", url, media);
        url = media;
    } else if (k != STREAMLIST_HLS) {
        return -1;
    }
    return open_ff(d, url, 1);
}

/* A stream, through any playlists in the way: a link ending in .m3u, .m3u8 or
 * .pls is read for the stream it names (a playlist may name another), or is
 * HLS; a link that doesn't open as audio is looked at once more in case it's
 * a playlist all the same. */
static int open_net(decoder_t *d, const char *path)
{
    char url[1024], next[1024];
    snprintf(url, sizeof url, "%s", path);
    char *buf = malloc(LIST_MAX);
    if (!buf) return -1;
    int depth = 0, r = -1;
    for (; depth < LIST_DEPTH && streamlist_looks_like(url); depth++) {
        long n = fetch_text(d, url, buf, LIST_MAX);
        int k = n < 0 ? STREAMLIST_NONE : streamlist_first(buf, (size_t)n, url, next, sizeof next);
        if (k == STREAMLIST_HLS) { r = open_hls(d, url, buf, n); goto out; }
        if (k != STREAMLIST_FOUND) goto out;
        fprintf(stderr, "radio: %s names %s\n", url, next);
        snprintf(url, sizeof url, "%s", next);
    }
    r = open_stream(d, url);
    if (r && !d->fc && !d->vpb && depth < LIST_DEPTH && !(d->abort && *d->abort)) {
        long n = fetch_text(d, url, buf, 16384);
        int k = n > 0 ? streamlist_first(buf, (size_t)n, url, next, sizeof next) : STREAMLIST_NONE;
        if (k == STREAMLIST_HLS) {
            fprintf(stderr, "radio: %s is HLS\n", url);
            r = open_hls(d, url, buf, n);
        } else if (k == STREAMLIST_FOUND) {
            fprintf(stderr, "radio: %s is a playlist naming %s\n", url, next);
            r = open_stream(d, next);
        }
    }
out:
    free(buf);
    /* (not opened: a clock days behind may be why, a newer certificate
     * refused; the time is asked for, and the radio's next try finds it set) */
    if (r && !(d->abort && *d->abort) && netclock_net_failed) netclock_net_failed(path);
    return r;
}
#endif

/* ---------- public ---------- */

int decoder_is_stream(const char *path)
{
    return !strncasecmp(path, "http://", 7) || !strncasecmp(path, "https://", 8);
}

static int raw_seek(decoder_t *d, uint64_t frame);

/* Make `d` play the part [cs, ce) (CD frames; ce 0 = to the end) of its file:
 * 0, or -1 if the file has no such part. Seeks only if it's elsewhere. */
static int part_set(decoder_t *d, uint32_t cs, uint32_t ce)
{
    uint64_t s = (uint64_t)cs * d->fmt.rate / CUE_FPS, e = (uint64_t)ce * d->fmt.rate / CUE_FPS;
    if (d->file_frames && e >= d->file_frames) e = 0;   /* a sheet a little longer than its file */
    if ((d->file_frames && s >= d->file_frames) || (e && e <= s)) return -1;
    if (s != d->at && raw_seek(d, s)) return -1;
    d->part = 1;
    d->part_start = s;
    d->part_end = e;
    d->at = s;
    d->fmt.total_frames = e ? e - s : d->file_frames > s ? d->file_frames - s : 0;
    return 0;
}

static decoder_t *decoder_open_any(const char *path, audio_format_t *fmt, const volatile int *abort, unsigned flags)
{
    char file[1024];
    uint32_t cs, ce;
    if (cue_part_parse(path, file, sizeof file, &cs, &ce) > 0) {   /* a CUE track */
        decoder_t *d = decoder_open_any(file, NULL, abort, flags);
        if (!d) return NULL;
        d->file = strdup(file);
        if (!d->file || part_set(d, cs, ce)) { decoder_close(d); return NULL; }
        if (fmt) *fmt = d->fmt;
        return d;
    }
    const char *dot = strrchr(path, '.');
    decoder_t *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    int r = -1;
#ifdef FATHOM_HAVE_FFMPEG
    if (decoder_is_stream(path)) {
        d->kind = DEC_FF;
        d->net = 1;
        d->abort = abort;
        r = open_net(d, path);
        if (r) { decoder_close(d); return NULL; }
        if (fmt) *fmt = d->fmt;
        return d;
    }
#else
    (void)abort;
#endif
    if (!dot) { free(d); return NULL; }
    if (dsd_path(path)) {
        d->kind = DEC_DSD;
        d->dsd = dsd_open(path, (flags & DECODER_DOP) != 0, &d->fmt);
        r = d->dsd ? 0 : -1;
    }
    else if (!strcasecmp(dot, ".flac")) { d->kind = DEC_FLAC; r = open_flac(d, path); }
    else if (!strcasecmp(dot, ".mp3")) { d->kind = DEC_MP3; r = open_mp3(d, path); }
#ifdef FATHOM_HAVE_VORBIS
    /* .ogg and .oga hold Vorbis, Opus or FLAC: the first page says which */
    else if ((!strcasecmp(dot, ".ogg") || !strcasecmp(dot, ".oga")) && vorbis_file_is_vorbis(path)) {
        d->kind = DEC_VORBIS;
        r = open_vorbis(d, path);
    }
#endif
#ifdef FATHOM_HAVE_FFMPEG
    else { d->kind = DEC_FF; r = open_ff(d, path, 0); }
#endif
    if (r) { decoder_close(d); return NULL; }
    d->file_frames = d->fmt.total_frames;
    if (fmt) *fmt = d->fmt;
    return d;
}

int decoder_continue(decoder_t *d, const char *path, audio_format_t *fmt)
{
    char file[1024];
    uint32_t cs, ce;
    if (!d->part || !d->file || cue_part_parse(path, file, sizeof file, &cs, &ce) <= 0 || strcmp(file, d->file) ||
        (uint64_t)cs * d->fmt.rate / CUE_FPS != d->at || part_set(d, cs, ce))
        return -1;
    if (fmt) *fmt = d->fmt;
    return 0;
}

decoder_t *decoder_open(const char *path, audio_format_t *fmt)
{
    return decoder_open_ex(path, fmt, NULL);
}

decoder_t *decoder_open_ex(const char *path, audio_format_t *fmt, const volatile int *abort)
{
    return decoder_open_any(path, fmt, abort, 0);
}

decoder_t *decoder_open_flags(const char *path, audio_format_t *fmt, unsigned flags)
{
    return decoder_open_any(path, fmt, NULL, flags);
}

long decoder_read(decoder_t *d, int32_t *out, long max_frames)
{
    if (d->part_end) {                    /* a CUE track: up to the next one's first frame */
        if (d->at >= d->part_end) return 0;
        if ((uint64_t)max_frames > d->part_end - d->at) max_frames = (long)(d->part_end - d->at);
    }
    long n;
    if (d->kind == DEC_DSD) n = dsd_read(d->dsd, out, max_frames);
    else
#ifdef FATHOM_HAVE_FFMPEG
    if (d->kind == DEC_FF) n = read_ff(d, out, max_frames);
    else
#endif
    if (d->kind == DEC_VORBIS) {
        n = vorbis_read(d->vorbis, out, max_frames);
#ifdef FATHOM_HAVE_FFMPEG
        char t[256];
        if (d->net && vorbis_title(d->vorbis, t, sizeof t)) meta_set(d, t, NULL);   /* a new song (chained Ogg) */
#endif
    } else
    n = d->kind == DEC_FLAC ? read_flac(d, out, max_frames) : read_mp3(d, out, max_frames);
    if (n > 0) d->at += (uint64_t)n;
    return n;
}

int decoder_seek(decoder_t *d, uint64_t frame)
{
    uint64_t at = d->part_start + frame;
    if (d->part_end && at >= d->part_end) at = d->part_end - 1;
    if (raw_seek(d, at)) return -1;
    d->at = at;
    return 0;
}

static int raw_seek(decoder_t *d, uint64_t frame)
{
    if (d->kind == DEC_DSD) return dsd_seek(d->dsd, frame);
    if (d->kind == DEC_FLAC) {
        d->buf_frames = d->buf_pos = 0;
        if (FLAC__stream_decoder_seek_absolute(d->flac, frame)) return 0;
        /* a failed seek leaves the decoder needing a flush */
        FLAC__stream_decoder_flush(d->flac);
        return -1;
    }
#ifdef FATHOM_HAVE_FFMPEG
    if (d->kind == DEC_FF) return d->net ? -1 : seek_ff(d, frame);
#endif
#ifdef FATHOM_HAVE_FFMPEG
    if (d->kind == DEC_VORBIS && d->net) return -1;
#endif
    if (d->kind == DEC_VORBIS) return vorbis_seek(d->vorbis, frame);
    return mpg123_seek(d->mp3, (off_t)frame, SEEK_SET) >= 0 ? 0 : -1;
}

unsigned decoder_meta(decoder_t *d, char *out, size_t cap, char *url, size_t ucap)
{
#ifdef FATHOM_HAVE_FFMPEG
    if (out && cap) snprintf(out, cap, "%s", d->meta);
    if (url && ucap) snprintf(url, ucap, "%s", d->meta_url);
    return d->meta_seq;
#else
    (void)d;
    if (out && cap) out[0] = '\0';
    if (url && ucap) url[0] = '\0';
    return 0;
#endif
}

void decoder_set_next(decoder_t *d, const char *next_path)
{
    if (d->kind == DEC_DSD) dsd_set_followed(d->dsd, next_path && dsd_path(next_path));
}

void decoder_join(decoder_t *d, const decoder_t *prev)
{
    if (d->kind == DEC_DSD && prev->kind == DEC_DSD) dsd_join(d->dsd, prev->dsd);
}

void decoder_close(decoder_t *d)
{
    if (!d) return;
    dsd_close(d->dsd);
    if (d->flac) { FLAC__stream_decoder_finish(d->flac); FLAC__stream_decoder_delete(d->flac); }
    if (d->mp3) { mpg123_close(d->mp3); mpg123_delete(d->mp3); }
    if (d->mp3 && d->mp3_fd >= 0) close(d->mp3_fd);
    vorbis_close(d->vorbis);
#ifdef FATHOM_HAVE_FFMPEG
    avio_closep(&d->vpb);                /* (after Tremor, which reads from it) */
    av_frame_free(&d->frm);
    av_packet_free(&d->pkt);
    avcodec_free_context(&d->cc);
    avformat_close_input(&d->fc);
#endif
    free(d->buf);
    free(d->file);
    free(d);
}
