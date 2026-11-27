/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/*
 * A small coverage-guided fuzzer for Fathom's parsers (macOS's clang has no
 * libFuzzer): the libraries are built with -fsanitize-coverage=trace-pc-guard
 * and ASan/UBSan (tools/fuzz/build.sh); this program keeps every input that
 * reaches a new edge and mutates the kept ones.
 *
 *   fuzz TARGET SECONDS SEED... -> crash-TARGET-PID-TIME (the input) on a crash
 *   fuzz TARGET -run FILE...    -> run inputs once (to reproduce)
 *
 * TARGET: json feed opml search chapters dates cue m3u theme stream rfmeta rdir
 * cover mp3 flac ogg opus dsf dff art zip settings lang wifitxt batlog radio
 * release clock keys upt. Seeds: the files in
 * tests/data and tests/fixtures, and a few music files' first 64-400 KB.
 * FUZZ_TMP: where the file targets write their input (default /tmp);
 * FUZZ_CORPUS: a folder each new input is kept in, to seed the next run.
 * Run with ASAN_OPTIONS=detect_leaks=0 (macOS has no leak checker).
 * Found on 2026-11-21: a PNG of 0-bit samples (8 % 0), nonsense podcast
 * lengths and chapter times (overflow).
 */
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "audio/decoder.h"
#include "audio/dsd.h"
#include "audio/streamlist.h"
#include "core/batlog.h"
#include "core/lang.h"
#include "core/settings.h"
#include "core/tz.h"
#include "core/wifitxt.h"
#include "library/keys.h"
#include "library/artdec.h"
#include "library/cue.h"
#include "library/playlist.h"
#include "library/tags.h"
#include "library/scripts.h"
#include "net/coverfind.h"
#include "library/lyrics.h"
#include "net/lyricfind.h"
#include "net/json.h"
#include "net/netclock.h"
#include "net/podcast.h"
#include "net/radiodir.h"
#include "net/unzip.h"
#include "net/update.h"
#include "player/radio.h"
#include "player/rfmeta.h"
#include "sys/hibyupt.h"
#include "ui/glyphpack.h"
#include "ui/theme.h"

/* ---------- coverage ---------- */

static uint8_t *g_hit, *g_seen;
static uint32_t g_n_guards;

void __sanitizer_cov_trace_pc_guard_init(uint32_t *start, uint32_t *stop);
void __sanitizer_cov_trace_pc_guard(uint32_t *guard);
void __sanitizer_set_death_callback(void (*cb)(void));

void __sanitizer_cov_trace_pc_guard_init(uint32_t *start, uint32_t *stop)
{
    if (start == stop || *start) return;
    for (uint32_t *g = start; g < stop; g++) *g = ++g_n_guards;
}

void __sanitizer_cov_trace_pc_guard(uint32_t *guard)
{
    if (g_hit) g_hit[*guard] = 1;
}

/* ---------- the targets ---------- */

static char g_tmp[512];   /* a file for the targets that read one */

static const char *as_file(const uint8_t *d, size_t n, const char *ext)
{
    snprintf(g_tmp, sizeof g_tmp, "%s/fuzz-in.%s", getenv("FUZZ_TMP") ? getenv("FUZZ_TMP") : "/tmp", ext);
    FILE *f = fopen(g_tmp, "wb");
    if (!f) { perror(g_tmp); exit(2); }
    fwrite(d, 1, n, f);
    fclose(f);
    return g_tmp;
}

static void json_walk(json_t v, int depth)
{
    if (depth > 64) return;
    char s[64];
    json_string(v, s, sizeof s);
    json_int(v, 0);
    json_bool(v, 0);
    size_t at = 0;
    json_t c;
    if (json_type(v) == JSON_ARRAY || json_type(v) == JSON_OBJECT)
        while (json_next(v, &at, &c)) json_walk(c, depth + 1);
    json_member(v, "name", &c);
}

static void t_json(const char *s, size_t n) { json_walk(json_root(s, n), 0); }

