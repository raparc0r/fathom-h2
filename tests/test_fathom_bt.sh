#!/bin/sh
# Off-device test of fathom-bt's device lists (scan, paired) and of its start, with stand-ins
# for bluetoothctl, the daemons and the chip; and of fathom-bluetooth's wait for the chip.
#   sh tests/test_fathom_bt.sh         (in the build container too: docker/run.sh sh tests/test_fathom_bt.sh)
set -u
here=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d); SRC=${SRC:-$here/buildroot/external/board/h2/rootfs-overlay}
mkdir -p $T/bt/usr/bin $T/run $T/bin
# timeout (busybox's on the H2; macOS has none): its command run as it is
printf '#!/bin/sh\n[ "$1" = -s ] && shift 2\nshift\nexec "$@"\n' > $T/bin/timeout; chmod +x $T/bin/timeout; export PATH=$T/bin:$PATH
# (nothing of the machine it runs on is touched: every path of the H2's becomes one in $T)
sed -e "s#^BT=.*#BT=$T/bt#" -e "s#^RUN=.*#RUN=$T/run#" -e "s#/mnt/sd/fathom#$T/sd#g" -e "s#/var/lib#$T/varlib#g" \
    -e "s#/var/run/dbus#$T/dbus#g" -e "s#>> /tmp/#>> $T/log/#g" -e "s#/etc/fathom#$T/etc#g" -e "s#^HCI=.*#HCI=$T/hci0-bt#" \
    $SRC/usr/sbin/fathom-bt > $T/fathom-bt
chmod +x $T/fathom-bt
# Known devices: headphones (paired, connected), a speaker found by the scan, a phone
# (not audio), and earbuds whose name has a '|' and that the batch leaves out.
cat > $T/devs <<'X'
AA:00:00:00:00:01;Anna’s AirPods;audio-headphones;yes;yes;0000110b-0000-1000-8000-00805f9b34fb
AA:00:00:00:00:02;Kitchen speaker;;no;no;0000110b-0000-1000-8000-00805f9b34fb
AA:00:00:00:00:03;Bob's phone;phone;yes;no;0000110a-0000-1000-8000-00805f9b34fb
AA:00:00:00:00:04;Buds | Pro;audio-headset;yes;no;
X
cat > $T/bt/usr/bin/bluetoothctl <<X
#!/bin/sh
D=$T/devs; echo "\$*" >> $T/calls
[ "\$1" = --timeout ] && shift 2
info() {   # as bluetoothctl prints it; \$2 what each line starts with
    awk -F';' -v a="\$1" -v p="\$2" '\$1 == a { print p "Device " a " (public)"; print p "\tName: " \$2; print p "\tAlias: " \$2
        if (\$3 != "") print p "\tIcon: " \$3; print p "\tPaired: " \$4; print p "\tConnected: " \$5
        if (\$6 != "") print p "\tUUID: " (\$6 ~ /^0000110b/ ? "Audio Sink  " : "Audio Source") "              (" \$6 ")" }' \$D
}
case "\$1" in
show)    # the controller, once bluetoothd has it (st/show-after looks from now)
    n=\$(cat $T/st/shows 2>/dev/null || echo 0); echo \$((n + 1)) > $T/st/shows
    [ \$n -ge \$(cat $T/st/show-after 2>/dev/null || echo 0) ] && cat $T/st/show || echo "No default controller available" ;;
devices)
    if [ "\${2:-}" = Paired ]; then awk -F';' '\$4=="yes"{print "Device "\$1" "\$2}' \$D
    else awk -F';' '{print "Device "\$1" "\$2}' \$D; fi ;;
info) info "\$2" "" ;;
"")     # interactive, fed on its input: prompts, echo, carriage returns, colours, news
    printf 'Agent registered\r\n[\033[0;94mbluetooth\033[0m]# '
    printf '[\001\033[0;93m\002NEW\001\033[0m\002] Device AA:00:00:00:00:02 Kitchen speaker\r\n'
    while read -r cmd a; do
        printf '%s\r\n' "\$cmd \$a"
        [ "\$a" = AA:00:00:00:00:04 ] && continue
        info "\$a" "\r\033[K" | awk '{ printf "%s\r\n", \$0 }'
        printf '[\033[0;94mbluetooth\033[0m]# '
    done
    printf 'quit\r\n' ;;
