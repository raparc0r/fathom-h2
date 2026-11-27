/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/*
 * A date and time in UTC, to the minute, for setting the clock by hand
 * (Settings > About > Clock) and for reading dates (netclock.c): fields
 * turned one at a time with the wheel, each kept to what makes a real date.
 */
#ifndef FATHOM_DATETIME_H
#define FATHOM_DATETIME_H

#include <stdint.h>

typedef struct {
    int year, mon, day;     /* mon 1-12, day 1-31 */
    int hour, min;
} dt_t;

/* The fields, in the order they're set */
enum { DT_YEAR, DT_MONTH, DT_DAY, DT_HOUR, DT_MIN, DT_FIELDS };

/* The years the clock can be set to by hand */
#define DT_YEAR_MIN 2026
#define DT_YEAR_MAX 2099
/* Where setting it starts while the clock isn't set: 2026-01-01 00:00 UTC */
#define DT_START 1767225600LL

int dt_days_in_month(int year, int mon);
/* Days from 1970-01-01 to year-mon-day (proleptic Gregorian; Howard
 * Hinnant's). Here in the header: files built without datetime.c use it too
 * (the feeds' dates, the uninstaller's), where each had its own copy. */
static inline int64_t dt_days_from_civil(int64_t y, int m, int d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}
/* t (seconds since 1970, UTC) as a date and time, its seconds dropped; a time
 * outside DT_YEAR_MIN-DT_YEAR_MAX (a clock that isn't set) gives DT_START's */
void dt_from_time(int64_t t, dt_t *d);
/* t as a date and time, whatever its year (dt_from_time keeps to the years above) */
void dt_split(int64_t t, dt_t *d);
/* d as seconds since 1970 (UTC, :00 seconds) */
int64_t dt_to_time(const dt_t *d);
/* One step of field (DT_*) up (dir > 0) or down: the year stops at its ends,
 * the others go round (Dec to Jan, 23 to 0 hours...); a day past the month's
 * end (Feb 30 after a change of month) becomes its last. */
void dt_adjust(dt_t *d, int field, int dir);

#endif
