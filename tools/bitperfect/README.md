# Bit-perfect checks

Proves that what the player writes to the sound card is exactly what the file
holds: no resampling, dithering, gain, truncation or dropped samples, and no
gap or overlap between gapless tracks. The DAC's analogue output can't be
captured, so the check covers everything up to the card: the bytes handed to
the kernel, in the card's own format.

1. On the Mac: `build/make-bitperfect build/bitperfect` writes FLAC test files
   with known samples (full-scale noise, starting with the extreme values) and
   the same samples as `.raw`.
2. Build `fathom-bitperfect` (device/CMakeLists.txt; it isn't installed: copy
   it from the Buildroot build folder) and copy it and the `.flac` files to the
   H2, e.g. `/mnt/sd/fathom/test/bp/`.
3. On the H2, with the player stopped (`/etc/init.d/S90fathom stop`):

       ./fathom-bitperfect tap.raw noise16_44k.flac noise24_44k.flac
       ./fathom-bitperfect -d /dev/snd/pcmC1D0p tap-usb.raw noise24_96k.flac   # a USB DAC

   It turns the card's volume control to its minimum first (the noise is full
   scale), refuses a card without one unless `BP_NO_VOLUME_OK=1` (turn the DAC
   down by hand), and prints the format the card runs at for each track.
4. Back on the Mac, one `REF:BITS:FORMAT` per track, as printed:

       python3 tools/bitperfect/compare.py tap.raw \
           build/bitperfect/noise16_44k.raw:16:S16_LE build/bitperfect/noise24_44k.raw:24:S24_LE

## Results (internal DAC, ES9018K2M, 2026-11-15)

| Sequence | Card format | Result |
|---|---|---|
| 16-bit / 44.1 kHz | S16_LE | bit-perfect |
| 24-bit / 96 kHz | S24_LE | bit-perfect |
| 24-bit / 192 kHz | S24_LE | bit-perfect |
| gapless pair, 24/96, cut at an odd frame | S24_LE | bit-perfect, no sample lost or repeated at the join |
| 16/44.1 then 24/44.1 | S16_LE, S24_LE | bit-perfect (was cut to 16 bits before the fix) |
| 24/96, 16/44.1, gapless pair 24/96 | S24_LE, S16_LE, S24_LE | bit-perfect |

Other formats (through FFmpeg, 2026-11-16): WAV, AIFF and ALAC (.m4a), each at 16/44.1
and 24/96, made from the same samples with macOS's `afconvert`: S16_LE and S24_LE,
bit-perfect, exact lengths. AAC (lossy) starts on the right sample.

USB DACs: not checked yet. With a DAC that has its own volume control the path
is the same; one without falls back to software volume (exact only at 0 dB).

## USB DAC mode (the H2 as a computer's sound card)

`tools/usbdac`: `usbdac-play` (macOS) plays silence, then 24-bit noise, to the "Fathom
H2" output; the H2 records what its DAC gets while `/tmp/fathom-usbdac-tap` exists, and
`check.py` finds the noise in that recording. The recording restarts with each stream, so
it holds the last play only. USB DAC mode is the audio function alone (no network or
serial console while it's on), so fetch the recording after leaving it:

    cc -O2 -o usbdac-play tools/usbdac/usbdac_play.c -framework CoreAudio -framework CoreFoundation
    ssh root@10.42.0.1 touch /tmp/fathom-usbdac-tap     # then Sound -> USB DAC mode
    ./usbdac-play "Fathom H2" 96000 2 ref96.raw
    # Back on the H2 ends USB DAC mode; the network comes back
    ssh root@10.42.0.1 cat /tmp/fathom-usbdac.raw > tap96.raw
    python3 tools/usbdac/check.py tap96.raw ref96.raw S24_LE

Results (MacBook Pro, macOS, 2026-11-16): 44.1, 48, 88.2, 96, 176.4 and 192 kHz, 32-bit
from the Mac into the DAC at S24_LE: bit-perfect. Three minutes at 44.1 kHz: no overruns,
and the DAC's queue stayed between 34 and 50 ms (the H2's clock leads; the Mac follows its
feedback). An optional fifth argument, 16 or 32, sends 16-bit samples or adds detail below
the 24th bit (as a float app does), to check the bit depth the H2 shows. Starting and stopping: 200 half-second plays switching between 44.1, 48 and
96 kHz, then a bit-perfect check: no errors, no overruns.

Why the audio function is alone: the X1000's USB controller has one receive FIFO for
every OUT endpoint. Stopping the audio stream (the computer does it at each pause or rate
change) needs that FIFO to drain; network packets waiting in it made the stop time out,
and later transfers came out garbled (a lost device, or a kernel panic in the network
gadget). Kernel patch 0018 now also refuses to complete a transfer longer than its buffer.