static void t_feed(const char *s, size_t n)
{
    static pod_show_t show;
    static pod_episode_t eps[POD_MAX_EPISODES];
    memset(&show, 0, sizeof show);
    pod_parse_feed(s, n, &show, eps, POD_MAX_EPISODES);
}

static void t_opml(const char *s, size_t n)
{
    static pod_show_t out[POD_MAX_SHOWS];
    pod_parse_opml(s, n, out, POD_MAX_SHOWS);
}

static void t_search(const char *s, size_t n)
{
    static pod_show_t out[POD_SEARCH_MAX];
    pod_parse_search(s, n, out, POD_SEARCH_MAX);
}

static void t_chapters(const char *s, size_t n)
{
    static pod_chapter_t out[64];
    pod_parse_chapters(s, n, out, 64);
}

static void t_dates(const char *s, size_t n)
{
    (void)n;
    pod_parse_date(s);
    pod_parse_duration(s);
}

static void t_cue(const char *s, size_t n)
{
    static cue_sheet_t sh;
    cue_parse(s, n, &sh);
    size_t ol;
    free(cue_to_utf8(s, n, &ol));
    char file[256];
    uint32_t a, b;
    cue_part_parse(s, file, sizeof file, &a, &b);
}

static void t_m3u(const char *s, size_t n)
{
    m3u_t m;
    memset(&m, 0, sizeof m);
    if (m3u_parse(s, n, "/mnt/sd/Music/Some Folder", "fallback", &m) == 0) m3u_free(&m);
    char k[256];
    path_loose_key(s, 2, k, sizeof k);
    text_loose_key(s, k, sizeof k);
    text_search_fold(s, k, sizeof k);
    char p[1024];
    snprintf(p, sizeof p, "%s", s);
    path_normalize(p);
}

static void t_theme(const char *s, size_t n)
{
    (void)n;
    theme_t t;
    char err[256], out[8192];
    if (theme_parse(s, &t, err, sizeof err) == 0) {
        theme_validate(&t, err, sizeof err);
        theme_format(&t, out, sizeof out);
    }
}

static void t_stream(const char *s, size_t n)
{
    char out[1024];
    streamlist_first(s, n, "http://example.com/a/b.m3u", out, sizeof out);
    streamlist_hls_pick(s, n, "https://example.com/live/master.m3u8", out, sizeof out);
}

static void t_rfmeta(const char *s, size_t n)
{
    (void)n;
    char a[128], t[128], v[256];
    rf_parse(s, a, sizeof a, t, sizeof t, v, sizeof v);
}

static void t_rdir(const char *s, size_t n)
{
    static radio_station_t st[200];
    static rdir_meta_t meta[200];
    static rdir_country_t c[300];
    rdir_parse_stations(s, n, st, meta, 200);
    rdir_parse_countries(s, n, c, 300);
}

static void t_cover(const char *s, size_t n)
{
    cf_album_t a;
    memset(&a, 0, sizeof a);
    snprintf(a.title, sizeof a.title, "OK Computer");
    snprintf(a.artist, sizeof a.artist, "Radiohead");
    a.year = 1997;
    static cf_match_t m[16];
    int k = cf_parse_mb(s, n, &a, m, 16);
    if (k > 0) cf_pick(&a, m, k);
    k = cf_parse_apple(s, n, &a, m, 16);
    if (k > 0) cf_pick(&a, m, k);
    char norm[256];
    cf_norm(s, 1, norm, sizeof norm);
    cf_norm(s, 0, norm, sizeof norm);
}

/* Lyrics: a text's lines and times (an .lrc, a tag's, LRCLIB's), and LRCLIB's answer */
static void t_lyrics(const uint8_t *d, size_t n)
{
    lyrics_t l;
    lyrics_looks_synced((const char *)d, n);
    if (lyrics_parse((const char *)d, n, &l)) return;
    for (int i = 0; i < l.n; i++) {
        if (strlen(lyrics_line(&l, i)) > 2 * n) abort();          /* (Latin-1 read as UTF-8: twice at most) */
        if (l.synced && i && l.line[i].ms < l.line[i - 1].ms) abort();
        if (l.synced && l.line[i].ms < 0) abort();
    }
    static const int32_t at[] = {-5, 0, 1, 999, 60000, 3600000, INT32_MAX};
    for (size_t k = 0; k < sizeof at / sizeof at[0]; k++) {
        int i = lyrics_at(&l, at[k]);
        if (i < -1 || i >= l.n) abort();
    }
    lyrics_free(&l);
}

