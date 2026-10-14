#!/usr/bin/env python3
"""Decode the MC-303 wave ROM dump stream (see make_wavedump_rom.py) into a 4 MB binary.

Input: raw bytes as captured from MIDI IN (e.g. `amidi -p hw:1,0,0 -r dump.syx -t 3000`), or the
simulator's --midiout file. The dump is  F0 7D 4D 43  then two bytes per ROM byte (high nibble, low
nibble, each 0x00-0x0F)  then F7. Real-time bytes (F8-FF) that a MIDI interface may interleave are
ignored.

usage: decode_wavedump.py CAPTURE OUT.bin
"""
import sys

def main():
    if len(sys.argv) != 3:
        print(__doc__); sys.exit(1)
    data = open(sys.argv[1], "rb").read()
    start = data.find(bytes([0xF0, 0x7D, 0x4D, 0x43]))
    if start < 0:
        sys.exit("no dump header (F0 7D 4D 43) found")
    out = bytearray(); hi = None; end = False
    for b in data[start + 4:]:
        if b >= 0xF8:
            continue                     # MIDI real-time, not part of the stream
        if b == 0xF7:
            end = True; break
        if b > 0x0F:
            sys.exit("unexpected byte %02X after %d decoded bytes" % (b, len(out)))
        if hi is None:
            hi = b
        else:
            out.append((hi << 4) | b); hi = None
    print("decoded %d bytes (%s), end marker %s" % (len(out), "complete 4 MB" if len(out) == 0x400000 else "INCOMPLETE", "seen" if end else "missing"))
    open(sys.argv[2], "wb").write(out)

if __name__ == "__main__":
    main()
