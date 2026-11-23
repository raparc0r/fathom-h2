#!/bin/sh
# Off-device test of fathom-wifi's network choice, names, joining and DHCP handling, with a
# stand-in wpa_cli and udhcpc.
#   sh tests/test_fathom_wifi.sh       (in the build container too: docker/run.sh sh tests/test_fathom_wifi.sh)
set -u
here=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d); SRC=${SRC:-$here/buildroot/external/board/h2/rootfs-overlay}
mkdir -p $T/wifi/usr/sbin $T/run $T/ctrl $T/st $T/fw
# (nothing of the machine it runs on is touched: its modules, its MMC hosts, its /var/run and resolv.conf)
sed -e "s#^W=.*#W=$T/wifi#" -e "s#^RUN=.*#RUN=$T/run#" -e "s#^CTRL=.*#CTRL=$T/ctrl#" \
    -e "s#^FW=.*#FW=$T/fw/brcmfmac43430-sdio.bin#" -e "s#^JOIN=.*#JOIN=6#" \
    -e "s#^UPTIME=.*#UPTIME=$T/uptime#" -e "s#^LOCK=[^ ]*#LOCK=$T/run/lock#" -e "s#^NTPLOCK=[^ ]*#NTPLOCK=$T/run/ntp.lock#" \
    -e "s#^MMC=.*#MMC=$T/mmc#" -e "s#/proc/modules#$T/modules#g" -e "s#^NET=[^ ]*#NET=$T/net/wlan0#" -e "s#^VM=.*#VM=$T/vm#" \
    -e "s#^BUDDY=.*#BUDDY=$T/buddyinfo#" -e "s#^CHIPWAIT=[^ ]*#CHIPWAIT=30#" -e "s#/tmp/fathom-lat#$T/lat#" \
    $SRC/usr/sbin/fathom-wifi > $T/fathom-wifi
sed -e "s#^RUN=.*#RUN=$T/run#" -e "s#^CARRIER=.*#CARRIER=$T/carrier#" -e "s#^RESOLV=.*#RESOLV=$T/resolv.conf#" \
    -e "s#/usr/sbin/fathom-wifi#$T/fathom-wifi#" $SRC/usr/share/udhcpc/default.script.d/fathom-wifi-lease > $T/hook
chmod +x $T/fathom-wifi $T/hook
: > $T/modules
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

# 2. The user picks the hotspot: the others off only while it's being joined; no answer
# in the wait: kept, every network on again at once, and saved with none disabled
: > $T/st/join; : > $T/st/calls
$T/fathom-wifi connect Phone >/dev/null 2>&1; rc=$?
check "connect: the others disabled while it joins" 'grep -qx "select_network 1" $T/st/calls'
check "connect: no answer in the wait: exit 6, every network on again" '[ $rc = 6 ] && ! grep -q "	d	" $T/st/nets'
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

# 4. The picked network goes away (the others still off, as select_network left them):
# re-enabled only after a minute
awk -F'\t' -v OFS='\t' '{$3=($1==1)?"e":"d"}1' $T/st/nets > $T/st/n; mv $T/st/n $T/st/nets
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
CONF=$T/wifi/networks.conf; grep -q '^[[:space:]]*disabled=1$' $CONF && sed '/^[[:space:]]*disabled=1$/d' $CONF > $CONF.new && mv $CONF.new $CONF
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