static void t_lyricfind(const char *s, size_t n)
{
    static const int dur[] = {0, 1, 180, 100000};
    for (size_t k = 0; k < sizeof dur / sizeof dur[0]; k++) {
        char *text = NULL;
        size_t tl = 0;
        int r = lf_parse(s, n, dur[k], &text, &tl);
        if ((r == 1) != (text != NULL)) abort();
        if (text) {
            lyrics_t l;
            if (strlen(text) != tl) abort();
            if (lyrics_parse(text, tl, &l) == 0) lyrics_free(&l);
            free(text);
        }
    }
}

static void t_tags(const uint8_t *d, size_t n, const char *ext)
{
    tags_t t;
    tags_read(as_file(d, n, ext), &t);
    char *cue = NULL;
    size_t cl;
    if (tags_cuesheet(g_tmp, &cue, &cl) == 0) free(cue);
    unsigned char *pic = NULL;
    size_t pl;
    if (tags_picture(g_tmp, &pic, &pl) == 0) free(pic);
    long off, len;
    tags_picture_at(g_tmp, &off, &len);
    char *ly = NULL;
    size_t ll;
    if (tags_lyrics(g_tmp, &ly, &ll) == 0) {
        lyrics_t l;
        if (lyrics_parse(ly, ll, &l) == 0) lyrics_free(&l);
        free(ly);
    }
    pod_chapter_t ch[32];
    pod_file_chapters(g_tmp, ch, 32);
}

static void t_art(const uint8_t *d, size_t n)
{
    art_limits_t lim = {4 << 20, 4LL << 20, 4 << 20, 0};
    unsigned char *rgb = NULL;
    int w, h;
    if (art_decode_mem(d, n, 120, &lim, &rgb, &w, &h) == 0) free(rgb);
}

static void t_dsd(const uint8_t *d, size_t n, const char *ext)
{
    audio_format_t fmt;
    dsd_dec_t *x = dsd_open(as_file(d, n, ext), 0, &fmt);
    if (!x) return;
    static int32_t out[4096 * 8];
    for (int i = 0; i < 8; i++) if (dsd_read(x, out, 512) <= 0) break;
    dsd_seek(x, 1000);
    dsd_read(x, out, 512);
    dsd_close(x);
}

static int zsink(const void *data, size_t len, void *ctx) { (void)data; (void)len; (void)ctx; return 0; }

static void t_zip(const uint8_t *d, size_t n)
{
    zip_t z;
    if (zip_open(&z, as_file(d, n, "zip"))) return;
    zip_entry_t e;
    for (int i = 0; i < 16 && zip_next(&z, &e) > 0; i++)
        if (e.usize < (1u << 20)) zip_extract(&z, &e, zsink, NULL, NULL);
    zip_close(&z);
}

static void t_settings(const uint8_t *d, size_t n)
{
    settings_load(as_file(d, n, "ini"));
    settings_get("a.b", "x");
    settings_get_int("volume", 3);
}

static void t_lang(const uint8_t *d, size_t n)
{
    lang_load(as_file(d, n, "lang"));
    tr("Settings");
    char out[128];
    tr_fill(out, sizeof out, "%s tracks", "12", NULL);
}

static void t_wifitxt(const char *s, size_t n)
{
    wifitxt_net_t nets[WIFITXT_MAX];
    int bad = 0;
    int k = wifitxt_parse(s, n, nets, WIFITXT_MAX, &bad);
    for (int i = 0; i < k; i++)                  /* (each a string that ends inside its array) */
        if (strlen(nets[i].ssid) > 32 || strlen(nets[i].pass) > 63) abort();
}

