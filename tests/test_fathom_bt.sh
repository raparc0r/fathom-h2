#!/bin/sh
# Off-device test of fathom-bt's device lists (scan, paired), with a stand-in bluetoothctl.
# In the build container: docker/run.sh sh tests/test_fathom_bt.sh
# (elsewhere: SRC=<the tree's rootfs-overlay> sh tests/test_fathom_bt.sh)
set -u
T=$(mktemp -d); SRC=${SRC:-/work/buildroot/external/board/h2/rootfs-overlay}
mkdir -p $T/bt/usr/bin $T/run
sed -e "s#^BT=.*#BT=$T/bt#" -e "s#^RUN=.*#RUN=$T/run#" $SRC/usr/sbin/fathom-bt > $T/fathom-bt
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
info() {   # as bluetoothctl prints it; \$2 what each line starts with
    awk -F';' -v a="\$1" -v p="\$2" '\$1 == a { print p "Device " a " (public)"; print p "\tName: " \$2; print p "\tAlias: " \$2
        if (\$3 != "") print p "\tIcon: " \$3; print p "\tPaired: " \$4; print p "\tConnected: " \$5
        if (\$6 != "") print p "\tUUID: " (\$6 ~ /^0000110b/ ? "Audio Sink  " : "Audio Source") "              (" \$6 ")" }' \$D
}
case "\$1" in
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

kill $(cat $T/run/bluetoothd.pid) 2>/dev/null
echo "$ok passed, $bad failed"; [ $bad = 0 ]
