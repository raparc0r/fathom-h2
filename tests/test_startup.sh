#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional terms under GPLv3 section 7: see LICENSE.
#
# Off-device test of the start-up scripts around the player, against a root
# made here, with stand-ins for the player, fathom-ssh, fathom-usbdisk, ip,
# udhcpd, pidof and sleep:
#   S40usbnet   a USB network that isn't there doesn't hold the start-up up
#   S90fathom   the player started first and SSH set up beside it; SSH all
#               the same with no player (and the card-out screen), or with
#               autostart off; at stop, the
#               player's exit looked for 10 times a second, and how long it
#               took said (on the card when it was long, or it was killed)
#   sh tests/test_startup.sh           (under dash too, when there is one)
set -u
. "$(dirname "$0")/lib.sh"
mkdir -p $T/bin $T/st $R/usr/lib/fathom $R/usr/sbin
ROOTED="$ROOTED|etc"
rooted $SRC/etc/init.d/S40usbnet $T/S40usbnet
rooted $SRC/etc/init.d/S90fathom $T/S90fathom
rooted $SRC/usr/lib/fathom/card.sh $R/usr/lib/fathom/card.sh

# sleep: counted by how long was asked for; short ones pass at once (really
# 0.05 s each while st/hold is there), long ones (the 10 minutes between two
# looks at the logs) stay until they're killed. st/at.S.N (a script) runs after the
# Nth sleep of S seconds. While st/clock is there, the clock (/proc/uptime)
# moves on by what was asked, and each really takes 0.02 s.
cat > $T/bin/sleep <<X
#!/bin/sh
echo "\$1" >> $T/st/sleeps.\$1
case "\$1" in [0-9][0-9]*)
    if [ -f $T/st/minute ]; then rm -f $T/st/minute; exit 0; fi      # (one that passes at once)
    echo \$\$ >> $T/st/long.pids
    exec $SLEEP 30 ;;
esac
n=\$(wc -l < $T/st/sleeps.\$1); n=\$((n + 0))
[ -f $T/st/at.\$1.\$n ] && . $T/st/at.\$1.\$n
if [ -f $T/st/clock ]; then
    awk -v d="\$1" '{ printf "%.2f %s\\n", \$1 + d, \$2 }' $R/proc/uptime > $T/st/uptime && cat $T/st/uptime > $R/proc/uptime
    exec $SLEEP 0.02
