/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* Radio France's live API answer: the step on now (levels[0].position), its
 * times, and when to ask again. */
#include "player/rfmeta.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_util.h"

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

    /* the cover's address goes to a fetch: only one on the network is one
     * (a file's path, file:, another scheme: the song's info without a cover) */
    static const char *const not_web[] = {"/mnt/sdcard/Music/cover.jpg", "file:///etc/passwd", "crypto:file:///x.jpg",
                                          "ftp://example.org/c.jpg", "//example.org/c.jpg", " https://example.org/c.jpg", ""};
    for (int i = 0; i < 7; i++) {
        char js[320];
        snprintf(js, sizeof js, "{\"levels\":[{\"position\":0,\"items\":[\"c_7\"]}],"
                 "\"steps\":{\"c_7\":{\"title\":\"Next\",\"visual\":\"%s\"}}}", not_web[i]);
        snprintf(v, sizeof v, "x");
        CHECK(rf_parse(js, a, sizeof a, t, sizeof t, v, sizeof v) == 0 && !strcmp(t, "Next") && v[0] == '\0');
    }
    CHECK(rf_parse("{\"levels\":[{\"position\":0,\"items\":[\"c_7\"]}],"
                   "\"steps\":{\"c_7\":{\"title\":\"Next\",\"visual\":\"http://example.org/c?id=1\"}}}",
                   a, sizeof a, t, sizeof t, v, sizeof v) == 0 && !strcmp(v, "http://example.org/c?id=1"));

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

    /* the step's times: of the one on now (b_7 above has none; a_7's aren't asked for) */
    int64_t s = -1, e = -1;
    CHECK(rf_step_times(j, &s, &e) != 0 && s == 0 && e == 0);
    const char *live =
        "{\"steps\":{"
        "\"p_7\":{\"title\":\"Before\",\"start\":1763900000,\"end\":1763900180},"
        "\"q_7\":{\"title\":\"Now\",\"authors\":\"Them\",\"start\": 1763900180 ,\"end\":1763900420,\"depth\":1}},"
        "\"levels\":[{\"items\":[\"p_7\",\"q_7\"],\"position\":1}]}";
    CHECK(rf_parse(live, a, sizeof a, t, sizeof t, v, sizeof v) == 0 && !strcmp(t, "Now"));
    CHECK(rf_step_times(live, &s, &e) == 0 && s == 1763900180 && e == 1763900420);
    CHECK(rf_step_times("{\"steps\":{\"x\":{\"title\":\"T\",\"start\":\"soon\",\"end\":null}},"
                        "\"levels\":[{\"items\":[\"x\"],\"position\":0}]}", &s, &e) != 0 && s == 0 && e == 0);
    CHECK(rf_step_times("{\"steps\":{\"x\":{\"title\":\"T\",\"start\":5}},"
                        "\"levels\":[{\"items\":[\"x\"],\"position\":0}]}", &s, &e) != 0);
    CHECK(rf_step_times("not json", &s, &e) != 0);

    /* when to ask again: 3 s after the song ends... */
    CHECK(rf_next_fetch_s(1763900200, 1763900180, 1763900420) == RF_POLL_MAX_S);   /* (223 s away: a look meanwhile) */
    CHECK(rf_next_fetch_s(1763900300, 1763900180, 1763900420) == 123);
    CHECK(rf_next_fetch_s(1763900180, 1763900180, 1763900420) == RF_POLL_MAX_S);   /* a long song: a look meanwhile */
    CHECK(rf_next_fetch_s(1763900410, 1763900180, 1763900420) == RF_POLL_S);       /* about to end: not sooner than before */
    CHECK(rf_next_fetch_s(1763900405, 1763900180, 1763900420) == 18);
    /* ...and every 15 s as before when that can't be told */
    CHECK(rf_next_fetch_s(1763900420, 1763900180, 1763900420) == RF_POLL_S);       /* over: the next one isn't out yet */
    CHECK(rf_next_fetch_s(1763990000, 1763900180, 1763900420) == RF_POLL_S);
    CHECK(rf_next_fetch_s(0, 1763900180, 1763900420) == RF_POLL_S);                /* the clock unset */
    CHECK(rf_next_fetch_s(1763800000, 1763900180, 1763900420) == RF_POLL_S);       /* ...or a day behind */
    CHECK(rf_next_fetch_s(1763900160, 1763900180, 1763900420) == RF_POLL_MAX_S);   /* 20 s behind: near enough */
    CHECK(rf_next_fetch_s(1763900200, 0, 0) == RF_POLL_S);                         /* no times given */
    CHECK(rf_next_fetch_s(1763900200, 1763900420, 1763900180) == RF_POLL_S);       /* nonsense */
    CHECK(rf_next_fetch_s(1763900200, 1763900180, 1763900180 + 5 * 3600) == RF_POLL_S);

    /* a request that gave nothing: asked again after 5 s, then less and less
     * often while they go on failing (every 5 s for ever, it was) */
    CHECK(rf_retry_s(1) == 5 && rf_retry_s(2) == 15 && rf_retry_s(3) == 60);
    CHECK(rf_retry_s(4) == RF_POLL_MAX_S && rf_retry_s(5) == RF_POLL_MAX_S && rf_retry_s(1000000) == RF_POLL_MAX_S);
    CHECK(rf_retry_s(0) == 5 && rf_retry_s(-1) == 5);                              /* (none counted: as the first) */

    CHECK(rf_station("radiofrance:7") == 7);
    CHECK(rf_station("radiofrance:") == -1);
    CHECK(rf_station("other:7") == -1);
    char u[96];
    rf_url(7, u, sizeof u);
    CHECK(!strcmp(u, "https://api.radiofrance.fr/livemeta/pull/7"));

    return t_result();
}
