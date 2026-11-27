#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional terms under GPLv3 section 7: see LICENSE.
"""A release's about/SOURCES.txt and about/THIRD-PARTY.txt, and its licence files.

    tools/release/third-party.py VERSION OUT/about

From build/legal-info (tools/release/collect-sources.sh): each third-party
component the release ships, its version and licence, and the archive of its
source in the sources repository (published once per version by
tools/release/publish-sources.sh). Fathom's own part of the source is the tag.
"""
import csv
import os
import shutil
import sys

REPO = 'https://github.com/raparc0r/fathom-h2'
SOURCES = 'https://github.com/raparc0r/fathom-h2-sources/releases/download/sources/'

# Where each Buildroot package ends up (the rest: the card's own folders say)
WHERE = {
    'linux': 'the system (uImage), and its Wi-Fi modules (sd/fathom/wifi/modules)',
    'busybox': 'the system (uImage): the shell and basic commands',
    'musl': 'the system (uImage), and every program: the C library',
    'dropbear': 'the system (uImage): SSH',
    'ffmpeg': 'the player (fathom-player): reading and decoding audio',
    'flac': 'the player: FLAC (libFLAC)',
    'mpg123': 'the player: MP3; and Bluetooth (sd/fathom/bt)',
    'alsa-lib': 'the player: sound output; and Bluetooth (sd/fathom/bt)',
    'tremor': 'the player: Ogg Vorbis',
    'libogg': 'the player: Ogg',
    'opus': 'the player: Opus; and Bluetooth (sd/fathom/bt)',
    'jpeg-turbo': 'the player: covers',
    'sqlite': 'the player: the library; and the network drive (sd/fathom/share)',
    'libxml2': 'the player: podcasts; and the network drive (sd/fathom/share)',
    'libzlib': 'the player; and Bluetooth (sd/fathom/bt)',
    'expat': 'Bluetooth (sd/fathom/bt)',
    'dbus': 'Bluetooth (sd/fathom/bt); and the player (fathom-avrcp)',
    'bluez5_utils': 'Bluetooth (sd/fathom/bt)',
    'bluez-alsa': 'Bluetooth (sd/fathom/bt)',
    'libglib2': 'Bluetooth (sd/fathom/bt)',
    'libffi': 'Bluetooth (sd/fathom/bt)',
    'pcre2': 'Bluetooth (sd/fathom/bt)',
    'readline': 'Bluetooth (sd/fathom/bt: bluetoothctl) and Wi-Fi (sd/fathom/wifi: wpa_cli)',
    'ncurses': 'Bluetooth (sd/fathom/bt)',
    'sbc': 'Bluetooth (sd/fathom/bt)',
    'libfreeaptx': 'Bluetooth (sd/fathom/bt)',
    'wpa_supplicant': 'Wi-Fi (sd/fathom/wifi)',
    'iw': 'Wi-Fi (sd/fathom/wifi)',
    'libnl': 'Wi-Fi (sd/fathom/wifi)',
    'lighttpd': 'the network drive (sd/fathom/share)',
    'mbedtls': 'the network drive (sd/fathom/share): TLS',
    'mdnsd': 'the network drive (sd/fathom/share): fathom-h2.local',
    'gesftpserver': 'the network drive (sd/fathom/share): SFTP',
    'xxhash': 'the network drive (sd/fathom/share)',
    'ca-certificates': 'the network drive and updates (sd/fathom/share): the trusted certificates',
    'tinyalsa': 'the ALSA test tools (sd/fathom/tools)',
}


