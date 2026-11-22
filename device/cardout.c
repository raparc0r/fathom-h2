/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/*
 * fathom-cardout: the screen while the microSD card is out. fathom-sdcard
 * starts it once the player has gone (the player, its fonts and everything it
 * would draw with are on the card) and stops it when the card is back.
 *
 * It draws the picture in card_out.h ("No/bad card /
 * Put in a card with Fathom on it."), for a card pulled out and one that
 * can't be used alike, lights the screen, and then sleeps in poll() on the
 * buttons: no timer runs unless something is due.
 *   - the screen goes dark after LIT_MS without a button, as the player's
 *     would; any button lights it again, a short press of Power toggles it;
 *   - Power held for POWER_OFF_MS switches the H2 off, as in the player;
 *   - after OFF_MS with no card and no button, on battery, the H2 switches
 *     itself off (on USB power it waits on).
 * On SIGTERM (the card is back) the text goes and the drop stays, on a lit
 * screen, until the player's start-up screen (the same drop, with ripples).
 * The panel keeps the last picture when the DRM device is closed (kernel
 * patch 0013), so nothing flashes black in between.
 *
 * Messages go to syslog ("cardout").
 */
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include "card_out.h"
#include "hw_paths.h"

#define LIT_MS        60000            /* the screen stays lit this long after a button */
#define POWER_OFF_MS  2000             /* hold Power this long to switch off */
#define OFF_MS        (10 * 60000)     /* on battery, switch off after this long */
#define MAX_INPUTS 8

static volatile sig_atomic_t g_quit;
static int g_fd = -1, g_w, g_h;
static uint32_t g_fb, g_pitch;
static uint8_t *g_map;
static size_t g_map_size;

static void on_signal(int s) { (void)s; g_quit = 1; }

static long now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static int read_int(const char *path)
{
    char s[16] = "";
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t n = read(fd, s, sizeof s - 1);
    close(fd);
    return n > 0 ? (int)strtol(s, NULL, 10) : -1;
}

static void backlight(int v)
{
    char s[16];
    int fd = open(HW_BACKLIGHT "brightness", O_WRONLY | O_CLOEXEC);
    if (fd < 0) return;
    int n = snprintf(s, sizeof s, "%d\n", v);
    if (write(fd, s, (size_t)n) < 0) syslog(LOG_ERR, "backlight: %m");
    close(fd);
}

static uint16_t rgb565(uint32_t c) { return (uint16_t)((c >> 8 & 0xF800) | (c >> 5 & 0x07E0) | (c >> 3 & 0x001F)); }

static uint32_t blend(uint32_t bg, uint32_t fg, int lvl)
{
    uint32_t out = 0;
    for (int sh = 0; sh <= 16; sh += 8) {
        int b = (int)(bg >> sh & 0xFF), f = (int)(fg >> sh & 0xFF);
        out |= (uint32_t)((b * (CARD_OUT_LEVELS - lvl) + f * lvl + CARD_OUT_LEVELS / 2) / CARD_OUT_LEVELS) << sh;
    }
    return out;
}

static void flush(void)
{
    struct drm_clip_rect clip = {0, 0, (uint16_t)g_w, (uint16_t)g_h};
    struct drm_mode_fb_dirty_cmd dirty = {.fb_id = g_fb, .num_clips = 1, .clips_ptr = (uintptr_t)&clip};
    ioctl(g_fd, DRM_IOCTL_MODE_DIRTYFB, &dirty);   /* synchronous: the panel has it when this returns */
}

/* The picture's rows up to `rows`, the background below (centred if the
 * screen isn't 320x240). */
