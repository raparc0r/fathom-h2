/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/*
 * PB5 through the GPIO character device (v2 ABI): 0 routes the amplifier to
 * the headphone jack, 1 to the line-out jack.
 */
#include "audio_route.h"
#include "gpio_line.h"

#include <fcntl.h>
#include <linux/gpio.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define SELECT_LINE 5   /* PB5 */

static int g_line = -1;

int audio_route_open(audio_route_t initial)
{
    if (g_line < 0) {
        g_line = gpio_line_request("GPIOB", SELECT_LINE, initial == ROUTE_LINE_OUT, "fathom-output-select");
        if (g_line == -2) perror("audio route: PB5");
    }
    return g_line >= 0 ? 0 : -1;
}

int audio_route_set(audio_route_t r)
{
    if (g_line < 0) return -1;
    return gpio_line_set(g_line, r == ROUTE_LINE_OUT);
}
