/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "audio/ff_util.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

void ff_quiet(void)
{
    static int done;
    if (!done) av_log_set_level(AV_LOG_QUIET);
    done = 1;
}

int ff_bits(const AVCodecParameters *par)
{
    const AVCodecDescriptor *desc = avcodec_descriptor_get(par->codec_id);
    if (!desc || !(desc->props & AV_CODEC_PROP_LOSSLESS)) return 0;
    if (par->codec_id == AV_CODEC_ID_PCM_F32LE || par->codec_id == AV_CODEC_ID_PCM_F32BE ||
        par->codec_id == AV_CODEC_ID_PCM_F64LE || par->codec_id == AV_CODEC_ID_PCM_F64BE)
        return 0;                                     /* float: not carried bit for bit */
    /* ALAC: the depth is in its "magic cookie" (an 'alac' atom of 36 bytes, or
     * its 24-byte body), which FFmpeg reads only when the decoder opens: the
     * container's sample size field says 16 even for 24-bit files. */
    if (par->codec_id == AV_CODEC_ID_ALAC && par->extradata) {
        const uint8_t *x = par->extradata;
        if (par->extradata_size >= 36 && !memcmp(x + 4, "alac", 4)) return x[17];
        if (par->extradata_size >= 24) return x[5];
    }
    int b = par->bits_per_raw_sample ? par->bits_per_raw_sample : par->bits_per_coded_sample;
    return b > 0 && b <= 32 ? b : 0;
}

uint64_t ff_frames(const AVFormatContext *fc, const AVStream *st)
{
    int rate = st->codecpar->sample_rate;
    if (rate <= 0) return 0;
    if (st->duration != AV_NOPTS_VALUE && st->duration > 0)
        return (uint64_t)av_rescale_q(st->duration, st->time_base, (AVRational){1, rate});
    if (fc->duration != AV_NOPTS_VALUE && fc->duration > 0)
        return (uint64_t)av_rescale(fc->duration, rate, AV_TIME_BASE);
    return 0;
}

void ff_codec_name(const AVFormatContext *fc, const AVCodecParameters *par, char *out, size_t cap)
{
    const char *n;
    switch (par->codec_id) {
    case AV_CODEC_ID_ALAC: n = "ALAC"; break;
    case AV_CODEC_ID_AAC: n = "AAC"; break;
    case AV_CODEC_ID_OPUS: n = "Opus"; break;
    case AV_CODEC_ID_VORBIS: n = "Vorbis"; break;
    case AV_CODEC_ID_WAVPACK: n = "WavPack"; break;
    case AV_CODEC_ID_APE: n = "APE"; break;
    case AV_CODEC_ID_TTA: n = "TTA"; break;
    case AV_CODEC_ID_FLAC: n = "FLAC"; break;     /* (streams: Ogg FLAC) */
    case AV_CODEC_ID_MP3: n = "MP3"; break;
    default: {
        const char *f = fc && fc->iformat ? fc->iformat->name : "";
        n = strstr(f, "aiff") ? "AIFF" : (strstr(f, "wav") || strstr(f, "w64")) ? "WAV" : "PCM";
    }
    }
    snprintf(out, cap, "%s", n);
}

static int32_t clip_f(double v)
{
    if (v >= 1.0) return INT32_MAX;
    if (v <= -1.0) return INT32_MIN;
    return (int32_t)(v * 2147483648.0);
}

int ff_to_s32(const AVFrame *f, int ch, int32_t *out)
{
    int n = f->nb_samples;
    enum AVSampleFormat fmt = (enum AVSampleFormat)f->format;
    int planar = av_sample_fmt_is_planar(fmt);
    for (int c = 0; c < ch; c++) {
        const uint8_t *p = planar ? f->extended_data[c] : f->extended_data[0];
        int step = planar ? 1 : ch, at = planar ? 0 : c;
        int32_t *o = out + c;
        switch (av_get_packed_sample_fmt(fmt)) {
        case AV_SAMPLE_FMT_U8:
            for (int i = 0; i < n; i++) o[i * ch] = (int32_t)((uint32_t)(p[i * step + at] ^ 0x80) << 24);
            break;
        case AV_SAMPLE_FMT_S16: {
            const int16_t *s = (const int16_t *)p;
            for (int i = 0; i < n; i++) o[i * ch] = (int32_t)((uint32_t)(uint16_t)s[i * step + at] << 16);
            break;
        }
        case AV_SAMPLE_FMT_S32: {             /* FFmpeg's 32-bit samples are left-justified */
            const int32_t *s = (const int32_t *)p;
            for (int i = 0; i < n; i++) o[i * ch] = s[i * step + at];
            break;
        }
        case AV_SAMPLE_FMT_FLT: {
            const float *s = (const float *)p;
            for (int i = 0; i < n; i++) o[i * ch] = clip_f(s[i * step + at]);
            break;
        }
        case AV_SAMPLE_FMT_DBL: {
            const double *s = (const double *)p;
            for (int i = 0; i < n; i++) o[i * ch] = clip_f(s[i * step + at]);
            break;
        }
        default:
            return -1;
        }
    }
    return 0;
}

static char g_ca_file[512];

void ff_set_ca_file(const char *path)
{
    snprintf(g_ca_file, sizeof g_ca_file, "%s", path && !access(path, R_OK) ? path : "");
}

/* Always checked: without the bundle nothing verifies, so HTTPS fails
 * rather than trusting whoever answers (updates are installed from it). */
void ff_tls_options(AVDictionary **opt)
{
    if (g_ca_file[0]) av_dict_set(opt, "ca_file", g_ca_file, 0);
    av_dict_set(opt, "tls_verify", "1", 0);
}

const char *ff_ca_file(void)
{
    return g_ca_file[0] ? g_ca_file : NULL;
}
