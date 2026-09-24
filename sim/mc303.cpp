#include "mc303.h"
#include "f2mc16d.h"
#include "pcm.h"
#include "mcu.h"
#include "mcu_interrupt.h"
#include <fstream>
#include <iostream>
#include <chrono>
#include <mutex>
#include <cmath>
#include <algorithm>
#include <set>

static const int MACHINE_HZ = 12000000;   // 24 MHz crystal / 2 (assumed)
static MC303Board *g_board = nullptr;
extern const int PCM_RATE; const int PCM_RATE = 33103;   // 24 MHz / 725 cycles per frame        // PCM chip output rate (24 MHz clock / 800 cycles per frame); refine when pitch is checked
int run_gui(MC303Board &board, f2mc16_device &cpu, const char *shot_path, double shot_at);
int mcu_mk1 = 0;
int mcu_jv880 = 1;                          // MC-303 wave ROM addressing follows the JV-880 path
void MCU_PostSample(int *sample)
{
	if (!g_board) return;
	g_board->audio_samples++;
	int l = sample[0] >> 15, r = sample[1] >> 15;
	if (l > 32767) l = 32767; if (l < -32768) l = -32768;
	if (r > 32767) r = 32767; if (r < -32768) r = -32768;
	g_board->audio_push((s16)l, (s16)r);
}

// Nuked-SC55's wave ROM unscramble (address and data line permutation between ROM pins and PCM chip)
static void unscramble(const u8 *src, u8 *dst, int len)
{
	static const int aa[] = { 2, 0, 3, 4, 1, 9, 13, 10, 18, 17, 6, 15, 11, 16, 8, 5, 12, 7, 14, 19 };
	static const int dd[] = { 2, 0, 4, 5, 7, 6, 3, 1 };
	for (int i = 0; i < len; i++) {
		int address = i & ~0xfffff;
		for (int j = 0; j < 20; j++) if (i & (1 << j)) address |= 1 << aa[j];
		u8 srcdata = src[address], data = 0;
		for (int j = 0; j < 8; j++) if (srcdata & (1 << dd[j])) data |= 1 << j;
		dst[i] = data;
	}
}

bool MC303Board::load_waverom(const char *path, bool do_unscramble)
{
	std::ifstream f(path, std::ios::binary); if (!f) return false;
	std::vector<u8> v((std::istreambuf_iterator<char>(f)), {});
	if (v.size() < 0x200000) { fprintf(stderr, "wave ROM: too small (%zu)\n", v.size()); return false; }
	if (v.size() > 0x400000) { fprintf(stderr, "wave ROM: %zu bytes, using the first 4 MB\n", v.size()); v.resize(0x400000); }
	if (do_unscramble) { std::vector<u8> u(v.size()); unscramble(v.data(), u.data(), v.size()); v.swap(u); }
	memcpy(waverom1, v.data(), 0x200000);
	if (v.size() > 0x200000) memcpy(waverom2, v.data() + 0x200000, 0x200000);
	waverom_loaded = true;
	return true;
}

void MC303Board::audio_push(s16 l, s16 r)
{
	if (wav) { wav_data.push_back(l); wav_data.push_back(r); }
	std::lock_guard<std::mutex> g(audio_mutex);
	if (audio_ring.size() < 65536) { audio_ring.push_back(l); audio_ring.push_back(r); }
}

void MC303Board::write_wav(const char *path, int rate)
{
	std::ofstream f(path, std::ios::binary);
	u32 datasz = wav_data.size() * 2, byterate = rate * 4;
	auto w32 = [&](u32 v) { f.write((char *)&v, 4); }; auto w16 = [&](u16 v) { f.write((char *)&v, 2); };
	f.write("RIFF", 4); w32(36 + datasz); f.write("WAVE", 4); f.write("fmt ", 4); w32(16); w16(1); w16(2); w32(rate); w32(byterate); w16(4); w16(16);
	f.write("data", 4); w32(datasz); f.write((const char *)wav_data.data(), datasz);
	printf("wrote %s: %zu frames at %d Hz\n", path, wav_data.size() / 2, rate);
}
void MCU_GA_SetGAInt(int line, int value) { if (g_board) g_board->ga_interrupt(value); }
void MCU_Interrupt_SetRequest(int, int value) { if (g_board) g_board->ga_interrupt(value); }

