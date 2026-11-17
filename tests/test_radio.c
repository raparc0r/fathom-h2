/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The station list (fathom/radio.txt) and splitting what a stream says. */
#include "player/radio.h"

#include <stdio.h>
#include <string.h>

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

    if (!fails) printf("radio: ok\n");
    return fails != 0;
}
