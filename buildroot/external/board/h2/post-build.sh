#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The root filesystem is an initramfs, so every byte in it costs RAM. The player
# links its libraries statically and runs from the SD card: drop the shared
# copies and tools those libraries install into the target.
set -eu
t="$1"
rm -f "$t"/usr/lib/libsqlite3.so* "$t"/usr/lib/libFLAC*.so* "$t"/usr/lib/libmpg123.so* \
      "$t"/usr/lib/libout123.so* "$t"/usr/lib/libsyn123.so* "$t"/usr/lib/libogg.so* \
      "$t"/usr/lib/libjpeg.so* "$t"/usr/lib/libturbojpeg.so* \
      "$t"/usr/lib/libavcodec.so* "$t"/usr/lib/libavformat.so* "$t"/usr/lib/libavutil.so* \
      "$t"/usr/lib/libopus.so* "$t"/usr/lib/libvorbisidec.so*
rm -rf "$t"/usr/lib/mpg123
rm -f "$t"/usr/bin/sqlite3 "$t"/usr/bin/flac "$t"/usr/bin/metaflac \
      "$t"/usr/bin/mpg123 "$t"/usr/bin/mpg123-id3dump "$t"/usr/bin/mpg123-strip "$t"/usr/bin/out123 \
      "$t"/usr/bin/cjpeg "$t"/usr/bin/djpeg "$t"/usr/bin/jpegtran "$t"/usr/bin/rdjpgcom \
      "$t"/usr/bin/wrjpgcom "$t"/usr/bin/tjbench

# Bluetooth (D-Bus, BlueZ, bluez-alsa) runs from the card, like the player: its
# programs, the shared libraries they need, and their configuration go to
# $BINARIES_DIR/bt (post-image.sh puts them in fathom/bt on the card) and leave
# the initramfs, with the tools that came along (GLib's, PCRE's, D-Bus's).
bt="$BINARIES_DIR/bt"
progs="usr/bin/dbus-daemon usr/libexec/bluetooth/bluetoothd usr/bin/bluetoothctl
       usr/bin/bluealsa usr/bin/bluealsad usr/bin/bluealsa-cli usr/bin/bluealsa-aplay usr/bin/a2dpconf"
confs="etc/dbus-1 etc/bluetooth usr/share/dbus-1 usr/share/alsa"
readelf=$(ls "$HOST_DIR"/bin/*-readelf | head -1)
strip=$(ls "$HOST_DIR"/bin/*-linux-*strip | head -1)
needed() { "$readelf" -d "$1" 2>/dev/null | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p'; }
libpath() { for d in usr/lib lib; do [ -e "$t/$d/$1" ] && { echo "$d/$1"; return 0; }; done; return 0; }
# (libraries from staging: post-build never touches it, so every run sees them all)
staged() { for d in usr/lib lib; do [ -e "$STAGING_DIR/$d/$1" ] && { echo "$d/$1"; return 0; }; done; return 0; }
# A fresh stage: programs and configuration from the target, or from the last
# stage when an earlier run already moved them; libraries from staging.
new="$bt.new"
rm -rf "$new"
progs_in=
for p in $progs; do
    src=
    [ -e "$t/$p" ] && src="$t/$p"
    [ -z "$src" ] && [ -e "$bt/$p" ] && src="$bt/$p"
    [ -n "$src" ] || continue
    mkdir -p "$new/$(dirname "$p")"
    cp -L "$src" "$new/$p"
    progs_in="$progs_in $p"
done
if [ -n "$progs_in" ]; then
    # merged: a package reinstalled alone (bluez-alsa) brings back only its own
    # files to the target, the others are in the last stage
    for d in $confs; do
        for src in "$bt/$d" "$t/$d"; do
            [ -e "$src" ] || continue
            mkdir -p "$new/$d"
            cp -rL "$src/." "$new/$d/"
        done
    done
    # alsa-lib's per-card settings are for PC sound cards: only the list of
    # aliases (which alsa.conf always reads) is kept
    find "$new/usr/share/alsa/cards" -type f ! -name aliases.conf -delete 2>/dev/null || true
    # bluez-alsa's ALSA plugins, from staging too
    [ -d "$STAGING_DIR/usr/lib/alsa-lib" ] && { mkdir -p "$new/usr/lib"; cp -rL "$STAGING_DIR/usr/lib/alsa-lib" "$new/usr/lib/"; }
    libs= todo=$(cd "$new" && find . -type f | sed 's|^\./||')
    while [ -n "$todo" ]; do
        next=
        for f in $todo; do
            for n in $(needed "$new/$f"); do
                case "$n" in libc.so|ld-musl*|libgcc_s.so*) continue ;; esac
                l=$(staged "$n")
                [ -n "$l" ] || { echo "post-build: $f needs $n, not in staging" >&2; exit 1; }
                case " $libs " in *" $l "*) continue ;; esac
                libs="$libs $l"; next="$next $l"
                mkdir -p "$new/$(dirname "$l")"
                cp -L "$STAGING_DIR/$l" "$new/$l"        # FAT has no symlinks: the real file, by its soname
                "$strip" --strip-unneeded "$new/$l" 2>/dev/null || true
            done
        done
        todo=$next
    done
    # Their licences go along (fathom/bt/LICENSES): package, licence, files
    lic="$new/LICENSES"
    mkdir -p "$lic"
    : > "$lic/README.txt"
    while read -r pkg spdx files; do
        dir=$(ls -d "$BUILD_DIR/$pkg"-[0-9]* 2>/dev/null | head -1)
        [ -n "$dir" ] || { echo "post-build: no build of $pkg for its licence" >&2; exit 1; }
        mkdir -p "$lic/${dir##*/}"
        for f in $files; do cp "$dir/$f" "$lic/${dir##*/}/$(echo "$f" | tr / -)"; done
        echo "${dir##*/}: $spdx" >> "$lic/README.txt"
    done <<'EOF'
