# MC-303 simulator

Runs the original MC-303 OS ROM on an emulated MB90705 with the PCM chip modelled by Nuked-SC55's
`pcm.cpp`. Headless for now: the two 3-digit LED displays are decoded to text and the 16x8 LED matrix
is printed.

```
make
./mc303sim -t 3                 # boot for 3 emulated seconds (about 10x real time)
```

Output during the run: every change of the display text, e.g.

```
[  0.001 s] display [8.8.8.] [8.8.8.]      all segments (lamp test)
[  0.169 s] display [MC-] [303]            splash, last digit runs a spinner
[  1.615 s] display [A01] [A01]            pattern mode
```

Options:

| Option | Meaning |
|--------|---------|
| `-t SECONDS` | emulated run time |
| `--press COL BIT [T_ON T_OFF]` | hold a switch-matrix position (see table below), optionally only between two times |
| `--knob N VALUE` | ADC channel N (0-7) value 0-255 (channel 4 = battery, must stay mid-range) |
| `--sram FILE` | load battery-backed SRAM before boot, save it at exit |
| `--trace CYCLE [--task BLOCK]` | instruction trace from a cycle count, optionally only while a given task (flag block address) is current |
| `--bp ADDR` | print A/PS/SP/stack when PC hits ADDR |
| `--wp ADDR` | print writes to an SRAM address (bank A0 etc.) |
| `--dumpw ADDR N` | dump N words at exit |
| `--irq`, `--io`, `--ga` | trace interrupts, I/O register writes, PCM chip accesses |
| `--check` | verify every executed instruction's length against the disassembler (catches core bugs) |
| `--waverom FILE [--unscramble]` | 4 MB wave ROM image for the PCM chip (default `../roms/waverom.bin`); larger files are truncated, `--unscramble` applies Nuked's pin-order permutation |
| `--midi T "90 3C 7F"` | inject MIDI bytes into the UART at time T |
| `--wav FILE` | record the PCM chip output (stereo 16-bit, 33103 Hz) |
| `--gui` | SDL front panel with audio. Computer keyboard plays notes: `Z`-`/` = C3 upwards, `Q`-`P` = C4 upwards (MIDI channel 1), `1`-`8` = drums (channel 10). Esc quits and saves SRAM. `--shot FILE T` saves a screenshot at time T |

## Audio

