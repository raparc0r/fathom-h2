/* SPDX-License-Identifier: GPL-3.0-or-later */
/* LVGL display on DRM/KMS: one dumb buffer, LVGL renders into it directly and
 * each flushed area is sent to the panel with DIRTYFB (only what changed). */
#ifndef FATHOM_DISP_DRM_H
#define FATHOM_DISP_DRM_H

#include "lvgl.h"

lv_display_t *disp_drm_create(const char *card);
/* Backlight PWM duty, 0 (off) .. disp_drm_backlight_max(). */
void disp_drm_set_backlight_raw(int value);
/* The picture shown as the player exits (RGB565, the screen's size; kept by
 * the caller): the boot logo. disp_drm_show_last_frame() puts it on screen;
 * it only copies and makes one ioctl, so it is safe in a signal handler. */
void disp_drm_set_last_frame(const uint16_t *rgb565);
void disp_drm_show_last_frame(void);
int  disp_drm_backlight_max(void);

#endif