# 7b. A join wakes DHCP itself, not left to the player's next status: no address yet: asked;
# an address that was another network's: let go, then asked; this network's own: left alone
nets '0|Home|e|WPA-PSK\n1|Phone|e|WPA-PSK\n'; echo 0 > $T/st/cur; echo COMPLETED > $T/st/state
rm -f $T/run/lease-ssid $T/run/asked; : > $T/st/ip; : > $T/st/sig; echo ok > $T/st/join
$T/fathom-wifi connect Phone >/dev/null; sleep 0.3
check "connect: joined with no address: udhcpc woken at the join" '[ "$(tr "\n" " " < $T/st/sig)" = "USR1 " ]'
out=$($T/fathom-wifi status); sleep 0.3
check "...and not again by the status that follows" '[ "$out" = "connecting|Phone||-50" ] && [ "$(tr "\n" " " < $T/st/sig)" = "USR1 " ]'
echo 172.20.10.3 > $T/st/ip; interface=wlan0 $T/hook bound; : > $T/st/sig; echo "2000.00 900.00" > $T/uptime
check "(the lease is the hotspot's)" '[ "$(cat $T/run/lease-ssid)" = Phone ]'
$T/fathom-wifi connect Home >/dev/null; sleep 0.5
check "connect: joined with another network's address: released, then asked afresh" '[ "$(tr "\n" " " < $T/st/sig)" = "USR2 USR1 " ] && [ ! -f $T/run/lease-ssid ]'
echo 192.168.1.5 > $T/st/ip; interface=wlan0 $T/hook bound; : > $T/st/sig; echo "2100.00 900.00" > $T/uptime
$T/fathom-wifi connect Home >/dev/null; sleep 0.3
check "connect: the network its address is already from: udhcpc left alone" '[ ! -s $T/st/sig ]'
check "connect: the join noted (status's minute without a connection counts from it)" '[ "$(cat $T/run/seen)" = 2100 ]'
echo refuse > $T/st/join; : > $T/st/sig; rm -f $T/run/asked
$T/fathom-wifi connect Phone 2>/dev/null >/dev/null; sleep 0.3
check "connect: not joined: udhcpc not touched" '[ ! -s $T/st/sig ]'

# 8. The chip's settings for this board: the stock ones and Bluetooth coexistence
NV=$T/fw/brcmfmac43430-sdio.txt; NVB=$T/fw/brcmfmac43430-sdio.hifiwalker,h2.txt
printf 'boardflags=0x00404201\nmuxenab=0x10\n' > $NV; touch -t 201901020000 $NV
$T/fathom-wifi nvram
check "nvram: the board's file made, stock lines first" '[ "$(head -2 $NVB)" = "$(cat $NV)" ]'
check "nvram: with the coexistence lines" 'grep -qx btc_mode=1 $NVB && grep -qx btc_params8=0x4e20 $NVB && grep -qx btc_params1=0x7530 $NVB'
check "nvram: the stock file untouched" '[ "$(cat $NV)" = "$(printf "boardflags=0x00404201\nmuxenab=0x10")" ]'
touch -t 202001010000 $NVB; touch -t 202001010001 $T/after; $T/fathom-wifi nvram
check "nvram: not written again when it's up to date" '[ $T/after -nt $NVB ]'
printf 'boardflags=0x00404201\nmuxenab=0x10\nnew=1\n' > $NV; $T/fathom-wifi nvram
check "nvram: made again from a newer stock file" 'grep -qx new=1 $NVB && grep -qx btc_mode=1 $NVB'
printf 'muxenab=0x10\nbtc_mode=4\n' > $NV; $T/fathom-wifi nvram
check "nvram: a stock file with its own coexistence left as it is" '[ ! -f $NVB ]'
rm -f $NV; $T/fathom-wifi nvram
check "nvram: no stock file, nothing made" '[ ! -f $NVB ]'

# 9. Name lookups: the hook puts its options line in resolv.conf once, beside what udhcpc's
# own script writes there (which rewrites only its "# wlan0" lines)
printf 'nameserver 192.168.1.1 # wlan0\n' > $T/resolv.conf
interface=wlan0 $T/hook bound
check "hook: bound adds the lookup options, the name server kept" '[ "$(cat $T/resolv.conf)" = "$(printf "nameserver 192.168.1.1 # wlan0\noptions timeout:3 attempts:3")" ]'
interface=wlan0 $T/hook renew; interface=wlan0 $T/hook bound
check "hook: once only, whatever comes after" '[ "$(grep -c "^options" $T/resolv.conf)" = 1 ] && [ "$(wc -l < $T/resolv.conf)" -eq 2 ]'
interface=usb0 $T/hook bound; interface=wlan0 $T/hook deconfig; interface=wlan0 $T/hook leasefail
check "hook: nothing else touches it" '[ "$(wc -l < $T/resolv.conf)" -eq 2 ]'
rm -f $T/resolv.conf; interface=wlan0 $T/hook bound 2>$T/st/err
check "hook: a resolv.conf not there yet is made, quietly" '[ "$(cat $T/resolv.conf)" = "options timeout:3 attempts:3" ] && [ ! -s $T/st/err ]'

