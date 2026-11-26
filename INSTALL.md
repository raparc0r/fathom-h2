# Installing Fathom on a HiFi Walker H2

Fathom runs from the microSD card. All it puts on the H2 itself is a small bootloader,
which starts Fathom from the card and can still start the stock firmware. Your music
stays where it is: Fathom only reads it.

**Which H2:** the one sold from 2023. In the stock firmware, System Settings → About the
player → Version shows **V1.9 to V2.3** on these. Older H2s (V1.1 to V1.8) aren't
supported yet, and the installer refuses them.

**Surfans F20:** it's the same player inside, and one owner has reported that Fathom runs
well on theirs. We haven't tried it ourselves. Only the newest F20 matches the supported
H2: the one whose stock firmware shows **V3.4**. Older F20s (V2.2 to V3.3) are like the
older H2s and aren't supported. Uninstall Fathom has only been tried with HiFi Walker's
update file.

**You need:**

- a microSD card formatted **FAT32** (exFAT won't work: see below);
- the H2 charged above 50 %;
- the release, `fathom-h2-VERSION.zip`, unzipped on a computer.

### A FAT32 card

Cards bigger than 32 GB come formatted exFAT, which the H2's bootloader can't read, so
they need formatting as FAT32 first. Formatting erases the card: copy anything on it to
the computer first, and back afterwards. FAT32 works on cards of any size; it only can't
hold a single file over 4 GB (an hour or more of DSD256, say).

- **Mac:** Disk Utility, View > Show All Devices, select the card itself (not the volume
  under it), Erase: Format **MS-DOS (FAT)**, Scheme **Master Boot Record**.
- **Windows:** its own formatter offers FAT32 only up to 32 GB. For a bigger card, use a
  free formatter that does FAT32 at any size, such as Rufus (rufus.ie: Boot selection
  "Non bootable", File system "Large FAT32").
- **Linux:** `sudo mkfs.vfat -F 32 /dev/sdX1` (your card's partition).

## 1. Put your music in a `Music` folder

Fathom looks for music in a folder called `Music` at the top of the card (subfolders
inside it are fine). Music anywhere else on the card won't show up, so if yours is at the
top of the card or in a folder with another name, move it into `Music` first.

## 2. Copy Fathom to the card

Put the card in the computer and copy everything inside the release's `sd/` folder to the
top of the card. The release's other folders (`recovery/` and `about/`) stay on the
computer.

| On the card | What it is |
|---|---|
| `update.upt` | the installer, for the stock firmware's updater |
| `uImage`, `linux_cmdline.txt` | the system Fathom runs on |
| `fathom/` | the player, its fonts and themes; later also your settings and library |
| `Music/` | your music, untouched |

## 3. Install

Put the card in the H2 and turn it on (the stock firmware, as usual). Go to
**System Settings → Firmware Update**. The H2 restarts into its updater, installs Fathom's
bootloader, and restarts into Fathom, which reads your library (a few minutes the first
time for a big one; later checks take seconds).

**If your H2 has Rockbox:** Firmware Update won't work from the stock firmware, because
Rockbox's bootloader starts Rockbox where the updater should start. Instead, turn the H2
off, hold **Vol+** while turning it on to open Rockbox's bootloader menu, and choose
**Boot select → HibyOS Recovery**. The stock updater runs from there and installs Fathom
as above. Fathom's bootloader takes the place of Rockbox's.

At that first start, Fathom also copies the Bluetooth and Wi-Fi firmware from the stock
firmware into its own folder, and says so. Fathom can't distribute those files, but every
H2 already has them: there's nothing more to do for Bluetooth and Wi-Fi.

The stock firmware stays on the H2: hold **Play** while turning it on to start it instead.
You can delete `update.upt` from the card afterwards.

## Next

- **Recommended:** make a backup of your H2's flash, in a few minutes, from the
  bootloader's menu. [ADVANCED.md](ADVANCED.md) explains how.
- **Using Fathom**, from adding music to EQ, Bluetooth headphones, radio and updates:
  [GUIDE.md](GUIDE.md).
- **To remove Fathom** and go back to HiBy's firmware: in Fathom (not in the stock
  firmware), Settings → About → Uninstall Fathom ([GUIDE.md](GUIDE.md)). If Fathom won't
  start, [ADVANCED.md](ADVANCED.md) has the way by hand, under "Removing Fathom".
- **If the installer won't run** or the H2 doesn't start: [ADVANCED.md](ADVANCED.md).
- **If Fathom keeps starting again while it reads your library:** a file in the library
  is stopping the player. Fathom sets such a file aside by itself once it has stopped on
  it twice, so a few minutes may get it through, and Settings → Library → Files not read
  then says which files they were. If it doesn't get through, start with a few albums
  in `Music` and add the rest in parts, and please report it with the logs
  ([ADVANCED.md](ADVANCED.md), "Logs").
