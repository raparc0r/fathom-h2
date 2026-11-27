/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* The station list (fathom/radio.txt) and splitting what a stream says. */
#include "player/radio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "test_util.h"

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

    /* a station's icon: after the stream, with or without an info source, in either order */
    char long_icon[400] = "https://long.example/";
    memset(long_icon + strlen(long_icon), 'a', 300);
    char tl[2048];
    snprintf(tl, sizeof tl,
        "A = http://a.example/s | icon: https://a.example/logo.png\n"
        "B = http://b.example/s | info: radiofrance:7 | icon: https://b.example/l.jpg\n"
        "C = http://c.example/s | icon: https://c.example/l.jpg | info: radiofrance:1\n"
        "D = http://d.example/s | icon: https://d.example/a b.png\n"
        "E = http://e.example/s | icon: ftp://e.example/l.png | info: x\n"
        "F = http://f.example/s|icon:https://f.example/l.png?a=1&b=2|info:y\n"
        "G = http://g.example/s | info: z | later: something | icon: %s\n"
        "H = http://h.example/s | icon:\n", long_icon);
    radio_station_t ic[8];
    n = radio_parse(tl, ic, 8);
    CHECK(n == 8);
    CHECK(!strcmp(ic[0].url, "http://a.example/s") && !strcmp(ic[0].icon, "https://a.example/logo.png") && !ic[0].info[0]);
    CHECK(!strcmp(ic[1].info, "radiofrance:7") && !strcmp(ic[1].icon, "https://b.example/l.jpg"));
    CHECK(!strcmp(ic[2].info, "radiofrance:1") && !strcmp(ic[2].icon, "https://c.example/l.jpg"));
    CHECK(!ic[3].icon[0] && !strcmp(ic[3].url, "http://d.example/s"));     /* a space in it: left out */
    CHECK(!ic[4].icon[0] && !strcmp(ic[4].info, "x"));                    /* not http(s) */
    CHECK(!strcmp(ic[5].url, "http://f.example/s") && !strcmp(ic[5].icon, "https://f.example/l.png?a=1&b=2") && !strcmp(ic[5].info, "y"));
    CHECK(!ic[6].icon[0] && !strcmp(ic[6].info, "z"));                    /* too long to keep whole: left out, not cut */
    CHECK(!ic[7].icon[0] && !strcmp(ic[7].name, "H"));
    /* written and read back the same; without an icon, the line as it always was */
    for (int i = 0; i < 3; i++) {
        char line[1536];
        radio_station_t back;
        radio_line(&ic[i], line, sizeof line);
        CHECK(radio_parse(line, &back, 1) == 1 && !memcmp(&back, &ic[i], sizeof back));
    }
    char one[1536];
    radio_line(&ic[1], one, sizeof one);
    CHECK(!strcmp(one, "B = http://b.example/s | info: radiofrance:7 | icon: https://b.example/l.jpg"));
    radio_line(&ic[0], one, sizeof one);
    CHECK(!strcmp(one, "A = http://a.example/s | icon: https://a.example/logo.png"));
    radio_station_t odd = {.name = "Odd", .url = "http://o.example/s", .icon = "https://o.example/a|b.png"};
    radio_line(&odd, one, sizeof one);
    CHECK(!strcmp(one, "Odd = http://o.example/s"));                      /* a '|' would cut the line there */
    snprintf(odd.icon, sizeof odd.icon, "https://o.example/a b.png");
    radio_line(&odd, one, sizeof one);
    CHECK(!strcmp(one, "Odd = http://o.example/s"));
    snprintf(odd.icon, sizeof odd.icon, "file:///etc/passwd");
    radio_line(&odd, one, sizeof one);
    CHECK(!strcmp(one, "Odd = http://o.example/s"));
    {   /* a name that holds "= http://...": the line reads back with its own stream, not the name's */
        radio_station_t tr = {.name = "X = http://other.example/s", .url = "http://real.example/s"}, back[2];
        radio_line(&tr, one, sizeof one);
        CHECK(radio_parse(one, back, 2) == 1 && !strcmp(back[0].url, "http://real.example/s") &&
              !strcmp(back[0].name, "X - http://other.example/s"));
        snprintf(tr.name, sizeof tr.name, "2 + 2 = 4, a=https://x");
        radio_line(&tr, one, sizeof one);
        CHECK(radio_parse(one, back, 2) == 1 && !strcmp(back[0].url, "http://real.example/s") &&
              !strcmp(back[0].name, "2 + 2 = 4, a-https://x"));
    }
    CHECK(radio_icon_ok("http://x.example/i.png") && radio_icon_ok("https://x.example/i") && !radio_icon_ok("") &&
          !radio_icon_ok(NULL) && !radio_icon_ok("//x.example/i.png") && !radio_icon_ok(long_icon));
    /* the longest line there can be fits the buffers that hold one */
    radio_station_t full;
    memset(&full, 'x', sizeof full);
    full.name[sizeof full.name - 1] = full.url[sizeof full.url - 1] = full.info[sizeof full.info - 1] = full.icon[sizeof full.icon - 1] = '\0';
    full.site[sizeof full.site - 1] = '\0';
    memcpy(full.url, "http://", 7);
    memcpy(full.icon, "https://", 8);
    memcpy(full.site, "https://", 8);
    radio_line(&full, one, sizeof one);
    radio_station_t back;
    CHECK(strlen(one) < 1200 && radio_parse(one, &back, 1) == 1 && !memcmp(&back, &full, sizeof back));

    /* a station's own site: read with or without an icon, in any order,
     * written last, and left out when it isn't an address */
    radio_station_t si[4];
    n = radio_parse("A = http://a.example/s | site: https://a.example/\n"
                    "B = http://b.example/s | site: https://b.example/radio | icon: https://b.example/l.png | info: x\n"
                    "C = http://c.example/s | site: c.example\n"
                    "D = http://d.example/s | icon: https://d.example/l.png\n", si, 4);
    CHECK(n == 4 && !strcmp(si[0].site, "https://a.example/") && !si[0].icon[0]);
    CHECK(n == 4 && !strcmp(si[1].site, "https://b.example/radio") && !strcmp(si[1].icon, "https://b.example/l.png") &&
          !strcmp(si[1].info, "x"));
    CHECK(n == 4 && !si[2].site[0] && !si[3].site[0]);
    radio_line(&si[1], one, sizeof one);
    CHECK(!strcmp(one, "B = http://b.example/s | info: x | icon: https://b.example/l.png | site: https://b.example/radio"));
    radio_line(&si[0], one, sizeof one);
    CHECK(!strcmp(one, "A = http://a.example/s | site: https://a.example/"));

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
    CHECK(t_mkdtemp(dir, sizeof dir, "fathom-radio") != NULL);
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
    {   /* a list that is there and can't be read (the card not answering: here, no
         * leave to read it) is not "no list yet": nothing is made from the example and
         * written over it, and the owner's stations are there when it reads again */
        radio_station_t fresh = {.name = "New", .url = "http://new.example/"};
        char before[2048] = "", after[2048] = "";
        f = fopen(path2, "rb");
        if (f) { before[fread(before, 1, sizeof before - 1, f)] = '\0'; fclose(f); }
        CHECK(chmod(path2, 0) == 0);
        if (geteuid() != 0) {              /* (root reads it whatever its mode) */
            CHECK(radio_add(path2, "assets/radio.txt", &fresh) == -1);
            CHECK(radio_remove(path2, "assets/radio.txt", "http://one.example/") == -1);
        }
        CHECK(chmod(path2, 0644) == 0);
        f = fopen(path2, "rb");
        if (f) { after[fread(after, 1, sizeof after - 1, f)] = '\0'; fclose(f); }
        CHECK(before[0] && !strcmp(before, after));
    }
    /* full: RADIO_MAX stations shown, no more added */
    for (int i = 2; i < RADIO_MAX; i++) {
        radio_station_t s = {.name = "S"};
        snprintf(s.url, sizeof s.url, "http://s%d.example/", i);
        CHECK(radio_add(path2, NULL, &s) == 1);
    }
    CHECK(radio_load(path2, big, RADIO_MAX + 4) == RADIO_MAX);
    radio_station_t more = {.name = "More", .url = "http://more.example/"};
    CHECK(radio_add(path2, NULL, &more) == -2);
    /* a station added with its icon: in its line; and with an info source too */
    char path3[128], ex[128];
    snprintf(path3, sizeof path3, "%s/icons.txt", dir);
    snprintf(ex, sizeof ex, "%s/example.txt", dir);
    radio_station_t withicon = {.name = "Logo FM", .url = "http://logo.example/s", .icon = "https://logo.example/l.png"};
    f = fopen(path3, "wb");
    if (f) { fputs("Mine = http://mine.example/s\nTheirs = http://theirs.example/s | info: radiofrance:3\n", f); fclose(f); }
    CHECK(radio_add(path3, NULL, &withicon) == 1);
    snprintf(withicon.name, sizeof withicon.name, "Both");
    snprintf(withicon.url, sizeof withicon.url, "http://both.example/s");
    snprintf(withicon.info, sizeof withicon.info, "radiofrance:9");
    CHECK(radio_add(path3, NULL, &withicon) == 1);
    f = fopen(path3, "rb");
    if (f) { text[fread(text, 1, sizeof text - 1, f)] = '\0'; fclose(f); }
    CHECK(!strcmp(text, "Mine = http://mine.example/s\nTheirs = http://theirs.example/s | info: radiofrance:3\n"
                        "Logo FM = http://logo.example/s | icon: https://logo.example/l.png\n"
                        "Both = http://both.example/s | info: radiofrance:9 | icon: https://logo.example/l.png\n"));
    n = radio_load(path3, big, RADIO_MAX);
    CHECK(n == 4 && !strcmp(big[2].icon, "https://logo.example/l.png") && !strcmp(big[3].info, "radiofrance:9") && !big[0].icon[0]);
    CHECK(radio_remove(path3, NULL, "http://logo.example/s") == 0 && radio_load(path3, big, RADIO_MAX) == 3);
    /* Home > Radio's list: yours; a station of yours without an icon takes the
     * example's for the same stream (a list made from the example before it had any) */
    f = fopen(ex, "wb");
    if (f) {
        fputs("Mine = http://mine.example/s | icon: https://mine.example/m.png\n"
              "Both = http://both.example/s | icon: https://other.example/o.png\n"
              "Only here = http://only.example/s | icon: https://only.example/o.png\n", f);
        fclose(f);
    }
    n = radio_load_list(path3, ex, big, RADIO_MAX);
    CHECK(n == 3 && !strcmp(big[0].name, "Mine") && !strcmp(big[0].icon, "https://mine.example/m.png"));
    CHECK(n == 3 && !big[1].icon[0] && !strcmp(big[1].info, "radiofrance:3"));          /* (not in the example) */
    CHECK(n == 3 && !strcmp(big[2].icon, "https://logo.example/l.png"));               /* its own stays */
    CHECK(radio_load_list(path3, NULL, big, RADIO_MAX) == 3 && !big[0].icon[0]);
    remove(path3);
    n = radio_load_list(path3, ex, big, RADIO_MAX);                                    /* no list yet: the example */
    CHECK(n == 3 && !strcmp(big[2].name, "Only here") && !strcmp(big[2].icon, "https://only.example/o.png"));
    CHECK(radio_load_list(path3, NULL, big, RADIO_MAX) == -1);
    remove(ex);
    /* a long list (70 KB of notes, then the stations): all of it is read, where
     * only its first 64 KB were and the stations after that weren't shown; a
     * station is added to it and removed from it, its notes as they were. One
     * past any list's length (1 MB) is shown as far as it is read and is not
     * changed: whole afterwards (such a list was written back cut off, once) */
    {
        char pathl[128];
        snprintf(pathl, sizeof pathl, "%s/long.txt", dir);
        f = fopen(pathl, "wb");
        if (f) {
            for (int i = 0; i < 1000; i++) fprintf(f, "# a note, %04d: sixty-nine bytes a line with its end, as notes are\n", i);
            fputs("Far = http://far.example/\nFarther = http://farther.example/\n", f);
            fclose(f);
        }
        struct stat st0, st1;
        CHECK(stat(pathl, &st0) == 0 && st0.st_size > 66000);
        n = radio_load(pathl, big, RADIO_MAX);
        CHECK(n == 2 && !strcmp(big[0].name, "Far") && !strcmp(big[1].url, "http://farther.example/"));
        radio_station_t far = {.name = "Farthest", .url = "http://farthest.example/"};
        CHECK(radio_add(pathl, NULL, &far) == 1);
        CHECK(radio_load(pathl, big, RADIO_MAX) == 3 && !strcmp(big[2].name, "Farthest"));
        CHECK(radio_remove(pathl, NULL, "http://far.example/") == 0);
        CHECK(radio_load(pathl, big, RADIO_MAX) == 2 && !strcmp(big[0].name, "Farther"));
        CHECK(stat(pathl, &st1) == 0 && st1.st_size == st0.st_size + (off_t)strlen("Farthest = http://farthest.example/\n")
                                                                   - (off_t)strlen("Far = http://far.example/\n"));
        f = fopen(pathl, "wb");
        if (f) {
            fputs("Near = http://near.example/\n", f);
            for (int i = 0; i < 16000; i++) fprintf(f, "# a note, %05d: sixty-nine bytes a line with its end, as notes are\n", i);
            fputs("Lost = http://lost.example/\n", f);
            fclose(f);
        }
        CHECK(stat(pathl, &st0) == 0 && st0.st_size > 1024 * 1024);
        CHECK(radio_load(pathl, big, RADIO_MAX) == 1 && !strcmp(big[0].name, "Near"));
        CHECK(radio_add(pathl, NULL, &far) == -1);
        CHECK(radio_remove(pathl, NULL, "http://near.example/") == -1);
        CHECK(stat(pathl, &st1) == 0 && st1.st_size == st0.st_size);
        remove(pathl);
    }
    /* an address with a '|' in it (a directory's station is anyone's): the
     * line ends its address there, and what followed isn't read back as the
     * station's logo and site */
    {
        radio_station_t bar = {.name = "Bar", .url = "http://radio.example/live|icon:http://192.168.1.1/x|site:http://192.168.1.1/"};
        char bline[1536];
        radio_line(&bar, bline, sizeof bline);
        CHECK(!strcmp(bline, "Bar = http://radio.example/live"));
        radio_station_t rb;
        CHECK(radio_parse(bline, &rb, 1) == 1 && !rb.icon[0] && !rb.site[0] && !strcmp(rb.url, "http://radio.example/live"));
    }
    /* nowhere to write */
    CHECK(radio_add("/nonexistent-dir/radio.txt", "assets/radio.txt", &more) == -1);
    remove(path);
    remove(path2);
    rmdir(dir);

    return t_result();
}
