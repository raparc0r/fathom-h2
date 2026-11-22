# SPDX-License-Identifier: GPL-3.0-or-later
# Additional terms under GPLv3 section 7: see LICENSE.
#
# The microSD card, for the scripts that mount it or wait for it (S01bootlog,
# fathom-sdcard, fathom-usbdisk, S90fathom): sourced, ". /usr/lib/fathom/card.sh".

MNT=/mnt/sd
CARD_OUT=/var/run/fathom-card-out       # (exists while the card is out or unusable)
CARDOUT_PID=/var/run/fathom-cardout.pid

# The card's disk (mmcblkN), by its slot: MSC0, 13450000.mmc (MSC1 is Wi-Fi).
# mmcblk0 since the device tree numbers the slots, but a card back from being
# out may come as another one.
card_disk() {
    for b in /sys/block/mmcblk*; do
        case "$(readlink -f "$b")" in */13450000.mmc/*) echo "${b##*/}"; return 0 ;; esac
    done
    return 1
}

# Mount disk $1 on /mnt/sd: its first partition, or the whole disk (FAT either
# way); prints the device it mounted
card_mount() {
    [ -n "$1" ] || return 1
    for d in /dev/${1}p1 /dev/$1; do
        [ -b "$d" ] && mount -t vfat -o rw,noatime "$d" $MNT 2>/dev/null && { echo "$d"; return 0; }
    done
    return 1
}

card_mounted() { grep -q " $MNT " /proc/mounts; }
card_has_fathom() { [ -x $MNT/fathom/bin/fathom-player ]; }

# No card the player can run from: the restart loop (S90fathom) stops, and
# fathom-cardout's screen ("No/bad card") stays
# until a card is put in (fathom-sdcard's add starts it all again)
card_wait() {
    touch $CARD_OUT
    start-stop-daemon -S -q -b -m -p $CARDOUT_PID -x /usr/sbin/fathom-cardout
}
