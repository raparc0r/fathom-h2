# SPDX-License-Identifier: GPL-3.0-or-later
# Additional terms under GPLv3 section 7: see LICENSE.
#
# The microSD card, for the scripts that mount it or wait for it (S01bootlog,
# fathom-sdcard, fathom-usbdisk, S90fathom): sourced, ". /usr/lib/fathom/card.sh".

MNT=/mnt/sd
CARD_OUT=/var/run/fathom-card-out       # (exists while the card is out or unusable)
CARDOUT_PID=/var/run/fathom-cardout.pid
BOOTLOG_BUSY=/var/run/fathom-bootlog.busy   # (exists while S01bootlog's report is being written to the card)

# Hundredths of a second since the kernel started, in $cs, for the scripts
# that time what they wait for (no program run: read is the shell's own, and
# busybox's sleep isn't, so counting sleeps runs slow). 0 if it can't be read;
# the "1" before the fraction: "08" would be taken for octal.
now_cs() {
    cs=0
    read -r card_up card_rest < /proc/uptime 2>/dev/null || return 0
    case "$card_up" in *[0-9].[0-9][0-9]) cs=$(( ${card_up%.*} * 100 + 1${card_up#*.} - 100 )) ;; esac
}

# The card's disk (mmcblkN), by its slot: MSC0, 13450000.mmc (MSC1 is Wi-Fi).
# mmcblk0 since the device tree numbers the slots, but a card back from being
# out may come as another one.
card_disk() {
    for b in /sys/block/mmcblk*; do
        [ -e "$b" ] || continue             # (none: no readlink run for nothing, S01bootlog asks 20 times a second)
        case "$(readlink -f "$b")" in */13450000.mmc/*) echo "${b##*/}"; return 0 ;; esac
    done
    return 1
}

# Mount disk $1 on /mnt/sd: its first partition, or the whole disk (FAT either
# way); prints the device it mounted.
# dirsync: a file renamed over another (how settings, the queue, playlists...
# are saved) has its entry written at once. Without it the entry on the card
# goes on naming the old file's clusters for up to 30 s after they're freed:
# a power cut then leaves it dangling, and the next save trips over it.
# errors=continue: the kernel's answer to a damaged FAT is to make the card
# read-only (its default), which here means nothing is saved, at every start,
# until someone repairs the card on a computer (there's no fsck on the H2).
# Carrying on lets the next save replace the bad entry instead.
card_mount() {
    [ -n "$1" ] || return 1
    for d in /dev/${1}p1 /dev/$1; do
        [ -b "$d" ] && mount -t vfat -o rw,noatime,dirsync,errors=continue "$d" $MNT 2>/dev/null && { echo "$d"; return 0; }
    done
    return 1
}

# (read by the shell: a grep here was a program started at every look, and
# the start-up scripts look several times)
card_mounted() {
    while read -r cm_dev cm_mnt cm_rest; do
        [ "$cm_mnt" = "$MNT" ] && return 0
    done < /proc/mounts
    return 1
}
card_has_fathom() { [ -x $MNT/fathom/bin/fathom-player ]; }

# Before the card is unmounted while the H2 stays on (USB drive mode): the
# start-up's report is written in the background, 8 s in (S01bootlog), and for
# the half second that takes the card is busy. Being written just now, it's
# waited for: 5 s at most (a mark left by a report that died is removed, and
# an unmount that still finds the card busy says so, as it always did). Almost
# always there's nothing to wait for. A report still to come finds the card
# gone when its time comes, and isn't written.
card_report_done() {
    [ -e $BOOTLOG_BUSY ] || return 0
    for card_i in $(seq 1 50); do
        sleep 0.1
        [ -e $BOOTLOG_BUSY ] || return 0
    done
    rm -f $BOOTLOG_BUSY
    return 1
}

# No card the player can run from: the restart loop (S90fathom) stops, and
# fathom-cardout's screen ("No/bad card") stays
# until a card is put in (fathom-sdcard's add starts it all again)
card_wait() {
    touch $CARD_OUT
    start-stop-daemon -S -q -b -m -p $CARDOUT_PID -x /usr/sbin/fathom-cardout
}
