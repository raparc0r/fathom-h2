/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/*
 * The two pages with a graph over their rows. Settings > Power > Battery: its
 * rows' words (the spans on battery, their days, how long a charge lasts) and
 * the graph of the charge over a day or a week. Settings > Sound > EQ, a
 * profile or a band: the curve of the profile being edited, worked out again
 * when it changes. ui.c asks for a row's text (battery_row), places each graph
 * when its page opens (battery_place, eq_place) and says when what they show
 * has changed (battery_labels, eqg_update).
 */
#include "ui/ui_internal.h"

/* ---------- battery history (Settings > Power > Battery) ---------- */

#define BAT_GX0 34              /* the plot: a column a pixel, 0-100 % between these */
#define BAT_GY0 6
#define BAT_GY1 64

static const char *const BAT_ACTS[BL_ACTS] = {"Playing", "Screen on", "Paused"};   /* (playing and paused: the screen off) */

static void fmt_dur(char *b, size_t n, int64_t secs)
{
    long m = (long)((secs + 30) / 60);
    if (m < 60) tr_num(b, n, "%s min", m);
    else if (m % 60 == 0) tr_num(b, n, "%s h", m / 60);
    else {
        char h[16], mm[16];
        snprintf(h, sizeof h, "%ld", m / 60);
        snprintf(mm, sizeof mm, "%02ld", m % 60);
        tr_fill(b, n, "%s h %s min", h, mm);
    }
}

static void fmt_full(char *b, size_t n, int h10)
{
    tr_num(b, n, "about %s h from full", (h10 + 5) / 10);   /* (shown only above 0: a dash where there's too little to say) */
}

/* A span's day: Today, Yesterday, 16 Nov (in the time zone, as About's
 * clock); without a clock ever set, how many days back */
void bat_day(char *b, size_t n, int64_t t)
{
    int64_t now = S.bat.t1;
    if (!S.bat.clock_ok) {
        long d = (long)((now - t) / 86400);
        if (d <= 0) snprintf(b, n, "%s", tr("Today"));
        else if (d == 1) snprintf(b, n, "%s", tr("Yesterday"));
        else tr_num(b, n, "%s days ago", d);
        return;
    }
    int z = ui_zone();
    int64_t lnow = tz_to_local(z, now), lt = tz_to_local(z, t);
    long dn = (long)(lnow / 86400), dt = (long)(lt / 86400);
    if (dt == dn) { snprintf(b, n, "%s", tr("Today")); return; }
    if (dt == dn - 1) { snprintf(b, n, "%s", tr("Yesterday")); return; }
    dt_t d;
    dt_split(lt, &d);
    char day[8];
    snprintf(day, sizeof day, "%d", d.day);
    snprintf(b, n, "%s %s", day, ui_month(d.mon));
}

static const batspan_t *bat_cur(void)
{
    if (S.bat_span < 0) return S.bat.have_since ? &S.bat.since : NULL;
    return S.bat_span < S.bat.nspans ? &S.bat.span[S.bat_span] : NULL;
}

/* The first of the metas (longest first, NULL-ended) that fits beside main
 * in this theme's fonts (the pixel one is wide) */
static void bat_meta(const char *main, char *meta, size_t tn, const char *const *alts)
{
    int room = UI_W - 2 * ROW_MARGIN - 2 * 12 - 16;
    int a = text_w(tr(main), S.f->body);
    int i = 0;
    for (; alts[i + 1]; i++)
        if (a + text_w(alts[i], VALUE_FONT) <= room) break;
    snprintf(meta, tn, "%s", alts[i]);
}