static void t_batlog(const uint8_t *d, size_t n)
{
    /* the file as a cut or a bad card left it, read back and reported both ways */
    batrec_t *r = NULL;
    int k = batlog_load(as_file(d, n, "bat"), NULL, 1000, &r);
    if (k > 0) {
        static batrep_t rep;
        batlog_report(r, k, 1, &rep);
        batlog_report(r, k, 7, &rep);
    }
    free(r);
    /* ...and taken on by a start: trimmed, noted, written */
    static batlog_t b;
    batlog_open(&b, g_tmp, 1795000000, 5);
    batlog_note(&b, 20, 80, 0);
    batlog_clock_set(&b, 1795003600, 30);
    batlog_off(&b, 40);
}

static void t_radio(const char *s, size_t n)
{
    (void)n;
    static radio_station_t st[64];
    int k = radio_parse(s, st, 64);
    char line[1024], a[128], t[128];
    for (int i = 0; i < k; i++) radio_line(&st[i], line, sizeof line);
    radio_split_meta(s, a, sizeof a, t, sizeof t);
    radio_icon_ok(s);
}

static void t_release(const char *s, size_t n)
{
    update_release_t r;
    update_parse_release(s, n, &r);
    int v[3];
    update_parse_version(s, v);
    update_is_newer(s, "0.21.0");
    update_is_newer("v0.21.0", s);
}

static void t_clock(const char *s, size_t n)
{
    netclock_parse_date(s, n);
    tz_find(s);          /* (not tz_rule_offset: only the built-in rules reach it; a ten-digit hour overflows its int) */
}

static void t_keys(const char *s, size_t n)
{
    char out[256], names[KEY_CREDIT_MAX][KEY_CREDIT_LEN];
    key_artist(s, "", out, sizeof out);
    key_artist("", s, out, sizeof out);
    key_artist(s, s, out, 8);
    key_credits(s, s + n / 2, names);
    key_is_various(s);
}

/* A character pack from outside (a download, a copy from a computer): opened,
 * and every character it says it has looked up and its cells read end to end
 * (a number or a place the checks let through would be read off the mapping) */
static void t_glyphpack(const uint8_t *d, size_t n)
{
    glyphpack_t *p = glyphpack_open(as_file(d, n, "bin"));
    if (!p) return;
    int px[GLYPHPACK_MAX_SIZES], ns = glyphpack_sizes(p, px), count = glyphpack_count(p);
    volatile unsigned sink = 0;
    for (uint32_t cp = 0; cp < 0x10400; cp++) {
        int half = 0, num = glyphpack_find(p, cp, &half);
        if (num < 0) continue;
        if (num >= count) abort();
        for (int s = 0; s < ns; s++) {
            int row = 0;
            const uint8_t *c = glyphpack_cell(p, s, num, &row);
            if (!c || row != (px[s] + 1) / 2) abort();
            sink += c[0] + c[(size_t)row * (size_t)px[s] - 1];
            if (glyphpack_foot(p, s) > 0) abort();
        }
    }
    for (int want = 0; want < 80; want++) {
        int s = glyphpack_size_for(p, want);
        if (s < 0 || s >= ns) abort();
    }
    glyphpack_close(p);
    (void)sink;
}

/* A name from a tag, any bytes: which writing it is in, alone and into a library's count */
static void t_scripts(const char *s, size_t n)
{
    lib_scripts_t c;
    int w[WANT_N];
    memset(&c, 0, sizeof c);
    unsigned has = script_of(s);
    if (has & ~(unsigned)(SCRIPT_KANA | SCRIPT_HANGUL | SCRIPT_HAN | SCRIPT_SIMPLIFIED | SCRIPT_TRADITIONAL)) abort();
    scripts_add(&c, s, s + n / 2, s + n / 3);
    scripts_add(&c, s + n / 4, NULL, s);
    scripts_want(&c, w);
    for (int i = 0; i < WANT_N; i++)
        if (w[i] < 0 || w[i] > 2) abort();
    if (c.simplified_chars > 1060 || c.traditional_chars > 1389) abort();
}

static void t_upt(const uint8_t *d, size_t n)
{
    static upt_t u;
    if (upt_open(&u, as_file(d, n, "upt"), 0) == 0) {
        upt_file(&u, "update.txt");
        upt_check(&u, NULL, NULL);
    }
    upt_close(&u);
}

