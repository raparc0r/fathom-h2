#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional terms under GPLv3 section 7: see LICENSE.
"""Copy the H2's Bluetooth and Wi-Fi firmware out of a backup of its flash.

    python3 extract-firmware.py flash.img [OUT]          Bluetooth: OUT (BCM43430A1.hcd)
    python3 extract-firmware.py --wifi flash.img [DIR]   Wi-Fi: DIR/brcmfmac43430-sdio.bin, .txt
    python3 extract-firmware.py --all flash.img DIR      both, in DIR (the card's fathom/firmware/brcm)

flash.img is the backup from step 1 of the install guide (Rockbox's "Dump entire
flash"). The files are in the stock system, partition 4 (UBI, volume rootfs,
UBIFS): the Bluetooth patch BCM4343A1_001.002.009.0122.0538.hcd, and for Wi-Fi
the chip's firmware cyw43438-7.46.58.35.bin and the board's calibration
nvram_azw372.txt. On the card they go in fathom/firmware/brcm/ as
BCM43430A1.hcd, brcmfmac43430-sdio.bin and brcmfmac43430-sdio.txt.

Needs only Python 3: it reads UBIFS and decompresses LZO by itself. It only reads
flash.img.
"""
import struct
import sys
import zlib

NAME = b"BCM4343A1_001.002.009.0122.0538.hcd"
ROOTFS = (0x1000000, 0x4000000)      # mtd4: offset, size
MAGIC = b"\x31\x18\x10\x06"          # UBIFS node magic, little endian
BLOCK = 4096                         # UBIFS data block


def fail(msg):
    sys.exit("extract-firmware: " + msg)


def lzo1x_decompress(src, out_len):
    """LZO1X, as in Linux's lib/lzo/lzo1x_decompress_safe.c (the format notes are
    in Documentation/staging/lzo.rst)."""
    out = bytearray()
    ip = 0
    n = len(src)

    def length(base, mask_bits):
        nonlocal ip
        if mask_bits:
            return base + mask_bits
        extra = 0
        while src[ip] == 0:
            extra += 255
            ip += 1
        extra += src[ip]
        ip += 1
        return base + extra

    def literals(k):
        nonlocal ip
        if ip + k > n:
            raise ValueError("literal run past the input")
        out.extend(src[ip:ip + k])
        ip += k

    def match(dist, k):
        start = len(out) - dist
        if start < 0:
            raise ValueError("match before the start")
        for i in range(k):
            out.append(out[start + i])

    state = 0
    t = src[0]
    if t > 17:
        ip = 1
        literals(t - 17)
        state = 4 if t - 17 >= 4 else t - 17
    while ip < n:
        t = src[ip]
        ip += 1
        if t < 16:
            if state == 0:
                literals(length(3, t & 15) if t & 15 else length(18, 0))
                state = 4
                continue
            h = src[ip]
            ip += 1
            if state < 4:
                match((h << 2) + ((t >> 2) & 3) + 1, 2)
            else:
                match((h << 2) + ((t >> 2) & 3) + 2049, 3)
            s = t & 3
        elif t < 32:
            k = length(2, t & 7) if t & 7 else length(9, 0)
            d = src[ip] | src[ip + 1] << 8
            ip += 2
            dist = 16384 + ((t >> 3) & 1) * 16384 + (d >> 2)
            if dist == 16384:
                break                                   # end of stream
            match(dist, k)
            s = d & 3
        elif t < 64:
            k = length(2, t & 31) if t & 31 else length(33, 0)
            d = src[ip] | src[ip + 1] << 8
            ip += 2
            match((d >> 2) + 1, k)
            s = d & 3
        else:
            h = src[ip]
            ip += 1
            dist = (h << 3) + ((t >> 2) & 7) + 1
            match(dist, (3 + ((t >> 5) & 1)) if t < 128 else (5 + ((t >> 5) & 3)))
            s = t & 3
        if s:
            literals(s)
        state = s
    if len(out) != out_len:
        raise ValueError("decompressed %d bytes, expected %d" % (len(out), out_len))
    return bytes(out)


def nodes(fs):
    """Every UBIFS node with a good CRC: (sqnum, type, bytes)."""
    i = 0
    while True:
        i = fs.find(MAGIC, i)
        if i < 0:
            return
        if i % 8 == 0 and i + 24 <= len(fs):
            crc, sqnum, ln = struct.unpack_from("<IQI", fs, i + 4)
            if 24 <= ln <= 4096 + 64 and i + ln <= len(fs):
                body = fs[i + 8:i + ln]
                if zlib.crc32(body) ^ 0xFFFFFFFF == crc:
                    yield sqnum, fs[i + 20], fs[i:i + ln]
                    i += (ln + 7) & ~7
                    continue
        i += 4


