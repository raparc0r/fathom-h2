/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* The player on the H2: the screens that come before the player proper (the
 * first scan with its progress, a library from an older version brought up
 * to date, a card that can't be used), and Power and the battery on them
 * (main_int.h). Out of main.c, where it stood. */
#include "main_int.h"

/* ---- first-run scan, with progress on screen ----
 * Each step named, how far, and once it can tell (10 s in, 2 % done), about how
 * long that step has left; and that only the first scan takes this long. */

typedef struct { lv_obj_t *step, *count, *left, *path; uint32_t last, t0, left_at; int covers, sorting; } scan_ui_t;

/* Power held on one of the screens that come before the player proper (the
 * first scan, a library being brought up to date, one that can't be opened):
 * the keys aren't served there otherwise, and the only way out was to force
 * the H2 off mid-write. The scan is stopped where it is (the next start
 * carries on from there), what's written is put on the card, and the H2
 * powers off. */
static int g_early_off;
static library_t *g_early_lib;       /* the scan to stop (NULL: none running) */

/* The battery on these screens. The player's own look at it (poll_battery:
 * off at the third low reading in a row) only starts with its loop, so a
 * battery that ran out during a first scan had its power cut by the PMIC in
 * the middle of the library's writes. The same two readings here, about
 * every 10 s, from the calls these screens make as they go (early_keys); the
 * third low one in a row stops the scan and powers off as Power held does.
 * A reading that couldn't be made is not a low one (batlog_low_step). */
static int g_early_empty;            /* it was the battery, not Power: said so */

static void early_battery(void)
{
    static uint32_t at;
    static int looked, run;
    uint32_t now = clock_ms32();
    if (looked && now - at < 10000) return;
    looked = 1;
    at = now;
    char status[32], volt[24];
    int no_status = sysfile_line(HW_BATTERY "status", status, sizeof status);
    int no_volt = sysfile_line(HW_BATTERY "voltage_now", volt, sizeof volt);
    if (batlog_low_step(&run, no_status ? NULL : status, no_volt ? NULL : volt, LOW_BATTERY_UV) && !g_early_off)
        g_early_empty = g_early_off = 1;
}

static void early_keys(void)
{
    input_event_t ev;
    while (input_next(&ev, clock_ms32()))
        if (ev.kind == IN_POWER_OFF) g_early_off = 1;
    early_battery();
    /* (g_quit: a stop asked by init stops the scan too, early_stop below) */
    if ((g_early_off || g_quit) && g_early_lib) library_scan_cancel(g_early_lib);
}

static void early_power_off(lv_obj_t *label)
{
    fprintf(stderr, "power off: %s before the player was up\n", g_early_empty ? "battery empty" : "Power held");
    if (label) {
        lv_label_set_text(label, tr(g_early_empty ? "Battery empty" : "Powering off"));
        lv_refr_now(NULL);
    }
    clock_save();
    settings_save();
    mark_clean_stop();                 /* (as at the end of main: the library was closed before coming here, or never opened) */
    sync();
    if (system("/sbin/poweroff") != 0) fprintf(stderr, "poweroff failed\n");
    /* init stops us (S90fathom stop), and we go as soon as it asks. This
     * waited for ever before: the player was killed 5 s later, and with no
     * mark the next start checked the whole library for it. */
    struct pollfd q = {.fd = g_quit_pipe[0], .events = POLLIN};
    while (!g_quit) poll(&q, 1, 1000);
    exit(0);
}

/* A stop asked by init on one of these screens with no Power held (the card
 * taken out, S90fathom stop): nothing there looked at g_quit, so the scan ran
 * on, the player was killed 5 s later, and the next start checked the whole
 * library for it. The scan is stopped as Power stops it (early_keys); then
 * out as at the end of main, the H2 left on: the logo, the stop marked clean
 * (the library was closed before coming here, or never opened). */
static void early_stop(void)
{
    fprintf(stderr, "fathom-player: asked to stop before the player was up: exiting\n");
    DISP(show_last_frame)();
    clock_save();
    settings_save();
    mark_clean_stop();
    exit(0);
}

static void scan_left(scan_ui_t *s, int done, int total, uint32_t now)
{
    uint32_t el = now - s->t0;
    if (total <= 0 || done <= 0 || el < 10000 || done * 50 < total) { lv_label_set_text(s->left, ""); return; }
    if (now - s->left_at < 2000) return;                 /* (steadier: every 2 s) */
    s->left_at = now;
    uint32_t left_s = (uint32_t)((uint64_t)el * (uint32_t)(total - done) / (uint32_t)done / 1000);
    char t[64];
    ui_left_words(left_s, t, sizeof t);
    lv_label_set_text(s->left, t);
}

static void scan_progress(int done, int total, const char *path, void *ctx)
{
    scan_ui_t *s = ctx;
    early_keys();                                        /* (Power held: the scan stops) */
    if (path && !strcmp(path, "done")) return;           /* (the screen goes) */
    if (path && (!strcmp(path, "looking") || !strcmp(path, "checking"))) {   /* the folder being listed: how many so far */
        char n[16], t[64];
        snprintf(n, sizeof n, "%d", done);
        tr_fill(t, sizeof t, path[0] == 'c' ? "%s songs checked" : "%s songs found", n, NULL);
        lv_label_set_text(s->step, tr("Looking for music"));
        lv_label_set_text(s->count, t);
        lv_label_set_text(s->left, "");
        lv_label_set_text(s->path, "");
        s->covers = -1;                                  /* (the songs' step says its own name when it comes) */
        lv_refr_now(NULL);
        if (g_shot) { g_shot = 0; screen_shot(); }
        return;
    }
    if (path && (!strcmp(path, "sorting") || !strcmp(path, "working"))) {   /* that step, at work */
        char a[16], b[16], t[64];
        snprintf(a, sizeof a, "%d", done);
        snprintf(b, sizeof b, "%d", total);
        if (path[0] == 's') tr_fill(t, sizeof t, "%s of %s songs", a, b);
        else snprintf(t, sizeof t, "%s%s", tr("Working"), &"..."[2 - done % 3]);
        if (!s->sorting) {                               /* (a first scan groups as it reads: this step wasn't named yet) */
            s->sorting = 1;
            lv_label_set_text(s->step, tr("Sorting albums and artists"));
            lv_label_set_text(s->left, "");
            lv_label_set_text(s->path, "");
        }
        lv_label_set_text(s->count, t);
        lv_refr_now(NULL);
        if (g_shot) { g_shot = 0; screen_shot(); }
        return;
    }
    if (path && (!strcmp(path, "albums") || !strcmp(path, "indexes"))) {   /* no count: steps of their own */
        lv_label_set_text(s->step, tr(path[0] == 'a' ? "Sorting albums and artists" : "Getting your lists ready"));
        s->sorting = 1;
        lv_label_set_text(s->count, "");
        lv_label_set_text(s->left, "");
        lv_label_set_text(s->path, "");
        lv_refr_now(NULL);
        if (g_shot) { g_shot = 0; screen_shot(); }
        return;
    }
    uint32_t now = clock_ms32();
    int covers = total < 0;
    if (covers) total = -total - 1;
    if (covers != s->covers) {                           /* the next step: its own clock */
        s->covers = covers;
        s->t0 = now;
        s->left_at = 0;
        lv_label_set_text(s->step, tr(covers ? "Finding covers" : "Reading songs"));
        lv_label_set_text(s->left, "");
    }
    if (now - s->last < 250 && done != total) return;   /* redraw at most 4x per second */
    s->last = now;
    char a[16], b[16], t[64];
    snprintf(a, sizeof a, "%d", done);
    snprintf(b, sizeof b, "%d", total);
    if (total > 0 && done <= total) tr_fill(t, sizeof t, covers ? "%s of %s albums" : "%s of %s songs", a, b);
    else snprintf(t, sizeof t, "%d", done);
    lv_label_set_text(s->count, t);
    scan_left(s, done, total, now);
    const char *slash = path && !covers ? strrchr(path, '/') : NULL;
    lv_label_set_text(s->path, slash ? slash + 1 : "");
    lv_refr_now(NULL);
    if (g_shot) { g_shot = 0; screen_shot(); }          /* (kill -USR1: this screen too) */
}

/* These screens' rows: a column, so a title that wraps (a wide font, a
 * translation) pushes the rest down instead of over them; each row one line
 * (cut with "..."), the title and the note at the foot wrapped */
int scan_with_progress(library_t *lib, const char *music, const theme_t *t, const ui_fonts_t *f)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(t->bg), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);   /* (no theme to set it) */
    lv_obj_t *col = ui_scan_column(scr);
    lv_obj_t *title = ui_scan_label(col, f->title, t->text, 1);
    lv_label_set_text(title, tr("Reading your library"));
    scan_ui_t s = {.step = ui_scan_label(col, f->body, t->text, 0), .count = ui_scan_label(col, f->body, t->signal, 0),
                   .left = ui_scan_label(col, f->meta, t->text_muted, 0), .path = ui_scan_label(col, f->meta, t->text_muted, 0),
                   .t0 = clock_ms32(), .covers = -1};
    lv_label_set_text(s.step, tr("Looking for music"));   /* (the folders first: a few seconds) */
    lv_obj_t *note = ui_scan_note(scr, f->meta, t->text_muted);
    lv_label_set_text(note, tr("Only the initial scan takes this long"));
    ui_scan_fit(col, note);
    lv_obj_t *prev = lv_screen_active();
    lv_screen_load(scr);
    lv_refr_now(NULL);

    uint32_t t0 = clock_ms32();
    /* The songs, albums and artists here; the covers (the longest step: a
     * third of it and more) in the background once the library is in use
     * (library_needs_art starts that scan, below in main) */
    library_scan_defer_art(lib, 1);
    g_early_lib = lib;
    int n = library_scan(lib, music, scan_progress, &s);
    if (n >= 0) pack_songs_count(lib);          /* (a first scan: its names counted with it) */
    g_early_lib = NULL;
    fprintf(stderr, "library: %d tracks in %.1f s\n", n, (clock_ms32() - t0) / 1000.0);
    early_keys();
    if (g_early_off) {
        library_close(lib);                              /* (its log emptied into the database) */
        early_power_off(s.step);
    }
    if (g_quit) {                                        /* (init's stop: out, the H2 left on) */
        library_close(lib);
        early_stop();
    }
    /* Then the database rewritten tight: a first scan leaves it fragmented,
     * and reading every song (Never played, a rescan) takes over twice as long
     * (on the H2, 50,000 songs: 2.3 s a read, 1.0 s after; compacting took 36 s,
     * about 0.75 ms a song). Here, where waiting is expected and nothing else
     * writes; skipped if the card hasn't the room. */
    if (n > 0) {
        char left[64];
        ui_left_words((uint32_t)((int64_t)n * 3 / 4000 + 1), left, sizeof left);
        lv_label_set_text(s.step, tr("Tidying up"));
        lv_label_set_text(s.count, "");
        lv_label_set_text(s.left, left);
        lv_label_set_text(s.path, "");
        lv_refr_now(NULL);
        if (g_shot) { g_shot = 0; screen_shot(); }
        library_compact(lib);
    }

    lv_screen_load(prev);
    lv_obj_delete(scr);
    return n;
}

