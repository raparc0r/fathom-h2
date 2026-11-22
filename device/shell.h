/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* What the player's workers (wifi.c, bt.c, share.c, ssh.c) need to run the
 * system's scripts and read the network's state. Safe from any thread. */
#ifndef FATHOM_SHELL_H
#define FATHOM_SHELL_H

#include <stddef.h>

/* Runs a shell command; its first `cap` bytes of output to out (NULL: none
 * kept, all read). Returns the exit status, -1 if it couldn't run. */
int  sh_run(const char *cmd, char *out, size_t cap);
/* s as one single-quoted shell word ('it'\''s), cut to fit cap. */
void sh_quote(const char *s, char *out, size_t cap);
/* An interface's IPv4 address, "" without one. */
void if_ip(const char *name, char *out, size_t cap);

#endif
