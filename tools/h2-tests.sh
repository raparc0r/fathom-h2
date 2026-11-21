#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The unit tests, built for the H2 and run on it: what the Mac's C library
# hides shows here (the battery log's header at every flush, 2026-11-19, came
# from musl's ftell() in append mode).
#
#     tools/h2-tests.sh [TEST...]      (default: every test that builds for the H2)
#
# Builds with Buildroot's toolchain in the build container (build/h2-tests),
# sends the tests, the shared libraries they need (the player has them built
# in, so they aren't on the H2) and the files they read to the card's
# fathom/test/unit, runs each there (niced, its playback threads not real-time:
# FATHOM_NO_RT; its scratch files on the card, not in /tmp: that's RAM),
# prints each one's result and the failures' output, and removes it all again.
# Needs SSH to the H2 over the USB link (Settings > SSH; H2SSH: another command to
# reach it) and a Buildroot build done once.
# The player keeps running; nothing outside fathom/test is touched.
#
# (The bundle is made inside the container and streamed out: files the
# container has just written aren't always all visible on the Mac yet.)
set -eu
cd "$(dirname "$0")/.."
H2SSH=${H2SSH:-ssh root@10.42.0.1}
DIR=/mnt/sd/fathom/test/unit
VERBOSE=${H2_TESTS_VERBOSE:-0}   # 1: every test's output, not only a failure's
want=$(for t in "$@"; do printf ' test_%s' "${t#test_}"; done)

echo "== building for the H2, and copying to it ($DIR)"
# (no single quotes inside this script)
docker/run.sh sh -c '
    set -e
    OUT=build/h2-tests H=/cache/br-h2/host
    S=$H/mipsel-buildroot-linux-musl/sysroot
    [ -f $H/share/buildroot/toolchainfile.cmake ] || { echo "no Buildroot build yet (buildroot/build.sh all)" >&2; exit 1; }
    {
        cmake -S . -B $OUT -DCMAKE_TOOLCHAIN_FILE=$H/share/buildroot/toolchainfile.cmake \
            -DCMAKE_BUILD_TYPE=Release -DPKG_CONFIG_EXECUTABLE=$H/bin/pkg-config -DFATHOM_BUILD_SIM=OFF \
            -DCMAKE_C_FLAGS=-Wno-format-truncation
        all=$(cmake --build $OUT --target help | sed -n "s/^\.\.\. \(test_[a-z0-9_]*\).*/\1/p")
        cmake --build $OUT -j"$(nproc)" --target ${1:-$all}
    } >/tmp/build.log 2>&1 || { cat /tmp/build.log >&2; exit 1; }
    all=$(cmake --build $OUT --target help | sed -n "s/^\.\.\. \(test_[a-z0-9_]*\).*/\1/p")
    # test_art makes 3000 x 3000 progressive JPEGs to test with: more memory
    # than the H2 has (the decoder it tests keeps to the H2s limits, as the
    # Mac run checks)
    all=$(echo $all | tr " " "\n" | grep -vx test_art)
    tests=${1:-$all}
    stage=$(mktemp -d)
    mkdir -p $stage/lib $stage/assets $stage/tests
    for t in $tests; do cp $OUT/$t $stage/; done
    cp assets/radio.txt $stage/assets/
    cp -R assets/themes $stage/assets/
    cp -R tests/fixtures tests/tls tests/data $stage/tests/
    # their shared libraries, and those libraries own
    needed() { $H/bin/mipsel-buildroot-linux-musl-readelf -d "$@" | sed -n "s/.*NEEDED.*\[\(.*\)\]/\1/p" | sort -u; }
    todo=$(cd $stage && needed $tests)
    while [ -n "$todo" ]; do
        next=
        for so in $todo; do
            if [ "$so" = libc.so ] || [ -e $stage/lib/$so ]; then continue; fi
            for d in $S/usr/lib $S/lib; do
                if [ -e $d/$so ]; then cp -L $d/$so $stage/lib/; next="$next $(needed $stage/lib/$so)"; break; fi
            done
        done
        todo=$next
    done
    echo $tests > $stage/run-list.txt
    tar -C $stage -cf - .
' sh "$want" | $H2SSH "rm -rf $DIR && mkdir -p $DIR/t && tar -xf - -C $DIR && test -s $DIR/run-list.txt"

echo "== running"
$H2SSH "cd $DIR && fails=0 && for t in \$(cat run-list.txt); do
        s=\$(date +%s); FATHOM_NO_RT=1 SQLITE_TMPDIR=$DIR/t LD_LIBRARY_PATH=$DIR/lib TMPDIR=$DIR/t nice -n 19 ./\$t > \$t.log 2>&1; rc=\$?
        printf '%-18s %s  %ss\n' \$t \$([ \$rc = 0 ] && echo ok || echo FAILED) \$((\$(date +%s) - s))
        [ \$rc = 0 ] || { fails=\$((fails + 1)); sed 's/^/    /' \$t.log | tail -40; }
        [ \$rc != 0 ] || [ $VERBOSE = 0 ] || sed 's/^/    /' \$t.log | tail -40
        rm -rf $DIR/t/*
    done; cd / && rm -rf $DIR; echo; [ \$fails = 0 ] && echo 'all passed on the H2' || { echo \"\$fails failed on the H2\"; exit 1; }"
