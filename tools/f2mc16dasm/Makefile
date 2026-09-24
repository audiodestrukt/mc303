CXXFLAGS ?= -O2 -std=c++17 -Wall -Wno-unused
f2mc16dasm: main.cpp f2mc16d.cpp f2mc16d.h emu.h
	$(CXX) $(CXXFLAGS) -o $@ main.cpp f2mc16d.cpp
clean:
	rm -f f2mc16dasm
