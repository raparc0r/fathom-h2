/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* The player on the H2: the battery (its reading, its history, time left),
 * powering off and restarting with everything saved, and charging mode
 * (main_int.h). Out of main.c, where it stood. */
#include "main_int.h"

/* ---- battery history (core/batlog.c): Settings > Power > Battery ----
 * fathom/battery.log on the card: the charge and the state, noted every 10 s
 * (battery_log_poll), written every 10 minutes, at charger changes and when
 * the player stops. */
batlog_t g_batlog;
int g_batlog_on;

static void batlog_end(void) { if (g_batlog_on) batlog_off(&g_batlog, clock_s()); }   /* switching off, restarting */

int hook_battery_report(batrep_t *rep, int days)
{
    if (!g_batlog_on) return -1;
    batrec_t *r = NULL;
    int n = batlog_load(g_batlog.path, &g_batlog, clock_s(), &r);
    if (n > 0) batlog_report(r, n, days, rep);
    free(r);
    return n > 0 ? 0 : -1;
}


/* What a stop or a restart leaves on the card: the battery's log closed, the
 * clock, the settings and the queue. */
void save_state(void)
{
    batlog_end();
    clock_save();
    settings_save();
    queue_save();
}

/* The screen as it now is, everything on the card, and the restart. */
void restart_now(void)
{
    lv_refr_now(NULL);
    sync();
    if (system("/sbin/reboot") != 0) fprintf(stderr, "reboot failed\n");
}

void power_off(const char *why)
{
    if (g_powering_off) return;        /* (charging mode asks every second until init stops us) */
    fprintf(stderr, "power off: %s\n", why);
    g_powering_off = 1;
    screen_set(1);                     /* the message must be visible */
    ui_toast(why, 5000);
    lv_refr_now(NULL);                 /* ...and at once: the saves below write to the card, and Power is still held */
    save_state();
    if (system("/sbin/poweroff") != 0) {
        fprintf(stderr, "poweroff failed\n");
        g_powering_off = 0;            /* (asked again later, as before) */
        /* (still on: the battery's log, closed by its off record, opens again) */
        if (g_batlog_on) batlog_open(&g_batlog, CARD_DIR "/fathom/battery.log", time(NULL), clock_s());
    }
}

/* The stock firmware cuts power at 3.40 V. Power off a little above that, on
 * three readings in a row, so the player shuts down cleanly first. */


/* ---- battery: time left and charge cycles ----
 * The PMIC (AXP2101) gives the charge in percent and the voltage, not the
 * current. Time left is the rate the percentage has fallen at over the last
 * 90 minutes on battery; charge cycles are the percentage used up, counted by
 * Fathom ("battery.used": 100 = one cycle). The battery's health (its capacity
 * now) can't be measured without the current. */
#define BAT_HIST 32
static struct { uint32_t s; int pct; } g_bat_hist[BAT_HIST];   /* when each percentage was first seen */
static int g_bat_n, g_bat_last = -1;
int g_bat_pct = -1;

static void battery_track(int pct, int on_power)
{
    g_bat_pct = pct;
    if (on_power) { g_bat_n = 0; g_bat_last = -1; return; }
    if (g_bat_last >= 0 && pct < g_bat_last)
        settings_set_int("battery.used", settings_get_int("battery.used", 0) + (g_bat_last - pct));
    g_bat_last = pct;
    if (g_bat_n && g_bat_hist[(g_bat_n - 1) % BAT_HIST].pct == pct) return;
    g_bat_hist[g_bat_n % BAT_HIST].s = clock_ms32() / 1000;
    g_bat_hist[g_bat_n % BAT_HIST].pct = pct;
    g_bat_n++;
}

/* Seconds left on battery, or -1 while there's too little to go on. */
static long battery_left_s(void)
{
    if (g_bat_n < 3) return -1;                       /* two drops at least (the first sample is mid-step) */
    uint32_t now = clock_ms32() / 1000;
    int last = (g_bat_n - 1) % BAT_HIST, first = -1;
    for (int k = g_bat_n - 1; k >= 1 && k > g_bat_n - BAT_HIST; k--) {   /* the oldest drop within 90 min */
        int i = k % BAT_HIST;
        if (now - g_bat_hist[i].s > 90 * 60) break;
        first = i;
    }
    if (first < 0 || first == last) return -1;
    int drop = g_bat_hist[first].pct - g_bat_hist[last].pct;
    uint32_t dt = g_bat_hist[last].s - g_bat_hist[first].s;
    if (drop < 2 || dt < 600) return -1;
    return (long)((double)g_bat_pct * dt / drop);
}

void hook_battery_info(char *out, size_t cap)
{
    char pct[16], n[16];
    if (g_bat_pct < 0) { snprintf(out, cap, "-"); return; }
    snprintf(pct, sizeof pct, "%d %%", g_bat_pct);
    if (g_charging) { tr_fill(out, cap, g_bat_pct >= 100 ? "%s, full" : "%s, charging", pct, NULL); return; }
    long s = battery_left_s();
    if (s < 0) { snprintf(out, cap, "%s", pct); return; }
    snprintf(n, sizeof n, "%ld", s >= 2 * 3600 ? (s + 1800) / 3600 : (s + 300) / 600 * 10 > 10 ? (s + 300) / 600 * 10 : 10);
    tr_fill(out, cap, s >= 2 * 3600 ? "%s, about %s h left" : "%s, about %s min left", pct, n);
}

