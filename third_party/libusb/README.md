# libusb 1.0.30

The source of libusb, as released: `libusb-1.0.30.tar.bz2` from
https://github.com/libusb/libusb/releases/tag/v1.0.30
(SHA-256 fea36f34f9156400209595e300840767ab1a385ede1dc7ee893015aea9c6dbaf).

`tools/build-jztool.sh` builds it, unchanged, into the release's `recovery/jztool` (the
USB-boot tool for macOS), so jztool needs nothing installed. libusb is under the GNU Lesser
General Public License 2.1 or later (`COPYING` in the archive); this archive goes into
every release's bootloader source archive with jztool's own source.
