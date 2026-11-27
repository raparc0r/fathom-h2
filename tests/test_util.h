/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/* What the tests share: the checks and their count, and the small things
 * nearly every test did for itself (a scratch folder, a file read or written
 * whole, a short sleep). Every test had its own copy of these few lines.
 *
 *     CHECK(cond);                          the condition itself is the message
 *     CHECKF(cond, "why: %d", n);           a message of the test's own
 *     CHECK_STR(got, want);                 two strings, both shown when they differ
 *     return t_result();                    main()'s last line: the summary and the exit status
 *
 * A check that fails prints "FAIL file.c:123: what" and counts in t_fails; the
 * test goes on. The line goes to stdout, among whatever else the test says and
 * in its order, and is flushed at once: a test that crashes later has still
 * said what failed. tools/h2-tests.sh picks a failed test's lines out of its
 * output by that shape ("[.]c:[0-9]+: " or "FAIL"): keep both in it.
 *
 * (The sqlite helpers are in test_db.h and the FLAC files the tests make in
 * test_flac.h: only the tests that use those libraries include them.) */
#ifndef FATHOM_TEST_UTIL_H
#define FATHOM_TEST_UTIL_H

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int t_fails;                              /* the checks that failed */

#define CHECK(c) do { if (!(c)) { \
    t_fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fflush(stdout); } } while (0)
#define CHECKF(c, ...) do { if (!(c)) { \
    t_fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); putchar('\n'); fflush(stdout); } } while (0)
#define CHECK_STR(got, want) do { if (strcmp((got), (want))) { \
    t_fails++; printf("FAIL %s:%d: got \"%s\", want \"%s\"\n", __FILE__, __LINE__, (got), (want)); fflush(stdout); } } while (0)

/* The test's last word, and main()'s return: "name: ok" and 0, or how many
 * checks failed and 1 (name: the file's, without "test_" and ".c"). */
static inline int t_result_(const char *file)
{
    const char *name = strrchr(file, '/');
    name = name ? name + 1 : file;
    if (!strncmp(name, "test_", 5)) name += 5;
    printf("%.*s: ", (int)strcspn(name, "."), name);
    if (t_fails) printf("%d failed\n", t_fails);
    else printf("ok\n");
    return t_fails != 0;
}
#define t_result() t_result_(__FILE__)

static inline void t_sleep_ms(int ms)
{
    struct timespec t = {ms / 1000, (long)(ms % 1000) * 1000000};
    nanosleep(&t, NULL);
}

/* The most memory the process has held, in KB. */
static inline long t_peak_kb(void)
{
    struct rusage u;
    getrusage(RUSAGE_SELF, &u);
#ifdef __APPLE__
    return (long)(u.ru_maxrss / 1024);
#else
    return (long)u.ru_maxrss;
#endif
}

/* ---------- files ---------- */

/* A scratch folder of the test's own, "<name>-XXXXXX" under $TMPDIR, or /tmp
 * (on the H2, /tmp is RAM: tools/h2-tests.sh points TMPDIR at the card); its
 * path in out, which is returned. Under the real path of that folder: macOS's
 * own TMPDIR is under /var, a link to /private/var, and the library compares
 * the paths it's given with those it finds. A test that can't have one has
 * failed: it says so and ends there. */
static inline char *t_mkdtemp(char *out, size_t cap, const char *name)
{
    const char *t = getenv("TMPDIR");
    char real[PATH_MAX];
    if (!t || !*t) t = "/tmp";
    snprintf(out, cap, "%s/%s-XXXXXX", realpath(t, real) ? real : t, name);
    if (!mkdtemp(out)) {
        printf("FAIL: no scratch folder %s: %s\n", out, strerror(errno));
        exit(1);
    }
    return out;
}

/* A folder and all in it, gone (rm -rf). What rm returned: 0 when it's done. */
static inline int t_rm_rf(const char *path)
{
    char cmd[PATH_MAX + 16];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", path);
    return system(cmd);
}

static inline int t_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

/* A file's length in bytes (-1: there's none). */
static inline long t_file_size(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 ? (long)st.st_size : -1;
}

/* A whole file, a 0 after its last byte, its length in *len (len may be NULL);
 * free() it. NULL when it can't be read: what then is the caller's to say. */
static inline char *t_slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = n >= 0 ? malloc((size_t)n + 1) : NULL;
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    fclose(f);
    if (b) {
        b[n] = '\0';
        if (len) *len = (size_t)n;
    }
    return b;
}

/* A file made, or made again, holding this text. 0, or -1 when it couldn't be
 * written whole. */
static inline int t_write_text(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    int bad = fputs(text, f) < 0;
    return fclose(f) || bad ? -1 : 0;
}

/* A file copied. 0, or -1 when either can't be opened or the copy is short. */
static inline int t_copy(const char *from, const char *to)
{
    FILE *a = fopen(from, "rb"), *b = fopen(to, "wb");
    char buf[16384];
    size_t n;
    int bad = !a || !b;
    while (!bad && (n = fread(buf, 1, sizeof buf, a)) > 0) bad = fwrite(buf, 1, n, b) != n;
    if (a) fclose(a);
    if (b && fclose(b)) bad = 1;
    return bad ? -1 : 0;
}

#endif
