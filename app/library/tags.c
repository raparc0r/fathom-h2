/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
#define _FILE_OFFSET_BITS 64   /* DSD files pass 2 GB */
#include "tags.h"

#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <FLAC/metadata.h>
#include <mpg123.h>

#include "audio/dsd.h"
#ifdef FATHOM_HAVE_FFMPEG
#include "audio/ff_util.h"
#endif
#include "library/cue.h"
#include "audio/vorbis.h"

/* ---------- helpers ---------- */

static const char *ext_of(const char *path)
{
    const char *dot = strrchr(path, '.');
    const char *slash = strrchr(path, '/');
    return (dot && (!slash || dot > slash)) ? dot + 1 : "";
}

/* Formats through FFmpeg (with the decoders the H2's build has: integer ones,
 * as Fathom is built soft-float). Ogg Vorbis is read here and decoded by Tremor (an .ogg
 * holding Opus or FLAC goes to FFmpeg). */
#ifdef FATHOM_HAVE_FFMPEG
static const char *const FF_EXT[] = {"m4a", "m4b", "mp4", "alac", "aac", "wav", "wave", "w64", "aif", "aiff",
                                     "aifc", "wv", "ape", "tta", "opus", "ogg", "oga", NULL};

static int ff_ext(const char *e)
{
    for (int i = 0; FF_EXT[i]; i++)
        if (!strcasecmp(e, FF_EXT[i])) return 1;
    return 0;
}
#endif

#ifdef FATHOM_HAVE_VORBIS
static int ogg_ext(const char *e) { return !strcasecmp(e, "ogg") || !strcasecmp(e, "oga"); }
#endif

int tags_supported(const char *path)
{
    const char *e = ext_of(path);
#ifdef FATHOM_HAVE_FFMPEG
    if (ff_ext(e)) return 1;
#endif
#ifdef FATHOM_HAVE_VORBIS
    if (ogg_ext(e)) return 1;
#endif
    return !strcasecmp(e, "flac") || !strcasecmp(e, "mp3") || dsd_path(path);
}

static void set_str(char *dst, size_t cap, const char *src, size_t len)
{
    if (len >= cap) len = cap - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
    /* trim trailing whitespace and NULs */
    while (len > 0 && (dst[len - 1] == ' ' || dst[len - 1] == '\0')) dst[--len] = '\0';
}

static int leading_int(const char *s)
{
    while (*s && !isdigit((unsigned char)*s)) s++;
    return atoi(s);
}

/* ---------- ReplayGain ---------- */

/* REPLAYGAIN_TRACK_GAIN "-7.89 dB" and friends; R128_*_GAIN (Q7.8 dB from
 * -23 LUFS) moved to ReplayGain's -18 LUFS. Returns 1 if `key` was one. */
static int rg_field(tags_t *t, const char *key, const char *val)
{
    double v = strtod(val, NULL);
    if (!strcasecmp(key, "REPLAYGAIN_TRACK_GAIN")) { t->rg_track_gain = (float)v; t->has_rg_track = 1; }
    else if (!strcasecmp(key, "REPLAYGAIN_ALBUM_GAIN")) { t->rg_album_gain = (float)v; t->has_rg_album = 1; }
    else if (!strcasecmp(key, "REPLAYGAIN_TRACK_PEAK")) t->rg_track_peak = (float)v;
    else if (!strcasecmp(key, "REPLAYGAIN_ALBUM_PEAK")) t->rg_album_peak = (float)v;
    else if (!strcasecmp(key, "R128_TRACK_GAIN") && !t->has_rg_track) { t->rg_track_gain = (float)(v / 256.0 + 5.0); t->has_rg_track = 1; }
    else if (!strcasecmp(key, "R128_ALBUM_GAIN") && !t->has_rg_album) { t->rg_album_gain = (float)(v / 256.0 + 5.0); t->has_rg_album = 1; }
    else return 0;
    return 1;
}

/* ---------- FLAC ---------- */

static void flac_comment(tags_t *t, const char *entry, size_t len)
{
    const char *eq = memchr(entry, '=', len);
    if (!eq) return;
    size_t klen = (size_t)(eq - entry);
    const char *v = eq + 1;
    size_t vlen = len - klen - 1;
    char val[64];
#define KEY(name) (klen == sizeof(name) - 1 && !strncasecmp(entry, name, klen))
    if (KEY("TITLE")) set_str(t->title, sizeof t->title, v, vlen);
    else if (KEY("ARTIST") && !t->artist[0]) set_str(t->artist, sizeof t->artist, v, vlen);
    else if (KEY("ALBUMARTIST") || KEY("ALBUM ARTIST") || KEY("ALBUM_ARTIST"))
        set_str(t->album_artist, sizeof t->album_artist, v, vlen);
    else if (KEY("ALBUM")) set_str(t->album, sizeof t->album, v, vlen);
    else if (KEY("GENRE") && !t->genre[0]) set_str(t->genre, sizeof t->genre, v, vlen);
    else if (KEY("COMPOSER") && !t->composer[0]) set_str(t->composer, sizeof t->composer, v, vlen);
    else if (KEY("DATE") || KEY("YEAR") || KEY("ORIGINALDATE")) {
        set_str(val, sizeof val, v, vlen);
        if (!t->year || KEY("ORIGINALDATE")) t->year = leading_int(val);
    } else if (KEY("TRACKNUMBER")) { set_str(val, sizeof val, v, vlen); t->track = leading_int(val); }
    else if (KEY("DISCNUMBER")) { set_str(val, sizeof val, v, vlen); t->disc = leading_int(val); }
    else if (KEY("CUESHEET")) t->has_cuesheet = 1;
    else if (klen > 5 && (!strncasecmp(entry, "REPLAYGAIN_", 11) || !strncasecmp(entry, "R128_", 5)) && klen < 32) {
        char key[32];
        memcpy(key, entry, klen);
        key[klen] = '\0';
        set_str(val, sizeof val, v, vlen);
        rg_field(t, key, val);
    }
#undef KEY
}

/* The same as read_flac() through libFLAC: three passes over the file's
 * metadata, and the front cover read whole to learn there is one. Kept to
 * check read_flac() against (tags_read_flac_reference). */
