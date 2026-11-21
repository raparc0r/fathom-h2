/* SPDX-License-Identifier: GPL-2.0-or-later */
/* h2boot (Fathom): the boot logo lit at the user's brightness (bootlight.c). */
#ifndef FATHOM_BOOTLIGHT_H
#define FATHOM_BOOTLIGHT_H

/* The brightness level (1-16) in settings.ini's text ("display.brightness"),
 * or the player's default (3) when it isn't there or makes no sense. */
int fathom_boot_level(const char *ini, int len);
/* A level's PWM value out of 255, as the player sets it (level_pwm). */
int fathom_boot_level_pwm(int level);

#ifndef BOOTLIGHT_HOST
/* The level from /fathom/settings.ini (has_card: the card is there to read),
 * out of 255: read before the logo lights up. */
int fathom_boot_light_level(int has_card);
/* The backlight at that value, at the period Linux uses (20 us, 50 kHz):
 * right after the logo's backlight_init(), which lights it at full. */
void fathom_boot_light(int pwm);
#endif

#endif
