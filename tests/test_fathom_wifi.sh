#!/bin/sh
# Off-device test of fathom-wifi's network choice and DHCP handling, with a stand-in wpa_cli and
# udhcpc. In the build container: docker/run.sh sh tests/test_fathom_wifi.sh
set -u
T=$(mktemp -d); SRC=/work/buildroot/external/board/h2/rootfs-overlay
mkdir -p $T/wifi/usr/sbin $T/run $T/ctrl $T/st $T/fw
sed -e "s#^W=.*#W=$T/wifi#" -e "s#^RUN=.*#RUN=$T/run#" -e "s#^CTRL=.*#CTRL=$T/ctrl#" \
    -e "s#^FW=.*#FW=$T/fw/brcmfmac43430-sdio.bin#" $SRC/usr/sbin/fathom-wifi > $T/fathom-wifi
sed -e "s#^RUN=.*#RUN=$T/run#" -e "s#/usr/sbin/fathom-wifi#$T/fathom-wifi#" $SRC/usr/share/udhcpc/default.script.d/fathom-wifi-lease > $T/hook
chmod +x $T/fathom-wifi $T/hook
mkdir -p $T/bin; printf '#!/bin/sh\necho "$@" >> %s/st/log\n' $T > $T/bin/logger; chmod +x $T/bin/logger; export PATH=$T/bin:$PATH
# wpa_cli stand-in: networks in st/nets (id ssid flag), joined id in st/cur, state in st/state
cat > $T/wifi/usr/sbin/wpa_cli <<X
#!/bin/sh
S=$T/st; shift 4; echo "\$*" >> \$S/calls
case "\$1" in
status) echo "wpa_state=\$(cat \$S/state)"; [ "\$(cat \$S/state)" = COMPLETED ] && { awk -v c=\$(cat \$S/cur) '\$1==c{print "ssid="\$2}' \$S/nets; echo "ip_address=\$(cat \$S/ip)"; } ;;
list_networks) echo "network id / ssid / bssid / flags"; awk '{f=\$3=="d"?"[DISABLED]":""; print \$1"\t"\$2"\tany\t"f}' \$S/nets ;;
enable_network) if [ "\$2" = all ]; then awk '{print \$1, \$2, "e"}' \$S/nets > \$S/n; else awk -v i=\$2 '\$1==i{\$3="e"}1' \$S/nets > \$S/n; fi; mv \$S/n \$S/nets; echo OK ;;
select_network) awk -v i=\$2 '{\$3=(\$1==i)?"e":"d"}1' \$S/nets > \$S/n; mv \$S/n \$S/nets; echo OK ;;
save_config) { printf 'ctrl_interface=x\nupdate_config=1\n'; awk '{print "\nnetwork={\n\tssid=\""\$2"\"\n\tpsk=\"x\""; if(\$3=="d")print "\tdisabled=1"; print "}"}' \$S/nets; } > $T/wifi/networks.conf; echo OK ;;
get_network) echo '*' ;;
signal_poll) echo RSSI=-50 ;;
*) echo OK ;;
esac
X
chmod +x $T/wifi/usr/sbin/wpa_cli
# a running wpa_supplicant (any process) and a udhcpc that records its signals
sleep 300 & echo $! > $T/run/wpa.pid
( trap 'echo USR2 >> $T/st/sig' USR2; trap 'echo USR1 >> $T/st/sig' USR1; while :; do sleep 0.1; done ) & U=$!; echo $U > $T/run/udhcpc.pid
sleep 0.3
ok=0; bad=0
check() { if eval "$2"; then echo "ok   $1"; ok=$((ok+1)); else echo "FAIL $1"; bad=$((bad+1)); fi; }

# 1. At home (0), joined; the hook records the network the address is from
printf '0 Home e\n1 Phone e\n' > $T/st/nets; echo 0 > $T/st/cur; echo COMPLETED > $T/st/state; echo 192.168.1.5 > $T/st/ip
interface=wlan0 $T/hook bound
check "hook records the lease's network" '[ "$(cat $T/run/lease-ssid)" = Home ]'
out=$($T/fathom-wifi status); check "status at home: connected" '[ "$out" = "connected|Home|192.168.1.5|-50" ]'
check "no DHCP signal on the same network" '[ ! -s $T/st/sig ]'

