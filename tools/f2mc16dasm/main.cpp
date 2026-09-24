// Standalone F2MC-16 disassembler for the MC-303 OS ROM, built on MAME's f2mc16d.cpp.
//   linear:  f2mc16dasm rom.bin load_addr [start] [end]
//   recurse: f2mc16dasm -r rom.bin load_addr [extra_seed ...]   (follows flow from the vector table)
#include "emu.h"
#include "f2mc16d.h"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <map>
#include <regex>
#include <set>
#include <vector>

struct pub_dasm : f2mc16_disassembler { using f2mc16_disassembler::disassemble; };

struct rom_buffer : util::disasm_interface::data_buffer {
	std::vector<u8> d; u32 base;
	bool has(offs_t a) const { return a >= base && a - base < d.size(); }
	u8 at(offs_t a) const { return has(a) ? d[a - base] : 0xff; }
	u8  r8 (offs_t pc) const override { return at(pc); }
	u16 r16(offs_t pc) const override { return at(pc) | (at(pc+1)<<8); }
	u32 r32(offs_t pc) const override { return r16(pc) | (r16(pc+2)<<16); }
	u64 r64(offs_t pc) const override { return r32(pc) | ((u64)r32(pc+4)<<32); }
};

static std::string mnemonic(const std::string &s) { auto p = s.find(' '); return s.substr(0, p); }

static void print_insn(const rom_buffer &rom, u32 pc, u32 len, const std::string &text) {
	std::string bytes;
	for (u32 i = 0; i < len && i < 8; i++) { char b[4]; snprintf(b, 4, "%02X ", rom.at(pc+i)); bytes += b; }
	printf("%06X  %-24s %s\n", pc, bytes.c_str(), text.c_str());
}

int main(int argc, char **argv) {
	bool recurse = false; int ai = 1;
	if (argc > 1 && std::string(argv[1]) == "-r") { recurse = true; ai++; }
	if (argc < ai + 2) { fprintf(stderr, "usage: %s [-r] rom.bin load_addr [start|seed...] [end]\n", argv[0]); return 1; }
	rom_buffer rom;
	std::ifstream f(argv[ai], std::ios::binary); rom.d.assign(std::istreambuf_iterator<char>(f), {});
	rom.base = strtoul(argv[ai+1], 0, 16);
	pub_dasm dasm;

	if (!recurse) {
		u32 pc = argc > ai+2 ? strtoul(argv[ai+2], 0, 16) : rom.base;
		u32 end = argc > ai+3 ? strtoul(argv[ai+3], 0, 16) : rom.base + rom.d.size();
		while (pc < end) {
			std::ostringstream s; offs_t r = dasm.disassemble(s, pc, rom, rom);
			u32 len = std::max(1u, r & util::disasm_interface::LENGTHMASK);
			print_insn(rom, pc, len, s.str()); pc += len;
		}
		return 0;
	}

	// ---- recursive descent ----
	std::set<u32> seeds, funcs, visited;
	std::vector<u32> work;
	auto seed = [&](u32 a, bool isfunc) { if (rom.has(a) && !visited.count(a)) { work.push_back(a); } if (isfunc) funcs.insert(a); };
	// vector table: FFFF00..FFFFFF, 4 bytes each: addr24 + mode byte
	for (u32 v = 0xFFFF00; v < 0x1000000; v += 4) {
		u32 a = rom.r8(v) | (rom.r8(v+1)<<8) | (rom.r8(v+2)<<16);
		if (rom.has(a) && a != 0xFFFFFF) { seed(a, true); seeds.insert(a); }
	}
	for (int i = ai+2; i < argc; i++) seed(strtoul(argv[i], 0, 16), true);

	std::map<u32, std::pair<u32,std::string>> code;  // pc -> (len, text)
	std::regex re24("0x([0-9A-F]{6})"), re16("0x([0-9A-F]{4})\\b"), reimm32("#0x00([0-9A-F]{6})\\b");
	std::set<u32> ptr_seeds;  // 32-bit immediates that point into ROM (function pointers / task entries)
	for (int pass = 0; pass < 8; pass++) {
	size_t before = visited.size();
	static const std::set<std::string> flow = {"CALL","CALLP","JMP","JMPP","BRA","BZ","BNZ","BC","BNC","BN","BP","BV","BNV","BT","BNT","BLT","BGE","BLE","BGT","BLS","BHI","BEQ","BNE",
		"CBNE","CWBNE","DBNZ","DWBNZ","SBBS","BBC","BBS"};
	while (!work.empty()) {
		u32 pc = work.back(); work.pop_back();
		while (rom.has(pc) && !visited.count(pc)) {
			std::ostringstream s; offs_t r = dasm.disassemble(s, pc, rom, rom);
			u32 len = std::max(1u, r & util::disasm_interface::LENGTHMASK);
			std::string text = s.str(); std::string mn = mnemonic(text);
			for (u32 i = 0; i < len; i++) visited.insert(pc+i);
			code[pc] = {len, text};
			bool iscall = (mn == "CALL" || mn == "CALLP");
			if (flow.count(mn)) {
				// the branch target is always the LAST address operand (CBNE/DBNZ carry an immediate first)
				u32 tgt = 0; bool ok = false;
				for (auto it = std::sregex_iterator(text.begin(), text.end(), re24); it != std::sregex_iterator(); ++it) { tgt = strtoul((*it)[1].str().c_str(), 0, 16); ok = true; }
				if (!ok) for (auto it = std::sregex_iterator(text.begin(), text.end(), re16); it != std::sregex_iterator(); ++it) { tgt = (pc & 0xFF0000) | strtoul((*it)[1].str().c_str(), 0, 16); ok = true; }
				if (ok && text.find('@') == std::string::npos) seed(tgt, iscall);
			}
			if (mn == "MOVL") { std::smatch m; if (std::regex_search(text, m, reimm32)) { u32 p = strtoul(m[1].str().c_str(), 0, 16);
				// only accept pointers whose target starts like a C function (LINK / PUSHW / MOV RP / CALLP / CALL)
				u8 b = rom.at(p); if (rom.has(p) && (b == 0x08 || b == 0x4F || b == 0x0A || b == 0x65 || b == 0x64)) ptr_seeds.insert(p); } }
			if (r & util::disasm_interface::STEP_OUT) break;                       // RET/RETP/RETI
			if (mn == "JMP" || mn == "JMPP" || mn == "BRA") break;                  // unconditional
			pc += len;
		}
	}
	for (u32 p : ptr_seeds) if (!visited.count(p)) seed(p, true);
	if (work.empty() && visited.size() == before) break;
	}
	// ---- output ----
	u32 covered = visited.size();
	printf("; recursive-descent listing: %u bytes of code (%.1f%% of ROM), %zu call targets, %zu vector seeds, %zu pointer seeds\n",
		covered, 100.0*covered/rom.d.size(), funcs.size(), seeds.size(), ptr_seeds.size());
	u32 pc = rom.base, end = rom.base + rom.d.size();
	while (pc < end) {
		auto it = code.find(pc);
		if (it != code.end()) {
			if (funcs.count(pc)) printf("\nsub_%06X:%s\n", pc, seeds.count(pc) ? "   ; vector" : "");
			print_insn(rom, pc, it->second.first, it->second.second); pc += it->second.first;
		} else {
			u32 start = pc; while (pc < end && !code.count(pc)) pc++;
			printf("\n%06X  ; ---- %u bytes not reached as code ----\n\n", start, pc - start);
		}
	}
	return 0;
}
