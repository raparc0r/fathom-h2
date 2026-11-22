#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# The third-party source behind a release, for publishing once per version
# (the GPL and LGPL ask for it beside the binaries). In the build image:
#
#   docker/run.sh tools/release/collect-sources.sh
#
# From Buildroot's legal-info: each package the release ships, as Buildroot
# built it (the project's release and Buildroot's patches), becomes one tar in
# /cache/fathom-sources, named by its content (PACKAGE-VERSION-SHA8.tar): made
# the same way every time, so an unchanged package keeps its name and is
# uploaded once. Rockbox at the bootloader's pinned commit likewise. Fathom's
# own part (its patches, settings, scripts, the player) is the release's tag.
#
# Writes for make-release.sh (build/legal-info/):
#   sources.list   NAME SHA256 BYTES PACKAGE VERSION, one per archive
#   manifest.csv   Buildroot's: each package's licence
#   licenses/      each shipped package's licence files, and LVGL's
set -eu
br=/cache/br-h2
store=/cache/fathom-sources
out=/work/build/legal-info

make -C /work/buildroot/buildroot BR2_EXTERNAL=/work/buildroot/external O=$br legal-info > /tmp/legal-info.log 2>&1 ||
    { tail -20 /tmp/legal-info.log >&2; exit 1; }
li=$br/legal-info
mkdir -p "$store"
rm -rf "$out"
mkdir -p "$out/licenses"
cp "$li/manifest.csv" "$out/"
: > "$out/sources.list"

# Built with, not shipped: the compiler (its runtime is under the GCC exception),
# the kernel's headers (the kernel itself is listed), util-linux (only BusyBox's
# applets ship). Fathom's own packages are the tag.
skip() {
    case $1 in gcc-final-*|linux-headers-*|util-linux-*|fathom-*) return 0 ;; esac
    return 1
}

add() {   # add NAME FILE PACKAGE VERSION
    h=$(sha256sum "$2" | cut -c1-64)
    echo "$1 $h $(stat -c %s "$2") $3 $4" >> "$out/sources.list"
}

tail -n +2 "$li/manifest.csv" | while IFS= read -r line; do
    pkg=$(echo "$line" | cut -d'"' -f2)
    ver=$(echo "$line" | cut -d'"' -f4)
    d=$pkg-$ver
    skip "$d" && continue
    [ -d "$li/sources/$d" ] || { echo "collect-sources: no source for $d" >&2; exit 1; }
    [ -d "$li/licenses/$d" ] && cp -r "$li/licenses/$d" "$out/licenses/"
    tmp=$store/.part.tar
    tar --sort=name --mtime=@0 --owner=0 --group=0 --numeric-owner --format=gnu \
        -C "$li/sources" -cf "$tmp" "$d"
    h=$(sha256sum "$tmp" | cut -c1-8)
    name=$d-$h.tar
    if [ -f "$store/$name" ]; then rm -f "$tmp"; else mv "$tmp" "$store/$name"; fi
    add "$name" "$store/$name" "$pkg" "$ver"
done

# The bootloader's (and jztool's) Rockbox, at the pinned commit
rb=$(git -c safe.directory='*' -C /work/bootloader/rockbox rev-parse HEAD)
rb12=$(echo "$rb" | cut -c1-12)
tmp=$store/.part.tar.xz
git -c safe.directory='*' -C /work/bootloader/rockbox archive --prefix="rockbox-$rb12/" "$rb" | xz -6 -T1 > "$tmp"
h=$(sha256sum "$tmp" | cut -c1-8)
name=rockbox-$rb12-$h.tar.xz
if [ -f "$store/$name" ]; then rm -f "$tmp"; else mv "$tmp" "$store/$name"; fi
add "$name" "$store/$name" rockbox "$rb"

# LVGL, built into the player (fetched by device/CMakeLists.txt; MIT: its notice)
lv=/cache/dl/fathom-fetch/lvgl-src
lvver=$(git -c safe.directory='*' -C "$lv" describe --tags --exact-match 2>/dev/null || echo unknown)
mkdir -p "$out/licenses/lvgl-${lvver#v}"
cp "$lv/LICENCE.txt" "$out/licenses/lvgl-${lvver#v}/"

echo "$(wc -l < "$out/sources.list") source archives in $store, listed in build/legal-info/sources.list"
