/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* Files read in a helper process.
 *
 * Reading a file's tags or its cover runs code that a file can crash, hang or
 * drive out of memory (a cover of 12 MB in each song, three covers in one
 * file: the H2 has about 10 MB to spare). Done in the player, that took the
 * player with it: the screen back to the logo, the music stopped, once or
 * twice for each such file before it was set aside. So the scan asks a
 * helper instead: the player's own program started with --reader, which
 * reads one file at a time and answers over a pipe. A file that kills the
 * helper, or that it hasn't answered for after a minute, costs the helper:
 * the player starts another, asks once more (a death can be chance), and
 * then leaves the file out and carries on.
 *
 * The helper isn't favoured by the kernel when memory runs out, nor marked
 * to go first: whichever of the two is the bigger goes, so a file that needs
 * 12 MB takes the helper, and a player grown large on a huge library is
 * still the one that's restarted (and not the helper, over and over, with
 * blameless files left out).
 *
 * And it is told, with each file, how much memory it may take for it (what
 * is available less a reserve: the caller's sum). Without that a file that
 * asked for more than there is took the H2 to its last few hundred KB
 * before the kernel stepped in, some seconds later: the player's own code
 * was pushed out of memory meanwhile, and music that was playing broke up
 * (101 dropouts in a minute, on the H2, 2026-11-24). With it, what such a
 * file asks for is refused at once, and it is one more file that can't be
 * read.
 *
 * With no helper set (the simulator, most tests) the work is done in the
 * caller's process, as before. */
#ifndef FATHOM_READER_H
#define FATHOM_READER_H

#include <stddef.h>

#define READER_DIED (-2)   /* the helper died on this file twice running, or hung on it */
#define READER_NONE (-3)   /* no helper could be started: do it here */

/* The program to run as the helper (it gets the one argument --reader);
 * NULL or "": none. */
void reader_set_helper(const char *exe);
int  reader_on(void);

/* `op` on `path`, done by the helper with at most budget_kb more memory
 * than it has (0: no limit): 0 with its status and its answer (*out
 * malloc'd, *len bytes; NULL when it has none), READER_DIED, or READER_NONE.
 * One at a time (others wait). */
int reader_call(int op, const char *path, long budget_kb, int *status, unsigned char **out, size_t *len);

/* The helper sent away (the end of a scan): the next call starts another. */
void reader_close(void);

/* The helper's side: serves requests from stdin with fn until the player
 * closes the pipe or is gone. fn does `op` on `path` and returns its status,
 * with an answer in *out (malloc'd) if it has one. */
typedef int (*reader_fn)(int op, const char *path, unsigned char **out, size_t *len);
int reader_serve(reader_fn fn);

/* How long a file may take before the helper is taken to have hung (60 s) */
void reader_set_timeout_ms(int ms);
/* Helpers that died or hung, and helpers started, since the program began */
int reader_deaths(void);
int reader_starts(void);

#endif
