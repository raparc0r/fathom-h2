/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * h2boot (Fathom): the boot logo lit at the user's brightness.
 *
 * Rockbox's backlight_init() lights the screen at full. The logo stayed at
 * full until the player set the user's level, seconds later; now it's at
 * that level from the start: display.brightness from /fathom/settings.ini
 * (the player's own setting), turned into the PWM value the player would
 * set, at the period Linux uses, which keeps it (kernel patch 0026). No
 * card or no setting: level 3, the player's default.
 *
 * The parsing is plain C, so tests/test_bootlight.c runs it on the host
 * (BOOTLIGHT_HOST); the rest reads the card and sets the PWM in h2boot.
 */
#include "bootlight.h"

#define LEVELS 16
#define LEVEL_DEFAULT 3
#define PERIOD_NS 20000

/* The player's curve (device/main.c, level_pwm): level 1 at 50 (below about
 * 49 the backlight goes dark), then a gentle curve (exponent 1.6) up to 255:
 * 50 + lround(205 * ((level - 1) / 15) ^ 1.6). */
static const unsigned char LEVEL_PWM[LEVELS] = {
    50, 53, 58, 66, 75, 85, 97, 111, 125, 141, 157, 175, 193, 213, 234, 255,
};

int fathom_boot_level_pwm(int level)
{
    if(level < 1 || level > LEVELS)
        level = LEVEL_DEFAULT;
    return LEVEL_PWM[level - 1];
}

static int is_space(char c) { return c == ' ' || c == '\t'; }

int fathom_boot_level(const char *ini, int len)
{
    static const char key[] = "display.brightness";
    const int klen = (int)sizeof key - 1;
    int i = 0;
    while(i < len) {
        int line = i;                            /* a line: [spaces] key [spaces] = [spaces] N */
        while(i < len && ini[i] != '\n')
            i++;
        int end = i++;
        int p = line;
        while(p < end && is_space(ini[p]))
            p++;
        if(end - p < klen)
            continue;
        int k = 0;
        while(k < klen && ini[p + k] == key[k])
            k++;
        if(k < klen)
            continue;
        p += klen;
        while(p < end && is_space(ini[p]))
            p++;
        if(p >= end || ini[p] != '=')
            continue;
        p++;
        while(p < end && is_space(ini[p]))
            p++;
        int v = 0, digits = 0;
        while(p < end && ini[p] >= '0' && ini[p] <= '9' && digits < 3) {
            v = v * 10 + (ini[p++] - '0');
            digits++;
        }
        return digits && v >= 1 && v <= LEVELS ? v : LEVEL_DEFAULT;
    }
    return LEVEL_DEFAULT;
}

#ifndef BOOTLIGHT_HOST
#include "file.h"
#include "pwm-x1000.h"

int fathom_boot_light_level(int has_card)
{
    static char buf[4096];               /* (settings.ini: about 1 KB; the setting near its top) */
    int n = 0;
    if(has_card) {
        int fd = open("/fathom/settings.ini", O_RDONLY);
        if(fd >= 0) {
            n = read(fd, buf, sizeof buf);
            close(fd);
        }
    }
    return fathom_boot_level_pwm(fathom_boot_level(buf, n > 0 ? n : 0));
}

void fathom_boot_light(int pwm)
{
    if(pwm < 1 || pwm > 255)
        return;
    pwm_set_period(0, PERIOD_NS, PERIOD_NS * pwm / 255);
}
#endif
