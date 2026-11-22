/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* Radio France's live API answer: the step on now (levels[0].position). */
#include "player/rfmeta.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int main(void)
{
    const char *j =
        "{\"steps\":{"
        "\"a_7\":{\"uuid\":\"x\",\"title\":\"Old song\",\"authors\":\"Someone\",\"highlightedArtists\":[\"Someone\"],"
        "\"start\":1,\"end\":2,\"fatherStepId\":null,\"depth\":3},"
        "\"b_7\":{\"title\":\"Caf\\u00e9 \\\"live\\\"\",\"authors\":\"A & B\",\"performers\":\"\","
        "\"highlightedArtists\":[],\"nested\":{\"x\":[1,{\"y\":\"}\"}]},\"titreAlbum\":\"Album\",\"visual\":\"https://example.org/c.jpg\"},"
        "\"c_7\":{\"title\":\"Next\",\"highlightedArtists\":[\"Later\"]}},"
        "\"levels\":[{\"items\":[\"a_7\",\"b_7\",\"c_7\"],\"position\":1}],\"stationId\":7}";
    char a[64], t[64], v[96];
    CHECK(rf_parse(j, a, sizeof a, t, sizeof t, v, sizeof v) == 0);
    CHECK(!strcmp(t, "Caf\xc3\xa9 \"live\""));
    CHECK(!strcmp(a, "A & B"));                /* no highlighted artist: the authors */
    CHECK(!strcmp(v, "https://example.org/c.jpg"));

    const char *k =
        "{ \"levels\" : [ { \"position\" : 0 , \"items\" : [ \"c_7\" ] } ],"
        "  \"steps\" : { \"c_7\" : { \"title\" : \"Next\", \"highlightedArtists\" : [ \"Later\", \"X\" ] } } }";
    CHECK(rf_parse(k, a, sizeof a, t, sizeof t, v, sizeof v) == 0);
    CHECK(!strcmp(t, "Next") && !strcmp(a, "Later") && v[0] == '\0');

    CHECK(rf_parse("{\"levels\":[{\"items\":[\"z\"],\"position\":0}],\"steps\":{}}", a, sizeof a, t, sizeof t, NULL, 0) != 0);
    CHECK(rf_parse("{\"levels\":[{\"items\":[],\"position\":0}]}", a, sizeof a, t, sizeof t, NULL, 0) != 0);
    CHECK(rf_parse("not json", a, sizeof a, t, sizeof t, NULL, 0) != 0);
    CHECK(rf_parse("{\"steps\":{\"a\":{\"title\":\"unterminated", a, sizeof a, t, sizeof t, NULL, 0) != 0);
    /* escapes cut short at the very end (in a buffer of their own size, for ASan): refused, nothing past the end read */
    static const char *const cut[] = {"{\"levels\":[{\"position\":0,\"items\":[\"\\ud83c\\ua", "{\"levels\":[{\"items\":[\"\\u12",
                                      "{\"levels\":[{\"items\":[\"\\ud8\"]}]}"};
    for (int i = 0; i < 3; i++) {
        size_t n = strlen(cut[i]) + 1;
        char *c = malloc(n);
        memcpy(c, cut[i], n);
        CHECK(rf_parse(c, a, sizeof a, t, sizeof t, NULL, 0) != 0);
        free(c);
    }

    CHECK(rf_station("radiofrance:7") == 7);
    CHECK(rf_station("radiofrance:") == -1);
    CHECK(rf_station("other:7") == -1);
    char u[96];
    rf_url(7, u, sizeof u);
    CHECK(!strcmp(u, "https://api.radiofrance.fr/livemeta/pull/7"));

    if (!fails) printf("rfmeta: ok\n");
    return fails != 0;
}
