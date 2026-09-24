CXX ?= g++
CXXFLAGS ?= -O2 -g -std=c++17 -Wall -Wno-unused -Wno-sign-compare
OBJS = f2mc16.o f2mc16d.o pcm.o mc303.o gui.o
LDLIBS = $(shell pkg-config --libs sdl2 SDL2_ttf)
CXXFLAGS += $(shell pkg-config --cflags sdl2 SDL2_ttf)
mc303sim: $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJS) $(LDLIBS)
%.o: %.cpp emu.h f2mc16.h f2mc16d.h mc303.h
	$(CXX) $(CXXFLAGS) -c $<
clean:
	rm -f $(OBJS) mc303sim
