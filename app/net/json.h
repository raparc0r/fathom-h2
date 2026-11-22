/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/*
 * Reading JSON in place (the radio directory's results, GitHub's release
 * list): no tree is built and nothing is allocated. A json_t is a value's
 * span in the original text; look members up in objects, step through arrays,
 * and copy out strings (unescaped, as UTF-8) and numbers.
 *
 *   json_t root = json_root(text, len), rel, assets, a;
 *   json_member(root, "tag_name", &v); json_string(v, tag, sizeof tag);
 *   size_t at = 0;
 *   while (json_next(assets, &at, &a)) { ... }
 *
 * Malformed input never reads past the text: lookups just fail.
 */
#ifndef FATHOM_JSON_H
#define FATHOM_JSON_H

#include <stddef.h>

typedef struct {
    const char *s;      /* the value's first character (NULL: none) */
    size_t n;           /* its length */
} json_t;

typedef enum { JSON_NONE, JSON_OBJECT, JSON_ARRAY, JSON_STRING, JSON_NUMBER, JSON_BOOL, JSON_NULL } json_type_t;

json_t      json_root(const char *text, size_t len);      /* the top-level value */
json_type_t json_type(json_t v);
/* An object's member by name: 1 and *out, or 0. */
int         json_member(json_t obj, const char *name, json_t *out);
/* An array's (or object's values') next element: start with *at = 0. 1 and *out, or 0 at the end. */
int         json_next(json_t arr, size_t *at, json_t *out);
/* A string's text, unescaped (\n, \", \uXXXX incl. surrogate pairs) as UTF-8 into out:
 * its length (cut at cap - 1, never mid-character), or -1 if v isn't a string. */
long        json_string(json_t v, char *out, size_t cap);
/* A number (or a numeric string) as an integer; def if it isn't one. */
long long   json_int(json_t v, long long def);
/* true/false (or 1/0); def otherwise. */
int         json_bool(json_t v, int def);

#endif