/* ---- a library from an older version, brought up to date ----
 * library_open() does it (new indexes, a pass over every song): on a big
 * library a while (50,000 songs: ~40 s on the H2), so a screen says what's
 * happening and about how long it has left, then the library is tidied up
 * (compacted) while at it. Smaller ones: under the logo, as before (2,400
 * songs: a couple of seconds). */

/* (measured on the H2, 50,000 songs: the upgrade ~0.8 ms a song, compacting ~0.75) */
#define UPGRADE_US_PER_SONG 800
#define TIDY_US_PER_SONG 750


static void upgrade_left(upgrade_ui_t *u, int force)
{
    uint32_t now = clock_ms32(), el = now - u->t0;
    if (!force && now - u->left_at < 2000) return;   /* (steadier: every 2 s) */
    u->left_at = now;
    char t[64];
    ui_left_words(u->estimate_ms > el ? (u->estimate_ms - el) / 1000 : 0, t, sizeof t);
    lv_label_set_text(u->left, t);
    lv_refr_now(NULL);
    if (g_shot) { g_shot = 0; screen_shot(); }
}

static void upgrade_progress(int done, int total, void *ctx)
{
    (void)done;
    (void)total;
    early_keys();                    /* (Power held: off once the library is open again, below) */
    upgrade_left(ctx, 0);
}

