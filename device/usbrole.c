/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/*
 * fathom-usbrole: which role the H2's USB-C port offers, through the TCS1421's
 * CFG1:CFG0 pins (PB30, PD5): 00 = sink only, 11 = dual role (DRP).
 *
 * DRP is what finds a USB DAC: the chip then also offers to be the source, and
 * dwc2 becomes the host when a sink is attached. But a
 * computer's port is dual-role too, and two DRP ports pick their roles at
 * random: half the time the H2 would be the host, powering the computer. So:
 *
 *   - sink while someone else supplies VBUS (computer, charger), and at boot;
 *   - DRP once nobody has supplied VBUS for 2 s (cable out, or a DAC in);
 *     6 s after a computer or charger, so a quick replug finds us a sink and
 *     the computer the host at once (in DRP it's a coin toss);
 *   - host with a USB device on the port but no audio interface: it's a
 *     computer (decided as soon as the device's interfaces are all there,
 *     or after 4 s if it can't be placed, e.g. a hub). A Mac takes 1-2 s
 *     to show up as a device, and as long again to supply VBUS once we're a
 *     sink: we wait 8 s for it before trying DRP again. Back to sink, where it becomes the source and we the
 *     device, and stay there until the cable comes out;
 *   - host with nothing on the bus after 10 s (something that takes power
 *     but never shows up as a device: a broken DAC, a lamp, a charging
 *     case): VBUS off, back to sink, and DRP again only after 30 s, then
 *     60 s, then every 2 min while it stays that way (a battery drained
 *     into it otherwise).
 *
 * Inputs: the PMIC's VBUS-present bit (battery "online") and our own VBUS
 * regulator's state, so our own 5 V never counts as someone else's. Neither
 * tells anyone when it changes, so they're read once a second, and four
 * times a second while a device on the port is being placed.
 *
 * Messages go to syslog ("usbrole"; to stderr too when run from a terminal).
 */
#include <dirent.h>
#include <fcntl.h>
#include <linux/gpio.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include "core/clock.h"
#include "gpio_line.h"
#include "sysfile.h"

#define TICK_MS       250       /* while placing a device as the host */
#define IDLE_MS       1000      /* otherwise: nothing here needs to be quicker */
#define NO_VBUS_MS    2000      /* sink -> DRP */
#define AFTER_PC_MS   6000      /* the same, after a computer: a quick replug finds us a sink */
#define HANDOVER_MS   8000      /* after handing a computer the host role: time to supply VBUS */
#define NOT_AUDIO_MS  4000      /* host without an audio interface -> sink */
#define NO_DEVICE_MS  10000     /* host with nothing on the bus -> sink */
#define RETRY_MS      30000     /* ...and DRP again after this long, twice as long each time */
#define RETRY_MAX_MS  120000

static int g_cfg1 = -1, g_cfg0 = -1;
static char g_vbus_state[300];   /* /sys/class/regulator/regulator.N/state of "usb-vbus" */
static volatile sig_atomic_t g_quit;

/* (with the time since boot: the clock may not be set yet) */
static void say(const char *fmt, const char *a)
{
    char msg[300];
    snprintf(msg, sizeof msg, fmt, a);
    long t = (long)clock_ms();
    syslog(LOG_INFO, "[%ld.%02ld] %s", t / 1000, t % 1000 / 10, msg);
}

static void find_vbus_regulator(void)
{
    DIR *d = opendir("/sys/class/regulator");
    struct dirent *e;
    while (d && (e = readdir(d))) {
        char path[300], name[32];
        snprintf(path, sizeof path, "/sys/class/regulator/%s/name", e->d_name);
        if (sysfile_line(path, name, sizeof name) == 0 && !strncmp(name, "usb-vbus", 8))
            snprintf(g_vbus_state, sizeof g_vbus_state, "/sys/class/regulator/%s/state", e->d_name);
    }
    if (d) closedir(d);
}

/* We are the host: our own VBUS is on. */
static int we_source(void)
{
    char s[16];
    return g_vbus_state[0] && sysfile_line(g_vbus_state, s, sizeof s) == 0 && !strncmp(s, "enabled", 7);
}

/* Someone else supplies VBUS (the PMIC sees it while ours is off). */
static int other_source(void)
{
    char s[8];
    return !we_source() && sysfile_line("/sys/class/power_supply/battery/online", s, sizeof s) == 0 && s[0] == '1';
}

/* What is on the bus: 0 nothing (yet), 1 a device we can't place yet, 2 an
 * audio interface, 3 a device whose interfaces are all there, none of them
 * audio or a hub: not a DAC, so the other end is (most likely) a computer. */
