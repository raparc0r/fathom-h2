/* SPDX-License-Identifier: GPL-3.0-or-later */
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

/* Splits what a stream says is playing ("Artist - Title") into its parts;
 * without " - ", it all goes to title. */
void radio_split_meta(const char *meta, char *artist, size_t an, char *title, size_t tn);

#endif
