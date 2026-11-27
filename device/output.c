/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* The player on the H2: where the sound goes and how it is shaped. The
 * volume and the jacks, USB DACs, Transport mode, Bluetooth as an output,
 * the EQ, the balance, the stereo width, crossfeed and the volume limit (each
 * kept per output), Pure mode and Settings > Sound > Output's signal path
 * (main_int.h). Out of main.c, where it stood. */
#include "main_int.h"
#include "audio/gain.h"
#include "ui/sound_text.h"

/* ---- volume and output routing ----
 *
 * Volume is the DAC's hardware attenuator (0.5 dB steps), 1 dB per key press,
 * on headphones only. Line out is fixed at 0 dB, the standard line level, and
 * only once the line-out detect reports a device: the detect works only while
 * that path is selected and the amplifier is on, so with no headphones the
 * switch goes to line out but the DAC stays muted until something answers.
 */

static mixer_t g_mix = {.fd = -1};
static char g_vol_path[512];
static int g_user_vol = -1;            /* headphone volume (raw mixer value) */
static int g_have_route;
static audio_route_t g_route = ROUTE_HEADPHONES;
static int g_jack_hp;
int g_jack_lo;

int g_usb_card = -1;                    /* card number while a USB DAC is the output */

static int vol_limit(void);
static int hp_cap(void);
static int bt_limit_pct(void);
static int bt_cap(void);

static void volume_show(int raw)
{
    char t[32];
    int tenths = (raw - g_mix.max) * 5;                 /* 0.5 dB per step below max */
    if (raw <= g_mix.min) snprintf(t, sizeof t, "%s", tr("muted"));
    else snprintf(t, sizeof t, "%s%d.%d dB%s", tenths < 0 ? "-" : "", abs(tenths) / 10, abs(tenths) % 10,
                  vol_limit() && raw >= hp_cap() ? tr(" (limit)") : "");
    ui_level("Volume", raw - g_mix.min, g_mix.max - g_mix.min, t);
}

