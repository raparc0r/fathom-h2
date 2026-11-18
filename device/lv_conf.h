/* SPDX-License-Identifier: GPL-3.0-or-later */
/* LVGL configuration for the H2 (DRM display, evdev input handled by Fathom). Only overrides. */
#if 1
#ifndef LV_CONF_H
#define LV_CONF_H

#define LV_COLOR_DEPTH 16                 /* the panel's native RGB565 */
#define LV_USE_STDLIB_MALLOC  LV_STDLIB_CLIB
#define LV_USE_STDLIB_STRING  LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_CLIB
#define LV_USE_OS LV_OS_NONE
#define LV_DEF_REFR_PERIOD 16

#define LV_USE_TINY_TTF 1
#define LV_TINY_TTF_FILE_SUPPORT 0
#define LV_USE_SNAPSHOT 1

#define LV_USE_LOG 1
#define LV_LOG_LEVEL LV_LOG_LEVEL_WARN
#define LV_LOG_PRINTF 1

/* Only what Fathom draws: objects, labels, images and bars (flex layout), into
 * RGB565 from A8 (glyphs, icons), RGB565 and ARGB8888 (layers, images), and
 * RGB888 for screenshots. No theme: the UI styles everything itself (the
 * default theme pulled in most widgets). The rest was 245 KB of code the player
 * carried on the card for nothing. Same in sim/lv_conf.h and device/lv_conf.h,
 * so the simulator shows what the H2 can draw. */
#define LV_DRAW_SW_SUPPORT_RGB565A8 0
#define LV_DRAW_SW_SUPPORT_XRGB8888 0
#define LV_DRAW_SW_SUPPORT_L8       0
#define LV_DRAW_SW_SUPPORT_AL88     0
#define LV_DRAW_SW_SUPPORT_I1       0
#define LV_USE_ANIMIMG     0
#define LV_USE_ARC         0
#define LV_USE_BUTTON      0
#define LV_USE_BUTTONMATRIX 0
#define LV_USE_CALENDAR    0
#define LV_USE_CANVAS      0
#define LV_USE_CHART       0
#define LV_USE_CHECKBOX    0
#define LV_USE_DROPDOWN    0
#define LV_USE_IMAGEBUTTON 0
#define LV_USE_KEYBOARD    0
#define LV_USE_LED         0
#define LV_USE_LINE        0
#define LV_USE_LIST        0
#define LV_USE_MENU        0
#define LV_USE_MSGBOX      0
#define LV_USE_ROLLER      0
#define LV_USE_SCALE       0
#define LV_USE_SLIDER      0
#define LV_USE_SPAN        0
#define LV_USE_SPINBOX     0
#define LV_USE_SPINNER     0
#define LV_USE_SWITCH      0
#define LV_USE_TEXTAREA    0
#define LV_USE_TABLE       0
#define LV_USE_TABVIEW     0
#define LV_USE_TILEVIEW    0
#define LV_USE_WIN         0
#define LV_LABEL_TEXT_SELECTION 0
#define LV_USE_THEME_DEFAULT 0
#define LV_USE_THEME_SIMPLE  0
#define LV_USE_THEME_MONO    0
#define LV_USE_GRID          0
#define LV_USE_OBSERVER      0

#define LV_FONT_MONTSERRAT_14 1
#define LV_USE_DEMO_WIDGETS 0
#define LV_BUILD_EXAMPLES 0

#endif
#endif
