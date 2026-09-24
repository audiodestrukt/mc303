// Minimal stand-in for MAME's emu.h so the unmodified f2mc16.cpp / f2mc16d.cpp (BSD-3) build standalone.
#pragma once
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using u8 = uint8_t;  using u16 = uint16_t; using u32 = uint32_t; using u64 = uint64_t;
using s8 = int8_t;   using s16 = int16_t;  using s32 = int32_t;  using s64 = int64_t;
using offs_t = u32;
#define ATTR_COLD

template<typename T> constexpr T BIT(T x, unsigned n) { return (x >> n) & T(1); }
template<typename T> constexpr T BIT(T x, unsigned n, unsigned w) { return (x >> n) & ((T(1) << w) - 1); }

// ---------------------------------------------------------------- util::stream_format & disasm_interface
namespace util {
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
		char conv = spec.back(); char buf[64];
		if (conv == 'c') { snprintf(buf, sizeof buf, "%c", (int)v); os << buf; return; }
		std::string f = spec.substr(0, spec.size()-1);
		if (conv == 'd') { f += "lld"; snprintf(buf, sizeof buf, f.c_str(), (long long)v); }
		else            { f += "ll"; f += conv; snprintf(buf, sizeof buf, f.c_str(), (unsigned long long)(std::make_unsigned_t<T>)v); }
		os << buf;
	}
	inline void fmt_rest(std::ostream &os, const char *f) { for (; *f; f++) { if (f[0]=='%' && f[1]=='%') { os << '%'; f++; } else os << *f; } }
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

class disasm_interface {
public:
	enum : u32 { NONVANILLA_ALIGNMENT = 1, PAGED = 2, PAGED2LEVEL = 4, INTERNAL_DECRYPTION = 8,
		SUPPORTED = 0x80000000, STEP_OUT = 0x40000000, STEP_OVER = 0x20000000, STEP_COND = 0x10000000, LENGTHMASK = 0xffff };
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
} // namespace util

// ---------------------------------------------------------------- device framework stubs
using device_type = const char *;
#define DEFINE_DEVICE_TYPE(TYPE, CLASS, SHORT, FULL) const device_type TYPE = SHORT;
#define DECLARE_DEVICE_TYPE(TYPE, CLASS) extern const device_type TYPE;
#define NAME(x) x
struct machine_config {};
enum { STATE_GENPC = -1, STATE_GENPCBASE = -2, STATE_GENFLAGS = -3 };
enum { AS_PROGRAM = 0 };
enum endianness_t { ENDIANNESS_LITTLE, ENDIANNESS_BIG };

class device_state_entry {
public:
	explicit device_state_entry(int i) : m_index(i) {}
	int index() const { return m_index; }
private:
	int m_index;
};
struct state_entry_builder {
	state_entry_builder &callimport() { return *this; }
	state_entry_builder &callexport() { return *this; }
	state_entry_builder &formatstr(const char *) { return *this; }
	state_entry_builder &noshow() { return *this; }
};

class address_space_config {
public:
	address_space_config(const char *, endianness_t, int, int, int) {}
};
class address_space {
public:
	virtual ~address_space() = default;
	virtual u8 read_byte(offs_t a) = 0;
	virtual u16 read_word(offs_t a) { return read_byte(a) | (read_byte(a + 1) << 8); }
	virtual u32 read_dword(offs_t a) { return read_word(a) | (read_word(a + 2) << 16); }
	virtual void write_byte(offs_t a, u8 d) = 0;
	virtual void write_word(offs_t a, u16 d) { write_byte(a, d & 0xff); write_byte(a + 1, d >> 8); }
	virtual void write_dword(offs_t a, u32 d) { write_word(a, d & 0xffff); write_word(a + 2, d >> 16); }
};
class running_machine {
public:
	std::string describe_context() const { return m_context ? m_context() : std::string("?"); }
	std::function<std::string()> m_context;
};
class device_memory_interface {
public:
	using space_config_vector = std::vector<std::pair<int, const address_space_config *>>;
};

[[noreturn]] inline void fatalerror(const char *fmt, ...) {
	va_list ap; va_start(ap, fmt); char buf[512]; vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
	throw std::runtime_error(buf);
}

class device_t {
public:
	device_t(const machine_config &, device_type, const char *, device_t *, u32 clock) : m_clock(clock) {}
	virtual ~device_t() = default;
	running_machine &machine() { return m_machine; }
	u32 clock() const { return m_clock; }
	template<typename... A> void logerror(const char *fmt, A&&... a) { fputs(util::string_format(fmt, std::forward<A>(a)...).c_str(), stderr); }
protected:
	virtual void device_start() {}
	virtual void device_reset() {}
	template<typename T> void save_item(T &) {}
	u32 m_clock;
	running_machine m_machine;
};

class cpu_device : public device_t, public device_memory_interface {
public:
	cpu_device(const machine_config &mc, device_type t, const char *tag, device_t *o, u32 clock) : device_t(mc, t, tag, o, clock) {}

	// host-side API (not MAME)
	void set_space(address_space *s) { m_space = s; }
	void start() { device_start(); }
	void reset() { device_reset(); }
	int run(int cycles) { *m_icountptr = cycles; execute_run(); return cycles - *m_icountptr; }
	std::function<u8(int level, u32 pc)> irq_ack;   // returns vector number, or 0 for none
	std::function<void(u32 pc)> instruction_hook;
	virtual std::unique_ptr<util::disasm_interface> create_disassembler() = 0;

protected:
	address_space &space(int) { return *m_space; }
	void set_icountptr(s32 &c) { m_icountptr = &c; }
	template<typename T> state_entry_builder state_add(int, const char *, T &) { return {}; }
	u8 standard_irq_callback(int level, u32 pc) { return irq_ack ? irq_ack(level, pc) : 0; }
	void debugger_instruction_hook(u32 pc) { if (instruction_hook) instruction_hook(pc); }
	virtual void execute_run() = 0;
	virtual void execute_set_input(int, int) {}
	virtual void state_import(const device_state_entry &) {}
	virtual void state_export(const device_state_entry &) {}
	virtual void state_string_export(const device_state_entry &, std::string &) const {}
	virtual space_config_vector memory_space_config() const = 0;

	s32 *m_icountptr = nullptr;
	address_space *m_space = nullptr;
};

using util::string_format;
inline u16 swapendian_int16(u16 v) { return (v << 8) | (v >> 8); }
inline u32 swapendian_int32(u32 v) { return (v << 24) | ((v << 8) & 0xff0000) | ((v >> 8) & 0xff00) | (v >> 24); }