esac
X
chmod +x $T/bt/usr/bin/bluetoothctl
sleep 300 & echo $! > $T/run/bluetoothd.pid
ok=0; bad=0
check() { if eval "$2"; then echo "ok   $1"; ok=$((ok+1)); else echo "FAIL $1"; bad=$((bad+1)); fi; }

out=$($T/fathom-bt paired); calls=$(wc -l < $T/calls)
check "paired: audio devices only, with their icons and connected flags" '[ "$out" = "$(printf "AA:00:00:00:00:01|Anna’s AirPods|audio-headphones|1\nAA:00:00:00:00:04|Buds | Pro|audio-headset|0")" ]'
check "paired: one list, one batch, one device asked on its own" '[ $calls = 3 ] && grep -qx "info AA:00:00:00:00:04" $T/calls'
: > $T/calls
out=$($T/fathom-bt scan 1)
check "scan: every audio device, paired flags, no icon: audio" '[ "$out" = "$(printf "AA:00:00:00:00:01|Anna’s AirPods|audio-headphones|1\nAA:00:00:00:00:02|Kitchen speaker|audio|0\nAA:00:00:00:00:04|Buds | Pro|audio-headset|1")" ]'
check "scan: the scan, the list, the batch and the one left out" '[ $(wc -l < $T/calls) = 4 ]'

kill $(cat $T/run/bluetoothd.pid) 2>/dev/null; rm -f $T/run/bluetoothd.pid

# --- start: the chip, D-Bus, bluetoothd, bluealsa, the AVRCP helper ---
mkdir -p $T/st $T/sd/bin $T/log $T/bt/usr/libexec/bluetooth
if grep -v '^#' $T/fathom-bt | grep -q -E '(^|[^[:alnum:]_.-])/(var/lib|var/run/dbus|mnt/sd|etc/fathom)|>> /tmp/[a-z-]*\.log'; then
    echo "FAIL the script still names the machine's own paths: its start isn't run here"; bad=$((bad+1))
    echo "$ok passed, $bad failed"; exit 1
fi
stay='while :; do sleep 0.1; done'
printf '#!/bin/sh\necho "$*" >> %s/st/chip\n[ "$1" = on ] && [ -f %s/st/no-chip ] && exit 1\nexit 0\n' $T $T > $T/bin/fathom-bluetooth
printf '#!/bin/sh\necho "$*" >> %s/st/log\n' $T > $T/bin/logger
printf '#!/bin/sh\nexit 0\n' > $T/bin/sync
printf '#!/bin/sh\n( %s ) > /dev/null 2>&1 &\necho $!\n' "$stay" > $T/bt/usr/bin/dbus-daemon
printf '#!/bin/sh\n[ -f %s/st/btd-dies ] && exit 1\n%s\n' $T "$stay" > $T/bt/usr/libexec/bluetooth/bluetoothd
printf '#!/bin/sh\necho "$*" >> %s/st/ba\nif [ "$1" = -h ]; then cat %s/st/ba-help; exit 0; fi\n%s\n' $T $T "$stay" > $T/bt/usr/bin/bluealsad
printf '#!/bin/sh\n%s\n' "$stay" > $T/sd/bin/fathom-avrcp
chmod +x $T/bin/fathom-bluetooth $T/bin/logger $T/bin/sync $T/bt/usr/bin/dbus-daemon $T/bt/usr/libexec/bluetooth/bluetoothd $T/bt/usr/bin/bluealsad $T/sd/bin/fathom-avrcp
# bluealsa's help, as 4.3.1 prints the parts the script reads
cat > $T/st/ba-help <<'X'
Usage:
  bluealsad -p PROFILE [OPTION]...
  --io-rt-priority=NUM		real-time priority for IO threads
  --sbc-quality=MODE		set SBC encoder quality mode
  --aac-bitrate=BPS		CBR bitrate or max peak for VBR