bluez5_utils GPL-2.0-or-later,LGPL-2.1-or-later COPYING COPYING.LIB
bluez-alsa MIT LICENSE
dbus AFL-2.1-or-GPL-2.0-or-later COPYING
libglib2 LGPL-2.1-or-later COPYING
expat MIT COPYING
libffi MIT LICENSE
pcre2 BSD-3-Clause LICENCE.md
readline GPL-3.0-or-later COPYING
ncurses X11 COPYING
libzlib Zlib LICENSE
sbc GPL-2.0-or-later,LGPL-2.1-or-later COPYING COPYING.LIB
opus BSD-3-Clause COPYING
mpg123 LGPL-2.1 COPYING
alsa-lib LGPL-2.1-or-later COPYING
EOF
    # optional ones, when they're built
    for pkg in libfreeaptx; do
        dir=$(ls -d "$BUILD_DIR/$pkg"-[0-9]* 2>/dev/null | head -1)
        [ -n "$dir" ] && [ -n "$(staged "$pkg.so.0")" ] || continue
        mkdir -p "$lic/${dir##*/}" && cp "$dir/COPYING" "$lic/${dir##*/}/"
        echo "${dir##*/}: LGPL-2.1-or-later" >> "$lic/README.txt"
    done
    printf '\nBuilt from source by Buildroot 2026.02 at the same Fathom tag as the player.\n' >> "$lic/README.txt"
    rm -rf "$bt"
    mv "$new" "$bt"
fi
# Every file of every library on the card stage leaves the initramfs
if [ -d "$bt" ]; then
    for f in $(find "$bt/usr/lib" "$bt/lib" -maxdepth 1 -name 'lib*.so*' 2>/dev/null); do
        b=$(basename "$f"); b=${b%%.so*}
        rm -f "$t"/usr/lib/$b.so* "$t"/lib/$b.so*
    done
fi
# Started from the card by fathom-bt, never at boot; and what came along
rm -f "$t"/etc/init.d/S30dbus-daemon "$t"/etc/init.d/S30dbus "$t"/etc/init.d/S40bluetoothd \
      "$t"/etc/init.d/S40bluetooth "$t"/etc/init.d/S*bluealsa
rm -rf "$t"/etc/dbus-1 "$t"/etc/bluetooth "$t"/usr/share/dbus-1 "$t"/usr/lib/bluetooth \
       "$t"/usr/libexec/bluetooth "$t"/usr/lib/alsa-lib "$t"/usr/share/alsa "$t"/usr/lib/gio \
       "$t"/usr/share/glib-2.0 "$t"/usr/share/gettext "$t"/usr/share/terminfo "$t"/usr/lib/terminfo
