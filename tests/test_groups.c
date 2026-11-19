/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Genres, Composers, Years and Folders (app/library/library.c), on a small
 * generated library in nested folders. */
#include <FLAC/metadata.h>
#include <FLAC/stream_encoder.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <sqlite3.h>

#include "library/library.h"

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

typedef struct { const char *dir, *file, *title, *artist, *album, *genre, *composer, *year; int track; } spec_t;

static const spec_t SPECS[] = {
    {"Ana Ruiz/First Light", "01.flac", "Morning", "Ana Ruiz", "First Light", "Jazz", "", "2019", 1},
    {"Ana Ruiz/First Light", "02.flac", "Noon", "Ana Ruiz", "First Light", "Jazz", "", "2019", 2},
    {"Ana Ruiz/Second", "01.flac", "Evening", "Ana Ruiz", "Second", "jazz", "", "2022", 1},       /* same genre, other case */
    {"Bach Works/Suites", "01.flac", "Suite 1: Prelude", "Some Ensemble", "Suites", "Classical", "J. S. Bach", "2019", 1},
    {"Bach Works/Suites", "02.flac", "Suite 1: Air", "Some Ensemble", "Suites", "Classical", "J. S. Bach", "2019", 2},
    {"Bach Works/Suites", "03.flac", "Bonus", "Some Ensemble", "Suites", "Classical", "Anon", "2019", 3},
    {"", "loose.flac", "Loose Song", "Nobody", "Loose", "", "", "", 1},                             /* at the top */
};
#define N ((int)(sizeof SPECS / sizeof SPECS[0]))

static void tag(FLAC__StreamMetadata *vc, const char *k, const char *v)
{
    if (!v || !*v) return;
    FLAC__StreamMetadata_VorbisComment_Entry en;
    FLAC__metadata_object_vorbiscomment_entry_from_name_value_pair(&en, k, v);
    FLAC__metadata_object_vorbiscomment_append_comment(vc, en, false);
}

static int write_flac(const char *path, const spec_t *sp)
{
    FLAC__StreamEncoder *e = FLAC__stream_encoder_new();
    FLAC__stream_encoder_set_channels(e, 2);
    FLAC__stream_encoder_set_bits_per_sample(e, 16);
    FLAC__stream_encoder_set_sample_rate(e, 44100);
    FLAC__StreamMetadata *vc = FLAC__metadata_object_new(FLAC__METADATA_TYPE_VORBIS_COMMENT);
    char tr[8];
    snprintf(tr, sizeof tr, "%d", sp->track);
    tag(vc, "TITLE", sp->title);
    tag(vc, "ARTIST", sp->artist);
    tag(vc, "ALBUM", sp->album);
    tag(vc, "GENRE", sp->genre);
    tag(vc, "COMPOSER", sp->composer);
    tag(vc, "DATE", sp->year);
    tag(vc, "TRACKNUMBER", tr);
    FLAC__stream_encoder_set_metadata(e, &vc, 1);
    if (FLAC__stream_encoder_init_file(e, path, NULL, NULL) != FLAC__STREAM_ENCODER_INIT_STATUS_OK) return -1;
    static FLAC__int32 pcm[2 * 4410];
    FLAC__stream_encoder_process_interleaved(e, pcm, 4410);
    FLAC__stream_encoder_finish(e);
    FLAC__stream_encoder_delete(e);
    FLAC__metadata_object_delete(vc);
    return 0;
}

static void mkdirs(const char *path)
{
    char p[600];
    snprintf(p, sizeof p, "%s", path);
    for (char *s = p + 1; *s; s++)
        if (*s == '/') { *s = '\0'; mkdir(p, 0755); *s = '/'; }
    mkdir(p, 0755);
}

/* main of row i of list l */
static const char *row(library_t *lib, const lib_list_t *l, int i, char *meta)
{
    static char main[256];
    char m[64];
    library_row(lib, l->view, l->ids[i], main, sizeof main, meta ? meta : m, 64);
    return main;
}