Available BT profiles:
  - a2dp-source	Advanced Audio Source (v1.4)
Available BT audio codecs:
  a2dp-source:	SBC, AAC, aptX
  a2dp-sink:	SBC
X
controller() { printf 'Controller 00:11:22:33:44:55 (public)\n\tName: Fathom H2\n\tPowered: %s\n\tDiscoverable: no\n\tPairable: %s\n' $1 $2 > $T/st/show; }
alive() { for d in "$@"; do kill -0 "$(cat $T/run/$d.pid 2>/dev/null)" 2>/dev/null || return 1; done; }
begin() { : > $T/calls; : > $T/st/ba; : > $T/st/chip; echo 0 > $T/st/shows; }

# 1. A first start: bluetoothd has powered the controller and made it pairable by itself
# (AutoEnable, AlwaysPairable): nothing more asked of it; bluealsa's help read once
controller yes yes; echo 0 > $T/st/show-after; echo "bluetooth.range = 0" > $T/sd/settings.ini; begin
out=$($T/fathom-bt start 2>$T/st/err); rc=$?; sleep 0.2
check "start: on, everything running" '[ "$out" = on ] && [ $rc = 0 ] && alive dbus bluetoothd bluealsa avrcp'
check "start: the chip turned on once" '[ "$(cat $T/st/chip)" = on ]'
check "start: already on and pairable: neither asked for" '! grep -q "power on" $T/calls && ! grep -q "pairable on" $T/calls'
check "start: bluealsa asked what it can do, then run with aptX and real-time IO" '[ "$(cat $T/st/ba)" = "$(printf "%s\n%s" "-h" "-p a2dp-source -c aptX --io-rt-priority=10 --a2dp-force-audio-cd --a2dp-volume")" ]'
check "start: already running: answered at once" '[ "$($T/fathom-bt start)" = on ] && [ "$(cat $T/st/chip)" = on ]'
$T/fathom-bt stop >/dev/null 2>&1; sleep 0.2
check "stop: everything ended, the chip off" '! alive dbus && ! alive bluetoothd && ! alive bluealsa && ! alive avrcp && [ "$(tail -1 $T/st/chip)" = off ]'

# 2. The next start doesn't run bluealsa for its help again; "Longer range" from the same answer
begin; echo "bluetooth.range = 1" > $T/sd/settings.ini
out=$($T/fathom-bt start 2>$T/st/err); sleep 0.2
check "start again: bluealsa's help not asked for again" '[ "$out" = on ] && ! grep -qx -- "-h" $T/st/ba'
check "start again: longer range: SBC medium, AAC 128 kb/s, no aptX" '[ "$(cat $T/st/ba)" = "-p a2dp-source --io-rt-priority=10 --sbc-quality=medium --aac-bitrate=128000 --a2dp-force-audio-cd --a2dp-volume" ]'
$T/fathom-bt stop >/dev/null 2>&1

# 3. Another bluealsa in its place (an older one: no aptX, no real-time option): asked again
echo "# another build" >> $T/bt/usr/bin/bluealsad
printf 'Usage:\n  --sbc-quality=MODE\nAvailable BT profiles:\n  - a2dp-source\tAdvanced Audio Source (v1.4)\n  a2dp-source:\tSBC\n' > $T/st/ba-help
begin; echo "bluetooth.range = 0" > $T/sd/settings.ini
$T/fathom-bt start >/dev/null 2>&1; sleep 0.2
check "another bluealsa: asked again, run with only what it has" '[ "$(cat $T/st/ba)" = "$(printf "%s\n%s" "-h" "-p a2dp-source --a2dp-force-audio-cd --a2dp-volume")" ]'
$T/fathom-bt stop >/dev/null 2>&1

# 4. A bluealsa that can't even say what it does: not remembered (asked again next time)
echo "# and another" >> $T/bt/usr/bin/bluealsad
echo "bluealsad: error while loading shared libraries" > $T/st/ba-help; begin
$T/fathom-bt start >/dev/null 2>&1; $T/fathom-bt stop >/dev/null 2>&1
begin; $T/fathom-bt start >/dev/null 2>&1; sleep 0.2
check "an answer that isn't its help isn't kept" 'grep -qx -- "-h" $T/st/ba'
$T/fathom-bt stop >/dev/null 2>&1