# 10. One run at a time of what changes Wi-Fi: a second waits for the first; a lock whose
# owner is gone is cleared away; one held too long isn't waited for; none is left behind
L=$T/run/lock
check "no lock left by any run above (joined, refused, wrong password, forgotten)" '[ ! -e $L ]'
# (a run under way: any process with fathom-wifi in its command line)
printf '#!/bin/sh\nwhile :; do sleep 0.1; done\n' > $T/bin/fathom-wifi-hold; chmod +x $T/bin/fathom-wifi-hold
$T/bin/fathom-wifi-hold & H=$!
mkdir $L; echo $H > $L/pid; : > $T/st/calls
$T/fathom-wifi forget Nothing & F=$!
sleep 1
check "a second run waits while the first holds the lock" 'kill -0 $F 2>/dev/null && [ ! -s $T/st/calls ]'
out=$($T/fathom-wifi status)
check "...status doesn't (it changes nothing)" '[ -n "$out" ]'
: > $T/st/calls; kill $H; wait $F
check "...and goes ahead once the first is gone" 'grep -q list_networks $T/st/calls'
check "...leaving no lock" '[ ! -e $L ]'
mkdir $L; echo $H > $L/pid; : > $T/st/calls
$T/fathom-wifi forget Nothing
check "a lock whose owner is gone: cleared, the run made, no lock left" 'grep -q list_networks $T/st/calls && [ ! -e $L ]'
mkdir $L; : > $T/st/calls
$T/fathom-wifi forget Nothing
check "a lock nobody wrote a pid in: the same" 'grep -q list_networks $T/st/calls && [ ! -e $L ]'
if [ -r /proc/$$/cmdline ]; then                   # (where a pid can be asked what it is)
    sleep 300 & X=$!
    mkdir $L; echo $X > $L/pid; : > $T/st/calls
    $T/fathom-wifi forget Nothing
    check "a lock whose pid is some other program's by now: the same" 'grep -q list_networks $T/st/calls && [ ! -e $L ]'
    kill $X
fi
: > $T/st/join
$T/fathom-wifi connect Home >/dev/null 2>&1 & F=$!
sleep 1.5
check "the lock is its own for the whole of a run (the subshells it starts don't give it up)" '[ "$(cat $L/pid 2>/dev/null)" = $F ]'
wait $F
check "...and gone at its end" '[ ! -e $L ]'
sed "s#^LOCKWAIT=.*#LOCKWAIT=5#" $T/fathom-wifi > $T/fathom-wifi-short; chmod +x $T/fathom-wifi-short
$T/bin/fathom-wifi-hold & H=$!
mkdir $L; echo $H > $L/pid; : > $T/st/calls; : > $T/st/log
$T/fathom-wifi-short forget Nothing
check "a lock still held when the wait is over: run without it, and said" 'grep -q list_networks $T/st/calls && grep -q "not waited for" $T/st/log'
check "...the other run's lock left to it" '[ "$(cat $L/pid 2>/dev/null)" = $H ]'
kill $H; rm -rf $L
sed "s#^LOCK=[^ ]*#LOCK=$T/nowhere/lock#" $T/fathom-wifi > $T/fathom-wifi-nolock; chmod +x $T/fathom-wifi-nolock
: > $T/st/calls
$T/fathom-wifi-nolock forget Nothing
check "a lock that can't be made at all: run without it, as before" 'grep -q list_networks $T/st/calls'
echo wrong > $T/st/join
$T/fathom-wifi connect Cafe wrongpass 2>/dev/null >/dev/null
check "an exit part-way (wrong password) leaves no lock either" '[ ! -e $L ]'
check "on, already running: answered at once" '[ "$($T/fathom-wifi on)" = on ] && [ ! -e $L ]'

# 11. off: the clock hook's sleeper (fathom-ntp's loop) ended with it; a pid in its lock
# that isn't it any more left alone. (Last: off ends the stand-ins too.)
mkdir -p $T/run/ntp.lock
if [ -r /proc/$$/cmdline ]; then
    sleep 300 & X=$!
    echo $X > $T/run/ntp.lock/pid
    $T/fathom-wifi off >/dev/null 2>&1
    check "off: a pid in fathom-ntp's lock that isn't fathom-ntp is left alone" 'kill -0 $X 2>/dev/null'
    kill $X
