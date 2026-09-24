# Reference notes

## MB90570A_datasheet.pdf (Fujitsu DS07-13701-9E, 2008)

This is the **MB90570A/570C series** (F2MC-16**LX**, 120-pin, internal ROM/flash), *not* the MC-303's
**MB90705** (F2MC-16 original core, 144-pin, ROM-less, with CS0-CS7, DREQ0-3, STWA/STWB wait pins).
None of the MC-303 CPU pin names beyond the generic ports exist on this part, and MAME already
implements this exact chip (`cpu/f2mc16/mb90570.cpp`).

What still transfers (same Fujitsu peripheral IP, checked against the firmware init code):

| I/O addr | 16LX map (this PDF) | Firmware use | Match |
|----------|--------------------|--------------|-------|
| 0x00-0x0D | PDR0-PDRD | PDR5 `0xFF`, PDR8 (AMUX), PDR9, PDRB (SD0-7), PDRC (DIR/SC0-3) | yes |
| 0x10-0x1D | DDR0-DDRD | DDR5 `0xBF`, DDR6/DDR8 `0x3F`, DDRB toggled 00/FF, DDRC `0xFF` | yes |
| 0x20-0x27 | UART0 / UART1 SMR,SCR,SIDR/SODR,SSR | UART1 `0x24-0x27` = MIDI (SMR1 `0x09`, SCR1 `0x10`) | yes |
| 0x28-0x2F | CDCR0/1 only | eight bytes written `04 00 00 00 00 00 00 04` | **no** (MB90705: probably chip-select / area regs) |
| 0x30-0x33 | ENIR/EIRR/ELVR ext-int | init `38/28/28/xx`, ISRs clear bits 0x30:3 0x31:2 0x32:2 0x33:2 | **no** (timer/UART status on MB90705) |
| 0x40-0x67 | PPG, SMCS, ICU, OCU | four 16-bit reload timers: TMCSR at 0x40/0x50/0x58/0x60, reload at 0x44/0x54/0x5C/0x64 | **no** |
| 0x68-0x6B | I2C | ADC: 0x68 `0xC0`, 0x69 ctrl (start = bit 5), 0x6A/0x6B data | **no** |
| 0x9F | DIRR delayed interrupt | vector FF013E clears `I:0x9F:0` | yes |
| 0xA0, 0xA1 | LPMCR, CKSCR | `0x1C`, `0x2C` | plausible |
| 0xA2-0xA5 | (A5 = ARSR, A6 HACR, A7 ECSR on 16LX) | `A2=80 A3=10 A4=FC A5=00` | shifted: MB90705 likely ARSR/HACR/ECSR/WDTC at A2-A5 |
| 0xB0-0xBF | ICR00-ICR15 interrupt levels | `B0-B2=4, B4-B7=4, B8=2, B9/BA=3, BB=4, BE=3` | yes |

So the register *bit definitions* for ports, UART, ICR, LPMCR/CKSCR and DIRR can be read from this PDF;
the addresses of the reload timers, ADC, external-bus and chip-select registers must come from the
firmware (already partly derived) or from a real MB90700-series hardware manual.

Still wanted: Fujitsu "F2MC-16 MB90700 series hardware manual" / MB90705 data sheet (1994-96).
The Roland A-90 service notes list the same CPU.