def main():
    ver, about = sys.argv[1], sys.argv[2]
    li = 'build/legal-info'
    lic = {}
    with open(os.path.join(li, 'manifest.csv'), newline='') as f:
        for row in csv.DictReader(f):
            lic[row['PACKAGE']] = row['LICENSE']
    archives = []
    with open(os.path.join(li, 'sources.list')) as f:
        for line in f:
            name, sha, size, pkg, v = line.split()
            archives.append((name, sha, int(size), pkg, v))
    rockbox = [a for a in archives if a[3] == 'rockbox']
    pkgs = [a for a in archives if a[3] != 'rockbox']
    missing = [a[3] for a in pkgs if a[3] not in WHERE]
    if missing:
        sys.exit('third-party.py: where does %s go? (add it to WHERE)' % ', '.join(missing))
    lvgl = [d for d in os.listdir(os.path.join(li, 'licenses')) if d.startswith('lvgl-')]
    if len(rockbox) != 1 or len(lvgl) != 1:
        sys.exit('third-party.py: build/legal-info is incomplete: run tools/release/collect-sources.sh')
    rb = rockbox[0]
    lvver = lvgl[0][len('lvgl-'):]

    # the licence files, each component's in its own folder
    dst = os.path.join(about, 'licenses')
    shutil.copytree(os.path.join(li, 'licenses'), dst)

    w = []
    w.append('Fathom %s for the HiFi Walker H2: what else is in it' % ver)
    w.append('')
    w.append('Fathom is free software (about/LICENSE). It is built with the components')
    w.append('below, each under its own licence, whose texts are in about/licenses/. The')
    w.append('source of each, as built, is the archive named (in the sources repository:')
    w.append('%s).' % SOURCES.rstrip('/'))
    w.append('')
    for name, sha, size, pkg, v in sorted(pkgs, key=lambda a: a[3].lower()):
        w.append('%s %s' % (pkg, v))
        w.append('  licence:  %s' % lic.get(pkg, 'see about/licenses/%s-%s' % (pkg, v)))
        w.append('  in:       %s' % WHERE[pkg])
        w.append('  source:   %s (SHA-256 %s)' % (name, sha))
        w.append('')
    w.append('Rockbox (the bootloader in sd/update.upt and recovery/bootloader.erosq, and')
    w.append('recovery/jztool), commit %s' % rb[4])
    w.append('  licence:  GPL-2.0-or-later (the second text in about/LICENSE)')
    w.append('  source:   %s (SHA-256 %s)' % (rb[0], rb[1]))
    w.append('            with Fathom\'s patches in bootloader/ at the release\'s tag (about/SOURCES.txt)')
    w.append('')
    w.append('libusb 1.0.30 (built into recovery/jztool)')
    w.append('  licence:  LGPL-2.1-or-later (about/LICENSE-libusb.txt)')
    w.append('  source:   third_party/libusb/ at the release\'s tag (about/SOURCES.txt)')
    w.append('')
    w.append('LVGL %s (the player\'s screens; with Fathom\'s patches in patches/lvgl/)' % lvver)
    w.append('  licence:  MIT (about/licenses/lvgl-%s/)' % lvver)
    w.append('  source:   https://github.com/lvgl/lvgl/tree/v%s' % lvver)
    w.append('')
    w.append('stb_image and stb_image_resize2 (the player: covers)')
    w.append('  licence:  public domain, or MIT (in the files: third_party/stb/ at the release\'s tag)')
    w.append('')
    w.append('Fonts (sd/fathom/assets/fonts/): Atkinson Hyperlegible Next, Noto Sans,')
    w.append('VT323, Ark Pixel (SIL Open Font License 1.1) and Tabler Icons (MIT), their')
    w.append('licences next to them.')
    w.append('')
    w.append('Language packs (not in this zip: the player downloads them from')
    w.append('%s/releases/tag/packs-1): the characters are drawn from' % REPO)
    w.append('Noto Sans JP, KR, SC and TC, Regular and Medium (SIL Open Font License 1.1;')
    w.append('https://github.com/notofonts/noto-cjk). Each pack\'s zip carries its font\'s')
    w.append('copyright line and the licence.')
    open(os.path.join(about, 'THIRD-PARTY.txt'), 'w').write('\n'.join(w) + '\n')

    s = []
    s.append('Fathom %s for the HiFi Walker H2: where its source is' % ver)
    s.append('')
    s.append('Fathom\'s own part, at tag v%s of %s :' % (ver, REPO))
    s.append('%s/tree/v%s' % (REPO, ver))
    s.append('the player and its tools; the Linux kernel\'s patches and settings')
    s.append('(buildroot/external/board/h2/); the Buildroot configuration that builds the')
    s.append('system (buildroot/external/, with Buildroot itself pinned as a submodule);')
    s.append('the bootloader\'s patches and code (bootloader/), and what builds jztool')
    s.append('(tools/build-jztool.sh, third_party/libusb/). Its README and docs say how to')
    s.append('build it all.')
    s.append('')
    s.append('Everything else, each component as it was built (the project\'s release with')
    s.append('Buildroot\'s patches; Rockbox at the bootloader\'s commit), in the sources')
    s.append('repository, one archive per version:')
    s.append(SOURCES.rstrip('/'))
    s.append('about/THIRD-PARTY.txt lists each component, its licence and its archive.')
    s.append('')
    s.append('The Bluetooth and Wi-Fi chip\'s firmware is not in the release: the')
    s.append('bootloader copies it from the H2\'s own stock firmware.')
    open(os.path.join(about, 'SOURCES.txt'), 'w').write('\n'.join(s) + '\n')


main()
