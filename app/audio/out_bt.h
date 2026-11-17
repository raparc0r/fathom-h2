/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Bluetooth output through bluez-alsa (see out_bt.c). */
#ifndef FATHOM_OUT_BT_H
#define FATHOM_OUT_BT_H

#include "audio/output.h"

/* bt_dir: where bluez-alsa lives (usr/bin/bluealsa-cli, usr/lib). */
int  out_bt_create(audio_output_t *out, const char *bt_dir);
/* For engine_switch_output(): arg is the PCM's D-Bus path (output closed). */
void out_bt_set_pcm(void *ctx, void *pcm);

#endif
