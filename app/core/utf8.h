/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* A character as UTF-8: written in one place (tags, cue sheets, playlists,
 * JSON, feeds, FAT's long names and the interface each had their own). */
#ifndef FATHOM_UTF8_H
#define FATHOM_UTF8_H

#include <stddef.h>
#include <stdint.h>

/* The code point c as 1 to 4 bytes at o (not ended with a 0); how many. What
 * may be written is the caller's business: a surrogate or a value past
 * U+10FFFF is encoded as it comes. */
static inline size_t utf8_encode(uint32_t c, char *o)
{
    if (c < 0x80) { o[0] = (char)c; return 1; }
    if (c < 0x800) { o[0] = (char)(0xC0 | c >> 6); o[1] = (char)(0x80 | (c & 0x3F)); return 2; }
    if (c < 0x10000) {
        o[0] = (char)(0xE0 | c >> 12); o[1] = (char)(0x80 | ((c >> 6) & 0x3F)); o[2] = (char)(0x80 | (c & 0x3F));
        return 3;
    }
    o[0] = (char)(0xF0 | c >> 18); o[1] = (char)(0x80 | ((c >> 12) & 0x3F));
    o[2] = (char)(0x80 | ((c >> 6) & 0x3F)); o[3] = (char)(0x80 | (c & 0x3F));
    return 4;
}

#endif
