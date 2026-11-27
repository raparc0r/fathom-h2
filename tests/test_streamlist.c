/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* A station's playlist (.m3u, .pls, HLS) to its stream's URL (app/audio/streamlist.c). */
#include <stdio.h>
#include <string.h>

#include "audio/streamlist.h"
#include "test_util.h"

static int first(const char *text, const char *base, char *out)
{
    return streamlist_first(text, strlen(text), base, out, 512);
}

static int hls(const char *text, const char *base, char *out)
{
    return streamlist_hls_pick(text, strlen(text), base, out, 512);
}

int main(void)
{
    char u[512];

    /* which links are playlists */
    CHECK(streamlist_looks_like("http://radio.example/listen.pls"));
    CHECK(streamlist_looks_like("https://radio.example/a/live.M3U?token=1"));
    CHECK(streamlist_looks_like("https://radio.example/live.m3u#x"));
    CHECK(streamlist_looks_like("https://radio.example/live.m3u8"));      /* HLS, or a plain list */
    CHECK(streamlist_looks_like("https://radio.example/hls/master.M3U8?sig=abc"));
    CHECK(!streamlist_looks_like("https://radio.example/live.m3u8.aac"));
    CHECK(!streamlist_looks_like("https://radio.example/stream.mp3"));
    CHECK(!streamlist_looks_like("https://radio.example/listen.pls.mp3"));
    CHECK(!streamlist_looks_like("/mnt/sd/Music/list.m3u"));

    /* PLS: the lowest File number, whatever the order */
    CHECK(first("[playlist]\nNumberOfEntries=2\nFile2=http://b.example/2\nTitle2=Two\n"
                "File1=http://a.example:8000/stream\nTitle1=One\nVersion=2\n", NULL, u) == STREAMLIST_FOUND);
    CHECK(!strcmp(u, "http://a.example:8000/stream"));
    CHECK(first("[Playlist]\r\nFile1= https://a.example/x \r\n", NULL, u) == STREAMLIST_FOUND && !strcmp(u, "https://a.example/x"));
    CHECK(first("[playlist]\nNumberOfEntries=0\n", NULL, u) == STREAMLIST_NONE);

    /* M3U: the first entry, past comments, CRLF and a BOM */
    CHECK(first("#EXTM3U\n#EXTINF:-1,Some Radio\nhttps://s.example/live.aac\nhttps://s.example/backup.aac\n", NULL, u)
          == STREAMLIST_FOUND && !strcmp(u, "https://s.example/live.aac"));
    CHECK(first("\xEF\xBB\xBF#EXTM3U\r\n\r\nhttp://s.example/x.mp3\r\n", NULL, u) == STREAMLIST_FOUND &&
          !strcmp(u, "http://s.example/x.mp3"));
    CHECK(first("http://plain.example/stream\n", NULL, u) == STREAMLIST_FOUND && !strcmp(u, "http://plain.example/stream"));

    /* relative entries, against the playlist's own URL */
    CHECK(first("stream.aac\n", "https://h.example/dir/list.m3u?x=1", u) == STREAMLIST_FOUND &&
          !strcmp(u, "https://h.example/dir/stream.aac"));
    CHECK(first("/live\n", "https://h.example:8443/a/b.pls", u) == STREAMLIST_FOUND && !strcmp(u, "https://h.example:8443/live"));
    CHECK(first("//cdn.example/x.mp3\n", "https://h.example/a.m3u", u) == STREAMLIST_FOUND && !strcmp(u, "https://cdn.example/x.mp3"));
    CHECK(first("x.mp3\n", "http://h.example", u) == STREAMLIST_FOUND && !strcmp(u, "http://h.example/x.mp3"));
    CHECK(first("x.mp3\n", NULL, u) == STREAMLIST_NONE);                 /* relative with nothing to resolve it */
    CHECK(first("rtsp://h.example/x\n", "http://h.example/a.m3u", u) == STREAMLIST_NONE);   /* not http(s) */

    /* HLS: segments, not a stream URL */
    CHECK(first("#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-STREAM-INF:BANDWIDTH=128000\nhi/index.m3u8\n", NULL, u) == STREAMLIST_HLS);

    /* a stream's bytes, or nothing */
    static const char mp3[] = "\xFF\xFB\x90\x64\x00\x00\x00\x00http://x";
    CHECK(streamlist_first(mp3, sizeof mp3 - 1, NULL, u, sizeof u) == STREAMLIST_NONE);
    CHECK(first("", NULL, u) == STREAMLIST_NONE);
    CHECK(first("#EXTM3U\n# only notes\n", NULL, u) == STREAMLIST_NONE);

    /* HLS master playlists: the best audio-only variant */
    static const char master[] =
        "#EXTM3U\n#EXT-X-VERSION:3\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=341000,CODECS=\"mp4a.40.2\"\nhi/320.m3u8\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=53000,CODECS=\"mp4a.40.5\"\nhi/48.m3u8\n"
        "#EXT-X-STREAM-INF:CODECS=\"mp4a.40.2\",BANDWIDTH=101000\nhi/96.m3u8\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=135000,CODECS=\"mp4a.40.2\"\nhi/128.m3u8\n";
    CHECK(hls(master, "https://h.example/radio/master.m3u8?t=1", u) == STREAMLIST_FOUND &&
          !strcmp(u, "https://h.example/radio/hi/320.m3u8"));
    CHECK(first(master, NULL, u) == STREAMLIST_HLS);           /* (streamlist_first only says it's HLS) */
    /* CRLF, a blank line before the URI */
    CHECK(hls("#EXTM3U\r\n#EXT-X-STREAM-INF:BANDWIDTH=48000\r\nhttps://cdn.example/48.m3u8\r\n"
              "#EXT-X-STREAM-INF:BANDWIDTH=64000\r\n\r\nhttps://cdn.example/64.m3u8\r\n", NULL, u) == STREAMLIST_FOUND &&
          !strcmp(u, "https://cdn.example/64.m3u8"));
    /* codecs the H2 doesn't decode are passed over, however good */
    CHECK(hls("#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=640000,CODECS=\"ec-3\"\n/ec3.m3u8\n"
              "#EXT-X-STREAM-INF:BANDWIDTH=70000,CODECS=\"mp4a.40.2\"\n/aac.m3u8\n", "http://h.example/a/m.m3u8", u) ==
          STREAMLIST_FOUND && !strcmp(u, "http://h.example/aac.m3u8"));
    /* video variants with an audio rendition of their own: the default rendition */
    CHECK(hls("#EXTM3U\n"
              "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"aud\",NAME=\"English\",URI=\"audio/en.m3u8\"\n"
              "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"aud\",NAME=\"Main\",DEFAULT=YES,URI=\"audio/main.m3u8\"\n"
              "#EXT-X-STREAM-INF:BANDWIDTH=800000,RESOLUTION=640x360,CODECS=\"avc1.4d401e,mp4a.40.2\",AUDIO=\"aud\"\n"
              "v/360.m3u8\n", "https://tv.example/live/index.m3u8", u) == STREAMLIST_FOUND &&
          !strcmp(u, "https://tv.example/live/audio/main.m3u8"));
    /* video only, audio muxed in: the cheapest */
    CHECK(hls("#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=2000000,CODECS=\"avc1.64001f,mp4a.40.2\"\nv/720.m3u8\n"
              "#EXT-X-STREAM-INF:BANDWIDTH=600000,CODECS=\"avc1.42e00a,mp4a.40.2\"\nv/240.m3u8\n",
              "https://tv.example/x/", u) == STREAMLIST_FOUND && !strcmp(u, "https://tv.example/x/v/240.m3u8"));
    /* nothing we can play */
    CHECK(hls("#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=640000,CODECS=\"ac-3\"\nac3.m3u8\n", "https://h.example/", u) ==
          STREAMLIST_NONE);
    /* a media playlist: played as it is */
    static const char segs[] = "#EXTM3U\n#EXT-X-TARGETDURATION:10\n#EXT-X-MEDIA-SEQUENCE:4021\n"
                               "#EXTINF:10.0,\nseg4021.aac\n#EXTINF:10.0,\nseg4022.aac\n";
    CHECK(hls(segs, "https://h.example/live/96.m3u8", u) == STREAMLIST_HLS);
    CHECK(first(segs, NULL, u) == STREAMLIST_HLS);
    CHECK(hls("#EXTM3U\n# notes\n", NULL, u) == STREAMLIST_NONE);

    /* too long for out */
    char small[16];
    CHECK(streamlist_first("http://a.example/a-long-path\n", 28, NULL, small, sizeof small) == STREAMLIST_NONE);

    return t_result();
}
