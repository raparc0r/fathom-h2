/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/*
 * The H2's hardware as its kernel shows it: one name for each, for the player
 * and its helpers (cardout). A USB DAC or another card is found at run time.
 */
#ifndef FATHOM_HW_PATHS_H
#define FATHOM_HW_PATHS_H

/* The AXP2101's fuel gauge: capacity (percent), status, voltage_now (uV),
 * temp (tenths of a degree), online (1: on USB power) */
#define HW_BATTERY      "/sys/class/power_supply/battery/"
#define HW_USB_ONLINE   HW_BATTERY "online"
#define HW_BACKLIGHT    "/sys/class/backlight/backlight/"   /* brightness, max_brightness */
#define HW_DRM          "/dev/dri/card0"                    /* the screen */
#define HW_CTL          "/dev/snd/controlC0"                /* the H2's own DAC: its mixer */
#define HW_PCM          "/dev/snd/pcmC0D0p"                 /* and its playback */
/* Card n's (a USB DAC's): printf formats */
#define HW_CTL_N        "/dev/snd/controlC%d"
#define HW_PCM_N        "/dev/snd/pcmC%dD0p"
#define HW_USBID_N      "/proc/asound/card%d/usbid"

#endif