void battery_row(list_kind_t k, int i, char *main, size_t mn, char *meta, size_t tn)
{
    char d[32], m1[64], m2[64], m3[64];
    const char *dash = "\xe2\x80\x93";
    if (k == L_BATTERY) {
        if (i == 0) {
            snprintf(main, mn, "%s", S.bat_days == 7 ? "Last 7 days" : "Last 24 hours");
            snprintf(meta, tn, "%s", S.bat_days == 7 ? "<Play>: 24 hours" : "<Play>: 7 days");
        } else if (i == 1) {
            snprintf(main, mn, "Since full charge");
            fmt_dur(d, sizeof d, S.bat.since.on_s);
            snprintf(m1, sizeof m1, "%s, %d %%", d, S.bat.since.used);   /* (d translated) */
            snprintf(m2, sizeof m2, "%d %%", S.bat.since.used);
            const char *alts[] = {m1, m2, NULL};
            if (S.bat.have_since) bat_meta(main, meta, tn, alts);
            else snprintf(meta, tn, "%s", dash);
        } else {
            snprintf(main, mn, "History");
            if (S.bat.total_spans == 1) snprintf(meta, tn, "%s", tr("1 charge"));
            else if (S.bat.total_spans) tr_num(meta, tn, "%s charges", S.bat.total_spans);
            else snprintf(meta, tn, "%s", dash);
        }
        return;
    }
    if (k == L_BAT_HISTORY) {
        if (i < S.bat.nspans) {
            const batspan_t *x = &S.bat.span[i];
            bat_day(main, mn, x->start);
            if (S.bat.clock_ok) {                  /* (two on one day: when each began) */
                dt_t at;
                dt_split(tz_to_local(ui_zone(), x->start), &at);
                size_t l = strlen(main);
                snprintf(main + l, mn - l, ", %02d:%02d", at.hour, at.min);
            }
            fmt_dur(d, sizeof d, x->on_s);
            snprintf(m1, sizeof m1, "%d%s%d %%, %s", x->pct_start, dash, x->pct_end, d);
            snprintf(m2, sizeof m2, "%d%s%d %%", x->pct_start, dash, x->pct_end);
            const char *alts[] = {m1, m2, NULL};
            bat_meta(main, meta, tn, alts);
        } else {
            snprintf(main, mn, "%s", tr("Average"));
            fmt_full(m1, sizeof m1, S.bat.avg_full_h10);
            tr_num(m2, sizeof m2, "%s h", (S.bat.avg_full_h10 + 5) / 10);
            const char *alts[] = {m1, m2, NULL};
            if (S.bat.avg_full_h10 > 0) bat_meta(main, meta, tn, alts);
            else snprintf(meta, tn, "%s", dash);
        }
        return;
    }
    const batspan_t *x = bat_cur();            /* L_BAT_SPAN */
    if (!x) return;
    if (i == 0) {
        snprintf(main, mn, "On battery");
        fmt_dur(d, sizeof d, x->on_s);
        char pc[16];
        snprintf(pc, sizeof pc, "%d %%", x->used);
        tr_fill(m1, sizeof m1, "%s, %s used", d, pc);
        snprintf(m2, sizeof m2, "%s, %d %%", d, x->used);
        snprintf(m3, sizeof m3, "%s", d);
        const char *alts[] = {m1, m2, m3, NULL};
        bat_meta(main, meta, tn, alts);
    } else if (i == 1) {
        snprintf(main, mn, "At this rate");
        fmt_full(m1, sizeof m1, x->full_h10);
        tr_num(m2, sizeof m2, "%s h from full", (x->full_h10 + 5) / 10);
        tr_num(m3, sizeof m3, "%s h", (x->full_h10 + 5) / 10);
        const char *alts[] = {m1, m2, m3, NULL};
        if (x->full_h10 > 0) bat_meta(main, meta, tn, alts);
        else snprintf(meta, tn, "%s", dash);
    } else {
        int a = i - 2;
        snprintf(main, mn, "%s", BAT_ACTS[a]);
        fmt_dur(d, sizeof d, x->act_s[a]);
        char per[32];
        tr_num(per, sizeof per, "%s min", x->min_per_pct[a]);
        snprintf(m1, sizeof m1, "%s, 1 %%/%s", d, per);
        snprintf(m2, sizeof m2, "1 %%/%s", per);
        const char *alts[] = {m1, m2, NULL};
        if (!x->act_s[a]) snprintf(meta, tn, "%s", dash);
        else if (x->min_per_pct[a]) bat_meta(main, meta, tn, alts);
        else snprintf(meta, tn, "%s", d);
    }
}

