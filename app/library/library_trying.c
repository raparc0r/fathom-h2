/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* The library's "trying" notes: what a scan, the covers' pass or the
 * levelling is reading, noted before it is read, so that a file the player
 * (or the H2 itself) stopped on is found at the next start and set aside
 * after the second time. Out of library.c, as it was there. */
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE
#define _GNU_SOURCE
#include "library/library_int.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

/* ---------- what the player died on ----------
 * A file whose tags or cover crash the reader, or take all the memory there is
 * (the player is killed), did so again at every start: the scan picks up where
 * it left off, at the same file. Before reading one, its name is noted in a
 * file in RAM (library_set_trying_dir: /tmp on the H2); done, the note goes.
 * A note still there when the next scan starts means the player died on it.
 * Once could be chance (the player killed for want of memory while reading a
 * blameless file): it's tried again. Twice running at the same one: that file
 * is set aside as unreadable (as one that can't be read is: tried again when
 * it changes), that album as having no cover, and the scan carries on.
 *
 * A file that stops the H2 itself (it hangs until the watchdog restarts it)
 * takes the RAM with it. So a run also leaves a mark on the card
 * (library_set_trying_keep: an empty file, written once, when the run reads
 * its first item, and removed at its end). A mark found with no note in RAM
 * means the H2 went down during the last run: this run is "careful", and
 * writes each item's name into the mark before reading it (a write to the
 * card per item: only ever after such a stop). A careful run's mark found at
 * the next start names the item the H2 went down on: once, twice, as above. */
static char g_trying_dir[256];
static char g_keep_dir[256];

#define RAM_PRE  "fathom-trying-"
#define KEEP_PRE "scan-reading-"

/* The runs that leave a mark on the card (each on a thread of its own) */
static struct keep { const char *what; int careful, marked, fd; } g_keep[] = {
    {"tags", 0, 0, -1}, {"cover", 0, 0, -1}, {"level", 0, 0, -1},
};

void library_set_trying_dir(const char *dir)
{
    snprintf(g_trying_dir, sizeof g_trying_dir, "%s", dir ? dir : "");
}

void library_set_trying_keep(const char *dir)
{
    snprintf(g_keep_dir, sizeof g_keep_dir, "%s", dir ? dir : "");
    for (size_t i = 0; i < sizeof g_keep / sizeof g_keep[0]; i++) {
        if (g_keep[i].fd >= 0) close(g_keep[i].fd);
        g_keep[i].careful = g_keep[i].marked = 0;
        g_keep[i].fd = -1;
    }
}

static struct keep *keep_slot(const char *what)
{
    if (!g_keep_dir[0]) return NULL;
    for (size_t i = 0; i < sizeof g_keep / sizeof g_keep[0]; i++)
        if (!strcmp(g_keep[i].what, what)) return &g_keep[i];
    return NULL;
}

static void note_path(const char *dir, const char *pre, const char *what, const char *sfx, char *out, size_t cap)
{
    snprintf(out, cap, "%s/%s%s%s", dir, pre, what, sfx);
}