static const char *g_target;

static void run_one(const uint8_t *d, size_t n)
{
    char *s = malloc(n + 1);   /* as the callers give them: NUL after the end */
    memcpy(s, d, n);
    s[n] = 0;
    const char *t = g_target;
    if (!strcmp(t, "json")) t_json(s, n);
    else if (!strcmp(t, "feed")) t_feed(s, n);
    else if (!strcmp(t, "opml")) t_opml(s, n);
    else if (!strcmp(t, "search")) t_search(s, n);
    else if (!strcmp(t, "chapters")) t_chapters(s, n);
    else if (!strcmp(t, "dates")) t_dates(s, n);
    else if (!strcmp(t, "cue")) t_cue(s, n);
    else if (!strcmp(t, "m3u")) t_m3u(s, n);
    else if (!strcmp(t, "theme")) t_theme(s, n);
    else if (!strcmp(t, "stream")) t_stream(s, n);
    else if (!strcmp(t, "rfmeta")) t_rfmeta(s, n);
    else if (!strcmp(t, "rdir")) t_rdir(s, n);
    else if (!strcmp(t, "cover")) t_cover(s, n);
    else if (!strcmp(t, "mp3")) t_tags(d, n, "mp3");
    else if (!strcmp(t, "flac")) t_tags(d, n, "flac");
    else if (!strcmp(t, "ogg")) t_tags(d, n, "ogg");
    else if (!strcmp(t, "opus")) t_tags(d, n, "opus");
    else if (!strcmp(t, "dsf")) { t_tags(d, n, "dsf"); t_dsd(d, n, "dsf"); }
    else if (!strcmp(t, "dff")) { t_tags(d, n, "dff"); t_dsd(d, n, "dff"); }
    else if (!strcmp(t, "art")) t_art(d, n);
    else if (!strcmp(t, "zip")) t_zip(d, n);
    else if (!strcmp(t, "settings")) t_settings(d, n);
    else if (!strcmp(t, "lang")) t_lang(d, n);
    else if (!strcmp(t, "wifitxt")) t_wifitxt(s, n);
    else if (!strcmp(t, "batlog")) t_batlog(d, n);
    else if (!strcmp(t, "radio")) t_radio(s, n);
    else if (!strcmp(t, "release")) t_release(s, n);
    else if (!strcmp(t, "clock")) t_clock(s, n);
    else if (!strcmp(t, "keys")) t_keys(s, n);
    else if (!strcmp(t, "upt")) t_upt(d, n);
    else if (!strcmp(t, "glyphpack")) t_glyphpack(d, n);
    else if (!strcmp(t, "scripts")) t_scripts(s, n);
    else if (!strcmp(t, "lyrics")) t_lyrics(d, n);
    else if (!strcmp(t, "lyricfind")) t_lyricfind(s, n);
    else { fprintf(stderr, "unknown target %s\n", t); exit(2); }
    free(s);
}

/* ---------- the corpus and mutations ---------- */

typedef struct { uint8_t *d; size_t n; } input_t;
static input_t *g_corp;
static int g_nc, g_capc;
static const uint8_t *g_cur;
static size_t g_cur_n;

static void dump_current(void)
{
    char p[256];
    snprintf(p, sizeof p, "crash-%s-%d-%ld", g_target, getpid(), (long)time(NULL));
    FILE *f = fopen(p, "wb");
    if (f) { fwrite(g_cur, 1, g_cur_n, f); fclose(f); fprintf(stderr, "fuzz: input saved to %s\n", p); }
}

static void on_alarm(int sig)
{
    (void)sig;
    fprintf(stderr, "fuzz: timeout (an input took over 10 s)\n");
    dump_current();
    _exit(3);
}

static void add(const uint8_t *d, size_t n)
{
    if (g_nc == g_capc) { g_capc = g_capc ? g_capc * 2 : 64; g_corp = realloc(g_corp, (size_t)g_capc * sizeof *g_corp); }
    g_corp[g_nc].d = malloc(n ? n : 1);
    memcpy(g_corp[g_nc].d, d, n);
    g_corp[g_nc++].n = n;
}

