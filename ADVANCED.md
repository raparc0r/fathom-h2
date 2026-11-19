# Fathom: advanced

Backups, recovery, the bootloader, SSH, building Fathom and the other technical parts.
For installing, see [INSTALL.md](INSTALL.md); for everyday use, [GUIDE.md](GUIDE.md).

## What Fathom changes on the H2

The only thing written to the H2's internal flash is Fathom's bootloader, in its first
128 KiB. It starts Fathom from the card, can still start the stock firmware, and has a
menu of its own. Everything else, the Linux kernel, the system and the player, is on the
card. Fathom is for the H2 with the AXP2101 power chip (hardware revision 4, sold from
2023).

| At power-on, hold | To |
|---|---|
| nothing | start Fathom |
| **Play** | start the stock firmware |
| **Vol+** | open the bootloader's menu (the wheel moves, Play selects) |

## Backing up your H2 (recommended)

The bootloader can copy the H2's whole flash to the card, factory data included. Keep the
copy somewhere safe and never share it.

1. Turn the H2 off. Hold **Vol+** while turning it on: the bootloader's menu appears.
2. **Debug tools → Dump entire flash.** It takes a few minutes and writes `flash.img`
   (128 MiB) to the card.
3. **System → Reboot**, then copy `flash.img` off the card.

## Updating the bootloader

Updates over Wi-Fi never touch the bootloader. When a release's notes say it changed:

1. Copy `bootloader.erosq` from the release's `recovery/` folder to the top of the card.
2. Hold **Vol+** while turning the H2 on, and choose **Bootloader → Install or update**.
3. **Boot select → Linux** (or restart). You can delete `bootloader.erosq` afterwards.

From Fathom 0.9.0 on, the bootloader also goes back to the previous version by itself if
an update doesn't start twice in a row.

## If an update doesn't start

With a bootloader older than 0.9.0's, put the card in a computer and rename `uImage.prev`
(the kernel the update replaced) at the top of the card to `uImage`, or copy the previous
release's `sd/` folder over the card.

## Installing or recovering with a computer (jztool)

If the stock firmware's updater won't run, or the H2 doesn't start at all, the H2's chip
has a USB-boot mode that can't be overwritten. Rockbox's `jztool` loads the bootloader
into the H2's memory through it, from a Mac or Linux computer with a USB-C cable that
carries data. The release has it for macOS in `recovery/`. On Linux, build it from
Rockbox's source (Debian and Ubuntu package names; on Fedora, `libusb1-devel`):

```bash
sudo apt install build-essential git libusb-1.0-0-dev
git clone --depth 1 https://github.com/Rockbox/rockbox.git
make -C rockbox/utils/jztool
```

and use `sudo rockbox/utils/jztool/jztool` below (it needs root to reach the USB device).

1. Turn the H2 **fully off** (hold Power for 10 seconds if unsure), with the card in.
2. Plug the cable into the H2 only. **Hold Menu**, plug the other end into the computer,
   then let go. The screen stays black: that's normal.
3. In the release's `recovery/` folder: `./jztool erosq load bootloader.erosq` (on a Mac,
   if it's refused as coming from an unknown developer, run
   `xattr -d com.apple.quarantine jztool` first). The bootloader's menu appears; nothing
   has been written yet.
4. From there:
   - **Boot select → Linux** tries Fathom without installing anything;
   - **Boot select → Hiby Player** starts the stock firmware;
   - **Debug tools → Dump entire flash** makes the backup above;
   - **Bootloader → Backup** saves the stock bootloader as `erosqnative-boot.bin`, for a
     later Restore;
   - **Bootloader → Install or update** installs Fathom's bootloader (with
     `bootloader.erosq` from `recovery/` at the top of the card).

## The Bluetooth and Wi-Fi firmware

Bluetooth and Wi-Fi need three files from the stock firmware: the Bluetooth chip's patch,
its Wi-Fi firmware, and the H2's Wi-Fi calibration. They aren't Fathom's to share, so the
bootloader copies them from your H2's own flash to the card's `fathom/firmware/brcm/`
whenever it starts Fathom and one of them is missing. It only reads the flash.

If it says it couldn't (for instance if the stock firmware is gone from the flash), it
doesn't try again by itself. To try again: hold **Vol+** while turning the H2 on, then
**Advanced → Copy BT/Wi-Fi firmware**. Or, from a backup of the flash, on a computer with
Python 3:

```bash
python3 recovery/extract-firmware.py --all flash.img DIR
```

and copy the three files it writes to the card's `fathom/firmware/brcm/`.

## SSH and SFTP

For a shell on the H2, or to copy files with an SFTP app (Cyberduck, Transmit, FileZilla,
WinSCP). Off unless you turn it on in Settings → SSH. The screen shows the addresses
(`10.42.0.1` over the USB cable, and the Wi-Fi address), the user `root` and a password
made up for your H2.

```bash
ssh root@10.42.0.1
```

SFTP uses the same login; the music is in `/mnt/sd/Music`. Music copied this way is found
at the next rescan (Settings → Library → Rescan the music folder) or the next start. The
password is also the USB serial console's. To log in with a key instead, put your public
key in the card's `fathom/keys/authorized_keys`. The SSH and network drive passwords, and
the keys, are kept on the card, readable by anyone holding it.

## Logs

The card keeps the last five start-ups: `fathom/boot-1.log` is the latest, `boot-5.log`
the oldest, and after a crash they also hold what the kernel recorded. Please attach them
when you report a bug.

## AAC for Bluetooth headphones

Fathom sends SBC, which all headphones take, or aptX to headphones that have it. AAC
(better on AirPods and many others) can't be part of a release for licensing reasons, but
you can build it for your own H2: `tools/bluetooth-aac/README.md` in the source.

## Removing Fathom

The stock firmware was never removed: hold **Play** while turning the H2 on to start it,
any time. To take Fathom's bootloader off the flash as well:

- if you saved `erosqnative-boot.bin` with jztool, put it at the top of the card, open the
  bootloader's menu (Vol+ at power-on) and choose **Bootloader → Restore**;
- otherwise, take HiBy's own update file for your H2, add its bootloader with Rockbox's
  `hibyos_nativepatcher.sh mkstockuboot` (HiBy's updates leave the bootloader alone by
  themselves), and put it at the top of the card as `update.upt`. Then hold **Vol+** while
  turning the H2 on and choose **Boot select → HibyOS Recovery**: HiBy's own updater
  installs it, bootloader included. Use the update for the version your H2 had, or a newer
  one: on an H2 sold with newer firmware, HiBy's V2.2 left the screen blank.

## Building it yourself

Everything builds with Buildroot inside Docker, from the source on GitHub:

```bash
docker/run.sh buildroot/build.sh fathom-player-rebuild all   # kernel, system and player
docker/run.sh bootloader/build.sh                           # the bootloader
```

On a Mac, `cmake -S . -B build && cmake --build build` builds the tests and a simulator of
the UI (`build/sim/fathom-sim --music ~/Music`; needs SDL2).
