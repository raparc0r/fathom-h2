#!/bin/sh
# Off-device test of fathom-wifi's network choice, names, joining and DHCP handling, with a
# stand-in wpa_cli and udhcpc. In the build container: docker/run.sh sh tests/test_fathom_wifi.sh
# (elsewhere: SRC=<the tree's rootfs-overlay> sh tests/test_fathom_wifi.sh; GNU sed and touch)
set -u
T=$(mktemp -d); SRC=${SRC:-/work/buildroot/external/board/h2/rootfs-overlay}
mkdir -p $T/wifi/usr/sbin $T/run $T/ctrl $T/st $T/fw
sed -e "s#^W=.*#W=$T/wifi#" -e "s#^RUN=.*#RUN=$T/run#" -e "s#^CTRL=.*#CTRL=$T/ctrl#" \
    -e "s#^FW=.*#FW=$T/fw/brcmfmac43430-sdio.bin#" -e "s#^JOIN=.*#JOIN=6#" \
    -e "s#^UPTIME=.*#UPTIME=$T/uptime#" $SRC/usr/sbin/fathom-wifi > $T/fathom-wifi
sed -e "s#^RUN=.*#RUN=$T/run#" -e "s#^CARRIER=.*#CARRIER=$T/carrier#" -e "s#/usr/sbin/fathom-wifi#$T/fathom-wifi#" $SRC/usr/share/udhcpc/default.script.d/fathom-wifi-lease > $T/hook
chmod +x $T/fathom-wifi $T/hook
echo "1000.50 900.00" > $T/uptime
mkdir -p $T/bin; printf '#!/bin/sh\necho "$@" >> %s/st/log\n' $T > $T/bin/logger; chmod +x $T/bin/logger; export PATH=$T/bin:$PATH
# wpa_cli stand-in. st/nets: one network per line, tab-separated: id, name as wpa_cli
# prints it (escaped), flag (e enabled, d disabled, t temp-disabled), key_mgmt. The
# joined id in st/cur, the state in st/state. What a join does: st/join (ok, wrong,
# refuse, or nothing), played out over the status calls after select_network.
cat > $T/wifi/usr/sbin/wpa_cli <<X
#!/bin/sh
S=$T/st; shift 4; echo "\$*" >> \$S/calls
# (the value through the environment: awk -v would take its backslashes for escapes)
set_col() { X="\$3" awk -F'\t' -v OFS='\t' -v i="\$1" -v c="\$2" '\$1==i{\$c=ENVIRON["X"]}1' \$S/nets > \$S/n; mv \$S/n \$S/nets; }
case "\$1" in
status)
    if [ -f \$S/sel ]; then                 # a join under way
        n=\$(cat \$S/step); echo \$((n + 1)) > \$S/step; id=\$(cat \$S/sel)
        case "\$(cat \$S/join 2>/dev/null):\$n" in
        ok:*) echo COMPLETED > \$S/state; echo \$id > \$S/cur; rm \$S/sel ;;
        wrong:0) echo ASSOCIATING > \$S/state ;;
        wrong:1) echo 4WAY_HANDSHAKE > \$S/state ;;
        wrong:*) echo DISCONNECTED > \$S/state; set_col \$id 3 t; rm \$S/sel ;;
        refuse:*) echo SCANNING > \$S/state; set_col \$id 3 t; rm \$S/sel ;;
        esac
    fi
    echo "bssid=00:11:22:33:44:55"; echo "wpa_state=\$(cat \$S/state)"
    [ "\$(cat \$S/state)" = COMPLETED ] && { awk -F'\t' -v c=\$(cat \$S/cur) '\$1==c{print "ssid="\$2; print "id="\$1}' \$S/nets; echo "ip_address=\$(cat \$S/ip)"; }
    ;;
list_networks) echo "network id / ssid / bssid / flags"
    awk -F'\t' '{f=\$3=="d"?"[DISABLED]":\$3=="t"?"[TEMP-DISABLED]":""; print \$1"\t"\$2"\tany\t"f}' \$S/nets ;;
enable_network) if [ "\$2" = all ]; then awk -F'\t' -v OFS='\t' '{\$3="e"}1' \$S/nets > \$S/n; mv \$S/n \$S/nets; else set_col \$2 3 e; fi; echo OK ;;
select_network) awk -F'\t' -v OFS='\t' -v i=\$2 '{\$3=(\$1==i)?"e":"d"}1' \$S/nets > \$S/n; mv \$S/n \$S/nets
    echo \$2 > \$S/sel; echo 0 > \$S/step; echo OK ;;