The PCM chip is clocked at 24 MHz (the CPU crystal) and, with the firmware's 28-slot configuration,
produces one frame every 725 cycles = 33103 Hz, the same rate Nuked uses for the SC-55mk2. The wave
ROM (IC3, uPD23C32000GX-364, 4 MB, SOP-44 on the PCM chip's private bus) has not been dumped yet, so
there is no real sound; with any 4 MB image as a stand-in, a MIDI note-on produces enveloped output,
which proves the path. The IC8 "DATA" EPROM is preset pattern data, not samples.

## Board model (mc303.cpp)

- Memory: OS ROM F80000, data ROM F00000, 256 KB SRAM A00000-A3FFFF, internal RAM 0100-1FFF, I/O 00-FF.
- PCM/gate array: bank 0 `E000-E0FF`, register = address >> 1, forwarded to `PCM_Read/PCM_Write`.
  The PCM chip's IRQ maps to external interrupt 3 (flag `I:0x33` bit 2, vector 25). Wave ROM is all
  zero until dumped, so there is no sound; the boot self-test of the chip's internal RAM passes.
- Interrupt controller: ICR00-15 at `B0-BF`, vector v uses ICR (v-11)/2; highest priority (lowest
  level) pending vector wins; accepted when level < ILM.
- Reload timers (control at 0x40/0x48/0x50/0x58/0x60, reload at +4): CNTE bit1, UF bit2, INTE bit3,
  RELD bit4, prescaler bits 10-11 (2/8/32). Vectors 11, -, 14, 15, 16.
- Period timers at 0x38/0x78: 16-bit period at +4 -> flag +6 bit3 (vectors 29, 31), 8-bit period at
  +2 -> flag +6 bit2 (vector 30). Both behind a /128 prescaler (guess; gives 267 ms active sensing).
- ADC at 0x68-0x6B: start = 0x69 bit5, done flag bit7 (cleared by writing 0), INTE bit3, vector 27,
  1000 cycles per conversion. The panel scan ISR restarts it every step, so the panel refresh rate
  follows CPU load, as on the real unit.
- UART1 at 0x24-0x27 = MIDI. TDRE always set; RDRF from `midi_in`; TX bytes collected in `midi_out`.
- Ports: B = SD0-7 (switch read when DDR B = 0 and PC0 low, LED data otherwise), C bit0 = DIR,
  bits1-4 = scan column. LED column data is latched on the rising edge of DIR.

## Panel decode

Displays: left = columns 7,6,5, right = columns 13,14,15 (one column per digit; bit0..6 = A..G,
bit7 = DP, lit when 0). Discrete LEDs: columns 0-4 and 8-12 (a bit per LED, lit when 0).

Switch matrix positions found so far (`--press COL BIT`), from what the firmware does with them:

| COL BIT | Function |
|---------|----------|
| 4 3 | OCTAVE - (display `OCt -1`) |
| 4 4 | OCTAVE + (`OCt 1`) |
| 4 5 | TRANSPOSE (`tP5 -24`) |
| 8 4 | shows `5ty 1-4` (scale/style) |
| 10 6 | PTN SET (`Pt5 1`) |
| 10 7 | RPS SET (`rP5 1`) |
| 11 1 | shows `8 / 1.1` (tempo/measure?) |
| 2 1, 2 4, 4 0, 4 1, 8 1-3, 10 2-5, 11 2, 12 0-7 | LED-only changes (part select / mutes / mode) |

## Core fixes made to MAME's f2mc16.cpp

- Interrupt acknowledge passed a truncated 8-bit level and wiped ILM when taking an interrupt.
- `CALLP @RWx+disp8` pushed pc+4 for a 3-byte instruction (stack leak).
- Added: `MOVW @AL,AH`; `CALLP @RLx`; `MOVL A,@RWx` / `MOVL @RWx,A`; `MOVEA A,@PC+disp16`;
  generic `JMP @ea` / `CALL @ea`; `MULUW A,addr16`; `ORL A,addr16`; `ADDW addr16,A`;
  `ORW @RWx+disp8,A`; `ANDW/ORW A,addr16`; `OR addr16,A`; `AND @RWx,A`; `XOR A,@RWx+disp16`;
  and generic all-mode versions of the `MOV/MOVW Ri/RWi <-> ea` and `XCHW` prefix families.

## Credits and licence

This project stands on other people's reverse engineering, and the sound engine is not ours at all:

- **[Nuked-SC55](https://github.com/nukeykt/Nuked-SC55)** by **nukeykt** — the TC6116AF PCM chip
  emulation (`sim/pcm.cpp`, used unmodified) that produces every sample this simulator makes. Its
  register interface, sample decoding, envelope, address generator and effects were all recovered from
  the silicon, and it is why the MC-303's firmware talks to a "real" chip here. GPL-2.0-or-later.
- **John McMaster** ([siliconprawn.org](https://siliconprawn.org)) — decapped and imaged the PCM chip
  that Nuked-SC55's model was derived from.
- **MAME** — the Fujitsu F2MC-16 CPU core by **R. Belmont** and the disassembler by **AJR**
  (`sim/f2mc16.cpp`, `sim/f2mc16d.cpp`, BSD-3-Clause), extended here with the instruction forms the
  MC-303 firmware needs and two interrupt/return-address fixes.
- **Roland** — the 1996 MC-303 service notes, whose block diagram and schematics made the board model
  possible.

Because `pcm.cpp` is GPL-2.0-or-later, the simulator as a whole is GPL-2.0-or-later.