MC303Board::MC303Board(f2mc16_device &c) : cpu(c), sram(0x40000, 0), iram(0x2000, 0)
{
	knobs.fill(0x40);
	g_board = this;
	cpu.set_space(this);
	cpu.irq_ack = [this](int level, u32 pc) { return irq_ack(level, pc); };
	cpu.machine().m_context = [this]() { return string_format("%llu", (unsigned long long)cycles); };
}

bool MC303Board::load_roms(const char *os_path, const char *data_path)
{
	auto load = [](const char *p, std::vector<u8> &v) {
		std::ifstream f(p, std::ios::binary); if (!f) return false;
		v.assign(std::istreambuf_iterator<char>(f), {}); return v.size() == 0x80000;
	};
	return load(os_path, os_rom) && load(data_path, data_rom);
}

void MC303Board::reset()
{
	memset(io, 0, sizeof io);
	io[0x27] = 0x08;                        // UART1 SSR: TDRE
	for (int p = 0; p < 0x10; p++) io[p] = 0xff;   // ports read high (pull-ups) until driven
	io[0x08] = 0;
	pending = 0;
	for (auto &t : reload) t.count = 0;
	adc_busy = 0;
	PCM_Reset();
	pcm_cycles = 0;
	cpu.reset();
}

// ------------------------------------------------------------------ memory map
u8 MC303Board::read_byte(offs_t a)
{
	a &= 0xffffff;
	u8 bank = a >> 16;
	if (bank == 0x00) {
		if (a < 0x100) return io_read(a);
		if (a < 0x2000) return iram[a];
		if ((a & 0xff00) == 0xe000) {
			ga_reads++;
			u8 v = (a & 1) ? 0xff : PCM_Read((a >> 1) & 0x3f);
			if (trace_ga) printf("[%llu] GA rd %04X -> %02X\n", (unsigned long long)cycles, a, v);
			return v;
		}
	}
	else if (bank >= 0xa0 && bank <= 0xa3) return sram[a - 0xa00000];
	else if (bank >= 0xf0 && bank <= 0xf7) return data_rom[a - 0xf00000];
	else if (bank >= 0xf8)                 return os_rom[a - 0xf80000];
	unmapped[a]++;
	return 0xff;
}

void MC303Board::write_byte(offs_t a, u8 d)
{
	a &= 0xffffff;
	u8 bank = a >> 16;
	if (bank == 0x00) {
		if (a < 0x100) { io_write(a, d); return; }
		if (a < 0x2000) { iram[a] = d; return; }
		if ((a & 0xff00) == 0xe000) {
			ga_writes++;
			if (trace_ga) printf("[%llu] GA wr %04X = %02X\n", (unsigned long long)cycles, a, d);
			if (!(a & 1)) PCM_Write((a >> 1) & 0x3f, d);
			return;
		}
	}
	else if (bank >= 0xa0 && bank <= 0xa3) {
		if (a == watch_addr) printf("[%llu] WATCH %06X <= %02X at pc %06X\n", (unsigned long long)cycles, a, d, cpu.pc24());
		sram[a - 0xa00000] = d; return;
	}
	unmapped[a | 0x1000000]++;
}

// ------------------------------------------------------------------ on-chip I/O
u8 MC303Board::io_read(u8 a)
{
	io_reads[a]++;
	switch (a) {
	case 0x0b: {                            // port B: SD0-7, switch data when DIR (PC0) low and DDRB = input
		if (io[0x1b] == 0 && !(io[0x0c] & 1)) {
			int col = (io[0x0c] >> 1) & 0x0f;
			return ~switches[col];
		}
		return io[0x0b];
	}
	case 0x05: return tap   ? 0xbf : 0xff;  // P56 read inverted by firmware: 0 = pressed
	case 0x09: return pedal ? 0x7f : 0xff;  // P97
	case 0x26: {                            // UART1 receive data
		u8 d = midi_in.empty() ? 0 : midi_in.front();
		if (!midi_in.empty()) midi_in.pop_front();
		uart_update();
		return d;
	}
	default: return io[a];
	}
}

