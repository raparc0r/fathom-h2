#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional terms under GPLv3 section 7: see LICENSE.
#
# Off-device test of fathom-usbdisk (USB drive mode): the card lent, the end of
# it seen five times a second (ejected on the computer: at once; unplugged:
# after 3 s; a link reset or a sleeping computer: not an end), the card
# mounted again; and the boot report, if it's being written just then, waited
# for before the card is unmounted. Against a root made here, with stand-ins
# for mount, umount, sleep and the scripts it calls.
#   sh tests/test_fathom_usbdisk.sh    (under dash too, when there is one)
set -u
here=$(cd "$(dirname "$0")/.." && pwd)
SRC=${SRC:-$here/buildroot/external/board/h2/rootfs-overlay}
T=$(mktemp -d)
R=$T/root
mkdir -p $T/bin $T/st $R/usr/lib/fathom
rooted() {
    sed -E -e "s#(^|[ =\"(<>])/(usr|mnt|sys|tmp|dev|proc|var)/#\\1$R/\\2/#g" -e "s#$R/dev/null#/dev/null#g" \
        -e 's#\[ -b #[ -e #g' "$1" > "$2"
}
rooted $SRC/usr/sbin/fathom-usbdisk $T/fathom-usbdisk
rooted $SRC/usr/lib/fathom/card.sh $R/usr/lib/fathom/card.sh
SLEEP=$(command -v sleep)
LUN=$R/sys/kernel/config/usb_gadget/fathom/functions/mass_storage.0/lun.0
STATE=$R/sys/class/udc/13500000.usb/state
BUSY=$R/var/run/fathom-bootlog.busy

# sleep: counted by how long was asked for; st/at.S.N (a script) runs after
# the Nth sleep of S seconds (the computer ejecting, the cable pulled). They
# pass at once, or really take 0.05 s while st/real is there.
cat > $T/bin/sleep <<X
#!/bin/sh
echo "\$1" >> $T/st/sleeps.\$1
n=\$(wc -l < $T/st/sleeps.\$1); n=\$((n + 0))
[ -f $T/st/at.\$1.\$n ] && . $T/st/at.\$1.\$n
[ -f $T/st/real ] && exec $SLEEP 0.05
exit 0
X
# umount: refused while st/busy is there; mount: the card's, when st/fat names its device
cat > $T/bin/umount <<X
#!/bin/sh
echo umount >> $T/st/order
[ -f $T/st/busy ] && exit 1
: > $R/proc/mounts
X
cat > $T/bin/mount <<X
#!/bin/sh
[ -e "\$5" ] && grep -qx "\$5" $T/st/fat 2>/dev/null || exit 1
echo "\$5 \$6 vfat rw 0 0" >> $R/proc/mounts
X
for c in fathom-bt fathom-share fathom-wifi sync fuser start-stop-daemon; do
    printf '#!/bin/sh\necho "%s $*" >> %s/st/calls\n' $c $T > $T/bin/$c
