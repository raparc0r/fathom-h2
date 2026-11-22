/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/*
 * FFmpeg's transforms (libavutil/tx.c) list their codelets for every sample
 * type, so a static link pulls in the float and double ones (480 KB of code,
 * 16 MB of lazily filled tables) though only integer decoders are built (the
 * build is soft-float): AAC's fixed-point MDCT uses the int32 codelets. Defining
 * the two lists here, empty, keeps tx_float.o and tx_double.o out of the
 * player; a float transform would simply not be found (av_tx_init fails), and
 * nothing built asks for one. If FFmpeg ever pulled those objects in for
 * something else, the link would fail on the duplicates rather than misbehave.
 */
#include <stddef.h>

const void *const ff_tx_codelet_list_float_c[] = {NULL};
const void *const ff_tx_codelet_list_double_c[] = {NULL};
