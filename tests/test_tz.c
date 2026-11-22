/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* app/core/tz.c: each zone's offset, summer time included, against the C
 * library's reading of the same POSIX rule; local times read back. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "core/tz.h"

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static int64_t utc(int y, int mo, int d, int h, int mi)
{
    struct tm tm = {0};
    tm.tm_year = y - 1900;
    tm.tm_mon = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min = mi;
    return (int64_t)timegm(&tm);
}

int main(void)
{
    int n = tz_count();
    CHECK(n > 30 && !strcmp(TZ_ZONES[0].name, "UTC"));
    CHECK(tz_find("London") > 0 && !strcmp(TZ_ZONES[tz_find("London")].name, "London"));
    CHECK(tz_find("Atlantis") == 0 && tz_find(NULL) == 0);
    /* every zone, against the C library, 2026-2032, every 3 h 17 min (both sides of each change) */
    for (int z = 0; z < n; z++) {
        setenv("TZ", TZ_ZONES[z].rule, 1);
        tzset();
        int bad = 0;
        for (int64_t t = utc(2026, 1, 1, 0, 0); t < utc(2033, 1, 1, 0, 0) && bad < 3; t += 3 * 3600 + 17 * 60) {
            time_t tt = (time_t)t;
            struct tm tm;
            localtime_r(&tt, &tm);
            if (tm.tm_gmtoff != tz_offset(z, t)) {
                fprintf(stderr, "%s at %lld: %d, the C library %ld\n", TZ_ZONES[z].name, (long long)t,
                        tz_offset(z, t), (long)tm.tm_gmtoff);
                fails++;
                bad++;
            }
            /* read back: what the local time shows means this time (except in the hour that repeats) */
            int64_t l = tz_to_local(z, t);
            int64_t back = tz_to_utc(z, l);
            if (back != t && tz_to_local(z, back) != l) {
                fprintf(stderr, "%s: %lld back as %lld\n", TZ_ZONES[z].name, (long long)t, (long long)back);
                fails++;
                bad++;
            }
        }
        /* and right at the changes: a minute either side of each the C library sees */
        for (int64_t t = utc(2026, 1, 1, 0, 0); t < utc(2028, 1, 1, 0, 0); t += 60 * 15) {
            time_t a = (time_t)t, b = (time_t)(t + 60 * 15);
            struct tm ta, tb;
            localtime_r(&a, &ta);
            localtime_r(&b, &tb);
            if (ta.tm_gmtoff == tb.tm_gmtoff) continue;
            for (int64_t u = t; u <= t + 60 * 15; u += 60) {
                time_t uu = (time_t)u;
                struct tm tu;
                localtime_r(&uu, &tu);
                if (tu.tm_gmtoff != tz_offset(z, u)) {
                    fprintf(stderr, "%s near a change, %lld: %d, the C library %ld\n", TZ_ZONES[z].name, (long long)u,
                            tz_offset(z, u), (long)tu.tm_gmtoff);
                    fails++;
                    break;
                }
            }
        }
    }
    unsetenv("TZ");

    int lon = tz_find("London"), syd = tz_find("Sydney"), ind = tz_find("India"), nfl = tz_find("St. John's");
    CHECK(tz_offset(lon, utc(2026, 1, 15, 12, 0)) == 0);
    CHECK(tz_offset(lon, utc(2026, 7, 15, 12, 0)) == 3600);
    CHECK(tz_offset(syd, utc(2026, 1, 15, 12, 0)) == 11 * 3600);   /* (summer in January) */
    CHECK(tz_offset(syd, utc(2026, 7, 15, 12, 0)) == 10 * 3600);
    /* London's spring change, 2026-03-29 01:00 UTC: 01:30 local doesn't exist (read as 01:30 GMT) */
    CHECK(tz_to_utc(lon, utc(2026, 3, 29, 1, 30)) == utc(2026, 3, 29, 1, 30));
    CHECK(tz_to_utc(lon, utc(2026, 3, 29, 2, 30)) == utc(2026, 3, 29, 1, 30));
    /* its autumn one, 2026-10-25 01:00 UTC: 01:30 local twice, the first */
    CHECK(tz_to_utc(lon, utc(2026, 10, 25, 1, 30)) == utc(2026, 10, 25, 0, 30));
    char s[16];
    tz_offset_text(0, 0, s, sizeof s);
    CHECK(!strcmp(s, "UTC"));
    tz_offset_text(lon, utc(2026, 7, 1, 0, 0), s, sizeof s);
    CHECK(!strcmp(s, "UTC+1"));
    tz_offset_text(ind, utc(2026, 7, 1, 0, 0), s, sizeof s);
    CHECK(!strcmp(s, "UTC+5:30"));
    tz_offset_text(nfl, utc(2026, 1, 1, 0, 0), s, sizeof s);
    CHECK(!strcmp(s, "UTC-3:30"));
    CHECK(tz_rule_offset("nonsense", 0) == 0 && tz_rule_offset(NULL, 0) == 0);

    if (fails) {
        fprintf(stderr, "%d failed\n", fails);
        return 1;
    }
    printf("tz: all passed\n");
    return 0;
}