static void draw(int rows)
{
    uint16_t bg = rgb565(CARD_OUT_BG), pal[CARD_OUT_LEVELS + 1];
    for (int y = 0; y < g_h; y++) {
        uint16_t *p = (uint16_t *)(g_map + (size_t)y * g_pitch);
        for (int x = 0; x < g_w; x++) p[x] = bg;
    }
    int ox = (g_w - CARD_OUT_W) / 2, oy = (g_h - CARD_OUT_H) / 2, band = -1;
    size_t i = 0;
    for (int y = 0; y < CARD_OUT_H && y < rows; y++) {
        int b = 0;
        while (card_out_bands[b].end <= y) b++;
        if (b != band) {
            band = b;
            for (int l = 0; l <= CARD_OUT_LEVELS; l++) pal[l] = rgb565(blend(CARD_OUT_BG, card_out_bands[b].rgb, l));
        }
        for (int x = 0; x < CARD_OUT_W && i < sizeof card_out_runs;) {
            unsigned r = card_out_runs[i++];
            int lvl = r & 0x80 ? (int)(r >> 3 & 15) : 0, n = r & 0x80 ? (int)(r & 7) + 1 : (int)r + 1;
            int X = ox + x, Y = oy + y;
            if (Y >= 0 && Y < g_h)
                for (int k = 0; k < n; k++)
                    if (X + k >= 0 && X + k < g_w) ((uint16_t *)(g_map + (size_t)Y * g_pitch))[X + k] = pal[lvl];
            x += n;
        }
    }
}

/* A dumb buffer on the first connector and CRTC, as the player's disp_drm.c.
 * Fails while another program (the player, on its way out) is the DRM master. */
static int screen_open(void)
{
    g_fd = open(HW_DRM, O_RDWR | O_CLOEXEC);
    if (g_fd < 0) return -1;
    uint32_t conn[4], crtc[4];
    struct drm_mode_card_res res = {0};
    if (ioctl(g_fd, DRM_IOCTL_MODE_GETRESOURCES, &res) || !res.count_connectors || !res.count_crtcs) goto fail;
    res.count_connectors = res.count_connectors > 4 ? 4 : res.count_connectors;
    res.count_crtcs = res.count_crtcs > 4 ? 4 : res.count_crtcs;
    res.count_encoders = res.count_fbs = 0;
    res.connector_id_ptr = (uintptr_t)conn;
    res.crtc_id_ptr = (uintptr_t)crtc;
    if (ioctl(g_fd, DRM_IOCTL_MODE_GETRESOURCES, &res)) goto fail;
    struct drm_mode_modeinfo modes[4];
    struct drm_mode_get_connector c = {.connector_id = conn[0]};
    if (ioctl(g_fd, DRM_IOCTL_MODE_GETCONNECTOR, &c) || !c.count_modes) goto fail;
    c.count_modes = c.count_modes > 4 ? 4 : c.count_modes;
    c.modes_ptr = (uintptr_t)modes;
    c.count_props = c.count_encoders = 0;
    if (ioctl(g_fd, DRM_IOCTL_MODE_GETCONNECTOR, &c)) goto fail;
    g_w = modes[0].hdisplay;
    g_h = modes[0].vdisplay;
    struct drm_mode_create_dumb cd = {.width = (uint32_t)g_w, .height = (uint32_t)g_h, .bpp = 16};
    if (ioctl(g_fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd)) goto fail;
    g_pitch = cd.pitch;
    struct drm_mode_fb_cmd fb = {.width = (uint32_t)g_w, .height = (uint32_t)g_h, .pitch = cd.pitch,
                                 .bpp = 16, .depth = 16, .handle = cd.handle};
    if (ioctl(g_fd, DRM_IOCTL_MODE_ADDFB, &fb)) goto fail;
    g_fb = fb.fb_id;
    struct drm_mode_map_dumb md = {.handle = cd.handle};
    if (ioctl(g_fd, DRM_IOCTL_MODE_MAP_DUMB, &md)) goto fail;
    g_map = mmap(NULL, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, g_fd, (off_t)md.offset);
    if (g_map == MAP_FAILED) { g_map = NULL; goto fail; }
    g_map_size = cd.size;
    draw(CARD_OUT_H);                   /* (in the buffer before the CRTC shows it) */
    struct drm_mode_crtc set = {.crtc_id = crtc[0], .fb_id = g_fb, .set_connectors_ptr = (uintptr_t)&conn[0],
                                .count_connectors = 1, .mode = modes[0], .mode_valid = 1};
    if (ioctl(g_fd, DRM_IOCTL_MODE_SETCRTC, &set)) goto fail;
    flush();
    return 0;
fail:
    /* (the buffer goes once nothing holds it: the mapping does too, and
     * this is retried up to 40 times while the player lets go of the screen) */
    if (g_map) munmap(g_map, g_map_size);
    close(g_fd);
    g_fd = -1;
    g_map = NULL;
    return -1;
}