rm -f "$t"/usr/bin/dbus-* "$t"/usr/bin/bt* "$t"/usr/bin/hci* "$t"/usr/bin/l2* "$t"/usr/bin/rctest \
      "$t"/usr/bin/sdptool "$t"/usr/bin/ciptool "$t"/usr/bin/mpris-proxy "$t"/usr/bin/bluemoon \
      "$t"/usr/bin/hex2hcd "$t"/usr/bin/isotest "$t"/usr/bin/bluealsa* "$t"/usr/bin/a2dpconf \
      "$t"/usr/bin/bluetoothctl "$t"/usr/bin/gio "$t"/usr/bin/gio-* "$t"/usr/bin/gapplication "$t"/usr/bin/gi-* "$t"/usr/bin/gdbus* \
      "$t"/usr/bin/gsettings "$t"/usr/bin/glib-* "$t"/usr/bin/gresource "$t"/usr/bin/gobject-query \
      "$t"/usr/bin/gtester* "$t"/usr/bin/pcre2* "$t"/usr/bin/aserver \
      "$t"/usr/libexec/gio-launch-desktop
for l in libgirepository-2.0 libgthread-2.0 libpcre2-posix libform libmenu libpanel libhistory \
         libatopology libcurses; do
    rm -f "$t"/usr/lib/$l.so*
done
# (the device table sets this one's permissions, so it must exist: empty)
mkdir -p "$t"/usr/libexec && : > "$t"/usr/libexec/dbus-daemon-launch-helper

# Wi-Fi runs from the card too (fathom/wifi, started by fathom-wifi): wpa_supplicant,
# wpa_cli and iw, the libraries they need that Bluetooth's stage doesn't have
# (fathom-wifi puts both on the library path), and the kernel's Wi-Fi modules
# (cfg80211, brcmutil, brcmfmac), loaded only while Wi-Fi is on.
wifi="$BINARIES_DIR/wifi"
wnew="$wifi.new"
rm -rf "$wnew"
wprogs_in=
for p in usr/sbin/wpa_supplicant usr/sbin/wpa_cli usr/sbin/iw; do
    src=
    [ -e "$t/$p" ] && src="$t/$p"
    [ -z "$src" ] && [ -e "$wifi/$p" ] && src="$wifi/$p"
    [ -n "$src" ] || continue
    mkdir -p "$wnew/$(dirname "$p")"
    cp -L "$src" "$wnew/$p"
    "$strip" --strip-unneeded "$wnew/$p" 2>/dev/null || true
    wprogs_in="$wprogs_in $p"
done
mods=$(find "$t/lib/modules" -name '*.ko' 2>/dev/null)
if [ -n "$mods" ]; then
    mkdir -p "$wnew/modules"
    for m in $mods; do cp "$m" "$wnew/modules/"; done
elif [ -d "$wifi/modules" ]; then
    mkdir -p "$wnew" && cp -r "$wifi/modules" "$wnew/"
fi
if [ -n "$wprogs_in" ]; then
    wlibs= todo=$wprogs_in
    while [ -n "$todo" ]; do
        next=
        for f in $todo; do
            for n in $(needed "$wnew/$f"); do
                case "$n" in libc.so|ld-musl*|libgcc_s.so*) continue ;; esac
                [ -e "$bt/usr/lib/$n" ] || [ -e "$bt/lib/$n" ] && continue   # Bluetooth's stage has it
                l=$(staged "$n")
                [ -n "$l" ] || { echo "post-build: $f needs $n, not in staging" >&2; exit 1; }
                case " $wlibs " in *" $l "*) continue ;; esac
                wlibs="$wlibs $l"; next="$next $l"
                mkdir -p "$wnew/$(dirname "$l")"
                cp -L "$STAGING_DIR/$l" "$wnew/$l"
                "$strip" --strip-unneeded "$wnew/$l" 2>/dev/null || true
            done
        done
        todo=$next
    done
    wl="$wnew/LICENSES"
    mkdir -p "$wl"
    : > "$wl/README.txt"
    while read -r pkg spdx files; do
        dir=$(ls -d "$BUILD_DIR/$pkg"-[0-9]* 2>/dev/null | head -1)
        [ -n "$dir" ] || continue
        mkdir -p "$wl/${dir##*/}"
        for f in $files; do [ -f "$dir/$f" ] && cp "$dir/$f" "$wl/${dir##*/}/"; done
        echo "${dir##*/}: $spdx" >> "$wl/README.txt"
    done <<'EOF'