void MC303Board::io_write(u8 a, u8 d)
{
	io_writes[a]++;
	if (trace_io && a >= 0x20) printf("[%llu] io wr %02X = %02X\n", (unsigned long long)cycles, a, d);
	u8 old = io[a];
	io[a] = d;
	switch (a) {
	case 0x0c:                              // port C: bit0 = DIR, bits1-4 = scan column
		if ((d & 1) && !(old & 1)) {        // rising DIR: latch LED column data from port B
			int col = (d >> 1) & 0x0f;
			leds[col] = io[0x0b];
		}
		break;
	case 0x26:                              // UART1 transmit
		midi_out.push_back(d);
		break;
	case 0x40: case 0x41: case 0x44: case 0x45:
	case 0x48: case 0x49: case 0x4c: case 0x4d:
	case 0x50: case 0x51: case 0x54: case 0x55:
	case 0x58: case 0x59: case 0x5c: case 0x5d:
	case 0x60: case 0x61: case 0x64: case 0x65:
		for (auto &t : reload) if ((a & 0xf8) == t.base) { if ((a & 7) == 0 && (d & 1)) { t.count = io[t.base+4] | (io[t.base+5] << 8); } reload_update(t); }
		break;
	case 0x3e: case 0x7e: case 0x39: case 0x79:
		for (auto &p : period) if ((a & 0xf8) == p.base) {
			request_irq(p.vecA, (io[p.base+6] & 0x08) != 0);
			if (p.vecB) request_irq(p.vecB, (io[p.base+6] & 0x04) != 0);
		}
		break;
	case 0x69:                              // ADC control: bit5 start, bit3 INTE, bit7 done flag (write 0 clears)
		if (d & 0x20) adc_busy = 1000;        // ~83 us per conversion: 16-step panel scan at a few hundred Hz
		adc_update();
		break;
	case 0x9f:                              // DIRR: delayed interrupt -> vector 42
		request_irq(42, d & 1);
		break;
	case 0x30: case 0x31: case 0x32: case 0x33: case 0x35:
		request_irq(19, (io[0x30] & 0x08) != 0);
		request_irq(21, (io[0x31] & 0x04) != 0);
		request_irq(23, (io[0x32] & 0x04) != 0);
		request_irq(25, (io[0x33] & 0x04) != 0);
		request_irq(33, (io[0x35] & 0x08) != 0);
		break;
	default:
		if (a >= 0xb0 && a <= 0xbf) update_irq();
		break;
	}
}

// ------------------------------------------------------------------ peripherals
void MC303Board::reload_update(Reload &t)
{
	if (!t.vector) return;
	u16 csr = io[t.base] | (io[t.base+1] << 8);
	request_irq(t.vector, (csr & 0x0c) == 0x0c);   // UF && INTE
}

void MC303Board::reload_step(Reload &t, int cyc)
{
	u16 csr = io[t.base] | (io[t.base+1] << 8);
	if (!(csr & 0x02)) return;                      // CNTE
	static const int presc[4] = { 2, 8, 32, 32 };
	int div = presc[(csr >> 10) & 3];
	t.frac += cyc;
	int ticks = t.frac / div; t.frac %= div;
	t.count -= ticks;
	if (t.count < 0) {
		u16 rl = io[t.base+4] | (io[t.base+5] << 8);
		if (csr & 0x10) t.count += rl + 1;          // RELD
		else { t.count = 0; io[t.base] &= ~0x02; }  // one-shot: stop
		io[t.base] |= 0x04;                          // UF
		reload_update(t);
	}
}

