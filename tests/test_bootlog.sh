#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional terms under GPLv3 section 7: see LICENSE.
#
# Off-device test of S01bootlog: its wait for the microSD card (looked for 20
# times a second, a slow card given its 10 s, an empty slot given up on after
# 3 s, both by the clock), and the boot report, written in the background 8 s
# later, not at all when the card has gone, and waited for by whoever unmounts
# the card while it's written (card_report_done); the blue LED solid for 5 s
# once it's saved, then off. The script runs whole
# against a root made here (its /sys, /dev, /proc, /mnt, /var and /tmp), with
# stand-ins for sleep, mount, busybox (devmem), dmesg, uname and sync: no real
# waiting, nothing mounted.
#   sh tests/test_bootlog.sh           (under dash too, when there is one)
set -u
. "$(dirname "$0")/lib.sh"
mkdir -p $T/bin $T/st
# The scripts with their paths inside the root; a plain file stands for a
# block device (-b: nobody here may make one)
rooted $SRC/etc/init.d/S01bootlog $T/S01bootlog -e "$FILE_FOR_DEVICE"
mkdir -p $R/usr/lib/fathom
rooted $SRC/usr/lib/fathom/card.sh $R/usr/lib/fathom/card.sh -e "$FILE_FOR_DEVICE"