void upgrade_screen_open(upgrade_ui_t *u, int64_t songs, const theme_t *t, const ui_fonts_t *f)
{
    u->scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(u->scr, lv_color_hex(t->bg), 0);
    lv_obj_set_style_bg_opa(u->scr, LV_OPA_COVER, 0);
    lv_obj_t *col = ui_scan_column(u->scr);
    lv_label_set_text(ui_scan_label(col, f->title, t->text, 1), tr("Updating your library"));
    u->step = ui_scan_label(col, f->body, t->text, 0);
    lv_label_set_text(u->step, tr("Making your lists quicker"));
    u->left = ui_scan_label(col, f->meta, t->text_muted, 0);
    lv_obj_t *note = ui_scan_note(u->scr, f->meta, t->text_muted);
    lv_label_set_text(note, tr("Only this once, after the update"));
    ui_scan_fit(col, note);
    u->prev = lv_screen_active();
    u->t0 = clock_ms32();
    u->estimate_ms = (uint32_t)(songs * UPGRADE_US_PER_SONG / 1000);
    lv_screen_load(u->scr);
    upgrade_left(u, 1);
    library_set_upgrade_progress(upgrade_progress, u);
}

/* After library_open(): tidied up, and the screen gone */
void upgrade_screen_close(upgrade_ui_t *u, library_t *lib, int64_t songs)
{
    library_set_upgrade_progress(NULL, NULL);
    fprintf(stderr, "library: brought up to date in %.1f s\n", (clock_ms32() - u->t0) / 1000.0);
    early_keys();
    if (g_early_off) {
        if (lib) library_close(lib);
        early_power_off(u->step);
    }
    if (g_quit) {                    /* (init's stop: out before the tidying, the H2 left on) */
        if (lib) library_close(lib);
        early_stop();
    }
    if (lib) {
        lv_label_set_text(u->step, tr("Tidying up"));
        u->t0 = clock_ms32();
        u->estimate_ms = (uint32_t)(songs * TIDY_US_PER_SONG / 1000);
        upgrade_left(u, 1);
        library_compact(lib);
    }
    lv_screen_load(u->prev);
    lv_obj_delete(u->scr);
    u->scr = NULL;
}

/* The library can be neither opened nor made anew: the card is the trouble.
 * Said, and Power turns the H2 off; this doesn't return. */
void card_unusable_screen(const theme_t *t, const ui_fonts_t *f)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(t->bg), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_t *col = ui_scan_column(scr);
    lv_label_set_text(ui_scan_label(col, f->title, t->text, 1), tr("The card can't be used"));
    lv_obj_t *why = ui_scan_label(col, f->body, t->text, 1);
    lv_label_set_text(why, tr("Fathom can't open or make its library on it. Check the card on a computer."));
    lv_obj_t *note = ui_scan_note(scr, f->meta, t->text_muted);
    lv_label_set_text(note, tr("Hold <Power> to turn off"));
    lv_screen_load(scr);
    lv_refr_now(NULL);
    for (;;) {
        early_keys();
        if (g_early_off) early_power_off(why);
        if (g_quit) early_stop();
        usleep(100000);
    }
}
