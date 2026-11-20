/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Just enough of Linux's <linux/input.h> to build device/input.c on any host
 * (tests/test_input.c). The real codes; the ioctls are never made there. */
#ifndef FATHOM_FAKE_LINUX_INPUT_H
#define FATHOM_FAKE_LINUX_INPUT_H

#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/time.h>

struct input_event {
    struct timeval time;
    uint16_t type;
    uint16_t code;
    int32_t  value;
};

#define EV_KEY 0x01
#define EV_REL 0x02
#define EV_SW  0x05

#define SW_HEADPHONE_INSERT 0x02
#define SW_LINEOUT_INSERT   0x06
#define SW_MAX              0x10

#define KEY_POWER        116
#define KEY_VOLUMEDOWN   114
#define KEY_VOLUMEUP     115
#define KEY_PAUSE        119
#define KEY_MENU         139
#define KEY_BACK         158
#define KEY_NEXTSONG     163
#define KEY_PLAYPAUSE    164
#define KEY_PREVIOUSSONG 165
#define KEY_STOPCD       166
#define KEY_PLAYCD       200
#define KEY_PAUSECD      201
#define KEY_PLAY         207

#define EVIOCGNAME(len) (0x40004506ul | ((unsigned long)(len) << 16))
#define EVIOCGSW(len)   (0x4000451bul | ((unsigned long)(len) << 16))

#endif
