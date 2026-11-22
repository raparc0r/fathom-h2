/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* Time zones for the clock set by hand (tz.h). */
#include "core/tz.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "core/datetime.h"

/* Places for each offset people live at, summer time as their rules have it
 * (2026). West to east, UTC first. */
const tz_zone_t TZ_ZONES[] = {
    {"UTC", "UTC0"},
    {"Honolulu", "HST10"},
    {"Anchorage", "AKST9AKDT,M3.2.0,M11.1.0"},
    {"Los Angeles", "PST8PDT,M3.2.0,M11.1.0"},
    {"Phoenix", "MST7"},
    {"Denver", "MST7MDT,M3.2.0,M11.1.0"},
    {"Mexico City", "CST6"},
    {"Chicago", "CST6CDT,M3.2.0,M11.1.0"},
    {"Bogota, Lima", "<-05>5"},
    {"New York", "EST5EDT,M3.2.0,M11.1.0"},
    {"Halifax", "AST4ADT,M3.2.0,M11.1.0"},
    {"St. John's", "NST3:30NDT,M3.2.0,M11.1.0"},
    {"Sao Paulo", "<-03>3"},
    {"Buenos Aires", "<-03>3"},
    {"London", "GMT0BST,M3.5.0/1,M10.5.0"},
    {"Lisbon", "WET0WEST,M3.5.0/1,M10.5.0"},
    {"Lagos", "WAT-1"},
    {"Paris, Berlin", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Johannesburg", "SAST-2"},
    {"Athens, Helsinki", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"Istanbul", "<+03>-3"},
    {"Moscow", "MSK-3"},
    {"Nairobi", "EAT-3"},
    {"Tehran", "<+0330>-3:30"},
    {"Dubai", "<+04>-4"},
    {"Karachi", "PKT-5"},
    {"India", "IST-5:30"},
    {"Kathmandu", "<+0545>-5:45"},
    {"Dhaka", "<+06>-6"},
    {"Bangkok, Jakarta", "<+07>-7"},
    {"China, Singapore", "CST-8"},
    {"Tokyo, Seoul", "JST-9"},
    {"Adelaide", "ACST-9:30ACDT,M10.1.0,M4.1.0/3"},
    {"Brisbane", "AEST-10"},
    {"Sydney", "AEST-10AEDT,M10.1.0,M4.1.0/3"},
    {"Auckland", "NZST-12NZDT,M9.5.0,M4.1.0/3"},
};

int tz_count(void) { return (int)(sizeof TZ_ZONES / sizeof TZ_ZONES[0]); }

int tz_find(const char *name)
{
    for (int i = 0; name && i < tz_count(); i++)
        if (!strcmp(TZ_ZONES[i].name, name)) return i;
    return 0;
}

/* ---- POSIX TZ: std offset [dst [offset] [,start[/time],end[/time]]] ---- */
typedef struct { int m, w, d, secs; } tz_when_t;     /* Mm.w.d/time: the d-day of week w (5: last) of month m */
typedef struct { int std, dst, has_dst; tz_when_t start, end; } tz_rule_t;   /* (offsets east of UTC) */

static const char *zone_name(const char *p)
{
    if (*p == '<') {
        while (*p && *p != '>') p++;
        return *p ? p + 1 : NULL;
    }
    const char *q = p;
    while (isalpha((unsigned char)*p)) p++;
    return p - q >= 3 ? p : NULL;
}

/* [+-]hh[:mm[:ss]] as seconds */
static const char *hms(const char *p, int *secs)
{
    int sign = 1;
    if (*p == '+' || *p == '-') sign = *p++ == '-' ? -1 : 1;
    if (!isdigit((unsigned char)*p)) return NULL;
    int v[3] = {0, 0, 0};
    for (int i = 0; i < 3; i++) {
        while (isdigit((unsigned char)*p)) v[i] = v[i] * 10 + (*p++ - '0');
        if (i < 2 && *p == ':' && isdigit((unsigned char)p[1])) p++;
        else break;
    }
    *secs = sign * (v[0] * 3600 + v[1] * 60 + v[2]);
    return p;
}

static const char *when(const char *p, tz_when_t *w)
{
    if (*p++ != 'M') return NULL;
    if (sscanf(p, "%d.%d.%d", &w->m, &w->w, &w->d) != 3 || w->m < 1 || w->m > 12 || w->w < 1 || w->w > 5 ||
        w->d < 0 || w->d > 6)
        return NULL;
    while (*p && *p != '/' && *p != ',') p++;
    w->secs = 7200;                                    /* (2:00 unless said) */
    if (*p == '/' && !(p = hms(p + 1, &w->secs))) return NULL;
    return p;
}

static int parse(const char *s, tz_rule_t *r)
{
    memset(r, 0, sizeof *r);
    int west;
    const char *p = zone_name(s);
    if (!p || !(p = hms(p, &west))) return -1;
    r->std = r->dst = -west;
    if (!*p) return 0;
    if (!(p = zone_name(p))) return -1;
    r->has_dst = 1;
    r->dst = r->std + 3600;
    if (*p && *p != ',') {
        if (!(p = hms(p, &west))) return -1;
        r->dst = -west;
    }
    if (*p != ',' || !(p = when(p + 1, &r->start)) || *p != ',' || !(p = when(p + 1, &r->end))) return -1;
    return *p ? -1 : 0;
}

/* The local time of w in year y */
static int64_t local_at(int y, const tz_when_t *w)
{
    int64_t first = dt_days_from_civil(y, w->m, 1);
    int wd = (int)(((first + 4) % 7 + 7) % 7);         /* (1970-01-01 was a Thursday; 0: Sunday) */
    int day = 1 + (w->d - wd + 7) % 7 + (w->w - 1) * 7;
    while (day > dt_days_in_month(y, w->m)) day -= 7;
    return (first + day - 1) * 86400 + w->secs;
}

int tz_rule_offset(const char *rule, int64_t utc)
{
    tz_rule_t r;
    if (!rule || parse(rule, &r)) return 0;
    if (!r.has_dst) return r.std;
    dt_t d;
    dt_split(utc + r.std, &d);
    int64_t on = local_at(d.year, &r.start) - r.std;  /* (the change happens in the time before it) */
    int64_t off = local_at(d.year, &r.end) - r.dst;
    int summer = on < off ? utc >= on && utc < off     /* (north) */
                          : utc >= on || utc < off;    /* (south: summer over New Year) */
    return summer ? r.dst : r.std;
}

int tz_offset(int z, int64_t utc)
{
    return z >= 0 && z < tz_count() ? tz_rule_offset(TZ_ZONES[z].rule, utc) : 0;
}

int64_t tz_to_utc(int z, int64_t local)
{
    /* the offsets a day before and after (changes are months apart): each
     * gives a time; those that read back as local are the answers */
    int oa = tz_offset(z, local - 86400), ob = tz_offset(z, local + 86400);
    int64_t ua = local - oa, ub = local - ob;
    int va = tz_offset(z, ua) == oa, vb = tz_offset(z, ub) == ob;
    if (va && vb) return ua < ub ? ua : ub;           /* (the hour that repeats: the first) */
    if (vb) return ub;
    return ua;                                         /* (or skipped: as before the change) */
}

void tz_offset_text(int z, int64_t utc, char *out, int cap)
{
    int o = tz_offset(z, utc), a = o < 0 ? -o : o;
    if (!o) snprintf(out, (size_t)cap, "UTC");
    else if (a % 3600) snprintf(out, (size_t)cap, "UTC%c%d:%02d", o < 0 ? '-' : '+', a / 3600, a % 3600 / 60);
    else snprintf(out, (size_t)cap, "UTC%c%d", o < 0 ? '-' : '+', a / 3600);
}
