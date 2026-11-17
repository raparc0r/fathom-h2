/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The radio's extras, fetched on a worker thread: what's playing on stations
 * whose streams don't say (radio.txt "info:", app/player/rfmeta.h), and the
 * cover of the song playing, when the station names one. */
#ifndef FATHOM_RADIOMETA_H
#define FATHOM_RADIOMETA_H

#include <stddef.h>
#include <stdint.h>

/* The station playing now's info source ("" or NULL = none). */
void radiometa_want(const char *info);
/* New news since the last call: 1 and the artist, title and cover URL ("" = unknown). */
int  radiometa_take(char *artist, size_t an, char *title, size_t tn, char *visual, size_t vn);

/* The cover to show (an image URL; "" or NULL = none). */
void radiometa_want_art(const char *url);
/* The cover changed since the last call: 1 with px (size*size RGB565) filled,
 * 0 if there's none to show (no URL, or it couldn't be fetched), -1 = no change. */
int  radiometa_take_art(uint16_t *px, int size);

#endif
