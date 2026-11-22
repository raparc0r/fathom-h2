/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/*
 * Internet radio stations: fathom/radio.txt on the card (assets/radio.txt is
 * an example). One station per line,
 *     Name = stream URL [| info: SOURCE]
 * '#' starts a note. SOURCE says where to ask what's playing when the stream
 * doesn't say: "radiofrance:N" (Radio France's live API, station N).
 */
#ifndef FATHOM_RADIO_H
#define FATHOM_RADIO_H

#include <stddef.h>

#define RADIO_MAX 64

typedef struct {
    char name[64];
    char url[512];
    char info[64];     /* "" = what the stream says */
} radio_station_t;

/* Reads up to `max` stations from `text` (the file's contents): how many. */
int radio_parse(const char *text, radio_station_t *out, int max);
/* The same from a file: -1 if it can't be read. */
int radio_load(const char *path, radio_station_t *out, int max);

/* Your list (fathom/radio.txt) changed from the H2 (Home > Radio > Find
 * stations). Until there's one, it's made from the example's stations
 * (example: the bundled list, may be NULL), so adding a station doesn't make
 * them vanish. The file is written whole (path.tmp, then renamed); notes and
 * the other lines stay as they were. */
/* Adds st at the end: 1 added, 0 already there (the same stream), -2 the list
 * is full (RADIO_MAX shown), -1 couldn't write. */
int radio_add(const char *path, const char *example, const radio_station_t *st);
/* Removes the station(s) with this stream: 0 removed, 1 none had it, -1 couldn't write. */
int radio_remove(const char *path, const char *example, const char *url);
/* st as a line of the list ("Name = URL", " | info: ..." if it has some), no line end. */
void radio_line(const radio_station_t *st, char *out, size_t cap);

/* Splits what a stream says is playing ("Artist - Title") into its parts;
 * without " - ", it all goes to title. */
void radio_split_meta(const char *meta, char *artist, size_t an, char *title, size_t tn);

#endif
