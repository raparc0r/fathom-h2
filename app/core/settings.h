/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/*
 * Settings: one human-readable file of `key = value` lines (fathom/settings.ini
 * on the H2). Keys are dotted ("display.brightness"). Comments start with ';'
 * or '#'. Unknown keys are kept, so a newer file survives an older firmware.
 * Saved atomically (temporary file, then rename) and only when something changed.
 * Any number of entries, keys and values of any length.
 */
#ifndef FATHOM_SETTINGS_H
#define FATHOM_SETTINGS_H

/* Loads `path` (a missing file is fine: everything has a default). */
int         settings_load(const char *path);
/* (The value's good until that key is set again, a load or a reset.) */
const char *settings_get(const char *key, const char *def);
int         settings_get_int(const char *key, int def);
void        settings_set(const char *key, const char *value);
void        settings_set_int(const char *key, int value);
/* Forget every setting (back to the defaults) and save. */
int         settings_reset(void);
/* Writes the file if anything changed. 0 on success (or nothing to do). */
int         settings_save(void);
/* For a periodic tick: save once changes have settled for `quiet_ms`. */
void        settings_autosave(unsigned now_ms, unsigned quiet_ms);

#endif
