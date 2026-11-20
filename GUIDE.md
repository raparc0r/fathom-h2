# Using Fathom

Most of it should feel familiar. This guide covers the keys, then each feature that needs
a word of explanation. In Settings, **Menu** on any setting says in a line or two what it
does.

## The keys

| Key | Press | Hold |
|---|---|---|
| Wheel | move through a list | |
| Play | choose (OK); on Now Playing, play or pause | |
| Back | go back | jump to Now Playing, and back |
| Menu | options for what's selected (play next, add to a playlist, go to the album...) | quick settings: brightness, shuffle, repeat, EQ, Bluetooth, Wi-Fi, sleep timer |
| Menu + wheel | | brightness: hold Menu and turn the wheel |
| Prev / Next | previous or next track | seek |
| Power | screen off or on | switch off (2 seconds) |

With the screen off, Play, Prev, Next and the volume keys still work.

## Adding music

Plug the H2 into a computer and choose **Use as a USB drive** (or Settings → USB
connection → USB drive mode). The card appears on the computer: copy your music into its
`Music` folder, then eject it. Fathom picks up what changed by itself. Fathom reads FLAC,
MP3, ALAC, AAC, WAV, AIFF, WavPack, APE, TTA, Opus, Ogg Vorbis and DSD, and albums ripped
as one file with a CUE sheet.

You can also take the card out and put it back: Fathom stops, shows "No/bad card", and
starts again with your new music when the card is back. Over Wi-Fi, the network drive
(below) lets you copy music while the H2 plays.

## The H2 as your computer's sound card

Plug it in and choose **Use as a USB DAC** (or Settings → Sound → USB DAC mode). The
computer then plays through the H2's headphone or line out, at the H2's volume. On a Mac,
the first time, allow the accessory to connect when macOS asks, then choose "Fathom H2"
as the output. Play, Prev and Next on the H2 control the computer's player; Back ends it.

Settings → USB connection → On a computer chooses what happens when you plug in: ask
(the default), USB drive, USB DAC, or just charge.

## Covers, artists and themes

Settings → Interface holds the look: the theme, album covers in the lists, artist names
in Albums, and the language. Your own themes go in the card's `fathom/themes/` folder
([THEMES.md](THEMES.md) explains every option); restart the H2 to see a new one.
Translations go in `fathom/lang/`, one `English text = translation` per line.

## EQ

Fathom plays your music untouched unless you turn something on. To correct your
headphones' sound, get their profile from [AutoEQ](https://autoeq.app) (choose your
headphones, then "Equalizer APO" / Parametric EQ, and download `ParametricEQ.txt`), and
copy it to the card's `fathom/eq/` folder. Then, with the headphones plugged in or
connected: Settings → Sound → EQ, and choose it. Fathom remembers the choice for each
output (the headphone jack, the line out, each USB DAC, each pair of Bluetooth
headphones), so the EQ follows your headphones. Now Playing shows "EQ" while it's on, and
quick settings can switch between Off and your EQs as you listen.

To make your own or change one: in Settings → Sound → EQ, **New EQ** starts five flat
bands, and **Menu** on any EQ opens it. Choose the preamp or a band, press Play, and turn
the wheel; press Play again when it's right. You hear each change as you make it. A band's
Type switches between peak, low shelf and high shelf.

**Pure mode** (Settings → Sound) turns off the EQ, balance and volume levelling in one go,
and Now Playing shows "Bit+" while the music reaches the DAC exactly as in the file.

## Bluetooth headphones

Settings → Bluetooth, turn it on, put the headphones in pairing mode, choose **Find
headphones** and select them. Playback moves to them while they're connected, the volume
keys set their volume, and their own buttons play, pause and skip. Menu on a paired
device forgets it.

If the sound breaks up a few metres away, Settings → Bluetooth → Audio → **Longer range**
sends less over the air so the link holds further; **Best sound** is the default. With
Wi-Fi on, Bluetooth shares the radio with it, so turning Wi-Fi off can help too.

AirPods that are also paired with an iPhone may move to the iPhone while the H2 is paused,
and play nothing when it resumes. To stop that, on the iPhone: Settings → Bluetooth → (i)
next to the AirPods → Connect to This iPhone → When Last Connected to This iPhone.

## Wi-Fi

Settings → Wi-Fi, turn it on, choose your network and type its password with the wheel:
Play adds the letter shown, Menu switches between lower case, capitals, digits and
symbols, Back deletes, and **Join** at the end of the letters joins. Saved networks are
joined by themselves next time; Menu on one forgets it. If you use something that needs
Wi-Fi while it's off, the H2 offers to turn it on.

The H2 sets its clock over Wi-Fi when it connects, since it has no clock that keeps
running while it's off.

## Internet radio

Home → Radio plays stations over Wi-Fi. Fathom comes with a few, and **Find stations**
searches a directory of thousands: by name, by genre or country, the most popular, or a
random one. Menu on a station adds it to your stations. Play stops and starts the stream;
Prev and Next change station.

