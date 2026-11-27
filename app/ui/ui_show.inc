/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/*
 * The screens by name, for the simulator's pictures and checks: show_named()
 * opens "albums", "eqedit", "rdirresults" and the rest as a person would reach
 * them (ui_show() in ui.c has first gone back to Home). A piece of ui.c, read
 * into it where ui_show() is and only in the simulator's build (FATHOM_UI_SHOW:
 * sim/CMakeLists.txt): it uses what ui.c keeps to itself, and the player has
 * no use for it (it asks for one screen, "nowplaying", which ui.c knows).
 */

/* Screenshots: the directory's answer (saved ones, in the simulator) before the picture. */
static void rd_wait(void)
{
    for (int i = 0; i < 300 && S.rd_job; i++) {
        if (!rd_poll()) break;
        struct timespec t = {0, 10 * 1000 * 1000L};
        nanosleep(&t, NULL);
    }
}

static void show_named(const char *what)
{
    if (!strcmp(what, "albums") || !strcmp(what, "tracks")) {
        push_ui(L_ALBUMS, "Albums", 0);
        level_t *l = &S.stack[S.depth - 1];
        l->sel = l->items.n > 1 ? 1 : 0;
        char name[128];
        if (!strcmp(what, "tracks") && l->items.n && S.lib &&
            library_album_name(S.lib, l->items.ids[l->sel], name, sizeof name) == 0)
            push(L_TRACKS, name, l->items.ids[l->sel]);
    } else if (!strcmp(what, "playlists") || !strcmp(what, "playlist") || !strncmp(what, "smart", 5)) {
        /* Playlists: the third of yours ("playlists"), opened ("playlist"); the
         * first smart one ("smartlists"), opened ("smartopen"), its menu ("smartmenu") */
        S.stack[0].sel = home_index(H_PLAYLISTS);
        push_ui(L_PLAYLISTS, "Playlists", 0);
        level_t *l = &S.stack[S.depth - 1];
        int first = 0;
        while (first < l->items.n && l->items.ids[first] < 0) first++;   /* (the smart ones first) */
        int smart = !strncmp(what, "smart", 5);
        l->sel = smart ? 0 : first + 2 < l->items.n ? first + 2 : first;
        if (l->sel >= l->items.n) l->sel = l->items.n > 0 ? l->items.n - 1 : 0;   /* (only smart ones: none of yours) */
        char name[128];
        if (!strcmp(what, "playlist") && l->sel < l->items.n && S.lib &&
            library_playlist_name(S.lib, l->items.ids[l->sel], name, sizeof name) == 0)
            push(L_PLAYLIST_TRACKS, name, l->items.ids[l->sel]);
        if (!strcmp(what, "smartopen") && l->items.n && l->items.ids[0] < 0)
            push(L_SMART_TRACKS, tr(library_smart_name((int)-l->items.ids[0])), -l->items.ids[0]);
        if (!strcmp(what, "smartmenu")) open_context();
    } else if (!strcmp(what, "share")) {
        S.stack[0].sel = home_index(H_SETTINGS);
        push_ui(L_SETTINGS, "Settings", 0);
        S.stack[S.depth - 1].sel = SET_SHARE;
        push_ui(L_SHARE, "Network drive", 0);
        S.stack[S.depth - 1].sel = 1;
    } else if (!strcmp(what, "ssh")) {
        S.stack[0].sel = home_index(H_SETTINGS);
        push_ui(L_SETTINGS, "Settings", 0);
        S.stack[S.depth - 1].sel = SET_SSH;
        push_ui(L_SSH, "SSH", 0);
        S.stack[S.depth - 1].sel = 1;
    } else if (!strcmp(what, "about") || !strcmp(what, "update")) {
        S.stack[0].sel = home_index(H_SETTINGS);
        push_ui(L_SETTINGS, "Settings", 0);
        S.stack[S.depth - 1].sel = SET_ABOUT;
        push_ui(L_ABOUT, "About", 0);
        level_t *l = &S.stack[S.depth - 1];
        l->sel = S.about_n;                         /* Check for updates (or what follows) */
        if (!strcmp(what, "update")) push_ui(L_UPDATE, "Updates", 0);
        ui_update_changed();
    } else if (!strcmp(what, "clockset")) {         /* About > Clock, set by hand: on the day */
        S.stack[0].sel = home_index(H_SETTINGS);
        push_ui(L_SETTINGS, "Settings", 0);
        S.stack[S.depth - 1].sel = SET_ABOUT;
        push_ui(L_ABOUT, "About", 0);
        clock_open();
        CK.zone = tz_find("Paris, Berlin");
        S.clk = (dt_t){2026, 11, 22, 18, 5};          /* the day being changed */
        clock_key(UI_KEY_DOWN);
        clock_key(UI_KEY_DOWN);
        clock_key(UI_KEY_OK);
    } else if (!strcmp(what, "eqedit") || !strcmp(what, "eqband")) {
        /* the EQ editor on a profile, its curve over it: the 8.9 kHz band selected,
         * or its own page with the gain being turned up */
        eqe_open("AirPods Pro");
        level_t *e = &S.stack[S.depth - 1];
        int b = 0;
        for (int i = 0; i < S.eqe.n; i++)
            if (S.eqe.band[i].fc > 8000 && S.eqe.band[i].fc < 9000) b = i;
        e->sel = b + 1;
        if (!strcmp(what, "eqband")) {
            S.eqe.band[b].gain_db = 8;
            push(L_EQ_BAND, "Peak 8.9 kHz", b);
            S.stack[S.depth - 1].sel = 2;
            S.editing = 1;
        }
    } else if (!strcmp(what, "podsettings")) {      /* Settings > Podcasts */
        S.stack[0].sel = home_index(H_SETTINGS);
        push_ui(L_SETTINGS, "Settings", 0);
        S.stack[S.depth - 1].sel = SET_PODCASTS;
        push_ui(L_POD_SETTINGS, "Podcasts", 0);
    } else if (!strcmp(what, "wifi")) {             /* Settings > Wi-Fi */
        S.stack[0].sel = home_index(H_SETTINGS);
        push_ui(L_SETTINGS, "Settings", 0);
        S.stack[S.depth - 1].sel = SET_WIFI;
        push_ui(L_WIFI, "Wi-Fi", 0);
    } else if (!strcmp(what, "bluetooth")) {        /* Settings > Bluetooth */
        S.stack[0].sel = home_index(H_SETTINGS);
        push_ui(L_SETTINGS, "Settings", 0);
        S.stack[S.depth - 1].sel = SET_BLUETOOTH;
        push_ui(L_BLUETOOTH, "Bluetooth", 0);
    } else if (!strcmp(what, "settings")) {         /* Settings: its pages */
        S.stack[0].sel = home_index(H_SETTINGS);
        push_ui(L_SETTINGS, "Settings", 0);
    } else if (!strcmp(what, "libsettings")) {      /* Settings > Library, on Find missing covers */
        S.stack[0].sel = home_index(H_SETTINGS);
        push_ui(L_SETTINGS, "Settings", 0);
        S.stack[S.depth - 1].sel = SET_LIBRARY;
        push_ui(L_LIBRARY, "Library", 0);
        S.stack[S.depth - 1].sel = 1;
    } else if (!strcmp(what, "libunread") || !strcmp(what, "unread") || !strcmp(what, "unreadwhere")) {
        /* Settings > Library on Files not read (its last row, there while there are any);
         * the list it opens; Play on a file there (where it is) */
        S.stack[0].sel = home_index(H_SETTINGS);
        push_ui(L_SETTINGS, "Settings", 0);
        S.stack[S.depth - 1].sel = SET_LIBRARY;
        push_ui(L_LIBRARY, "Library", 0);
        S.stack[S.depth - 1].sel = list_count(&S.stack[S.depth - 1]) - 1;
        if (strcmp(what, "libunread") && S.lib_unread > 0) {
            push_ui(L_UNREAD, "Files not read", 0);
            if (!strcmp(what, "unreadwhere")) open_selected();
        }
    } else if (!strcmp(what, "covermenu") || !strcmp(what, "coverfind") || !strcmp(what, "covermenuown")) {
        /* an album without a cover: Menu on it ("covermenu"), Find cover's first match ("coverfind");
         * one with its own: Menu on it ("covermenuown") */
        int want = !strcmp(what, "covermenuown") ? LIB_COVER_OWN : LIB_COVER_NONE;
        S.stack[0].sel = home_index(H_ALBUMS);
        push_ui(L_ALBUMS, "Albums", 0);
        level_t *l = &S.stack[S.depth - 1];
        for (int i = 0; S.lib && i < l->items.n; i++)
            if (library_album_cover(S.lib, l->items.ids[i]) == want) { l->sel = i; break; }
        if (strcmp(what, "coverfind")) {
            open_context();
            for (int i = 0; i < S.ctx_n; i++)
                if (S.ctx[i] == CTX_FIND_COVER || S.ctx[i] == CTX_FIND_ANOTHER) S.stack[S.depth - 1].sel = i;
        } else if (l->items.n) {
            cover_find_open(l->items.ids[l->sel], 0);
            for (int i = 0; i < 300; i++) {          /* (the simulator's saved answers, and the picture) */
                int n = 0;
                cf_poll();
                if (cf_find_state(&n) == 1 && n && cf_find_show(0, NULL, NULL) != 0) break;
                struct timespec ts = {0, 10 * 1000 * 1000L};
                nanosleep(&ts, NULL);
            }
        }
    } else if (!strcmp(what, "albummenu") || !strcmp(what, "artistmenu") || !strcmp(what, "aartistmenu")) {
        /* Menu on an album (Play, Shuffle, ...), an artist in Artists, one in Album artists */
        int art = !strcmp(what, "artistmenu"), aart = !strcmp(what, "aartistmenu");
        S.stack[0].sel = home_index(art ? H_ARTISTS : aart ? H_ALBUM_ARTISTS : H_ALBUMS);
        push_ui(art ? L_ARTISTS : aart ? L_ALBUM_ARTISTS : L_ALBUMS, art ? "Artists" : aart ? "Album artists" : "Albums", 0);
        S.stack[S.depth - 1].sel = S.stack[S.depth - 1].items.n > 2 ? 2 : 0;
        open_context();
    } else if (!strncmp(what, "pod", 3)) {
        /* Podcasts (the simulator's sample shows): "podcasts" the list, "podshow" a
         * show's episodes, "podsearch" Find podcasts' answer, "podnp" an episode playing */
        S.stack[0].sel = home_index(H_PODCASTS);
        pod_open();
        if (!strcmp(what, "podshow") || !strcmp(what, "podnp") || !strcmp(what, "podchapters") || !strcmp(what, "podmenu")) {
            S.stack[S.depth - 1].sel = 1;
            pod_open_show(0);
            S.stack[S.depth - 1].sel = S.pod_ne > 1 ? 1 : 0;
        }
        if ((!strcmp(what, "podnp") || !strcmp(what, "podchapters") || !strcmp(what, "podmenu")) && S.pod_ne > 1 && S.lib) {   /* (a song of the library stands in for its file) */
            lib_list_t songs;
            lib_track_t t;
            if (library_list(S.lib, LIB_SONGS, 0, &songs) > 0 && library_track(S.lib, songs.ids[0], &t) == 0) {
                player_episode_t pe = {0};
                snprintf(pe.show, sizeof pe.show, "%s", S.pod_shows[0].title);
                snprintf(pe.title, sizeof pe.title, "%s", S.pod_eps[1].title);
                snprintf(pe.path, sizeof pe.path, "%s", t.path);
                snprintf(pe.id, sizeof pe.id, "%s", S.pod_shows[0].id);
                snprintf(pe.key, sizeof pe.key, "%s", S.pod_eps[1].key);
                pe.len_ms = (uint32_t)S.pod_eps[1].dur_s * 1000;
                pe.start_ms = 23 * 60 * 1000;
                S.pod_nch = pod_chapters(pod_dir(), pe.id, &S.pod_eps[1], S.pod_ch, 64);
                snprintf(S.pod_ch_key, sizeof S.pod_ch_key, "%s", pe.key);
                player_episode_play(S.player, &pe);
                player_pause(S.player, 1);
                snprintf(S.pod_play.id, sizeof S.pod_play.id, "%s", pe.id);
                snprintf(S.pod_play.key, sizeof S.pod_play.key, "%s", pe.key);
                S.now_playing_view = 1;
            }
            if (songs.ids) library_list_free(&songs);
            if (!strcmp(what, "podmenu")) {             /* Menu on it: Chapters, Speed (at 1.25x) */
                player_set_speed(S.player, 125);
                player_now(S.player, &S.now);
                open_context();
                S.stack[S.depth - 1].sel = 1;
            }
            if (!strcmp(what, "podchapters") && S.pod_nch) {
                S.now_playing_view = 0;
                push_ui(L_POD_CHAPTERS, "Chapters", 0);
                S.stack[S.depth - 1].sel = 1;
            }
        }
        if (!strcmp(what, "podsearch")) {
            S.stack[S.depth - 1].sel = 0;
            snprintf(S.entry, sizeof S.entry, "radiolab");
            S.entry_back = 0;
            S.entry_ch = 0;
            push_ui(L_POD_SEARCH, "Find podcasts", 0);
            pod_search_run();
            for (int i = 0; i < 300 && S.pod_search == 0; i++) {
                pod_poll();
                struct timespec ts = {0, 10 * 1000 * 1000L};
                nanosleep(&ts, NULL);
            }
        }
    } else if (!strncmp(what, "theme", 5) && strcmp(what, "themes")) {
        /* the theme editor, as in its mockups: a new dark theme with a coral accent, "Harbour";
         * "themesnew": the list's New theme row; "themecolours": its colours, Text selected;
         * "themecolour": Text being picked, at Dark grey (bad contrast); "themesave": Save */
        if (!strcmp(what, "themesnew")) {
            push_ui(L_THEMES, "Theme", 0);
            S.stack[S.depth - 1].sel = S.n_themes;
        } else {
            the_open(-1);
            for (S.the_acc = 0; strcmp(ACCENTS[S.the_acc].name, "Coral"); S.the_acc++) {}
            theme_make(&S.the, 1, 215, 30);
            theme_accent(&S.the, ACCENTS[S.the_acc].hue, ACCENTS[S.the_acc].sat);
            snprintf(S.the.name, sizeof S.the.name, "Harbour");
            level_t *e = &S.stack[S.depth - 1];
            snprintf(e->title, sizeof e->title, "%s", S.the.name);
            e->sel = !strcmp(what, "themesave") ? TE_SAVE : TE_ACCENT;
            if (!strcmp(what, "themecolours") || !strcmp(what, "themecolour")) {
                push_ui(L_THEME_COLOURS, "Colours", 0);
                S.stack[S.depth - 1].sel = TC_TEXT;
            }
            if (!strcmp(what, "themecolour")) {
                S.the_pal = 4;                               /* Dark grey */
                S.the.text = the_pal_at(S.the_pal, NULL, 0);
                S.editing = 1;
            }
        }
    } else if (!strcmp(what, "packs")) {            /* Settings > Interface > Language packs */
        S.stack[0].sel = home_index(H_SETTINGS);
        push_ui(L_SETTINGS, "Settings", 0);
        S.stack[S.depth - 1].sel = SET_INTERFACE;
        push_ui(L_INTERFACE, "Interface", 0);
        S.stack[S.depth - 1].sel = list_count(&S.stack[S.depth - 1]) - 1;
        push_ui(L_PACKS, "Language packs", 0);
        ui_packs_changed(0);
    } else if (!strcmp(what, "uninstall")) {
        S.stack[0].sel = home_index(H_SETTINGS);
        push_ui(L_SETTINGS, "Settings", 0);
        S.stack[S.depth - 1].sel = SET_ABOUT;
        push_ui(L_ABOUT, "About", 0);
        push_ui(L_UNINSTALL, "Uninstall Fathom", 0);
        ui_uninstall_changed();
    } else if (!strncmp(what, "battery", 7)) {       /* "battery", "battery7", "batterysince", "batteryhistory" */
        S.stack[0].sel = home_index(H_SETTINGS);
        push_ui(L_SETTINGS, "Settings", 0);
        S.stack[S.depth - 1].sel = SET_POWER;
        push_ui(L_POWER, "Power", 0);
        S.bat_days = !strcmp(what, "battery7") ? 7 : 1;
        battery_load();
        push_ui(L_BATTERY, "Battery", 0);
        if (!strcmp(what, "battery7")) S.stack[S.depth - 1].sel = 0;
        if (!strcmp(what, "batterysince")) { S.stack[S.depth - 1].sel = 1; S.bat_span = -1; push_ui(L_BAT_SPAN, "Since full charge", 0); }
        if (!strcmp(what, "batteryhistory")) { S.stack[S.depth - 1].sel = 2; push_ui(L_BAT_HISTORY, "History", 0); }
    } else if (!strcmp(what, "radio")) {
        S.stack[0].sel = home_index(H_RADIO);
        radio_read();
        push_ui(L_RADIO, "Radio", 0);
        if (S.radio_n > 1) S.stack[S.depth - 1].sel = 2;
    } else if (!strncmp(what, "rdir", 4)) {         /* Radio > Find stations, from saved answers */
        S.stack[0].sel = home_index(H_RADIO);
        radio_read();
        push_ui(L_RADIO, "Radio", 0);
        push_ui(L_RDIR, "Find stations", 0);
        level_t *m = &S.stack[S.depth - 1];
        if (!strcmp(what, "rdir")) {
            char v[32];
            snprintf(v, sizeof v, "%s:%s:%s", "tag", "jazz", "Jazz");   /* (as after opening Jazz) */
            settings_set("radio.random", v);
            m->sel = RD_MENU_RANDOM;
        } else if (!strcmp(what, "rdircountries") || !strcmp(what, "rdirerror")) {
            m->sel = RD_MENU_COUNTRIES;
            open_selected();
            rd_wait();
            level_t *c = &S.stack[S.depth - 1];
            for (int i = 0; i < S.rd_cty_n; i++)
                if (!strcmp(S.rd_cty[i].code, "GB")) c->sel = i;
        } else if (!strcmp(what, "rdirsearch")) {
            m->sel = RD_MENU_SEARCH;
            open_selected();
            snprintf(S.entry, sizeof S.entry, "fip");
            S.entry_back = 0;
            S.entry_ch = (int)strlen(SEARCH_CHARS);   /* the wheel on Search */
            S.rd_q = (rdir_query_t){.by = RDIR_NAME, .arg = "fip"};
            rd_ask(&S.rd_q);
            rd_wait();
            S.stack[S.depth - 1].sel = 0;
        } else {                                     /* rdirgenres, rdirresults, rdirbusy, rdirctx */
            m->sel = RD_MENU_GENRES;
            open_selected();
            level_t *g = &S.stack[S.depth - 1];
            for (int i = 0; i < RDIR_N_GENRES; i++)
                if (!strcmp(RDIR_GENRES[i], "Jazz")) g->sel = i;
            if (strcmp(what, "rdirgenres")) {
                open_selected();
                if (strcmp(what, "rdirbusy")) rd_wait();
                S.stack[S.depth - 1].sel = S.rd_n > 1 ? 2 : 0;
                if (!strcmp(what, "rdirctx")) open_context();
            }
        }
    } else if (!strcmp(what, "search")) {
        S.stack[0].sel = home_index(H_SEARCH);
        const char *typed = getenv("FATHOM_SIM_SEARCH");   /* what's typed, else "bu" */
        snprintf(S.entry, sizeof S.entry, "%s", typed ? typed : "bu");   /* (set and empty: nothing typed) */
        S.entry_back = getenv("FATHOM_SIM_CURSOR_BACK") ? atoi(getenv("FATHOM_SIM_CURSOR_BACK")) : 0;   /* (the cursor that far from the end) */
        S.entry_ch = 'e' - 'a';
        search_run();
        push_ui(L_SEARCH, "Search", 0);
        if (getenv("FATHOM_SIM_SEARCH_ROW")) {            /* the typing left for the results: that row selected */
            level_t *sl = &S.stack[S.depth - 1];
            sl->sel = atoi(getenv("FATHOM_SIM_SEARCH_ROW"));
            sl->typed = 1;
        }
    } else if (!strcmp(what, "artists") || !strcmp(what, "artist") || !strcmp(what, "artistsongs")) {
        S.stack[0].sel = home_index(H_ARTISTS);
        push_ui(L_ARTISTS, "Artists", 0);
        level_t *l = &S.stack[S.depth - 1];
        const char *want = getenv("FATHOM_SIM_ARTIST");   /* a name to open, else the third */
        char name[128];
        l->sel = l->items.n > 2 ? 2 : 0;
        for (int i = 0; want && S.lib && i < l->items.n; i++)
            if (library_artist_name(S.lib, l->items.ids[i], name, sizeof name) == 0 && !strcmp(name, want)) l->sel = i;
        if (strcmp(what, "artists") && l->items.n && S.lib &&
            library_artist_name(S.lib, l->items.ids[l->sel], name, sizeof name) == 0) {
            push(L_CREDIT_ALBUMS, name, l->items.ids[l->sel]);
            level_t *a = &S.stack[S.depth - 1];
            if (!strcmp(what, "artistsongs") && a->items.n &&
                library_album_name(S.lib, a->items.ids[0], name, sizeof name) == 0)
                push_credit_tracks(name, a->parent, a->items.ids[0]);
        }
    } else if (!strcmp(what, "albumartists")) {
        S.stack[0].sel = home_index(H_ALBUM_ARTISTS);
        push_ui(L_ALBUM_ARTISTS, "Album artists", 0);
    } else if (!strcmp(what, "homeend")) {    /* Home, at its last items */
        S.stack[0].sel = S.home_n > 0 ? S.home_n - 2 : 0;
    } else if (!strcmp(what, "homeall")) {    /* Settings > Interface > Home screen items: every item, shown or not */
        S.stack[0].sel = home_index(H_SETTINGS);
        push_ui(L_SETTINGS, "Settings", 0);
        push_ui(L_HOME_EDIT, "Home screen items", 0);
        S.stack[S.depth - 1].sel = H_N - 2;
    } else if (!strcmp(what, "genres")) {
        S.stack[0].sel = home_index(H_GENRES);
        push_ui(L_GENRES, "Genres", 0);
    } else if (!strcmp(what, "folders")) {
        S.stack[0].sel = home_index(H_FOLDERS);
        push_ui(L_FOLDER, "Folders", 0);
    } else if (!strcmp(what, "folder")) {     /* the first folder in Folders */
        S.stack[0].sel = home_index(H_FOLDERS);
        push_ui(L_FOLDER, "Folders", 0);
        level_t *l = &S.stack[S.depth - 1];
        char name[128];
        if (l->items.n && l->items.ids[0] < 0 && S.lib &&
            library_folder_name(S.lib, -l->items.ids[0], name, sizeof name) == 0)
            push(L_FOLDER, name, -l->items.ids[0]);
    } else if (!strcmp(what, "years")) {
        S.stack[0].sel = home_index(H_YEARS);
        push_ui(L_YEARS, "Years", 0);
    } else if (!strcmp(what, "scrub")) {       /* Now Playing with the marker moved on */
        S.now_playing_view = 1;
        render(R_SNAP);
        for (int i = 0; i < 12; i++) ui_key(UI_KEY_DOWN);
        finish_transition();
    } else if (!strcmp(what, "usbask")) {
        ui_usb_prompt();
    } else if (!strcmp(what, "wifiask")) {          /* (as from a station, with Wi-Fi off) */
        push_ui(L_RADIO, "Radio", 0);
        push_ui(L_WIFI_ASK, "Turn on Wi-Fi?", 0);
    } else if (!strcmp(what, "interface")) {
        S.stack[0].sel = home_index(H_SETTINGS);
        push_ui(L_SETTINGS, "Settings", 0);
        push_ui(L_INTERFACE, "Interface", 0);
    } else if (!strcmp(what, "sound")) {
        S.stack[0].sel = home_index(H_SETTINGS);
        push_ui(L_SETTINGS, "Settings", 0);
        S.stack[S.depth - 1].sel = SET_SOUND;
        push_ui(L_SOUND, "Sound", 0);
        {   /* on EQ, wherever it is among the rows there are */
            setting_t r[sizeof SOUND_ROWS / sizeof SOUND_ROWS[0]];
            int n = sound_rows(r), at = 0;
            for (int i = 0; i < n; i++)
                if (r[i] == ST_EQ) at = i;
            S.stack[S.depth - 1].sel = at;
        }
    } else if (!strcmp(what, "signal")) {
        S.stack[0].sel = home_index(H_SETTINGS);
        push_ui(L_SETTINGS, "Settings", 0);
        S.stack[S.depth - 1].sel = SET_SOUND;
        push_ui(L_SOUND, "Sound", 0);
        push_ui(L_SIGNAL, "Signal path", 0);
    } else if (!strcmp(what, "homeedit")) {
        S.stack[0].sel = home_index(H_SETTINGS);
        push_ui(L_SETTINGS, "Settings", 0);
        S.stack[S.depth - 1].sel = SET_INTERFACE;
        push_ui(L_INTERFACE, "Interface", 0);
        S.stack[S.depth - 1].sel = 1;                /* (Home screen items) */
        push_ui(L_HOME_EDIT, "Home screen items", 0);
        S.stack[S.depth - 1].sel = 5;
    } else if (!strcmp(what, "songs")) {
        S.stack[0].sel = home_index(H_SONGS);
        push_ui(L_SONGS, "Songs", 0);
    } else if (!strcmp(what, "quick")) {
        open_quick();
    } else if (!strcmp(what, "queue")) {
        S.now_playing_view = 1;
        if (open_context() == 0) run_context(CTX_SHOW_QUEUE);
    } else if (!strcmp(what, "context") || !strcmp(what, "info")) {
        push_ui(L_ALBUMS, "Albums", 0);
        level_t *l = &S.stack[S.depth - 1];
        l->sel = l->items.n > 1 ? 1 : 0;
        char name[128];
        if (l->items.n && S.lib && library_album_name(S.lib, l->items.ids[l->sel], name, sizeof name) == 0) {
            push(L_TRACKS, name, l->items.ids[l->sel]);
            if (open_context() == 0 && !strcmp(what, "info")) run_context(CTX_INFO);
        }
    } else if (!strcmp(what, "themes")) {
        push_ui(L_SETTINGS, "Settings", 0);
        push_ui(L_THEMES, "Theme", 0);
    } else if (!strncmp(what, "lyrics", 6)) {
        /* Lyrics: the song cued, from Now Playing (timed ones: "lyrics", and
         * read ahead with the wheel: "lyricsahead"); its album's second song,
         * from the list (a page: "lyricsplain", and turned on: "lyricsmore");
         * the next album's first, which has none ("lyricsnone") */
        int k = !strcmp(what, "lyricsplain") || !strcmp(what, "lyricsmore") ? 1 : !strcmp(what, "lyricsnone") ? 2 : 0;
        if (S.player) player_now(S.player, &S.now);        /* (the song playing: as drawn, here not yet) */
        if (k == 0 && S.now.active && !S.now.radio && !S.now.podcast && S.now.track.id) {
            S.now_playing_view = 1;
        } else {
            /* (the pictures' later themes: a podcast is "playing" by then, and
             * the cued song's lyrics are opened from its album, not followed) */
            push_ui(L_ALBUMS, "Albums", 0);
            level_t *l = &S.stack[S.depth - 1];
            int al = k == 2 ? 2 : 1;
            l->sel = l->items.n > al ? al : 0;
            char name[128];
            if (!l->items.n || !S.lib || library_album_name(S.lib, l->items.ids[l->sel], name, sizeof name)) return;
            if (!push(L_TRACKS, name, l->items.ids[l->sel])) return;
            l = &S.stack[S.depth - 1];
            if (k == 1 && l->items.n > 1) l->sel = 1;
        }
        if (open_context() == 0) run_context(CTX_LYRICS);
        lyrics_settle();
        if (!strcmp(what, "lyricsahead")) for (int i = 0; i < 3; i++) lyrics_key(UI_KEY_DOWN);
        if (!strcmp(what, "lyricsmore")) for (int i = 0; i < 4; i++) lyrics_key(UI_KEY_DOWN);
        lyrics_still();
    }
}
