/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* app/core/datetime.c: setting the clock by hand, a field at a time. */
#include <stdio.h>
#include <time.h>

#include "core/datetime.h"

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static int is(const dt_t *d, int y, int mo, int day, int h, int mi)
{
    return d->year == y && d->mon == mo && d->day == day && d->hour == h && d->min == mi;
}

static dt_t make(int y, int mo, int day, int h, int mi)
{
    dt_t d = {y, mo, day, h, mi};
    return d;
}

int main(void)
{
    dt_t d;
    /* to and from seconds, against the C library's */
    for (int64_t t = 1767225600; t < 4102444800LL; t += 86400 * 7 + 3600 * 5 + 60 * 17) {
        time_t tt = (time_t)t;
        struct tm tm;
        gmtime_r(&tt, &tm);
        dt_from_time(t, &d);
        if (!is(&d, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min)) {
            fprintf(stderr, "dt_from_time(%lld)\n", (long long)t);
            fails++;
            break;
        }
        CHECK(dt_to_time(&d) == t - tm.tm_sec);
    }
    dt_from_time(1795284990, &d);                       /* 2026-11-21 18:16:30 */
    CHECK(is(&d, 2026, 11, 21, 18, 16));
    CHECK(dt_to_time(&d) == 1795284960);
    /* a clock that isn't set (1970), or out of the years (2025 included): from 2026-01-01 */
    dt_from_time(1767225599, &d);
    CHECK(is(&d, 2026, 1, 1, 0, 0));
    dt_from_time(0, &d);
    CHECK(is(&d, 2026, 1, 1, 0, 0) && dt_to_time(&d) == DT_START);
    dt_from_time(4200000000LL, &d);
    CHECK(is(&d, 2026, 1, 1, 0, 0));

    CHECK(dt_days_in_month(2026, 2) == 28 && dt_days_in_month(2028, 2) == 29);
    CHECK(dt_days_in_month(2100, 2) == 28 && dt_days_in_month(2000, 2) == 29);
    CHECK(dt_days_in_month(2026, 4) == 30 && dt_days_in_month(2026, 12) == 31 && dt_days_in_month(2026, 13) == 0);

    /* the year stops at its ends */
    d = make(DT_YEAR_MIN, 6, 15, 12, 30);
    dt_adjust(&d, DT_YEAR, -1);
    CHECK(d.year == DT_YEAR_MIN);
    d.year = DT_YEAR_MAX;
    dt_adjust(&d, DT_YEAR, +1);
    CHECK(d.year == DT_YEAR_MAX);
    /* the others go round */
    d = make(2026, 12, 31, 23, 59);
    dt_adjust(&d, DT_MONTH, +1);
    CHECK(d.mon == 1 && d.year == 2026);                /* (the year stays: each field on its own) */
    dt_adjust(&d, DT_MONTH, -1);
    CHECK(d.mon == 12);
    dt_adjust(&d, DT_DAY, +1);
    CHECK(d.day == 1 && d.mon == 12);
    dt_adjust(&d, DT_DAY, -1);
    CHECK(d.day == 31);
    dt_adjust(&d, DT_HOUR, +1);
    CHECK(d.hour == 0);
    dt_adjust(&d, DT_HOUR, -1);
    CHECK(d.hour == 23);
    dt_adjust(&d, DT_MIN, +1);
    CHECK(d.min == 0 && d.hour == 23);
    dt_adjust(&d, DT_MIN, -1);
    CHECK(d.min == 59);
    dt_adjust(&d, DT_MIN, 0);
    CHECK(d.min == 59);
    /* a day past the month's end becomes its last */
    d = make(2026, 1, 31, 0, 0);
    dt_adjust(&d, DT_MONTH, +1);
    CHECK(d.mon == 2 && d.day == 28);
    d = make(2028, 2, 29, 0, 0);
    dt_adjust(&d, DT_YEAR, +1);
    CHECK(d.year == 2029 && d.day == 28);
    d = make(2026, 2, 28, 0, 0);
    dt_adjust(&d, DT_DAY, +1);
    CHECK(d.day == 1);                                  /* (round within February) */
    d = make(2028, 2, 28, 0, 0);
    dt_adjust(&d, DT_DAY, +1);
    CHECK(d.day == 29);
    /* the result is always a real time */
    d = make(2026, 3, 31, 10, 5);
    dt_adjust(&d, DT_MONTH, +1);
    CHECK(is(&d, 2026, 4, 30, 10, 5));
    dt_t back;
    dt_from_time(dt_to_time(&d), &back);
    CHECK(is(&back, 2026, 4, 30, 10, 5));

    if (fails) {
        fprintf(stderr, "%d failed\n", fails);
        return 1;
    }
    printf("datetime: all passed\n");
    return 0;
}