wpa_supplicant BSD-3-Clause COPYING README
iw ISC COPYING
libnl LGPL-2.1-or-later COPYING
linux GPL-2.0-only COPYING
EOF
    printf '\nThe modules are Linux 6.18 (cfg80211, brcmutil, brcmfmac), from the same source\nas the kernel (uImage). Built by Buildroot 2026.02 at the same Fathom tag as the player.\n' >> "$wl/README.txt"
fi
if [ -d "$wnew" ]; then rm -rf "$wifi"; mv "$wnew" "$wifi"; fi
# ...out of the initramfs
rm -f "$t"/usr/sbin/wpa_supplicant "$t"/usr/sbin/wpa_cli "$t"/usr/sbin/wpa_passphrase "$t"/usr/sbin/iw
find "$t/lib/modules" -name '*.ko' -delete 2>/dev/null || true
if [ -d "$wifi" ]; then
    for f in $(find "$wifi/usr/lib" "$wifi/lib" -maxdepth 1 -name 'lib*.so*' 2>/dev/null); do
        b=$(basename "$f"); b=${b%%.so*}
        rm -f "$t"/usr/lib/$b.so* "$t"/lib/$b.so*
    done
fi
rm -f "$t"/etc/init.d/S*wpa_supplicant
for l in libnl-route-3 libnl-nf-3 libnl-xfrm-3 libnl-idiag-3 libnl-cli-3; do   # libnl's, unused
    rm -f "$t"/usr/lib/$l.so*
done
rm -f "$t"/usr/bin/nl-* "$t"/usr/bin/genl-ctrl-list "$t"/usr/bin/idiag-socket-details
# The network drive runs from the card too (fathom/share, started by fathom-share):
# lighttpd with its WebDAV, auth, log and TLS modules, mdnsd (Bonjour), the
# SFTP server SSH logins use (/usr/libexec/sftp-server runs it from there), and
# the libraries they need that the Bluetooth and Wi-Fi stages don't have
# (libxml2, SQLite, mbedTLS...). fathom-share puts all three stages on the
# library path.
share="$BINARIES_DIR/share"
snew="$share.new"
rm -rf "$snew"
sprogs_in=
for p in usr/sbin/lighttpd usr/sbin/mdnsd usr/libexec/gesftpserver; do
    src=
    [ -e "$t/$p" ] && src="$t/$p"
    [ -z "$src" ] && [ -e "$share/$p" ] && src="$share/$p"
    [ -n "$src" ] || continue
    mkdir -p "$snew/$(dirname "$p")"
    cp -L "$src" "$snew/$p"
    "$strip" --strip-unneeded "$snew/$p" 2>/dev/null || true
    sprogs_in="$sprogs_in $p"
done
for m in mod_webdav mod_auth mod_authn_file mod_accesslog mod_mbedtls; do
    src=
    [ -e "$t/usr/lib/lighttpd/$m.so" ] && src="$t/usr/lib/lighttpd/$m.so"
    [ -z "$src" ] && [ -e "$share/usr/lib/lighttpd/$m.so" ] && src="$share/usr/lib/lighttpd/$m.so"
    [ -n "$src" ] || continue
    mkdir -p "$snew/usr/lib/lighttpd"
    cp -L "$src" "$snew/usr/lib/lighttpd/"
    "$strip" --strip-unneeded "$snew/usr/lib/lighttpd/$m.so" 2>/dev/null || true
    sprogs_in="$sprogs_in usr/lib/lighttpd/$m.so"
done
# The certificate authorities' bundle, for the player's HTTPS (radio)
ca="$t/etc/ssl/certs/ca-certificates.crt"
[ -s "$ca" ] || ca="$share/ca-certificates.crt"
if [ -s "$ca" ]; then
    mkdir -p "$snew"
    cp -L "$ca" "$snew/ca-certificates.crt"