def check_hcd(data):
    """An .hcd is a chain of HCI vendor commands: opcode (LE16), length, payload."""
    i = 0
    while i < len(data):
        if i + 3 > len(data):
            return False
        i += 3 + data[i + 2]
    return i == len(data) and data[1] == 0xFC


def read_stock(fs):
    """The stock system's files by name: name -> (inode, size); and its data blocks."""
    dents, inodes, blocks = {}, {}, {}
    for sqnum, typ, nd in nodes(fs):
        if typ == 2:                                    # directory entry
            inum = struct.unpack_from("<Q", nd, 40)[0]
            nlen = struct.unpack_from("<H", nd, 50)[0]
            name = nd[56:56 + nlen]
            if sqnum > dents.get(name, (-1, 0))[0]:
                dents[name] = (sqnum, inum)
        elif typ == 0:                                  # inode
            ino = struct.unpack_from("<I", nd, 24)[0]
            size = struct.unpack_from("<Q", nd, 48)[0]
            if sqnum > inodes.get(ino, (-1, 0))[0]:
                inodes[ino] = (sqnum, size)
        elif typ == 1:                                  # data
            ino, kb = struct.unpack_from("<II", nd, 24)
            if sqnum > blocks.get((ino, kb & 0x1FFFFFFF), (-1,))[0]:
                blocks[(ino, kb & 0x1FFFFFFF)] = (sqnum, nd)
    return dents, inodes, blocks


def extract(stock, name):
    dents, inodes, blocks = stock
    if name not in dents:
        fail("no %s in the stock system" % name.decode())
    inum = dents[name][1]
    if inum not in inodes:
        fail("%s: its inode is missing" % name.decode())
    size = inodes[inum][1]
    data = bytearray()
    for b in range((size + BLOCK - 1) // BLOCK):
        if (inum, b) not in blocks:
            fail("block %d of %s is missing" % (b, name.decode()))
        nd = blocks[(inum, b)][1]
        usize, compr = struct.unpack_from("<IH", nd, 40)
        payload = nd[48:]
        if compr == 0:
            chunk = payload[:usize]
        elif compr == 1:
            chunk = lzo1x_decompress(payload, usize)
        elif compr == 2:
            chunk = zlib.decompress(payload, -15)
        else:
            fail("block %d uses compression %d, which this script doesn't read" % (b, compr))
        data += chunk
    return bytes(data[:size])


WIFI_FW = b"cyw43438-7.46.58.35.bin"
WIFI_NVRAM = b"nvram_azw372.txt"


def check_wifi_fw(data):
    return len(data) > 200000 and b"43430a1-roml" in data


def check_nvram(data):
    return b"manfid=0x2d0" in data and b"boardtype=" in data


def write(path, data):
    import hashlib
    with open(path, "wb") as f:
        f.write(data)
    print("wrote %s (%d bytes, sha256 %s)" % (path, len(data), hashlib.sha256(data).hexdigest()))


def main():
    import os
    args = sys.argv[1:]
    mode = "bt"
    if args and args[0] in ("--wifi", "--all"):
        mode = args.pop(0)[2:]
    if len(args) not in (1, 2) or (mode == "all" and len(args) != 2):
        print("\n".join(l.strip() for l in __doc__.strip().splitlines()[2:5]), file=sys.stderr)
        sys.exit(2)
    with open(args[0], "rb") as f:
        img = f.read()
    if len(img) < ROOTFS[0] + ROOTFS[1]:
        fail("%s is %d bytes: not a whole-flash backup (128 MiB)" % (args[0], len(img)))
    fs = img[ROOTFS[0]:ROOTFS[0] + ROOTFS[1]]
    if fs[:4] != b"UBI#":
        fail("no UBI at partition 4: is this an H2 flash backup?")
    stock = read_stock(fs)
    out = {}
    if mode in ("bt", "all"):
        hcd = extract(stock, NAME)
        if not check_hcd(hcd):
            fail("the Bluetooth patch doesn't look right; nothing written")
        out["BCM43430A1.hcd"] = hcd
    if mode in ("wifi", "all"):
        fw, nv = extract(stock, WIFI_FW), extract(stock, WIFI_NVRAM)
        if not check_wifi_fw(fw) or not check_nvram(nv):
            fail("the Wi-Fi firmware doesn't look right; nothing written")
        out["brcmfmac43430-sdio.bin"] = fw
        out["brcmfmac43430-sdio.txt"] = nv
    if mode == "bt":
        write(args[1] if len(args) == 2 else "BCM43430A1.hcd", out["BCM43430A1.hcd"])
        print("copy it to the card as fathom/firmware/brcm/BCM43430A1.hcd")
        return
    d = args[1] if len(args) == 2 else "."
    os.makedirs(d, exist_ok=True)
    for n, data in out.items():
        write(os.path.join(d, n), data)
    print("copy %s to the card's fathom/firmware/brcm/" % ("them" if len(out) > 1 else "it"))


if __name__ == "__main__":
    main()
