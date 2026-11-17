#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""
dac-probe (macOS): record what a USB DAC supports, before we ever plug it into the H2.

    python3 tools/dac-probe/dac_probe.py [-o NAME.md]

For every USB Audio device it reports:
  * USB identity: VID:PID, names, USB version, declared max bus power (mA)
  * USB Audio Class version, every playback alt setting with its format,
    subslot size / bit resolution, and whether a RAW_DATA (native DSD) format is offered
  * Feature-unit controls (hardware volume / mute present?)
  * From CoreAudio: the sample rates and bit depths macOS negotiated

Requires: pyusb (pip install -r tools/dac-probe/requirements.txt) and libusb (brew install libusb).
Only standard descriptor reads are made; nothing is written to the device.
"""
import argparse
import ctypes
import ctypes.util
import struct
import sys

try:
    import usb.backend.libusb1
    import usb.core
    import usb.util
except ImportError:
    sys.exit("pyusb is missing: pip install -r tools/dac-probe/requirements.txt")

import os

# Homebrew's libusb is not on the default dyld search path on Apple Silicon.
for _candidate in ("/opt/homebrew/lib/libusb-1.0.dylib", "/usr/local/lib/libusb-1.0.dylib"):
    if os.path.exists(_candidate):
        BACKEND = usb.backend.libusb1.get_backend(find_library=lambda _name, p=_candidate: p)
        break
else:
    BACKEND = None

AUDIO = 0x01
SUB_CONTROL, SUB_STREAMING = 0x01, 0x02
CS_INTERFACE = 0x24
UAC2_FORMAT_BITS = {0: "PCM", 1: "PCM8", 2: "IEEE_FLOAT", 3: "ALAW", 4: "MULAW", 31: "RAW_DATA (native DSD)"}


# ------------------------------------------------------------------ USB descriptors

def walk(raw):
    i = 0
    while i + 2 <= len(raw):
        n = raw[i]
        if n < 2:
            break
        yield raw[i:i + n]
        i += n


def parse_audio(dev):
    raw = bytes(dev.ctrl_transfer(0x80, 6, 0x0200, 0, 9))           # config header
    total = struct.unpack_from("<H", raw, 2)[0]
    raw = bytes(dev.ctrl_transfer(0x80, 6, 0x0200, 0, total))
    max_power_ma = raw[8] * (8 if dev.bcdUSB >= 0x0300 else 2)

    uac = None
    alts, features = [], []
    cur = None
    for d in walk(raw):
        dtype = d[1]
        if dtype == 0x04:                                              # interface
            cls, sub, proto = d[5], d[6], d[7]
            cur = {"if": d[2], "alt": d[3], "eps": d[4], "class": cls, "sub": sub}
            if cls == AUDIO and sub == SUB_CONTROL:
                uac = "2.0" if proto == 0x20 else "1.0"
            if cls == AUDIO and sub == SUB_STREAMING and d[4] > 0:
                cur["formats"] = []
                alts.append(cur)
        elif dtype == CS_INTERFACE and cur is not None and cur["class"] == AUDIO:
            st = d[2]
            if cur["sub"] == SUB_CONTROL and st == 0x06:               # feature unit
                features.append(d)
            if cur["sub"] == SUB_STREAMING and "formats" in cur:
                if st == 0x01 and uac == "2.0" and len(d) >= 16:      # AS_GENERAL (UAC2)
                    bm = struct.unpack_from("<I", d, 6)[0]
                    cur["formats"] = [UAC2_FORMAT_BITS.get(b, f"bit{b}") for b in range(32) if bm >> b & 1]
                    cur["channels"] = d[10]
                elif st == 0x01 and uac == "1.0" and len(d) >= 7:
                    cur["formats"] = [{1: "PCM", 2: "PCM8", 3: "IEEE_FLOAT"}.get(struct.unpack_from("<H", d, 5)[0], "other")]
                elif st == 0x02:                                       # FORMAT_TYPE
                    if uac == "2.0" and len(d) >= 6:
                        cur["subslot"], cur["bits"] = d[4], d[5]
                    elif len(d) >= 8:
                        cur["channels"], cur["subslot"], cur["bits"] = d[4], d[5], d[6]
                        n = d[7]
                        if n:
                            cur["rates"] = [int.from_bytes(d[8 + 3 * k:11 + 3 * k], "little") for k in range(n)]
        elif dtype == 0x05 and cur is not None and "formats" in cur:   # endpoint
            if d[2] & 0x80 == 0 and d[3] & 0x03 == 1:                 # isochronous OUT
                cur["out"] = True
                sync = (d[3] >> 2) & 3
                cur["sync"] = {0: "none", 1: "async", 2: "adaptive", 3: "sync"}[sync]

    vol = mute = False
    for f in features:
        if uac == "2.0":
            # bmaControls: 4 bytes per channel from offset 5; mute bits 0-1, volume bits 2-3
            for off in range(5, len(f) - 1, 4):
                if off + 4 > len(f):
                    break
                bm = struct.unpack_from("<I", f, off)[0]
                mute |= bool(bm & 0x3)
                vol |= bool(bm & 0xC)
        else:
            size = f[5] if len(f) > 5 else 0
            for off in range(6, len(f) - 1, max(size, 1)):
                bm = int.from_bytes(f[off:off + size], "little") if size else 0
                mute |= bool(bm & 0x1)
                vol |= bool(bm & 0x2)
    playback = [a for a in alts if a.get("out")]
    return {"uac": uac, "max_power_ma": max_power_ma, "alts": playback, "volume": vol, "mute": mute}


def string(dev, idx):
    try:
        return usb.util.get_string(dev, idx) if idx else ""
    except Exception:
        return ""


# ------------------------------------------------------------------ CoreAudio

def fourcc(s):
    return struct.unpack(">I", s.encode())[0]


class Addr(ctypes.Structure):
    _fields_ = [("sel", ctypes.c_uint32), ("scope", ctypes.c_uint32), ("elem", ctypes.c_uint32)]


class ASBD(ctypes.Structure):
    _fields_ = [("rate", ctypes.c_double), ("fmt", ctypes.c_uint32), ("flags", ctypes.c_uint32),
                ("bpp", ctypes.c_uint32), ("fpp", ctypes.c_uint32), ("bpf", ctypes.c_uint32),
                ("ch", ctypes.c_uint32), ("bits", ctypes.c_uint32), ("res", ctypes.c_uint32)]


def coreaudio_devices():
    ca = ctypes.CDLL(ctypes.util.find_library("CoreAudio"))
    cf = ctypes.CDLL(ctypes.util.find_library("CoreFoundation"))
    cf.CFStringGetCString.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_long, ctypes.c_uint32]
    GLOBAL, OUTPUT = fourcc("glob"), fourcc("outp")

    def get(obj, sel, scope, ctype_):
        a = Addr(fourcc(sel), scope, 0)
        size = ctypes.c_uint32(0)
        if ca.AudioObjectGetPropertyDataSize(obj, ctypes.byref(a), 0, None, ctypes.byref(size)):
            return []
        n = size.value // ctypes.sizeof(ctype_)
        buf = (ctype_ * max(n, 1))()
        if ca.AudioObjectGetPropertyData(obj, ctypes.byref(a), 0, None, ctypes.byref(size), buf):
            return []
        return list(buf)[:n]

    def name(obj):
        ref = get(obj, "lnam", GLOBAL, ctypes.c_void_p)
        if not ref or not ref[0]:
            return ""
        out = ctypes.create_string_buffer(256)
        cf.CFStringGetCString(ref[0], out, 256, 0x08000100)
        return out.value.decode("utf-8", "replace")

    devices = []
    for dev in get(1, "dev#", GLOBAL, ctypes.c_uint32):
        transport = get(dev, "tran", GLOBAL, ctypes.c_uint32)
        if not transport or transport[0] != fourcc("usb "):
            continue
        streams = get(dev, "stm#", OUTPUT, ctypes.c_uint32)
        if not streams:
            continue
        rates = sorted({int(r) for r in get(dev, "nsr#", GLOBAL, ctypes.c_double)[::2]})
        fmts = set()
        for s in streams:
            # 'pft#' (kAudioStreamPropertyPhysicalFormats) returns plain 40-byte ASBDs
            for r in get(s, "pft#", GLOBAL, ASBD):
                if r.fmt == fourcc("lpcm"):
                    fmts.add((r.bits, r.ch, int(r.rate)))
        devices.append({"name": name(dev), "rates": rates,
                        "depths": sorted({b for b, _, _ in fmts}),
                        "channels": sorted({c for _, c, _ in fmts})})
    return devices


# ------------------------------------------------------------------ report

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-o", "--output", help="also write the report to this Markdown file")
    args = ap.parse_args()

    lines = []
    found = 0
    for dev in usb.core.find(find_all=True, backend=BACKEND):
        try:
            info = parse_audio(dev)
        except Exception:
            continue  # descriptors not readable (e.g. a hub or a device macOS won't share)
        if not info["alts"]:
            continue  # not a USB audio playback device
        found += 1
        prod, manu = string(dev, dev.iProduct), string(dev, dev.iManufacturer)
        lines.append(f"## {manu} {prod}".strip())
        lines.append("")
        lines.append(f"- USB ID: `{dev.idVendor:04x}:{dev.idProduct:04x}`, USB {dev.bcdUSB >> 8}.{(dev.bcdUSB >> 4) & 0xF}")
        lines.append(f"- Declared max bus power: **{info['max_power_ma']} mA**")
        lines.append(f"- USB Audio Class: **{info['uac']}**")
        lines.append(f"- Hardware volume: **{'yes' if info['volume'] else 'no'}**, mute: {'yes' if info['mute'] else 'no'}")
        lines.append("")
        lines.append("| Interface/alt | Formats | Channels | Subslot | Bits | Sync | Rates (UAC1 only) |")
        lines.append("|---|---|---|---|---|---|---|")
        for a in info["alts"]:
            rates = ", ".join(str(r) for r in a.get("rates", [])) or "via clock source"
            lines.append(f"| {a['if']}/{a['alt']} | {', '.join(a.get('formats', [])) or '?'} | {a.get('channels', '?')} | "
                         f"{a.get('subslot', '?')} | {a.get('bits', '?')} | {a.get('sync', '?')} | {rates} |")
        lines.append("")

    try:
        ca = coreaudio_devices()
    except Exception as e:
        ca = []
        lines.append(f"(CoreAudio query failed: {e})")
    if ca:
        lines.append("## What macOS negotiated (CoreAudio)")
        lines.append("")
        for d in ca:
            lines.append(f"- **{d['name']}**: rates {', '.join(str(r) for r in d['rates'])} Hz; "
                         f"bit depths {', '.join(str(b) for b in d['depths'])}; channels {', '.join(str(c) for c in d['channels'])}")
        lines.append("")

    if not found:
        lines.insert(0, "No USB audio playback device found. Plug in the DAC and run again.\n")
    report = "\n".join(lines)
    print(report)
    if args.output:
        with open(args.output, "w", encoding="utf-8") as fp:
            fp.write("# USB DAC probe\n\n" + report + "\n")
        print(f"\nwritten to {args.output}")


if __name__ == "__main__":
    main()
