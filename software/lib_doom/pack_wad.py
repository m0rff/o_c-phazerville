#!/usr/bin/env python3
"""Pack a Doom IWAD for the DOOM app's built-in flash storage (Teensy 4.1).

The WAD is compressed in 32 KB chunks (raw DEFLATE) and written as an Intel
HEX file at 0x60400000, above the four 1 MB boot slots and below LittleFS.
The multiboot build (res/assemble-multiboot.py) merges it automatically when
a .wad file is in software/lib_doom/.

Mind the licence of the WAD: id's shareware doom1.wad may only be passed on
unmodified, so keep firmware with it built in to yourself.

usage: pack_wad.py <in.wad> <out.hex> [--bin out.bin]
"""
import os
import struct
import sys
import zlib

BASE = 0x60400000
LIMIT = 0x60740000          # LittleFS (512 KB) starts here on a Teensy 4.1
CHUNK = 32768


def pack(data, name):
    chunks = []
    for i in range(0, len(data), CHUNK):
        c = zlib.compressobj(9, zlib.DEFLATED, -15)
        chunks.append(c.compress(data[i:i + CHUNK]) + c.flush())
    header_len = 8 + 16 + 12 + 4 * (len(chunks) + 1)
    offsets, pos = [], header_len
    for c in chunks:
        offsets.append(pos)
        pos += len(c)
    offsets.append(pos)
    name_b = name.lower().encode()[:15]
    blob = b"OCWAD1\0\0" + name_b.ljust(16, b"\0")
    blob += struct.pack("<III", len(data), CHUNK, len(chunks))
    blob += struct.pack("<%dI" % len(offsets), *offsets)
    blob += b"".join(chunks)
    return blob


def to_ihex(blob, base):
    lines, upper = [], None
    for off in range(0, len(blob), 16):
        addr = base + off
        if addr >> 16 != upper:
            upper = addr >> 16
            rec = struct.pack(">BHBH", 2, 0, 4, upper)
            lines.append(":" + rec.hex().upper() + "%02X" % ((-sum(rec)) & 0xFF))
        part = blob[off:off + 16]
        rec = struct.pack(">BHB", len(part), addr & 0xFFFF, 0) + part
        lines.append(":" + rec.hex().upper() + "%02X" % ((-sum(rec)) & 0xFF))
    lines.append(":00000001FF")
    return "\n".join(lines) + "\n"


def main():
    args = sys.argv[1:]
    if len(args) < 2:
        sys.exit(__doc__)
    src, out = args[0], args[1]
    data = open(src, "rb").read()
    if data[:4] not in (b"IWAD", b"PWAD"):
        sys.exit("%s is not a WAD file" % src)
    blob = pack(data, os.path.basename(src))
    if BASE + len(blob) > LIMIT:
        sys.exit("%s packs to %d bytes, more than the %d bytes of free flash"
                 % (src, len(blob), LIMIT - BASE))
    with open(out, "w") as f:
        f.write(to_ihex(blob, BASE))
    if "--bin" in args:
        open(args[args.index("--bin") + 1], "wb").write(blob)
    print("pack_wad: %s %d -> %d bytes at 0x%08X" % (os.path.basename(src), len(data), len(blob), BASE))


if __name__ == "__main__":
    main()
