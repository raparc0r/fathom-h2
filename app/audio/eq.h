/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Parametric EQ: AutoEQ / Equalizer APO profiles, run in integer arithmetic
 * (the H2 has no FPU) on the player's left-justified int32 samples. */
#ifndef FATHOM_EQ_H
#define FATHOM_EQ_H

#include <stddef.h>
#include <stdint.h>

#define EQ_MAX_BANDS 16
#define EQ_MAX_CH 8

typedef enum { EQ_PEAK, EQ_LOW_SHELF, EQ_HIGH_SHELF } eq_type_t;

typedef struct {
    eq_type_t type;
    double fc, gain_db, q;
} eq_band_t;

typedef struct {
    double preamp_db;
    int n;
    eq_band_t band[EQ_MAX_BANDS];
} eq_profile_t;

/* An AutoEQ "ParametricEQ.txt" (Equalizer APO's format): a "Preamp: -6.2 dB"
 * line and "Filter N: ON PK|LSC|HSC Fc 105 Hz Gain -3.4 dB Q 0.70" lines
 * (LS/HS too; OFF filters are skipped). 0, or -1 with a reason in err. */
int eq_parse(const char *text, eq_profile_t *p, char *err, size_t cap);
int eq_load(const char *path, eq_profile_t *p, char *err, size_t cap);

typedef struct eq eq_t;

/* The profile's filters at one sample rate (a band too close to Nyquist is
 * left out). NULL on failure. */
eq_t *eq_create(const eq_profile_t *p, unsigned rate, int channels);
void  eq_destroy(eq_t *e);
/* In place, `frames` interleaved frames; saturates rather than wraps. */
void  eq_run(eq_t *e, int32_t *s, long frames);
void  eq_reset(eq_t *e);              /* forget the filters' history (a seek, a new track) */
unsigned eq_rate(const eq_t *e);
int   eq_channels(const eq_t *e);

/* The profile's response in dB at f Hz (double precision; for tests and the UI). */
double eq_response_db(const eq_profile_t *p, unsigned rate, double f);
/* The bands' response (preamp left out) at n frequencies f[] into db[]: the
 * same as eq_response_db() minus the preamp, with each band designed once and
 * the trigonometry done once a frequency (the UI's curve, redrawn as a value
 * is turned, on a CPU without an FPU). */
void eq_curve_db(const eq_profile_t *p, unsigned rate, const double *f, int n, float *db);

#endif
