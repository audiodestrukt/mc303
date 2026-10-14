// SDL2 front panel for the MC-303 simulator, laid out like the real unit (service notes "PANEL LAYOUT").
// Every control is wired to the firmware through the board model: buttons press switch-matrix positions,
// knobs drive the ADC channels, the VALUE dial pulses the encoder inputs, LEDs and the two LB-603VP
// displays are read back from the scan buffer the firmware writes.
#include "mc303.h"
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <string>
#include <vector>
#include <cstring>
#include <cmath>
#include <functional>

static const int MACHINE_HZ = 12000000;
extern const int PCM_RATE;

// ------------------------------------------------------------------ panel description
// Coordinates are panel-relative pixels (panel = 1140 x 755, traced from the service-notes drawing).
enum Style { MCG, LCG, BLK, RED, WHITE_KEY, BLACK_KEY, TAPKEY };
struct Btn {
	const char *label, *sub;   // printed above / below (sub in a dark box = shifted function)
	int cx, cy, w, h;
	int col, bit;              // switch matrix position (-1 = TAP switch on P97)
	int lcol, lbit;            // LED (-1 = none)
	Style style;
};
static const Btn BTNS[] = {
	// top row: section buttons
	{"ARPEGGIO\nON", nullptr, 262, 180, 30, 16, 8, 4, -1, -1, MCG},
	{nullptr, nullptr, 383, 180, 30, 16, 8, 3, -1, -1, MCG},            // PLAY QUANTIZE select
	{nullptr, nullptr, 520, 180, 30, 16, 10, 2, -1, -1, MCG},           // EFFECT select
	{"RANDOM\nPAN", "PORTA", 583, 180, 30, 16, 10, 3, -1, -1, MCG},
	{nullptr, nullptr, 705, 180, 30, 16, 10, 4, -1, -1, MCG},           // LFO RATE/MOD
	{nullptr, nullptr, 1040, 180, 30, 16, 10, 5, -1, -1, MCG},          // ENVELOPE ATTACK/DECAY
	// part section
	{"PART SELECT", "QUANTIZE SELECT", 105, 262, 44, 18, 8, 0, 8, 0, MCG},
	{"PART MUTE", nullptr, 105, 318, 44, 18, 8, 2, 8, 2, MCG},
	{"RHYTHM MUTE", nullptr, 105, 374, 44, 18, 8, 1, 8, 1, MCG},
	{"R", "BD", 255, 262, 54, 18, 12, 0, 12, 0, MCG},
	{"1", "SD", 320, 262, 54, 18, 12, 1, 12, 1, MCG},
	{"2", "HH", 390, 262, 54, 18, 12, 2, 12, 2, MCG},
	{"3", "CLP", 455, 262, 54, 18, 12, 3, 12, 3, MCG},
	{"4", "CYM", 525, 262, 54, 18, 12, 4, 12, 4, MCG},
	{"5", "TOM/PERC", 595, 262, 54, 18, 12, 5, 12, 5, MCG},
	{"6", "HIT", 660, 262, 54, 18, 12, 6, 12, 6, MCG},
	{"7", "OTHERS", 725, 262, 54, 18, 12, 7, 12, 7, MCG},
	// select / transport
	{nullptr, nullptr, 800, 300, 36, 18, 10, 6, -1, -1, MCG},           // SELECT <
	{nullptr, nullptr, 885, 300, 36, 18, 10, 7, -1, -1, MCG},           // SELECT >
	{"BWD/TIE", nullptr, 965, 262, 50, 18, 2, 0, -1, -1, MCG},
	{"FWD/REST", nullptr, 1035, 262, 50, 18, 2, 1, -1, -1, MCG},
	{"REC", nullptr, 965, 318, 50, 18, 2, 2, 9, 6, RED},
	{"STOP", nullptr, 965, 375, 50, 18, 2, 3, -1, -1, MCG},
	{"PLAY", nullptr, 1035, 375, 50, 18, 2, 4, 9, 5, MCG},
	{"ENTER", nullptr, 965, 453, 50, 18, 2, 5, -1, -1, MCG},
	{"EXIT", nullptr, 1035, 453, 50, 18, 2, 6, -1, -1, MCG},
	// keyboard pad
	{"OCTAVE -", nullptr, 245, 372, 40, 18, 4, 3, 3, 3, MCG},
	{"OCTAVE +", nullptr, 290, 372, 40, 18, 4, 4, 3, 4, MCG},
	{"TRANSPOSE", "ERASE", 345, 372, 40, 18, 4, 5, 3, 2, MCG},
	{"PTN SET", nullptr, 245, 432, 40, 18, 4, 0, 3, 5, MCG},
	{"RPS SET", nullptr, 293, 432, 40, 18, 4, 1, 3, 6, MCG},
	{"PLAY MODE", "DEMO", 345, 432, 40, 18, 4, 2, -1, -1, LCG},
	// left column
	{"SCALE/MEASURE", nullptr, 105, 472, 40, 18, 11, 1, -1, -1, MCG},
	{"FUNC", nullptr, 105, 535, 40, 18, 11, 3, 11, 0, LCG},      // 11/3 gives the FUNC (STYLE..SYSTEM) pad functions
	{"SHIFT", "EDIT", 105, 590, 40, 18, 11, 2, -1, -1, LCG},
	// TAP
	{"TAP", "LOOP REST", 1040, 585, 50, 72, -1, 0, -1, -1, TAPKEY},
};
struct Led { const char *label; int x, y; int col, bit; bool green; int lx, ly; };   // label drawn centred at (x+lx, y+ly)
static const Led LEDS[] = {
	{"GRID", 374, 112, 8, 5, false, 24, -6}, {"GROOVE", 374, 130, 8, 6, false, 28, -6}, {"SHUFFLE", 374, 148, 8, 7, false, 30, -6},
	{"DELAY/REVERB \xe2\x96\xad", 522, 118, 10, 2, false, 0, -24}, {"FLANGER/CHORUS \xe2\x9c\xb3", 522, 118, -1, -1, false, 0, 8},
	{"", 762, 86, 10, 0, false, 0, 0},
	{"RATE \xe2\x96\xad", 705, 118, 10, 4, false, 0, -24}, {"MOD \xe2\x9c\xb3", 705, 118, -1, -1, false, 0, 8},
	{"ATTACK \xe2\x96\xad", 1040, 118, 10, 5, false, 0, -24}, {"DECAY \xe2\x9c\xb3", 1040, 118, -1, -1, false, 0, 8},
	{"TEMPO", 790, 272, 9, 0, false, 0, -30}, {"PTN SET", 815, 272, 9, 1, false, 0, -18}, {"PTN/SONG", 845, 272, 9, 2, false, 0, -30},
	{"RPS SET", 873, 272, 9, 3, false, 0, -18}, {"TONE", 900, 272, 9, 4, false, 0, -30},
	{"SONG", 410, 418, 3, 0, true, 26, -6}, {"PATTERN", 410, 438, 3, 1, false, 30, -6}, {"STEP REC", 715, 430, 3, 7, false, -30, -6},
	{"", 152, 465, 11, 4, false, 0, 0}, {"", 152, 485, 11, 5, false, 0, 0}, {"", 152, 505, 11, 6, false, 0, 0}, {"", 152, 525, 11, 7, false, 0, 0},
};
// ch = ADC channel (74HC4051 input). Panel pots VR2..VR8 (left to right) reach main-board VR6..VR0 through the
// straight CN4/CN2 cable, and RA10/RA11 route VR0..VR6 to MUX3,5,0,7,1,6,2; MUX4 is the backup battery.
struct Knob { const char *label, *sub; int cx, cy, r; int ch; };   // ch = -1: VOLUME (analogue only)
static const Knob KNOBS[] = {
	{"VOLUME", nullptr, 100, 130, 24, -1},
	{"ACCENT RATE", "OCTAVE RANGE", 200, 130, 24, 2},
	{"TIMING", "VELOCITY", 330, 130, 24, 6},
	{"TIME/RATE", "EFX LEVEL", 460, 130, 24, 1},
	{"LFO", "PORTA TIME", 645, 130, 24, 7},
	{"CUTOFF", "LEVEL", 775, 130, 24, 0},
	{"RESONANCE", "PANPOT", 885, 130, 24, 5},
	{"ENVELOPE", "RELEASE", 980, 130, 24, 3},
};
static const char *EDIT1[16] = {"COPY","PART COPY","ERASE","DELETE","INSERT","TRANSPOSE","CHG VELO","CHG GATE","SHIFT CLK","DATA THIN","QUANTIZE","","VARI WRITE","PTN SET W","SET UP W","DUMP"};
static const char *EDIT2[16] = {"STYLE","GRID","GROOVE","SHUFFLE","TYPE","PART LEVEL","TYPE","PART LEVEL","WAVE","PITCH","FILTER","AMP","BEND RANGE","OUT ASSIGN","","SYSTEM"};
static const bool BLACK[16] = {0,0,1,0,1,0,0,1,0,1,0,1,0,0,1,0};
static const int KEY_X0 = 160, KEY_PITCH = 52, KEY_W = 50, KEY_Y = 545, KEY_H = 80;
static const int VALUE_CX = 845, VALUE_CY = 410, VALUE_R = 58;
static const int PX = 20, PY = 20;             // panel origin in the window
static const int WIN_W = 1180, WIN_H = 830;