int main(void)
{
    char dir[] = "/tmp/fathom-test-groups-XXXXXX";
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; }
    char music[300], path[700], db[300], art[300], meta[64], name[128];
    snprintf(music, sizeof music, "%s/music", dir);
    for (int i = 0; i < N; i++) {
        snprintf(path, sizeof path, "%s/%s", music, SPECS[i].dir);
        mkdirs(path);
        snprintf(path, sizeof path, "%s/%s%s%s", music, SPECS[i].dir, *SPECS[i].dir ? "/" : "", SPECS[i].file);
        if (write_flac(path, &SPECS[i])) { fprintf(stderr, "cannot write %s\n", path); return 1; }
    }
    snprintf(db, sizeof db, "%s/lib.db", dir);
    snprintf(art, sizeof art, "%s/art", dir);
    library_t *lib = library_open(db, art);
    CHECK(lib && library_scan(lib, music, NULL, NULL) == N);
    lib_list_t l, a;

    /* Genres: Jazz and jazz are one; untagged tracks aren't a genre */
    CHECK(library_list(lib, LIB_GENRES, 0, &l) == 2);
    CHECK(!strcmp(row(lib, &l, 0, meta), "Classical") && !strcmp(meta, "1"));
    CHECK(!strcasecmp(row(lib, &l, 1, meta), "Jazz") && !strcmp(meta, "2"));          /* two albums */
    CHECK(library_list(lib, LIB_GENRE_ALBUMS, l.ids[1], &a) == 2);
    CHECK(!strcmp(row(lib, &a, 0, NULL), "First Light") && !strcmp(row(lib, &a, 1, NULL), "Second"));
    CHECK(library_genre_name(lib, l.ids[0], name, sizeof name) == 0 && !strcmp(name, "Classical"));
    library_list_free(&a);
    library_list_free(&l);

    /* Composers: Anon and J. S. Bach, each on the Suites album */
    CHECK(library_list(lib, LIB_COMPOSERS, 0, &l) == 2);
    CHECK(!strcmp(row(lib, &l, 0, NULL), "Anon") && !strcmp(row(lib, &l, 1, NULL), "J. S. Bach"));
    CHECK(library_list(lib, LIB_COMPOSER_ALBUMS, l.ids[1], &a) == 1 && !strcmp(row(lib, &a, 0, NULL), "Suites"));
    lib_list_t t;
    CHECK(library_list_composer_tracks(lib, l.ids[1], a.ids[0], &t) == 2);
    CHECK(!strcmp(row(lib, &t, 0, NULL), "Suite 1: Prelude") && !strcmp(row(lib, &t, 1, NULL), "Suite 1: Air"));
    library_list_free(&t);
    library_list_free(&a);
    library_list_free(&l);

    /* Years: newest first, untagged left out */
    CHECK(library_list(lib, LIB_YEARS, 0, &l) == 2);
    CHECK(l.ids[0] == 2022 && l.ids[1] == 2019);
    CHECK(!strcmp(row(lib, &l, 1, meta), "2019") && !strcmp(meta, "2"));             /* First Light, Suites */
    CHECK(library_list(lib, LIB_YEAR_ALBUMS, 2019, &a) == 2);
    /* (a year's albums show their artist, not the year again) */
    CHECK(!strcmp(row(lib, &a, 0, meta), "First Light") && !strcmp(meta, "Ana Ruiz"));
    CHECK(!strcmp(row(lib, &a, 1, meta), "Suites") && !strcmp(meta, "Some Ensemble"));
    library_list_free(&a);
    CHECK(library_list(lib, LIB_ALBUMS, 0, &a) > 0);   /* (so do all albums) */
    int found = 0;
    for (int i = 0; i < a.n; i++)
        if (!strcmp(row(lib, &a, i, meta), "First Light")) found = !strcmp(meta, "Ana Ruiz");
    CHECK(found);
    library_list_free(&a);
    library_list_free(&l);

    /* Folders: the top shows the two folders (as -id), then the loose song */
    CHECK(library_list(lib, LIB_FOLDER, 0, &l) == 3);
    CHECK(l.ids[0] < 0 && l.ids[1] < 0 && l.ids[2] > 0);
    CHECK(!strcmp(row(lib, &l, 0, meta), "Ana Ruiz") && !strcmp(meta, "3 songs"));
    CHECK(!strcmp(row(lib, &l, 1, meta), "Bach Works") && !strcmp(meta, "3 songs"));
    CHECK(!strcmp(row(lib, &l, 2, NULL), "Loose Song"));
    CHECK(library_folder_name(lib, -l.ids[0], name, sizeof name) == 0 && !strcmp(name, "Ana Ruiz"));
    CHECK(library_folder_name(lib, 0, name, sizeof name) == 0 && !strcmp(name, "Folders"));
    lib_list_t f;
    CHECK(library_list(lib, LIB_FOLDER, -l.ids[0], &f) == 2);                        /* its two album folders */
    CHECK(!strcmp(row(lib, &f, 0, NULL), "First Light") && !strcmp(row(lib, &f, 1, NULL), "Second"));
    lib_list_t g;
    CHECK(library_list(lib, LIB_FOLDER, -f.ids[0], &g) == 2 && g.ids[0] > 0 && !strcmp(row(lib, &g, 0, NULL), "Morning"));
    library_list_free(&g);
    library_list_free(&f);
    library_list_free(&l);

    /* A rescan with nothing changed keeps the same ids (open lists stay right) */
    CHECK(library_list(lib, LIB_GENRES, 0, &l) == 2);
    int64_t before = l.ids[0];
    library_list_free(&l);
    CHECK(library_scan(lib, music, NULL, NULL) == N);
    CHECK(library_list(lib, LIB_GENRES, 0, &l) == 2 && l.ids[0] == before);
    library_list_free(&l);

    CHECK(!library_needs_tags(lib));
    library_close(lib);

    /* A library from before composers (tags_v 0), with levelling measured on
     * the H2: the next scan reads the composers back and nothing else */
    sqlite3 *raw = NULL;
    CHECK(sqlite3_open(db, &raw) == SQLITE_OK);
    CHECK(sqlite3_exec(raw, "UPDATE tracks SET tags_v=0, composer=NULL, composer_id=NULL, rg_state=2, rg_track=-3.5;"
                            "DELETE FROM composers;", NULL, NULL, NULL) == SQLITE_OK);
    sqlite3_close(raw);
    lib = library_open(db, art);
    CHECK(lib && library_needs_tags(lib));
    CHECK(library_list(lib, LIB_COMPOSERS, 0, &l) == 0);
    library_list_free(&l);
    CHECK(library_scan(lib, music, NULL, NULL) == N);
    CHECK(!library_needs_tags(lib));
    CHECK(library_list(lib, LIB_COMPOSERS, 0, &l) == 2);
    library_list_free(&l);
    library_close(lib);
    CHECK(sqlite3_open(db, &raw) == SQLITE_OK);
    sqlite3_stmt *q = NULL;
    CHECK(sqlite3_prepare_v2(raw, "SELECT COUNT(*) FROM tracks WHERE rg_state=2 AND rg_track=-3.5", -1, &q, NULL) == SQLITE_OK);
    CHECK(sqlite3_step(q) == SQLITE_ROW && sqlite3_column_int(q, 0) == N);   /* the levelling kept */
    sqlite3_finalize(q);
    sqlite3_close(raw);

    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    printf("groups: all good\n");
    return 0;
}