# sleep: counted, by how long was asked for (0.05: the wait for the card; 8:
# the report's; 5: the LED's; 0.1: whoever waits for the report), and st/at.N
# (a script) run after the Nth of 0.05: the card answering, its disk appearing.
# One of S seconds lasts for as long as st/hold.S is there. With st/tick (hundredths of a second),
# each 0.05 moves the clock (/proc/uptime) on by that much: what a look really costs.
cat > $T/bin/sleep <<X
#!/bin/sh
echo "\$1" >> $T/st/sleeps.\$1
n=\$(wc -l < $T/st/sleeps.\$1); n=\$((n + 0))
[ "\$1" = 0.05 ] && [ -f $T/st/at.\$n ] && . $T/st/at.\$n
[ "\$1" = 0.05 ] && [ -f $T/st/tick ] && echo "\$((10 + n * \$(cat $T/st/tick) / 100)).\$((n * \$(cat $T/st/tick) / 10 % 10))\$((n * \$(cat $T/st/tick) % 10)) 1.00" > $R/proc/uptime
while [ -f $T/st/hold.\$1 ]; do $SLEEP 0.02; done
exit 0
X
# mount: the card's only when its device is there (FAT: st/fat names what holds one)
cat > $T/bin/mount <<X
#!/bin/sh
echo "\$*" >> $T/st/mounts
[ "\$2" = vfat ] || exit 0
[ -e "\$5" ] && grep -qx "\$5" $T/st/fat 2>/dev/null || exit 1
echo "\$5 \$6 vfat rw 0 0" >> $R/proc/mounts
X
printf '#!/bin/sh\n[ "$1" = devmem ] && [ $# -eq 3 ] && echo 0x00000001\nexit 0\n' > $T/bin/busybox
printf '#!/bin/sh\necho "kernel says hello"\n' > $T/bin/dmesg
printf '#!/bin/sh\necho "Linux fathom-h2 test"\n' > $T/bin/uname
printf '#!/bin/sh\necho sync >> %s/st/syncs\n' $T > $T/bin/sync
chmod +x $T/bin/*
PATH0=$PATH
export PATH=$T/bin:$PATH

sleeps() { count $T/st/sleeps.${1:-0.05}; }
mounted() { grep -q "^$R/dev/$1 $R/mnt/sd " $R/proc/mounts; }
BUSY=$R/var/run/fathom-bootlog.busy
LOG=$R/mnt/sd/fathom/boot-1.log

HOST=$R/sys/devices/platform/13450000.mmc/mmc_host/mmc0
# A root as the H2's at init: the slot's host there (mmc0, unless $1 says no), no card yet
fresh() {
    rm -rf $R/sys $R/dev $R/proc $R/mnt $R/tmp $R/var $T/st
    mkdir -p $T/st $R/dev $R/proc $R/tmp $R/var/run $R/sys/block $R/sys/class/mmc_host $R/sys/fs/pstore \
        $R/sys/bus/platform/devices $R/sys/class/leds/blue $HOST
    : > $R/proc/mounts
    echo "12.34 5.67" > $R/proc/uptime
    for f in cmdline cpuinfo meminfo interrupts; do echo "$f" > $R/proc/$f; done
    [ "${1:-}" = no-mmc0 ] || ln -s ../../devices/platform/13450000.mmc/mmc_host/mmc0 $R/sys/class/mmc_host/mmc0
}
# What the kernel does as a card comes up, as lines for st/at.N: it answers, its disk, its partition
answers="mkdir -p $HOST/mmc0:0001/block"
disk="mkdir -p $HOST/mmc0:0001/block/mmcblk0; ln -s ../devices/platform/13450000.mmc/mmc_host/mmc0/mmc0:0001/block/mmcblk0 $R/sys/block/mmcblk0; : > $R/dev/mmcblk0"
part=": > $R/dev/mmcblk0p1"
# The clock moving on by 0.06 s with each look (what a sleep of 0.05 s costs, more or less)
ticking() { echo 6 > $T/st/tick; echo "10.00 1.00" > $R/proc/uptime; }
# A card with Fathom's folder on it, in the slot, there before init looks
card() { eval "$answers; $disk; $part"; echo $R/dev/mmcblk0p1 > $T/st/fat; mkdir -p $R/mnt/sd/fathom; }
# The start-up's part only; and with the report after it (when a card was mounted), waited for
start() { $SH $T/S01bootlog start > $T/st/out 2>&1; }
run() { start; [ ! -s $R/proc/mounts ] || until_ '[ -f $T/st/reports ]'; }

for SH in sh dash; do
command -v $SH > /dev/null 2>&1 || continue
echo "-- $SH"
# (the report runs under the same shell: "sh S01bootlog report"; when it has ended is noted)
shreal=$(PATH=$PATH0 command -v $SH)
printf '#!/bin/sh\n%s "$@"; rc=$?\n[ "${2:-}" = report ] && echo done >> %s/st/reports\nexit $rc\n' "$shreal" $T > $T/bin/sh
chmod +x $T/bin/sh

# 1. The card there before init looks: mounted without a wait
fresh; eval "$answers; $disk; $part"; echo $R/dev/mmcblk0p1 > $T/st/fat
run
check "there already: its partition mounted" 'mounted mmcblk0p1'
check "...with no wait at all" '[ $(sleeps) -eq 0 ]'
check "the reason for the start kept" '[ "$(cat $R/tmp/fathom-reset)" = power-on ]'

# 2. The disk two looks later, its partition one more: three short waits, not a second
fresh; echo $R/dev/mmcblk0p1 > $T/st/fat
echo "$answers" > $T/st/at.1; echo "$disk" > $T/st/at.2; echo "$part" > $T/st/at.3
run
check "just after init: mounted after three looks" 'mounted mmcblk0p1 && [ $(sleeps) -eq 3 ]'

# 3. A card with no partition table: 0.3 s for a partition, then the whole disk
fresh; eval "$answers; $disk"; echo $R/dev/mmcblk0 > $T/st/fat
run
check "no partition table: the disk itself mounted" 'mounted mmcblk0'
check "...after 0.3 s" '[ $(sleeps) -eq 6 ]'

# 4. No card: given up on after 3 s, by the clock (a look costing 0.06 s: 50 of them)
fresh; ticking
run
check "empty slot: given up after 3 s" '[ $(sleeps) -eq 50 ]'
check "...nothing mounted, no report, none to come" '[ ! -s $R/proc/mounts ] && [ ! -e $R/mnt/sd/fathom ] && [ $(sleeps 8) -eq 0 ]'
# ...and by the number of looks, with a clock that can't be read
fresh; echo "?" > $R/proc/uptime
run
check "empty slot, no clock: given up after 60 looks" '[ $(sleeps) -eq 60 ]'

# 5. A card that answers but is slow with its disk (5 s): waited for
fresh; echo $R/dev/mmcblk0p1 > $T/st/fat; ticking
echo "$answers" > $T/st/at.20; echo "$disk; $part" > $T/st/at.84
run
check "a slow card: waited for past 3 s, and mounted" 'mounted mmcblk0p1 && [ $(sleeps) -eq 84 ]'

# 6. A card that answers and never gives a disk: the whole 10 s, by the clock
fresh; eval "$answers"; ticking
run
check "a card with no disk: 10 s, as before" '[ $(sleeps) -eq 167 ] && [ ! -s $R/proc/mounts ]'
fresh; eval "$answers"; echo "?" > $R/proc/uptime
run
check "...or 200 looks, with no clock" '[ $(sleeps) -eq 200 ]'

# 7. The slot's host not mmc0 (a kernel that doesn't number them): 10 s for a card, as before
fresh no-mmc0; ticking
run
check "the slot's host unknown: 10 s, as before" '[ $(sleeps) -eq 167 ]'
fresh no-mmc0; echo $R/dev/mmcblk0p1 > $T/st/fat; ticking
echo "$disk; $part" > $T/st/at.80
run
check "...and its card still found" 'mounted mmcblk0p1 && [ $(sleeps) -eq 80 ]'

# 8. The report: not written by the start-up itself; 8 s later, with the last four kept
fresh; card; touch $T/st/hold.8 $T/st/hold.5
for n in 1 2 3 4 5; do echo "start-up $n ago" > $R/mnt/sd/fathom/boot-$n.log; done
echo "an oops" > $R/sys/fs/pstore/dmesg-ramoops-0
start
until_ '[ $(sleeps 8) -eq 1 ]'
check "the start-up doesn't wait for the report" '[ $(sleeps 8) -eq 1 ] && [ "$(cat $R/mnt/sd/fathom/boot-1.log)" = "start-up 1 ago" ] && [ ! -e $BUSY ]'
LED=$R/sys/class/leds/blue
check "the LED left alone until then" '[ ! -e $LED/trigger ] && [ ! -e $LED/brightness ]'
rm $T/st/hold.8
until_ '[ $(sleeps 5) -eq 1 ]'
check "the report written 8 s later" 'grep -q "kernel says hello" $LOG'
check "...the LED solid, and the card no longer said to be busy" '[ "$(cat $LED/trigger)" = none ] && [ "$(cat $LED/brightness)" = 1 ] && [ ! -e $BUSY ]'
rm $T/st/hold.5
until_ '[ -f $T/st/reports ]'
check "...and off 5 s later" '[ "$(cat $LED/brightness)" = 0 ] && [ "$(cat $LED/trigger)" = none ]'
check "...with the reason for the start, when it was written, and the last crash" \
    'grep -q "^start: power-on (RSR 0x00000001)" $LOG && grep -q "^written: 12.34 s" $LOG && grep -q "an oops" $LOG'
check "the four before it kept, the oldest gone" \
    '[ "$(cat $R/mnt/sd/fathom/boot-2.log)" = "start-up 1 ago" ] && [ "$(cat $R/mnt/sd/fathom/boot-5.log)" = "start-up 4 ago" ] && [ $(ls $R/mnt/sd/fathom | wc -l) -eq 5 ]'
check "written out to the card, and no mark left" '[ -s $T/st/syncs ] && [ -z "$(ls $R/var/run)" ]'

# 9. The card gone before the report's time (taken out, lent, switching off): none, and nothing written under the mount point
fresh; card; touch $T/st/hold.8
start
until_ '[ $(sleeps 8) -eq 1 ]'
: > $R/proc/mounts; rm -rf $R/mnt/sd/fathom
rm $T/st/hold.8
until_ '[ -f $T/st/reports ]'
check "the card gone meanwhile: no report, nothing made where it was" '[ -z "$(ls $R/mnt/sd)" ] && [ ! -e $T/st/syncs ] && [ -z "$(ls $R/var/run)" ]'
check "...and the LED left as the kernel has it" '[ -z "$(ls $R/sys/class/leds/blue)" ] && [ $(sleeps 5) -eq 0 ]'

# 10. Whoever unmounts the card (USB drive mode) while the report is written waits for it
fresh
( . $R/usr/lib/fathom/card.sh; card_report_done ); rc=$?
check "no report being written: nothing to wait for" '[ $rc -eq 0 ] && [ $(sleeps 0.1) -eq 0 ]'
: > $BUSY; ( $SLEEP 0.3; rm -f $BUSY ) &
( . $R/usr/lib/fathom/card.sh; PATH=$PATH0; card_report_done ); rc=$?
wait
check "a report being written: waited for" '[ $rc -eq 0 ] && [ ! -e $BUSY ]'
: > $BUSY
( . $R/usr/lib/fathom/card.sh; card_report_done ); rc=$?
check "a report that died while writing: 5 s, then no longer in the way" '[ $rc -eq 1 ] && [ $(sleeps 0.1) -eq 50 ] && [ ! -e $BUSY ]'

done
rm -rf $T
finish
