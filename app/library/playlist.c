/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "library/playlist.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define M3U_MAX_BYTES (1 << 20)   /* a playlist bigger than this is not a playlist */

int m3u_supported(const char *path)
{
    const char *dot = strrchr(path, '.');
    return dot && (!strcasecmp(dot, ".m3u") || !strcasecmp(dot, ".m3u8"));
}

void path_normalize(char *p)
{
    for (char *c = p; *c; c++)
        if (*c == '\\') *c = '/';
    int abs = p[0] == '/';
    /* split into components, drop "" and ".", let ".." eat the one before */
    char *comp[256];
    int n = 0;
    char *save = NULL;
    for (char *tok = strtok_r(p, "/", &save); tok; tok = strtok_r(NULL, "/", &save)) {
        if (!strcmp(tok, ".")) continue;
        if (!strcmp(tok, "..") && n > 0 && strcmp(comp[n - 1], "..")) { n--; continue; }
        if (n < 256) comp[n++] = tok;
    }
    char out[4096];
    size_t w = 0;
    if (abs) out[w++] = '/';
    for (int i = 0; i < n && w < sizeof out - 1; i++) {
        size_t l = strlen(comp[i]);
        if (w + l + 2 > sizeof out) break;
        if (i) out[w++] = '/';
        memcpy(out + w, comp[i], l);
        w += l;
    }
    out[w] = '\0';
    strcpy(p, out);   /* never longer than the input */
}

/* U+00C0..U+00FF as base letters ('_' = no letter: ×, ÷) */
static const char LATIN1[] = "aaaaaaaceeeeiiiidnooooo_ouuuuytsaaaaaaaceeeeiiiidnooooo_ouuuuyty";

