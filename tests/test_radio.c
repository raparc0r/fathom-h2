/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* The station list (fathom/radio.txt) and splitting what a stream says. */
#include "player/radio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "tmpdir.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int main(void)
{
    radio_station_t r[8];
    const char *t =
        "# notes\n"
        "  FIP = https://icecast.radiofrance.fr/fip-hifi.aac?id=radiofrance | info: radiofrance:7\n"
        "\n"
        "Radio Paradise=https://stream.radioparadise.com/flacm\n"
        "not a station\n"
        "A = B = http://x.example/s?a=1&b=2   \n"
        " = http://nameless.example/\n"
        "Trailing = https://t.example/a  comment";
    int n = radio_parse(t, r, 8);
    CHECK(n == 5);
    CHECK(!strcmp(r[0].name, "FIP"));
    CHECK(!strcmp(r[0].url, "https://icecast.radiofrance.fr/fip-hifi.aac?id=radiofrance"));
    CHECK(!strcmp(r[0].info, "radiofrance:7"));
    CHECK(!strcmp(r[1].name, "Radio Paradise"));
    CHECK(!strcmp(r[1].url, "https://stream.radioparadise.com/flacm"));
    CHECK(r[1].info[0] == '\0');
    CHECK(!strcmp(r[2].name, "A = B"));
    CHECK(!strcmp(r[2].url, "http://x.example/s?a=1&b=2"));
    CHECK(!strcmp(r[3].name, "Station 4"));
    CHECK(!strcmp(r[4].url, "https://t.example/a"));
    CHECK(radio_parse(t, r, 2) == 2);

    /* the example shipped for the card */
    n = radio_load("assets/radio.txt", r, 8);
    CHECK(n == 4);
    CHECK(n == 4 && !strcmp(r[3].name, "EHFM"));

    char a[64], ti[64];
    radio_split_meta("Nina Simone - Feeling Good", a, sizeof a, ti, sizeof ti);
    CHECK(!strcmp(a, "Nina Simone") && !strcmp(ti, "Feeling Good"));
    radio_split_meta("Morning show with Someone", a, sizeof a, ti, sizeof ti);
    CHECK(a[0] == '\0' && !strcmp(ti, "Morning show with Someone"));
    radio_split_meta("A - B - C", a, sizeof a, ti, sizeof ti);
    CHECK(!strcmp(a, "A") && !strcmp(ti, "B - C"));
    radio_split_meta("Artist - ", a, sizeof a, ti, sizeof ti);
    CHECK(a[0] == '\0' && !strcmp(ti, "Artist"));

    /* changing the list from the H2 */
    char dir[200], path[128], path2[128];
    test_tmp(dir, sizeof dir, "fathom-radio");
    CHECK(mkdtemp(dir) != NULL);
    snprintf(path, sizeof path, "%s/radio.txt", dir);
    snprintf(path2, sizeof path2, "%s/other.txt", dir);
    radio_station_t big[RADIO_MAX + 4], add = {.name = "#Jazz\nFM ", .url = "https://jazz.example/live"};
    /* no list yet: made from the example's stations, then the new one */
    CHECK(radio_add(path, "assets/radio.txt", &add) == 1);
    n = radio_load(path, big, RADIO_MAX);
    CHECK(n == 5);
    CHECK(n == 5 && !strcmp(big[0].name, "FIP") && !strcmp(big[0].info, "radiofrance:7"));
    CHECK(n == 5 && !strcmp(big[3].name, "EHFM"));
    CHECK(n == 5 && !strcmp(big[4].name, "Jazz FM") && !strcmp(big[4].url, "https://jazz.example/live"));
    FILE *f = fopen(path, "rb");
    char text[4096] = "";
    if (f) { text[fread(text, 1, sizeof text - 1, f)] = '\0'; fclose(f); }
    CHECK(!strncmp(text, "# Your internet radio stations", 30));
    CHECK(strstr(text, "\nJazz FM = https://jazz.example/live\n") != NULL);
    CHECK(strstr(text, "copy it there") == NULL);           /* the example's notes stay in the example */
    /* the same stream again: not added */
    snprintf(add.name, sizeof add.name, "Another name");
    CHECK(radio_add(path, "assets/radio.txt", &add) == 0);
    CHECK(radio_load(path, big, RADIO_MAX) == 5);
    /* removing */
    CHECK(radio_remove(path, "assets/radio.txt", big[0].url) == 0);
    n = radio_load(path, big, RADIO_MAX);
    CHECK(n == 4 && !strcmp(big[0].name, "Radio Paradise"));
    CHECK(radio_remove(path, "assets/radio.txt", "https://not.there/") == 1);
    CHECK(radio_load(path, big, RADIO_MAX) == 4);
    /* removing one of the example's before there's a list: the rest stay */
    CHECK(radio_remove(path2, "assets/radio.txt", "https://ehfm.out.airtime.pro/ehfm_a") == 0);
    n = radio_load(path2, big, RADIO_MAX);
    CHECK(n == 3 && !strcmp(big[2].name, "Djam Radio"));
    /* a list of your own: notes and Windows line ends kept, no line end at the end */
    f = fopen(path2, "wb");
    if (f) { fputs("# mine\r\nOne = http://one.example/\r\n# two later\r\nTwo = http://two.example/ | info: x", f); fclose(f); }
    radio_station_t three = {.name = "Three", .url = "http://three.example/", .info = "radiofrance:1"};
    CHECK(radio_add(path2, NULL, &three) == 1);
    f = fopen(path2, "rb");
    if (f) { text[fread(text, 1, sizeof text - 1, f)] = '\0'; fclose(f); }
    CHECK(!strcmp(text, "# mine\r\nOne = http://one.example/\r\n# two later\r\nTwo = http://two.example/ | info: x\n"
                        "Three = http://three.example/ | info: radiofrance:1\n"));
    CHECK(radio_remove(path2, NULL, "http://two.example/") == 0);
    f = fopen(path2, "rb");
    if (f) { text[fread(text, 1, sizeof text - 1, f)] = '\0'; fclose(f); }
    CHECK(!strcmp(text, "# mine\r\nOne = http://one.example/\r\n# two later\r\n"
                        "Three = http://three.example/ | info: radiofrance:1\n"));
    /* full: RADIO_MAX stations shown, no more added */
    for (int i = 2; i < RADIO_MAX; i++) {
        radio_station_t s = {.name = "S"};
        snprintf(s.url, sizeof s.url, "http://s%d.example/", i);
        CHECK(radio_add(path2, NULL, &s) == 1);
    }
    CHECK(radio_load(path2, big, RADIO_MAX + 4) == RADIO_MAX);
    radio_station_t more = {.name = "More", .url = "http://more.example/"};
    CHECK(radio_add(path2, NULL, &more) == -2);
    /* nowhere to write */
    CHECK(radio_add("/nonexistent-dir/radio.txt", "assets/radio.txt", &more) == -1);
    remove(path);
    remove(path2);
    rmdir(dir);

    if (!fails) printf("radio: ok\n");
    return fails != 0;
}