// ------------------------------------------------------------------ drawing helpers
static SDL_Renderer *R;
static TTF_Font *F8, *F9, *F10, *F11B, *F16B, *F22B, *C8;
static void col(int r, int g, int b, int a = 255) { SDL_SetRenderDrawColor(R, r, g, b, a); }
static void frect(int x, int y, int w, int h) { SDL_Rect rc{x, y, w, h}; SDL_RenderFillRect(R, &rc); }
static void drect(int x, int y, int w, int h) { SDL_Rect rc{x, y, w, h}; SDL_RenderDrawRect(R, &rc); }
static void fcircle(int cx, int cy, int r)
{
	for (int dy = -r; dy <= r; dy++) { int dx = (int)std::sqrt((double)(r*r - dy*dy)); SDL_RenderDrawLine(R, cx - dx, cy + dy, cx + dx, cy + dy); }
}
static void frrect(int x, int y, int w, int h, int r)
{
	frect(x + r, y, w - 2*r, h); frect(x, y + r, r, h - 2*r); frect(x + w - r, y + r, r, h - 2*r);
	fcircle(x + r, y + r, r); fcircle(x + w - r - 1, y + r, r); fcircle(x + r, y + h - r - 1, r); fcircle(x + w - r - 1, y + h - r - 1, r);
}
enum Align { L, C, Rt };
static int text(TTF_Font *f, int x, int y, const std::string &s, SDL_Color c, Align al = L)
{
	if (s.empty()) return 0;
	SDL_Surface *su = TTF_RenderUTF8_Blended(f, s.c_str(), c);
	if (!su) return 0;
	SDL_Texture *tx = SDL_CreateTextureFromSurface(R, su);
	int dx = al == C ? x - su->w / 2 : al == Rt ? x - su->w : x;
	SDL_Rect d{dx, y, su->w, su->h};
	SDL_RenderCopy(R, tx, nullptr, &d);
	int w = su->w; SDL_DestroyTexture(tx); SDL_FreeSurface(su);
	return w;
}
static const SDL_Color WHITE{235, 235, 235, 255}, GREY{170, 172, 178, 255}, DIM{110, 112, 118, 255}, BLACKC{20, 20, 22, 255};
// label in a dark rounded box (shifted / FUNC functions are printed white on black on the real panel)
static void boxlabel(TTF_Font *f, int cx, int y, const std::string &s)
{
	int w, h; TTF_SizeUTF8(f, s.c_str(), &w, &h);
	col(16, 16, 18); frrect(cx - w/2 - 4, y, w + 8, h, 3);
	text(f, cx, y, s, WHITE, C);
}
static void led(int x, int y, bool lit, bool green, int r = 4)
{
	if (green) col(lit ? 70 : 20, lit ? 230 : 55, lit ? 80 : 25); else col(lit ? 255 : 80, lit ? 60 : 24, lit ? 40 : 20);
	fcircle(x, y, r);
	if (lit) { col(255, 200, 180, 90); fcircle(x, y, r + 3); }
}
// 7-segment digit: v bits 0..6 = A..G, bit 7 = DP, lit when 0 (LB-603VP, common cathode via sink driver)
static void digit(int x, int y, int w, int h, u8 v)
{
	int t = w / 5;
	auto seg = [&](int b, int sx, int sy, int sw, int sh) { bool lit = !((v >> b) & 1); col(lit ? 255 : 48, lit ? 48 : 16, lit ? 28 : 12); frect(sx, sy, sw, sh); };
	seg(0, x + t, y, w - 2*t, t);
	seg(1, x + w - t, y + t, t, h/2 - t);
	seg(2, x + w - t, y + h/2, t, h/2 - t);
	seg(3, x + t, y + h - t, w - 2*t, t);
	seg(4, x, y + h/2, t, h/2 - t);
	seg(5, x, y + t, t, h/2 - t);
	seg(6, x + t, y + h/2 - t/2, w - 2*t, t);
	seg(7, x + w + 2, y + h - t, t, t);
}
static void knob(int cx, int cy, int r, int value /*0-255*/)
{
	col(10, 10, 12); fcircle(cx, cy, r + 2);
	col(44, 44, 48); fcircle(cx, cy, r);
	col(28, 28, 30); fcircle(cx, cy, r - 6);
	double a = (-135.0 + 270.0 * value / 255.0) * M_PI / 180.0;   // 7 o'clock .. 5 o'clock
	col(240, 240, 240);
	for (int i = -1; i <= 1; i++) SDL_RenderDrawLine(R, cx + (int)((r - 14) * std::sin(a)) + i, cy - (int)((r - 14) * std::cos(a)), cx + (int)((r - 3) * std::sin(a)) + i, cy - (int)((r - 3) * std::cos(a)));
	// scale ticks
	col(150, 150, 155);
	for (int k = 0; k <= 10; k++) { double b = (-135.0 + 27.0 * k) * M_PI / 180.0; SDL_RenderDrawLine(R, cx + (int)((r + 4) * std::sin(b)), cy - (int)((r + 4) * std::cos(b)), cx + (int)((r + 7) * std::sin(b)), cy - (int)((r + 7) * std::cos(b))); }
}
static void button(const Btn &b, bool pressed, bool lit)
{
	int x = PX + b.cx - b.w/2, y = PY + b.cy - b.h/2;
	switch (b.style) {
	case LCG: col(pressed ? 150 : 190, pressed ? 150 : 190, pressed ? 152 : 192); break;
	case RED: col(pressed ? 120 : 170, 30, 30); break;
	case BLK: case TAPKEY: col(pressed ? 40 : 18, pressed ? 40 : 18, pressed ? 42 : 20); break;
	default: col(pressed ? 60 : 84, pressed ? 60 : 84, pressed ? 64 : 88); break;
	}
	frrect(x, y, b.w, b.h, 3);
	col(0, 0, 0, 120); drect(x, y, b.w, b.h);
	if (b.lcol >= 0) { col(lit ? 255 : 90, lit ? 70 : 28, lit ? 40 : 20); frect(x + b.w/2 - 6, y + 3, 12, 3); }
	if (b.label) {
		std::string s = b.label; size_t nl = s.find('\n');
		if (nl == std::string::npos) text(b.style == TAPKEY ? F9 : C8, PX + b.cx, y - 12, s, b.style == TAPKEY ? WHITE : GREY, C);
		else { text(C8, PX + b.cx, y - 22, s.substr(0, nl), GREY, C); text(C8, PX + b.cx, y - 12, s.substr(nl + 1), GREY, C); }
	}
	if (b.sub) boxlabel(C8, PX + b.cx, y + b.h + 2, b.sub);
}

