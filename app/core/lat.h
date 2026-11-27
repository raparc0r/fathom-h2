/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* A timing trace for development: LAT("what") prints "lat <ms>: what" to the
 * log (monotonic ms, so from the kernel's start), only while /tmp/fathom-lat,
 * or the card's fathom/test/fathom-lat (for a start-up: /tmp is new then),
 * exists when the player starts (each file looks once). For timing paths on
 * the H2 itself: key to screen, a station to its first audio, start-up, Wi-Fi.
 * Off, it costs one access() per source file. */
#ifndef FATHOM_LAT_H
#define FATHOM_LAT_H

#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>

#include "core/clock.h"

static inline int lat_on(void)
{
    static int on = -1;
    if (on < 0) on = access("/tmp/fathom-lat", F_OK) == 0 || access("/mnt/sd/fathom/test/fathom-lat", F_OK) == 0;
    return on;
}

#define LAT(...) do { if (lat_on()) { fprintf(stderr, "lat %u: ", clock_ms32()); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

#endif