static uint64_t g_r = 88172645463325252ull;
static uint64_t rnd(void) { g_r ^= g_r << 13; g_r ^= g_r >> 7; g_r ^= g_r << 17; return g_r; }
static size_t rn(size_t n) { return n ? (size_t)(rnd() % n) : 0; }

static const char *const DICT[] = {
    "<", ">", "</", "/>", "<![CDATA[", "]]>", "&amp;", "&#", "&#x", ";", "&lt;", "=\"", "\"", "'",
    "<item>", "</item>", "<entry>", "<enclosure url=\"", "<title>", "</title>", "<itunes:duration>",
    "<pubDate>", "<guid>", "<podcast:chapters url=\"", "<outline xmlUrl=\"", "<link href=\"", "<channel>",
    "{", "}", "[", "]", ":", ",", "\"\\u", "\\\"", "null", "true", "-", "1e999", "99999999999999999999",
    "\"startTime\":", "\"title\":", "\"feedUrl\":", "\"results\":", "\"releases\":", "\"id\":",
    "\xc3\xa9", "\xe2\x80\x99", "\xf0\x9f\x8e\xb5", "\xff", "\xc3", "\xe2\x80", "\n", "\r\n", " ", "\t", "\0",
    "FILE \"", "\" WAVE\n", "TRACK 01 AUDIO\n", "INDEX 01 ", "00:00:00", "99:59:74", "TITLE \"", "PERFORMER \"",
    "REM GENRE ", "#EXTM3U\n", "#EXTINF:", "#PLAYLIST:", "#EXT-X-STREAM-INF:BANDWIDTH=", "http://", "../", "\\",
    "background = ", "accent = ", "#", "0x", "selection = bar", "font = ",
    "ID3", "TIT2", "APIC", "CHAP", "CTOC", "COMM", "TXXX", "fLaC", "OggS", "OpusTags", "\x03vorbis",
    "METADATA_BLOCK_PICTURE=", "DSD ", "fmt ", "data", "FRM8", "DSD ", "PROP", "SND ", "PK\x03\x04", "PK\x01\x02",
    "PK\x05\x06", "\x89PNG\r\n\x1a\n", "IHDR", "IDAT", "\xff\xd8\xff", "\xff\xc2", "\xff\xc0",
};

static size_t mutate(uint8_t *b, size_t n, size_t cap)
{
    int k = 1 + (int)rn(6);
    while (k--) {
        switch (rn(11)) {
        case 0: if (n) b[rn(n)] ^= (uint8_t)(1 << rn(8)); break;
        case 1: if (n) b[rn(n)] = (uint8_t)rnd(); break;
        case 2: if (n) { static const uint8_t iv[] = {0, 1, 0x7f, 0x80, 0xff, 0xfe, '0', '9', ' '}; b[rn(n)] = iv[rn(sizeof iv)]; } break;
        case 3: if (n > 1) { size_t a = rn(n), l = 1 + rn(n - a < 64 ? n - a : 64); memmove(b + a, b + a + l, n - a - l); n -= l; } break;
        case 4: if (n + 16 < cap) { size_t a = rn(n + 1), l = 1 + rn(16); memmove(b + a + l, b + a, n - a); for (size_t i = 0; i < l; i++) b[a + i] = (uint8_t)rnd(); n += l; } break;
        case 5: case 6: {
            const char *w = DICT[rn(sizeof DICT / sizeof *DICT)];
            size_t l = strlen(w);
            if (!l) l = 1;
            if (n + l < cap) { size_t a = rn(n + 1); memmove(b + a + l, b + a, n - a); memcpy(b + a, w, l); n += l; }
            break;
        }
        case 7: if (n > 2) { size_t a = rn(n), l = 1 + rn(n - a), at = rn(n + 1); if (n + l < cap) { uint8_t *t = malloc(l); memcpy(t, b + a, l); memmove(b + at + l, b + at, n - at); memcpy(b + at, t, l); free(t); n += l; } } break;
        case 8: if (g_nc) { input_t *o = &g_corp[rn((size_t)g_nc)]; if (o->n) { size_t a = rn(o->n), l = 1 + rn(o->n - a); size_t at = rn(n + 1); if (n + l < cap) { memmove(b + at + l, b + at, n - at); memcpy(b + at, o->d + a, l); n += l; } } } break;
        case 9: if (n >= 4) { size_t a = rn(n - 3); uint32_t v = (uint32_t)rnd(); static const uint32_t iv[] = {0, 0xffffffff, 0x7fffffff, 0x80000000, 0x10000, 0xffff}; if (rn(2)) v = iv[rn(6)]; memcpy(b + a, &v, 4); } break;
        case 10: if (n) n = rn(n) + 1; break;   /* cut short */
        }
    }
    return n;
}

