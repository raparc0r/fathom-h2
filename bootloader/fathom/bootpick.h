/* SPDX-License-Identifier: GPL-2.0-or-later */
/* h2boot (Fathom): which kernel to start after an update (bootpick.c). */
#ifndef FATHOM_BOOTPICK_H
#define FATHOM_BOOTPICK_H

/* Before loading /uImage: a kernel an update put there gets two starts to
 * reach the player; on the third, the one before it goes back in place.
 * 1 if it fell back now. */
int fathom_boot_pick(void);
/* /uImage won't load at all: the one before it back in place at once.
 * 0 if it did (load /uImage again), -1 if there's none to go back to. */
int fathom_boot_fall_back(void);

#ifdef BOOTPICK_HOST
void fathom_boot_root(const char *root);   /* (tests: the card's top folder) */
#endif

#endif