fi
[ -f $T/st/hold ] && exec $SLEEP 0.05
exit 0
X
printf '#!/bin/sh\necho "ip $*" >> %s/st/net\n[ -e %s/sys/class/net/usb0 ]\n' $T $R > $T/bin/ip
printf '#!/bin/sh\necho "udhcpd $*" >> %s/st/net\n' $T > $T/bin/udhcpd
# pidof: the stand-in player's, while it runs
cat > $T/bin/pidof <<X
#!/bin/sh
p=\$(cat $T/st/player.pid 2>/dev/null) && kill -0 "\$p" 2>/dev/null && echo "\$p"
X
printf '#!/bin/sh\necho "$*" >> %s/st/daemons\n' $T > $T/bin/start-stop-daemon
printf '#!/bin/sh\necho sync >> %s/st/syncs\n' $T > $T/bin/sync
chmod +x $T/bin/*
export PATH=$T/bin:$PATH
# fathom-ssh: what it was asked noted, and when it was done; it takes its time while st/ssh.hold is there
cat > $R/usr/sbin/fathom-ssh <<X
#!/bin/sh
echo "\$1" >> $T/st/ssh
while [ -f $T/st/ssh.hold ]; do $SLEEP 0.02; done
echo "ssh: \$1 done" >&2
echo done >> $T/st/ssh
X
printf '#!/bin/sh\necho usbdisk >> %s/st/usbdisk\n' $T > $R/usr/sbin/fathom-usbdisk
chmod +x $R/usr/sbin/*
# The player: runs until st/player.exit says with what it exits, or it's
# told to stop ("player deaf": it doesn't listen to that)
PLAYER=$R/mnt/sd/fathom/bin/fathom-player
player() {
    mkdir -p $R/mnt/sd/fathom/bin
    if [ "${1:-}" = deaf ]; then term="trap '' TERM"; else term="trap 'exit 0' TERM"; fi
    cat > $PLAYER <<X
#!/bin/sh
echo \$\$ > $T/st/player.pid
echo run >> $T/st/player.runs
$term
while [ ! -f $T/st/player.exit ]; do $SLEEP 0.02; done
rc=\$(cat $T/st/player.exit); rm -f $T/st/player.exit
exit \$rc
X
    chmod +x $PLAYER
}

PIDF=$R/var/run/fathom.pid
LOG=$R/tmp/fathom-player.log
STOPLOG=$R/mnt/sd/fathom/player-stop.log
fresh() {
    [ -f $PIDF ] && $SH $T/S90fathom stop > /dev/null 2>&1
    rm -rf $R/sys $R/tmp $R/var $R/mnt $R/proc $T/st
    mkdir -p $T/st $R/sys/class/net $R/tmp $R/var/run $R/mnt/sd/fathom $R/proc
    echo "100.00 90.00" > $R/proc/uptime
    echo "$R/dev/mmcblk0p1 $R/mnt/sd vfat rw 0 0" > $R/proc/mounts
}
started() { $SH $T/S90fathom start > $T/st/out 2>&1; until_ '[ -f $T/st/player.pid ]'; p=$(cat $T/st/player.pid); }

for SH in sh dash; do
command -v $SH > /dev/null 2>&1 || continue
echo "-- $SH"

# --- S40usbnet ---
# 1. usb0 there (S30usbgadget made the gadget): set up before the next script, as before
fresh; mkdir $R/sys/class/net/usb0
$SH $T/S40usbnet start
check "usb0 there: set up at once, in order" \
    '[ "$(lines $T/st/net)" = "ip link set usb0 up ip addr add 10.42.0.1/24 dev usb0 udhcpd '$R'/etc/udhcpd.conf" ] && [ -f $R/tmp/udhcpd.leases ]'
check "...without a wait" '[ $(count $T/st/sleeps.0.2) -eq 0 ]'

# 2. usb0 late: the start-up goes on; it's set up when it comes
fresh; touch $T/st/hold; echo "mkdir $R/sys/class/net/usb0" > $T/st/at.0.2.3
$SH $T/S40usbnet start
check "usb0 not there: the start-up isn't held up" '[ ! -f $T/st/net ]'
rm $T/st/hold
until_ '[ $(count $T/st/net) -eq 3 ]'
check "...and it's set up once it comes" '[ $(count $T/st/net) -eq 3 ] && [ $(count $T/st/sleeps.0.2) -eq 3 ]'

# 3. usb0 never: given 10 s, then tried all the same, as before
fresh
$SH $T/S40usbnet start
until_ '[ $(count $T/st/net) -eq 3 ]'
check "usb0 never there: 10 s, then tried as before" '[ $(count $T/st/sleeps.0.2) -eq 50 ] && [ $(count $T/st/net) -eq 3 ]'

# --- S90fathom start ---
# 4. The player there: started without waiting for SSH's setting-up, which runs beside it
fresh; player; touch $T/st/ssh.hold
$SH $T/S90fathom start > $T/st/out 2>&1
until_ '[ -f $T/st/player.runs ] && [ -f $T/st/ssh ]'
check "the player started while fathom-ssh is still at it" '[ $(count $T/st/player.runs) -eq 1 ] && [ "$(lines $T/st/ssh)" = boot ]'
check "its loop's pid kept" 'kill -0 $(cat $PIDF)'
rm $T/st/ssh.hold
until_ '[ $(count $T/st/ssh) -eq 2 ]'
check "fathom-ssh done beside it, what it said in the player's log" '[ "$(lines $T/st/ssh)" = "boot done" ] && grep -q "ssh: boot done" $LOG'
# 5. Started again while it runs (a card put in during the start): nothing more
$SH $T/S90fathom start > $T/st/out 2>&1
$SLEEP 0.1
check "started twice: one player, one call to fathom-ssh" '[ $(count $T/st/player.runs) -eq 1 ] && [ $(count $T/st/ssh) -eq 2 ]'
p=$(cat $T/st/player.pid)
$SH $T/S90fathom stop
check "stop: the player and its loop gone" '! kill -0 $p 2>/dev/null && [ ! -f $PIDF ]'
check "...how long it took said in its log, not on the card" 'grep -q "^fathom-player stopped in 0\.[0-9][0-9] s$" $LOG && [ ! -e $STOPLOG ]'

# 6. No player on the card: SSH all the same, before the script ends
fresh
$SH $T/S90fathom start > $T/st/out 2>&1
check "no player: fathom-ssh run all the same" '[ "$(lines $T/st/ssh)" = "boot done" ] && grep -q "not found" $T/st/out'
check "...and no loop" '[ ! -f $PIDF ] && [ ! -f $T/st/player.runs ]'
check "...and the card-out screen asked for (not the logo for ever), the card marked unusable" \
    'grep -q "fathom-cardout" $T/st/daemons 2>/dev/null && [ -e $R/var/run/fathom-card-out ]'

# 7. Autostart off (development): SSH, no player
fresh; player; touch $R/mnt/sd/fathom/no-autostart
$SH $T/S90fathom start > $T/st/out 2>&1
$SLEEP 0.1
check "autostart off: fathom-ssh run, the player not" '[ "$(lines $T/st/ssh)" = "boot done" ] && grep -q "autostart disabled" $T/st/out && [ ! -f $T/st/player.runs ] && [ ! -f $PIDF ]'
check "...and no card-out screen: the console is what was asked for" '[ ! -f $T/st/daemons ] && [ ! -e $R/var/run/fathom-card-out ]'

# --- S90fathom stop ---
# 8. A player slow to stop (over a second): waited for, and said on the card too
fresh; player deaf; touch $T/st/clock; echo "echo 0 > $T/st/player.exit" > $T/st/at.0.1.12
started
$SH $T/S90fathom stop
check "a slow stop: waited for, looked at every 0.1 s" '! kill -0 $p 2>/dev/null && [ $(count $T/st/sleeps.0.1) -ge 12 ] && [ $(count $T/st/sleeps.0.1) -lt 50 ]'
check "...and said in the log and on the card" 'grep -q "^fathom-player stopped in [1-4]\.[0-9][0-9] s$" $LOG && grep -q " fathom-player stopped in [1-4]\.[0-9][0-9] s$" $STOPLOG'

# 9. A player that doesn't stop: killed after 5 s, and said on the card (whose note stays short)
fresh; player deaf; touch $T/st/clock
started
head -c 9000 /dev/zero | tr '\0' 'x' > $STOPLOG; echo >> $STOPLOG
$SH $T/S90fathom stop
until_ '! kill -0 $p 2>/dev/null'
check "a player that won't stop: killed after 5 s" '! kill -0 $p 2>/dev/null && [ $(count $T/st/sleeps.0.1) -eq 50 ]'
check "...and said in the log" 'grep -q "^fathom-player hadn.t stopped after 5\.00 s: killed$" $LOG'
check "...and on the card, with the day and the hour" 'tail -n 1 $STOPLOG | grep -q "^20[0-9][0-9]-[0-9][0-9]-[0-9][0-9] [0-9:]* fathom-player hadn.t stopped after 5\.00 s: killed$"'
check "the card's note kept short" '[ $(wc -c < $STOPLOG) -le 4096 ] && [ ! -e $STOPLOG.cut ]'

# 10. The card gone (taken out: that's why it stops): nothing written where it was
fresh; player deaf; touch $T/st/clock
started
: > $R/proc/mounts
$SH $T/S90fathom stop
check "killed with the card out: said in the log only" 'grep -q "killed" $LOG && [ ! -e $STOPLOG ]'

# 11. No player running: nothing to do
fresh
$SH $T/S90fathom stop; rc=$?
check "stop with no player: nothing done" '[ $rc -eq 0 ] && [ ! -e $LOG ]'

# --- S90fathom's loop ---
runs() { count $T/st/player.runs; }
exits() { echo "$1" > $T/st/player.exit; until_ "[ \$(runs) -eq $2 ]"; }
# 12. The player gone after a long run (a crash, Reset all settings): started again at once.
# (Its exit is seen at once: the 10 minutes between two looks at the logs, 30 s here, aren't sat out.)
fresh; player; started
echo "150.00 90.00" > $R/proc/uptime
exits 0 2
check "after a long run: started again, with no wait" '[ $(runs) -eq 2 ] && [ $(count $T/st/sleeps.2) -eq 0 ] && grep -q "^fathom-player exited (0), restarting$" $LOG'
# 13. ...after a short one (it can't start): 2 s first, as before
exits 1 3
check "after a short run: 2 s first" '[ $(runs) -eq 3 ] && [ $(count $T/st/sleeps.2) -eq 1 ] && grep -q "^fathom-player exited (1), restarting$" $LOG'
# 14. USB drive mode asked for (42): the card lent, then the player again, with no wait
exits 42 4
check "exit 42: fathom-usbdisk run, then the player, with no wait" '[ $(runs) -eq 4 ] && [ $(count $T/st/usbdisk) -eq 1 ] && [ $(count $T/st/sleeps.2) -eq 1 ] && ! grep -q "exited (42)" $LOG'
# 15. The loop that looks at the logs: one for each run, gone with its player
until_ '[ $(count $T/st/long.pids) -eq 4 ]'
check "one look-at-the-logs loop per run, the earlier ones gone" \
    '[ $(count $T/st/long.pids) -eq 4 ] && ! kill -0 $(sed -n 1p $T/st/long.pids) 2>/dev/null && ! kill -0 $(sed -n 3p $T/st/long.pids) 2>/dev/null && kill -0 $(sed -n 4p $T/st/long.pids) && kill -0 $(cat $R/var/run/fathom-trim.pid)'
t=$(cat $R/var/run/fathom-trim.pid)
$SH $T/S90fathom stop
until_ '! kill -0 $t 2>/dev/null'
check "stop: that loop and its sleep gone too" '! kill -0 $t 2>/dev/null && ! kill -0 $(sed -n 4p $T/st/long.pids) 2>/dev/null && [ -z "$(ls $R/var/run)" ]'

# 16. Ten minutes on: a log over 256 KB keeps its last 128 KB, and the looking goes on
fresh; player; touch $T/st/minute
head -c 300000 /dev/zero | tr '\0' 'x' > $R/tmp/other.log
started
until_ '[ $(wc -c < $R/tmp/other.log) -eq 131072 ] && [ $(count $T/st/sleeps.600) -eq 2 ]'
check "a big log cut to its last 128 KB after 10 minutes" '[ $(wc -c < $R/tmp/other.log) -eq 131072 ] && [ ! -e $R/tmp/other.log.cut ] && [ $(count $T/st/sleeps.600) -eq 2 ]'

# 17. A player that can't start (gone within 20 s, five times running): its log kept on the card, then tried every half minute
fresh; player; started
for n in 2 3 4 5; do exits 1 $n; done
echo 1 > $T/st/player.exit
until_ '[ -f $T/st/sleeps.28 ]'
check "five quick exits: the log kept on the card, and a longer wait" 'grep -q "exited (1)" $R/mnt/sd/fathom/player-crash.log && [ $(count $T/st/sleeps.28) -eq 1 ] && [ $(count $T/st/sleeps.2) -eq 4 ] && [ $(runs) -eq 5 ]'

fresh
done
rm -rf $T
finish
