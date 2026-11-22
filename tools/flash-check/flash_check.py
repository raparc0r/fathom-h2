#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional terms under GPLv3 section 7: see LICENSE.
"""
flash-check: sanity-check a full H2 flash dump (flash.img from the bootloader's
"Dump entire flash") against the stock firmware update file.

    python3 tools/flash-check/flash_check.py backups/flash.img H2-Firmware-V2.2.upt

It checks, read-only:
  * size (128 MiB) and how much of each partition is erased (all 0xFF)
  * the kernel and recovery uImages: header and payload CRCs (self-consistency),
    and whether they are the same build as the .upt's uimage.bin
  * every UBI erase-counter and volume header in rootfs and data (CRC32)
  * the U-Boot region against uboot.bin from the .upt (informational: a
    third-party bootloader such as Rockbox replaces it)
Uses macOS/BSD `bsdtar` to read the .upt.
"""
import hashlib
import struct
import subprocess
import sys
import zlib

FLASH_SIZE = 128 * 1024 * 1024
BLOCK = 128 * 1024
PARTS = [  # name, offset, size
    ("uboot", 0x0, 0x80000),
    ("logo", 0x80000, 0x280000),
    ("kernel", 0x300000, 0x600000),
    ("recovery", 0x900000, 0x700000),
    ("rootfs", 0x1000000, 0x4000000),
    ("data", 0x5000000, FLASH_SIZE - 0x5000000),
]


def from_upt(upt, name):
    return subprocess.run(["bsdtar", "-xOf", upt, name], check=True, capture_output=True).stdout


def erased_blocks(buf):
    blank = b"\xff" * BLOCK
    return sum(1 for o in range(0, len(buf), BLOCK) if buf[o:o + BLOCK] == blank)


def check_uimage(b):
    if b[:4] != b"\x27\x05\x19\x56":
        return False, "no uImage header"
    h = bytearray(b[:64])
    hcrc = struct.unpack(">I", h[4:8])[0]
    h[4:8] = b"\0\0\0\0"
    size, dcrc = struct.unpack(">I", b[12:16])[0], struct.unpack(">I", b[24:28])[0]
    name = b[32:64].split(b"\0")[0].decode("latin1")
    good = (zlib.crc32(bytes(h)) & 0xFFFFFFFF) == hcrc and (zlib.crc32(b[64:64 + size]) & 0xFFFFFFFF) == dcrc
    return good, f"{name}, {size:,} bytes, CRCs {'ok' if good else 'BAD'}"


def ubi_crc(buf):
    return ~zlib.crc32(buf) & 0xFFFFFFFF   # UBI: crc32_le(0xFFFFFFFF, ...) without final xor


def check_ubi(part):
    pebs = len(part) // BLOCK
    erased = used = bad = marked = 0
    for i in range(pebs):
        peb = part[i * BLOCK:(i + 1) * BLOCK]
        if peb[:4] == b"\xff\xff\xff\xff":
            erased += 1
            continue
        if peb.count(0) == len(peb):
            marked += 1          # retired bad block: Linux/UBI fills it with zeros
            continue
        ec = peb[:64]
        if ec[:4] != b"UBI#" or ubi_crc(ec[:60]) != struct.unpack(">I", ec[60:64])[0]:
            bad += 1
            continue
        vid_off = struct.unpack(">I", ec[16:20])[0]
        vid = peb[vid_off:vid_off + 64]
        if vid[:4] == b"\xff\xff\xff\xff":
            erased += 1          # formatted but unmapped
        elif vid[:4] == b"UBI!" and ubi_crc(vid[:60]) == struct.unpack(">I", vid[60:64])[0]:
            used += 1
        else:
            bad += 1
    good = bad == 0 and used > 0
    return good, f"{pebs} blocks: {used} in use, {erased} free, {marked} marked bad, {bad} bad headers"


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    img = open(sys.argv[1], "rb").read()
    upt = sys.argv[2]
    ok = True

    print(f"flash.img: {len(img):,} bytes, sha256 {hashlib.sha256(img).hexdigest()[:16]}...")
    if len(img) != FLASH_SIZE:
        print(f"  FAIL: expected {FLASH_SIZE:,} bytes")
        ok = False

    print("\nPartition       offset      size      erased blocks")
    for name, off, size in PARTS:
        part = img[off:off + size]
        print(f"  {name:<10} {off:#10x} {size // 1024:>8} KiB   {erased_blocks(part)}/{size // BLOCK}")

    uboot = from_upt(upt, "uboot.bin")
    kernel = from_upt(upt, "uimage.bin")

    # The bootloader area may already hold a different first-stage loader at
    # offset 0 if a third-party bootloader was installed, so report rather than fail.
    same = img[:len(uboot)] == uboot
    print(f"\nU-Boot region matches the .upt's uboot.bin: {'yes' if same else 'no (another bootloader is installed, e.g. Rockbox)'}")

    for name, off in (("kernel", 0x300000), ("recovery", 0x900000)):
        good, desc = check_uimage(img[off:off + 0x700000])
        print(f"{name} uImage: {desc}")
        ok &= good
    same = img[0x300000:0x300000 + len(kernel)] == kernel
    print(f"kernel is the same build as the .upt: {'yes' if same else 'no (different firmware version installed)'}")

    for name, off, size in PARTS[4:]:
        good, desc = check_ubi(img[off:off + size])
        print(f"{name} UBI: {desc}")
        ok &= good
    print("\nResult:", "the dump looks complete and consistent" if ok else "problems found (see above)")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