fi
printf '#!/bin/sh\nwhile :; do sleep 0.1; done\n' > $T/bin/fathom-ntp; chmod +x $T/bin/fathom-ntp
$T/bin/fathom-ntp loop & N=$!
echo $N > $T/run/ntp.lock/pid
$T/fathom-wifi off >/dev/null 2>&1; sleep 0.3
check "off: fathom-ntp's sleeper ended" '! kill -0 $N 2>/dev/null'
check "off: no lock left" '[ ! -e $L ]'
check "off: says so, and status agrees" '[ "$($T/fathom-wifi off 2>/dev/null)" = off ] && [ "$($T/fathom-wifi status)" = "off|||" ]'

# 12. on: the chip's start, with stand-ins for the kernel's side (insmod, rmmod, dmesg, the
# MMC host's bind and unbind, wlan0 appearing, /proc/buddyinfo) and for wpa_supplicant and udhcpc
mkdir -p $T/net $T/vm $T/mmc; : > $T/fw/brcmfmac43430-sdio.bin
printf '#!/bin/sh\necho "insmod $(basename $1)" >> %s/st/sys\necho "$(basename $1 .ko | tr - _) 16384 0" >> %s/modules\n' $T $T > $T/bin/insmod
printf '#!/bin/sh\necho "rmmod $1" >> %s/st/sys\ngrep -v "^$1 " %s/modules > %s/modules.n; mv %s/modules.n %s/modules\n' $T $T $T $T $T > $T/bin/rmmod
printf '#!/bin/sh\ncat %s/st/dmesg 2>/dev/null\n' $T > $T/bin/dmesg
printf '#!/bin/sh\necho sync >> %s/st/sys\n' $T > $T/bin/sync
printf '#!/bin/sh\necho "ifconfig $*" >> %s/st/sys\n' $T > $T/bin/ifconfig
cat > $T/bin/udhcpc <<X
#!/bin/sh
while [ \$# -gt 0 ]; do [ "\$1" = -p ] && echo \$\$ > "\$2"; shift; done
while :; do sleep 0.1; done
X
# (its control socket made too, where there's a python3 to make one: else "on" waits its 4 s for it)
cat > $T/wifi/usr/sbin/wpa_supplicant <<X
#!/bin/sh
while [ \$# -gt 0 ]; do [ "\$1" = -P ] && P="\$2"; shift; done
( while :; do sleep 0.1; done ) > /dev/null 2>&1 &
echo \$! > "\$P"
python3 -c 'import socket, sys; socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM).bind(sys.argv[1])' $T/ctrl/wlan0 2>/dev/null
exit 0
X
chmod +x $T/bin/insmod $T/bin/rmmod $T/bin/dmesg $T/bin/sync $T/bin/ifconfig $T/bin/udhcpc $T/wifi/usr/sbin/wpa_supplicant
ROOMY='Node 0, zone   Normal      5     10      8      6      4      2      1      1      0      0      0'   # 20 pieces of 64 KB
TIGHT='Node 0, zone   Normal    500    300    100     20      3      1      0      0      0      0      0'   # 5
# off (whatever ran before), then a clean slate: wlan0 appears $1 seconds after
fresh() {
    $T/fathom-wifi off >/dev/null 2>&1
    rm -rf $T/net/wlan0 $T/mmc/bind $T/mmc/unbind $T/lat; : > $T/st/sys; : > $T/st/dmesg; : > $T/st/log; : > $T/vm/drop_caches; : > $T/vm/compact_memory
    [ -n "${1:-}" ] && ( sleep $1; : > $T/net/wlan0 ) &
}
echo "$ROOMY" > $T/buddyinfo; fresh 0.7; : > $T/lat
out=$($T/fathom-wifi on 2>$T/st/err); rc=$?; sleep 0.3
check "on: started, nothing said on stderr" '[ "$out" = on ] && [ $rc = 0 ] && [ ! -s $T/st/err ]'
check "on: the four modules loaded, in order" '[ "$(grep insmod $T/st/sys | tr "\n" " ")" = "insmod cfg80211.ko insmod brcmutil.ko insmod brcmfmac.ko insmod brcmfmac-wcc.ko " ]'
check "on: room in memory: nothing synced, dropped or compacted" '! grep -q sync $T/st/sys && [ ! -s $T/vm/drop_caches ] && [ ! -s $T/vm/compact_memory ]'
check "on: the chip powered once, never unpowered" '[ "$(cat $T/mmc/bind)" = 13460000.mmc ] && [ ! -e $T/mmc/unbind ]'
check "on: wpa_supplicant and udhcpc running, the interface up" 'kill -0 $(cat $T/run/wpa.pid) && kill -0 $(cat $T/run/udhcpc.pid) && grep -q "ifconfig wlan0 up" $T/st/sys'
check "on: no lock left; status answers" '[ ! -e $L ] && [ "$($T/fathom-wifi status)" != "off|||" ]'
check "on: with the timing switch, each step in the log, stamped in ms" 'grep -q "lat 2100000: modules loaded" $T/st/log && grep -q "lat 2100000: try 1: room in memory (20 free" $T/st/log && grep -q "lat 2100000: wlan0 (try 1)" $T/st/log && grep -q "lat 2100000: wpa_supplicant started" $T/st/log && grep -q "lat 2100000: its control socket" $T/st/log'
: > $T/st/sys; $T/fathom-wifi off >/dev/null 2>&1
check "off: the driver's modules unloaded, the last loaded first" '[ "$(grep rmmod $T/st/sys | tr "\n" " ")" = "rmmod brcmfmac_wcc rmmod brcmfmac rmmod brcmutil rmmod cfg80211 " ]'
echo "$TIGHT" > $T/buddyinfo; fresh 0.3
out=$($T/fathom-wifi on 2>$T/st/err); sleep 0.3
check "on: memory in small pieces: synced, caches dropped, compacted, as before" '[ "$out" = on ] && grep -q sync $T/st/sys && [ "$(cat $T/vm/drop_caches)" = 3 ] && [ "$(cat $T/vm/compact_memory)" = 1 ]'
check "on: without the timing switch, nothing in the log" '! grep -q "lat " $T/st/log'
for b in "garbage" "Node 0, zone   Normal      5     10      8      6      x      2      1      1      0      0      0" ""; do
    if [ -n "$b" ]; then echo "$b" > $T/buddyinfo; else rm -f $T/buddyinfo; fi
    fresh 0.2
    out=$($T/fathom-wifi on 2>$T/st/err); sleep 0.3
    check "on: memory's state unreadable (${b:-no file}): tidied, as before" '[ "$out" = on ] && [ "$(cat $T/vm/drop_caches)" = 3 ] && [ ! -s $T/st/err ]'
done
# a zone of its own for each line: counted together
printf 'Node 0, zone      DMA      1      1      1      1      1      1      1      0      0      0      0\nNode 0, zone   Normal      1      1      1      1      1      0      0      1      0      0      0\n' > $T/buddyinfo   # 7 + 9
fresh 0.2
out=$($T/fathom-wifi on 2>$T/st/err); sleep 0.3
check "on: two zones' free blocks counted together (16: room)" '[ "$out" = on ] && [ ! -s $T/vm/drop_caches ]'
# room, but the driver says the download failed all the same: unpowered, tidied, a second try
echo "$ROOMY" > $T/buddyinfo; fresh; echo "an old line: download failed" > $T/st/dmesg
( sleep 0.3; echo "brcmfmac: brcmf_sdio_download_firmware: dongle image file download failed" >> $T/st/dmesg; sleep 1.2; : > $T/net/wlan0 ) &
out=$($T/fathom-wifi on 2>$T/st/err); sleep 0.3
check "on: a first try that fails after all: chip unpowered, memory tidied, the second try starts it" '[ "$out" = on ] && [ "$(cat $T/mmc/unbind 2>/dev/null)" = 13460000.mmc ] && grep -q sync $T/st/sys && [ "$(cat $T/vm/drop_caches)" = 3 ]'
# the chip never comes up, and the driver says nothing: given up (3 s here), nothing unbound
fresh
out=$($T/fathom-wifi on 2>$T/st/err); rc=$?
check "on: the chip never starts: exit 1, said as the player expects it, not unpowered" '[ $rc = 1 ] && grep -q "the chip didn.t start in 45 s" $T/st/err && [ ! -e $T/mmc/unbind ]'
check "...and no lock left" '[ ! -e $L ]'
$T/fathom-wifi off >/dev/null 2>&1

kill $U $(cat $T/run/wpa.pid 2>/dev/null) $(cat $T/run/udhcpc.pid 2>/dev/null) 2>/dev/null
echo "$ok passed, $bad failed"; [ $bad = 0 ]
