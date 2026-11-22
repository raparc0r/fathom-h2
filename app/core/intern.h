/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* String interning: maps strings to dense ids 1..n (0 is reserved for "none"). */
#ifndef FATHOM_INTERN_H
#define FATHOM_INTERN_H

#include <stddef.h>
#include <stdint.h>

typedef struct intern intern_t;

intern_t   *intern_new(void);
void        intern_free(intern_t *in);
/* Returns the id for `s`, adding it if new. Returns 0 on allocation failure. */
uint32_t    intern_id(intern_t *in, const char *s);
const char *intern_str(const intern_t *in, uint32_t id);
uint32_t    intern_count(const intern_t *in);

#endif
