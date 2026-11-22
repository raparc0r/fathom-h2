#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional terms under GPLv3 section 7: see LICENSE.
#
# Build the H2 system with Buildroot, inside the Docker image:
#   docker/run.sh buildroot/build.sh [make target...]
#
# The build tree lives in the Docker cache volume (case-sensitive ext4, fast);
# the files for the SD card are copied to build/images/ in the repo.
set -eu
out=/cache/br-h2
make -C /work/buildroot/buildroot BR2_EXTERNAL=/work/buildroot/external O="$out" fathom_h2_defconfig >/dev/null
make -C "$out" "$@"
mkdir -p /work/build/images
rm -rf /work/build/images/sd                      # no leftovers from earlier builds
cp -r "$out/images/sd" /work/build/images/
echo "SD card files: build/images/sd/"
