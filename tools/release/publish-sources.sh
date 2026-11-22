#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional terms under GPLv3 section 7: see LICENSE.
#
# Publish the third-party source behind a release, before the release itself:
#
#   tools/release/publish-sources.sh            # what's missing: list it, ask, upload
#
# The archives build/legal-info/sources.list names (collect-sources.sh) go to
# the "sources" release of raparc0r/fathom-h2-sources, each once: one already
# there (the same name is the same bytes) is left alone. Afterwards every
# listed archive must be there with GitHub's SHA-256 equal to the list's, or
# this fails, and the release must wait. Run on the Mac, as raparc0r (gh).
set -eu
root=$(cd "$(dirname "$0")/../.." && pwd)
cd "$root"
repo=raparc0r/fathom-h2-sources
tag=sources
list=build/legal-info/sources.list
fail() { echo "publish-sources: $1" >&2; exit 1; }

[ -s "$list" ] || fail "no $list: docker/run.sh tools/release/collect-sources.sh"
gh auth status 2>&1 | grep -q "account raparc0r" || fail "gh isn't signed in as raparc0r"
gh api user --jq .login | grep -qx raparc0r || fail "the active gh account isn't raparc0r"

if ! gh repo view "$repo" > /dev/null 2>&1; then
    printf 'Create the public repository %s? [y/N] ' "$repo"
    read -r a; [ "$a" = y ] || fail "stopped"
    gh repo create "$repo" --public --add-readme \
        --description "Source of the third-party components in Fathom's releases (github.com/raparc0r/fathom-h2)"
    # (--add-readme: GitHub makes no release in a repository without a commit)
fi
if ! gh release view "$tag" --repo "$repo" > /dev/null 2>&1; then
    printf 'Create its release "%s"? [y/N] ' "$tag"
    read -r a; [ "$a" = y ] || fail "stopped"
    gh release create "$tag" --repo "$repo" --title "Third-party sources" --notes \
"The source of the third-party components in Fathom's releases, one archive per version, as each was built (the project's release with Buildroot's patches; Rockbox at the bootloader's commit). Each release's about/THIRD-PARTY.txt names the archives it uses. Fathom itself: https://github.com/raparc0r/fathom-h2"
fi

have=$(gh api --paginate "repos/$repo/releases/tags/$tag" --jq '.assets[].name')
missing=$(awk '{print $1}' "$list" | while read -r n; do echo "$have" | grep -qx "$n" || echo "$n"; done)
if [ -n "$missing" ]; then
    echo "To upload to $repo ($tag):"
    for n in $missing; do echo "  $n $(awk -v n="$n" '$1 == n {printf "%.1f MB", $3 / 1e6}' "$list")"; done
    printf 'Upload them? [y/N] '
    read -r a; [ "$a" = y ] || fail "stopped"
    tmp=$(mktemp -d)
    # All in one container: docker/run.sh first chowns the whole cache, minutes
    # each time (no terminal: binary)
    docker/run.sh sh -c "cd /cache/fathom-sources && tar -cf - $(echo $missing)" < /dev/null | tar -xf - -C "$tmp"
    for n in $missing; do
        want=$(awk -v n="$n" '$1 == n {print $2}' "$list")
        [ "$(shasum -a 256 "$tmp/$n" | cut -c1-64)" = "$want" ] || fail "$n in the cache isn't the listed one: collect-sources.sh again"
    done
    (cd "$tmp" && gh release upload "$tag" $missing --repo "$repo")
    for n in $missing; do rm -f "$tmp/$n"; done
    rmdir "$tmp"
fi

# Every listed archive there, with the listed bytes
digests=$(gh api --paginate "repos/$repo/releases/tags/$tag" --jq '.assets[] | .name + " " + .digest')
bad=0
while read -r n sha _; do
    echo "$digests" | grep -qx "$n sha256:$sha" || { echo "publish-sources: $n missing or different on GitHub" >&2; bad=1; }
done < "$list"
[ $bad = 0 ] || exit 1
echo "All $(wc -l < "$list" | tr -d ' ') source archives are in $repo ($tag)."