static int bus_contents(void)
{
    DIR *d = opendir("/sys/bus/usb/devices");
    struct dirent *e;
    int devices = 0, want = 0, have = 0, audio = 0, hub = 0;
    char seen[200] = "";             /* for the log: ids and interface classes */
    size_t sl = 0;
    while (d && (e = readdir(d))) {
        if (!strncmp(e->d_name, "usb", 3) || e->d_name[0] == '.') continue;   /* root hubs */
        if (strstr(e->d_name, "-0:")) continue;                                /* and their interfaces */
        char path[300], v[8] = "";       /* (empty unless read: a device not configured yet has no count) */
        if (!strchr(e->d_name, ':')) {                                          /* a device */
            devices++;
            snprintf(path, sizeof path, "/sys/bus/usb/devices/%s/bNumInterfaces", e->d_name);
            if (sysfile_line(path, v, sizeof v) == 0) want += atoi(v);   /* blank until configured */
            char vid[8] = "?", pid[8] = "?";
            snprintf(path, sizeof path, "/sys/bus/usb/devices/%s/idVendor", e->d_name);
            sysfile_line(path, vid, sizeof vid);
            snprintf(path, sizeof path, "/sys/bus/usb/devices/%s/idProduct", e->d_name);
            sysfile_line(path, pid, sizeof pid);
            vid[strcspn(vid, "\n")] = pid[strcspn(pid, "\n")] = '\0';
            if (sl < sizeof seen) sl += (size_t)snprintf(seen + sl, sizeof seen - sl, " %s:%s(%d)", vid, pid, atoi(v));
            continue;
        }
        have++;
        snprintf(path, sizeof path, "/sys/bus/usb/devices/%s/bInterfaceClass", e->d_name);
        if (sysfile_line(path, v, sizeof v) == 0) {
            if (!strncmp(v, "01", 2)) audio = 1;
            if (!strncmp(v, "09", 2)) hub = 1;
            if (sl < sizeof seen) sl += (size_t)snprintf(seen + sl, sizeof seen - sl, " if%.2s", v);
        }
    }
    if (d) closedir(d);
    static char last[200];
    if (strcmp(seen, last)) {        /* when each device and interface shows up */
        snprintf(last, sizeof last, "%s", seen);
        say("bus:%s", seen[0] ? seen : " empty");
    }
    if (audio) return 2;
    if (!devices) return 0;
    return !hub && want > 0 && have >= want ? 3 : 1;
}

static void set_drp(int on)
{
    gpio_line_set(g_cfg1, on);
    gpio_line_set(g_cfg0, on);
    say("%s", on ? "dual role (looking for a USB DAC)" : "sink only");
}

static void on_signal(int s) { (void)s; g_quit = 1; }

int main(void)
{
    openlog("usbrole", isatty(2) ? LOG_PERROR : 0, LOG_DAEMON);
    /* Start as the bootloader leaves the chip: sink. */
    g_cfg1 = gpio_line_request("GPIOB", 30, 0, "tcs1421-cfg1");
    g_cfg0 = gpio_line_request("GPIOD", 5, 0, "tcs1421-cfg0");
    if (g_cfg1 < 0 || g_cfg0 < 0) { syslog(LOG_ERR, "CFG pins: %m"); return 1; }
    find_vbus_regulator();
    if (!g_vbus_state[0]) say("%s", "no usb-vbus regulator: host detection off");
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);

    int drp = 0, no_vbus_ms = 0, host_ms = 0;
    int wait_ms = NO_VBUS_MS;        /* how long without VBUS before DRP */
    int pc_seen = 0;                 /* in DRP, someone else supplied VBUS */
    int retry_ms = RETRY_MS;         /* DRP again after a device that never showed up */
    say("%s", "sink only");
    long last = (long)clock_ms();
    int tick = IDLE_MS;
    while (!g_quit) {
        struct timespec ts = {tick / 1000, (tick % 1000) * 1000000L};
        nanosleep(&ts, NULL);
        long t = (long)clock_ms();
        int dt = (int)(t - last);      /* (timers count what really went by) */
        last = t;
        tick = IDLE_MS;
        if (!drp) {
            if (other_source()) {        /* a computer or charger supplies VBUS */
                if (no_vbus_ms || wait_ms != AFTER_PC_MS) say("%s", "VBUS from the other side");
                no_vbus_ms = 0;
                wait_ms = AFTER_PC_MS;
                retry_ms = RETRY_MS;
                continue;
            }
            no_vbus_ms += dt;
            if (no_vbus_ms >= wait_ms) { set_drp(1); drp = 1; host_ms = 0; wait_ms = NO_VBUS_MS; }
            continue;
        }
        if (!we_source()) {
            host_ms = 0;
            if (other_source()) { if (!pc_seen) say("%s", "VBUS from the other side"); pc_seen = 1; }
            else if (pc_seen) {          /* that computer or charger is gone: sink, as after one */
                set_drp(0);
                drp = 0;
                no_vbus_ms = 0;
                wait_ms = AFTER_PC_MS;
                pc_seen = 0;
            }
            continue;
        }
        host_ms += dt;
        int bus = bus_contents();
        if (bus == 2) { host_ms = 0; retry_ms = RETRY_MS; continue; }   /* a DAC: stay the host */
        tick = TICK_MS;
        if (bus == 3 || (bus == 1 && host_ms >= NOT_AUDIO_MS)) { /* a computer, most likely */
            say("%s", "host, but no audio interface on the port: a computer?");
            set_drp(0);
            drp = 0;
            no_vbus_ms = 0;
            wait_ms = HANDOVER_MS;
            retry_ms = RETRY_MS;
        } else if (bus == 0 && host_ms >= NO_DEVICE_MS) {        /* powered, but nothing answers */
            char in[16];
            snprintf(in, sizeof in, "%d s", retry_ms / 1000);
            say("host, but no device on the port: VBUS off, dual role again in %s", in);
            set_drp(0);
            drp = 0;
            no_vbus_ms = 0;
            wait_ms = retry_ms;
            retry_ms = retry_ms * 2 > RETRY_MAX_MS ? RETRY_MAX_MS : retry_ms * 2;
        }
    }
    /* Leave the port as a sink: safe with anything plugged in. */
    set_drp(0);
    return 0;
}