/* Route by the jacks and set the level for that route; pop-free order. */
void route_apply(int announce)
{
    if (g_mix.fd < 0) return;
    if (g_usb_card >= 0 || out_router_on_bt()) { mixer_set(&g_mix, g_mix.min); return; }   /* a USB DAC or Bluetooth plays */
    audio_route_t want = g_jack_hp || !g_have_route ? ROUTE_HEADPHONES : ROUTE_LINE_OUT;
    if (want != g_route) {
        mixer_set(&g_mix, g_mix.min);                   /* mute while switching */
        audio_route_set(want);
        g_route = want;
        struct timespec ts = {0, 20 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
    if (g_route == ROUTE_HEADPHONES) mixer_set(&g_mix, g_user_vol > hp_cap() ? hp_cap() : g_user_vol);
    else mixer_set(&g_mix, g_jack_lo ? g_mix.max : g_mix.min);
    ui_set_output(g_route == ROUTE_HEADPHONES ? "Headphones" : "Line out");
    if (announce)
        ui_toast(g_route == ROUTE_HEADPHONES ? "Headphones" : "Line out", 1200);
}

void volume_init(const char *data)
{
    snprintf(g_vol_path, sizeof g_vol_path, "%s/volume", data);
    if (mixer_open(&g_mix, NULL, "Master Playback Volume")) { fprintf(stderr, "volume: no mixer control\n"); return; }
    g_user_vol = mixer_get(&g_mix);                     /* the driver's safe default */
    FILE *f = fopen(g_vol_path, "re");
    int v;
    if (f && fscanf(f, "%d", &v) == 1 && v >= g_mix.min && v <= g_mix.max) g_user_vol = v;
    if (f) fclose(f);
    input_jacks(&g_jack_hp, &g_jack_lo);
    g_have_route = audio_route_open(g_jack_hp ? ROUTE_HEADPHONES : ROUTE_LINE_OUT) == 0;
    if (!g_have_route) fprintf(stderr, "audio route: no PB5, headphones only\n");
    g_route = g_jack_hp || !g_have_route ? ROUTE_HEADPHONES : ROUTE_LINE_OUT;
    if (g_route == ROUTE_LINE_OUT) mixer_set(&g_mix, g_mix.min);
    route_apply(0);
}

static void usb_volume_step(int dir);

/* The headphones' top level under the volume limit (the mixer: 0.5 dB steps). */
static int hp_cap(void)
{
    int c = g_mix.max + vol_limit() * 2;
    return c < g_mix.min ? g_mix.min : c;
}

void volume_step(int dir)
{
    if (out_router_on_bt()) {               /* the headphones' own volume (AVRCP), or bluez-alsa's */
        int v = bt_volume_step(dir);
        char t[48];
        snprintf(t, sizeof t, "%d %%%s", (v * 100 + 63) / 127, bt_limit_pct() < 100 && v >= bt_cap() ? tr(" (limit)") : "");
        ui_level("Volume", v, 127, t);
        return;
    }
    if (g_usb_card >= 0) { usb_volume_step(dir); return; }
    if (g_mix.fd < 0) { ui_toast("No volume control", 1200); return; }
    if (g_route == ROUTE_LINE_OUT) { ui_toast("Line out: fixed at 0 dB", 1500); return; }
    int cap = hp_cap(), cur = g_user_vol > cap ? cap : g_user_vol;
    int want = cur + dir * 2;
    int v = mixer_set(&g_mix, want > cap ? cap : want);
    if (v < 0) return;
    g_user_vol = v;
    volume_show(v);
    char rec[24];
    snprintf(rec, sizeof rec, "%d", v);
    file_put_small(g_vol_path, rec, 0);                 /* (in place: no entry for a cut to leave dangling) */
}

void jacks_changed(int hp, int lo)
{
    int hp_changed = hp != g_jack_hp;
    if (g_jack_hp && !hp && g_usb_card < 0 && !out_router_on_bt() && g_player &&
        settings_get_int("playback.pause_on_unplug", UI_PAUSE_ON_UNPLUG_DEFAULT))
        player_pause(g_player, 1);   /* headphones out: pause */
    g_jack_hp = hp;
    g_jack_lo = lo;
    route_apply(hp_changed);    /* line-out detect flips with the amp; only announce plugging */
}


/* ---- USB DACs ----
 *
 * While a USB audio card is present, playback goes to it and the H2's own DAC
 * stays muted. Volume is the DAC's own control (bit-perfect), 1 dB per key
 * press, remembered per DAC (DATA/volume-usb-VVVV-PPPP, in 1/100 dB). A DAC
 * seen for the first time starts at -40 dB. A DAC with no volume control gets
 * the engine's gain instead (no longer bit-perfect, but never full scale by
 * surprise). Unplugging it pauses playback and goes back to the H2's output.
 */

#define USB_FIRST_CDB (-4000L)

engine_t *g_eng;                /* NULL unless the output is ALSA (switchable) */
static mixer_t g_usb_mix = {.fd = -1};
static char g_usb_vol_path[600], g_usb_name[80], g_usb_dev[32];
static long g_usb_cdb, g_usb_lo, g_usb_hi;
static int g_usb_soft;                 /* no volume control: engine gain */

/* The first USB audio card with a playback device: its number, and VID:PID. */
static int usb_card_find(char *usbid, size_t cap)
{
    for (int n = 1; n < 8; n++) {
        char path[64], pcm[32];
        snprintf(path, sizeof path, HW_USBID_N, n);
        snprintf(pcm, sizeof pcm, HW_PCM_N, n);
        if (access(pcm, W_OK) || sysfile_line(path, usbid, cap)) continue;
        usbid[strcspn(usbid, "\r\n")] = 0;
        return n;
    }
    return -1;
}

static void usb_card_name(int n, char *out, size_t cap)
{
    char ctl[32];
    snprintf(ctl, sizeof ctl, HW_CTL_N, n);
    snprintf(out, cap, "USB DAC");
    int fd = open(ctl, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;
    struct snd_ctl_card_info ci;
    memset(&ci, 0, sizeof ci);
    if (ioctl(fd, SNDRV_CTL_IOCTL_CARD_INFO, &ci) == 0 && ci.name[0]) snprintf(out, cap, "%s", (const char *)ci.name);
    close(fd);
}

static void usb_volume_apply(void)
{
    if (g_usb_soft) engine_set_gain(g_eng, (float)pow(10.0, g_usb_cdb / 2000.0));
    else mixer_set(&g_usb_mix, mixer_value_for_db(&g_usb_mix, g_usb_cdb));
}

/* A USB DAC's top level under the volume limit, in 1/100 dB. */
static long usb_cap(void)
{
    long c = g_usb_hi + vol_limit() * 100L;
    return c < g_usb_lo ? g_usb_lo : c;
}

static void usb_volume_step(int dir)
{
    long v = g_usb_cdb + dir * 100L, cap = usb_cap();
    g_usb_cdb = v < g_usb_lo ? g_usb_lo : v > cap ? cap : v;
    usb_volume_apply();
    char t[64];
    long a = labs(g_usb_cdb);
    snprintf(t, sizeof t, "%s%ld.%ld dB%s", g_usb_cdb < 0 ? "-" : "", a / 100, (a % 100) / 10,
             vol_limit() && g_usb_cdb >= cap ? tr(" (limit)") : "");
    ui_level("Volume", (int)(g_usb_cdb - g_usb_lo), (int)(g_usb_hi - g_usb_lo), t);
    char rec[24];
    snprintf(rec, sizeof rec, "%ld", g_usb_cdb);
    file_put_small(g_usb_vol_path, rec, 0);
}

/* ---- Transport mode ----
 * With a USB DAC as the output and Settings > Sound > Transport mode on, the
 * H2 only feeds the DAC: Bluetooth stops (and starts again when the DAC goes,
 * if it was on) and the screen turns off 10 s after the last key. (The CPU
 * stays at its one speed: the kernel has no frequency scaling for the X1000.) */
int g_transport;              /* on now */
static int g_transport_bt;           /* Bluetooth was on: on again after */

static void transport_update(void)
{
    int want = g_usb_card >= 0 && settings_get_int("sound.transport", UI_TRANSPORT_DEFAULT);
    if (want == g_transport) return;
    g_transport = want;
    if (want) {
        g_transport_bt = bt_available() && bt_on();
        if (g_transport_bt) bt_set(0);
        fprintf(stderr, "transport mode: on%s\n", g_transport_bt ? ", Bluetooth off" : "");
    } else {
        if (g_transport_bt) bt_set(1);
        g_transport_bt = 0;
        fprintf(stderr, "transport mode: off\n");
    }
}

void hook_transport_toggle(void)
{
    int on = !settings_get_int("sound.transport", UI_TRANSPORT_DEFAULT);
    settings_set_int("sound.transport", on);
    transport_update();
    if (on) ui_toast(g_transport ? "Transport mode: Bluetooth off, screen off after 10 s"
                                 : "With a USB DAC: Bluetooth off, screen off after 10 s", 2500);
}

static void usb_dac_attach(int n, const char *usbid)
{
    char vid[8] = "", pid[8] = "";
    sscanf(usbid, "%7[0-9a-fA-F]:%7[0-9a-fA-F]", vid, pid);
    usb_card_name(n, g_usb_name, sizeof g_usb_name);
    snprintf(g_usb_vol_path, sizeof g_usb_vol_path, "%s/volume-usb-%s-%s", g_data_dir, vid, pid);

    char ctl[32], name[44];
    snprintf(ctl, sizeof ctl, HW_CTL_N, n);
    g_usb_soft = mixer_find_playback_volume(ctl, name) || mixer_open(&g_usb_mix, ctl, name) ||
                 mixer_db_range(&g_usb_mix, &g_usb_lo, &g_usb_hi);
    if (g_usb_soft) { mixer_close(&g_usb_mix); g_usb_lo = -6000; g_usb_hi = 0; }
    g_usb_cdb = USB_FIRST_CDB;
    FILE *f = fopen(g_usb_vol_path, "re");
    long v;
    if (f && fscanf(f, "%ld", &v) == 1) g_usb_cdb = v;
    if (f) fclose(f);
    if (g_usb_cdb < g_usb_lo) g_usb_cdb = g_usb_lo;
    if (g_usb_cdb > g_usb_hi) g_usb_cdb = g_usb_hi;

    g_usb_card = n;
    if (g_usb_cdb > usb_cap()) g_usb_cdb = usb_cap();   /* (the limit is per DAC: once it's the output) */
    route_apply(0);                    /* mutes the H2's own DAC */
    usb_volume_apply();                /* the DAC's level before any audio reaches it */
    transport_update();
    snprintf(g_usb_dev, sizeof g_usb_dev, HW_PCM_N, n);
    engine_switch_output(g_eng, out_router_alsa, g_usb_dev);
    fprintf(stderr, "usb dac: card %d %s (%s), %s, %ld cdB\n", n, g_usb_name, usbid,
            g_usb_soft ? "software volume" : "hardware volume", g_usb_cdb);
    char t[112];
    snprintf(t, sizeof t, "%s", g_usb_name);
    ui_toast(t, 2500);
    ui_set_output(g_usb_name);
}

char g_bt_routed[200];        /* the Bluetooth PCM playback went to */

/* Back from a USB DAC or USB DAC mode: to Bluetooth headphones if they're
 * connected, else to the H2's own DAC. */
void output_home(void)
{
    char pcm[200], name[64];
    if (bt_pcm(pcm, sizeof pcm, name, sizeof name)) {
        engine_switch_output(g_eng, out_router_bt, pcm);
        snprintf(g_bt_routed, sizeof g_bt_routed, "%s", pcm);
        route_apply(0);
        ui_set_output(name);
    } else {
        engine_switch_output(g_eng, out_router_alsa, (void *)HW_PCM);
        route_apply(0);
    }
}

static void usb_dac_detach(void)
{
    fprintf(stderr, "usb dac: %s gone\n", g_usb_name);
    engine_pause(g_eng, 1);
    mixer_close(&g_usb_mix);
    if (g_usb_soft) engine_set_gain(g_eng, 1.0f);
    g_usb_card = -1;
    transport_update();                /* (Bluetooth back first, if it was on) */
    output_home();                     /* the H2's own output at its saved level, or Bluetooth */
    ui_toast("USB DAC unplugged: paused", 2500);
}

void usb_dac_poll(lv_timer_t *t)
{
    (void)t;
    if (!g_eng) return;
    char usbid[16];
    if (g_usb_card < 0) {
        int n = usb_card_find(usbid, sizeof usbid);
        if (n >= 0) usb_dac_attach(n, usbid);
        return;
    }
    char path[64];
    snprintf(path, sizeof path, HW_USBID_N, g_usb_card);
    eng_status_t st;
    engine_status(g_eng, &st);
    if (access(path, F_OK) || st.output_lost) usb_dac_detach();
}

/* ---- Bluetooth ----
 * bt.c does the work on its own thread; here playback follows it: to the
 * headphones when they connect (unless a USB DAC or USB DAC mode has the
 * audio), paused and back to the H2 when they drop out. */

void bt_ui_toggle_or_do(int what, const char *addr)
{
    if (what == UI_BT_TOGGLE) settings_set_int("bluetooth.on", !bt_on());
    if (what == UI_BT_RANGE) {                   /* fathom-bt reads it as it starts bluez-alsa */
        settings_set_int("bluetooth.range", !settings_get_int("bluetooth.range", UI_BT_RANGE_DEFAULT));
        settings_save();
    }
    bt_ui_do(what, addr);
}


/* ---- EQ ----
 * Profiles are AutoEQ / Equalizer APO "ParametricEQ.txt" files in
 * DATA/eq/ (the file name, without ".txt" and " ParametricEQ", is the name
 * shown). The choice is remembered per output: the headphone jack, the line
 * out, each USB DAC (by its USB ID) and each pair of Bluetooth headphones, so
 * the EQ follows the headphones. Off by default: bit-perfect. */
static char g_eq_key[64];               /* the output the EQ was last set for */

/* The output playing now: its settings key and its name. */
static void eq_output(char *key, size_t kc, char *label, size_t lc)
{
    char pcm[200], name[64];
    if (g_usb_card >= 0) {
        const char *id = strstr(g_usb_vol_path, "volume-usb-");   /* volume-usb-VID-PID */
        snprintf(key, kc, "eq.usb.%s", id ? id + 11 : "dac");
        snprintf(label, lc, "%.63s", g_usb_name);
    } else if (out_router_on_bt() && g_bt_routed[0]) {
        const char *d = strstr(g_bt_routed, "/dev_");                  /* .../dev_F4_33_.../a2dpsrc/sink */
        char addr[24] = "";
        if (d) sscanf(d + 5, "%23[0-9A-Fa-f_]", addr);
        snprintf(key, kc, "eq.bt.%s", addr);
        if (bt_pcm(pcm, sizeof pcm, name, sizeof name)) snprintf(label, lc, "%s", name);
        else snprintf(label, lc, "Bluetooth");
    } else {
        snprintf(key, kc, "%s", g_route == ROUTE_HEADPHONES ? "eq.headphones" : "eq.line_out");
        snprintf(label, lc, "%s", g_route == ROUTE_HEADPHONES ? "Headphones" : "Line out");
    }
}

/* The profiles in DATA/eq: their names, sorted. */
static int eq_names(char names[][64], int max)
{
    char dir[600];
    snprintf(dir, sizeof dir, "%s/eq", g_data_dir);
    DIR *d = opendir(dir);
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < max) {
        size_t len = strlen(e->d_name);
        if (e->d_name[0] == '.' || len < 5 || strcasecmp(e->d_name + len - 4, ".txt")) continue;
        size_t keep = len - 4;
        static const char sfx[] = " ParametricEQ";               /* AutoEQ's file names */
        if (keep > sizeof sfx - 1 && !strncasecmp(e->d_name + keep - (sizeof sfx - 1), sfx, sizeof sfx - 1))
            keep -= sizeof sfx - 1;
        snprintf(names[n], 64, "%.*s", (int)(keep > 63 ? 63 : keep), e->d_name);
        n++;
    }
    closedir(d);
    qsort(names, (size_t)n, 64, (int (*)(const void *, const void *))strcasecmp);
    return n;
}

/* A profile's file: AutoEQ's "NAME ParametricEQ.txt" if there is one, else "NAME.txt". */
static void eq_path(const char *name, char *out, size_t cap)
{
    snprintf(out, cap, "%s/eq/%s ParametricEQ.txt", g_data_dir, name);
    if (access(out, R_OK)) snprintf(out, cap, "%s/eq/%s.txt", g_data_dir, name);
}

/* Sets the engine's EQ for the output playing now, from its saved choice. */
static void balance_apply(void);
static void crossfeed_apply(void);
static void width_apply(void);
static void limit_apply(void);
static int pure_on(void);
static void pure_report(void);
static void dop_apply(void);

void eq_apply(void)
{
    char key[64], label[64];
    eq_output(key, sizeof key, label, sizeof label);
    snprintf(g_eq_key, sizeof g_eq_key, "%s", key);
    const char *name = settings_get(key, "");
    eq_profile_t p;
    char path[700], err[128] = "";
    eq_path(name, path, sizeof path);
    int on = name[0] && eq_load(path, &p, err, sizeof err) == 0;
    if (name[0] && !on) {
        char t[200];
        tr_fill(t, sizeof t, "EQ \"%s\": %s", name, err);
        ui_toast(t, 4000);
        fprintf(stderr, "eq: %s: %s\n", path, err);
    }
    int pure = pure_on();
    if (g_eng) engine_set_eq(g_eng, on && !pure ? &p : NULL);
    ui_set_eq(on ? name : "", label);
    fprintf(stderr, "eq: %s: %s%s\n", key, on ? name : "off", pure ? " (Pure mode: bypassed)" : "");
    balance_apply();                     /* (a new output: its balance, crossfeed, limit and Pure mode too) */
    crossfeed_apply();
    width_apply();
    limit_apply();
    levelling_apply();
    dop_apply();
    pure_report();
}

/* ---- Balance and volume limit ----
 * Per output, like the EQ: "balance.<output>" and "vollimit.<output>".
 * Balance: -12..+12 dB, below 0 toward the left (the right channel turned
 * down), above 0 toward the right; centred, the samples stay untouched.
 * Volume limit: on the headphone jack and USB DACs, dB below the top of the
 * volume (0 = none, down to -30 in 3 dB steps); on Bluetooth headphones, a
 * percentage of theirs (100 = none). Line out is fixed: no limit. */
#define BALANCE_MAX 12
#define LIMIT_MIN_DB (-30)

static void out_setting_key(const char *what, char *key, size_t cap)
{
    char k[64], label[64];
    eq_output(k, sizeof k, label, sizeof label);
    snprintf(key, cap, "%s.%s", what, k + 3);        /* "eq.headphones" -> "balance.headphones" */
}

/* A number kept for the output playing now ("balance.headphones"...) */
static int out_setting_int(const char *what, int def)
{
    char key[80];
    out_setting_key(what, key, sizeof key);
    return settings_get_int(key, def);
}

static void out_setting_set_int(const char *what, int v)
{
    char key[80];
    out_setting_key(what, key, sizeof key);
    settings_set_int(key, v);
}

int on_bt_now(void) { return g_usb_card < 0 && out_router_on_bt() && g_bt_routed[0]; }
static int on_line_out_now(void) { return g_usb_card < 0 && !out_router_on_bt() && g_route == ROUTE_LINE_OUT; }

static int balance_db(void)
{
    int b = out_setting_int("balance", 0);
    return b < -BALANCE_MAX ? -BALANCE_MAX : b > BALANCE_MAX ? BALANCE_MAX : b;
}

/* Crossfeed, per output like the balance ("crossfeed.<output>": XF_OFF..),
 * off by default; Pure mode bypasses it */
static int crossfeed_now(void)
{
    int l = out_setting_int("crossfeed", XF_OFF);
    return l > XF_OFF && l < XF_LEVELS ? l : XF_OFF;
}

static void crossfeed_apply(void)
{
    if (g_eng) engine_set_crossfeed(g_eng, pure_on() ? XF_OFF : crossfeed_now());
}

/* Stereo width, per output like the balance ("width.<output>", percent: one
 * of WIDTH_STEPS; 100 by default, shown as Off: the samples untouched; wider in
 * tens, as the owner asked, 2026-11-23); Pure mode bypasses it */
static int width_step(void)                         /* which of WIDTH_STEPS: the nearest to what's saved */
{
    return width_step_nearest(out_setting_int("width", WIDTH_NORMAL));
}

static void width_apply(void)
{
    if (g_eng) engine_set_width(g_eng, pure_on() ? WIDTH_NORMAL : WIDTH_STEPS[width_step()]);
}

static void balance_apply(void)
{
    float l, r;
    balance_gains(pure_on() ? 0 : balance_db(), &l, &r);
    if (g_eng) engine_set_balance(g_eng, l, r);
}

static int vol_limit(void)                          /* dB, wired outputs; 0 = none */
{
    if (on_bt_now() || on_line_out_now()) return 0;
    int l = out_setting_int("vollimit", 0);
    return l > 0 ? 0 : l < LIMIT_MIN_DB ? LIMIT_MIN_DB : l;
}

static int bt_limit_pct(void)                       /* Bluetooth: percent; 100 = none */
{
    if (!on_bt_now()) return 100;
    int l = out_setting_int("vollimit", 100);
    return l < 10 ? 10 : l > 100 ? 100 : l;
}

static int bt_cap(void) { return (bt_limit_pct() * 127 + 50) / 100; }

/* The limit for the output playing now: its volume down to it if it's above. */
static void limit_apply(void)
{
    if (on_bt_now()) {
        bt_volume_limit(bt_cap());
    } else if (g_usb_card >= 0) {
        if (g_usb_cdb > usb_cap()) {
            g_usb_cdb = usb_cap();
            usb_volume_apply();
        }
    } else if (g_mix.fd >= 0 && g_route == ROUTE_HEADPHONES) {
        int cur = mixer_get(&g_mix);
        if (cur > hp_cap()) mixer_set(&g_mix, hp_cap());
    }
}

/* UI: Sound > Balance and Volume limit, for the output playing now. */
void hook_sound_value(int what, char *out, size_t cap)
{
    if (what == UI_SOUND_CROSSFEED) {
        snprintf(out, cap, "%s", tr(crossfeed_name(crossfeed_now())));
        return;
    }
    if (what == UI_SOUND_WIDTH) {
        sound_text_width(WIDTH_STEPS[width_step()], out, cap);
        return;
    }
    if (what == UI_SOUND_BALANCE) {
        sound_text_balance(balance_db(), out, cap);
    } else if (on_line_out_now()) {
        snprintf(out, cap, "%s", tr("Fixed level"));
    } else if (on_bt_now()) {
        int p = bt_limit_pct();
        if (p >= 100) snprintf(out, cap, "%s", tr("Off"));
        else snprintf(out, cap, "%d %%", p);
    } else {
        sound_text_limit_db(vol_limit(), out, cap);
    }
}

int hook_sound_adjust(int what, int dir)
{
    if (!dir) {
        if (what == UI_SOUND_LIMIT && on_line_out_now()) { ui_toast("Line out: fixed at 0 dB", 1500); return -1; }
        return 0;
    }
    if (what == UI_SOUND_CROSSFEED) {        /* Off, Light, Medium, Strong, Off... */
        out_setting_set_int("crossfeed", (crossfeed_now() + (dir < 0 ? XF_LEVELS - 1 : 1)) % XF_LEVELS);
        crossfeed_apply();
        return 0;
    }
    if (what == UI_SOUND_WIDTH) {            /* Mono, 50, 75, Off, 110 ... 200 %: the wheel stops at each end */
        int i = width_step() + dir;
        out_setting_set_int("width", WIDTH_STEPS[i < 0 ? 0 : i >= WIDTH_N_STEPS ? WIDTH_N_STEPS - 1 : i]);
        width_apply();
        return 0;
    }
    if (what == UI_SOUND_BALANCE) {
        int b = balance_db() + dir;
        out_setting_set_int("balance", b < -BALANCE_MAX ? -BALANCE_MAX : b > BALANCE_MAX ? BALANCE_MAX : b);
        balance_apply();
        return 0;
    }
    if (on_line_out_now()) { ui_toast("Line out: fixed at 0 dB", 1500); return -1; }
    if (on_bt_now()) {
        int p = bt_limit_pct() + dir * 10;
        out_setting_set_int("vollimit", p < 10 ? 10 : p > 100 ? 100 : p);
    } else {
        int l = vol_limit() + dir * 3;
        out_setting_set_int("vollimit", l < LIMIT_MIN_DB ? LIMIT_MIN_DB : l > 0 ? 0 : l);
    }
    limit_apply();
    return 0;
}

/* DSD as DoP for a USB DAC that takes it: "dop.<output>" = 1 in the settings
 * (settings.ini), and only with the DAC's own volume. Otherwise DSD is made PCM. */
static void dop_apply(void)
{
    if (g_eng) engine_set_dop(g_eng, g_usb_card >= 0 && !g_usb_soft && out_setting_int("dop", 0));
}

/* ---- Pure mode and the signal path ----
 * Pure mode ("pure.<output>", per output like the EQ) bypasses the EQ, the
 * balance and the levelling, keeping their choices for when it's off. With it
 * on, Now Playing says "Bit+" while nothing changes the samples between
 * the file and the DAC. Settings > Sound > Output shows each stage. */
static int pure_on(void)
{
    return out_setting_int("pure", 0) != 0;
}

void levelling_apply(void)
{
    if (g_player) player_set_levelling(g_player, pure_on() ? LEVEL_OFF
                                                           : (levelling_t)settings_get_int("sound.levelling", LEVEL_OFF));
}

/* The samples reach the DAC exactly as decoded from a lossless file. */
static int path_bitperfect(const eng_status_t *st)
{
    return st->fmt.rate && st->fmt.bits && !st->stream && st->untouched && !on_bt_now() &&
           st->out_fmt.rate == st->fmt.rate && st->out_fmt.channels == st->fmt.channels &&
           st->out_fmt.bits >= st->fmt.bits;
}

static void pure_report(void)
{
    int pure = pure_on(), bp = 0;
    if (pure && g_eng) {
        eng_status_t st;
        engine_status(g_eng, &st);
        bp = path_bitperfect(&st);
    }
    ui_set_pure(pure, bp);
}

void hook_pure_toggle(void)
{
    out_setting_set_int("pure", !pure_on());
    eq_apply();                                      /* the EQ, balance and levelling, bypassed or back */
    ui_toast(pure_on() ? "Pure mode: EQ, balance and levelling bypassed" : "Pure mode off", 2000);
}

static void fmt_khz(char *out, size_t cap, unsigned hz)
{
    if (hz % 1000) snprintf(out, cap, "%.1f kHz", hz / 1000.0);
    else snprintf(out, cap, "%u kHz", hz / 1000);
}

static void db_text(char *out, size_t cap, double db)
{
    snprintf(out, cap, "%s%.1f dB", db > 0.05 ? "+" : "", db);
}

/* Settings > Sound > Output: the file, each stage, the output, and the verdict. */
int hook_signal_path(int i, char *label, size_t lc, char *value, size_t vc)
{
    static const char *const L[] = {"File", "Levelling", "EQ", "Balance", "Volume", "Output", "Sent as", "Result", "Because"};
    if (i < 0 || i > (int)(sizeof L / sizeof L[0]) || !g_eng) return -1;
    if (i == 4) {                                    /* Crossfeed, between Balance and Volume */
        snprintf(label, lc, "%s", tr("Crossfeed"));
        if (pure_on()) snprintf(value, vc, "%s", tr("Bypassed"));
        else hook_sound_value(UI_SOUND_CROSSFEED, value, vc);
        return 0;
    }
    if (i > 4) i--;
    eng_status_t st;
    engine_status(g_eng, &st);
    int pure = pure_on(), playing = st.fmt.rate && st.state != ENG_STOPPED;
    int bp = playing && path_bitperfect(&st);
    char k[16];
    if (i == 8) {                                    /* why not bit-perfect (only then) */
        if (!playing || bp) return -1;
        value[0] = '\0';
        size_t n = 0;
#define WHY(c, s) do { if (c) n += (size_t)snprintf(value + n, n < vc ? vc - n : 0, "%s%s", n ? ", " : "", s); } while (0)
        WHY(on_bt_now(), tr("Bluetooth"));
        WHY(st.eq, tr("EQ"));
        WHY(st.track_gain != 1.0f, tr("levelling"));
        WHY(st.balance[0] != 1.0f || st.balance[1] != 1.0f, tr("balance"));
        WHY(st.crossfeed != XF_OFF, tr("crossfeed"));
        WHY(st.width != WIDTH_NORMAL, tr("stereo width"));
        WHY(st.gain != 1.0f, tr("software volume"));
        WHY(st.out_fmt.rate && st.out_fmt.rate != st.fmt.rate, tr("resampled"));
        WHY(!st.fmt.bits && strncmp(st.fmt.codec, "DSD", 3), tr("lossy file"));
        WHY(!st.fmt.bits && !strncmp(st.fmt.codec, "DSD", 3), tr("DSD made PCM"));
        WHY(st.stream, tr("a stream"));
#undef WHY
        if (!value[0]) snprintf(value, vc, "-");
        snprintf(label, lc, "%s", tr(L[i]));
        return 0;
    }
    snprintf(label, lc, "%s", tr(L[i]));   /* (translated here: the values hold names too) */
    switch (i) {
    case 0:
        if (!playing) { snprintf(value, vc, "%s", tr("Nothing playing")); break; }
        fmt_khz(k, sizeof k, st.fmt.rate);
        if (!strncmp(st.fmt.codec, "DSD", 3))       /* the rate is the PCM's (or DoP's) */
            snprintf(value, vc, "%s as %s %s%s", st.fmt.codec, st.fmt.dop ? "DoP" : "PCM", k, st.fmt.channels == 1 ? tr(" mono") : "");
        else if (st.fmt.bits) snprintf(value, vc, "%s %u-bit %s%s", st.fmt.codec, st.fmt.bits, k, st.fmt.channels == 1 ? tr(" mono") : "");
        else snprintf(value, vc, "%s %s%s", st.fmt.codec, k, st.fmt.channels == 1 ? tr(" mono") : "");
        break;
    case 1:
        if (pure) snprintf(value, vc, "%s", tr("Bypassed"));
        else if (settings_get_int("sound.levelling", LEVEL_OFF) == LEVEL_OFF) snprintf(value, vc, "%s", tr("Off"));
        else db_text(value, vc, 20.0 * log10(st.track_gain > 0 ? st.track_gain : 1e-6));
        break;
    case 2: {
        char key[64], lab[64];
        eq_output(key, sizeof key, lab, sizeof lab);
        const char *name = settings_get(key, "");
        if (pure) snprintf(value, vc, "%s", tr("Bypassed"));
        else if (!st.eq || !name[0]) snprintf(value, vc, "%s", tr("Off"));
        else snprintf(value, vc, "%s", name);
        break;
    }
    case 3:
        if (pure) snprintf(value, vc, "%s", tr("Bypassed"));
        else hook_sound_value(UI_SOUND_BALANCE, value, vc);
        break;
    case 4:
        if (on_bt_now()) snprintf(value, vc, "%s", tr("The headphones'"));
        else if (g_usb_card >= 0) {
            char d[24];
            db_text(d, sizeof d, g_usb_cdb / 100.0);
            tr_fill(value, vc, g_usb_soft ? "Software %s" : "In the DAC %s", d, NULL);
        } else if (on_line_out_now()) snprintf(value, vc, "%s", tr("Fixed 0 dB"));
        else {
            char d[24];
            int raw = mixer_get(&g_mix);
            db_text(d, sizeof d, (raw - g_mix.max) * 0.5);
            tr_fill(value, vc, "In the DAC %s", d, NULL);
        }
        break;
    case 5: {
        char pcm[200], name[64];
        if (g_usb_card >= 0) snprintf(value, vc, "%s", g_usb_name);
        else if (on_bt_now() && bt_pcm(pcm, sizeof pcm, name, sizeof name)) snprintf(value, vc, "%s", name);
        else snprintf(value, vc, "ES9018K2M %s", tr(on_line_out_now() ? "line out" : "headphones"));
        break;
    }
    case 6: {                                        /* what the hardware took */
        char d[160] = "", f[16] = "", codec[16] = "";
        unsigned rate = 0;
        float khz = 0;
        engine_describe_output(g_eng, d, sizeof d);
        if (!d[0]) snprintf(value, vc, "-");
        else if (sscanf(d, "Bluetooth %15s %f kHz", codec, &khz) == 2) snprintf(value, vc, "%s 16-bit %.1f kHz", codec, khz);
        else if (sscanf(d, "%*s %15s %u Hz", f, &rate) == 2) {
            int bits = !strcmp(f, "S16_LE") ? 16 : !strcmp(f, "S32_LE") ? 32 : 24;
            fmt_khz(k, sizeof k, rate);
            snprintf(value, vc, "%d-bit %s", bits, k);
        } else snprintf(value, vc, "%s", d);
        break;
    }
    case 7:
        snprintf(value, vc, "%s", !playing ? "-" : tr(bp ? "Bit-perfect" : "Not bit-perfect"));
        break;
    }
    return 0;
}

/* Every 300 ms: the output changed (headphones in, a USB DAC, Bluetooth): its EQ. */
void eq_poll(lv_timer_t *t)
{
    (void)t;
    char key[64], label[64];
    eq_output(key, sizeof key, label, sizeof label);
    if (strcmp(key, g_eq_key)) eq_apply();
    else pure_report();
}

/* UI: the profiles, and a choice for the output playing now ("" = off). */
int hook_eq_list(char names[][64], int max) { return eq_names(names, max); }
void hook_eq_set(const char *name)
{
    char key[64], label[64];
    eq_output(key, sizeof key, label, sizeof label);
    settings_set(key, name ? name : "");
    eq_apply();
}

/* The editor (Settings > Sound > EQ, Menu on a profile). Profiles are saved
 * in Equalizer APO's format, so AutoEQ files edited here still load elsewhere. */
int hook_eq_get(const char *name, eq_profile_t *p)
{
    char path[700], err[128];
    eq_path(name, path, sizeof path);
    return eq_load(path, p, err, sizeof err);
}

int hook_eq_put(const char *name, const eq_profile_t *p)
{
    static const char *const type[] = {"PK", "LSC", "HSC"};
    char dir[600], path[700], tmp[720];
    snprintf(dir, sizeof dir, "%s/eq", g_data_dir);
    mkdir(dir, 0755);
    eq_path(name, path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.new", path);
    FILE *f = fopen(tmp, "we");
    if (!f) return -1;
    fprintf(f, "Preamp: %.1f dB\n", p->preamp_db);
    for (int i = 0; i < p->n; i++)
        fprintf(f, "Filter %d: ON %s Fc %.0f Hz Gain %.1f dB Q %.2f\n", i + 1, type[p->band[i].type],
                p->band[i].fc, p->band[i].gain_db, p->band[i].q);
    if (file_commit(f, 1, tmp, path)) return -1;
    char key[64], label[64];
    eq_output(key, sizeof key, label, sizeof label);
    if (!strcmp(settings_get(key, ""), name)) eq_apply();   /* this output's choice: from the saved file */
    return 0;
}

/* Edited, not saved yet: heard on the output playing now. NULL: its own choice again. */
void hook_eq_preview(const eq_profile_t *p)
{
    if (!p) { eq_apply(); return; }
    if (g_eng) engine_set_eq(g_eng, p);
}

int hook_eq_remove(const char *name)
{
    char path[700], key[64], label[64];
    eq_path(name, path, sizeof path);
    if (unlink(path)) return -1;
    eq_output(key, sizeof key, label, sizeof label);
    if (!strcmp(settings_get(key, ""), name)) settings_set(key, "");
    eq_apply();
    return 0;
}

/* Once a second while Bluetooth is on: what's playing, for fathom-avrcp (what
 * the headphones see of the player). Rewritten only when it changed. */
#define NOW_FILE "/tmp/fathom-now"
void now_publish(lv_timer_t *t)
{
    (void)t;
    static char last[1100];
    if (!g_player || !bt_on()) {
        if (last[0]) { unlink(NOW_FILE); last[0] = '\0'; }
        return;
    }
    player_now_t n;
    player_now(g_player, &n);
    char buf[sizeof last];
    snprintf(buf, sizeof buf, "status %s\nid %lld\nlen %u\npos %u\ntitle %s\nartist %s\nalbum %s\n",
             n.playing ? "Playing" : n.active && !n.stopped ? "Paused" : "Stopped",
             n.radio ? -2 - (long long)player_seq(g_player) : (long long)n.track.id,   /* radio: new song, new id */
             (unsigned)n.len_ms, (unsigned)n.pos_ms, n.track.title, n.track.artist, n.track.album);
    if (!strcmp(buf, last)) return;
    FILE *f = fopen(NOW_FILE ".new", "we");
    if (!f) return;
    fputs(buf, f);
    if (fclose(f) == 0 && rename(NOW_FILE ".new", NOW_FILE) == 0) memcpy(last, buf, sizeof last);
}

void bt_poll(lv_timer_t *t)
{
    (void)t;
    static unsigned seen;
    static int lost_shown;
    if (!g_eng) return;
    if (out_router_on_bt()) {                          /* the link stopped taking audio: say why it paused */
        eng_status_t st;
        engine_status(g_eng, &st);
        if (st.output_lost && !lost_shown) ui_toast("Headphones stopped: paused", 3000);
        lost_shown = st.output_lost;
    }
    unsigned s = bt_seq();
    if (s == seen) return;
    seen = s;
    char msg[96];
    if (bt_take_message(msg, sizeof msg)) ui_toast(msg, 2500);
    ui_bt_changed();
    char pcm[200], name[64], last[18], busy[32];
    int have = bt_pcm(pcm, sizeof pcm, name, sizeof name);
    int on = bt_ui_state(busy, sizeof busy);
    /* (on with no headphones yet: the icon dimmed, as Wi-Fi's while it has
     * no network; it was hidden until headphones were connecting, and
     * Bluetooth just turned on looked as if it hadn't been) */
    ui_set_bt_link(have ? UI_BT_LINK_CONNECTED : on ? UI_BT_LINK_CONNECTING : UI_BT_LINK_NONE);
    if (have && strcmp(pcm, g_bt_routed)) {            /* headphones connected */
        snprintf(g_bt_routed, sizeof g_bt_routed, "%s", pcm);
        if (bt_last(last, sizeof last) && strcmp(last, settings_get("bluetooth.last", "")))
            settings_set("bluetooth.last", last);
        if (g_usb_card >= 0 || g_relay) return;        /* the USB DAC keeps the audio */
        engine_switch_output(g_eng, out_router_bt, pcm);
        route_apply(0);                                 /* the H2's own output goes quiet */
        ui_set_output(name);
        ui_toast(name, 2000);
        fprintf(stderr, "bluetooth: playing to %s (%s)\n", name, pcm);
    } else if (!have && g_bt_routed[0]) {              /* gone */
        g_bt_routed[0] = '\0';
        if (!out_router_on_bt()) return;
        engine_pause(g_eng, 1);
        engine_switch_output(g_eng, out_router_alsa, (void *)HW_PCM);
        route_apply(0);
        ui_toast("Bluetooth disconnected: paused", 2500);
        fprintf(stderr, "bluetooth: disconnected\n");
    }
}