void poll_battery(lv_timer_t *t)
{
    (void)t;
    static int low_count;
    char cap[16], status[32], volt[24];
    if (sysfile_line(HW_BATTERY "capacity", cap, sizeof cap)) { ui_set_battery(-1, 0); return; }
    int have_status = !sysfile_line(HW_BATTERY "status", status, sizeof status);   /* (read once: it was read for each) */
    int charging = have_status && !strncmp(status, "Charging", 8);
    int pct = atoi(cap);
    ui_set_battery(pct, charging);
    /* on power: charging, or full (or held) while plugged in */
    g_charging = have_status && strncmp(status, "Discharging", 11) != 0;
    battery_track(pct, g_charging);

    /* low-battery warnings, once each per discharge */
    static int warned10, warned5;
    if (charging) warned10 = warned5 = 0;
    else if (pct <= 5 && !warned5) { warned5 = warned10 = 1; ui_toast("Battery at 5 %: charge soon", 4000); }
    else if (pct <= 10 && !warned10) { warned10 = 1; ui_toast("Battery at 10 %", 3000); }

    long uv = sysfile_line(HW_BATTERY "voltage_now", volt, sizeof volt) ? 0 : atol(volt);
    if (!charging && uv > 0 && uv < LOW_BATTERY_UV) {
        if (++low_count == 3) power_off("Battery empty: powering off");
    } else {
        low_count = 0;
    }
}

/* The battery history's note: the charge, and what drains it */
void battery_log_poll(void)
{
    if (!g_batlog_on || g_bat_pct < 0) return;
    player_now_t n = {0};
    if (g_player) player_now(g_player, &n);
    int out = g_usb_card >= 0 ? BL_OUT_USB : on_bt_now() ? BL_OUT_BT : g_jack_lo ? BL_OUT_LINE : BL_OUT_WIRED;
    int f = (g_charging ? BL_CHARGING : 0) | (g_screen_on ? BL_SCREEN : 0) | (n.playing ? BL_PLAYING : 0) |
            (wifi_available() && wifi_on() ? BL_WIFI : 0) | (bt_available() && bt_on() ? BL_BT : 0) | out << BL_OUT_SHIFT;
    batlog_note(&g_batlog, clock_s(), g_bat_pct, f);
}

/* ---- charging mode ----
 * Powered on by plugging in USB power (not the Power key), the H2 shows that
 * it's charging, then turns its screen off; nothing plays. Power starts it as
 * usual; other keys show the charge again; unplugged, it turns itself off.
 * The reason comes from the PMIC (power_on_reason, axp2101 driver), unless
 * the SoC was restarted (/tmp/fathom-reset, from S01bootlog: the PMIC still
 * says what powered it on originally); /tmp/fathom-power-on-reason overrides
 * both (tests). */
int g_charge_mode;
static lv_obj_t *g_charge_screen;
static uint32_t g_charge_shown;     /* when the screen was last shown */
#define CHARGE_SHOW_MS 6000

void power_on_reason(char *out, size_t cap)
{
    char reset[16] = "";
    snprintf(out, cap, "other");
    if (sysfile_line("/tmp/fathom-power-on-reason", out, cap) == 0) {
        /* tests */
    } else if (sysfile_line("/tmp/fathom-reset", reset, sizeof reset) == 0 && !strncmp(reset, "restart", 7)) {
        snprintf(out, cap, "restart");
    } else {
        DIR *d = opendir("/sys/bus/i2c/devices");
        struct dirent *e;
        char path[300];
        while (d && (e = readdir(d))) {
            snprintf(path, sizeof path, "/sys/bus/i2c/devices/%s/power_on_reason", e->d_name);
            if (sysfile_line(path, out, cap) == 0) break;
        }
        if (d) closedir(d);
    }
    out[strcspn(out, "\r\n")] = '\0';
}

void charge_screen_show(void)
{
    char pct[32] = "", body[160];
    sysfile_line(HW_BATTERY "capacity", pct, sizeof pct);
    pct[strcspn(pct, "\r\n")] = '\0';
    char pc[40];
    snprintf(pc, sizeof pc, "%s%%", pct[0] ? pct : "-");
    tr_fill(body, sizeof body, "%s\n\nPress <Power> to start", pc, NULL);
    if (g_charge_screen) lv_obj_delete(g_charge_screen);
    g_charge_screen = ui_message_screen("Charging", body);
    screen_set(1);
    g_charge_shown = clock_ms32();
}

/* Started for good in this boot (not in charging mode, or left it): a
 * restart of the player (after USB drive mode, the card back) mustn't go
 * back into charging mode because the PMIC still says USB powered it on. */

void started_mark(void)
{
    int fd = open(STARTED_MARK, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
    if (fd >= 0) close(fd);
}

void charge_mode_end(void)
{
    started_mark();
    g_charge_mode = 0;
    if (g_charge_screen) lv_obj_delete(g_charge_screen);
    g_charge_screen = NULL;
    screen_set(1);
}

/* Every second: the screen off after a while; off altogether when unplugged. */
void charge_poll(lv_timer_t *t)
{
    (void)t;
    if (!g_charge_mode) return;
    if (!g_charging) { power_off("Unplugged"); return; }
    if (g_screen_on && clock_ms32() - g_charge_shown > CHARGE_SHOW_MS) screen_set(0);
}
