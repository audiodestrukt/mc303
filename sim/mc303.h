// MC-303 board model: memory map, MB90705 on-chip peripherals (as far as the firmware reveals them),
// interrupt controller, and the panel LED/switch matrix.
#pragma once
#include "emu.h"
#include "f2mc16.h"
#include <array>
#include <deque>
#include <map>
#include <mutex>

class MC303Board : public address_space {
public:
	MC303Board(f2mc16_device &cpu);
	bool load_roms(const char *os_path, const char *data_path);
	void reset();
	void step(int cycles);                 // advance peripherals by CPU cycles

	// address_space
	u8 read_byte(offs_t a) override;
	void write_byte(offs_t a, u8 d) override;

	// interrupt controller
	u8 irq_ack(int level, u32 pc);
	void request_irq(int vector, bool state);
	void update_irq();

	// panel
	std::array<u8, 16> leds{};             // one byte per scan column (SD0-7 as written to port B)
	std::array<u8, 16> switches{};         // pressed bits per column (1 = pressed)
	std::array<u8, 8> knobs;
	bool tap = false, pedal = false;
	std::string display_text() const;

	// MIDI
	std::deque<u8> midi_in;
	std::deque<u8> midi_out;

	// diagnostics
	bool trace_io = false, trace_ga = false, trace_irq = false;
	std::map<u32, u64> io_reads, io_writes, unmapped;
	u64 cycles = 0;
	u32 watch_addr = 0;
	bool irq_just_taken = false;
	u64 ga_reads = 0, ga_writes = 0;
	u64 audio_samples = 0, ga_irqs = 0, pcm_cycles = 0, pcm_frac = 0;
	bool waverom_loaded = false, wav = false;
	bool load_waverom(const char *path, bool do_unscramble);
	void audio_push(s16 l, s16 r);
	void write_wav(const char *path, int rate);
	std::vector<s16> wav_data;
	std::deque<s16> audio_ring; std::mutex audio_mutex;
	void ga_interrupt(int value);

	f2mc16_device &cpu;
	std::vector<u8> os_rom, data_rom, sram, iram;
	u8 io[0x100]{};

private:
	u8 io_read(u8 a);
	void io_write(u8 a, u8 d);

	// 16-bit reload timers: control at base, reload at base+4
	struct Reload { u8 base; int vector; s32 count = 0; u32 frac = 0; };
	std::array<Reload, 5> reload{{ {0x40, 11}, {0x48, 0}, {0x50, 14}, {0x58, 15}, {0x60, 16} }};
	void reload_step(Reload &t, int cycles);
	void reload_update(Reload &t);

	// PPG-style period timers: block at base (0x38 / 0x78), period A at +4 flag bit3, period B at +2 flag bit2
	struct Period { u8 base; int vecA, vecB; s32 countA = 0, countB = 0; };
	std::array<Period, 2> period{{ {0x38, 29, 30}, {0x78, 31, 0} }};
	void period_step(Period &p, int cycles);

	// ADC at 0x68-0x6B
	s32 adc_busy = 0;
	void adc_update();

	// UART1 at 0x24-0x27 (MIDI)
	void uart_update();

	u64 pending = 0;                       // bit per vector number
};
