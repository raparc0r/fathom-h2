#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional terms under GPLv3 section 7: see LICENSE.
#
# USB DAC bring-up on the H2, for when the cable to the computer is unplugged
# (no ssh): waits for a USB audio card, logs what the kernel found, sets the
# DAC's own volume to -40 dB and, only if that worked, plays short quiet tones.
# Repeats for every DAC plugged in. Log: /mnt/sd/fathom/usbdac.log
#
#   nohup /mnt/sd/fathom/test/usbdac-test.sh >/dev/null 2>&1 &
t=/mnt/sd/fathom/test
log=/mnt/sd/fathom/usbdac.log
exec >>"$log" 2>&1
now() { cut -d' ' -f1 /proc/uptime; }
echo "=== usbdac-test start at $(now) s"
dmesg -c >/dev/null

# Heartbeat, synced to the card every second, so a freeze still leaves its
# last moments: the VBUS line, dwc2's OTG and interrupt status, the battery.
# Only these two global registers: reading a device-mode register while dwc2
# is the host (debugfs regdump does) raises a "Mode Mismatch" interrupt and a
# console warning each, enough to starve the audio stream.
(
    while :; do
        vbus=$(grep regulator-usb-vbus /sys/kernel/debug/gpio | awk '{print $NF}')
        r="GOTGCTL=$(devmem 0x13500000) GINTSTS=$(devmem 0x13500014) "
        echo "[$(now)] hb vbus=$vbus ${r}bat=$(cat /sys/class/power_supply/battery/voltage_now)"
        sync
        sleep 1
    done
) &
hb=$!

while :; do
    # wait for a USB audio card (card 0 is the H2's own DAC)
    card=""
    while [ -z "$card" ]; do
        for c in /proc/asound/card[1-9]; do
            [ -e "$c/stream0" ] && card=${c#/proc/asound/card}
        done
        k=$(dmesg -c)
        [ -n "$k" ] && printf '%s\n' "$k" | grep -i -E "usb|dwc2|vbus|regulator|otg|oops|bug|warn|lockup" | sed "s/^/[$(now)] /"
        sleep 1
    done
    sleep 2
    echo "--- card $card at $(now) s"
    cat /proc/asound/cards
    cat /proc/asound/card$card/stream0
    for d in /sys/bus/usb/devices/[0-9]-[0-9]*; do
        [ -e "$d/idVendor" ] || continue
        echo "$d: $(cat $d/idVendor):$(cat $d/idProduct) '$(cat $d/manufacturer 2>/dev/null)' '$(cat $d/product 2>/dev/null)' speed $(cat $d/speed) maxpower $(cat $d/bMaxPower)"
    done
    echo "battery $(cat /sys/class/power_supply/battery/voltage_now) uV"
    $t/alsa-ctl $card
    if $t/alsa-ctl $card volume -40dB; then
        # Two 10 s tones: a click every ~2.05 s would be dwc2's microframe
        # counter wrapping (16384 microframes)
        kill -STOP $hb
        for f in "48000 16" "192000 24"; do
            echo "tone $f at $(now) s"
            ALSA_TONE_DEV=/dev/snd/pcmC${card}D0p $t/alsa-tone $f -30 10 LR 0.3
            echo "alsa-tone $f: exit $?"
            sleep 2
        done
        kill -CONT $hb
    else
        echo "volume not set: no tones played"
    fi
    echo "battery $(cat /sys/class/power_supply/battery/voltage_now) uV"
    dmesg -c | grep -i -E "usb|xrun|error|fail" | sed "s/^/[$(now)] /"
    # wait for it to go away, then go round for the next one
    while [ -e /proc/asound/card$card ]; do sleep 1; done
    echo "--- card $card unplugged at $(now) s"
done
