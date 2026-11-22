#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional terms under GPLv3 section 7: see LICENSE.
#
# Off-device test of the udhcpc hook fathom-ntp: NTP tried again with longer
# and longer waits while Wi-Fi stays up, one loop at a time, the player told
# when the clock is set. With stand-ins for ntpd, timeout, sleep and logger:
# no network, and no real waiting.
#   sh tests/test_fathom_ntp.sh        (in the build container too: docker/run.sh sh tests/test_fathom_ntp.sh)
set -u
here=$(cd "$(dirname "$0")/.." && pwd)
SRC=${SRC:-$here/buildroot/external/board/h2/rootfs-overlay}
T=$(mktemp -d)
mkdir -p $T/run $T/st $T/bin
sed -e "s#^RUN=.*#RUN=$T/run#" -e "s#^CARRIER=.*#CARRIER=$T/carrier#" \
    $SRC/usr/share/udhcpc/default.script.d/fathom-ntp > $T/hook
chmod +x $T/hook
SLEEP=$(command -v sleep)
# ntpd: answers once st/answer_after tries have gone unanswered (none: never)
cat > $T/bin/ntpd <<X
#!/bin/sh
echo "\$*" >> $T/st/calls
n=\$(wc -l < $T/st/calls)
[ -f $T/st/answer_after ] && [ \$n -gt \$(cat $T/st/answer_after) ] && exit 0
exit 1
X
printf '#!/bin/sh\nshift\nexec "$@"\n' > $T/bin/timeout
printf '#!/bin/sh\necho "$*" >> %s/st/log\n' $T > $T/bin/logger
# sleep: the wait asked for noted; st/hold makes it a real (long) one, to be woken
cat > $T/bin/sleep <<X
#!/bin/sh
echo "\$1" >> $T/st/sleeps
echo \$\$ > $T/st/sleep.pid
[ -f $T/st/hold ] && exec $SLEEP 30
exec $SLEEP 0.02
X
chmod +x $T/bin/*
export PATH=$T/bin:$PATH
LOCK=$T/run/fathom-ntp.lock

ok=0; bad=0
check() { if eval "$2"; then echo "ok   $1"; ok=$((ok+1)); else echo "FAIL $1"; bad=$((bad+1)); fi; }
# until $1 holds (a few seconds at most)
until_() { i=0; while ! eval "$1" && [ $i -lt 200 ]; do $SLEEP 0.02; i=$((i+1)); done; }
fresh() { until_ '[ ! -d $LOCK ]'; rm -rf $T/st/* $T/run/*; }
lines() { [ -f "$1" ] && tr '\n' ' ' < "$1" | sed 's/ $//'; }

# 1. An answer at once: the clock set, the player told, nothing waiting
echo 1 > $T/carrier; echo 0 > $T/st/answer_after
interface=wlan0 ntpsrv=10.0.0.1 $T/hook bound
until_ '[ -f $T/run/fathom-clock ] && [ ! -d $LOCK ]'
check "answered: the player told" '[ -s $T/run/fathom-clock ]'
check "the network's own server asked" '[ "$(lines $T/st/calls)" = "-n -q -p 10.0.0.1" ]'
check "nothing waiting after" '[ ! -d $LOCK ] && [ ! -f $T/st/sleeps ] && [ ! -f $T/run/fathom-ntp-fails ]'

# 2. No server from DHCP: the pool
fresh; echo 1 > $T/carrier; echo 0 > $T/st/answer_after
interface=wlan0 ntpsrv= $T/hook renew
until_ '[ -f $T/run/fathom-clock ]'
check "no server named: the pool" '[ "$(lines $T/st/calls)" = "-n -q -p 0.pool.ntp.org -p 1.pool.ntp.org" ]'

# 3. Five tries unanswered, then an answer: 1, 5, 15, 30, 30 minutes between
fresh; echo 1 > $T/carrier; echo 5 > $T/st/answer_after
interface=wlan0 ntpsrv=10.0.0.1 $T/hook bound
until_ '[ -f $T/run/fathom-clock ] && [ ! -d $LOCK ]'
check "waits of 1, 5, 15, 30, 30 minutes" '[ "$(lines $T/st/sleeps)" = "60 300 900 1800 1800" ]'
check "six tries, the last answered" '[ $(wc -l < $T/st/calls) -eq 6 ] && [ -s $T/run/fathom-clock ]'
check "unanswered tries counted for the player" 'grep -q "(5): again in 1800 s" $T/st/log'
check "the count gone once set" '[ ! -f $T/run/fathom-ntp-fails ]'

# 4. Never an answer, a long wait: a new address wakes the loop (one loop, one try at a time)
fresh; echo 1 > $T/carrier; touch $T/st/hold
interface=wlan0 ntpsrv=10.0.0.1 $T/hook bound
until_ '[ -f $T/st/sleeps ]'
pid=$(cat $LOCK/pid)
check "waiting after one try, the count at 1" '[ $(wc -l < $T/st/calls) -eq 1 ] && [ "$(cat $T/run/fathom-ntp-fails)" = 1 ]'
interface=wlan0 ntpsrv=10.0.0.2 $T/hook renew
until_ '[ $(wc -l < $T/st/sleeps) -eq 2 ]'
check "a new address: tried again at once, with its server" '[ "$(tail -n 1 $T/st/calls)" = "-n -q -p 10.0.0.2" ]'
check "the same loop, waiting longer" '[ "$(cat $LOCK/pid)" = "$pid" ] && [ "$(lines $T/st/sleeps)" = "60 300" ]'
check "the count at 2" '[ "$(cat $T/run/fathom-ntp-fails)" = 2 ]'
interface=wlan0 $T/hook bound; until_ '[ $(wc -l < $T/st/sleeps) -eq 3 ]'
interface=wlan0 $T/hook renew; until_ '[ $(wc -l < $T/st/sleeps) -eq 4 ]'
check "no second loop" '[ $(wc -l < $T/st/calls) -eq 4 ] && [ "$(cat $LOCK/pid)" = "$pid" ]'
check "... and the waits go on growing" '[ "$(lines $T/st/sleeps)" = "60 300 900 1800" ]'
spid=$(cat $T/st/sleep.pid)
# 5. The address lost: the loop ends, its wait with it, the count cleared
interface=wlan0 $T/hook deconfig
until_ '[ ! -d $LOCK ] && ! kill -0 $pid 2>/dev/null'
check "deconfig: the loop gone" '! kill -0 $pid 2>/dev/null && [ ! -d $LOCK ]'
check "... its wait too" '! kill -0 $spid 2>/dev/null'
check "... and the count" '[ ! -f $T/run/fathom-ntp-fails ]'
rm -f $T/st/hold

# 6. Wi-Fi gone while waiting: no more tries, the loop ends
fresh; echo 1 > $T/carrier
interface=wlan0 ntpsrv=10.0.0.1 $T/hook bound
until_ '[ -f $T/st/sleeps ]'
echo 0 > $T/carrier
until_ '[ ! -d $LOCK ] && ! kill -0 $pid 2>/dev/null'
check "Wi-Fi gone: the loop ends" '[ ! -d $LOCK ] && [ $(wc -l < $T/st/calls) -eq 1 ]'

# 7. Not joined at all: no try
fresh; echo 0 > $T/carrier; echo 0 > $T/st/answer_after
interface=wlan0 $T/hook bound
until_ '[ ! -d $LOCK ]'; $SLEEP 0.1
check "no network joined: no try" '[ ! -f $T/st/calls ] && [ ! -d $LOCK ]'

# 8. Another interface (the USB link): nothing
fresh; echo 1 > $T/carrier
interface=usb0 $T/hook bound; $SLEEP 0.1
check "usb0: nothing" '[ ! -f $T/st/calls ] && [ ! -d $LOCK ]'

# 9. A loop that died without tidying up doesn't stop the next
fresh; echo 1 > $T/carrier; echo 0 > $T/st/answer_after
mkdir $LOCK; echo 999999 > $LOCK/pid
interface=wlan0 ntpsrv=10.0.0.1 $T/hook bound
until_ '[ -f $T/run/fathom-clock ]'
check "a stale lock: tried anyway" '[ -s $T/run/fathom-clock ]'

until_ '[ ! -d $LOCK ] && ! kill -0 $pid 2>/dev/null'
rm -rf $T
echo "$ok passed, $bad failed"
[ $bad -eq 0 ]