static void audio_cb(void *ud, Uint8 *stream, int len)
{
	MC303Board *b = (MC303Board *)ud; s16 *out = (s16 *)stream; int n = len / 2;
	std::lock_guard<std::mutex> g(b->audio_mutex);
	for (int i = 0; i < n; i++) { if (b->audio_ring.empty()) out[i] = 0; else { out[i] = b->audio_ring.front(); b->audio_ring.pop_front(); } }
}

// ------------------------------------------------------------------ main loop
static bool lit_at(const MC303Board &b, int c, int bit) { return c >= 0 && !((b.leds[c] >> bit) & 1); }
static void key_pos(int n /*0-15*/, int &c, int &bit) { c = n < 8 ? 1 : 0; bit = n & 7; }

int run_gui(MC303Board &board, f2mc16_device &cpu, const char *shot_path, double shot_at, const std::function<void(double)> &timed_events)
{
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0 || TTF_Init() != 0) { fprintf(stderr, "SDL init failed: %s\n", SDL_GetError()); return 1; }
	SDL_Window *win = SDL_CreateWindow("Roland MC-303 simulator", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, WIN_W, WIN_H, SDL_WINDOW_SHOWN);
	R = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
	if (!R) R = SDL_CreateRenderer(win, -1, 0);
	SDL_SetRenderDrawBlendMode(R, SDL_BLENDMODE_BLEND);
	const char *sans = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", *bold = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf";
	F8 = TTF_OpenFont(sans, 8); F9 = TTF_OpenFont(sans, 9); F10 = TTF_OpenFont(sans, 10); F11B = TTF_OpenFont(bold, 11); F16B = TTF_OpenFont(bold, 16); F22B = TTF_OpenFont(bold, 22);
	C8 = TTF_OpenFont("/usr/share/fonts/truetype/dejavu/DejaVuSansCondensed.ttf", 8); if (!C8) C8 = F8;
	if (!F8 || !F9 || !F10 || !F11B || !F16B || !F22B) { fprintf(stderr, "font load failed (DejaVu fonts needed)\n"); return 1; }

	SDL_AudioSpec want{}, have{}; want.freq = PCM_RATE; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 1024; want.callback = audio_cb; want.userdata = &board;
	SDL_AudioDeviceID adev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
	if (adev) { SDL_PauseAudioDevice(adev, 0); fprintf(stderr, "audio: %d Hz, %d ch, buffer %d\n", have.freq, have.channels, have.samples); } else fprintf(stderr, "no audio device: %s\n", SDL_GetError());

	int volume = 200;
	int held_btn = -1, held_key = -1; bool held_tap = false;
	int drag_knob = -1, drag_y0 = 0, drag_v0 = 0; bool drag_value = false; int drag_x0 = 0, value_acc = 0;
	bool running = true;
	u64 last = SDL_GetPerformanceCounter();
	const int NB = sizeof(BTNS) / sizeof(BTNS[0]), NK = sizeof(KNOBS) / sizeof(KNOBS[0]), NL = sizeof(LEDS) / sizeof(LEDS[0]);
	// computer keyboard -> pads, piano style (sharps on the upper row): pad n = PADKEYS[n]
	static const char *PADKEYS = "zxscdvbgnhmj,.l/";
	auto press_sw = [&](int c, int bit, bool on) { if (c < 0) { board.tap = on; return; } if (on) board.switches[c] |= 1 << bit; else board.switches[c] &= ~(1 << bit); };

	while (running) {
		SDL_Event e;
		while (SDL_PollEvent(&e)) {
			if (e.type == SDL_QUIT) running = false;
			else if ((e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) && !e.key.repeat) {
				bool dn = e.type == SDL_KEYDOWN; SDL_Keycode k = e.key.keysym.sym;
				if (k == SDLK_ESCAPE && dn) running = false;
				else if (k == SDLK_SPACE) press_sw(2, 4, dn);                 // PLAY
				else if (k == SDLK_BACKSPACE) press_sw(2, 3, dn);             // STOP
				else if (k == SDLK_RETURN) press_sw(2, 5, dn);                // ENTER
				else if (k == SDLK_LSHIFT || k == SDLK_RSHIFT) press_sw(11, 2, dn);
				else if (k == SDLK_LCTRL || k == SDLK_RCTRL) press_sw(11, 3, dn);   // FUNC
				else if (k == SDLK_LALT || k == SDLK_RALT) press_sw(11, 1, dn);     // SCALE
				else if (k == SDLK_TAB) press_sw(-1, 0, dn);                  // TAP
				else if (k == SDLK_RIGHT || k == SDLK_UP) { if (dn) board.encoder(+1); }
				else if (k == SDLK_LEFT || k == SDLK_DOWN) { if (dn) board.encoder(-1); }
				else if (k >= SDLK_F1 && k <= SDLK_F8) press_sw(12, k - SDLK_F1, dn);   // part buttons R,1-7
				else if (k < 128) {
					const char *p = strchr(PADKEYS, (char)k);
					if (p) { int c, bit; key_pos(p - PADKEYS, c, bit); press_sw(c, bit, dn); }
					else {   // top row: MIDI notes in on channel 1 (a fallback while the pads are being debugged)
						static const char *high = "q2w3er5t6y7ui9o0p"; const char *ph = strchr(high, (char)k);
						if (ph) { board.midi_in.push_back(dn ? 0x90 : 0x80); board.midi_in.push_back(60 + (ph - high)); board.midi_in.push_back(dn ? 100 : 0); }
					}
				}
			}
			else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
				int x = e.button.x - PX, y = e.button.y - PY;
				for (int i = 0; i < NB; i++) { const Btn &b = BTNS[i]; if (x >= b.cx - b.w/2 && x < b.cx + b.w/2 && y >= b.cy - b.h/2 && y < b.cy + b.h/2) { held_btn = i; press_sw(b.col, b.bit, true); } }
				for (int i = 0; i < NK; i++) { const Knob &k = KNOBS[i]; if ((x - k.cx)*(x - k.cx) + (y - k.cy)*(y - k.cy) <= (k.r + 8)*(k.r + 8)) { drag_knob = i; drag_y0 = y; drag_v0 = k.ch < 0 ? volume : board.knobs[k.ch]; } }
				if ((x - VALUE_CX)*(x - VALUE_CX) + (y - VALUE_CY)*(y - VALUE_CY) <= VALUE_R*VALUE_R) { drag_value = true; drag_x0 = x; value_acc = 0; }
				if (held_btn < 0 && y >= KEY_Y && y < KEY_Y + KEY_H && x >= KEY_X0 && x < KEY_X0 + 16*KEY_PITCH) { held_key = (x - KEY_X0) / KEY_PITCH; int c, bit; key_pos(held_key, c, bit); press_sw(c, bit, true); }
			}
			else if (e.type == SDL_MOUSEBUTTONUP && e.button.button == SDL_BUTTON_LEFT) {
				if (held_btn >= 0) press_sw(BTNS[held_btn].col, BTNS[held_btn].bit, false);
				if (held_key >= 0) { int c, bit; key_pos(held_key, c, bit); press_sw(c, bit, false); }
				held_btn = held_key = -1; drag_knob = -1; drag_value = false;
			}
			else if (e.type == SDL_MOUSEMOTION) {
				int x = e.motion.x - PX, y = e.motion.y - PY;
				if (drag_knob >= 0) { int v = drag_v0 + (drag_y0 - y) * 2; v = v < 0 ? 0 : v > 255 ? 255 : v; if (KNOBS[drag_knob].ch < 0) volume = v; else board.knobs[KNOBS[drag_knob].ch] = v; }
				if (drag_value) { value_acc += x - drag_x0; drag_x0 = x; while (value_acc >= 8) { board.encoder(+1); value_acc -= 8; } while (value_acc <= -8) { board.encoder(-1); value_acc += 8; } }
			}
			else if (e.type == SDL_MOUSEWHEEL) {
				int mx, my; SDL_GetMouseState(&mx, &my); int x = mx - PX, y = my - PY;
				bool onknob = false;
				for (int i = 0; i < NK; i++) { const Knob &k = KNOBS[i]; if ((x - k.cx)*(x - k.cx) + (y - k.cy)*(y - k.cy) <= (k.r + 8)*(k.r + 8)) { onknob = true; int v = (k.ch < 0 ? volume : board.knobs[k.ch]) + e.wheel.y * 8; v = v < 0 ? 0 : v > 255 ? 255 : v; if (k.ch < 0) volume = v; else board.knobs[k.ch] = v; } }
				if (!onknob) board.encoder(e.wheel.y > 0 ? +1 : -1);
			}
		}
		// run the emulation for the wall-clock time elapsed (capped)
		u64 now = SDL_GetPerformanceCounter();
		double dt = (double)(now - last) / SDL_GetPerformanceFrequency(); last = now;
		if (dt > 0.05) dt = 0.05;
		if (shot_path) dt = 0.02;
		u64 target = board.cycles + (u64)(dt * MACHINE_HZ);
		if (timed_events) timed_events((double)board.cycles / MACHINE_HZ);   // scripted presses / MIDI / knob moves (--press etc.)
		try {
			while (board.cycles < target) { int ran = cpu.run(100); board.step(ran > 0 ? ran : 100); }
		} catch (std::exception &ex) { fprintf(stderr, "STOP: %s\n", ex.what()); running = false; }

		// ---------------------------------------------------------------- draw
		col(50, 50, 54); SDL_RenderClear(R);
		col(34, 35, 39); frrect(PX, PY, 1140, 755, 8);                          // panel
		col(0, 0, 0, 100); drect(PX, PY, 1140, 755);
		text(F22B, PX + 55, PY + 38, "Roland", WHITE); text(F22B, PX + 150, PY + 38, "MC-303", WHITE);
		text(F8, PX + 130, PY + 18, "LOW BOOST", GREY);
		{ col(190, 190, 195); frrect(PX + 795, PY + 36, 270, 24, 12); text(F16B, PX + 930, PY + 38, "groovebox", BLACKC, C); }
		// section boxes in the knob row
		auto section = [&](int x, int w, const char *title, int tx) { col(22, 22, 25); frrect(PX + x, PY + 80, w, 115, 5); col(70, 70, 76); drect(PX + x, PY + 80, w, 115); col(60, 60, 66); frect(PX + x, PY + 80, w, 12); text(F8, PX + x + (tx ? tx : w/2), PY + 81, title, WHITE, tx ? L : C); };
		section(55, 100, "VOLUME", 0); section(165, 120, "ARPEGGIO", 0); section(295, 120, "PLAY QUANTIZE", 0); section(425, 125, "EFFECT", 0); section(555, 510, "REALTIME MODIFY", 215);
		for (int i = 0; i < NK; i++) {
			const Knob &k = KNOBS[i]; int v = k.ch < 0 ? volume : board.knobs[k.ch];
			knob(PX + k.cx, PY + k.cy, k.r, v);
			text(F9, PX + k.cx, PY + k.cy + k.r + 10, k.label, WHITE, C);
			if (k.sub) boxlabel(F8, PX + k.cx, PY + k.cy + k.r + 22, k.sub);
			if (k.ch < 0) { text(F8, PX + k.cx - 30, PY + k.cy + 18, "MIN", GREY); text(F8, PX + k.cx + 18, PY + k.cy + 18, "MAX", GREY); }
		}
		// part section frame and labels
		col(70, 70, 76); drect(PX + 225, PY + 245, 265, 36); drect(PX + 495, PY + 245, 265, 36);
		text(F9, PX + 495, PY + 232, "PART", GREY, C);
		text(F8, PX + 355, PY + 290, "VELOCITY", GREY, C); text(F8, PX + 625, PY + 290, "GATE TIME", GREY, C);
		col(90, 90, 96); SDL_RenderDrawLine(R, PX + 240, PY + 296, PX + 475, PY + 296); SDL_RenderDrawLine(R, PX + 510, PY + 296, PX + 745, PY + 296);
		text(F9, PX + 770, PY + 294, "\xe2\x97\x80", GREY); text(F9, PX + 910, PY + 294, "\xe2\x96\xb6", GREY);
		text(F8, PX + 843, PY + 296, "SELECT", GREY, C);
		col(70, 70, 76); drect(PX + 775, PY + 250, 150, 38); drect(PX + 930, PY + 245, 130, 170); drect(PX + 930, PY + 438, 130, 32);
		text(F8, PX + 965, PY + 335, "\xe2\x97\x80\xe2\x97\x80", GREY, C); text(F8, PX + 1035, PY + 335, "\xe2\x96\xb6\xe2\x96\xb6", GREY, C);
		text(F8, PX + 965, PY + 394, "\xe2\x96\xa0", GREY, C); text(F8, PX + 1035, PY + 394, "\xe2\x96\xb6", GREY, C);
		led(PX + 965, PY + 337, lit_at(board, 9, 6), false, 3); led(PX + 1035, PY + 396, lit_at(board, 9, 5), true, 3);
		// keyboard pad frame
		col(70, 70, 76); drect(PX + 220, PY + 355, 150, 36); drect(PX + 220, PY + 415, 100, 36);
		text(F8, PX + 268, PY + 342, "KBD PAD", GREY, C);
		for (int i = 0; i < NB; i++) {
			const Btn &b = BTNS[i];
			bool pressed = b.col < 0 ? board.tap : (board.switches[b.col] >> b.bit) & 1;
			button(b, pressed, lit_at(board, b.lcol, b.lbit));
		}
		// LEDs
		for (int i = 0; i < NL; i++) { const Led &l = LEDS[i]; if (l.col >= 0) led(PX + l.x, PY + l.y, lit_at(board, l.col, l.bit), l.green); if (*l.label) text(C8, PX + l.x + l.lx, PY + l.y + l.ly, l.label, GREY, C); }
		// VALUE dial
		{ col(10, 10, 12); fcircle(PX + VALUE_CX, PY + VALUE_CY, VALUE_R + 3); col(48, 48, 52); fcircle(PX + VALUE_CX, PY + VALUE_CY, VALUE_R);
		  col(70, 70, 76); for (int k = 0; k < 24; k++) { double a = k * M_PI / 12; SDL_RenderDrawLine(R, PX + VALUE_CX + (int)((VALUE_R - 6) * std::sin(a)), PY + VALUE_CY - (int)((VALUE_R - 6) * std::cos(a)), PX + VALUE_CX + (int)(VALUE_R * std::sin(a)), PY + VALUE_CY - (int)(VALUE_R * std::cos(a))); }
		  col(30, 30, 33); fcircle(PX + VALUE_CX - 10, PY + VALUE_CY - 28, 14);
		  text(F9, PX + VALUE_CX, PY + VALUE_CY + VALUE_R + 6, "VALUE", GREY, C); }
		// display window
		{ int dx = PX + 390, dy = PY + 320; col(190, 190, 195); frrect(dx - 3, dy - 3, 366, 141, 4); col(8, 8, 10); frrect(dx, dy, 360, 135, 3);
		  text(F9, dx + 100, dy + 6, "CURRENT", GREY, C); text(F9, dx + 270, dy + 6, "NEXT", GREY, C);
		  col(28, 10, 10); frect(dx + 30, dy + 24, 142, 76); frect(dx + 200, dy + 24, 142, 76);
		  for (int i = 0; i < 3; i++) digit(dx + 40 + i * 44, dy + 32, 30, 60, board.leds[7 - i]);
		  for (int i = 0; i < 3; i++) digit(dx + 210 + i * 44, dy + 32, 30, 60, board.leds[13 + i]);
		  text(F16B, dx + 186, dy + 50, "\xe2\x96\xb6", GREY, C);
		  bool br = board.beat_red(), bg = board.beat_green();
		  col(br && bg ? 255 : br ? 255 : bg ? 60 : 40, br && bg ? 180 : br ? 60 : bg ? 230 : 30, 30); fcircle(dx + 180, dy + 110, 4);
		  text(F8, dx + 180, dy + 118, "BEAT", GREY, C); }
		// scale bars with SCALE LEDs
		for (int i = 0; i < 4; i++) { col(12, 12, 14); frrect(PX + 145, PY + 458 + i * 20, i < 2 ? 630 : 840, 14, 3); }
		// keys
		for (int n = 0; n < 16; n++) {
			int x = PX + KEY_X0 + n * KEY_PITCH, y = PY + KEY_Y; int c, bit; key_pos(n, c, bit);
			bool pressed = (board.switches[c] >> bit) & 1;
			if (BLACK[n]) { col(pressed ? 50 : 22, pressed ? 50 : 22, pressed ? 54 : 25); frrect(x, y, KEY_W, KEY_H, 4); col(pressed ? 60 : 32, pressed ? 60 : 32, pressed ? 64 : 36); frrect(x + 4, y + 16, KEY_W - 8, KEY_H - 16, 3); }
			else { col(pressed ? 150 : 196, pressed ? 150 : 196, pressed ? 152 : 198); frrect(x, y, KEY_W, KEY_H, 4); col(pressed ? 135 : 180, pressed ? 135 : 180, pressed ? 138 : 183); frrect(x + 4, y + 16, KEY_W - 8, KEY_H - 16, 3); }
			col(0, 0, 0, 140); drect(x, y, KEY_W, KEY_H);
			bool lit = lit_at(board, c, bit); col(lit ? 255 : 70, lit ? 60 : 24, lit ? 40 : 20); frect(x + KEY_W/2 - 7, y + 7, 14, 3);
			text(F9, x + KEY_W/2, y - 14, std::to_string(n + 1), WHITE, C);
			if (*EDIT1[n]) boxlabel(C8, x + KEY_W/2, y + KEY_H + 4, EDIT1[n]);
			if (*EDIT2[n]) boxlabel(C8, x + KEY_W/2, y + KEY_H + 17, EDIT2[n]);
		}
		{ struct G { int a, b; const char *n; } gs[] = {{0, 0, "ARPEGGIO"}, {1, 3, "PLAY QUANTIZE"}, {4, 5, "DELAY/REVERB"}, {6, 7, "FLANGER/CHORUS"}, {8, 11, "LFO"}};
		  for (auto &g : gs) { int xa = PX + KEY_X0 + g.a * KEY_PITCH + 4, xb = PX + KEY_X0 + g.b * KEY_PITCH + KEY_W - 4, yy = PY + KEY_Y + KEY_H + 32;
		    if (g.a != g.b) { col(120, 120, 126); SDL_RenderDrawLine(R, xa, yy, xb, yy); SDL_RenderDrawLine(R, xa, yy, xa, yy - 4); SDL_RenderDrawLine(R, xb, yy, xb, yy - 4); }
		    text(C8, (xa + xb) / 2, yy + 2, g.n, GREY, C); } }
		// status line under the panel
		std::string status = std::string("wave ROM: ") + (board.waverom_loaded ? "loaded" : "none (silent)") + "   audio: " + (adev ? "on" : "OFF") +
			"   MIDI out: " + std::to_string(board.midi_out.size()) + " B" +
			"   emulated " + std::to_string((int)(board.cycles / MACHINE_HZ)) + " s   display " + board.display_text();
		text(F10, PX, PY + 762, status, GREY);
		text(F9, PX, PY + 778, "mouse: click buttons/keys, drag or wheel knobs and the VALUE dial.  keys: Space PLAY, Backspace STOP, Enter, Shift/Ctrl/Alt = SHIFT/FUNC/SCALE, Tab TAP, arrows = VALUE, F1-F8 parts, Z.. = pads, Q.. = MIDI notes, Esc quits", DIM);
		SDL_RenderPresent(R);
		if (shot_path && (double)board.cycles / MACHINE_HZ >= shot_at) {
			int w, h; SDL_GetRendererOutputSize(R, &w, &h);
			SDL_Surface *sf = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
			SDL_RenderReadPixels(R, nullptr, SDL_PIXELFORMAT_ARGB8888, sf->pixels, sf->pitch);
			SDL_SaveBMP(sf, shot_path); SDL_FreeSurface(sf);
			printf("saved %s\n", shot_path); running = false;
		}
	}
	if (adev) SDL_CloseAudioDevice(adev);
	for (TTF_Font *f : {F8, F9, F10, F11B, F16B, F22B}) TTF_CloseFont(f); if (C8 != F8) TTF_CloseFont(C8);
	SDL_DestroyRenderer(R); SDL_DestroyWindow(win); TTF_Quit(); SDL_Quit();
	return 0;
}
