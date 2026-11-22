#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# The installer for an H2 that still has its stock firmware: an update file that
# the stock firmware's own updater (System Settings > Firmware Update) applies,
# writing Fathom's bootloader and nothing else.
#
#   tools/release/make-upt.sh bootloader.erosq OUT.upt
#
# The same as Rockbox's native installer for this player
# (bootloader/rockbox/tools/hibyos_nativepatcher, mkrbinstall): an ISO 9660 image
# holding
#   uboot.bin    the bootloader as the flash holds it: the SPL at 0, the
#                compressed bootloader at 26 KiB (both from bootloader.erosq)
#   update.txt   the updater's list: only "bootloader", with its MD5
#   version.txt  the model the updater checks: erosq_2024, hardware revision 4
#                (older revisions have other names, so they refuse it)
# Nothing from the stock firmware is in it. One difference from Rockbox's: the
# version date is the build's, not a fixed 2024 one, in case an updater refuses
# a version older than what it runs.
#
# The stock bootloader hands over to the updater; Fathom's (like Rockbox's)
# doesn't, so on an H2 that already runs Fathom this file does nothing:
# the bootloader menu's "Install or update" is the way to update it there.
set -eu

[ $# -eq 2 ] || { echo "usage: $0 bootloader.erosq OUT.upt" >&2; exit 2; }
blfile=$1
out=$2
fail() { echo "make-upt: $1" >&2; exit 1; }
[ -f "$blfile" ] || fail "no $blfile"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir "$work/bl" "$work/iso"
tar -xf "$blfile" -C "$work/bl"
for f in spl.erosq bootloader.ucl bootloader-info.txt; do
    [ -s "$work/bl/$f" ] || fail "$blfile has no $f"
done

spl=$(wc -c < "$work/bl/spl.erosq")
ucl=$(wc -c < "$work/bl/bootloader.ucl")
[ "$spl" -le 26624 ] || fail "the SPL ($spl bytes) runs into the bootloader at 26624"
total=$((26624 + ucl))
[ "$total" -le 131072 ] || fail "the bootloader ($total bytes) doesn't fit its 128 KiB in flash"

u="$work/iso/uboot.bin"
dd if="$work/bl/spl.erosq" of="$u" bs=1 conv=notrunc 2>/dev/null
dd if="$work/bl/bootloader.ucl" of="$u" bs=1 seek=26624 conv=notrunc 2>/dev/null
[ "$(wc -c < "$u")" -eq "$total" ] || fail "uboot.bin came out the wrong size"

if command -v md5sum > /dev/null; then md5=$(md5sum "$u" | cut -d' ' -f1); else md5=$(md5 -q "$u"); fi
cat > "$work/iso/update.txt" <<EOF
bootloader={
        name=uboot
        file_path=autoupdate/uboot.bin
        md5=$md5
}

EOF
cat > "$work/iso/version.txt" <<EOF
version={
        name=erosq_2024
        ver=$(date -u +%Y-%m-%dT%H:%M:%S)+00:00
}
EOF

rm -f "$out"
if command -v hdiutil > /dev/null; then          # macOS
    hdiutil makehybrid -quiet -iso -joliet -o "$work/image.iso" "$work/iso"   # (it adds .iso itself)
    mv "$work/image.iso" "$out"
elif command -v genisoimage > /dev/null; then
    genisoimage -quiet -J -o "$out" "$work/iso"
elif command -v xorriso > /dev/null; then
    xorriso -as mkisofs -quiet -J -o "$out" "$work/iso"
else
    fail "needs hdiutil (macOS), genisoimage or xorriso to make the image"
fi
[ -s "$out" ] || fail "no image made"
echo "$out: bootloader $(cat "$work/bl/bootloader-info.txt"), $total bytes, md5 $md5"
