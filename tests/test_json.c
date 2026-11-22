/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* Reading JSON in place (app/net/json.c). */
#include <stdio.h>
#include <string.h>

#include "net/json.h"

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

int main(void)
{
    /* GitHub's release list, cut down */
    static const char rel[] =
        "{\"tag_name\": \"v0.7.0\", \"draft\": false, \"name\": \"Fathom 0.7.0\",\n"
        " \"body\": \"Line one\\nLine \\\"two\\\" \\u00e9\\ud83c\\udfb5\",\n"
        " \"assets\": [ {\"name\": \"fathom-h2-0.7.0.zip\", \"size\": 15689654,\n"
        "   \"browser_download_url\": \"https://github.com/x/y/releases/download/v0.7.0/fathom-h2-0.7.0.zip\"},\n"
        "   {\"name\": \"other\", \"size\": \"12\", \"nested\": {\"a\": [1, [2, 3], {\"b\": null}]}} ] }";
    json_t root = json_root(rel, strlen(rel)), v, assets, a;
    char s[128];
    CHECK(json_type(root) == JSON_OBJECT);
    CHECK(json_member(root, "tag_name", &v) && json_string(v, s, sizeof s) == 6 && !strcmp(s, "v0.7.0"));
    CHECK(json_member(root, "draft", &v) && json_bool(v, 1) == 0);
    CHECK(json_member(root, "body", &v) && json_string(v, s, sizeof s) > 0 &&
          !strcmp(s, "Line one\nLine \"two\" \xC3\xA9\xF0\x9F\x8E\xB5"));
    CHECK(!json_member(root, "missing", &v));
    CHECK(json_member(root, "assets", &assets) && json_type(assets) == JSON_ARRAY);
    size_t at = 0;
    int n = 0;
    while (json_next(assets, &at, &a)) {
        n++;
        if (n == 1) {
            CHECK(json_member(a, "size", &v) && json_int(v, -1) == 15689654);
            CHECK(json_member(a, "browser_download_url", &v) && json_string(v, s, sizeof s) > 0 &&
                  !strncmp(s, "https://github.com/", 19));
        } else {
            CHECK(json_member(a, "size", &v) && json_int(v, -1) == 12);   /* numeric string */
            json_t nested = {0}, arr = {0}, e;
            CHECK(json_member(a, "nested", &nested) && json_member(nested, "a", &arr));
            size_t k = 0;
            int m = 0;
            while (json_next(arr, &k, &e)) m++;
            CHECK(m == 3);
        }
    }
    CHECK(n == 2);

    /* radio-browser.info's station list, cut down: an array at the top */
    static const char st[] = "[{\"name\":\"FIP\",\"url_resolved\":\"https://icecast.radiofrance.fr/fip-hifi.aac\","
                             "\"codec\":\"AAC\",\"bitrate\":192,\"countrycode\":\"FR\",\"tags\":\"jazz,eclectic\"},"
                             "{\"name\":\"Radio \\u00e9t\\u00e9\",\"bitrate\":0}]";
    root = json_root(st, strlen(st));
    CHECK(json_type(root) == JSON_ARRAY);
    at = 0;
    CHECK(json_next(root, &at, &a) && json_member(a, "bitrate", &v) && json_int(v, 0) == 192);
    CHECK(json_next(root, &at, &a) && json_member(a, "name", &v) && json_string(v, s, sizeof s) > 0 &&
          !strcmp(s, "Radio \xC3\xA9t\xC3\xA9"));
    CHECK(!json_next(root, &at, &a));

    /* cut short at a character: never half of one */
    json_t e = json_root("\"\\u00e9\\u00e9\"", 14);
    CHECK(json_string(e, s, 4) == 2 && !strcmp(s, "\xC3\xA9"));

    /* malformed: lookups fail, nothing is read past the end */
    static const char bad[] = "{\"a\": [1, 2, \"x\"";
    CHECK(json_type(json_root(bad, strlen(bad))) == JSON_NONE);
    CHECK(json_type(json_root("", 0)) == JSON_NONE);
    CHECK(!json_member(json_root("[1]", 3), "a", &v));
    char deep[200];
    memset(deep, '[', sizeof deep - 1);
    deep[sizeof deep - 1] = '\0';
    CHECK(json_type(json_root(deep, strlen(deep))) == JSON_NONE);   /* too deep, unterminated */

    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    printf("json: all good\n");
    return 0;
}
