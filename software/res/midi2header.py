#!/usr/bin/env python3
"""
midi2header.py - embed Standard MIDI Files into the AcidMIDI applet.

The AcidMIDI applet parses .mid files itself (from the SD card on ORN8 /
Teensy 4.1, or from flash). This script turns one or more .mid files into a
C header of byte arrays that gets compiled into the firmware, so the files
are available even without an SD card.

Usage (from the software/ directory):

    python3 res/midi2header.py res/acid_midi/*.mid -o src/applets/AcidMIDIData.h

Each file becomes one entry in the applet's file list, named after the file
(up to 8 characters, without extension). The script validates each file
(MThd header, PPQN timing) and lists the note-carrying parts it finds, so you
know which "Part" numbers to dial in. Only standard library Python is needed.

Multi-instrument files: every track/channel with notes is one Part. When an
arrangement hands a role from one instrument to another halfway through (say
the melody moves from guitar to koto), you can merge parts into voices:

    python3 res/midi2header.py "song.mid@1+7,3+4,8+9:Song3V" ...

This embeds a rebuilt file whose Part 1 = parts 1+7, Part 2 = parts 3+4 and
Part 3 = parts 8+9 of the original (numbers as listed by this script), named
"Song3V". Merged voices are monophonic on the module: where two source parts
play at once, the louder (then higher) note wins on each step.
"""

import argparse
import os
import re
import struct
import sys

MAX_NAME = 8
MAX_PARTS = 16


def read_vlq(data, i):
    v = 0
    for _ in range(4):
        b = data[i]
        i += 1
        v = (v << 7) | (b & 0x7F)
        if not b & 0x80:
            break
    return v, i


def write_vlq(v):
    out = [v & 0x7F]
    v >>= 7
    while v:
        out.append(0x80 | (v & 0x7F))
        v >>= 7
    return bytes(reversed(out))


def read_file(data):
    """Return (format, division, tracks); each track is a list of
    (abs_tick, status, d1, d2) channel voice events."""
    if data[:4] != b"MThd":
        raise ValueError("not a Standard MIDI File (missing MThd)")
    hlen = struct.unpack(">I", data[4:8])[0]
    fmt, ntracks, division = struct.unpack(">HHH", data[8:14])
    if division & 0x8000:
        raise ValueError("SMPTE time division is not supported, export with PPQN timing")
    i = 8 + hlen
    tracks = []
    while i + 8 <= len(data) and len(tracks) < ntracks:
        cid = data[i:i + 4]
        clen = struct.unpack(">I", data[i + 4:i + 8])[0]
        body = data[i + 8:i + 8 + clen]
        i += 8 + clen
        if cid != b"MTrk":
            continue
        events = []
        j = 0
        tick = 0
        running = 0
        while j < len(body):
            delta, j = read_vlq(body, j)
            tick += delta
            if j >= len(body):
                break
            status = body[j]
            if status < 0x80:
                if not running:
                    break
                status = running
            else:
                j += 1
            if status == 0xFF:
                mtype = body[j]
                mlen, j = read_vlq(body, j + 1)
                j += mlen
                if mtype == 0x2F:
                    break
                continue
            if status in (0xF0, 0xF7):
                slen, j = read_vlq(body, j)
                j += slen
                continue
            if status >= 0xF0:
                break
            running = status
            hi = status & 0xF0
            n = 1 if hi in (0xC0, 0xD0) else 2
            d = body[j:j + n]
            j += n
            if len(d) < n:
                break
            events.append((tick, status, d[0], d[1] if n == 2 else 0))
        tracks.append(events)
    return fmt, division, tracks


def list_parts(tracks):
    """Parts in the same order as the applet: (track, channel) pairs in order
    of their first note-on. Returns [(track, channel, note_count)]."""
    counts = {}
    order = []
    for t, events in enumerate(tracks):
        for (_, status, _, d2) in events:
            if status & 0xF0 == 0x90 and d2 > 0:
                key = (t, status & 0x0F)
                if key not in counts:
                    counts[key] = 0
                    order.append(key)
                counts[key] += 1
    return [(k[0], k[1], counts[k]) for k in order[:MAX_PARTS]]