# 5. The controller off and not pairable when bluetoothd shows it: both asked for, as before
controller no no; begin
$T/fathom-bt start >/dev/null 2>&1
check "start: not on, not pairable: both asked for" 'grep -qx "power on" $T/calls && grep -qx "pairable on" $T/calls'
$T/fathom-bt stop >/dev/null 2>&1
controller yes no; begin
$T/fathom-bt start >/dev/null 2>&1
check "start: on, not pairable: only that asked for" '! grep -q "power on" $T/calls && grep -qx "pairable on" $T/calls'
$T/fathom-bt stop >/dev/null 2>&1

# 6. The controller only there at the fourth look (the firmware patch still loading): waited for
controller yes yes; echo 3 > $T/st/show-after; begin
out=$($T/fathom-bt start 2>/dev/null)
check "start: the controller waited for" '[ "$out" = on ] && [ "$(cat $T/st/shows)" = 4 ] && ! grep -q "power on" $T/calls'
$T/fathom-bt stop >/dev/null 2>&1
# ...never there (14 looks): started all the same, and both asked for, as before
echo 99 > $T/st/show-after; begin
out=$($T/fathom-bt start 2>/dev/null)
check "start: no controller in the wait: carried on, 14 looks, both asked for" '[ "$out" = on ] && [ "$(cat $T/st/shows)" = 14 ] && grep -qx "power on" $T/calls && grep -qx "pairable on" $T/calls'
$T/fathom-bt stop >/dev/null 2>&1; echo 0 > $T/st/show-after

# 7. bluetoothd dies as it starts (no controller is ever shown, then); the chip won't
# start: said, put away, exit 1
: > $T/st/btd-dies; echo 99 > $T/st/show-after; begin
out=$($T/fathom-bt start 2>$T/st/err); rc=$?
check "start: bluetoothd gone: exit 1, said, everything stopped" '[ $rc = 1 ] && grep -q "bluetoothd didn.t stay up" $T/st/err && ! alive dbus && [ "$(tail -1 $T/st/chip)" = off ]'
rm -f $T/st/btd-dies; echo 0 > $T/st/show-after; : > $T/st/no-chip; begin
out=$($T/fathom-bt start 2>/dev/null); rc=$?
check "start: the chip won't start: tried twice, left off, exit 1" '[ $rc = 1 ] && [ "$(tr "\n" " " < $T/st/chip)" = "on on off " ]'
rm -f $T/st/no-chip