/* The graph's labels: 100, 50 and 0 down the side; the hours (or days) under
 * it, and now */
void battery_labels(void)
{
    if (!S.bat_graph) return;
    const theme_t *t = T();
    for (int i = 0; i < 9; i++) {
        style_label(S.bat_lbl[i], S.f->meta, t->text_muted);
        show(S.bat_lbl[i], 0);
    }
    static const char *const Y[3] = {"100", "50", "0"};
    for (int i = 0; i < 3; i++) {
        set_text(S.bat_lbl[i], Y[i]);
        lv_obj_set_style_text_align(S.bat_lbl[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_width(S.bat_lbl[i], BAT_GX0 - 1);
        lv_obj_set_pos(S.bat_lbl[i], 0, BAT_GY0 + i * (BAT_GY1 - BAT_GY0) / 2 - lv_font_get_line_height(S.f->meta) / 2);
        show(S.bat_lbl[i], 1);
    }
    int64_t t0 = S.bat.t0, t1 = S.bat.t1, span = t1 - t0;
    if (span <= 0) return;
    int n = 3, last_x = BAT_GX0 + BATREP_COLS - text_w(tr("now"), S.f->meta) - 6;   /* ("now" at the right end) */
    int lbl_y = BAT_GRAPH_H - lv_font_get_line_height(S.f->meta);   /* (the wider fonts are taller) */
    if (lbl_y < BAT_GY1 + 4) lbl_y = BAT_GY1 + 4;   /* (below the charging strip) */
    int64_t step = S.bat_days == 7 ? 86400 : 6 * 3600;
    /* the marks at midnights (or every 6 hours) in the time zone: lt, at tt */
    int z = S.bat.clock_ok ? ui_zone() : 0;
    int64_t first = (tz_to_local(z, t0) / step + 1) * step;
    char b[32];
    for (int64_t lt = first, tt = tz_to_utc(z, lt); tt < t1 && n < 8; lt += step, tt = tz_to_utc(z, lt)) {
        int x = BAT_GX0 + (int)((tt - t0) * BATREP_COLS / span);
        int tw = text_w("00:00", S.f->meta);
        if (x + tw / 2 + 4 > last_x || x - tw / 2 < 2) continue;
        if (S.bat.clock_ok && S.bat_days == 7) {
            static const char *const WD[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
            snprintf(b, sizeof b, "%s", tr(WD[(lt / 86400 + 4) % 7]));   /* (1970-01-01 was a Thursday) */
        } else if (S.bat.clock_ok) {
            snprintf(b, sizeof b, "%02d:00", (int)((lt / 3600) % 24));
        } else {
            char ago[24];
            tr_num(ago, sizeof ago, S.bat_days == 7 ? "%s d" : "%s h",
                   (t1 - tt + step / 2) / (S.bat_days == 7 ? 86400 : 3600));
            snprintf(b, sizeof b, "-%s", ago);
        }
        set_text(S.bat_lbl[n], b);
        lv_obj_set_style_text_align(S.bat_lbl[n], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(S.bat_lbl[n], 64);
        lv_obj_set_pos(S.bat_lbl[n], x - 32, lbl_y);
        show(S.bat_lbl[n], 1);
        n++;
    }
    set_text(S.bat_lbl[8], tr("now"));
    lv_obj_set_style_text_align(S.bat_lbl[8], LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_width(S.bat_lbl[8], 64);
    lv_obj_set_pos(S.bat_lbl[8], BAT_GX0 + BATREP_COLS - 64, lbl_y);
    show(S.bat_lbl[8], 1);
}

static void bat_rect(lv_layer_t *layer, lv_draw_rect_dsc_t *d, int x1, int y1, int x2, int y2)
{
    lv_area_t a = {x1, y1, x2, y2};
    lv_draw_rect(layer, d, &a);
}

/* The graph, drawn whole: the charge a column a pixel (the theme's signal
 * colour on battery, its muted one charging, with a strip under), filled
 * faintly beneath; a dashed line where the H2 was off for a time not known */
static void bat_graph_draw(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(lv_event_get_target(e), &a);
    const theme_t *t = T();
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.radius = 0;
    d.bg_opa = LV_OPA_COVER;
    int x0 = a.x1 + BAT_GX0, y0 = a.y1 + BAT_GY0, y1 = a.y1 + BAT_GY1;
    d.bg_color = C(t->surface);
    for (int g = 0; g < 3; g++) {
        int y = y0 + g * (y1 - y0) / 2;
        bat_rect(layer, &d, x0, y, x0 + BATREP_COLS - 1, y);
    }
    /* runs of columns at the same charge and state: one rectangle each */
    for (int c = 0; c < BATREP_COLS;) {
        int p = S.bat.pct[c], chg = S.bat.charging[c], e2 = c + 1;
        while (e2 < BATREP_COLS && S.bat.pct[e2] == p && S.bat.charging[e2] == chg) e2++;
        if (p >= 0) {
            int y = y1 - p * (y1 - y0) / 100;
            d.bg_color = C(chg ? t->text_muted : t->signal);
            d.bg_opa = LV_OPA_40;
            if (y + 2 <= y1) bat_rect(layer, &d, x0 + c, y + 2, x0 + e2 - 1, y1);
            d.bg_opa = LV_OPA_COVER;
            bat_rect(layer, &d, x0 + c, y, x0 + e2 - 1, y + 1);
            if (chg) bat_rect(layer, &d, x0 + c, y1 + 2, x0 + e2 - 1, y1 + 3);
        }
        c = e2;
    }
    d.bg_color = C(t->text_muted);
    d.bg_opa = LV_OPA_COVER;
    for (int g = 0; g < S.bat.ngaps; g++)
        for (int y = y0; y < y1; y += 4) bat_rect(layer, &d, x0 + S.bat.gap[g], y, x0 + S.bat.gap[g], y + 1);
}

/* The Battery screen's graph over the top of the list (graphs_place moves the list down) */
int battery_place(list_kind_t k)
{
    int on = k == L_BATTERY && S.hooks.battery_report;
    if (on && !S.bat_graph) {
        S.bat_graph = box(S.list, 0, 0, UI_W, BAT_GRAPH_H);
        lv_obj_add_event_cb(S.bat_graph, bat_graph_draw, LV_EVENT_DRAW_MAIN, NULL);
        for (int i = 0; i < 9; i++) S.bat_lbl[i] = label(S.bat_graph);
    }
    if (S.bat_graph) {
        show(S.bat_graph, on);
        lv_obj_set_style_bg_color(S.bat_graph, C(T()->bg), 0);
        lv_obj_set_style_bg_opa(S.bat_graph, LV_OPA_COVER, 0);
        if (on) { lv_obj_move_foreground(S.bat_graph); battery_labels(); }
    }
    return on;
}

/* ---------- the EQ editor's curve (Settings > Sound > EQ > a profile, a band) ---------- */

static int eqg_x(double f)
{
    if (f < 20) f = 20;
    if (f > 20000) f = 20000;
    return EQG_X0 + (int)lround((log10(f) - log10(20.0)) / 3.0 * (EQG_X1 - EQG_X0));
}

static int eqg_y(double db)
{
    if (db > EQG_RANGE) db = EQG_RANGE;
    if (db < -EQG_RANGE) db = -EQG_RANGE;
    return EQG_Y0 + (int)lround((EQG_RANGE - db) / (2 * EQG_RANGE) * (EQG_Y1 - EQG_Y0));
}

/* The curve at the plot's column x, between the points either side of it */
static double eqg_at(int x)
{
    double pos = (double)x * (EQG_PTS - 1) / (EQG_X1 - EQG_X0);
    int i = (int)pos;
    if (i < 0) return S.eqg_db[0];
    if (i >= EQG_PTS - 1) return S.eqg_db[EQG_PTS - 1];
    double fr = pos - i;
    return S.eqg_db[i] * (1 - fr) + S.eqg_db[i + 1] * fr;
}

static int same_profile(const eq_profile_t *a, const eq_profile_t *b)
{
    if (a->preamp_db != b->preamp_db || a->n != b->n) return 0;
    for (int i = 0; i < a->n; i++)
        if (a->band[i].type != b->band[i].type || a->band[i].fc != b->band[i].fc ||
            a->band[i].gain_db != b->band[i].gain_db || a->band[i].q != b->band[i].q)
            return 0;
    return 1;
}

/* The curve for the profile being edited (worked out again only when it
 * changed), and the warning when, with the preamp, it goes above 0 dB: the
 * loudest moments can clip there. */
void eqg_update(void)
{
    if (S.eqg_have && same_profile(&S.eqg_for, &S.eqe)) return;
    double f[EQG_PTS];
    for (int i = 0; i < EQG_PTS; i++) f[i] = 20 * pow(1000, (double)i / (EQG_PTS - 1));
    eq_curve_db(&S.eqe, 48000, f, EQG_PTS, S.eqg_db);
    S.eqg_for = S.eqe;
    S.eqg_have = 1;
    double peak = -1e9;
    int at = 0;
    for (int i = 0; i < EQG_PTS; i++)
        if (S.eqg_db[i] > peak) peak = S.eqg_db[i], at = i;
    peak += S.eqe.preamp_db;
    if (S.eq_warn) {
        char num[16], t[64];
        snprintf(num, sizeof num, "+%.1f dB", peak);
        tr_fill(t, sizeof t, "%s over 0 dB", num, NULL);
        set_text(S.eq_warn, t);
        show(S.eq_warn, peak > 0.05);
        /* on the side away from the peak, so it covers neither it nor its dot */
        lv_obj_align(S.eq_warn, at >= EQG_PTS / 2 ? LV_ALIGN_TOP_LEFT : LV_ALIGN_TOP_RIGHT,
                     at >= EQG_PTS / 2 ? EQG_X0 : -(UI_W - EQG_X1), EQG_Y0);
    }
}

/* The band the page is about: the selected row's on the profile's page, the
 * page's own on a band's; -1 for none (Preamp, Add a band...) */
static int eqg_selected(void)
{
    const level_t *l = &S.stack[S.depth - 1];
    if (l->kind == L_EQ_BAND) return l->parent >= 0 && l->parent < S.eqe.n ? (int)l->parent : -1;
    if (l->kind == L_EQ_EDIT) return l->sel >= 1 && l->sel <= S.eqe.n ? l->sel - 1 : -1;
    return -1;
}

/* Drawn whole: the grid (+-6 dB, 100 Hz, 1 kHz, 10 kHz; 0 dB stronger), on a
 * band's page a faint line at its frequency, the curve a column a pixel (each
 * joined to the one before), the bands as dots on it, the selected one bigger
 * in the theme's signal colour */
static void eq_graph_draw(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(lv_event_get_target(e), &a);
    const theme_t *t = T();
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.radius = 0;
    d.bg_opa = LV_OPA_COVER;
    int ox = a.x1, oy = a.y1, x0 = ox + EQG_X0, x1 = ox + EQG_X1;
    d.bg_color = C(t->surface);
    for (int g = -1; g <= 1; g += 2) {
        int y = oy + eqg_y(6.0 * g);
        bat_rect(layer, &d, x0, y, x1, y);
    }
    static const double decade[3] = {100, 1000, 10000};
    for (int i = 0; i < 3; i++) {
        int x = ox + eqg_x(decade[i]);
        bat_rect(layer, &d, x, oy + EQG_Y0, x, oy + EQG_Y1);
    }
    d.bg_color = C(t->text_muted);
    d.bg_opa = LV_OPA_50;
    bat_rect(layer, &d, x0, oy + eqg_y(0), x1, oy + eqg_y(0));
    int sel = eqg_selected();
    if (S.stack[S.depth - 1].kind == L_EQ_BAND && sel >= 0) {
        d.bg_color = C(t->signal);
        d.bg_opa = LV_OPA_40;
        int x = ox + eqg_x(S.eqe.band[sel].fc);
        bat_rect(layer, &d, x, oy + EQG_Y0, x, oy + EQG_Y1);
    }
    d.bg_opa = LV_OPA_COVER;
    d.bg_color = C(t->text);
    int prev = -1;
    for (int x = 0; x <= EQG_X1 - EQG_X0; x++) {
        int y = eqg_y(eqg_at(x));
        int lo = prev < 0 || y < prev ? y : prev, hi = prev < 0 || y > prev ? y : prev;
        bat_rect(layer, &d, x0 + x, oy + lo, x0 + x, oy + hi + 1);
        prev = y;
    }
    int round_dots = t->corner == THEME_CORNER_ROUND;
    for (int pass = 0; pass < 2; pass++)          /* the others, then the selected one over them */
        for (int i = 0; i < S.eqe.n; i++) {
            if ((i == sel) != pass) continue;
            int x = eqg_x(S.eqe.band[i].fc), y = eqg_y(eqg_at(x - EQG_X0)), r = pass ? 4 : 2;
            d.bg_color = C(pass ? t->signal : t->text_muted);
            d.radius = round_dots ? r + 1 : 0;
            bat_rect(layer, &d, ox + x - r, oy + y - r, ox + x + r, oy + y + r);
        }
}

/* The curve over the top of the EQ editor's pages (graphs_place moves the list down) */
int eq_place(list_kind_t k)
{
    int on = k == L_EQ_EDIT || k == L_EQ_BAND;
    if (on && !S.eq_graph) {
        S.eq_graph = box(S.list, 0, 0, UI_W, BAT_GRAPH_H);
        lv_obj_add_event_cb(S.eq_graph, eq_graph_draw, LV_EVENT_DRAW_MAIN, NULL);
        for (int i = 0; i < 3; i++) S.eq_lbl[i] = label(S.eq_graph);
        S.eq_warn = label(S.eq_graph);
    }
    if (S.eq_graph) {
        show(S.eq_graph, on);
        lv_obj_set_style_bg_color(S.eq_graph, C(T()->bg), 0);
        lv_obj_set_style_bg_opa(S.eq_graph, LV_OPA_COVER, 0);
        if (on) {
            static const char *const mark[3] = {"100", "1k", "10k"};
            static const double at[3] = {100, 1000, 10000};
            for (int i = 0; i < 3; i++) {
                style_label(S.eq_lbl[i], S.f->meta, T()->text_muted);
                lv_obj_set_width(S.eq_lbl[i], 40);
                lv_obj_set_style_text_align(S.eq_lbl[i], LV_TEXT_ALIGN_CENTER, 0);
                set_text(S.eq_lbl[i], mark[i]);
                /* above the graph's foot, whatever the font's height (the pixel one is taller) */
                int y = BAT_GRAPH_H - lv_font_get_line_height(S.f->meta);
                lv_obj_set_pos(S.eq_lbl[i], eqg_x(at[i]) - 20, y > EQG_Y1 + 1 ? EQG_Y1 + 1 : y);
            }
            style_label(S.eq_warn, S.f->meta, T()->warning);
            lv_obj_set_style_bg_color(S.eq_warn, C(T()->bg), 0);   /* readable over the grid */
            lv_obj_set_style_bg_opa(S.eq_warn, LV_OPA_COVER, 0);
            lv_obj_set_style_pad_hor(S.eq_warn, 3, 0);
            S.eqg_have = 0;                         /* (a profile opened anew, or another theme) */
            eqg_update();
            lv_obj_move_foreground(S.eq_graph);
            lv_obj_invalidate(S.eq_graph);
        }
    }
    return on;
}
