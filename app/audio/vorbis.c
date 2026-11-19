/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Ogg Vorbis through Tremor or libvorbisfile: see vorbis.h. */
#include "audio/vorbis.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

int vorbis_sniff(const unsigned char *h, size_t n)
{
    /* "OggS", version 0, the first page of a stream, then its first packet */
    if (n < 27 || memcmp(h, "OggS", 4) || h[4] != 0 || !(h[5] & 0x02)) return 0;
    size_t at = 27 + (size_t)h[26];
    return n >= at + 7 && !memcmp(h + at, "\x01vorbis", 7);
}

int vorbis_file_is_vorbis(const char *path)
{
    unsigned char h[27 + 255 + 7];
    FILE *f = fopen(path, "rbe");
    if (!f) return 0;
    size_t n = fread(h, 1, sizeof h, f);
    fclose(f);
    return vorbis_sniff(h, n);
}

#ifdef FATHOM_HAVE_VORBIS

#ifdef FATHOM_VORBIS_TREMOR
/* Tremor installs its headers in include/tremor; some systems put them in
 * include itself */
#if defined(__has_include)
#if __has_include(<tremor/ivorbisfile.h>)
#include <tremor/ivorbisfile.h>
#else
#include <ivorbisfile.h>
#endif
#else
#include <tremor/ivorbisfile.h>
#endif
#else
#include <vorbis/vorbisfile.h>
#endif

struct vorbis_file {
    OggVorbis_File vf;     /* (a few KB: on the heap, never the stack) */
    int link, channels;
    long rate;
    int ended;             /* a chained file changed format: the track ends there */
    int tags_new;          /* a link began: its tags not yet asked for */
    size_t (*src)(void *ctx, void *buf, size_t n);   /* a stream's bytes */
    void *src_ctx;
};

/* stdio for vorbisfile (ov_open takes a FILE * only on some builds of each) */
static size_t cb_read(void *p, size_t size, size_t n, void *f) { return fread(p, size, n, f); }
static int cb_seek(void *f, ogg_int64_t off, int whence) { return fseek(f, (long)off, whence); }
static int cb_close(void *f) { return fclose(f); }
static long cb_tell(void *f) { return ftell(f); }

/* After ov_open_callbacks: the format, or NULL (and v closed) if it's none we play */
static vorbis_file_t *opened(vorbis_file_t *v, uint32_t *rate, uint8_t *channels, uint64_t *frames)
{
    vorbis_info *vi = ov_info(&v->vf, -1);
    if (!vi || vi->channels < 1 || vi->channels > 2 || vi->rate <= 0) {   /* stereo DAC; no downmixing */
        vorbis_close(v);
        return NULL;
    }
    v->channels = vi->channels;
    v->rate = vi->rate;
    v->link = 0;
    v->tags_new = 1;
    ogg_int64_t total = v->src ? 0 : ov_pcm_total(&v->vf, -1);
    *rate = (uint32_t)vi->rate;
    *channels = (uint8_t)vi->channels;
    if (frames) *frames = total > 0 ? (uint64_t)total : 0;
    return v;
}

vorbis_file_t *vorbis_open(const char *path, uint32_t *rate, uint8_t *channels, uint64_t *frames)
{
    vorbis_file_t *v = calloc(1, sizeof *v);
    FILE *f = v ? fopen(path, "rbe") : NULL;
    if (!f) { free(v); return NULL; }
    ov_callbacks cb = {cb_read, cb_seek, cb_close, cb_tell};
    if (ov_open_callbacks(f, &v->vf, NULL, 0, cb) < 0) {   /* (on failure the file is still ours) */
        fclose(f);
        free(v);
        return NULL;
    }
    return opened(v, rate, channels, frames);
}

static size_t cb_src(void *p, size_t size, size_t n, void *vv)
{
    vorbis_file_t *v = vv;
    return size && n ? v->src(v->src_ctx, p, size * n) / size : 0;
}

vorbis_file_t *vorbis_open_stream(size_t (*src)(void *ctx, void *buf, size_t n), void *ctx,
                                  uint32_t *rate, uint8_t *channels)
{
    vorbis_file_t *v = calloc(1, sizeof *v);
    if (!v) return NULL;
    v->src = src;
    v->src_ctx = ctx;
    ov_callbacks cb = {cb_src, NULL, NULL, NULL};          /* no seeking: vorbisfile streams */
    if (ov_open_callbacks(v, &v->vf, NULL, 0, cb) < 0) {
        free(v);
        return NULL;
    }
    return opened(v, rate, channels, NULL);
}

/* A new link of a chained file (or stream: a new song, on some stations):
 * fine if it keeps the format (else the engine would need to start again:
 * the track ends there instead). */
static int same_link(vorbis_file_t *v, int link)
{
    if (link == v->link) return 1;
    vorbis_info *vi = ov_info(&v->vf, link);
    if (!vi || vi->channels != v->channels || vi->rate != v->rate) return 0;
    v->link = link;
    v->tags_new = 1;
    return 1;
}