void MC303Board::period_step(Period &p, int cyc)
{
	u8 ctl = io[p.base+6];
	if (!(ctl & 0x20)) return;                      // running
	// Rates are guesses until the MB90705 timer block is documented: channel A counts at phi/2,
	// channel B (8-bit reload at +2) at phi/128.
	const int divA = 128, divB = 128;   // both channels behind a /128 prescaler: 25000 -> 267 ms (active sensing), 0x50 -> 0.85 ms tick
	u16 perA = io[p.base+4] | (io[p.base+5] << 8);
	u16 perB = io[p.base+2];
	p.countA += cyc; p.countB += cyc;
	if (perA && p.countA >= (s32)perA * divA) { p.countA -= perA * divA; io[p.base+6] |= 0x08; io[p.base+1] |= 0x80; request_irq(p.vecA, true); }
	if (p.vecB && perB && p.countB >= (s32)perB * divB) { p.countB -= perB * divB; io[p.base+6] |= 0x04; request_irq(p.vecB, true); }
}

void MC303Board::adc_update()
{
	request_irq(27, (io[0x69] & 0x88) == 0x88);
}

void MC303Board::uart_update()
{
	if (midi_in.empty()) io[0x27] &= ~0x10; else io[0x27] |= 0x10;   // RDRF
	io[0x27] |= 0x08;                                                 // TDRE always
	request_irq(39, (io[0x27] & 0x10) != 0);
}

void MC303Board::step(int cyc)
{
	cycles += cyc;
	for (auto &t : reload) reload_step(t, cyc);
	for (auto &p : period) period_step(p, cyc);
	if (adc_busy > 0) {
		adc_busy -= cyc;
		if (adc_busy <= 0) {
			int ch = io[0x08] & 7;
			io[0x6a] = knobs[ch] << 1;          // 10-bit result, low byte
			io[0x6b] = knobs[ch] >> 7;
			io[0x69] = (io[0x69] & ~0x20) | 0x80;
			adc_update();
		}
	}
	uart_update();
	// PCM chip clocked from the 24 MHz crystal (2x the CPU machine clock). pcm.cpp's JV-880 path scales
	// its per-frame cost by 25/29, so pre-scale by 25/29 so that one frame costs the real 725 chip cycles.
	// With the firmware's config (28 slots -> 725 cycles per frame) that gives 33.1 kHz output.
	pcm_frac += (u64)cyc * 2 * 25;
	pcm_cycles += pcm_frac / 29; pcm_frac %= 29;
	PCM_Update(pcm_cycles);
}

void MC303Board::ga_interrupt(int value)
{
	if (value) { ga_irqs++; io[0x33] |= 0x04; request_irq(25, true); }   // GPINT -> external interrupt flag, vector 25
}

// ------------------------------------------------------------------ interrupt controller
void MC303Board::request_irq(int vector, bool state)
{
	u64 bit = 1ull << vector;
	u64 np = state ? (pending | bit) : (pending & ~bit);
	if (np != pending) {
		pending = np; update_irq();
		if (trace_irq) printf("[%llu] irq %d %s  ICR=%d  PS=%04X pc=%06X\n", (unsigned long long)cycles, vector, state ? "REQ" : "clr", io[0xb0 + (vector - 11) / 2] & 7, cpu.ps(), cpu.pc24());
	}
}

void MC303Board::update_irq()
{
	int level = 7;
	for (int v = 11; v < 48; v++) if (pending & (1ull << v)) {
		int il = io[0xb0 + (v - 11) / 2] & 7;
		if (il < level) level = il;
	}
	cpu.set_irq_level(level);
}

u8 MC303Board::irq_ack(int level, u32 pc)
{
	// return the highest-priority pending vector (lowest ICR level, then lowest vector number)
	int best = 0, best_il = 8;
	for (int v = 11; v < 48; v++) if (pending & (1ull << v)) {
		int il = io[0xb0 + (v - 11) / 2] & 7;
		if (il < best_il) { best_il = il; best = v; }
	}
	if (best) irq_just_taken = true;
	if (trace_irq && best) printf("[%llu] take vector %d (level %d) at %06X\n", (unsigned long long)cycles, best, best_il, pc);
	return best;
}