static int read_flac_libflac(const char *path, tags_t *t)
{
    FLAC__StreamMetadata si;
    if (!FLAC__metadata_get_streaminfo(path, &si)) return -1;
    strcpy(t->codec, "FLAC");
    t->sample_rate = si.data.stream_info.sample_rate;
    t->bits = (uint8_t)si.data.stream_info.bits_per_sample;
    t->channels = (uint8_t)si.data.stream_info.channels;
    if (t->sample_rate)
        t->duration_ms = (uint32_t)(si.data.stream_info.total_samples * 1000 / t->sample_rate);

    FLAC__StreamMetadata *vc = NULL;
    if (FLAC__metadata_get_tags(path, &vc) && vc) {
        for (FLAC__uint32 i = 0; i < vc->data.vorbis_comment.num_comments; i++) {
            const FLAC__StreamMetadata_VorbisComment_Entry *e = &vc->data.vorbis_comment.comments[i];
            flac_comment(t, (const char *)e->entry, e->length);
        }
        FLAC__metadata_object_delete(vc);
    }
    FLAC__StreamMetadata *pic = NULL;   /* (any picture: the fast reader leaves out only icons) */
    if (FLAC__metadata_get_picture(path, &pic, (FLAC__StreamMetadata_Picture_Type)(-1),
                                   NULL, NULL, (unsigned)-1, (unsigned)-1, (unsigned)-1, (unsigned)-1) && pic) {
        t->has_picture = pic->data.picture.type != FLAC__STREAM_METADATA_PICTURE_TYPE_FILE_ICON_STANDARD &&
                         pic->data.picture.type != FLAC__STREAM_METADATA_PICTURE_TYPE_FILE_ICON;
        FLAC__metadata_object_delete(pic);
    }
    return 0;
}