static int run_count_new(const uint8_t *d, size_t n)
{
    memset(g_hit, 0, g_n_guards + 1);
    g_cur = d;
    g_cur_n = n;
    alarm(10);
    run_one(d, n);
    alarm(0);
    int fresh = 0;
    for (uint32_t i = 1; i <= g_n_guards; i++)
        if (g_hit[i] && !g_seen[i]) { g_seen[i] = 1; fresh++; }
    return fresh;
}

static uint8_t *read_file(const char *p, size_t *n)
{
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long l = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (l < 0) l = 0;
    if (l > (1 << 20)) l = 1 << 20;
    uint8_t *b = malloc((size_t)l + 1);
    *n = fread(b, 1, (size_t)l, f);
    fclose(f);
    return b;
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "fuzz TARGET SECONDS SEED... | fuzz TARGET -run FILE...\n"); return 2; }
    g_target = argv[1];
    g_hit = calloc(g_n_guards + 2, 1);
    g_seen = calloc(g_n_guards + 2, 1);
    __sanitizer_set_death_callback(dump_current);
    signal(SIGALRM, on_alarm);
    if (!strcmp(argv[2], "-run")) {
        for (int i = 3; i < argc; i++) {
            size_t n;
            uint8_t *b = read_file(argv[i], &n);
            if (!b) { perror(argv[i]); continue; }
            run_count_new(b, n);
            free(b);
        }
        printf("ran %d inputs\n", argc - 3);
        return 0;
    }
    double secs = atof(argv[2]);
    g_r ^= (uint64_t)getpid() * 2654435761u;
    for (int i = 3; i < argc; i++) {
        size_t n;
        uint8_t *b = read_file(argv[i], &n);
        if (!b) continue;
        run_count_new(b, n);
        add(b, n);
        free(b);
    }
    if (!g_nc) add((const uint8_t *)"", 0);
    size_t cap = 1 << 20;
    uint8_t *b = malloc(cap + 64);
    time_t t0 = time(NULL), last = t0;
    long iters = 0;
    while (difftime(time(NULL), t0) < secs) {
        input_t *src = &g_corp[rn((size_t)g_nc)];
        size_t n = src->n < cap ? src->n : cap;
        memcpy(b, src->d, n);
        n = mutate(b, n, cap);
        if (run_count_new(b, n)) {
            add(b, n);
            const char *cd = getenv("FUZZ_CORPUS");   /* kept for the next run */
            if (cd) {
                char p[600];
                snprintf(p, sizeof p, "%s/%s-%d-%d", cd, g_target, getpid(), g_nc);
                FILE *f = fopen(p, "wb");
                if (f) { fwrite(b, 1, n, f); fclose(f); }
            }
        }
        iters++;
        if (time(NULL) - last >= 30) {
            last = time(NULL);
            uint32_t cov = 0;
            for (uint32_t i = 1; i <= g_n_guards; i++) cov += g_seen[i];
            fprintf(stderr, "fuzz %s: %ld runs, corpus %d, %u edges\n", g_target, iters, g_nc, cov);
        }
    }
    uint32_t cov = 0;
    for (uint32_t i = 1; i <= g_n_guards; i++) cov += g_seen[i];
    printf("fuzz %s: %ld runs, corpus %d, %u edges\n", g_target, iters, g_nc, cov);
    return 0;
}
