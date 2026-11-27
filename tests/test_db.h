/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* What the tests that look into the library's database share: a number asked
 * of it, a statement run on it, each on a connection of its own (the library
 * may have the file open meanwhile). Apart from test_util.h, so that only
 * these tests need sqlite's header. */
#ifndef FATHOM_TEST_DB_H
#define FATHOM_TEST_DB_H

#include <stdint.h>

#include <sqlite3.h>

static inline int64_t t_db_int_(const char *db, const char *sql, int readonly)
{
    sqlite3 *h = NULL;
    sqlite3_stmt *q = NULL;
    int64_t v = -1;
    int rc = readonly ? sqlite3_open_v2(db, &h, SQLITE_OPEN_READONLY, NULL) : sqlite3_open(db, &h);
    if (rc == SQLITE_OK && sqlite3_prepare_v2(h, sql, -1, &q, NULL) == SQLITE_OK && sqlite3_step(q) == SQLITE_ROW)
        v = sqlite3_column_int64(q, 0);
    sqlite3_finalize(q);
    sqlite3_close(h);
    return v;
}

/* The first column of the query's first row, as a number (-1: no row, or the
 * database or the query is no good). */
static inline int64_t t_db_int(const char *db, const char *sql) { return t_db_int_(db, sql, 0); }

/* The same, the database opened for reading only: nothing is made or changed
 * by asking (a database that isn't there stays so). */
static inline int64_t t_db_int_ro(const char *db, const char *sql) { return t_db_int_(db, sql, 1); }

/* The statements run. SQLITE_OK (0), or sqlite's error; -1 when the database can't be opened. */
static inline int t_db_exec(const char *db, const char *sql)
{
    sqlite3 *h = NULL;
    int rc = sqlite3_open(db, &h) == SQLITE_OK ? sqlite3_exec(h, sql, NULL, NULL, NULL) : -1;
    sqlite3_close(h);
    return rc;
}

#endif