static uint32_t be24(const unsigned char *p) { return (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2]; }
static uint32_t be32(const unsigned char *p) { return (uint32_t)p[0] << 24 | be24(p + 1); }
static uint32_t le32(const unsigned char *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

#define FLAC_VC_MAX (16u << 20)    /* a comment block bigger than this is skipped */

/* A FLAC's tags in one pass over its metadata blocks: STREAMINFO, the first
 * VORBIS_COMMENT, and of each PICTURE only its type (whether there's a front
 * cover), skipping the picture itself: covers of a few hundred KB each are
 * most of a library's metadata, and libFLAC read one whole per file (three
 * passes in all) just to say it's there. Same results (tools/tags-compare). */
static int read_flac_cue(const char *path, tags_t *t, char **cue, size_t *cue_len);
static int read_flac(const char *path, tags_t *t) { return read_flac_cue(path, t, NULL, NULL); }

/* The same, and with `cue` the CUESHEET comment's text (malloc'd) */
static int read_flac_cue(const char *path, tags_t *t, char **cue, size_t *cue_len)
{
    FILE *f = fopen(path, "rbe");
    if (!f) return -1;
    unsigned char h[10];
    int ok = 0, vc_done = 0;
    if (fread(h, 1, 4, f) != 4) goto out;
    if (!memcmp(h, "ID3", 3)) {                   /* an ID3v2 tag in front (libFLAC skips it too) */
        if (fread(h + 4, 1, 6, f) != 6) goto out;
        long skip = 10 + ((long)(h[6] & 0x7f) << 21 | (long)(h[7] & 0x7f) << 14 | (long)(h[8] & 0x7f) << 7 | (h[9] & 0x7f));
        if (h[5] & 0x10) skip += 10;                /* footer */
        if (fseek(f, skip, SEEK_SET) || fread(h, 1, 4, f) != 4) goto out;
    }
    if (memcmp(h, "fLaC", 4)) goto out;
    for (int last = 0; !last;) {
        unsigned char bh[4];
        if (fread(bh, 1, 4, f) != 4) break;
        last = bh[0] & 0x80;
        int type = bh[0] & 0x7f;
        uint32_t len = be24(bh + 1);
        if (type == 0 && !ok && len >= 34) {          /* STREAMINFO */
            unsigned char b[34];
            if (fread(b, 1, 34, f) != 34) break;
            uint32_t rate = (uint32_t)b[10] << 12 | (uint32_t)b[11] << 4 | b[12] >> 4;
            unsigned ch = ((b[12] >> 1) & 7) + 1, bps = (((b[12] & 1) << 4) | (b[13] >> 4)) + 1;
            uint64_t total = (uint64_t)(b[13] & 0x0f) << 32 | be32(b + 14);
            strcpy(t->codec, "FLAC");
            t->sample_rate = rate;
            t->bits = (uint8_t)bps;
            t->channels = (uint8_t)ch;
            if (rate) t->duration_ms = (uint32_t)(total * 1000 / rate);
            ok = 1;
            if (len > 34 && fseek(f, (long)(len - 34), SEEK_CUR)) break;
        } else if (type == 4 && !vc_done && len <= FLAC_VC_MAX) {   /* VORBIS_COMMENT */
            unsigned char *b = malloc(len ? len : 1);
            if (!b) break;
            if (fread(b, 1, len, f) != len) { free(b); break; }
            vc_done = 1;
            uint32_t at = 0;
            if (len >= 8) {
                uint32_t vlen = le32(b);
                at = 4 + vlen;
                if (vlen <= len - 8) {
                    uint32_t n = le32(b + at);
                    at += 4;
                    for (uint32_t i = 0; i < n && at + 4 <= len; i++) {
                        uint32_t el = le32(b + at);
                        at += 4;
                        if (el > len - at) break;
                        flac_comment(t, (const char *)b + at, el);
                        if (cue && !*cue && el > 9 && el - 9 < CUE_MAX_BYTES && !strncasecmp((const char *)b + at, "CUESHEET=", 9)) {
                            if ((*cue = malloc(el - 9 + 1))) {
                                memcpy(*cue, b + at + 9, el - 9);
                                (*cue)[el - 9] = '\0';
                                *cue_len = el - 9;
                            }
                        }
                        at += el;
                    }
                }
            }
            free(b);
        } else if (type == 6 && len >= 4) {           /* PICTURE: its type only */
            unsigned char pt[4];
            if (fread(pt, 1, 4, f) != 4) break;
            if (be32(pt) != 1 && be32(pt) != 2) t->has_picture = 1;   /* a cover of any kind, not an icon */
            if (fseek(f, (long)(len - 4), SEEK_CUR)) break;
        } else if (type == 127) {
            break;                                     /* invalid */
        } else if (fseek(f, (long)len, SEEK_CUR)) {
            break;
        }
    }
out:
    fclose(f);
    return ok ? 0 : -1;
}

static int tags_finish(const char *path, tags_t *t, int r);

int tags_read_flac_reference(const char *path, tags_t *t)
{
    memset(t, 0, sizeof *t);
    return tags_finish(path, t, read_flac_libflac(path, t));
}

/* ---------- ID3v2 (MP3, DSF, DFF) ---------- */

typedef struct {
    unsigned char *buf;  /* whole tag body (after the 10-byte header and any extended header) */
    size_t len;
    int version;         /* 2, 3 or 4 */
} id3_t;

static uint32_t syncsafe(const unsigned char *p)
{
    return ((uint32_t)p[0] << 21) | ((uint32_t)p[1] << 14) | ((uint32_t)p[2] << 7) | p[3];
}

static int id3_load(FILE *f, id3_t *id)
{
    unsigned char h[10];
    memset(id, 0, sizeof *id);
    if (fread(h, 1, 10, f) != 10 || memcmp(h, "ID3", 3) || h[3] < 2 || h[3] > 4) return -1;
    if (h[3] == 2 && (h[5] & 0x40)) return -1;   /* v2.2 compressed: no scheme was ever defined */
    id->version = h[3];
    id->len = syncsafe(h + 6);
    if (id->len > 64 * 1024 * 1024) return -1;
    id->buf = malloc(id->len ? id->len : 1);
    if (!id->buf || fread(id->buf, 1, id->len, f) != id->len) { free(id->buf); id->buf = NULL; return -1; }
    if (h[5] & 0x80) {  /* whole-tag unsynchronisation (v2.2, v2.3): undo FF 00 -> FF */
        size_t w = 0;
        for (size_t r = 0; r < id->len; r++) {
            id->buf[w++] = id->buf[r];
            if (id->buf[r] == 0xFF && r + 1 < id->len && id->buf[r + 1] == 0x00) r++;
        }
        id->len = w;
    }
    if (id->version > 2 && (h[5] & 0x40) && id->len >= 4) {
        /* an extended header (a CRC, restrictions) before the frames: its
         * size excludes its own 4 bytes in v2.3, includes them (syncsafe) in
         * v2.4. Read as frames, it ended the tag: no tags at all. */
        size_t skip = id->version == 3 ? 4 + ((size_t)id->buf[0] << 24 | (size_t)id->buf[1] << 16 |
                                              (size_t)id->buf[2] << 8 | id->buf[3])
                                       : syncsafe(id->buf);
        if (skip > id->len) skip = id->len;
        memmove(id->buf, id->buf + skip, id->len - skip);
        id->len -= skip;
    }
    return 0;
}

/* v2.2's three-letter frames by their v2.3 names (iTunes wrote v2.2 for years) */
static const char *const ID3_V22[][2] = {
    {"TT2", "TIT2"}, {"TP1", "TPE1"}, {"TP2", "TPE2"}, {"TAL", "TALB"}, {"TCO", "TCON"}, {"TCM", "TCOM"},
    {"TRK", "TRCK"}, {"TPA", "TPOS"}, {"TYE", "TYER"}, {"TXX", "TXXX"},
};

/* Iterate frames: returns pointer to next frame body, fills id/size. A v2.2
 * frame comes with its v2.3 name, but its picture as "PIC" (laid out
 * differently from APIC). */
static const unsigned char *id3_next(const id3_t *id, size_t *pos, char fid[5], size_t *size)
{
    size_t hl = id->version == 2 ? 6 : 10;
    while (*pos + hl <= id->len) {
        const unsigned char *h = id->buf + *pos;
        if (h[0] == 0) return NULL;  /* padding */
        uint32_t sz;
        if (id->version == 2) {
            memcpy(fid, h, 3);
            fid[3] = '\0';
            for (size_t i = 0; i < sizeof ID3_V22 / sizeof ID3_V22[0]; i++)
                if (!strcmp(fid, ID3_V22[i][0])) { memcpy(fid, ID3_V22[i][1], 5); break; }
            sz = (uint32_t)h[3] << 16 | (uint32_t)h[4] << 8 | h[5];
        } else {
            memcpy(fid, h, 4);
            fid[4] = '\0';
            sz = id->version == 4 ? syncsafe(h + 4)
                                  : ((uint32_t)h[4] << 24 | (uint32_t)h[5] << 16 | (uint32_t)h[6] << 8 | h[7]);
        }
        *pos += hl;
        if (*pos + sz > id->len) return NULL;
        const unsigned char *body = id->buf + *pos;
        *pos += sz;
        *size = sz;
        return body;
    }
    return NULL;
}

/* Decode an ID3 text frame body into UTF-8. */
static void id3_text(const unsigned char *b, size_t n, char *out, size_t cap)
{
    size_t w = 0;
    if (n == 0 || cap == 0) { if (cap) out[0] = '\0'; return; }
    int enc = b[0];
    b++; n--;
    if (enc == 0 || enc == 3) {  /* Latin-1 or UTF-8 */
        for (size_t i = 0; i < n && b[i] && w + 3 < cap; i++) {
            unsigned c = b[i];
            if (enc == 0 && c >= 0x80) { out[w++] = (char)(0xC0 | (c >> 6)); out[w++] = (char)(0x80 | (c & 0x3F)); }
            else out[w++] = (char)c;
        }
    } else {  /* UTF-16 with BOM (1) or big-endian (2) */
        int le = 0;
        size_t i = 0;
        if (enc == 1 && n >= 2) {
            if (b[0] == 0xFF && b[1] == 0xFE) { le = 1; i = 2; }
            else if (b[0] == 0xFE && b[1] == 0xFF) { i = 2; }
        }
        for (; i + 1 < n && w + 4 < cap; i += 2) {
            uint32_t u = le ? (uint32_t)(b[i] | b[i + 1] << 8) : (uint32_t)(b[i] << 8 | b[i + 1]);
            if (u == 0) break;
            if (u >= 0xD800 && u < 0xDC00 && i + 3 < n) {
                uint32_t lo = le ? (uint32_t)(b[i + 2] | b[i + 3] << 8) : (uint32_t)(b[i + 2] << 8 | b[i + 3]);
                u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
                i += 2;
            }
            if (u < 0x80) out[w++] = (char)u;
            else if (u < 0x800) { out[w++] = (char)(0xC0 | u >> 6); out[w++] = (char)(0x80 | (u & 0x3F)); }
            else if (u < 0x10000) { out[w++] = (char)(0xE0 | u >> 12); out[w++] = (char)(0x80 | ((u >> 6) & 0x3F)); out[w++] = (char)(0x80 | (u & 0x3F)); }
            else { out[w++] = (char)(0xF0 | u >> 18); out[w++] = (char)(0x80 | ((u >> 12) & 0x3F)); out[w++] = (char)(0x80 | ((u >> 6) & 0x3F)); out[w++] = (char)(0x80 | (u & 0x3F)); }
        }
    }
    out[w] = '\0';
    while (w > 0 && out[w - 1] == ' ') out[--w] = '\0';
}

/* ID3v1's genres by number (with Winamp's additions), which v2.2 and v2.3
 * tags also give as "(17)" or "17". */
static const char *const ID3_GENRES[] = {
    "Blues", "Classic Rock", "Country", "Dance", "Disco", "Funk", "Grunge", "Hip-Hop", "Jazz", "Metal",
    "New Age", "Oldies", "Other", "Pop", "R&B", "Rap", "Reggae", "Rock", "Techno", "Industrial",
    "Alternative", "Ska", "Death Metal", "Pranks", "Soundtrack", "Euro-Techno", "Ambient", "Trip-Hop", "Vocal", "Jazz+Funk",
    "Fusion", "Trance", "Classical", "Instrumental", "Acid", "House", "Game", "Sound Clip", "Gospel", "Noise",
    "Alternative Rock", "Bass", "Soul", "Punk", "Space", "Meditative", "Instrumental Pop", "Instrumental Rock", "Ethnic", "Gothic",
    "Darkwave", "Techno-Industrial", "Electronic", "Pop-Folk", "Eurodance", "Dream", "Southern Rock", "Comedy", "Cult", "Gangsta",
    "Top 40", "Christian Rap", "Pop/Funk", "Jungle", "Native American", "Cabaret", "New Wave", "Psychedelic", "Rave", "Showtunes",
    "Trailer", "Lo-Fi", "Tribal", "Acid Punk", "Acid Jazz", "Polka", "Retro", "Musical", "Rock & Roll", "Hard Rock",
    "Folk", "Folk-Rock", "National Folk", "Swing", "Fast Fusion", "Bebop", "Latin", "Revival", "Celtic", "Bluegrass",
    "Avantgarde", "Gothic Rock", "Progressive Rock", "Psychedelic Rock", "Symphonic Rock", "Slow Rock", "Big Band", "Chorus",
    "Easy Listening", "Acoustic", "Humour", "Speech", "Chanson", "Opera", "Chamber Music", "Sonata", "Symphony", "Booty Bass",
    "Primus", "Porn Groove", "Satire", "Slow Jam", "Club", "Tango", "Samba", "Folklore", "Ballad", "Power Ballad",
    "Rhythmic Soul", "Freestyle", "Duet", "Punk Rock", "Drum Solo", "A Cappella", "Euro-House", "Dance Hall",
};
#define ID3_NGENRES ((int)(sizeof ID3_GENRES / sizeof ID3_GENRES[0]))

/* A genre as text: "(17)" and "17" are Rock, "(17)Rock 'n' Roll" is its text. */
static void id3_genre(char *g, size_t cap)
{
    const char *p = g;
    int paren = *p == '(';
    if (paren) p++;
    if (!isdigit((unsigned char)*p)) return;
    int n = 0;
    while (isdigit((unsigned char)*p) && n < 1000) n = n * 10 + (*p++ - '0');
    if (paren && *p++ != ')') return;
    if (paren && *p) { memmove(g, p, strlen(p) + 1); return; }   /* its own words after the number */
    if (*p || n >= ID3_NGENRES) return;
    snprintf(g, cap, "%s", ID3_GENRES[n]);
}

/* The tag at f's position. */
static void read_id3_file(FILE *f, tags_t *t)
{
    id3_t id;
    if (id3_load(f, &id) == 0) {
        size_t pos = 0, sz;
        char fid[5], tmp[64];
        const unsigned char *b;
        while ((b = id3_next(&id, &pos, fid, &sz))) {
            if (!strcmp(fid, "TIT2")) id3_text(b, sz, t->title, sizeof t->title);
            else if (!strcmp(fid, "TPE1")) id3_text(b, sz, t->artist, sizeof t->artist);
            else if (!strcmp(fid, "TPE2")) id3_text(b, sz, t->album_artist, sizeof t->album_artist);
            else if (!strcmp(fid, "TALB")) id3_text(b, sz, t->album, sizeof t->album);
            else if (!strcmp(fid, "TCON")) { id3_text(b, sz, t->genre, sizeof t->genre); id3_genre(t->genre, sizeof t->genre); }
            else if (!strcmp(fid, "TCOM")) id3_text(b, sz, t->composer, sizeof t->composer);
            else if (!strcmp(fid, "TRCK")) { id3_text(b, sz, tmp, sizeof tmp); t->track = leading_int(tmp); }
            else if (!strcmp(fid, "TPOS")) { id3_text(b, sz, tmp, sizeof tmp); t->disc = leading_int(tmp); }
            else if (!strcmp(fid, "TYER") || !strcmp(fid, "TDRC")) { id3_text(b, sz, tmp, sizeof tmp); t->year = leading_int(tmp); }
            else if (!strcmp(fid, "TXXX") && sz > 1 && (b[0] == 0 || b[0] == 3)) {
                /* user text (Latin-1 or UTF-8): description, NUL, value */
                const unsigned char *nul = memchr(b + 1, 0, sz - 1);
                if (nul) {
                    char key[48], val[48];
                    size_t kl = (size_t)(nul - (b + 1)), vl = sz - 1 - kl - 1;
                    if (kl < sizeof key) {
                        memcpy(key, b + 1, kl);
                        key[kl] = '\0';
                        if (vl >= sizeof val) vl = sizeof val - 1;
                        memcpy(val, nul + 1, vl);
                        val[vl] = '\0';
                        rg_field(t, key, val);
                    }
                }
            }
            else if (!strcmp(fid, "APIC") || !strcmp(fid, "PIC")) t->has_picture = 1;
        }
        free(id.buf);
    }
}

static void read_id3(const char *path, tags_t *t)
{
    FILE *f = fopen(path, "rbe");
    if (!f) return;
    read_id3_file(f, t);
    fclose(f);
}

/* An ID3v1 field: Latin-1, padded with NULs or spaces. */
static void id3v1_field(const unsigned char *b, size_t n, char *out, size_t cap)
{
    size_t w = 0;
    for (size_t i = 0; i < n && b[i] && w + 3 < cap; i++) {
        if (b[i] >= 0x80) { out[w++] = (char)(0xC0 | (b[i] >> 6)); out[w++] = (char)(0x80 | (b[i] & 0x3F)); }
        else out[w++] = (char)b[i];
    }
    while (w > 0 && out[w - 1] == ' ') w--;
    out[w] = '\0';
}

/* ID3v1 (the last 128 bytes: "TAG", title, artist, album, year, comment,
 * genre; v1.1: the comment's last byte the track number), for what an ID3v2
 * tag didn't give: files with only that gave no tags. */
static void read_id3v1(const char *path, tags_t *t)
{
    FILE *f = fopen(path, "rbe");
    if (!f) return;
    unsigned char b[128];
    int got = fseeko(f, -128, SEEK_END) == 0 && fread(b, 1, sizeof b, f) == sizeof b && !memcmp(b, "TAG", 3);
    fclose(f);
    if (!got) return;
    char v[32];
    if (!t->title[0]) id3v1_field(b + 3, 30, t->title, sizeof t->title);
    if (!t->artist[0]) id3v1_field(b + 33, 30, t->artist, sizeof t->artist);
    if (!t->album[0]) id3v1_field(b + 63, 30, t->album, sizeof t->album);
    id3v1_field(b + 93, 4, v, sizeof v);
    if (!t->year) t->year = atoi(v);
    if (!t->track && b[125] == 0 && b[126] != 0) t->track = b[126];
    if (!t->genre[0] && b[127] < ID3_NGENRES) snprintf(t->genre, sizeof t->genre, "%s", ID3_GENRES[b[127]]);
}

static int g_exact_mp3 = 1;
void tags_set_exact_mp3_length(int exact) { g_exact_mp3 = exact; }

static int read_mp3(const char *path, tags_t *t)
{
    strcpy(t->codec, "MP3");
    read_id3(path, t);
    if (!t->title[0] || !t->artist[0] || !t->album[0]) read_id3v1(path, t);   /* (a seek to the end only then) */
    mpg123_handle *mh = mpg123_new(NULL, NULL);
    if (!mh) return 0;
    mpg123_param(mh, MPG123_ADD_FLAGS, MPG123_QUIET | MPG123_GAPLESS, 0);
    mpg123_param(mh, MPG123_RESYNC_LIMIT, -1, 0);
    int fd = open(path, O_RDONLY | O_CLOEXEC);          /* (opened here: close-on-exec) */
    if (fd >= 0 && mpg123_open_fd(mh, fd) == MPG123_OK) {
        long rate;
        int ch, enc;
        if (mpg123_getformat(mh, &rate, &ch, &enc) == MPG123_OK) {
            t->sample_rate = (uint32_t)rate;
            t->channels = (uint8_t)ch;
            if (g_exact_mp3) mpg123_scan(mh);   /* walk the frame headers: exact length, no decoding */
            off_t len = mpg123_length(mh);
            if (len > 0 && rate > 0) t->duration_ms = (uint32_t)((int64_t)len * 1000 / rate);
        }
        mpg123_close(mh);
    }
    mpg123_delete(mh);
    if (fd >= 0) close(fd);
    return 0;
}

/* ---------- DSD (DSF, DFF) ---------- */

/* The format's rate (2822400 for DSD64) and 1 bit, as the file has it; the
 * player makes PCM of it. DST-compressed DFF and multichannel files are left
 * out: the player can't play them. */
static int read_dsd(const char *path, tags_t *t)
{
    FILE *f = fopen(path, "rbe");
    if (!f) return -1;
    dsd_info_t in;
    const char *name;
    if (dsd_probe(f, &in) || in.dst || in.channels > 2 || !(name = dsd_name(in.rate))) {
        fclose(f);
        return -1;
    }
    snprintf(t->codec, sizeof t->codec, "%s", name);
    t->sample_rate = in.rate;
    t->bits = 1;
    t->channels = (uint8_t)in.channels;
    t->duration_ms = (uint32_t)(in.bytes * 8000 / in.rate);
    if (in.id3 && fseeko(f, (off_t)in.id3, SEEK_SET) == 0) read_id3_file(f, t);
    if (!t->title[0]) set_str(t->title, sizeof t->title, in.title, strlen(in.title));
    if (!t->artist[0]) set_str(t->artist, sizeof t->artist, in.artist, strlen(in.artist));
    fclose(f);
    return 0;
}

/* ---------- Ogg Vorbis ---------- */

#ifdef FATHOM_HAVE_VORBIS
/* One stream's packets as bytes, across segments and pages, straight from
 * the file: the comment packet is walked in place, and a cover in it
 * (hundreds of KB of base64) is skipped rather than read into memory, as for
 * FLAC. (No CRC checks: a tag reader, not a decoder.) */
typedef struct {
    FILE *f;
    uint32_t serial;
    int have_serial, bad;
    unsigned char lace[255];
    int nseg, seg;      /* the page's segments, the one being read */
    uint32_t left;      /* its bytes not yet read */
    int last;           /* it ends the packet */
} ogg_rd_t;

static int ogg_page(ogg_rd_t *r)
{
    for (;;) {
        unsigned char h[27];
        if (fread(h, 1, 27, r->f) != 27 || memcmp(h, "OggS", 4) || h[4]) return -1;
        size_t n = h[26];
        if (fread(r->lace, 1, n, r->f) != n) return -1;
        uint32_t serial = le32(h + 14);
        if (r->have_serial && serial != r->serial) {     /* another stream's (multiplexed): skipped */
            long body = 0;
            for (size_t i = 0; i < n; i++) body += r->lace[i];
            if (fseek(r->f, body, SEEK_CUR)) return -1;
            continue;
        }
        r->serial = serial;
        r->have_serial = 1;
        r->nseg = (int)n;
        r->seg = -1;
        return 0;
    }
}

/* On to the packet's next segment: 0, or -1 where the packet (or the file) ends */
static int ogg_seg(ogg_rd_t *r)
{
    if (r->last || r->bad) return -1;
    while (++r->seg >= r->nseg)
        if (ogg_page(r)) { r->bad = 1; return -1; }
    r->left = r->lace[r->seg];
    r->last = r->left < 255;
    return 0;
}

/* The packet's next n bytes into buf, or skipped (buf NULL): how many there were */
static size_t ogg_read(ogg_rd_t *r, unsigned char *buf, size_t n)
{
    size_t got = 0;
    while (got < n) {
        if (!r->left) {
            if (ogg_seg(r)) break;
            continue;
        }
        size_t k = n - got < r->left ? n - got : r->left;
        if (buf ? fread(buf + got, 1, k, r->f) != k : fseek(r->f, (long)k, SEEK_CUR) != 0) {
            r->bad = 1;
            break;
        }
        r->left -= (uint32_t)k;
        got += k;
    }
    return got;
}

/* The rest of the packet skipped: the next one is read from here */
static void ogg_next(ogg_rd_t *r)
{
    while (ogg_read(r, NULL, 1u << 20) == 1u << 20) {}
    r->last = 0;
}

/* The file's Vorbis headers up to its comments: their number, or -1 if it
 * isn't Vorbis. The sample rate and channels from the identification header. */
static long ogg_vorbis_start(FILE *f, ogg_rd_t *r, uint32_t *rate, uint8_t *channels)
{
    memset(r, 0, sizeof *r);
    r->f = f;
    unsigned char id[30], h[11], c[4];
    if (ogg_read(r, id, 30) != 30 || memcmp(id, "\x01vorbis", 7)) return -1;
    *channels = id[11];
    *rate = le32(id + 12);
    ogg_next(r);
    if (ogg_read(r, h, 11) != 11 || memcmp(h, "\x03vorbis", 7)) return -1;
    uint32_t vendor = le32(h + 7);
    if (ogg_read(r, NULL, vendor) != vendor || ogg_read(r, c, 4) != 4) return -1;
    uint32_t count = le32(c);
    return count > 0x7fffffff ? 0x7fffffff : (long)count;
}

static uint64_t le64(const unsigned char *p) { return (uint64_t)le32(p + 4) << 32 | le32(p); }

/* The granule position of the stream's last whole page: its length in
 * samples (for a file starting at 0, as encoders write them), or 0. Pages
 * are a few KB, so the last 8 KB nearly always hold one; else the most a
 * page can be. */
static uint64_t ogg_last_granule(FILE *f, uint32_t serial)
{
    static const long tries[] = {8192, 27 + 255 + 255 * 255 + 4096};
    if (fseek(f, 0, SEEK_END)) return 0;
    long size = ftell(f);
    for (int k = 0; k < 2 && size > 0; k++) {
        long n = size < tries[k] ? size : tries[k];
        unsigned char *b = malloc((size_t)n);
        if (!b) return 0;
        if (fseek(f, size - n, SEEK_SET) || fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); return 0; }
        for (long i = n - 27; i >= 0; i--) {
            if (memcmp(b + i, "OggS", 4) || b[i + 4] || le32(b + i + 14) != serial) continue;
            size_t ns = b[i + 26], body = 0, room = (size_t)(n - i);
            if (room < 27 + ns) continue;
            for (size_t j = 0; j < ns; j++) body += b[i + 27 + j];
            uint64_t g = le64(b + i + 6);
            if (room < 27 + ns + body || g == UINT64_MAX) continue;   /* cut short, or no packet ends on it */
            free(b);
            return g;
        }
        free(b);
        if (n == size) break;
    }
    return 0;
}

