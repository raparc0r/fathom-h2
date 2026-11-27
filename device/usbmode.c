/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* The player on the H2: a computer on the USB port. USB drive mode (the
 * card lent to the computer: the player leaves and comes back) and USB DAC
 * mode (the H2 as the computer's sound card, with its media keys)
 * (main_int.h). Out of main.c, where it stood. */
#include "main_int.h"

/* ---------- USB drive mode ----------
 * The player can't run without the card, so it hands over: it leaves a "USB
 * drive" screen up (the display keeps it) and exits with code 42; S90fathom
 * runs fathom-usbdisk, which lends the card to the computer until it's
 * ejected or unplugged, and then starts the player again, which picks up the
 * new music (/tmp/fathom-rescan). */
int g_usb_was;        /* connected to a computer at the last poll */

int usb_on_computer(void)
{
    /* The controller's name never changes: its folder is looked for until
     * found, then only its state is read (this is asked up to four times a
     * second, and listed the folder each time). */
    static char path[300];
    char state[32] = "";
    if (!path[0]) {
        DIR *d = opendir("/sys/class/udc");
        struct dirent *e;
        while (d && (e = readdir(d)))
            if (e->d_name[0] != '.') {
                snprintf(path, sizeof path, "/sys/class/udc/%s/state", e->d_name);
                break;
            }
        if (d) closedir(d);
    }
    if (path[0]) sysfile_line(path, state, sizeof state);
    return !strncmp(state, "configured", 10);
}

/* After Reset all settings: out, and S90fathom starts the player again
 * (2 s), which sets everything from the defaults. */
void restart_player(void)
{
    fprintf(stderr, "settings reset: starting again\n");
    g_quit = 1;
}

void usb_drive(void)
{
    if (!usb_on_computer()) { ui_toast("Connect the player to a computer first", 2500); return; }
    g_usb_drive = 1;
    g_quit = 1;
}

static uint32_t g_usb_quiet_until;    /* the gadget was just switched: not a new connection */
/* ...while this is set (until that time has come). The time alone, never set
 * or long past, read as still to come once the H2 had been on for 24.8 days
 * (a signed 32-bit difference): plugging into a computer then did nothing. */
static int g_usb_quiet;

void usb_poll(lv_timer_t *t)
{
    (void)t;
    int on = usb_on_computer();
    if (g_usb_quiet && (int32_t)(clock_ms32() - g_usb_quiet_until) >= 0) g_usb_quiet = 0;
    if (g_relay || g_usb_quiet) on = g_usb_was = on || g_usb_was;
    if (on && !g_usb_was) {        /* just plugged into a computer */
        fprintf(stderr, "usb: on a computer [%u.%03u]\n", clock_ms32() / 1000, clock_ms32() % 1000);
        int what = settings_get_int("usb.computer", UI_USB_ASK);
        if (what == UI_USB_DRIVE) {
            usb_drive();
        } else if (what == UI_USB_DAC) {
            usb_dac_mode();
        } else if (what == UI_USB_ASK) {
            screen_set(1);
            screen_activity();
            ui_usb_prompt();
        }
    }
    if (!on && g_usb_was) ui_usb_prompt_close();   /* unplugged before answering */
    g_usb_was = on;
}

/* ---------- USB DAC mode ----------
 * The H2 becomes the computer's sound card: fathom-usbgadget puts a USB
 * audio input in the USB drive's place, and the relay (uac_relay.c) copies
 * what the computer plays to the DAC, bit for bit, at the H2's own volume
 * (headphones or line out as usual). The music pauses and the player lets go
 * of the DAC. Back ends it, and so does unplugging the cable. */
static lv_obj_t *g_dac_screen;
static int g_dac_gone;                /* seconds the cable has been out */
static char g_dac_text[200];

static int usb_gadget(const char *mode)
{
    char cmd[64];
    snprintf(cmd, sizeof cmd, "fathom-usbgadget %s", mode);
    int rc = system(cmd);
    return rc == 0 ? 0 : -1;
}

void usb_dac_mode(void)
{
    if (g_relay || !g_eng) return;
    if (!usb_on_computer()) { ui_toast("Connect the player to a computer first", 2500); return; }
    if (g_player) player_pause(g_player, 1);
    /* the engine lets go of the DAC (it can't open this) until the mode ends */
    engine_switch_output(g_eng, out_router_alsa, (void *)"/dev/snd/usb-dac-mode");
    route_apply(0);                    /* (from Bluetooth the H2's DAC was left muted) */
    g_usb_quiet_until = clock_ms32() + 15000;
    g_usb_quiet = 1;
    if (usb_gadget("dac")) {
        output_home();
        ui_toast("USB DAC mode isn't available", 2500);
        return;
    }
    g_relay = uac_relay_start(HW_PCM);
    if (!g_relay) {                    /* (out of memory): all the way back */
        usb_gadget("normal");
        output_home();
        ui_toast("USB DAC mode couldn't start", 2500);
        return;
    }
    g_dac_text[0] = '\0';
    g_dac_screen = ui_dac_screen();
    g_dac_gone = 0;
    screen_set(1);
    fprintf(stderr, "usb dac mode: on\n");
}

/* Media keys for the computer (fathom-usbgadget's HID function): one report
 * with the key's bit, then one with none, as a keyboard sends them. */
static int g_hid = -1;

static int hid_send(unsigned char r)
{
    struct pollfd pf = {.fd = g_hid, .events = POLLOUT};
    if (poll(&pf, 1, 50) <= 0) return -1;     /* the host collects a report every ms */
    return write(g_hid, &r, 1) == 1 ? 0 : -1;
}

void usb_dac_key(int key)
{
    if (g_hid < 0) g_hid = open("/dev/hidg0", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (g_hid < 0) { ui_toast("Media keys aren't available", 1500); return; }
    if (hid_send((unsigned char)key) || hid_send(0)) {
        ui_toast("The computer didn't take the key", 1500);
        return;
    }
    ui_toast(key == HID_PLAY ? "Play / pause" : key == HID_NEXT ? "Next" : "Previous", 800);
}

void usb_dac_mode_end(const char *why)
{
    if (!g_relay) return;
    uac_relay_status_t st;
    uac_relay_status(g_relay, &st);
    uac_relay_stop(g_relay);
    g_relay = NULL;
    if (g_hid >= 0) close(g_hid);
    g_hid = -1;
    fprintf(stderr, "usb dac mode: off (%s); %llu frames, %u overruns\n", why, st.frames, st.overruns);
    g_usb_quiet_until = clock_ms32() + 15000;
    g_usb_quiet = 1;
    usb_gadget("normal");
    output_home();
    if (g_dac_screen) lv_obj_delete(g_dac_screen);
    g_dac_screen = NULL;
    screen_set(1);
    ui_toast(why, 2500);
}

/* Twice a second: what's playing, and whether the cable is still in. */
void usb_dac_mode_poll(lv_timer_t *t)
{
    (void)t;
    if (!g_relay) return;
    uac_relay_status_t st;
    uac_relay_status(g_relay, &st);
    char fmt[32] = "", note[24] = "", text[200];
    int exact = 0;
    if (st.playing) {
        /* The DAC takes 24 bits: a 16- or 24-bit stream reaches it untouched; a
         * 32-bit one (an app's float output) loses its lowest 8 bits. */
        char depth[16] = "";
        if (st.bits) snprintf(depth, sizeof depth, "%d-bit ", st.bits);
        snprintf(fmt, sizeof fmt, "%s%u.%u kHz", depth, st.rate / 1000, st.rate % 1000 / 100);
        exact = st.bits && st.bits <= 24;
        snprintf(note, sizeof note, "%s", !st.bits ? "" : exact ? "Bit-perfect" : "Played at 24-bit");
    }
    snprintf(text, sizeof text, "%s|%s|%s", fmt, note,
             st.playing ? "Keys control the computer\nBack to stop" : "On the computer, choose \"Fathom Audio\" as the sound output.\n\nBack to stop");
    if (strcmp(text, g_dac_text) && g_dac_screen) {
        snprintf(g_dac_text, sizeof g_dac_text, "%s", text);
        ui_dac_screen_set(g_dac_screen, fmt, note, exact, strrchr(text, '|') + 1);
    }
    /* No USB power for 3 s: unplugged. (Not the link's state: it isn't
     * "configured" until the computer takes the new device, and a Mac first
     * asks whether to allow it; it's also suspended while the computer sleeps.) */
    static uint32_t last;
    uint32_t now = clock_ms32();
    if (now - last < 1000) return;
    last = now;
    g_dac_gone = !usb_powered() ? g_dac_gone + 1 : 0;
    if (g_dac_gone >= 3) usb_dac_mode_end("Unplugged: USB DAC mode ended");
}

/* A file in /tmp left by fathom-usbdisk, read and removed; 0 if there was one. */
int take_tmp_file(const char *path, char *out, size_t cap)
{
    FILE *f = fopen(path, "re");
    if (!f) return -1;
    if (out && cap) {
        out[0] = '\0';
        if (fgets(out, (int)cap, f)) out[strcspn(out, "\n")] = '\0';
    }
    fclose(f);
    unlink(path);
    return 0;
}

/* What fathom-usbdisk says when it can't lend the card, here for the
 * translations' list (the toast translates them); one it adds later shows
 * as it wrote it. */
const char *usbdisk_text(const char *msg)
{
    static const char *const known[] = {N_("USB drive mode isn't available"), N_("The card is in use: try again"),
                                        N_("Couldn't start USB drive mode")};
    for (size_t i = 0; i < sizeof known / sizeof known[0]; i++)
        if (!strcmp(msg, known[i])) return known[i];
    return msg;
}
