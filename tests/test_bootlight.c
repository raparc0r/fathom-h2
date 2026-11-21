/* SPDX-License-Identifier: GPL-2.0-or-later */
/* h2boot's boot logo brightness (bootloader/fathom/bootlight.c): the level
 * read from settings.ini's text, and its PWM value as the player sets it. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "bootlight.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static int level(const char *s) { return fathom_boot_level(s, (int)strlen(s)); }

int main(void)
{
    /* the player's file, as it writes it */
    const char *ini = "; Fathom settings. Edit with the player off; unknown keys are kept.\n"
                      "ui.theme = Studio Mint\nclock.last = 1795284990\ndisplay.brightness = 7\n"
                      "power.auto_off = 15\n";
    CHECK(level(ini) == 7, "%d", level(ini));
    CHECK(level("display.brightness=12") == 12, "no spaces, no newline");
    CHECK(level("  display.brightness\t =  16\r\n") == 16, "spaces, tabs, CRLF");
    CHECK(level("display.brightness = 1\n") == 1, "1");
    /* the default (3): none, nonsense, out of range, another key that starts the same */
    CHECK(level("") == 3 && level("ui.theme = x\n") == 3, "none");
    CHECK(level("display.brightness = 0\n") == 3 && level("display.brightness = 17\n") == 3, "range");
    CHECK(level("display.brightness = bright\n") == 3 && level("display.brightness =\n") == 3, "nonsense");
    CHECK(level("display.brightness_old = 9\ndisplay.brightness = 5\n") == 5, "only its own key");
    CHECK(level("; display.brightness = 9\n") == 3, "a comment isn't it");
    CHECK(fathom_boot_level("display.brightness = 12", 22) == 1 && fathom_boot_level("display.brightness = 12", 21) == 3,
          "only len bytes read");
    /* the PWM: the player's curve, 50 + lround(205 * ((l - 1) / 15)^1.6) */
    for (int l = 1; l <= 16; l++) {
        int want = 50 + (int)floor(205.0 * pow((l - 1) / 15.0, 1.6) + 0.5);
        CHECK(fathom_boot_level_pwm(l) == want, "level %d: %d, not %d", l, fathom_boot_level_pwm(l), want);
    }
    CHECK(fathom_boot_level_pwm(3) == 58 && fathom_boot_level_pwm(0) == 58 && fathom_boot_level_pwm(99) == 58, "default");
    printf("%s (%d failed)\n", fails ? "FAILED" : "bootlight: ok", fails);
    return fails != 0;
}