/* Base64, decoded as it comes (in chunks): the bytes written to out */
typedef struct { uint32_t acc; int bits; } b64_t;

static size_t b64_run(b64_t *s, const unsigned char *in, size_t n, unsigned char *out)
{
    size_t w = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned c = in[i];
        int v = c >= 'A' && c <= 'Z' ? (int)(c - 'A') : c >= 'a' && c <= 'z' ? (int)(c - 'a' + 26)
              : c >= '0' && c <= '9' ? (int)(c - '0' + 52) : c == '+' ? 62 : c == '/' ? 63 : -1;
        if (v < 0) continue;                      /* padding, line breaks */
        s->acc = s->acc << 6 | (uint32_t)v;
        s->bits += 6;
        if (s->bits >= 8) {
            s->bits -= 8;
            out[w++] = (unsigned char)(s->acc >> s->bits);
        }
    }
    return w;
}

#define OGG_TAG_MAX 4096                          /* longer comments aren't tags we show (lyrics...) */
#define OGG_PIC_KEY "METADATA_BLOCK_PICTURE="     /* base64 of a FLAC PICTURE block, as in Opus */
#define OGG_PIC_KEYLEN 23

/* A comment is a picture: its type (3 is the front cover), or -1 */
static int ogg_pic_type(const unsigned char *head, size_t n)
{
    if (n < OGG_PIC_KEYLEN + 8 || strncasecmp((const char *)head, OGG_PIC_KEY, OGG_PIC_KEYLEN)) return -1;
    unsigned char b[6];
    b64_t s = {0, 0};
    if (b64_run(&s, head + OGG_PIC_KEYLEN, 8, b) < 4) return -1;
    return (int)be32(b);
}

