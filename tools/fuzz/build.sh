#!/bin/sh
# Builds tools/fuzz/fuzz against the libraries compiled with coverage hooks and
# ASan/UBSan (macOS: Apple's clang, no libFuzzer). Usage: tools/fuzz/build.sh [BUILD_DIR]
set -e
cd "$(dirname "$0")/../.."
B=${1:-build-fuzz}
SAN="-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer"
cmake -S . -B "$B" -DCMAKE_BUILD_TYPE=Debug -DFATHOM_BUILD_SIM=OFF \
  -DCMAKE_C_FLAGS="$SAN -fsanitize-coverage=trace-pc-guard" >/dev/null
make -C "$B" -j8 fathom_player fathom_core >/dev/null
DEPS=$(pkg-config --cflags --libs sqlite3 flac libmpg123 zlib libjpeg vorbisfile)
# (the vendor-update reader is in no library: compiled here, with the same hooks)
cc $SAN -fsanitize-coverage=trace-pc-guard -g -O1 -Iapp -c app/sys/hibyupt.c -o "$B/fuzz-hibyupt.o"
cc $SAN -fsanitize-coverage=trace-pc-guard -g -O1 -Iapp -c app/sys/md5.c -o "$B/fuzz-md5.o"
cc $SAN -g -O1 -Iapp -Ithird_party tools/fuzz/fuzz.c "$B/fuzz-hibyupt.o" "$B/fuzz-md5.o" "$B/libfathom_player.a" "$B/libfathom_core.a" $DEPS -lm -o "$B/fuzz"
echo "$B/fuzz"
