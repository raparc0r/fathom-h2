/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* fathom/wifi.txt (app/core/wifitxt.c): what a person might type on a computer. */
#include <stdio.h>
#include <string.h>

#include "core/wifitxt.h"

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static int parse(const char *t, wifitxt_net_t *n, int *bad)
{
    return wifitxt_parse(t, strlen(t), n, WIFITXT_MAX, bad);
}

int main(void)
{
    wifitxt_net_t n[WIFITXT_MAX];
    int bad;

    /* the template, filled in */
    CHECK(parse("# Fathom: Wi-Fi networks\nnetwork = My home\npassword = correct horse\n", n, &bad) == 1 && !bad);
    CHECK(!strcmp(n[0].ssid, "My home") && !strcmp(n[0].pass, "correct horse"));

    /* Windows: a BOM, CRLF, no spaces, any case */
    CHECK(parse("\xEF\xBB\xBFNETWORK=Caf\xC3\xA9\r\nPassword=12345678\r\n", n, &bad) == 1 && !bad);
    CHECK(!strcmp(n[0].ssid, "Caf\xC3\xA9") && !strcmp(n[0].pass, "12345678"));

    /* two, the second open (no password line, or an empty one) */
    CHECK(parse("network = A\npassword = aaaaaaaa\nnetwork = Open one\n", n, &bad) == 2 && !bad);
    CHECK(!strcmp(n[1].ssid, "Open one") && !n[1].pass[0]);
    CHECK(parse("network = B\npassword =\nnetwork = C\npassword = cccccccc\n", n, &bad) == 2 && !bad);
    CHECK(!n[0].pass[0] && !strcmp(n[1].ssid, "C"));

    /* an = in the password is kept; spaces inside too */
    CHECK(parse("network = x\npassword = a=b c d e f\n", n, &bad) == 1 && !strcmp(n[0].pass, "a=b c d e f"));

    /* what can't be used: counted, the rest kept */
    CHECK(parse("network = short\npassword = 1234567\nnetwork = ok\npassword = 12345678\n", n, &bad) == 1 && bad == 1 &&
          !strcmp(n[0].ssid, "ok"));   /* (too short: that network dropped) */
    CHECK(parse("password = orphaned1\n", n, &bad) == 0 && bad == 1);
    CHECK(parse("network = 012345678901234567890123456789012\n", n, &bad) == 0 && bad == 1);   /* 33 bytes */
    CHECK(parse("hello\nssid = x\n", n, &bad) == 0 && bad == 2);
    CHECK(parse("", n, &bad) == 0 && !bad);

    /* no more than asked for */
    char many[1024] = "";
    for (int i = 0; i < 12; i++) snprintf(many + strlen(many), sizeof many - strlen(many), "network = n%d\n", i);
    CHECK(parse(many, n, &bad) == WIFITXT_MAX);

    if (failures) fprintf(stderr, "%d failure(s)\n", failures);
    else printf("wifitxt: all checks passed\n");
    return failures != 0;
}