add_network) n=\$(cat \$S/next 2>/dev/null || echo 10); echo \$((n + 1)) > \$S/next
    printf '%s\t\te\tWPA-PSK NONE\n' \$n >> \$S/nets; echo \$n ;;
remove_network) awk -F'\t' -v i=\$2 '\$1!=i' \$S/nets > \$S/n; mv \$S/n \$S/nets; echo OK ;;
set_network)
    case "\$3" in
    ssid)   # as printf_encode writes it: printable ASCII as it is (\\\\ and \\" escaped), the rest \\xHH
        e=\$(echo "\$4" | awk '{ p = " !\"#\$%&'"'"'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\\\]^_\`abcdefghijklmnopqrstuvwxyz{|}~"
            for (i = 1; i < length(\$0); i += 2) { b = index("0123456789abcdef", substr(\$0, i, 1)) * 16 + index("0123456789abcdef", substr(\$0, i + 1, 1)) - 17
                c = substr(p, b - 31, 1); if (b < 32 || b > 126) o = o sprintf("\\\\x%02x", b); else if (c == "\\\\" || c == "\"") o = o "\\\\" c; else o = o c }
            print o }')
        set_col \$2 2 "\$e" ;;
    key_mgmt) set_col \$2 4 "\$4" ;;
    psk) case "\$4" in \"????????*\") ;; *) echo FAIL; exit 0 ;; esac ;;
    esac
    echo OK ;;
save_config) { printf 'ctrl_interface=x\nupdate_config=1\n'; awk -F'\t' '{print "\nnetwork={\n\tssid=\""\$2"\"\n\tpsk=\"x\""; if(\$3=="d")print "\tdisabled=1"; print "}"}' \$S/nets; } > $T/wifi/networks.conf; echo OK ;;
get_network) [ "\$3" = key_mgmt ] && awk -F'\t' -v i=\$2 '\$1==i{print \$4}' \$S/nets || echo '*' ;;
scan_results) echo "bssid / frequency / signal level / flags / ssid"; cat \$S/scan ;;
signal_poll) echo RSSI=-50 ;;
*) echo OK ;;
esac
X
chmod +x $T/wifi/usr/sbin/wpa_cli
nets() { printf "$@" | tr '|' '\t' > $T/st/nets; }   # (| for a tab)
# a running wpa_supplicant (any process) and a udhcpc that records its signals
sleep 300 & echo $! > $T/run/wpa.pid
( trap 'echo USR2 >> $T/st/sig' USR2; trap 'echo USR1 >> $T/st/sig' USR1; while :; do sleep 0.1; done ) & U=$!; echo $U > $T/run/udhcpc.pid
sleep 0.3
ok=0; bad=0
check() { if eval "$2"; then echo "ok   $1"; ok=$((ok+1)); else echo "FAIL $1"; bad=$((bad+1)); fi; }

# 1. At home (0), joined; the hook records the network the address is from
nets '0|Home|e|WPA-PSK\n1|Phone|e|WPA-PSK\n'; echo 0 > $T/st/cur; echo COMPLETED > $T/st/state; echo 192.168.1.5 > $T/st/ip
interface=wlan0 $T/hook bound
check "hook records the lease's network" '[ "$(cat $T/run/lease-ssid)" = Home ]'
out=$($T/fathom-wifi status); check "status at home: connected" '[ "$out" = "connected|Home|192.168.1.5|-50" ]'
check "no DHCP signal on the same network" '[ ! -s $T/st/sig ]'

# 2. The user picks the hotspot: saved with every network enabled (no answer yet: kept)
: > $T/st/join
$T/fathom-wifi connect Phone >/dev/null
check "connect: the others disabled for now" 'grep -q "^0	Home	d" $T/st/nets'
check "connect: saved without disabled=1" '! grep -q disabled $T/wifi/networks.conf && grep -q Home $T/wifi/networks.conf'
rm -f $T/st/sel

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
echo SCANNING > $T/st/state; : > $T/st/calls
out=$($T/fathom-wifi status)
check "not joined: no signal, no process for it" '[ "$out" = "scanning|||" ] && [ "$(tr "\n" " " < $T/st/calls)" = "status list_networks " ]'
check "within the minute: Home still disabled" 'grep -q "^0	Home	d" $T/st/nets'
echo "1061.20 900.00" > $T/uptime
$T/fathom-wifi status >/dev/null
check "after a minute: every network enabled" '! grep -q "	d	" $T/st/nets'

