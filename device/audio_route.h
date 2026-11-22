/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* The H2's output switch (PB5 STEREOSW_SEL): headphone jack or line-out jack. */
#ifndef FATHOM_AUDIO_ROUTE_H
#define FATHOM_AUDIO_ROUTE_H

typedef enum { ROUTE_HEADPHONES = 0, ROUTE_LINE_OUT = 1 } audio_route_t;

int  audio_route_open(audio_route_t initial);   /* 0 on success */
int  audio_route_set(audio_route_t r);

#endif
