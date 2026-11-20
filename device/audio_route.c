/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * PB5 through the GPIO character device (v2 ABI): 0 routes the amplifier to
 * the headphone jack, 1 to the line-out jack.
 */
#include "audio_route.h"

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
    for (int i = 0; i < 8 && g_line < 0; i++) {
        char path[32];
        snprintf(path, sizeof path, "/dev/gpiochip%d", i);
        int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0) continue;
        struct gpiochip_info info;
        memset(&info, 0, sizeof info);
        if (ioctl(fd, GPIO_GET_CHIPINFO_IOCTL, &info) == 0 && !strcmp(info.label, "GPIOB")) {
            struct gpio_v2_line_request req;
            memset(&req, 0, sizeof req);
            req.offsets[0] = SELECT_LINE;
            req.num_lines = 1;
            req.config.flags = GPIO_V2_LINE_FLAG_OUTPUT;
            req.config.num_attrs = 1;
            req.config.attrs[0].attr.id = GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES;
            req.config.attrs[0].attr.values = initial == ROUTE_LINE_OUT;
            req.config.attrs[0].mask = 1;
            snprintf(req.consumer, sizeof req.consumer, "fathom-output-select");
            if (ioctl(fd, GPIO_V2_GET_LINE_IOCTL, &req) == 0) g_line = req.fd;
            else perror("audio route: PB5");
        }
        close(fd);
    }
    return g_line >= 0 ? 0 : -1;
}

int audio_route_set(audio_route_t r)
{
    if (g_line < 0) return -1;
    struct gpio_v2_line_values v = {.bits = r == ROUTE_LINE_OUT, .mask = 1};
    return ioctl(g_line, GPIO_V2_LINE_SET_VALUES_IOCTL, &v) ? -1 : 0;
}
