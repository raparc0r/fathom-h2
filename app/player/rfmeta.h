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
#include <stdint.h>

/* The station number in an info source ("radiofrance:7" -> 7), or -1. */
int rf_station(const char *info);
/* The API's URL for station n. */
void rf_url(int n, char *out, size_t cap);
/* From the API's answer: the step on now. 0 = found (artist may be "");
 * visual: its cover's URL (http or https only), or "" (may be NULL). */
int rf_parse(const char *json, char *artist, size_t an, char *title, size_t tn, char *visual, size_t vn);
/* The same step's "start" and "end" (Unix seconds): 0 when it has both. */
int rf_step_times(const char *json, int64_t *start, int64_t *end);

/* When to ask again, in seconds from now (the clock, Unix seconds). The song
 * on now doesn't change until it ends, so: 3 s after its end, though never
 * sooner than RF_POLL_S nor later than RF_POLL_MAX_S (the schedule changes; a
 * programme is one step for an hour). Every RF_POLL_S, as before, when that
 * can't be told: the step has no times, it has ended (the next one isn't out
 * yet), or it hasn't begun by this clock, which is then unset or behind. */
#define RF_POLL_S 15
#define RF_POLL_MAX_S 180
int rf_next_fetch_s(int64_t now, int64_t start, int64_t end);

/* When to ask again after `fails` requests in a row that gave nothing (no
 * network, no answer, an answer with no step on now): 5 s, 15, 60, then every
 * RF_POLL_MAX_S. Every 5 s for as long as they failed was a new TLS
 * connection each time, the screen off included. */
int rf_retry_s(int fails);

#endif
