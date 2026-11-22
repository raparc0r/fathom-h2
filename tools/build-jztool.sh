#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# jztool, Rockbox's USB-boot tool, for the release's recovery/ folder: one
# program for Intel and Apple-silicon Macs, macOS 11 or later, with libusb
# built in (third_party/libusb, unchanged), so it needs nothing installed.
#
#   tools/build-jztool.sh       -> build/rb-host/utils/jztool/jztool
#
# Each kind of Mac is built on its own (Rockbox's tool makefiles and libusb's
# configure take one -arch at a time), then lipo joins the two. The Rockbox
# sources are copied first: the submodule stays untouched.
set -eu
repo=$(cd "$(dirname "$0")/.." && pwd)
rb=$repo/bootloader/rockbox
out=$repo/build/rb-host
min=11.0
libusb=libusb-1.0.30

[ "$(uname -s)" = Darwin ] || { echo "build-jztool: run it on a Mac" >&2; exit 1; }
rm -rf "$out"
mkdir -p "$out/utils/jztool"
for arch in arm64 x86_64; do
    w=$out/$arch
    mkdir -p "$w"
    # libusb, static, for this kind of Mac
    tar -xjf "$repo/third_party/libusb/$libusb.tar.bz2" -C "$w"
    host=$([ $arch = arm64 ] && echo aarch64-apple-darwin || echo x86_64-apple-darwin)
    (cd "$w/$libusb" &&
        ./configure --host=$host --disable-shared --enable-static --prefix="$w/inst" \
            CC="cc -arch $arch -mmacosx-version-min=$min" > "$w/libusb.log" 2>&1 &&
        make -j"$(sysctl -n hw.ncpu)" >> "$w/libusb.log" 2>&1 &&
        make install >> "$w/libusb.log" 2>&1) || { echo "build-jztool: libusb ($arch) failed, see $w/libusb.log" >&2; exit 1; }
    # jztool and what it builds with (ucl, microtar), from a copy of Rockbox
    mkdir -p "$w/rb/utils" "$w/rb/tools" "$w/rb/lib"
    cp -R "$rb/utils/jztool" "$rb/utils/libtools.make" "$w/rb/utils/"
    cp -R "$rb/tools/ucl" "$w/rb/tools/"
    cp -R "$rb/lib/microtar" "$w/rb/lib/"
    # (-Wl,-S: no debug map, which notes each object file's path and time, so
    # the same sources give the same jztool wherever and whenever they're built)
    make -C "$w/rb/utils/jztool" CC=cc \
        ARCHFLAGS="-arch $arch -mmacosx-version-min=$min" \
        LDFLAGS="-Wl,-S" \
        LIBUSB_CFLAGS="-I$w/inst/include/libusb-1.0" \
        LIBUSB_LDOPTS="$w/inst/lib/libusb-1.0.a -framework IOKit -framework CoreFoundation -framework Security" \
        > "$w/jztool.log" 2>&1 || { echo "build-jztool: jztool ($arch) failed, see $w/jztool.log" >&2; exit 1; }
done
lipo -create -output "$out/utils/jztool/jztool" "$out/arm64/rb/utils/jztool/jztool" "$out/x86_64/rb/utils/jztool/jztool"
# Nothing from Homebrew or anywhere else: only the system's own libraries
if otool -L "$out/utils/jztool/jztool" | grep '^[[:space:]]' | grep -v -e '^[[:space:]]*/usr/lib/' -e '^[[:space:]]*/System/Library/' | grep -q .; then
    otool -L "$out/utils/jztool/jztool" >&2
    echo "build-jztool: jztool uses a library macOS doesn't have" >&2
    exit 1
fi
lipo -info "$out/utils/jztool/jztool"
