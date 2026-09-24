// Minimal shim standing in for MAME's emu.h so f2mc16d.cpp (BSD-3, AJR) builds standalone.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>

using u8 = uint8_t;  using u16 = uint16_t; using u32 = uint32_t; using u64 = uint64_t;
using s8 = int8_t;   using s16 = int16_t;  using s32 = int32_t;  using s64 = int64_t;
using offs_t = u32;

namespace util {

class disasm_interface {
public:
	enum : u32 {
		NONVANILLA_ALIGNMENT = 0x00000001,
		PAGED                = 0x00000002,
		PAGED2LEVEL          = 0x00000004,
		INTERNAL_DECRYPTION  = 0x00000008,
		SUPPORTED            = 0x80000000,
		STEP_OUT             = 0x40000000,
		STEP_OVER            = 0x20000000,
		STEP_COND            = 0x10000000,
		LENGTHMASK           = 0x0000ffff,
	};
	class data_buffer {
	public:
		virtual ~data_buffer() = default;
		virtual u8  r8 (offs_t pc) const = 0;
		virtual u16 r16(offs_t pc) const = 0;
		virtual u32 r32(offs_t pc) const = 0;
		virtual u64 r64(offs_t pc) const = 0;
	};
	virtual ~disasm_interface() = default;
	virtual u32 opcode_alignment() const = 0;
	virtual u32 interface_flags() const { return 0; }
	virtual u32 page_address_bits() const { return 0; }
	virtual offs_t disassemble(std::ostream &stream, offs_t pc, const data_buffer &opcodes, const data_buffer &params) = 0;
};

// Tiny printf-style formatter covering the specifiers f2mc16d.cpp uses:
// %d %X %02X %04X %06X %08X %s %-8s %c
namespace detail {
	inline void put(std::ostream &os, const std::string &spec, std::string_view v) {
		int width = 0; bool left = false; size_t i = 1;
		if (i < spec.size() && spec[i] == '-') { left = true; i++; }
		while (i < spec.size() && isdigit((unsigned char)spec[i])) width = width*10 + (spec[i++]-'0');
		std::string s(v);
		if ((int)s.size() < width) { if (left) s.append(width - s.size(), ' '); else s.insert(0, width - s.size(), ' '); }
		os << s;
	}
	inline void put(std::ostream &os, const std::string &spec, const char *v) { put(os, spec, std::string_view(v)); }
	inline void put(std::ostream &os, const std::string &spec, const std::string &v) { put(os, spec, std::string_view(v)); }
	template<typename T> inline void put(std::ostream &os, const std::string &spec, T v) {
		char conv = spec.back();
		char buf[64];
		if (conv == 'c') { snprintf(buf, sizeof buf, "%c", (int)v); os << buf; return; }
		std::string f = spec.substr(0, spec.size()-1);
		if (conv == 'd') { f += "lld"; snprintf(buf, sizeof buf, f.c_str(), (long long)v); }
		else            { f += "ll"; f += conv; snprintf(buf, sizeof buf, f.c_str(), (unsigned long long)(std::make_unsigned_t<T>)v); }
		os << buf;
	}
	inline void fmt_rest(std::ostream &os, const char *f) {
		for (; *f; f++) { if (f[0]=='%' && f[1]=='%') { os << '%'; f++; } else os << *f; }
	}
	template<typename A, typename... R>
	inline void fmt_rest(std::ostream &os, const char *f, A &&a, R&&... rest) {
		for (; *f; f++) {
			if (*f == '%') {
				if (f[1] == '%') { os << '%'; f++; continue; }
				std::string spec = "%"; f++;
				while (*f && !isalpha((unsigned char)*f)) spec += *f++;
				spec += *f;
				put(os, spec, std::forward<A>(a));
				fmt_rest(os, f+1, std::forward<R>(rest)...);
				return;
			}
			os << *f;
		}
	}
}
template<typename... A> inline void stream_format(std::ostream &os, const char *f, A&&... a) { detail::fmt_rest(os, f, std::forward<A>(a)...); }
template<typename... A> inline std::string string_format(const char *f, A&&... a) { std::ostringstream s; stream_format(s, f, std::forward<A>(a)...); return s.str(); }

} // namespace util

// MAME bit helpers
template<typename T> constexpr T BIT(T x, unsigned n) { return (x >> n) & T(1); }
template<typename T> constexpr T BIT(T x, unsigned n, unsigned w) { return (x >> n) & ((T(1) << w) - 1); }
