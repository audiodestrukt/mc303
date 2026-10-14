#!/usr/bin/env python3
"""Patch the MC-303 OS 1.03 ROM so that holding TAP at power-on streams the 4 MB wave ROM (IC3)
out of MIDI OUT as one SysEx message.

The wave ROM sits on the PCM chip's private bus, so the CPU cannot read it directly, but the
TC6116AF has a CPU read-back path (the SC-55 firmware uses it for its boot checksum): write the
24-bit address to PCM registers 0x21/0x22/0x23 (bank 0 addresses E042/E044/E046 on the MC-303,
register = address >> 1), then read register 0x3F (E07E). The patch does that for every byte and
sends each as two nibbles (0x0h 0x0l) inside  F0 7D 4D 43 ... F7  ("MC").

Hook: the power-on key checks at FFF02D-FFF0C0 (R(BD)+ARPEGGIO = test mode, FUNC/SHIFT = factory
preset, PLAY MODE = demo). The PLAY MODE check is retargeted to TAP (switch id 0x9E) and its demo
set-up replaced by JMPP to the dump routine, which lives in the empty 16 KB at FE0000. After the dump
(about 45 minutes at MIDI speed) interrupts are re-enabled and the boot continues.

usage: make_wavedump_rom.py OS_ROM.bin OUT.bin
"""
import sys, subprocess, os

HOOK = 0xFFF0AB
ROUTINE = 0xFE0000
BASE = 0xF80000

ORIG_HOOK = bytes.fromhex("42 44 4C 65 F8 74 FA 17 02 15 03 F0 09 D1 53 0E 8C 42 16 53 F2 8A")
#                           MOV A,#44 PUSHW CALLP FA74F8 ADDSP ZEXT NEG BZ   demo set-up (9 bytes)
NEW_HOOK = bytes.fromhex("42 9E 4C 65 F8 74 FA 17 02 15 03 F0 09") + bytes.fromhex("63 00 00 FE") + b"\x00" * 5
assert len(NEW_HOOK) == len(ORIG_HOOK)

def routine():
    code = bytearray()
    labels = {}
    fix = []   # (offset of rel byte, label, instruction length)
    def emit(hexs): code.extend(bytes.fromhex(hexs))
    def send():    emit("64"); fix.append((len(code), "SEND", "abs16")); code.extend(b"\0\0")
    def bnz(l):    emit("F1"); fix.append((len(code), l, "rel8")); code.append(0)
    emit("1A 00")             # MOV ILM,#0        mask every interrupt (display scan, MIDI, timers)
    emit("42 00 6F 11")       # MOV A,#0 ; MOV ADB,A   bank 0 for the PCM chip window
    for b in (0xF0, 0x7D, 0x4D, 0x43):
        emit("42 %02X" % b); send()
    emit("A9 00 00")          # MOVW RW1,#0       address bits 23..16
    labels["L_HI"] = len(code)
    emit("89 06 53 42 E0")    # MOVW A,RW1 ; MOV ADB:E042,A
    emit("A8 00 00")          # MOVW RW0,#0       address bits 15..0
    labels["L_LO"] = len(code)
    emit("88 16 06 53 44 E0") # MOVW A,RW0 ; SWAP ; MOV ADB:E044,A      bits 15..8
    emit("88 06 53 46 E0")    # MOVW A,RW0 ; MOV ADB:E046,A             bits 7..0 (latches the byte)
    emit("A4 FF 74 E4 FD")    # MOV R4,#255 ; DBNZ R4,$   ~100 us for the chip to fetch
    emit("06 52 7E E0")       # MOV A,ADB:E07E    the byte (zero-extended into AL)
    emit("4C 0E 0E 0E 0E"); send()          # PUSHW A ; ASRW A x4 ; high nibble
    emit("5C 34 0F"); send()                # POPW A ; AND A,#0F ; low nibble
    emit("73 40 88 3B 00 00"); bnz("L_LO")  # INCW RW0 ; MOVW A,RW0 ; CMPW A,#0 ; BNZ   (65536 bytes)
    emit("73 41 89 3B 40 00"); bnz("L_HI")  # INCW RW1 ; MOVW A,RW1 ; CMPW A,#40 ; BNZ (64 x 64 KB = 4 MB)
    emit("42 F7"); send()                   # end of SysEx
    emit("1A 07")             # MOV ILM,#7        interrupts back
    emit("63 C1 F0 FF")       # JMPP FFF0C1       continue the boot
    labels["SEND"] = len(code)
    emit("6C 83 27 FC")       # BBC I:0x27:3,$    wait for TDRE
    emit("51 26 67")          # MOV I:0x26,A ; RET
    for off, lab, kind in fix:
        if kind == "abs16":
            a = (ROUTINE + labels[lab]) & 0xFFFF
            code[off] = a & 0xFF; code[off + 1] = a >> 8
        else:
            rel = labels[lab] - (off + 1)
            assert -128 <= rel < 128
            code[off] = rel & 0xFF
    return bytes(code)

def main():
    if len(sys.argv) != 3:
        print(__doc__); sys.exit(1)
    rom = bytearray(open(sys.argv[1], "rb").read())
    assert len(rom) == 0x80000, "expected a 512 KB OS ROM"
    h = HOOK - BASE
    assert rom[h:h + len(ORIG_HOOK)] == ORIG_HOOK, "hook bytes differ: not OS 1.03?"
    r = ROUTINE - BASE
    code = routine()
    assert rom[r:r + len(code)] == b"\xff" * len(code), "routine area is not empty"
    rom[h:h + len(NEW_HOOK)] = NEW_HOOK
    rom[r:r + len(code)] = code
    open(sys.argv[2], "wb").write(rom)
    print("patched: hook at %06X, routine at %06X (%d bytes) -> %s" % (HOOK, ROUTINE, len(code), sys.argv[2]))
    dasm = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "f2mc16dasm", "f2mc16dasm")
    if os.path.exists(dasm):
        print(subprocess.run([dasm, sys.argv[2], "F80000", "%X" % ROUTINE, "%X" % (ROUTINE + len(code))], capture_output=True, text=True).stdout)
        print(subprocess.run([dasm, sys.argv[2], "F80000", "%X" % HOOK, "%X" % (HOOK + len(NEW_HOOK))], capture_output=True, text=True).stdout)

if __name__ == "__main__":
    main()
