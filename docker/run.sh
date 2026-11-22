#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional terms under GPLv3 section 7: see LICENSE.
#
# Run a command inside the Fathom build container, with the repo at /work.
#   docker/run.sh                 interactive shell
#   docker/run.sh make -C ...     run a command
# Downloads and ccache live in the named volume "fathom-cache" so rebuilds are fast.
# FATHOM_CACHE_VOLUME names another (a copy, for a second build that mustn't share it).
set -eu
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/.." && pwd)
image=fathom-build:latest
volume=${FATHOM_CACHE_VOLUME:-fathom-cache}

# Docker Desktop's CLI may not be on PATH yet (e.g. a shell started before it was installed).
if ! command -v docker >/dev/null 2>&1 && [ -x /Applications/Docker.app/Contents/Resources/bin/docker ]; then
    PATH="/Applications/Docker.app/Contents/Resources/bin:$PATH"
fi
if ! command -v docker >/dev/null 2>&1; then
    echo "Docker is not installed. Install Docker Desktop or OrbStack, then run this again." >&2
    exit 1
fi

if ! docker image inspect "$image" >/dev/null 2>&1 || [ "$here/Dockerfile" -nt "$here/.built" ]; then
    docker build --build-arg UID="$(id -u)" --build-arg GID="$(id -g)" -t "$image" "$here"
    touch "$here/.built"
fi

docker volume create "$volume" >/dev/null
# The cache volume is created root-owned; hand it to the build user once.
docker run --rm -v "$volume":/cache -u 0 "$image" sh -c "mkdir -p /cache/dl /cache/ccache && chown -R $(id -u):$(id -g) /cache"

tty_flag=""
[ -t 0 ] && tty_flag="-it"
exec docker run --rm $tty_flag \
    -v "$repo":/work \
    -v "$volume":/cache \
    "$image" "${@:-bash}"