done
chmod +x $T/bin/*
export PATH=$T/bin:$PATH

ok=0; bad=0
check() { if eval "$2"; then echo "ok   $1"; ok=$((ok+1)); else echo "FAIL $1"; bad=$((bad+1)); fi; }
count() { if [ -f "$1" ]; then echo $(($(wc -l < "$1") + 0)); else echo 0; fi; }
mounted() { grep -q "^$R/dev/mmcblk0p1 $R/mnt/sd " $R/proc/mounts; }
# The H2 with its card mounted and Fathom on it, on a computer
fresh() {
    rm -rf $R/sys $R/dev $R/proc $R/mnt $R/tmp $R/var $T/st
    d=$R/sys/devices/platform/13450000.mmc/mmc_host/mmc0/mmc0:0001/block/mmcblk0
    mkdir -p $T/st $R/dev $R/proc/sys/vm $R/tmp $R/var/run $R/sys/block $LUN ${STATE%/*} $d $R/mnt/sd/fathom/bin
    ln -s ../devices/platform/13450000.mmc/mmc_host/mmc0/mmc0:0001/block/mmcblk0 $R/sys/block/mmcblk0
    : > $R/dev/mmcblk0; : > $R/dev/mmcblk0p1; echo $R/dev/mmcblk0p1 > $T/st/fat
    echo "$R/dev/mmcblk0p1 $R/mnt/sd vfat rw 0 0" > $R/proc/mounts
    echo 1048576 > $R/proc/sys/vm/dirty_background_bytes; echo 4194304 > $R/proc/sys/vm/dirty_bytes
    : > $LUN/file; echo configured > $STATE
    printf '#!/bin/sh\n' > $R/mnt/sd/fathom/bin/fathom-player; chmod +x $R/mnt/sd/fathom/bin/fathom-player
}
run() { $SH $T/fathom-usbdisk > $T/st/out 2>&1; rc=$?; }
back() { mounted && [ -f $R/tmp/fathom-rescan ] && [ "$(cat $R/proc/sys/vm/dirty_bytes)" = 4194304 ] && [ "$(cat $LUN/forced_eject)" = 1 ]; }

for SH in sh dash; do
command -v $SH > /dev/null 2>&1 || continue
echo "-- $SH"

# 1. Ejected on the computer: seen at the next look, 0.2 s apart; the card back, new music looked for
fresh; echo ": > $LUN/file" > $T/st/at.0.2.7
run
check "ejected: over at the next look" '[ $rc -eq 0 ] && [ $(count $T/st/sleeps.0.2) -eq 7 ] && [ ! -f $T/st/sleeps.1 ]'
check "...the card mounted again, a rescan asked for, write-back as it was" 'back'
check "...everything that had the card open stopped first" 'grep -q "^fathom-bt stop" $T/st/calls && grep -q "^fathom-share off" $T/st/calls && grep -q "^fathom-wifi off" $T/st/calls'
check "the card was lent" 'grep -q "card lent" $T/st/out'

# 2. The cable pulled: over once it has stayed out for 3 s (15 looks)
fresh; echo "echo 'not attached' > $STATE" > $T/st/at.0.2.3
run
check "unplugged: over after 3 s" '[ $rc -eq 0 ] && [ $(count $T/st/sleeps.0.2) -eq 17 ] && back'

# 3. The link reset by the computer as the drive appears (a second "not attached"): not an end
fresh; echo "echo 'not attached' > $STATE" > $T/st/at.0.2.2; echo "echo configured > $STATE" > $T/st/at.0.2.7
echo ": > $LUN/file" > $T/st/at.0.2.30
run
check "a link reset: drive mode goes on until it's ejected" '[ $(count $T/st/sleeps.0.2) -eq 30 ] && back'

# 4. The computer asleep (the link suspended): not an end
fresh; echo "echo suspended > $STATE" > $T/st/at.0.2.2; echo ": > $LUN/file" > $T/st/at.0.2.60
run
check "a sleeping computer: drive mode goes on" '[ $(count $T/st/sleeps.0.2) -eq 60 ] && back'

# 5. Not on a computer: nothing done
fresh; echo "not attached" > $STATE
run
check "not connected: the card left alone" '[ $rc -eq 0 ] && mounted && [ ! -f $T/st/order ] && [ ! -s $LUN/file ]'

# 6. The card busy: said, and not lent
fresh; touch $T/st/busy
run
check "the card busy: not lent, and said so" '[ $rc -eq 1 ] && grep -q "in use" $R/tmp/fathom-usbdisk.msg && [ ! -s $LUN/file ] && mounted'

# 7. Back without Fathom on it (deleted on the computer): the card-out screen, no rescan
fresh; echo ": > $LUN/file; rm -rf $R/mnt/sd/fathom" > $T/st/at.0.2.4
run
check "back without Fathom: waiting for a card" 'grep -q "^start-stop-daemon .*fathom-cardout" $T/st/calls && [ -e $R/var/run/fathom-card-out ] && [ ! -f $R/tmp/fathom-rescan ]'

# 8. The boot report being written just then (the first seconds after start-up): waited for before the unmount
fresh; echo ": > $LUN/file" > $T/st/at.0.2.2; touch $T/st/real
: > $BUSY; ( $SLEEP 0.3; echo "report written" >> $T/st/order; rm -f $BUSY ) &
run
wait
check "the boot report being written: the unmount waits for it" '[ "$(sed -n 1p $T/st/order)" = "report written" ] && [ "$(sed -n 2p $T/st/order)" = umount ] && back'

done
rm -rf $T
echo "$ok passed, $bad failed"
[ $bad -eq 0 ]