# 8. health: every daemon and the chip there: 0; something gone: 3, and which
health() { out=$($T/fathom-bt health 2>&1); rc=$?; }
health; check "health: never started (or stopped by hand): gone, \"stopped\"" '[ $rc = 3 ] && [ "$out" = stopped ]'
: > $T/hci0-bt; begin
$T/fathom-bt start >/dev/null 2>&1; sleep 0.2
c0=$(wc -l < $T/calls); b0=$(wc -l < $T/st/ba)
health; check "health: all of it running: 0, nothing said" '[ $rc = 0 ] && [ -z "$out" ]'
check "health: asks nothing of the daemons (no bluetoothctl, no bluealsa run)" '[ "$(wc -l < $T/calls)" -eq "$c0" ] && [ "$(wc -l < $T/st/ba)" -eq "$b0" ]'
kill $(cat $T/run/avrcp.pid); sleep 0.2
health; check "health: the AVRCP helper gone" '[ $rc = 3 ] && [ "$out" = avrcp ]'
old=$(cat $T/run/bluealsa.pid)
$T/fathom-bt avrcp; sleep 0.2
health; check "avrcp: the helper alone started again, the rest untouched" '[ $rc = 0 ] && alive avrcp && [ "$(cat $T/run/bluealsa.pid)" = "$old" ]'
a=$(cat $T/run/avrcp.pid); $T/fathom-bt avrcp
check "avrcp: one that's running isn't started twice" '[ "$(cat $T/run/avrcp.pid)" = "$a" ]'
kill $(cat $T/run/bluealsa.pid); sleep 0.2
health; check "health: bluealsa gone" '[ $rc = 3 ] && [ "$out" = bluealsa ]'
kill $(cat $T/run/dbus.pid); rm -f $T/hci0-bt; sleep 0.2
health; check "health: the bus, bluealsa and the chip gone: all named" '[ $rc = 3 ] && [ "$out" = "dbus bluealsa hci0" ]'
kill $(cat $T/run/bluetoothd.pid); sleep 0.2
health; check "health: bluetoothd gone too (its pid file still there): gone, not \"off\"" '[ $rc = 3 ] && [ "$out" = "dbus bluetoothd bluealsa hci0" ]'
# what the player does then: stop, start
$T/fathom-bt stop >/dev/null 2>&1; : > $T/hci0-bt; begin
out=$($T/fathom-bt start 2>/dev/null); sleep 0.2
health; check "stop and start after that: well again" '[ $rc = 0 ] && alive dbus bluetoothd bluealsa avrcp'
if [ -r /proc/$$/cmdline ]; then                   # (where a pid can be asked what it is)
    kill $(cat $T/run/bluealsa.pid); sleep 300 & X=$!; echo $X > $T/run/bluealsa.pid
    health; check "health: a pid that's some other program's by now counts as gone" '[ $rc = 3 ] && [ "$out" = bluealsa ]'
    kill $X
fi
# no helper on the card: none started, none missed
$T/fathom-bt stop >/dev/null 2>&1; mv $T/sd/bin/fathom-avrcp $T/sd/bin/fathom-avrcp.away; begin
$T/fathom-bt start >/dev/null 2>&1; sleep 0.2
health; check "health: no AVRCP helper on the card: not counted" '[ $rc = 0 ] && [ ! -f $T/run/avrcp.pid ]'
$T/fathom-bt stop >/dev/null 2>&1; mv $T/sd/bin/fathom-avrcp.away $T/sd/bin/fathom-avrcp
health; check "health: after stop: \"stopped\"" '[ $rc = 3 ] && [ "$out" = stopped ]'

# --- fathom-bluetooth: hci0 looked for ten times a second, no fixed wait after it ---
mkdir -p $T/drv
sed -e "s#^DRV=.*#DRV=$T/drv#" -e "s#^FW=.*#FW=$T/patch.hcd#" -e "s#^HCI=.*#HCI=$T/hci0#" -e "s#^HCIWAIT=[^ ]*#HCIWAIT=8#" \
    $SRC/usr/sbin/fathom-bluetooth > $T/fathom-bluetooth
chmod +x $T/fathom-bluetooth; : > $T/patch.hcd
rm -f $T/bin/fathom-bluetooth
( sleep 0.3; : > $T/hci0 ) &
out=$($T/fathom-bluetooth on); rc=$?
check "chip on: hci0 appears: on" '[ $rc = 0 ] && [ "$out" = "bluetooth: on (hci0)" ] && [ "$(cat $T/drv/bind)" = serial0-0 ]'
check "chip status" '[ "$($T/fathom-bluetooth status)" = on ]'
rm -f $T/hci0
out=$($T/fathom-bluetooth on); rc=$?
check "chip on: no hci0 in the wait: said, exit 1" '[ $rc = 1 ] && [ "$out" = "bluetooth: no hci0" ]'
rm -f $T/patch.hcd; : > $T/hci0
out=$($T/fathom-bluetooth on); rc=$?
check "chip on: no firmware patch: said, started all the same" '[ $rc = 0 ] && echo "$out" | grep -q "no firmware patch" && echo "$out" | grep -q "on (hci0)"'
rm -rf $T/drv
out=$($T/fathom-bluetooth on); rc=$?
check "chip on: no driver: said, exit 1" '[ $rc = 1 ] && [ "$out" = "bluetooth: no driver" ]'

echo "$ok passed, $bad failed"; [ $bad = 0 ]
