/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
#include "wifitxt.h"

#include <ctype.h>
#include <string.h>
#include <strings.h>

/* s[0..*n) without spaces at either end */
static const char *trim(const char *s, size_t *n)
{
    while (*n && isspace((unsigned char)*s)) { s++; (*n)--; }
    while (*n && isspace((unsigned char)s[*n - 1])) (*n)--;
    return s;
}

int wifitxt_parse(const char *text, size_t n, wifitxt_net_t *out, int max, int *bad)
{
    int k = 0, have = 0, b = 0;   /* have: out[k] has its name, waiting for a password */
    size_t i = 0;
    if (n >= 3 && !memcmp(text, "\xEF\xBB\xBF", 3)) i = 3;   /* (a BOM, from Notepad) */
    while (i < n) {
        size_t e = i;
        while (e < n && text[e] != '\n') e++;
        size_t len = e - i;
        const char *line = trim(text + i, &len);
        i = e + 1;
        if (!len || line[0] == '#') continue;
        const char *eq = memchr(line, '=', len);
        if (!eq) { b++; continue; }
        size_t kl = (size_t)(eq - line), vl = len - kl - 1;
        const char *key = trim(line, &kl), *val = trim(eq + 1, &vl);
        if (kl == 7 && !strncasecmp(key, "network", 7)) {
            if (have) k++;                      /* (the one before: open) */
            have = 0;
            if (k >= max) break;
            if (!vl || vl > 32) { b++; continue; }
            memcpy(out[k].ssid, val, vl);
            out[k].ssid[vl] = '\0';
            out[k].pass[0] = '\0';
            have = 1;
        } else if (kl == 8 && !strncasecmp(key, "password", 8)) {
            if (!have) { b++; continue; }
            if (vl && (vl < 8 || vl > 63)) { b++; have = 0; continue; }   /* (that network dropped, not open) */
            memcpy(out[k].pass, val, vl);
            out[k].pass[vl] = '\0';
            k++;
            have = 0;
        } else {
            b++;
        }
    }
    if (have && k < max) k++;                   /* (the last one: open) */
    if (bad) *bad = b;
    return k;
}