static void power_off(const char *why)
{
    syslog(LOG_INFO, "%s: switching off", why);
    backlight(0);
    execl("/sbin/poweroff", "poweroff", (char *)NULL);
    syslog(LOG_ERR, "poweroff: %m");
}

int main(void)
{
    openlog("cardout", isatty(2) ? LOG_PERROR : 0, LOG_DAEMON);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;          /* (no SA_RESTART: poll() returns at once) */
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    /* the player may still be closing the screen: up to 10 s */
    for (int i = 0; i < 40 && !g_quit && screen_open(); i++) usleep(250000);
    if (g_fd < 0) syslog(LOG_ERR, "no screen: only the backlight says so");

    /* lit as the player had it, or at about its default if it was dark */
    int max = read_int(HW_BACKLIGHT "max_brightness"), level = read_int(HW_BACKLIGHT "brightness");
    if (max <= 0) max = 255;
    if (level <= 0) level = max * 100 / 255;
    backlight(level);

    struct pollfd in[MAX_INPUTS];
    int n_in = 0;
    for (int i = 0; i < 16 && n_in < MAX_INPUTS; i++) {
        char path[32];
        snprintf(path, sizeof path, "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd >= 0) in[n_in++] = (struct pollfd){.fd = fd, .events = POLLIN};
    }

    long now = now_ms(), lit_until = now + LIT_MS, off_at = now + OFF_MS, power_since = -1;
    int lit = 1;
    while (!g_quit) {
        long next = off_at;
        if (lit && lit_until < next) next = lit_until;
        if (power_since >= 0 && power_since + POWER_OFF_MS < next) next = power_since + POWER_OFF_MS;
        int wait = next > now ? (int)(next - now) : 0;
        poll(in, (nfds_t)n_in, wait);
        now = now_ms();
        int wake = 0, toggle = 0;
        for (int i = 0; i < n_in; i++) {
            struct input_event ev;
            while (read(in[i].fd, &ev, sizeof ev) == (ssize_t)sizeof ev) {
                if (ev.type == EV_KEY && ev.code == KEY_POWER) {
                    if (ev.value == 1) power_since = now;
                    else if (ev.value == 0 && power_since >= 0) { power_since = -1; toggle = 1; }
                } else if ((ev.type == EV_KEY && ev.value == 1) || (ev.type == EV_REL && ev.value)) {
                    wake = 1;
                }
            }
        }
        if (g_quit) break;
        if (power_since >= 0 && now - power_since >= POWER_OFF_MS) {
            power_off("Power held");
            power_since = -1;           /* (only if it failed) */
        }
        if (toggle) wake = !lit;
        if (toggle && lit) { lit = 0; backlight(0); }
        if (wake) {
            if (!lit) backlight(level);
            lit = 1;
            lit_until = now + LIT_MS;
            off_at = now + OFF_MS;
        }
        if (lit && now >= lit_until) { lit = 0; backlight(0); }
        if (now >= off_at) {
            if (read_int(HW_USB_ONLINE) == 1) off_at = now + OFF_MS;   /* on USB power */
            else { power_off("No card for 10 minutes"); off_at = now + OFF_MS; }
        }
    }
    /* The card is back: the drop alone, lit, until the player draws */
    if (g_fd >= 0) { draw(card_out_bands[0].end); flush(); }
    backlight(level);
    return 0;
}
