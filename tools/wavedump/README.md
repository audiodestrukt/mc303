# Dumping the wave ROM (IC3) without desoldering it

IC3 (uPD23C32000GX-364, 4 MB, SOP-44) sits on the PCM chip's private bus, so the CPU cannot read it
and a programmer cannot reach it in circuit. But the TC6116AF PCM chip has a read-back path the
SC-55 firmware uses for its boot checksum: write a 24-bit address to PCM registers 0x21-0x23 (bank-0
addresses E042/E044/E046 on the MC-303), then read register 0x3F (E07E) to get the byte.

`make_wavedump_rom.py` patches the OS 1.03 ROM so that holding **TAP at power-on** runs a routine that
reads all 4 MB this way and sends it out of MIDI OUT as one SysEx message (`F0 7D 4D 43`, then two
nibble bytes per ROM byte, then `F7`). The OS EPROM (IC7) is socketed, so the patched image goes on a
replacement EPROM; the original goes back afterwards. `decode_wavedump.py` turns the captured stream
into `waverom.bin`.

The patch was verified in the simulator: with a stand-in wave ROM loaded, the patched OS boots
normally without TAP, and with TAP held it streams the full 4 MB, which decodes to a byte-identical
copy of the image the chip model was given.

## Procedure

1. Build the patched image:

   ```
   python3 tools/wavedump/make_wavedump_rom.py "roms/1.03 [27C4096][37OD][Program OS][IC 7].BIN" roms/mc303_os_1.03_wavedump.bin
   ```

   It checks the hook bytes (OS 1.03 only), prints the disassembly of the routine and of the hook.

2. Burn it to a 27C4096 (512 KB, 16-bit wide, same part the original was read as) and put it in
   the IC7 socket, same orientation as the original. Keep the original EPROM safe.

3. Connect MIDI OUT to a MIDI interface on a computer and start capturing to a file, with a long
   timeout. With ALSA:

   ```
   amidi -l                                  # find the port, e.g. hw:1,0,0
   amidi -p hw:1,0,0 -r wavedump.syx -t 3600
   ```

4. Hold TAP and power the MC-303 on. Keep TAP held until the splash (`MC- 303`) has been showing for
   a second. The display then freezes on one scan column (interrupts are off while dumping), and the
   MIDI OUT activity LED on the interface stays on. The dump takes about 46 minutes at 31250 baud
   (4 MB as 8 M nibble bytes).

5. When it finishes the unit sends `F7`, re-enables interrupts and continues booting to `A01`. Stop
   the capture (`amidi` stops by itself at the timeout or on Ctrl-C) and decode:

   ```
   python3 tools/wavedump/decode_wavedump.py wavedump.syx roms/waverom.bin
   ```

   It reports `decoded 4194304 bytes (complete 4 MB), end marker seen`. Anything else means the
   capture lost bytes; just dump again.

6. Put the original EPROM back. `sim/mc303sim --gui` picks up `roms/waverom.bin` automatically and
   the simulator plays with the real MC-303 sounds.

## Notes

- The routine waits ~100 µs after latching each address before reading the byte, in case the real
  chip serves the CPU read in a later slot than the model does. If the decoded image looks shifted
  or repeats bytes, that delay is the first thing to lengthen (`A4 FF` / `DBNZ` in the script).
- The dump is raw ROM bytes in the PCM chip's address order, which is exactly what the simulator's
  `--waverom` expects (no `--unscramble`). It will not look like the SC-55 ROM images floating around,
  which are in the ROM's pin order.
- Nothing is written to user memory; the patched OS boots and behaves normally without TAP, except
  that the power-on demo (PLAY MODE held) is replaced by the dump.
