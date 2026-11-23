#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional terms under GPLv3 section 7: see LICENSE.
#
# Off-device test of fathom-ssh: root's password set with a quick hash (the
# build's own when chpasswd won't take that), SSH started as the settings say,
# and two calls at once (start-up's "boot" and the player's "on") starting one
# dropbear between them, with a lock that's never left in the way. With
# stand-ins for chpasswd and dropbear, against a root made here.
#   sh tests/test_fathom_ssh.sh        (under dash too, when there is one)
set -u
here=$(cd "$(dirname "$0")/.." && pwd)
SRC=${SRC:-$here/buildroot/external/board/h2/rootfs-overlay}
T=$(mktemp -d)
R=$T/root
mkdir -p $T/bin $T/st
sed -E -e "s#(^|[ =\"(<>])/(usr|mnt|sys|tmp|dev|proc|var|root)/#\\1$R/\\2/#g" -e "s#$R/dev/null#/dev/null#g" \
    -e "s#chmod 700 /root #chmod 700 $R/root #" $SRC/usr/sbin/fathom-ssh > $T/fathom-ssh
SLEEP=$(command -v sleep)
# chpasswd: what it was asked noted; slow (st/slow), or without -c (st/old: an older busybox)
cat > $T/bin/chpasswd <<X
#!/bin/sh
[ -f $T/st/old ] && [ \$# -gt 0 ] && { echo "\$* refused" >> $T/st/chpasswd; exit 1; }
[ -f $T/st/slow ] && $SLEEP 0.3
echo "\$* \$(cat)" >> $T/st/chpasswd
X
# sleep: short, and counted (the 20 s a lock is waited for pass at once)
printf '#!/bin/sh\necho "$1" >> %s/st/sleeps\nexec %s 0.01\n' $T $SLEEP > $T/bin/sleep
chmod +x $T/bin/*
export PATH=$T/bin:$PATH
# dropbear on the card (one program under several names): the key generator
# makes a key; the server takes a moment to come up, as the real one does,
# then stays (a sleep whose pid it leaves where -P says)
mkdir -p $R/mnt/sd/fathom/ssh
cat > $R/mnt/sd/fathom/ssh/dropbearmulti <<X
#!/bin/sh
if [ "\$1" = dropbearkey ]; then echo key > "\$5"; exit 0; fi
echo started >> $T/st/dropbear
( $SLEEP 0.05; $SLEEP 30 & echo \$! > "\$6"; echo \$! >> $T/st/pids ) > /dev/null 2>&1 &
X
chmod +x $R/mnt/sd/fathom/ssh/dropbearmulti
PIDF=$R/var/run/dropbear.pid
LOCK=$R/var/run/fathom-ssh.lock

ok=0; bad=0
check() { if eval "$2"; then echo "ok   $1"; ok=$((ok+1)); else echo "FAIL $1"; bad=$((bad+1)); fi; }
count() { if [ -f "$1" ]; then echo $(($(wc -l < "$1") + 0)); else echo 0; fi; }
stop_all() { [ -f $T/st/pids ] && kill $(cat $T/st/pids) 2>/dev/null; }
fresh() {
    stop_all
    rm -rf $T/st $R/var $R/root $R/mnt/sd/fathom/keys $R/mnt/sd/fathom/settings.ini
    mkdir -p $T/st $R/var/run $R/root
}
settings() { printf 'ssh.on = %s\r\nssh.password = %s\r\n' "$1" "$2" > $R/mnt/sd/fathom/settings.ini; }

for SH in sh dash; do
command -v $SH > /dev/null 2>&1 || continue
echo "-- $SH"
ssh_() { $SH $T/fathom-ssh "$@"; }

# 1. At start-up, SSH on in the settings: the password set with the quick hash, dropbear up
fresh; settings 1 abcd2345
ssh_ boot; rc=$?
check "boot, SSH on: dropbear running" '[ $rc -eq 0 ] && [ "$(ssh_ status)" = on ] && [ $(count $T/st/dropbear) -eq 1 ]'
check "the password hashed with MD5" '[ "$(cat $T/st/chpasswd)" = "-c md5 root:abcd2345" ]'
check "the host key made, and copied to RAM" '[ -s $R/mnt/sd/fathom/keys/ssh_host_ed25519_key ] && [ -s $R/var/run/fathom-ssh/host_key ]'
check "no lock left" '[ ! -e $LOCK ]'

# 2. The player's own call just after: the password again, no second dropbear
echo abcd2345 | ssh_ on > $T/st/out; rc=$?
check "on, already running: still one dropbear" '[ $rc -eq 0 ] && [ "$(cat $T/st/out)" = on ] && [ $(count $T/st/dropbear) -eq 1 ]'
check "no lock left" '[ ! -e $LOCK ]'

# 3. Off: stopped, the password still set
echo abcd2345 | ssh_ off > $T/st/out
check "off: dropbear stopped" '[ "$(cat $T/st/out)" = off ] && [ "$(ssh_ status)" = off ] && [ $(count $T/st/chpasswd) -eq 3 ]'

# 4. SSH off in the settings: the password set (the serial console's), nothing started
fresh; settings 0 abcd2345
ssh_ boot; rc=$?
check "boot, SSH off: the password set, no dropbear" '[ $rc -eq 0 ] && [ $(count $T/st/chpasswd) -eq 1 ] && [ $(count $T/st/dropbear) -eq 0 ]'
check "no lock left" '[ ! -e $LOCK ]'

# 5. No password in the settings yet (a first start): left to the player
fresh
ssh_ boot; rc=$?
check "boot, no password yet: nothing done, no lock left" '[ $rc -eq 0 ] && [ ! -f $T/st/chpasswd ] && [ ! -e $LOCK ]'

# 6. A chpasswd that doesn't know -c: the build's own hash, as before
fresh; settings 1 abcd2345; touch $T/st/old
ssh_ boot; rc=$?
check "chpasswd without -c: set the old way" '[ $rc -eq 0 ] && [ "$(tail -n 1 $T/st/chpasswd)" = " root:abcd2345" ] && [ "$(ssh_ status)" = on ]'

# 7. Start-up's call and the player's at once: one dropbear
fresh; settings 1 abcd2345; touch $T/st/slow
ssh_ boot &
echo abcd2345 | ssh_ on > $T/st/out; rc=$?
wait
check "two calls at once: both done, one dropbear" '[ $rc -eq 0 ] && [ "$(cat $T/st/out)" = on ] && [ $(count $T/st/chpasswd) -eq 2 ] && [ $(count $T/st/dropbear) -eq 1 ]'
check "no lock left" '[ ! -e $LOCK ]'

# 8. A lock left by a call that was killed: taken over at once
fresh; settings 1 abcd2345
$SLEEP 0 & dead=$!; wait
mkdir $LOCK; echo $dead > $LOCK/pid
ssh_ boot; rc=$?
check "a dead call's lock: taken over without waiting" '[ $rc -eq 0 ] && [ "$(ssh_ status)" = on ] && [ $(count $T/st/sleeps) -lt 30 ] && [ ! -e $LOCK ]'

# 9. A lock held by something that never lets go: taken over after 20 s
fresh; settings 1 abcd2345
mkdir $LOCK; echo $$ > $LOCK/pid
ssh_ boot; rc=$?
check "a lock never let go of: SSH started all the same, after 20 s" '[ $rc -eq 0 ] && [ "$(ssh_ status)" = on ] && [ $(grep -c "^0.1$" $T/st/sleeps) -ge 200 ]'

# 10. Nowhere to make a lock: without one, as before
fresh; settings 1 abcd2345
sed "s#^LOCK=.*#LOCK=$T/nowhere/lock#" $T/fathom-ssh > $T/fathom-ssh.nolock
$SH $T/fathom-ssh.nolock boot; rc=$?
check "no lock can be made: SSH started all the same" '[ $rc -eq 0 ] && [ "$(ssh_ status)" = on ] && [ $(count $T/st/sleeps) -lt 30 ]'

done
stop_all
rm -rf $T
echo "$ok passed, $bad failed"
[ $bad -eq 0 ]
