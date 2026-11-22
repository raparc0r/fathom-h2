#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional terms under GPLv3 section 7: see LICENSE.
#
# Compare backlight PWM frequencies by eye on the H2, without a new kernel.
# Runs on the H2 (over SSH), next to bl-probe:
#
#     sh bl-compare.sh list              the settings
#     sh bl-compare.sh 10k [SCALE]       switch to one (SCALE: duty x SCALE, default 1)
#     sh bl-compare.sh tour [SECONDS]    each setting in turn (default 20 s), then back
#     sh bl-compare.sh status            what's running, and the registers
#     sh bl-compare.sh off               back to the kernel's own setting (20 us, 50 kHz)
#     sh bl-compare.sh pmic [SECONDS]    log the PMIC (charger, power path, VBUS/VSYS/VBAT)
#                                        every second, detached, to pmic-TIME.txt here:
#                                        unplug USB meanwhile to compare battery and USB
#
# A setting follows the player's brightness (Menu + wheel, dimming, screen off),
# keeping its own period: the duty fraction stays the player's, so at a lower
# frequency the same level may look brighter (or a level that is dark at
# 33 kHz may light). A reboot or "off" ends it; nothing is saved.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
probe=$here/bl-probe
pidf=/tmp/bl-follow.pid
logf=/tmp/bl-follow.log

# name period_ns source what
settings() {
    cat <<'EOF'
33k   30000   pclk  33.3 kHz from PCLK (Fathom's kernel before 50 kHz)
stock 30000   ext   the stock firmware's own: 33.3 kHz from the 24 MHz clock
50k   20000   pclk  now: Fathom's kernel (50 kHz); a control, should look the same as off
25k   40000   pclk  25 kHz
20k   50000   pclk  20 kHz (the lowest above hearing)
10k   100000  pclk  10 kHz (may whine)
5k    200000  pclk  5 kHz (may whine)
2k    500000  pclk  2 kHz (may whine)
1k    1000000 pclk  1 kHz (may whine)
EOF
}

stop_follow() {
    if [ -f $pidf ]; then
        pid=$(cat $pidf)
        if kill "$pid" 2>/dev/null; then
            i=0
            while kill -0 "$pid" 2>/dev/null && [ $i -lt 20 ]; do usleep 100000; i=$((i + 1)); done
        fi
        rm -f $pidf
    fi
}

start() {
    line=$(settings | awk -v n="$1" '$1 == n')
    [ -n "$line" ] || { echo "no setting $1 (sh $0 list)" >&2; exit 1; }
    scale=$2
    set -- $line
    name=$1 period=$2 src=$3
    shift 3
    stop_follow
    cd /
    setsid "$probe" follow "$period" "$src" "$scale" > $logf 2>&1 < /dev/null &
    echo $! > $pidf
    usleep 300000
    kill -0 "$(cat $pidf)" 2>/dev/null || { cat $logf; exit 1; }
    echo "$(date +%T) now: $name, $*; duty x $scale"
}

case "${1:-}" in
    list)
        settings | awk '{ n = $1; $1 = $2 = $3 = ""; printf "  %-6s%s\n", n, $0 }'
        ;;
    off)
        stop_follow
        echo "off: the kernel's own setting"
        "$probe" regs | tail -2
        ;;
    status)
        if [ -f $pidf ] && kill -0 "$(cat $pidf)" 2>/dev/null; then
            echo "running:"; cat $logf
        else
            echo "not running (the kernel's own setting)"
        fi
        "$probe" regs | tail -2
        ;;
    pmic)
        regs=$(ls /sys/bus/i2c/devices/*/registers 2>/dev/null | head -1)
        [ -n "$regs" ] || { echo "this kernel has no PMIC registers file (needs patch 0025)" >&2; exit 1; }
        out=$here/pmic-$(date +%Y%m%d-%H%M%S).txt
        secs=${2:-300}
        cd /
        setsid sh -c '
            regs=$1; out=$2; secs=$3
            { echo "# full dump"; cat "$regs"; echo "# uptime  status1 status2 ilim  vbat vbus vsys (mV)  usb"; } > "$out"
            i=0
            while [ $i -lt "$secs" ]; do
                eval "$(sed -n "s/^\([0-9a-f][0-9a-f]\): \([0-9a-f][0-9a-f]\)$/r\1=0x\2/p" "$regs")"
                adc() { echo $(( ((($1) & 0x3f) << 8) | ($2) )); }
                echo "$(cut -d" " -f1 /proc/uptime) $r00 $r01 $r16 $(adc $r34 $r35) $(adc $r38 $r39) $(adc $r3a $r3b) $(cat /sys/class/power_supply/battery/online 2>/dev/null)" >> "$out"
                sync
                sleep 1
                i=$((i + 1))
            done' sh "$regs" "$out" "$secs" > /dev/null 2>&1 < /dev/null &
        echo "logging the PMIC for $secs s to $out"
        ;;
    tour)
        secs=${2:-20}
        for n in 33k 25k 20k 10k 5k 2k 50k stock; do
            start $n 1
            sleep "$secs"
        done
        stop_follow
        echo "back to the kernel's own setting"
        ;;
    ""|-h|--help)
        sed -n '4,20p' "$0" | sed 's/^# \{0,1\}//'
        ;;
    *)
        start "$1" "${2:-1}"
        ;;
esac