void path_loose_key(const char *path, int tail, char *out, size_t cap)
{
    const char *start = path + strlen(path);
    for (int k = 0; k < tail && start > path; ) {
        start--;
        if (*start == '/' && ++k == tail) { start++; break; }
    }
    size_t w = 0;
    for (const unsigned char *c = (const unsigned char *)start; *c && w + 1 < cap; c++) {
        if ((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9')) out[w++] = (char)*c;
        else if (*c >= 'A' && *c <= 'Z') out[w++] = (char)(*c + 32);
        else if (*c == '/') out[w++] = '/';
        else if (*c == 0xC3 && c[1] >= 0x80 && c[1] <= 0xBF) {   /* U+00C0..U+00FF */
            char b = LATIN1[c[1] - 0x80];
            if (b != '_') out[w++] = b;
            c++;
        }
        /* everything else, combining marks included, is dropped */
    }
    out[w] = '\0';
}

void text_search_fold(const char *s, char *out, size_t cap)
{
    size_t w = 0;
    int space = 1;                                   /* suppress leading and repeated spaces */
    for (const unsigned char *c = (const unsigned char *)s; c && *c && w + 1 < cap; c++) {
        char ch = 0;
        if ((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9')) ch = (char)*c;
        else if (*c >= 'A' && *c <= 'Z') ch = (char)(*c + 32);
        else if (*c == '\'') continue;
        else if (*c == 0xE2 && c[1] == 0x80 && c[2] == 0x99) { c += 2; continue; }   /* ’ */
        else if (*c == 0xC3 && c[1] >= 0x80 && c[1] <= 0xBF) {
            ch = LATIN1[c[1] - 0x80];
            c++;
            if (ch == '_') ch = 0;
        } else if (*c >= 0x80) {                      /* other characters: skip the whole sequence */
            while ((c[1] & 0xC0) == 0x80) c++;
        }
        if (ch) { out[w++] = ch; space = 0; }
        else if (!space) { out[w++] = ' '; space = 1; }
    }
    while (w && out[w - 1] == ' ') w--;
    out[w] = '\0';
}

void text_loose_key(const char *s, char *out, size_t cap)
{
    path_loose_key(s, 1 << 30, out, cap);
}

static int push(m3u_t *m, int *cap, const char *p, const char *info, int secs)
{
    if (m->n == *cap) {
        int nc = *cap ? *cap * 2 : 64;
        char **np = realloc(m->paths, (size_t)nc * sizeof *np);
        if (np) m->paths = np;
        char **ni = realloc(m->infos, (size_t)nc * sizeof *ni);
        if (ni) m->infos = ni;
        int *ns = realloc(m->secs, (size_t)nc * sizeof *ns);
        if (ns) m->secs = ns;
        if (!np || !ni || !ns) return -1;
        *cap = nc;
    }
    m->paths[m->n] = strdup(p);
    m->infos[m->n] = info && *info ? strdup(info) : NULL;
    m->secs[m->n] = secs;
    return m->paths[m->n] ? (m->n++, 0) : -1;
}

int m3u_parse(const char *text, size_t len, const char *dir, const char *fallback_name, m3u_t *out)
{
    memset(out, 0, sizeof *out);
    snprintf(out->name, sizeof out->name, "%s", fallback_name ? fallback_name : "");
    int cap = 0;
    size_t i = 0;
    if (len >= 3 && !memcmp(text, "\xEF\xBB\xBF", 3)) i = 3;   /* UTF-8 BOM */
    char line[4096], path[8192], info[512] = "";
    int secs = -1;
    while (i < len) {
        size_t e = i;
        while (e < len && text[e] != '\n' && text[e] != '\r') e++;
        size_t l = e - i < sizeof line - 1 ? e - i : sizeof line - 1;
        memcpy(line, text + i, l);
        line[l] = '\0';
        i = e + 1;
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        size_t sl = strlen(s);
        while (sl && (s[sl - 1] == ' ' || s[sl - 1] == '\t')) s[--sl] = '\0';
        if (!*s) continue;
        if (*s == '#') {
            if (!strncmp(s, "#PLAYLIST:", 10) && s[10]) snprintf(out->name, sizeof out->name, "%s", s + 10);
            if (!strncmp(s, "#EXTINF:", 8)) {          /* #EXTINF:277,Artist - Title */
                secs = atoi(s + 8);
                const char *comma = strchr(s + 8, ',');
                snprintf(info, sizeof info, "%s", comma ? comma + 1 : "");
            }
            continue;
        }
        if (strstr(s, "://")) continue;                         /* streams */
        if (!strncmp(s, "file:", 5)) continue;                  /* rare; would need %-decoding */
        int absolute = s[0] == '/' || s[0] == '\\' ||
                       (((s[0] | 32) >= 'a' && (s[0] | 32) <= 'z') && s[1] == ':');   /* C:\... */
        if (absolute || !dir || !*dir) snprintf(path, sizeof path, "%s", s);
        else snprintf(path, sizeof path, "%s/%s", dir, s);
        path_normalize(path);
        if (push(out, &cap, path, info, secs)) { m3u_free(out); return -1; }
        info[0] = '\0';                             /* #EXTINF belongs to one entry */
        secs = -1;
    }
    return 0;
}

/* One line's bytes are UTF-8. */
static int utf8_line(const unsigned char *s, size_t n)
{
    for (size_t i = 0; i < n;) {
        unsigned c = s[i];
        size_t k = c < 0x80 ? 0 : c >= 0xC2 && c < 0xE0 ? 1 : c >= 0xE0 && c < 0xF0 ? 2 : c >= 0xF0 && c < 0xF5 ? 3 : 9;
        if (k == 9 || (k && i + k >= n)) return 0;
        for (size_t j = 1; j <= k; j++)
            if ((s[i + j] & 0xC0) != 0x80) return 0;
        i += k + 1;
    }
    return 1;
}

/* Windows-1252 at 0x80-0x9F (Latin-1 has control codes there): ’ “ ” – … € */
static const uint16_t CP1252[32] = {
    0x20AC, 0x81, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x8D, 0x017D, 0x8F,
    0x90, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x9D, 0x017E, 0x0178};

static uint32_t cp1252(unsigned char c) { return c >= 0x80 && c < 0xA0 ? CP1252[c - 0x80] : c; }

static size_t put_utf8(char *o, uint32_t u)
{
    if (u < 0x80) { o[0] = (char)u; return 1; }
    if (u < 0x800) { o[0] = (char)(0xC0 | u >> 6); o[1] = (char)(0x80 | (u & 0x3F)); return 2; }
    o[0] = (char)(0xE0 | u >> 12); o[1] = (char)(0x80 | ((u >> 6) & 0x3F)); o[2] = (char)(0x80 | (u & 0x3F));
    return 3;
}

/* An .m3u's lines that aren't UTF-8 (in Windows' code page, as Windows
 * players and older software write them) as UTF-8, taken as Windows-1252
 * (Latin-1's letters, and ’ “ ” – …): "Café" spelt so never matched the
 * library's. A new buffer (*len its length), or NULL if every line was
 * UTF-8 already. */
static char *latin1_lines(const char *in, size_t *len)
{
    size_t n = *len, need = 0;
    int bad = 0;
    for (int pass = 0; pass < 2; pass++) {
        char *out = NULL;
        size_t w = 0;
        if (pass == 1) {
            if (!bad || !(out = malloc(need + 1))) return NULL;
        }
        for (size_t i = 0; i < n;) {
            size_t e = i;
            while (e < n && in[e] != '\n') e++;
            if (utf8_line((const unsigned char *)in + i, e - i)) {
                if (out) memcpy(out + w, in + i, e - i);
                w += e - i;
            } else {
                bad = 1;
                char tmp[3];   /* (counting) */
                for (size_t j = i; j < e; j++) w += put_utf8(out ? out + w : tmp, cp1252((unsigned char)in[j]));
            }
            if (e < n) {
                if (out) out[w] = '\n';
                w++;
            }
            i = e + 1;
        }
        need = w;
        if (out) {
            out[w] = '\0';
            *len = w;
            return out;
        }
    }
    return NULL;
}

int m3u_read(const char *path, m3u_t *out)
{
    memset(out, 0, sizeof *out);
    FILE *f = fopen(path, "rbe");
    if (!f) return -1;
    /* as much as the file holds (a 1 MB buffer for any playlist, before) */
    struct stat st;
    size_t cap = fstat(fileno(f), &st) == 0 && st.st_size >= 0 && st.st_size < M3U_MAX_BYTES ? (size_t)st.st_size
                                                                                            : M3U_MAX_BYTES;
    char *buf = malloc(cap + 1);
    size_t n = buf ? fread(buf, 1, cap, f) : 0;
    fclose(f);
    if (!buf) return -1;
    /* .m3u8 is UTF-8 by name; an .m3u may be either, line by line */
    const char *ext = strrchr(path, '.');
    if (ext && !strcasecmp(ext, ".m3u")) {
        char *u = latin1_lines(buf, &n);
        if (u) {
            free(buf);
            buf = u;
        }
    }

    char dir[4096], name[256];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    const char *base = slash ? slash + 1 : dir;
    snprintf(name, sizeof name, "%.255s", base);
    char *dot = strrchr(name, '.');
    if (dot) *dot = '\0';
    for (char *c = name; *c; c++)
        if (*c == '_') *c = ' ';
    if (slash) *slash = '\0';
    else dir[0] = '\0';

    int rc = m3u_parse(buf, n, dir, name, out);
    free(buf);
    return rc;
}

void m3u_free(m3u_t *m)
{
    for (int i = 0; i < m->n; i++) {
        free(m->paths[i]);
        if (m->infos) free(m->infos[i]);
    }
    free(m->paths);
    free(m->infos);
    free(m->secs);
    m->paths = m->infos = NULL;
    m->secs = NULL;
    m->n = 0;
}
