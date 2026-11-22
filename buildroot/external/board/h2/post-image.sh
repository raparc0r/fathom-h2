#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional terms under GPLv3 section 7: see LICENSE.
#
# Make the files for the microSD card that the Rockbox bootloader's "Linux" entry loads:
#   uImage             legacy U-Boot image, gzip, load 0x80100000, entry = kernel_entry
#   linux_cmdline.txt  kernel command line (passed as argv[1])
set -eu
sd="$BINARIES_DIR/sd"
mkdir -p "$sd"

vmlinux="$BUILD_DIR/linux-$(sed -n 's/^BR2_LINUX_KERNEL_CUSTOM_VERSION_VALUE="\(.*\)"/\1/p' "$BR2_CONFIG")/vmlinux"
readelf=$(ls "$HOST_DIR"/bin/*-readelf | head -1)
entry=$("$readelf" -h "$vmlinux" | sed -n 's/.*Entry point address: *0x\(ffffffff\)\{0,1\}\([0-9a-f]*\).*/0x\2/p')
load=0x80100000

gzip -9 -n -c "$BINARIES_DIR/vmlinux.bin" > "$BINARIES_DIR/vmlinux.bin.gz"
"$HOST_DIR/bin/mkimage" -A mips -O linux -T kernel -C gzip -a "$load" -e "$entry" \
    -n "Fathom $(date -u +%Y%m%d-%H%M)" -d "$BINARIES_DIR/vmlinux.bin.gz" "$sd/uImage" >/dev/null

# Console on the USB gadget's serial port only (there's no text console on the
# screen: h2boot's logo stays until the player; messages are in dmesg and pstore).
printf '%s\n' "loglevel=7 panic=10 console=ttyGS0,115200 snd_usb_audio.lowlatency=0 firmware_class.path=/mnt/sd/fathom/firmware" > "$sd/linux_cmdline.txt"

# The player and its assets, run from the card (see S90fathom): fathom/bin, fathom/assets
if [ -f "$BINARIES_DIR/fathom-player" ]; then
    repo="$BR2_EXTERNAL_FATHOM_PATH/../.."
    mkdir -p "$sd/fathom/bin" "$sd/fathom/assets"
    cp "$BINARIES_DIR/fathom-player" "$sd/fathom/bin/"
    [ -f "$BINARIES_DIR/fathom-avrcp" ] && cp "$BINARIES_DIR/fathom-avrcp" "$sd/fathom/bin/"
    rm -rf "$sd/fathom/assets/fonts" "$sd/fathom/assets/themes"
    cp -r "$repo/assets/fonts" "$repo/assets/themes" "$sd/fathom/assets/"
    cp "$repo/assets/logo.png" "$sd/fathom/assets/"
    cp "$repo/assets/radio.txt" "$sd/fathom/assets/"   # example stations: used until there's a fathom/radio.txt
    rm -rf "$sd/fathom/assets/fonts/src"     # font sources: for rebuilding fonts, not for the player
fi

# Bluetooth, run from the card too (post-build.sh staged it): fathom/bt
if [ -d "$BINARIES_DIR/bt" ]; then
    rm -rf "$sd/fathom/bt"
    mkdir -p "$sd/fathom"
    cp -rL "$BINARIES_DIR/bt" "$sd/fathom/bt"
fi

# Wi-Fi, run from the card (post-build.sh staged it): fathom/wifi. The user's
# saved networks (fathom/wifi/networks.conf) aren't in the image: kept on update.
if [ -d "$BINARIES_DIR/wifi" ]; then
    rm -rf "$sd/fathom/wifi"
    mkdir -p "$sd/fathom"
    cp -rL "$BINARIES_DIR/wifi" "$sd/fathom/wifi"
fi

# The network drive (lighttpd, mdnsd), run from the card: fathom/share
if [ -d "$BINARIES_DIR/share" ]; then
    rm -rf "$sd/fathom/share"
    mkdir -p "$sd/fathom"
    cp -rL "$BINARIES_DIR/share" "$sd/fathom/share"
    # (fathom-system's: keeps what the drive and SFTP read out of the page cache)
    [ -f "$BINARIES_DIR/fathom-nocache.so" ] && cp "$BINARIES_DIR/fathom-nocache.so" "$sd/fathom/share/usr/lib/"
fi

# SSH's dropbear and the ALSA bring-up tools, out of the initramfs: fathom/ssh,
# fathom/tools (run through /usr/libexec/fathom-card)
for d in ssh tools; do
    if [ -d "$BINARIES_DIR/$d" ]; then
        rm -rf "$sd/fathom/$d"
        mkdir -p "$sd/fathom"
        cp -rL "$BINARIES_DIR/$d" "$sd/fathom/$d"
    fi
done

echo "uImage: load $load, entry $entry"
ls -lR "$sd"