/* Walks the comments: into t (NULL: not wanted); *front and *first, the
 * index of the front cover and of the first picture (-1 if none); and if
 * want >= 0, that comment's picture decoded into *data (its image bytes,
 * at most max). 0, or -1 if the file isn't Vorbis. */
static int ogg_vorbis_walk(const char *path, tags_t *t, int *front, int *first, long want,
                           unsigned char **data, size_t *len, size_t max)
{
    FILE *f = fopen(path, "rbe");
    if (!f) return -1;
    ogg_rd_t r;
    uint32_t rate = 0;
    uint8_t ch = 0;
    unsigned char *buf = malloc(OGG_TAG_MAX);
    long n = buf ? ogg_vorbis_start(f, &r, &rate, &ch) : -1;
    *front = *first = -1;
    if (n < 0) { free(buf); fclose(f); return -1; }
    if (t) {
        strcpy(t->codec, "Vorbis");
        t->sample_rate = rate;
        t->channels = ch;
        t->bits = 0;
    }
    for (long i = 0; i < n && !r.bad; i++) {
        unsigned char lb[4];
        if (ogg_read(&r, lb, 4) != 4) break;
        uint32_t clen = le32(lb);
        size_t hn = clen < OGG_TAG_MAX ? clen : OGG_TAG_MAX;
        if (ogg_read(&r, buf, hn) != hn) break;
        uint32_t rest = clen - (uint32_t)hn;
        int pt = ogg_pic_type(buf, hn);
        if (pt >= 0) {
            if (*first < 0) *first = (int)i;
            if (pt == 3 && *front < 0) *front = (int)i;
            if (i == want) {                      /* this one's picture, decoded */
                size_t cap = (size_t)(clen - OGG_PIC_KEYLEN) / 4 * 3 + 3;
                /* (no bigger than the image may be, and room for its header) */
                unsigned char *p = cap <= 65536 || cap - 65536 <= max ? malloc(cap) : NULL;
                if (!p) break;
                b64_t s = {0, 0};
                size_t w = b64_run(&s, buf + OGG_PIC_KEYLEN, hn - OGG_PIC_KEYLEN, p);
                while (rest) {
                    size_t k = rest < OGG_TAG_MAX ? rest : OGG_TAG_MAX;
                    if (ogg_read(&r, buf, k) != k) break;
                    w += b64_run(&s, buf, k, p + w);
                    rest -= (uint32_t)k;
                }
                /* type, MIME type, description, width, height, depth, colours, the image */
                size_t at = 4, ml, dl, il;
                if (rest || at + 4 > w || (ml = be32(p + at)) > w - at - 4) { free(p); break; }
                at += 4 + ml;
                if (at + 4 > w || (dl = be32(p + at)) > w - at - 4) { free(p); break; }
                at += 4 + dl;
                if (at + 20 > w || (il = be32(p + at + 16)) > w - at - 20 || il > max || !il) { free(p); break; }
                memmove(p, p + at + 20, il);
                *data = p;
                *len = il;
                break;
            }
        } else if (t && !rest) {
            flac_comment(t, (const char *)buf, hn);
        }
        if (rest && ogg_read(&r, NULL, rest) != rest) break;
    }
    if (t && rate) {                              /* the length: the last page's granule position */
        uint64_t g = ogg_last_granule(f, r.serial);
        t->duration_ms = (uint32_t)(g * 1000 / rate);
    }
    free(buf);
    fclose(f);
    return 0;
}

