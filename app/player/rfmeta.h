/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/*
 * What's playing on a Radio France station (FIP...), from its live API:
 * https://api.radiofrance.fr/livemeta/pull/N (radio.txt: "| info: radiofrance:N").
 * The answer lists steps (songs, programmes) and, in levels[0], their order
 * and which one is on now (position): that is used, not the clock (the H2's
 * may be unset).
 */
#ifndef FATHOM_RFMETA_H
#define FATHOM_RFMETA_H

#include <stddef.h>

/* The station number in an info source ("radiofrance:7" -> 7), or -1. */
int rf_station(const char *info);
/* The API's URL for station n. */
void rf_url(int n, char *out, size_t cap);
/* From the API's answer: the step on now. 0 = found (artist may be "");
 * visual: its cover's URL, or "" (may be NULL). */
int rf_parse(const char *json, char *artist, size_t an, char *title, size_t tn, char *visual, size_t vn);

#endif