// ------------------------------------------------------------------ main
// LB-603VP digits: one scan column per digit, bit0..6 = segments A..G, bit7 = DP, lit when 0.
// Left display = columns 7,6,5 (digit 1..3), right display = columns 13,14,15.
static std::string seg_char(u8 v)
{
	static const struct { u8 seg; const char *ch; } font[] = {
		{0x3f,"0"},{0x06,"1"},{0x5b,"2"},{0x4f,"3"},{0x66,"4"},{0x6d,"5"},{0x7d,"6"},{0x07,"7"},{0x7f,"8"},{0x6f,"9"},
		{0x77,"A"},{0x7c,"b"},{0x39,"C"},{0x5e,"d"},{0x79,"E"},{0x71,"F"},{0x40,"-"},{0x00," "},{0x73,"P"},{0x50,"r"},
		{0x54,"n"},{0x5c,"o"},{0x78,"t"},{0x38,"L"},{0x76,"H"},{0x1c,"u"},{0x3e,"U"},{0x3d,"G"},{0x58,"c"},{0x74,"h"},
		{0x1e,"J"},{0x6e,"y"},{0x67,"q"},{0x08,"_"},{0x48,"="},{0x37,"M"},{0x5b,"Z"} };
	u8 seg = (~v) & 0x7f;
	for (auto &f : font) if (f.seg == seg) return std::string(f.ch) + ((v & 0x80) ? "" : ".");
	char b[8]; snprintf(b, sizeof b, "<%02X>", seg); return b;
}
std::string MC303Board::display_text() const
{
	std::string l, r;
	for (int c : {7, 6, 5}) l += seg_char(leds[c]);
	for (int c : {13, 14, 15}) r += seg_char(leds[c]);
	return "[" + l + "] [" + r + "]";
}

static void print_leds(const MC303Board &b)
{
	printf("display: %s\n", b.display_text().c_str());
	printf("LED matrix (columns 0-15 across, SD0-7 down; # = bit set):\n     ");
	for (int c = 0; c < 16; c++) printf("%2d ", c);
	printf("\n");
	for (int r = 0; r < 8; r++) {
		printf("SD%d  ", r);
		for (int c = 0; c < 16; c++) printf(" %c ", (b.leds[c] >> r) & 1 ? '#' : '.');
		printf("\n");
	}
}