static int read_ogg_vorbis(const char *path, tags_t *t)
{
    int front, first;
    if (ogg_vorbis_walk(path, t, &front, &first, -1, NULL, NULL, 0)) return -1;
    t->has_picture = first >= 0;
    if (t->channels < 1 || t->channels > 2 || !t->sample_rate) return -1;   /* the player can't play it */
    return 0;
}

/* The front cover, else the first picture there is */
static int picture_ogg_vorbis(const char *path, unsigned char **data, size_t *len, size_t max)
{
    int front, first;
    if (ogg_vorbis_walk(path, NULL, &front, &first, -1, NULL, NULL, 0) || first < 0) return -1;
    ogg_vorbis_walk(path, NULL, &front, &first, front >= 0 ? front : first, data, len, max);
    return *data ? 0 : -1;
}
#endif

/* ---------- FFmpeg: the other formats ---------- */

#ifdef FATHOM_HAVE_FFMPEG
/* Opens the file's container; the audio stream's index, or -1 (and fc closed)
 * when there's none we can decode. No stream probing: the headers are enough,
 * and a scan opens thousands of files from the card. */
static int ff_open(const char *path, AVFormatContext **fc)
{
    ff_quiet();
    *fc = NULL;
    if (avformat_open_input(fc, path, NULL, NULL) < 0) return -1;
    int s = av_find_best_stream(*fc, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (s < 0 || !avcodec_find_decoder((*fc)->streams[s]->codecpar->codec_id)) {
        avformat_close_input(fc);
        return -1;
    }
    return s;
}

static const char *ff_get(AVDictionary *a, AVDictionary *b, const char *key)
{
    const AVDictionaryEntry *e = av_dict_get(a, key, NULL, 0);   /* keys ignore case */
    if (!e) e = av_dict_get(b, key, NULL, 0);
    return e ? e->value : NULL;
}

static void ff_str(char *dst, size_t cap, AVDictionary *a, AVDictionary *b, const char *k1, const char *k2)
{
    const char *v = ff_get(a, b, k1);
    if (!v && k2) v = ff_get(a, b, k2);
    if (v) set_str(dst, cap, v, strlen(v));
}

static void ff_rg(tags_t *t, AVDictionary *m)
{
    const AVDictionaryEntry *e = NULL;
    while ((e = av_dict_get(m, "", e, AV_DICT_IGNORE_SUFFIX))) rg_field(t, e->key, e->value);
}

static int read_ff(const char *path, tags_t *t)
{
    AVFormatContext *fc;
    int s = ff_open(path, &fc);
    if (s < 0) return -1;
    AVStream *st = fc->streams[s];
    AVCodecParameters *par = st->codecpar;
    if (par->ch_layout.nb_channels < 1 || par->ch_layout.nb_channels > 2 || par->sample_rate <= 0) {
        avformat_close_input(&fc);
        return -1;                                   /* the player can't play it: leave it out */
    }
    /* Container tags (MP4, WAV, AIFF, APE) or the stream's (Ogg's Vorbis comments) */
    AVDictionary *a = fc->metadata, *b = st->metadata;
    ff_str(t->title, sizeof t->title, a, b, "title", NULL);
    ff_str(t->artist, sizeof t->artist, a, b, "artist", "author");
    ff_str(t->album_artist, sizeof t->album_artist, a, b, "album_artist", "album artist");
    if (!t->album_artist[0]) ff_str(t->album_artist, sizeof t->album_artist, a, b, "albumartist", NULL);
    ff_str(t->album, sizeof t->album, a, b, "album", NULL);
    ff_str(t->genre, sizeof t->genre, a, b, "genre", NULL);
    ff_str(t->composer, sizeof t->composer, a, b, "composer", NULL);
    const char *v;
    if ((v = ff_get(a, b, "date")) || (v = ff_get(a, b, "year"))) t->year = leading_int(v);
    if ((v = ff_get(a, b, "track"))) t->track = atoi(v);
    if ((v = ff_get(a, b, "disc"))) t->disc = atoi(v);
    ff_rg(t, a);
    ff_rg(t, b);
    if (ff_get(a, b, "cuesheet")) t->has_cuesheet = 1;   /* WavPack, APE: an APEv2 Cuesheet tag */

    ff_codec_name(fc, par, t->codec, sizeof t->codec);
    t->sample_rate = (uint32_t)par->sample_rate;
    t->channels = (uint8_t)par->ch_layout.nb_channels;
    t->bits = (uint8_t)ff_bits(par);
    uint64_t frames = ff_frames(fc, st);
    t->duration_ms = (uint32_t)(frames * 1000 / (uint64_t)par->sample_rate);
    for (unsigned i = 0; i < fc->nb_streams; i++)
        if (fc->streams[i]->disposition & AV_DISPOSITION_ATTACHED_PIC) t->has_picture = 1;
    avformat_close_input(&fc);
    return 0;
}

static int picture_ff(const char *path, unsigned char **data, size_t *len, size_t max)
{
    AVFormatContext *fc = NULL;
    ff_quiet();
    if (avformat_open_input(&fc, path, NULL, NULL) < 0) return -1;
    int found = -1;
    for (unsigned i = 0; i < fc->nb_streams && found; i++) {
        const AVPacket *p = &fc->streams[i]->attached_pic;
        if (!(fc->streams[i]->disposition & AV_DISPOSITION_ATTACHED_PIC) || p->size <= 0) continue;
        if ((size_t)p->size > max) continue;
        *data = malloc((size_t)p->size);
        if (!*data) break;
        memcpy(*data, p->data, (size_t)p->size);
        *len = (size_t)p->size;
        found = 0;
    }
    avformat_close_input(&fc);
    return found;
}
#endif

/* ---------- public ---------- */

int tags_read(const char *path, tags_t *t)
{
    memset(t, 0, sizeof *t);
    const char *e = ext_of(path);
    int r = -1;
    if (!strcasecmp(e, "flac")) r = read_flac(path, t);
    else if (!strcasecmp(e, "mp3")) r = read_mp3(path, t);
    else if (dsd_path(path)) r = read_dsd(path, t);
#ifdef FATHOM_HAVE_VORBIS
    else if (ogg_ext(e) && vorbis_file_is_vorbis(path)) r = read_ogg_vorbis(path, t);
#endif
#ifdef FATHOM_HAVE_FFMPEG
    else if (ff_ext(e)) r = read_ff(path, t);
#endif
    return tags_finish(path, t, r);
}

static int tags_finish(const char *path, tags_t *t, int r)
{
    if (r == 0 && !t->title[0]) {   /* fall back to the file name */
        const char *slash = strrchr(path, '/');
        const char *name = slash ? slash + 1 : path;
        const char *dot = strrchr(name, '.');
        set_str(t->title, sizeof t->title, name, dot ? (size_t)(dot - name) : strlen(name));
    }
    return r;
}

int tags_cuesheet(const char *path, char **text, size_t *len)
{
    *text = NULL;
    *len = 0;
    const char *e = ext_of(path);
    tags_t t;
    memset(&t, 0, sizeof t);
    if (!strcasecmp(e, "flac")) read_flac_cue(path, &t, text, len);
#ifdef FATHOM_HAVE_FFMPEG
    else if (ff_ext(e)) {
        AVFormatContext *fc;
        int s = ff_open(path, &fc);
        if (s < 0) return -1;
        const char *v = ff_get(fc->metadata, fc->streams[s]->metadata, "cuesheet");
        size_t n = v ? strlen(v) : 0;
        if (n && n < CUE_MAX_BYTES && (*text = malloc(n + 1))) {
            memcpy(*text, v, n + 1);
            *len = n;
        }
        avformat_close_input(&fc);
    }
#endif
    return *text ? 0 : -1;
}

static size_t g_max_picture = (size_t)-1;

/* A FLAC's PICTURE blocks, read for their type and where their bytes are:
 * the front cover (3), else "other" (0, what some taggers write for any
 * cover), else the first of any other kind; never a file icon (1, 2). */
int tags_picture_at(const char *path, long *off, long *len)
{
    const char *sfx = cue_part_suffix(path);
    if (sfx) {
        char file[1024];
        if ((size_t)(sfx - path) >= sizeof file) return -1;
        memcpy(file, path, (size_t)(sfx - path));
        file[sfx - path] = '\0';
        return tags_picture_at(file, off, len);
    }
    if (strcasecmp(ext_of(path), "flac")) return -1;
    FILE *f = fopen(path, "rbe");
    if (!f) return -1;
    unsigned char h[10];
    int best = 99;
    if (fread(h, 1, 4, f) != 4) goto out;
    if (!memcmp(h, "ID3", 3)) {
        if (fread(h + 4, 1, 6, f) != 6) goto out;
        long skip = 10 + ((long)(h[6] & 0x7f) << 21 | (long)(h[7] & 0x7f) << 14 | (long)(h[8] & 0x7f) << 7 | (h[9] & 0x7f));
        if (h[5] & 0x10) skip += 10;
        if (fseek(f, skip, SEEK_SET) || fread(h, 1, 4, f) != 4) goto out;
    }
    if (memcmp(h, "fLaC", 4)) goto out;
    for (int last = 0; !last;) {
        unsigned char bh[4];
        if (fread(bh, 1, 4, f) != 4) break;
        last = bh[0] & 0x80;
        int type = bh[0] & 0x7f;
        uint32_t blen = be24(bh + 1);
        long next = ftell(f) + (long)blen;
        if (type == 6 && blen >= 32) {
            unsigned char b[8];
            if (fread(b, 1, 8, f) != 8) break;          /* its type, the MIME type's length */
            uint32_t pt = be32(b), ml = be32(b + 4);
            int rank = pt == 3 ? 0 : pt == 0 ? 1 : (pt == 1 || pt == 2) ? 99 : 2;
            if (rank < best && ml < blen && !fseek(f, (long)ml, SEEK_CUR) && fread(b, 1, 4, f) == 4) {
                uint32_t dl = be32(b);                   /* the description's length, then 16 bytes of sizes */
                unsigned char d[4];
                if (dl < blen && !fseek(f, (long)dl + 16, SEEK_CUR) && fread(d, 1, 4, f) == 4) {
                    uint32_t n = be32(d);
                    long at = ftell(f);
                    if (n && at + (long)n <= next) { best = rank; *off = at; *len = (long)n; }
                }
            }
        } else if (type == 127) {
            break;
        }
        if (best == 0 || fseek(f, next, SEEK_SET)) break;
    }
out:
    fclose(f);
    return best < 99 ? 0 : -1;
}
void tags_set_max_picture(size_t bytes) { g_max_picture = bytes; }

int tags_picture(const char *path, unsigned char **data, size_t *len)
{
    *data = NULL;
    *len = 0;
    const char *sfx = cue_part_suffix(path);
    if (sfx) {                                   /* a CUE track: its file's cover */
        char file[1024];
        if ((size_t)(sfx - path) >= sizeof file) return -1;
        memcpy(file, path, (size_t)(sfx - path));
        file[sfx - path] = '\0';
        return tags_picture(file, data, len);
    }
    const char *e = ext_of(path);
    if (!strcasecmp(e, "flac")) {
        long off, n;
        if (tags_picture_at(path, &off, &n) || (size_t)n > g_max_picture) return -1;
        FILE *f = fopen(path, "rbe");
        if (!f) return -1;
        *data = malloc((size_t)n ? (size_t)n : 1);
        if (!*data || fseek(f, off, SEEK_SET) || fread(*data, 1, (size_t)n, f) != (size_t)n) {
            free(*data);
            *data = NULL;
        }
        fclose(f);
        *len = *data ? (size_t)n : 0;
        return *data ? 0 : -1;
    }
    if (!strcasecmp(e, "mp3") || dsd_path(path)) {
        FILE *f = fopen(path, "rbe");
        if (!f) return -1;
        dsd_info_t in;
        if (dsd_path(path) && (dsd_probe(f, &in) || !in.id3 || fseeko(f, (off_t)in.id3, SEEK_SET))) {
            fclose(f);
            return -1;                       /* DSF, DFF: their ID3 tag, where it is */
        }
        id3_t id;
        int found = -1;
        if (id3_load(f, &id) == 0) {
            size_t pos = 0, sz;
            char fid[5];
            const unsigned char *b;
            while ((b = id3_next(&id, &pos, fid, &sz))) {
                int pic = !strcmp(fid, "PIC");
                if ((strcmp(fid, "APIC") && !pic) || sz < 6) continue;
                /* enc, mime\0, type, description\0 (encoding-dependent), data;
                 * v2.2's PIC: enc, a 3-letter format ("JPG"), type, ... */
                int enc = b[0];
                size_t i = 1;
                if (pic) i = 5;
                else {
                    while (i < sz && b[i]) i++;
                    i += 2;  /* NUL + picture type */
                }
                if (enc == 1 || enc == 2) { while (i + 1 < sz && (b[i] || b[i + 1])) i += 2; i += 2; }
                else { while (i < sz && b[i]) i++; i++; }
                if (i >= sz || sz - i > g_max_picture) continue;
                *len = sz - i;
                *data = malloc(*len);
                if (*data) { memcpy(*data, b + i, *len); found = 0; }
                break;
            }
            free(id.buf);
        }
        fclose(f);
        return found;
    }
#ifdef FATHOM_HAVE_VORBIS
    if (ogg_ext(e) && vorbis_file_is_vorbis(path)) return picture_ogg_vorbis(path, data, len, g_max_picture);
#endif
#ifdef FATHOM_HAVE_FFMPEG
    if (ff_ext(e)) return picture_ff(path, data, len, g_max_picture);
#endif
    return -1;
}