fi
if [ -n "$sprogs_in" ]; then
    slibs= todo=$sprogs_in
    while [ -n "$todo" ]; do
        next=
        for f in $todo; do
            for n in $(needed "$snew/$f"); do
                case "$n" in libc.so|ld-musl*|libgcc_s.so*) continue ;; esac
                [ -e "$bt/usr/lib/$n" ] || [ -e "$bt/lib/$n" ] && continue
                [ -e "$wifi/usr/lib/$n" ] || [ -e "$wifi/lib/$n" ] && continue
                # (from staging; libmdnsd is only installed to the target, or
                # is in the last stage once an earlier run moved it)
                l=$(staged "$n") from=$STAGING_DIR
                [ -z "$l" ] && l=$(libpath "$n") && from=$t
                [ -z "$l" ] && for d in usr/lib lib; do [ -e "$share/$d/$n" ] && l=$d/$n from=$share; done
                [ -n "$l" ] || { echo "post-build: $f needs $n, not in staging" >&2; exit 1; }
                case " $slibs " in *" $l "*) continue ;; esac
                slibs="$slibs $l"; next="$next $l"
                mkdir -p "$snew/$(dirname "$l")"
                cp -L "$from/$l" "$snew/$l"
                "$strip" --strip-unneeded "$snew/$l" 2>/dev/null || true
            done
        done
        todo=$next
    done
    sl="$snew/LICENSES"
    mkdir -p "$sl"
    : > "$sl/README.txt"
    while read -r pkg spdx files; do
        dir=$(ls -d "$BUILD_DIR/$pkg"-[0-9]* 2>/dev/null | head -1)
        [ -n "$dir" ] || continue
        mkdir -p "$sl/${dir##*/}"
        for f in $files; do [ -f "$dir/$f" ] && cp "$dir/$f" "$sl/${dir##*/}/"; done
        echo "${dir##*/}: $spdx" >> "$sl/README.txt"
    done <<'EOF'
lighttpd BSD-3-Clause COPYING
mdnsd BSD-3-Clause LICENSE
libxml2 MIT Copyright
sqlite blessing -
mbedtls Apache-2.0 LICENSE
gesftpserver GPL-2.0-or-later COPYING
ca-certificates MPL-2.0 debian/copyright
EOF
    printf '\nBuilt from source by Buildroot 2026.02 at the same Fathom tag as the player.\n' >> "$sl/README.txt"
fi
if [ -d "$snew" ]; then rm -rf "$share"; mv "$snew" "$share"; fi
# ...out of the initramfs, with what came along
rm -f "$t"/usr/sbin/lighttpd "$t"/usr/sbin/lighttpd-angel "$t"/usr/sbin/mdnsd "$t"/usr/bin/mquery \
      "$t"/usr/libexec/gesftpserver
rm -rf "$t"/usr/lib/lighttpd "$t"/etc/lighttpd "$t"/etc/mdns.d "$t"/var/www
rm -rf "$t"/usr/share/ca-certificates "$t"/etc/ssl           # (the bundle is on the card)
rm -f "$t"/etc/init.d/S50lighttpd "$t"/etc/init.d/S50mdnsd "$t"/etc/default/mdnsd
if [ -d "$share" ]; then
    for f in $(find "$share/usr/lib" "$share/lib" -maxdepth 1 -name 'lib*.so*' 2>/dev/null); do
        b=$(basename "$f"); b=${b%%.so*}
        rm -f "$t"/usr/lib/$b.so* "$t"/lib/$b.so*
    done
fi
rm -f "$t"/usr/bin/xml2-config "$t"/usr/bin/xmllint "$t"/usr/bin/xmlcatalog "$t"/usr/lib/libblkid.so* \
      "$t"/usr/lib/libmount.so* "$t"/usr/lib/libsmartcols.so* "$t"/usr/lib/libfdisk.so*
# (lighttpd uses xxhash's header only; util-linux's libuuid serves nothing on the H2)
rm -f "$t"/usr/lib/libxxhash.so* "$t"/usr/lib/libuuid.so* "$t"/lib/libuuid.so*

# SSH is started by fathom-ssh (S90fathom, then Settings > SSH), not at boot
# by itself; and root is locked (no password works) until fathom-ssh sets the
# one the H2 shows, for SSH and the serial console alike.
rm -f "$t"/etc/init.d/S50dropbear "$t"/etc/default/dropbear
sed -i 's/^root:[^:]*:/root:*:/' "$t"/etc/shadow
# mbedTLS: linked into the player (FFmpeg's https), and in fathom/share for lighttpd
rm -f "$t"/usr/lib/libmbedtls.so* "$t"/usr/lib/libmbedx509.so* "$t"/usr/lib/libmbedcrypto.so*

# Nothing left in the initramfs may miss a library
missing=
for f in $(find "$t" -type f -perm -u+x | sed "s|^$t/||"); do
    for n in $(needed "$t/$f"); do
        [ -n "$(libpath "$n")" ] || missing="$missing $f:$n"
    done
done
[ -z "$missing" ] || { echo "post-build: missing libraries:$missing" >&2; exit 1; }