# 4b. Nothing joined: a DHCP try that fails puts udhcpc to rest; status wakes it once joined
echo 0 > $T/carrier; : > $T/st/sig; : > $T/run/asked
interface=wlan0 $T/hook leasefail; sleep 0.3
check "hook: a failed try with nothing joined: udhcpc rests (released)" '[ "$(tr "\n" " " < $T/st/sig)" = "USR2 " ] && [ ! -f $T/run/asked ]'
echo 1 > $T/carrier; : > $T/st/sig
interface=wlan0 $T/hook leasefail; sleep 0.3
check "hook: a failed try while joined: udhcpc carries on" '[ ! -s $T/st/sig ]'
rm -f $T/carrier; interface=wlan0 $T/hook leasefail 2>$T/st/err; sleep 0.3
check "hook: no interface at all counts as not joined, quietly" '[ "$(tr "\n" " " < $T/st/sig)" = "USR2 " ] && [ ! -s $T/st/err ]'
echo COMPLETED > $T/st/state; : > $T/st/ip; : > $T/st/sig
out=$($T/fathom-wifi status); sleep 0.3
check "joined, no address: connecting, udhcpc woken" '[ "$out" = "connecting|Phone||-50" ] && [ "$(tr "\n" " " < $T/st/sig)" = "USR1 " ]'
$T/fathom-wifi status >/dev/null; sleep 0.3
check "not woken again at once" '[ "$(tr "\n" " " < $T/st/sig)" = "USR1 " ]'
echo "1071.20 900.00" > $T/uptime; $T/fathom-wifi status >/dev/null; sleep 0.3
check "woken again 10 s on, still no address" '[ "$(tr "\n" " " < $T/st/sig)" = "USR1 USR1 " ]'
echo 172.20.10.3 > $T/st/ip; : > $T/st/sig; echo "1090.00 900.00" > $T/uptime
out=$($T/fathom-wifi status); sleep 0.3
check "an address: connected, no more signals" '[ "$out" = "connected|Phone|172.20.10.3|-50" ] && [ ! -s $T/st/sig ]'

# 5. Forget saves cleanly too; a card saved by 0.8.4 loses its disabled=1 at start
awk -F'\t' -v OFS='\t' '{$3=($1==1)?"e":"d"}1' $T/st/nets > $T/st/n; mv $T/st/n $T/st/nets
$T/fathom-wifi forget Nothing; echo 1 > $T/st/cur; $T/wifi/usr/sbin/wpa_cli -p x -i y save_config >/dev/null
check "a plain save_config writes disabled=1 (the stand-in is honest)" 'grep -q disabled=1 $T/wifi/networks.conf'
CONF=$T/wifi/networks.conf; grep -q '^[[:space:]]*disabled=1$' $CONF && sed -i '/^[[:space:]]*disabled=1$/d' $CONF
check "start-up strip leaves the networks, drops disabled=1" '! grep -q disabled $CONF && grep -c "network={" $CONF | grep -qx 2'
interface=wlan0 $T/hook deconfig
check "hook: deconfig drops the record" '[ ! -f $T/run/lease-ssid ]'
interface=usb0 $T/hook bound
check "hook ignores other interfaces" '[ ! -f $T/run/lease-ssid ]'