Your stations are kept in the card's `fathom/radio.txt`, one per line, which you can also
edit on a computer:

```
Radio Paradise = https://stream.radioparadise.com/flacm
```

The address can be the stream itself (MP3, AAC, FLAC, Ogg, over http or https) or the
station's playlist link (`.m3u`, `.pls`, `.m3u8`).

## Network drive

Copy music to the H2 over Wi-Fi, while it plays. Turn on Settings → Network drive; once
Wi-Fi is connected, the screen shows the address, the user (`fathom`), a password made up
for your H2 (New password makes another) and the start of its certificate's fingerprint.

The drive only works over a secure connection (HTTPS), so the password and your files
never cross the network in the clear. The H2 makes its own certificate, so the first time
your computer asks whether to trust it: check that the fingerprint it shows starts with
the one on the H2's screen, then trust it.

- **Mac:** Finder → Go → Connect to Server, `https://fathom-h2.local`. Accept the
  certificate (Show Certificate → "Always trust" spares you the question next time), then
  choose Registered User, change the name Finder fills in (your Mac's) to `fathom`, and
  enter the password. The music folder opens like a disk.
- **Windows:** Windows only connects to a certificate it already trusts. Open
  `https://fathom-h2.local` in a browser, save the certificate and install it under
  "Trusted Root Certification Authorities" (Local Machine). Then File Explorer → This PC
  → Map network drive, folder `https://fathom-h2.local`, with the user and password.

New music appears in the library about 10 seconds after the copying stops. Copies run at
about 1 to 2 MB/s, so for a whole library USB drive mode is faster.

## Battery

Settings → Power → Battery shows how much charge is left and roughly how long it lasts,
and opens a graph of the last day or week. **Power off when idle** switches the H2 off
when it's paused and untouched on battery (15 minutes by default), and the **sleep timer**
stops the music after a while or at the end of the album.

## Updating Fathom

Once a day, while the H2 rests on a Wi-Fi network it has joined before, Fathom asks GitHub
whether there's a newer version, tells you the next time you wake the H2, and puts
**Update available!** at the top of Home. Choose it, then **Install new version** to get
it, or **Ignore** to hide it until a newer one comes out. That's the only thing Fathom
contacts by itself, besides a time server when Wi-Fi connects; turn it off in Settings →
About → Check automatically.

To update: Settings → About → **Check for updates**, then **Update to vX.Y.Z**. It
downloads (about 14 MB) while the music plays on, checks every file, then asks: **Play**
installs it and restarts the H2, **Back** cancels. It needs about 65 MB free on the card
while it works. Your music, settings, library, playlists, EQs, themes and Wi-Fi networks
stay as they are.

Don't turn the H2 off while it says "Installing" (a few seconds); if it loses power anyway,
it finishes at the next start. If a new version ever fails to start, the H2 goes back to
the previous one by itself (with the bootloader from Fathom 0.9.0 or later: see
[ADVANCED.md](ADVANCED.md)).

Without Wi-Fi, copy the new release's `sd/` folder over the old files on the card (USB
drive mode works). Each release's notes say if anything else is needed.

## Uninstalling Fathom

HiBy's own firmware is still on the H2: hold **Play** while turning it on to start it, any
time. To go back to it for good, its bootloader included, Fathom can do it from the H2:

1. Download the firmware for the H2 from HiFi Walker's website: the version your H2 came
   with, or a newer one (an older one can leave the screen blank). HiBy's firmware shows
   its version in System Settings → About.
2. Put its `.upt` file at the top of the card, or in the `Music` folder (where the network
   drive puts files, from a phone too). There's no need to rename it.
3. Settings → About → **Uninstall Fathom**. Fathom checks the file (a few seconds) and
   shows its date. Choose **Uninstall Fathom** and press **OK** twice. The H2 needs half
   its battery, or to be plugged in.
4. **OK** restarts the H2 into HiBy's recovery, which installs HiBy's firmware and starts
   it, in a few minutes. Don't turn the H2 off meanwhile. **Back** instead puts everything
   back as it was.

If the H2 starts Fathom again instead, turn it off, hold **Vol+** while turning it on and
choose **Boot select → HibyOS Recovery**.

Your music and Fathom's folder stay on the card. To install Fathom again later, on a
computer rename HiBy's `update.upt` at the top of the card (back to its download name,
say) and name Fathom's installer `update.upt` (Uninstall set it aside as
`fathom-install.upt` if it was on the card), then run **System Settings → Firmware
Update** as in [INSTALL.md](INSTALL.md). Your settings and library are where you left them.

## What's in the `fathom` folder

Fathom keeps its settings, library and cover cache in the card's `fathom` folder, and
that's where you add things of your own: `radio.txt`, `eq/`, `themes/` and `lang/` (all
above). Wi-Fi passwords and the network drive's password are kept there too, so anyone
holding the card can read them.

More for the technically minded (SSH, logs, recovery): [ADVANCED.md](ADVANCED.md).
