# SPDX-License-Identifier: GPL-3.0-or-later
# Additional terms under GPLv3 section 7: see LICENSE.
#
# Fathom build environment: Buildroot (rootfs + cross toolchain), the Linux
# kernel, the bootloader, and the host-side tests. Used on the Mac through
# docker/run.sh.
FROM debian:trixie-slim

ENV DEBIAN_FRONTEND=noninteractive \
    LANG=C.UTF-8 \
    LC_ALL=C.UTF-8

RUN apt-get update && apt-get install -y --no-install-recommends \
        # Buildroot mandatory host packages
        build-essential bc bison flex cpio file rsync unzip wget curl \
        perl patch gzip bzip2 xz-utils tar sed which git ca-certificates \
        python3 python3-venv libncurses-dev \
        # Rockbox toolchain build (tools/rockboxdev.sh)
        texinfo automake autoconf libtool libtool-bin gawk zip \
        # kernel and bootloader
        libssl-dev libelf-dev u-boot-tools device-tree-compiler kmod \
        # flash images (UBI/UBIFS)
        mtd-utils \
        # host-side build and tests of the player core
        cmake ninja-build pkg-config libsdl2-dev \
        # USB-boot host tool (jztool-style)
        libusb-1.0-0-dev \
        ccache \
    && rm -rf /var/lib/apt/lists/*

# Build as an unprivileged user; run.sh passes the Mac user's uid/gid.
ARG UID=1000
ARG GID=1000
RUN groupadd -g ${GID} builder 2>/dev/null || true \
 && useradd -m -u ${UID} -g ${GID} -s /bin/bash builder 2>/dev/null || true

ENV BR2_DL_DIR=/cache/dl \
    BR2_CCACHE_DIR=/cache/ccache \
    CCACHE_DIR=/cache/ccache

WORKDIR /work
USER builder
