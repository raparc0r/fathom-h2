/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* The player on the H2: updates (Settings > About > Check for updates, the
 * quiet checks, what a start finds of an update's install) and Uninstall
 * Fathom: the player's side of updater.c and uninstaller.c (main_int.h). Out
 * of main.c, where it stood. */
#include "main_int.h"

/* ---- updates (Settings > About > Check for updates) ----
 * When asked, or a quiet check by itself three times a day with Check automatically
 * on (auto_update_poll); downloads and installs only ever when asked.
 * settings.ini keeps: update.auto (0/1), update.checked (the last answer's
 * time), update.found (a newer release seen, "" if none) and update.told (the
 * one the wake-up toast has announced: each is said once). */


static int g_update_asked;          /* a check from the screen: what it finds is seen there */

/* An install cut short by the battery going is the one that can leave the
 * card's files half moved (finished at the next start, but a cut inside one
 * of the card's writes can damage a file).
 * It takes seconds, and then a restart: not below this, unless on power. */
#define UPDATE_MIN_BATTERY 15       /* % */

void hook_update_do(int what)
{
    if (what == UI_UPD_INSTALL && !g_charging && g_bat_pct >= 0 && g_bat_pct < UPDATE_MIN_BATTERY) {
        ui_toast("Charge the player to 15 % or plug it in first", 3000);   /* (the update stays ready) */
        return;
    }
    if (what == UI_UPD_CHECK && !net_up()) {
        /* (not for an update that is on the card already: after "no room to
         * install", Try again fetches nothing, and with Wi-Fi off or away it
         * was answered with a Wi-Fi message, the update given up at the next
         * restart and downloaded again) */
        ui_update_t st;
        updater_ui_state(&st);
        if (!st.no_net) {
            updater_refuse(net_why());
            return;
        }
    }
    if (what == UI_UPD_CHECK) g_update_asked = 1;
    updater_do(what);
}

/* What a check found (tag: newer, or "" for up to date), and whether its owner has seen it. */
static void update_found(const char *tag, int seen)
{
    settings_set_int("update.checked", (int)time(NULL));
    settings_set("update.found", tag);
    if (seen && tag[0]) settings_set("update.told", tag);
    settings_save();
}

/* The screen turned on by Power: a newer release found by the automatic
 * check, once (on two lines: a long version number may not leave room for
 * the second half on the first in the wider themes). */
void update_announce(void)
{
    const char *tag = settings_get("update.found", "");
    if (!tag[0] || !strcmp(tag, settings_get("update.told", ""))) return;
    char t[160];
    tr_fill(t, sizeof t, "Fathom %s is available\nSettings > About", tag, NULL);
    ui_toast(t, 4000);
    fprintf(stderr, "update: told %s is available\n", tag);
    settings_set("update.told", tag);
    settings_save();
}

/* The player is up: an update's new kernel started (h2boot, bootloader/
 * fathom/bootpick.c, gives it two starts to get here before going back to the
 * one before); or it didn't, and the H2 went back: said once. */
/* An update's kernel started: its markers go as soon as the screen is up, not
 * after the library has opened. Bringing a big library up to date (a minute
 * and more, once, after an update) or a first scan came before, and forcing
 * the H2 off twice during it made the bootloader put the old kernel back
 * under the new player. */
void boot_started(void)
{
    static const char *const tried[] = {"boot-new", "boot-try1", "boot-try2"};
    char p[300];
    for (size_t i = 0; i < sizeof tried / sizeof tried[0]; i++) {
        snprintf(p, sizeof p, CARD_DIR "/fathom/%s", tried[i]);
        if (!unlink(p)) fprintf(stderr, "update: the new system started (%s)\n", tried[i]);
    }
}

/* The card can't be written (the kernel made it read-only after an error, or
 * its lock switch is on): nothing is saved, which would otherwise go unsaid.
 * Once a session. (/proc/mounts: statvfs would read the whole FAT first.) */
void card_writable_check(void)
{
    static int said;
    if (said) return;
    FILE *f = fopen("/proc/mounts", "re");
    if (!f) return;
    char dev[128], mnt[256], type[32], opts[256];
    int ro = 0;
    while (fscanf(f, "%127s %255s %31s %255s %*[^\n]", dev, mnt, type, opts) == 4)
        if (!strcmp(mnt, CARD_DIR)) ro = !strncmp(opts, "ro", 2) && (opts[2] == ',' || !opts[2]);
    fclose(f);
    if (!ro) return;
    said = 1;
    fprintf(stderr, "card: mounted read-only: nothing can be saved\n");
    ui_toast("The card can't be written, so nothing is saved. Check it on a computer.", 8000);
}

void boot_markers(void)
{
    char p[300];
    boot_started();
    snprintf(p, sizeof p, CARD_DIR "/fathom/boot-fell-back");
    if (!access(p, F_OK)) {
        fprintf(stderr, "update: the new system didn't start twice: h2boot went back to the one before\n");
        /* (only the kernel and its Wi-Fi modules went back: the player is the new one) */
        ui_toast("The new system didn't start, so the player kept the one before. Fathom itself is up to date.", 8000);
        unlink(p);
    }
}

/* Installed: the new kernel needs a restart, as power-off does it (init's
 * scripts stop the player, which closes the library). */
void update_poll(lv_timer_t *t)
{
    (void)t;
    packs_poll();
    static unsigned seen;
    unsigned s = updater_seq();
    if (s != seen) {
        seen = s;
        ui_update_changed();
        ui_update_t st;
        updater_ui_state(&st);
        if (g_update_asked && st.state == UI_UPD_AVAILABLE) { g_update_asked = 0; update_found(st.latest, 1); }
        else if (g_update_asked && st.state == UI_UPD_LATEST) { g_update_asked = 0; update_found("", 1); }
        else if (st.state == UI_UPD_FAILED || st.state == UI_UPD_IDLE) g_update_asked = 0;   /* (or cancelled) */
    }
    if (!updater_installed()) return;
    fprintf(stderr, "update: installed, restarting\n");
    g_powering_off = 1;                /* "Restarting" stays on the screen */
    save_state();
    screen_set(1);
    restart_now();
}

/* ---- Uninstall Fathom (Settings > About > Uninstall Fathom) ----
 * The battery is checked here, before the file is made ready and again before
 * the restart: HiBy's updater then rewrites the flash, and must not run out
 * half way. The restart goes straight into HiBy's recovery (h2boot honours
 * HiBy's own signal, uninstaller_ask_recovery); if the signal can't be set, the
 * H2 switches off and the toast says how to get there by hand. */

void hook_uninstall_do(int what)
{
    if ((what == UI_UNI_APPLY || what == UI_UNI_RESTART) && !g_charging && g_bat_pct >= 0 &&
        g_bat_pct < UNINSTALL_MIN_BATTERY) {
        ui_toast("Charge the player to 50 % or plug it in first", 3000);
        return;
    }
    if (what != UI_UNI_RESTART) {
        uninstaller_do(what);
        return;
    }
    if (uninstaller_ask_recovery()) {
        power_off("Hold <Vol+> as you turn the player on, then choose Boot select > HibyOS Recovery");
        return;
    }
    fprintf(stderr, "uninstall: restarting into HiBy's recovery\n");
    g_powering_off = 1;
    save_state();
    screen_set(1);
    ui_message_screen("Restarting", "HiBy's recovery puts its firmware back. Don't turn the player off.");
    restart_now();
}

void uninstall_poll(void)
{
    static unsigned seen;
    unsigned s = uninstaller_seq();
    if (s != seen) {
        seen = s;
        ui_uninstall_changed();
    }
}

/* ---- automatic update checks (Settings > About > Check automatically) ----
 * On unless the owner turns it off (UI_UPDATE_AUTO_DEFAULT). At most once a
 * day (a failed try again after an hour: update_auto_due), only on Wi-Fi
 * already joined and while the H2 rests: the screen off for a minute,
 * nothing playing unless it's charging, no download, network-drive transfer,
 * USB drive or DAC mode. It only asks GitHub: a newer release is said by a
 * toast the next time Power turns the screen on, and offered in Settings >
 * About. */

#define AUTO_UPDATE_REST_MS 60000

void auto_update_poll(void)
{
    static long tried;               /* (since this start: a restart tries again at once) */
    char tag[32];
    int r = updater_quiet_result(tag, sizeof tag);
    if (r) {
        fprintf(stderr, "update: checked by itself: %s\n", r < 0 ? "no answer" : tag[0] ? tag : "up to date");
        if (r > 0) update_found(tag, 0);
    }
    if (!settings_get_int("update.auto", UI_UPDATE_AUTO_DEFAULT) || g_screen_on || clock_ms32() - g_last_input < AUTO_UPDATE_REST_MS) return;
    if (!wifi_connected() || share_busy() || updater_busy() || packs_busy() || g_usb_drive || g_relay || g_charge_mode) return;
    player_now_t n = {0};
    if (g_player) player_now(g_player, &n);
    if (n.playing && !usb_powered()) return;
    long now = (long)time(NULL);
    if (!update_auto_due(now, settings_get_int("update.checked", 0), tried)) return;
    if (updater_check_quiet()) tried = now;
}