/* A comment's value (KEY=value, the key in any case), or NULL */
static const char *comment(const vorbis_comment *vc, const char *key, int *len)
{
    size_t kl = strlen(key);
    for (int i = 0; vc && i < vc->comments; i++) {
        const char *c = vc->user_comments[i];
        int n = vc->comment_lengths[i];
        if (c && n > (int)kl && c[kl] == '=' && !strncasecmp(c, key, kl)) {
            *len = n - (int)kl - 1;
            return c + kl + 1;
        }
    }
    return NULL;
}

int vorbis_title(vorbis_file_t *v, char *out, size_t cap)
{
    if (!v->tags_new || !cap) return 0;
    v->tags_new = 0;
    const vorbis_comment *vc = ov_comment(&v->vf, -1);
    int al = 0, tl = 0;
    const char *a = comment(vc, "ARTIST", &al), *t = comment(vc, "TITLE", &tl);
    if (!t) return 0;
    if (a) snprintf(out, cap, "%.*s - %.*s", al, a, tl, t);
    else snprintf(out, cap, "%.*s", tl, t);
    return 1;
}

#ifdef FATHOM_VORBIS_TREMOR
/* Tremor's ov_read gives 16-bit samples (its fixed-point output, cut to 16
 * bits): read into out and widened in place, back to front, as for mpg123's. */
long vorbis_read(vorbis_file_t *v, int32_t *out, long max)
{
    if (v->ended) return 0;
    int ch = v->channels;
    long got = 0;
    while (got < max) {
        long want = (max - got) * ch * 2;
        if (want > 1 << 20) want = 1 << 20;
        char *raw = (char *)(out + got * ch);   /* the space for the rest: twice what's asked */
        int link = 0;
        long r = ov_read(&v->vf, raw, (int)want, &link);
        if (r == OV_HOLE) continue;                   /* a gap in the data: decoding goes on after it */
        if (r < 0) return got ? got : -1;
        if (r == 0) break;
        if (!same_link(v, link)) { v->ended = 1; break; }
        long n = r / (2 * ch);
        for (long i = n * ch - 1; i >= 0; i--) {   /* (memcpy: the int16s and int32s share the bytes) */
            int16_t s;
            memcpy(&s, raw + 2 * i, 2);
            out[got * ch + i] = (int32_t)((uint32_t)(uint16_t)s << 16);
        }
        got += n;
    }
    return got;
}
#else
/* libvorbisfile: its float samples (a computer has the FPU for it) */
static int32_t clip_f(float x)
{
    if (x >= 1.0f) return INT32_MAX;
    if (x <= -1.0f) return INT32_MIN;
    return (int32_t)((double)x * 2147483648.0);
}

long vorbis_read(vorbis_file_t *v, int32_t *out, long max)
{
    if (v->ended) return 0;
    int ch = v->channels;
    long got = 0;
    while (got < max) {
        float **pcm;
        int link = 0;
        long want = max - got > 1 << 16 ? 1 << 16 : max - got;
        long n = ov_read_float(&v->vf, &pcm, (int)want, &link);
        if (n == OV_HOLE) continue;
        if (n < 0) return got ? got : -1;
        if (n == 0) break;
        if (!same_link(v, link)) { v->ended = 1; break; }
        for (long i = 0; i < n; i++)
            for (int c = 0; c < ch; c++) out[(got + i) * ch + c] = clip_f(pcm[c][i]);
        got += n;
    }
    return got;
}
#endif

int vorbis_seek(vorbis_file_t *v, uint64_t frame)
{
    if (ov_pcm_seek(&v->vf, (ogg_int64_t)frame)) return -1;
    v->ended = 0;
    return 0;
}

void vorbis_close(vorbis_file_t *v)
{
    if (!v) return;
    ov_clear(&v->vf);   /* closes the file (cb_close) */
    free(v);
}

#else   /* no Vorbis library */

vorbis_file_t *vorbis_open(const char *path, uint32_t *rate, uint8_t *channels, uint64_t *frames)
{
    (void)path; (void)rate; (void)channels; (void)frames;
    return NULL;
}
vorbis_file_t *vorbis_open_stream(size_t (*src)(void *ctx, void *buf, size_t n), void *ctx,
                                  uint32_t *rate, uint8_t *channels)
{
    (void)src; (void)ctx; (void)rate; (void)channels;
    return NULL;
}
int vorbis_title(vorbis_file_t *v, char *out, size_t cap) { (void)v; (void)out; (void)cap; return 0; }
long vorbis_read(vorbis_file_t *v, int32_t *out, long max) { (void)v; (void)out; (void)max; return -1; }
int vorbis_seek(vorbis_file_t *v, uint64_t frame) { (void)v; (void)frame; return -1; }
void vorbis_close(vorbis_file_t *v) { (void)v; }

#endif