# 2. The user picks the hotspot: saved with every network enabled
$T/fathom-wifi connect Phone >/dev/null
check "connect: the others disabled for now" 'grep -q "^0 Home d" $T/st/nets'
check "connect: saved without disabled=1" '! grep -q disabled $T/wifi/networks.conf && grep -q Home $T/wifi/networks.conf'

# 3. wpa_supplicant joins the hotspot; the address is still the home network's
echo 1 > $T/st/cur
out=$($T/fathom-wifi status); sleep 0.5
check "status after the switch: connecting, no stale address" '[ "$out" = "connecting|Phone||-50" ]'
cat $T/st/sig; check "udhcpc released, then asked afresh" '[ "$(tr "\n" " " < $T/st/sig)" = "USR2 USR1 " ]'
check "lease record dropped" '[ ! -f $T/run/lease-ssid ]'
: > $T/st/sig; echo 172.20.10.3 > $T/st/ip
interface=wlan0 $T/hook bound
out=$($T/fathom-wifi status); sleep 0.3
check "then connected with the new address, no more signals" '[ "$out" = "connected|Phone|172.20.10.3|-50" ] && [ ! -s $T/st/sig ]'

# 4. The picked network goes away: others re-enabled only after a minute
echo SCANNING > $T/st/state
$T/fathom-wifi status >/dev/null
check "within the minute: Home still disabled" 'grep -q "^0 Home d" $T/st/nets'
echo $(( $(cut -d. -f1 /proc/uptime) - 61 )) > $T/run/seen
$T/fathom-wifi status >/dev/null
check "after a minute: every network enabled" '! grep -q " d$" $T/st/nets'

# 5. Forget saves cleanly too; a card saved by 0.8.4 loses its disabled=1 at start
$T/fathom-wifi select_dummy 2>/dev/null; awk '{$3=($1==1)?"e":"d"}1' $T/st/nets > $T/st/n; mv $T/st/n $T/st/nets
$T/fathom-wifi forget Nothing; echo 1 > $T/st/cur; $T/wifi/usr/sbin/wpa_cli -p x -i y save_config >/dev/null
check "a plain save_config writes disabled=1 (the stand-in is honest)" 'grep -q disabled=1 $T/wifi/networks.conf'
CONF=$T/wifi/networks.conf; grep -q '^[[:space:]]*disabled=1$' $CONF && sed -i '/^[[:space:]]*disabled=1$/d' $CONF
check "start-up strip leaves the networks, drops disabled=1" '! grep -q disabled $CONF && grep -c "network={" $CONF | grep -qx 2'
interface=wlan0 $T/hook deconfig
check "hook: deconfig drops the record" '[ ! -f $T/run/lease-ssid ]'
interface=usb0 $T/hook bound
check "hook ignores other interfaces" '[ ! -f $T/run/lease-ssid ]'

# 7. The chip's settings for this board: the stock ones and Bluetooth coexistence
NV=$T/fw/brcmfmac43430-sdio.txt; NVB=$T/fw/brcmfmac43430-sdio.hifiwalker,h2.txt
printf 'boardflags=0x00404201\nmuxenab=0x10\n' > $NV; touch -d '2019-01-02' $NV
$T/fathom-wifi nvram
check "nvram: the board's file made, stock lines first" '[ "$(head -2 $NVB)" = "$(cat $NV)" ]'
check "nvram: with the coexistence lines" 'grep -qx btc_mode=1 $NVB && grep -qx btc_params8=0x4e20 $NVB && grep -qx btc_params1=0x7530 $NVB'
check "nvram: the stock file untouched" '[ "$(cat $NV)" = "$(printf "boardflags=0x00404201\nmuxenab=0x10")" ]'
touch -d '2020-01-01' $NVB; $T/fathom-wifi nvram
check "nvram: not written again when it's up to date" '[ "$(date -r $NVB +%Y)" = 2020 ]'
printf 'boardflags=0x00404201\nmuxenab=0x10\nnew=1\n' > $NV; $T/fathom-wifi nvram
check "nvram: made again from a newer stock file" 'grep -qx new=1 $NVB && grep -qx btc_mode=1 $NVB'
printf 'muxenab=0x10\nbtc_mode=4\n' > $NV; $T/fathom-wifi nvram
check "nvram: a stock file with its own coexistence left as it is" '[ ! -f $NVB ]'
rm -f $NV; $T/fathom-wifi nvram
check "nvram: no stock file, nothing made" '[ ! -f $NVB ]'

kill $U $(cat $T/run/wpa.pid) 2>/dev/null
echo "$ok passed, $bad failed"; [ $bad = 0 ]