# 6. Names beyond ASCII: shown, matched and added by their real bytes (wpa_cli escapes them)
echo 11 > $T/st/next; A='Anna’s iPhone'; AE='Anna\xe2\x80\x99s iPhone'; Q='Bob "5G" \ 100%'; QE='Bob \"5G\" \\ 100%'
nets '0|Home|e|WPA-PSK\n1|%s|e|WPA-PSK\n' "$AE"; echo 1 > $T/st/cur; echo COMPLETED > $T/st/state; echo 10.0.0.9 > $T/st/ip
printf 'aa:00:00:00:00:01\t2412\t-40\t[WPA2-PSK-CCMP][ESS]\t%s\naa:00:00:00:00:02\t2437\t-60\t[ESS]\t%s\naa:00:00:00:00:03\t2412\t-70\t[WPA2-PSK-CCMP][ESS]\t%s\naa:00:00:00:00:04\t2462\t-55\t[ESS]\t-dash\n' "$AE" "$QE" "$AE" > $T/st/scan
out=$($T/fathom-wifi scan 0)
check "scan: real names, best signal of each, saved marked" '[ "$out" = "$(printf "%s|-40|1|1\n-dash|-55|0|0\n%s|-60|0|0" "$A" "$Q")" ]'
out=$($T/fathom-wifi status); check "status: the real name" '[ "$out" = "connected|$A|10.0.0.9|-50" ]'
check "ssid (for the hook): the real name" '[ "$($T/fathom-wifi ssid)" = "$A" ]'
check "saved: the real names" '[ "$($T/fathom-wifi saved)" = "$(printf "Home\n%s" "$A")" ]'
echo ok > $T/st/join; : > $T/st/calls
$T/fathom-wifi connect "$A" >/dev/null
check "connect: a saved name with escapes is found, nothing added" '! grep -q add_network $T/st/calls && grep -q "select_network 1" $T/st/calls'
: > $T/st/calls
$T/fathom-wifi connect "$Q" >/dev/null; rc=$?
check "connect: a new one added by its real bytes, in hex" 'grep -qx "set_network 11 ssid 426f622022354722205c2031303025" $T/st/calls'
check "...joined and saved" '[ $rc = 0 ] && grep -q "^11	Bob \\\\\"5G\\\\\" \\\\\\\\ 100%	e" $T/st/nets && grep -q "Bob" $CONF'
$T/fathom-wifi forget "$A"
check "forget a name with escapes" '! grep -q Anna $T/st/nets && ! grep -q Anna $CONF'

# 7. Joining: a wrong password told and not kept; a refusal; a new password for a saved one
nets '0|Home|e|WPA-PSK\n'; echo 0 > $T/st/cur; echo COMPLETED > $T/st/state; echo 9 > $T/st/next
echo wrong > $T/st/join
$T/fathom-wifi connect Cafe wrongpass 2>/dev/null >/dev/null; rc=$?
check "wrong password: exit 4" '[ $rc = 4 ]'
check "...the new network not kept, the others back on" '! grep -q "^9	" $T/st/nets && grep -q "^0	Home	e" $T/st/nets && ! grep -q Cafe $CONF'
echo refuse > $T/st/join
$T/fathom-wifi connect Cafe goodpass1 2>/dev/null >/dev/null; rc=$?
check "refused before the key exchange: exit 5, not kept" '[ $rc = 5 ] && ! grep -q "Cafe" $T/st/nets'
$T/fathom-wifi connect Cafe short 2>/dev/null >/dev/null; rc=$?
check "a password too short: exit 3, nothing added" '[ $rc = 3 ] && ! grep -q "Cafe" $T/st/nets'
echo ok > $T/st/join
$T/fathom-wifi connect Cafe goodpass1 >/dev/null; rc=$?
check "joined: exit 0, saved" '[ $rc = 0 ] && grep -q "Cafe" $CONF'
old=$(awk -F'\t' '$2=="Cafe"{print $1}' $T/st/nets)
echo wrong > $T/st/join
$T/fathom-wifi connect Cafe mistyped 2>/dev/null >/dev/null; rc=$?
check "a wrong new password for a saved one: exit 4, the saved one stays" '[ $rc = 4 ] && [ "$(awk -F"\t" "\$2==\"Cafe\"{print \$1}" $T/st/nets)" = "$old" ]'
echo ok > $T/st/join
$T/fathom-wifi connect Cafe newpass12 >/dev/null
check "a right new password takes the saved one's place" '[ "$(grep -c "	Cafe	" $T/st/nets)" = 1 ] && ! grep -q "^$old	" $T/st/nets'
echo wrong > $T/st/join
$T/fathom-wifi connect Cafe 2>/dev/null >/dev/null; rc=$?
check "a saved one whose password went wrong: exit 4, forgotten (asked again next time)" '[ $rc = 4 ] && ! grep -q Cafe $T/st/nets && ! grep -q Cafe $CONF'

# 8. The chip's settings for this board: the stock ones and Bluetooth coexistence
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
