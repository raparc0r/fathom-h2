#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Build the H2 bootloader inside the Docker build image:
#   docker/run.sh bootloader/build.sh
#
# Builds Rockbox's X1000 bootloader for the Eros Q hw4 ("erosqnative_v4") with
# Fathom's fixes from bootloader/patches/ applied to a copy of the pinned
# submodule (the submodule itself stays untouched).
#
# Output: build/bootloader/bootloader.erosq (load it with jztool)
set -eu

repo=/work
pristine=$repo/bootloader/rockbox
src=/cache/rockbox-src
out=$repo/build/bootloader
target=erosqnative_v4

# Patched copy of the submodule
rsync -a --delete --exclude .git "$pristine/" "$src/"
for p in "$repo"/bootloader/patches/*.patch; do
    [ -e "$p" ] || continue
    echo "applying $(basename "$p")"
    patch -d "$src" -p1 -s < "$p"
done
# Fathom's own bootloader code (patch 0006 builds it): the firmware copy
cp "$repo"/bootloader/fathom/*.c "$repo"/bootloader/fathom/*.h "$src/bootloader/x1000/"
# The boot logo patch 0004 draws (from assets/logo.png)
python3 "$repo/tools/logo/png2rle.py" "$repo/assets/logo.png" "$src/bootloader/x1000/fathom-logo.h"
# The copy has no .git: give version.sh Fathom's version and the Rockbox commit
# it's built on ("fathom-v0.5.0-rb01e6db8f5a", then the build date), so builds
# can be told apart on the bootloader's screens and in bootloader-info.txt
ver=$(git -c safe.directory='*' -C "$pristine" rev-parse --short=10 HEAD 2>/dev/null || echo unknown)
fver=$(git -c safe.directory='*' -C "$repo" describe --always --dirty 2>/dev/null || echo unknown)
echo "fathom-$fver-rb$ver" > "$src/docs/VERSION"

if [ ! -x /cache/rbdev/bin/mipsel-elf-gcc ]; then
    echo "MIPS toolchain missing. Build it once (about 15 min):" >&2
    echo "  docker/run.sh sh -c 'cd bootloader/rockbox/tools && RBDEV_PREFIX=/cache/rbdev \\" >&2
    echo "    RBDEV_DOWNLOAD=/cache/rbdev-dl RBDEV_BUILD=/tmp/rbdev-build RBDEV_TARGET=i ./rockboxdev.sh'" >&2
    exit 1
fi
export PATH=/cache/rbdev/bin:$PATH

mkdir -p "$out/$target"
cd "$out/$target"
if [ ! -f Makefile ] || ! grep -q "$src" Makefile; then
    rm -f Makefile
    "$src/tools/configure" --target=$target --type=b
fi
make -j"$(nproc)"

cp bootloader.erosq "$out/bootloader.erosq"
echo
echo "Built $out/bootloader.erosq"
sha256sum "$out/bootloader.erosq"
