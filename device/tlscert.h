/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The network drive's own certificate (see tlscert.c). */
#ifndef FATHOM_TLSCERT_H
#define FATHOM_TLSCERT_H

#include <stddef.h>

/* A certificate for host, signed by its own key, in key_path and crt_path:
 * the one there while it's good for another month, else a new one. fp: the
 * start of its SHA-256 fingerprint, as computers show it ("3F 2A 91 0C 77 AB").
 * 0 on success, -1 without it. */
int tlscert_ensure(const char *key_path, const char *crt_path, const char *host, char *fp, size_t cap);

#endif
