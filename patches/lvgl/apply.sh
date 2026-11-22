#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional terms under GPLv3 section 7: see LICENSE.
# Apply our LVGL fixes (this folder's *.patch) in LVGL's source tree, the
# current directory. Run by CMake's FetchContent after fetching; a patch
# already in is skipped, so running it again is harmless.
for p in "$(dirname "$0")"/*.patch; do
    git apply --reverse --check "$p" 2>/dev/null && continue
    git apply "$p" || { echo "cannot apply $p" >&2; exit 1; }
done
