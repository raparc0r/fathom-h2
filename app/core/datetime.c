/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* A date and time in UTC, set a field at a time (datetime.h). */
#include "core/datetime.h"

int dt_days_in_month(int year, int mon)
{
    static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (mon < 1 || mon > 12) return 0;
    int leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    return days[mon - 1] + (mon == 2 && leap);
}

void dt_from_time(int64_t t, dt_t *d)
{
    if (t < dt_days_from_civil(DT_YEAR_MIN, 1, 1) * 86400 || t >= dt_days_from_civil(DT_YEAR_MAX + 1, 1, 1) * 86400)
        t = DT_START;
    dt_split(t, d);
}

void dt_split(int64_t t, dt_t *d)
{
    int64_t days = t / 86400, secs = t % 86400;
    if (secs < 0) { secs += 86400; days--; }
    /* (the inverse of dt_days_from_civil) */
    int64_t z = days + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    d->day = (int)(doy - (153 * mp + 2) / 5 + 1);
    d->mon = (int)(mp < 10 ? mp + 3 : mp - 9);
    d->year = (int)(yoe + era * 400 + (d->mon <= 2));
    d->hour = (int)(secs / 3600);
    d->min = (int)(secs % 3600 / 60);
}

int64_t dt_to_time(const dt_t *d)
{
    return dt_days_from_civil(d->year, d->mon, d->day) * 86400 + d->hour * 3600 + d->min * 60;
}

static int around(int v, int lo, int hi, int dir)
{
    v += dir > 0 ? 1 : -1;
    return v > hi ? lo : v < lo ? hi : v;
}

void dt_adjust(dt_t *d, int field, int dir)
{
    if (!dir) return;
    switch (field) {
    case DT_YEAR:
        d->year += dir > 0 ? 1 : -1;
        if (d->year < DT_YEAR_MIN) d->year = DT_YEAR_MIN;
        if (d->year > DT_YEAR_MAX) d->year = DT_YEAR_MAX;
        break;
    case DT_MONTH: d->mon = around(d->mon, 1, 12, dir); break;
    case DT_DAY: d->day = around(d->day, 1, dt_days_in_month(d->year, d->mon), dir); break;
    case DT_HOUR: d->hour = around(d->hour, 0, 23, dir); break;
    case DT_MIN: d->min = around(d->min, 0, 59, dir); break;
    default: return;
    }
    int last = dt_days_in_month(d->year, d->mon);
    if (d->day > last) d->day = last;
}
