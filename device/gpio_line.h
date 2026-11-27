/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* One output line of a GPIO chip through the character device (v2 ABI),
 * found by the chip's label: the amplifier's output select (audio_route.c,
 * in the player) and the USB-C controller's two configuration pins
 * (usbrole.c, a program of its own in the root filesystem) each asked for
 * theirs with the same lines. A header only: usbrole.c is built alone. */
#ifndef FATHOM_GPIO_LINE_H
#define FATHOM_GPIO_LINE_H

#include <errno.h>
#include <fcntl.h>
#include <linux/gpio.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* The line `line` of the chip labelled `chip_label`, as an output set to
 * `value`, named `consumer`: its descriptor; -1 if no such chip is there, -2
 * if the line was refused (errno says why). */
static inline int gpio_line_request(const char *chip_label, unsigned line, int value, const char *consumer)
{
    int out = -1;
    for (int i = 0; i < 8 && out < 0; i++) {
        char path[32];
        snprintf(path, sizeof path, "/dev/gpiochip%d", i);
        int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0) continue;
        struct gpiochip_info info;
        memset(&info, 0, sizeof info);
        if (ioctl(fd, GPIO_GET_CHIPINFO_IOCTL, &info) == 0 && !strcmp(info.label, chip_label)) {
            struct gpio_v2_line_request req;
            memset(&req, 0, sizeof req);
            req.offsets[0] = line;
            req.num_lines = 1;
            req.config.flags = GPIO_V2_LINE_FLAG_OUTPUT;
            req.config.num_attrs = 1;
            req.config.attrs[0].attr.id = GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES;
            req.config.attrs[0].attr.values = (unsigned)value;
            req.config.attrs[0].mask = 1;
            snprintf(req.consumer, sizeof req.consumer, "%s", consumer);
            if (ioctl(fd, GPIO_V2_GET_LINE_IOCTL, &req) == 0) out = req.fd;
            else {
                int why = errno;
                close(fd);
                errno = why;
                return -2;
            }
        }
        close(fd);
    }
    return out;
}

/* 0, or -1 */
static inline int gpio_line_set(int fd, int value)
{
    struct gpio_v2_line_values v = {.bits = (unsigned)value, .mask = 1};
    return ioctl(fd, GPIO_V2_LINE_SET_VALUES_IOCTL, &v) ? -1 : 0;
}

#endif
