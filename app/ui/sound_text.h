/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* Settings > Sound: how a value reads on its row. The interface asks the
 * platform for these texts (ui_hooks_t.sound_value); the H2 and the simulator
 * each wrote them out. (Its texts are in the translation template:
 * tools/lang/strings.py lists this file.) */
#ifndef FATHOM_SOUND_TEXT_H
#define FATHOM_SOUND_TEXT_H

#include <stdio.h>
#include <stdlib.h>

#include "audio/width.h"
#include "core/lang.h"

static inline void sound_text_width(int pct, char *out, size_t cap)
{
    if (pct == WIDTH_MONO) snprintf(out, cap, "%s", tr("Mono"));
    else if (pct == WIDTH_NORMAL) snprintf(out, cap, "%s", tr("Off"));
    else snprintf(out, cap, "%d %%", pct);
}

static inline void sound_text_balance(int db, char *out, size_t cap)
{
    char n[16];
    snprintf(n, sizeof n, "%d", abs(db));
    if (!db) snprintf(out, cap, "%s", tr("Centre"));
    else tr_fill(out, cap, db < 0 ? "%s dB left" : "%s dB right", n, NULL);
}

/* The volume limit of a wired output, dB (0: none) */
static inline void sound_text_limit_db(int db, char *out, size_t cap)
{
    if (!db) snprintf(out, cap, "%s", tr("Off"));
    else snprintf(out, cap, "%d dB", db);
}

#endif