int main(int argc, char **argv)
{
	const char *os_path = "../roms/1.03 [27C4096][37OD][Program OS][IC 7].BIN";
	const char *data_path = "../roms/1.0 [27C4096][C673][DATA OS][IC 8].BIN";
	double seconds = 2.0;
	bool trace = false, trace_io = false, trace_ga = false, trace_irq = false, check_len = false; int trace_task = -1; bool gui = false, dump_pcm = false;
	struct MidiEv { double t; std::vector<u8> bytes; bool done = false; }; std::vector<MidiEv> midi_evs; const char *wave_path = "../roms/waverom.bin"; bool wave_unscramble = false; const char *wav_path = nullptr; const char *shot_path = nullptr; double shot_at = 2.5; u32 dump_addr = 0; int dump_n = 0;
	int knob_over[8]; for (int i = 0; i < 8; i++) knob_over[i] = -1;
	std::vector<u32> bps; u32 watch = 0; const char *sram_path = nullptr;
	struct Press { int col, bit; double t_on = 0, t_off = 1e9; }; std::vector<Press> presses; u64 trace_from = 0;
	for (int i = 1; i < argc; i++) {
		std::string a = argv[i];
		if (a == "-t") seconds = atof(argv[++i]);
		else if (a == "--trace") { trace = true; trace_from = strtoull(argv[++i], 0, 0); }
		else if (a == "--io") trace_io = true;
		else if (a == "--ga") trace_ga = true;
		else if (a == "--irq") trace_irq = true;
		else if (a == "--gui") gui = true;
		else if (a == "--dumppcm") dump_pcm = true;
		else if (a == "--midi") {   // --midi T "90 3C 7F"  : inject MIDI bytes at time T (seconds)
			MidiEv ev; ev.t = atof(argv[++i]); std::string hex = argv[++i];
			for (size_t k = 0; k + 1 < hex.size(); ) { while (k < hex.size() && hex[k] == ' ') k++; if (k + 1 >= hex.size()) break; ev.bytes.push_back((u8)strtoul(hex.substr(k, 2).c_str(), 0, 16)); k += 2; }
			midi_evs.push_back(ev);
		}
		else if (a == "--waverom") wave_path = argv[++i];
		else if (a == "--unscramble") wave_unscramble = true;
		else if (a == "--wav") wav_path = argv[++i];
		else if (a == "--shot") { shot_path = argv[++i]; shot_at = atof(argv[++i]); gui = true; }
		else if (a == "--dumpw") { dump_addr = strtoul(argv[++i], 0, 16); dump_n = atoi(argv[++i]); }
		else if (a == "--task") trace_task = strtoul(argv[++i], 0, 16);
		else if (a == "--check") check_len = true;
		else if (a == "--sram") sram_path = argv[++i];
		else if (a == "--press") {   // --press col bit [t_on t_off]  (seconds; default held from boot)
			Press pr; pr.col = atoi(argv[++i]); pr.bit = atoi(argv[++i]);
			if (i + 2 < argc && argv[i+1][0] != '-') { pr.t_on = atof(argv[++i]); pr.t_off = atof(argv[++i]); }
			presses.push_back(pr);
		}
		else if (a == "--wp") watch = strtoul(argv[++i], 0, 16);
		else if (a == "--bp") bps.push_back(strtoul(argv[++i], 0, 16));
		else if (a == "--knob") { int n = atoi(argv[++i]); knob_over[n & 7] = atoi(argv[++i]); }
		else if (a == "--os") os_path = argv[++i];
		else if (a == "--data") data_path = argv[++i];
	}

	f2mc16_device cpu(machine_config{}, "cpu", nullptr, MACHINE_HZ);
	MC303Board board(cpu);
	if (!board.load_roms(os_path, data_path)) { fprintf(stderr, "cannot load ROMs\n"); return 1; }
	for (int i = 0; i < 8; i++) if (knob_over[i] >= 0) board.knobs[i] = knob_over[i];
	board.watch_addr = watch;
	board.trace_io = trace_io; board.trace_ga = trace_ga; board.trace_irq = trace_irq;
	if (board.load_waverom(wave_path, wave_unscramble)) printf("wave ROM loaded from %s%s\n", wave_path, wave_unscramble ? " (unscrambled)" : ""); else printf("no wave ROM (%s): silent\n", wave_path);
	if (wav_path) board.wav = true;
	cpu.start();
	board.reset();
	if (sram_path) { std::ifstream f(sram_path, std::ios::binary); if (f) { f.read((char *)board.sram.data(), board.sram.size()); printf("loaded SRAM from %s\n", sram_path); } }


	struct PubDasm : f2mc16_disassembler { using f2mc16_disassembler::disassemble; };
	PubDasm dasm_obj; f2mc16_disassembler *dasm = &dasm_obj;
	struct Buf : util::disasm_interface::data_buffer {
		MC303Board *b;
		u8 r8(offs_t a) const override { return b->read_byte(a); }
		u16 r16(offs_t a) const override { return r8(a) | (r8(a+1) << 8); }
		u32 r32(offs_t a) const override { return r16(a) | (r16(a+2) << 16); }
		u64 r64(offs_t a) const override { return r32(a) | ((u64)r32(a+4) << 32); }
	} buf; buf.b = &board;
	std::map<u32, u64> pc_hist;
	// --check: verify that every non-branching instruction advances PC by the disassembler's length
	u32 chk_prev_pc = 0, chk_prev_len = 0; bool chk_prev_flow = true; std::set<std::string> chk_reported;
	std::string chk_prev_text;
	cpu.instruction_hook = [&](u32 pc) {
		pc_hist[pc]++;
		if (check_len) {
			if (board.irq_just_taken) { board.irq_just_taken = false; chk_prev_flow = true; }
			if (!chk_prev_flow && pc != chk_prev_pc + chk_prev_len && (pc >> 16) == (chk_prev_pc >> 16)) {
				std::string key = chk_prev_text.substr(0, 8);
				std::ostringstream s; dasm_obj.disassemble(s, chk_prev_pc, buf, buf);
				std::string full = s.str();
				if (!chk_reported.count(full)) { chk_reported.insert(full); printf("LENGTH MISMATCH at %06X: '%s' len %u but next pc %06X (delta %d)\n", chk_prev_pc, full.c_str(), chk_prev_len, pc, (int)(pc - chk_prev_pc)); }
			}
			std::ostringstream s; offs_t r = dasm_obj.disassemble(s, pc, buf, buf);
			chk_prev_text = s.str(); chk_prev_pc = pc; chk_prev_len = r & util::disasm_interface::LENGTHMASK;
			std::string mn = chk_prev_text.substr(0, chk_prev_text.find(' '));
			static const std::set<std::string> flow = {"CALL","CALLP","CALLV","JMP","JMPP","BRA","BZ","BNZ","BC","BNC","BN","BP","BV","BNV","BT","BNT","BLT","BGE","BLE","BGT","BLS","BHI","BEQ","BNE","CBNE","CWBNE","DBNZ","DWBNZ","SBBS","BBC","BBS","RET","RETP","RETI","INT","INT9","INTP","WBTS","WBTC","MOVSI","MOVSD","MOVSWI","MOVSWD","FILSI","FILSWI","FILSD","FILSWD","SCEQI","SCEQD","SCWEQI","SCWEQD"};
			chk_prev_flow = flow.count(mn) || (r & (util::disasm_interface::STEP_OUT | util::disasm_interface::STEP_OVER | util::disasm_interface::STEP_COND)) || mn.rfind("PCB:",0)==0 || mn.rfind("DTB:",0)==0 || mn.rfind("ADB:",0)==0 || mn.rfind("SPB:",0)==0 || mn=="NCC" || mn=="CMR";
		}
		for (u32 b : bps) if (pc == b) {
			u32 sp = (cpu.ps() & 0x20) ? ((cpu.ssb() << 16) | cpu.ssp()) : ((cpu.usb() << 16) | cpu.usp());
			printf("[%llu] BP %06X task=%04X A=%08X PS=%04X SP=%06X stack:", (unsigned long long)board.cycles, pc, board.sram[2] | (board.sram[3] << 8), cpu.acc(), cpu.ps(), sp);
			for (int i = 0; i < 8; i++) printf(" %04X", board.read_byte(sp + 2*i) | (board.read_byte(sp + 2*i + 1) << 8));
			printf("\n");
		}
		if (trace && board.cycles >= trace_from && (trace_task < 0 || (board.sram[2] | (board.sram[3] << 8)) == trace_task)) {
			std::ostringstream s; dasm_obj.disassemble(s, pc, buf, buf);
			printf("%llu %06X  %s\n", (unsigned long long)board.cycles, pc, s.str().c_str());
		}
	};

	if (gui) { int rc = run_gui(board, cpu, shot_path, shot_at); if (sram_path) { std::ofstream f(sram_path, std::ios::binary); f.write((const char *)board.sram.data(), board.sram.size()); } return rc; }
	u64 total = (u64)(seconds * MACHINE_HZ);
	const int slice = 100;
	try {
		std::string last_disp; u64 next_check = 0;
		while (board.cycles < total) {
			int ran = cpu.run(slice);
			board.step(ran > 0 ? ran : slice);
			if (board.cycles >= next_check) {          // report display changes (every ~5 ms)
				next_check = board.cycles + MACHINE_HZ / 200;
				double now = (double)board.cycles / MACHINE_HZ;
				for (auto &ev : midi_evs) if (!ev.done && now >= ev.t) { ev.done = true; for (u8 b : ev.bytes) board.midi_in.push_back(b); }
				for (auto &pr : presses) {
					bool on = now >= pr.t_on && now < pr.t_off;
					if (on) board.switches[pr.col & 15] |= 1 << (pr.bit & 7); else board.switches[pr.col & 15] &= ~(1 << (pr.bit & 7));
				}
				std::string d = board.display_text();
				if (d != last_disp) { printf("[%7.3f s] display %s\n", (double)board.cycles / MACHINE_HZ, d.c_str()); last_disp = d; }
			}
		}
	} catch (std::exception &e) {
		printf("STOP at %llu cycles: %s\n", (unsigned long long)board.cycles, e.what());
	}

	if (sram_path) { std::ofstream f(sram_path, std::ios::binary); f.write((const char *)board.sram.data(), board.sram.size()); printf("saved SRAM to %s\n", sram_path); }
	if (wav_path) board.write_wav(wav_path, PCM_RATE);
	if (dump_pcm) {
		printf("PCM voice RAM (slots with data): mask=%08X 3c=%02X 3d=%02X\n", pcm.voice_mask, pcm.config_reg_3c, pcm.config_reg_3d);
		for (int v = 0; v < 32; v++) {
			bool any = false; for (int i = 0; i < 8; i++) if (pcm.ram1[v][i]) any = true;
			if (!any) continue;
			printf("  slot %2d ram1:", v); for (int i = 0; i < 8; i++) printf(" %05X", pcm.ram1[v][i]);
			printf("  ram2:"); for (int i = 0; i < 16; i++) printf(" %04X", pcm.ram2[v][i]); printf("\n");
		}
	}
	if (dump_n) { printf("dump %06X:", dump_addr); for (int i = 0; i < dump_n; i++) printf(" %04X", board.read_byte(dump_addr + 2*i) | (board.read_byte(dump_addr + 2*i + 1) << 8)); printf("\n"); }
	printf("\n== after %.3f s emulated (%llu cycles)\n", (double)board.cycles / MACHINE_HZ, (unsigned long long)board.cycles);
	print_leds(board);
	printf("\nblink mask A0:5D3A:        "); for (int c = 0; c < 16; c++) printf("%02X ", board.sram[0x5d3a + c]);
	printf("\nLED buffer in RAM A0:5D1A: ");
	for (int c = 0; c < 16; c++) printf("%02X ", board.sram[0x5d1a + c]);
	printf("\nMIDI out: %zu bytes:", board.midi_out.size());
	for (size_t i = 0; i < board.midi_out.size() && i < 32; i++) printf(" %02X", board.midi_out[i]);
	printf("\nGA reads %llu writes %llu, PCM samples %llu, GA irqs %llu\n", (unsigned long long)board.ga_reads, (unsigned long long)board.ga_writes, (unsigned long long)board.audio_samples, (unsigned long long)board.ga_irqs);
	printf("unmapped accesses (addr|0x1000000 = write): ");
	int n = 0; for (auto &u : board.unmapped) { if (n++ > 12) break; printf("%07X x%llu  ", u.first, (unsigned long long)u.second); }
	printf("\nhot PCs:");
	std::vector<std::pair<u64,u32>> hot; for (auto &h : pc_hist) hot.push_back({h.second, h.first});
	std::sort(hot.rbegin(), hot.rend());
	for (int i = 0; i < 8 && i < (int)hot.size(); i++) printf(" %06X(%llu)", hot[i].second, (unsigned long long)hot[i].first);
	printf("\nI/O writes:");
	for (auto &w : board.io_writes) printf(" %02X:%llu", w.first, (unsigned long long)w.second);
	printf("\n");
	return 0;
}
