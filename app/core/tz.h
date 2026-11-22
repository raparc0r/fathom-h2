/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/*
 * Time zones for the clock set by hand (Settings > About > Clock): a short
 * list of places, each with its POSIX TZ rule (summer time included), worked
 * out here rather than through the C library's TZ (process-wide, and the H2
 * has no zone files). The H2's clock itself stays in UTC; a zone only changes
 * how a time is shown and read. Kept as its name (clock.tz).
 */
#ifndef FATHOM_TZ_H
#define FATHOM_TZ_H

#include <stdint.h>

typedef struct {
    const char *name;        /* as shown: "London", "Paris, Berlin"; "UTC" */
    const char *rule;        /* POSIX TZ: "GMT0BST,M3.5.0/1,M10.5.0" */
} tz_zone_t;

/* The zones, west to east (UTC first); how many */
extern const tz_zone_t TZ_ZONES[];
int tz_count(void);
/* The zone called name (0, UTC, if none is) */
int tz_find(const char *name);

/* Seconds east of UTC in zone z at utc (summer time included) */
int tz_offset(int z, int64_t utc);
/* utc as it reads in zone z */
static inline int64_t tz_to_local(int z, int64_t utc) { return utc + tz_offset(z, utc); }
/* The UTC time a local time in zone z means (in the hour summer time skips,
 * as if it hadn't; in the hour it repeats, the first) */
int64_t tz_to_utc(int z, int64_t local);
/* "UTC", "UTC+1", "UTC-3:30" at utc */
void tz_offset_text(int z, int64_t utc, char *out, int cap);

/* Any POSIX TZ rule (as above, M rules only): seconds east of UTC at utc;
 * a rule that doesn't parse counts as UTC */
int tz_rule_offset(const char *rule, int64_t utc);

#endif