/* A note's text (1; 0: no note, or an empty one) */
static int note_read(const char *p, char *out, size_t cap)
{
    int fd = open(p, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    ssize_t n = read(fd, out, cap - 1);
    close(fd);
    if (n <= 0) return 0;
    out[n] = '\0';                                    /* (the text ends at its own NUL, if earlier) */
    return out[0] != '\0';
}

static void dir_sync(const char *dir)
{
    int fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return;
    fsync(fd);
    close(fd);
}

/* (durable: on the card, to be there after a reset) */
static void note_write(const char *dir, const char *p, const char *text, int durable)
{
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return;
    if (write(fd, text, strlen(text) + 1) < 0) { /* (no note: nothing set aside) */ }
    if (durable) fsync(fd);
    close(fd);
    if (durable) dir_sync(dir);
}

/* A run stopped on `text`: 2 when the stop before it was on the same one
 * (the note WHAT.1, forgotten now), else 1, and it's remembered */
static int note_again(const char *dir, const char *pre, const char *what, const char *text, int durable)
{
    char p[400], before[2048] = "";
    note_path(dir, pre, what, ".1", p, sizeof p);
    if (note_read(p, before, sizeof before) && !strcmp(before, text)) {
        unlink(p);
        if (durable) dir_sync(dir);
        return 2;
    }
    note_write(dir, p, text, durable);
    return 1;
}

/* What the last run of `what` stopped on, its text in out: 0 nothing, 1 for
 * the first time (to be tried once more), 2 twice running (to be set aside).
 * Called once as each run starts: it also says whether this one is careful. */
int trying_left(const char *what, char *out, size_t cap)
{
    char p[400];
    struct keep *k = keep_slot(what);
    if (cap < 2) return 0;
    if (k) k->careful = 0;
    if (g_trying_dir[0]) {                            /* the player died (the H2 stayed up) */
        note_path(g_trying_dir, RAM_PRE, what, "", p, sizeof p);
        int got = note_read(p, out, cap);
        unlink(p);
        if (got) {
            int r = note_again(g_trying_dir, RAM_PRE, what, out, 0);
            if (r == 1) fprintf(stderr, "library: the player stopped while on %.200s (%s): once more\n", out, what);
            return r;
        }
    }
    if (!k) return 0;
    note_path(g_keep_dir, KEEP_PRE, what, "", p, sizeof p);
    if (access(p, F_OK) != 0) return 0;
    k->careful = 1;                                   /* the H2 itself went down during the last run */
    if (!note_read(p, out, cap)) {
        fprintf(stderr, "library: the H2 stopped during the last run (%s): each item is noted on the card this time\n", what);
        return 0;
    }
    int r = note_again(g_keep_dir, KEEP_PRE, what, out, 1);
    if (r == 1) fprintf(stderr, "library: the H2 stopped while on %.200s (%s): once more\n", out, what);
    return r;
}

/* What a run stopped on twice running (1, its text in out, and forgotten) */
int library_trying_left(const char *what, char *out, size_t cap)
{
    return trying_left(what, out, cap) == 2;
}

/* A note kept open while a run of files is read: *fd -1 before the first */
void library_trying(const char *what, int *fd, const char *text)
{
    if (g_trying_dir[0]) {
        if (*fd < 0) {
            char p[400];
            note_path(g_trying_dir, RAM_PRE, what, "", p, sizeof p);
            *fd = open(p, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        }
        if (*fd >= 0 && pwrite(*fd, text, strlen(text) + 1, 0) < 0) { /* (no note: nothing set aside) */ }
    }
    struct keep *k = keep_slot(what);
    if (!k) return;
    int made = 0;
    if (!k->marked) {                                 /* the run's mark on the card, once */
        char p[400];
        note_path(g_keep_dir, KEEP_PRE, what, "", p, sizeof p);
        k->marked = 1;                                /* (a card that can't be written: not tried per item) */
        k->fd = open(p, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (k->fd < 0) return;
        made = 1;
        if (!k->careful) {
            fsync(k->fd);
            close(k->fd);
            k->fd = -1;
            dir_sync(g_keep_dir);
            return;
        }
    }
    if (k->careful && k->fd >= 0) {
        if (pwrite(k->fd, text, strlen(text) + 1, 0) < 0) { /* (as above) */ }
        fsync(k->fd);
        if (made) dir_sync(g_keep_dir);
    }
}

void library_trying_done(const char *what, int *fd)
{
    char p[400];
    if (g_trying_dir[0]) {
        if (*fd >= 0) close(*fd);
        *fd = -1;
        note_path(g_trying_dir, RAM_PRE, what, "", p, sizeof p);
        unlink(p);
    }
    struct keep *k = keep_slot(what);
    if (!k) return;
    if (k->fd >= 0) close(k->fd);
    k->fd = -1;
    int gone = 0;                                     /* (a run that read nothing left no mark: nothing written) */
    note_path(g_keep_dir, KEEP_PRE, what, "", p, sizeof p);
    gone |= unlink(p) == 0;
    if (k->careful) {                                 /* (a careful run that ended: its "once" forgotten too) */
        note_path(g_keep_dir, KEEP_PRE, what, ".1", p, sizeof p);
        gone |= unlink(p) == 0;
    }
    if (gone) dir_sync(g_keep_dir);
    k->marked = k->careful = 0;
}
