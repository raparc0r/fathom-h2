# AAC for Bluetooth headphones (build it yourself)

Fathom sends Bluetooth audio as SBC, which every pair of headphones takes. Many
sound better with AAC (AirPods, for example, and most headphones made for iPhones).
Fathom's releases can't include it, but you can build it for your own H2.

## Why it isn't in the releases

The only AAC encoder bluez-alsa supports is the Fraunhofer FDK AAC library. Its
licence is a free software licence, but:

- it grants no patent rights, and says so: AAC is patented, and you're told to get
  any patent licence you need yourself;
- the GNU project considers it incompatible with the GPL, and in Fathom it would
  share a program (bluez-alsa's daemon) with BlueZ's `libbluetooth`, which is GPL.

So Fathom's releases ship SBC only (`tools/release/make-release.sh` refuses a card
image with FDK AAC in it), and AAC is something you build and use yourself.
Distributions handle it the same way or ship a trimmed AAC-LC-only version
("fdk-aac-free", Fedora). This isn't legal advice: if you're unsure, don't.

## Build and install

    docker/run.sh tools/bluetooth-aac/build.sh

It builds bluez-alsa with FDK AAC in a Buildroot tree of its own (it needs a C++
toolchain, which Fathom's build doesn't have; the first build takes a while), and
leaves `build/bt-aac/`: `bluealsa`, `libfdk-aac.so.2`, FDK's licence notice and a
note. Copy that folder to the card as `fathom/bt-aac` (USB drive mode works), then
turn Bluetooth off and on in Settings. Headphones that do AAC choose it by themselves;
the add-on keeps SBC and aptX for the others.

Rebuild it whenever you update Fathom: it uses Fathom's own Bluetooth libraries from
`fathom/bt`, which must be the same versions. To go back to SBC, delete
`fathom/bt-aac`.

## Measured on the H2 (AirPods Pro, 2026-11-16)

AAC encoding takes 12-17 % of the CPU; with a 192 kHz 24-bit FLAC (decoding and
conversion to the 44.1 kHz link included) the H2 stays about half idle. AAC sounded
better than SBC on the AirPods.
