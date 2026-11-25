/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional terms under GPLv3 section 7: see LICENSE. */
/*
 * Updates over Wi-Fi: the parts that need no network, so the desktop tests
 * can run them (the device's updater.c fetches and drives them).
 *
 *   - which release is newer (semver; a dev build v0.7.0-12-gabc123 counts
 *     as 0.7.0);
 *   - GitHub's answer for the latest release: its tag, the zip, its size and
 *     digest;
 *   - staging: the zip's sd/ unpacked into STAGE/sd, every file checked
 *     against the zip's about/SHA256SUMS (and its CRC);
 *   - installing: each staged file renamed over the card's (vfat replaces the
 *     old file's directory entry in place, so the name is never missing),
 *     the kernel last, with the old one kept as uImage.prev. A marker makes
 *     an install cut short (power lost) finish at the next start, so the
 *     kernel and its Wi-Fi modules never stay from different releases.
 *
 * Only the release's own files are written: settings, the library, playlists,
 * keys, Wi-Fi networks, EQs and themes on the card are never in a release.
 * Files an older release had and this one doesn't are left, as when a person
 * copies sd/ over the card by hand.
 */
#ifndef FATHOM_UPDATE_H
#define FATHOM_UPDATE_H

#include <stddef.h>
#include <stdint.h>

#define UPDATE_API "https://api.github.com/repos/raparc0r/fathom-h2/releases/latest"

/* "v0.7.0", "0.7.0", "v0.7.0-12-gabc1234", "v0.7.0-dirty": 0 and v[3].
 * A pre-release ("v0.8.0-rc1"): 1 (v set). Anything else: -1. */
int update_parse_version(const char *s, int v[3]);
/* 1 if the release tagged `latest` should be offered to a player running
 * `running`: a release (not a pre-release) newer than it. A running version
 * that can't be read ("unknown") is older than any release. */
int update_is_newer(const char *latest, const char *running);

/* Automatic checks (Settings > About > Check automatically): 1 if one is due
 * at `now`, the last answer (last_ok) 8 hours old (three times a day: a
 * release is seen the day it's out, where once a day could take until the
 * next; the check is one small request, with the H2 idle on Wi-Fi) and the
 * last try an hour old (a failed one isn't retried sooner). Seconds, as time() gives them; 0 for
 * never. A clock set back since (NTP after a start without it) makes it due. */
#define UPDATE_AUTO_EVERY (8 * 3600)
#define UPDATE_AUTO_RETRY 3600
int update_auto_due(long now, long last_ok, long last_try);

typedef struct {
    char tag[32];           /* "v0.8.0" */
    char asset[128];        /* "fathom-h2-0.8.0.zip" */
    char url[512];          /* its browser_download_url (GitHub redirects it to its storage) */
    int64_t size;           /* bytes */
    char sha256[65];        /* GitHub's digest of it, "" if it didn't give one */
} update_release_t;

/* GitHub's /releases/latest answer: 0, or -1 (no tag, a draft or pre-release,
 * or no release zip: an asset fathom-h2-VERSION.zip, else any fathom-h2-*.zip). */
int update_parse_release(const char *json, size_t len, update_release_t *r);

/* Errors, for the UI's words (update_error_text). */
enum {
    UPDATE_OK, UPDATE_ERR_NET, UPDATE_ERR_ANSWER, UPDATE_ERR_SPACE, UPDATE_ERR_DOWNLOAD,
    UPDATE_ERR_DAMAGED, UPDATE_ERR_ZIP, UPDATE_ERR_WRITE, UPDATE_ERR_INSTALL, UPDATE_ERR_CANCELLED,
};
const char *update_error_text(int err);

/* SHA-256 of a file, as hex: 0, -1. progress may be NULL. */
typedef void (*update_progress_fn)(uint64_t done, uint64_t total, void *ctx);
int update_file_sha256(const char *path, char hex[65], update_progress_fn progress, void *ctx,
                       const volatile int *abort);

/* Bytes free where path is (statvfs), and the card's cluster size. */
int64_t update_free_space(const char *path, long *cluster);
/* What staging zip_path needs on the card beyond the zip itself (its sd/
 * files rounded up to clusters, and a copy of the kernel), or -1. */
int64_t update_stage_needs(const char *zip_path, long cluster, int64_t kernel_size);

/* Unpack the release's sd/ from zip_path into STAGE/sd (STAGE created),
 * checking every file against about/SHA256SUMS; STAGE/READY is written last.
 * progress: bytes written of the sd/ files' total. UPDATE_OK or an error. */
int update_stage(const char *zip_path, const char *stage, update_progress_fn progress, void *ctx,
                 const volatile int *abort);

/* Install STAGE/sd onto the card (card: its top folder), as above; STAGE is
 * removed afterwards. UPDATE_OK, or an error: with STAGE/INSTALLING there the
 * install had begun (update_resume finishes it at the next start, and the
 * stage must not be removed: update_install_begun); without it nothing has
 * moved (UPDATE_ERR_SPACE: no room for the old kernel's copy, the stage still
 * ready). Restart afterwards for the new kernel. */
int update_install(const char *card, const char *stage);
/* Whether an install of STAGE has begun and isn't finished. */
int update_install_begun(const char *stage);
/* At start: an install cut short? 1 and finished (restart now), 0 nothing
 * to do, -1 it failed again. A stage left from before (a download, or ready
 * and never installed) is removed. */
int update_resume(const char *card, const char *stage);

/* rm -r (for a stage): 0, -1. */
int update_remove_tree(const char *dir);

#endif