def merge_parts(division, tracks, parts, groups):
    """Build a format 1 file with one track per group of source parts."""
    out_tracks = []
    for v, group in enumerate(groups):
        events = []
        for p in group:
            if p < 1 or p > len(parts):
                sys.exit(f"part {p} doesn't exist (file has {len(parts)} parts)")
            trk, ch, _ = parts[p - 1]
            for (tick, status, d1, d2) in tracks[trk]:
                hi = status & 0xF0
                if (status & 0x0F) == ch and hi in (0x80, 0x90):
                    # rewrite to this voice's channel; note-offs sort first
                    is_on = hi == 0x90 and d2 > 0
                    events.append((tick, 1 if is_on else 0, (0x90 if is_on else 0x80) | (v & 0x0F), d1, d2))
        events.sort(key=lambda e: (e[0], e[1]))
        body = bytearray()
        name = f"Voice {v + 1}".encode()
        body += b"\x00\xff\x03" + write_vlq(len(name)) + name
        last = 0
        for (tick, _, status, d1, d2) in events:
            body += write_vlq(tick - last) + bytes([status, d1, d2])
            last = tick
        body += b"\x00\xff\x2f\x00"
        out_tracks.append(bytes(body))
    data = b"MThd" + struct.pack(">IHHH", 6, 1, len(out_tracks), division)
    for body in out_tracks:
        data += b"MTrk" + struct.pack(">I", len(body)) + body
    return data


def c_ident(name):
    ident = re.sub(r"[^0-9A-Za-z_]", "_", name)
    if ident[0].isdigit():
        ident = "_" + ident
    return ident


def parse_spec(spec):
    """'path[@1+7,3+4][:Name]' -> (path, groups or None, name or None)"""
    name = None
    groups = None
    m = re.match(r"^(.*?\.midi?)(@[0-9+,]+)?(:[^:/\\]+)?$", spec, re.IGNORECASE)
    if not m:
        return spec, None, None
    path = m.group(1)
    if m.group(2):
        groups = [[int(x) for x in g.split("+") if x] for g in m.group(2)[1:].split(",") if g]
    if m.group(3):
        name = m.group(3)[1:]
    return path, groups, name


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("files", nargs="+", help=".mid files to embed, optionally file.mid@1+7,3+4:Name")
    ap.add_argument("-o", "--output", default="src/applets/AcidMIDIData.h")
    args = ap.parse_args()

    entries = []
    for spec in args.files:
        path, groups, name = parse_spec(spec)
        with open(path, "rb") as f:
            data = f.read()
        try:
            fmt, division, tracks = read_file(data)
        except (ValueError, IndexError, struct.error) as e:
            sys.exit(f"{path}: {e}")
        parts = list_parts(tracks)
        if not parts:
            sys.exit(f"{path}: no notes found")
        print(f"{path}: format {fmt}, {division} PPQN, {len(data)} bytes")
        for idx, (trk, ch, count) in enumerate(parts):
            print(f"  Part {idx + 1}: track {trk + 1}, channel {ch + 1}, {count} notes")
        if groups:
            data = merge_parts(division, tracks, parts, groups)
            print(f"  merged into {len(groups)} voices: " +
                  ", ".join(f"Part {v + 1} = " + "+".join(map(str, g)) for v, g in enumerate(groups)))
        if not name:
            name = os.path.splitext(os.path.basename(path))[0]
        name = name[:MAX_NAME]
        entries.append((name, data))

    out = []
    out.append("// Generated by res/midi2header.py - do not edit by hand.")
    out.append("// Embedded Standard MIDI Files for the AcidMIDI applet.")
    out.append("#pragma once")
    out.append("")
    out.append("#include <stdint.h>")
    out.append("#include <stddef.h>")
    out.append("")
    out.append("namespace AcidMidiData {")
    out.append("")
    for n, (name, data) in enumerate(entries):
        out.append(f"// {name}: {len(data)} bytes")
        out.append(f"static const uint8_t PROGMEM file{n}_{c_ident(name)}[] = {{")
        for k in range(0, len(data), 16):
            out.append("  " + ", ".join(f"0x{b:02x}" for b in data[k:k + 16]) + ",")
        out.append("};")
        out.append("")
    out.append("struct EmbeddedFile {")
    out.append("  const char *name;")
    out.append("  const uint8_t *data;")
    out.append("  size_t size;")
    out.append("};")
    out.append("")
    out.append("static const EmbeddedFile files[] = {")
    for n, (name, data) in enumerate(entries):
        ident = f"file{n}_{c_ident(name)}"
        out.append(f'  {{ "{name}", {ident}, sizeof({ident}) }},')
    out.append("};")
    out.append("static constexpr int file_count = sizeof(files) / sizeof(files[0]);")
    out.append("")
    out.append("} // namespace AcidMidiData")
    out.append("")

    with open(args.output, "w") as f:
        f.write("\n".join(out))
    print(f"wrote {args.output} ({len(entries)} files)")


if __name__ == "__main__":
    main()
