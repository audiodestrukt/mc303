// SDL2 front panel for the MC-303 simulator: two LB-603VP displays, the 16x8 LED matrix and a
// clickable 16x8 switch matrix. Runs the emulation in real time (12 MHz machine clock).
#include "mc303.h"
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <string>
#include <cstring>

static const int MACHINE_HZ = 12000000;
extern const int PCM_RATE;

static void audio_cb(void *ud, Uint8 *stream, int len)
{
	MC303Board *b = (MC303Board *)ud; s16 *out = (s16 *)stream; int n = len / 2;
	std::lock_guard<std::mutex> g(b->audio_mutex);
	for (int i = 0; i < n; i++) { if (b->audio_ring.empty()) out[i] = 0; else { out[i] = b->audio_ring.front(); b->audio_ring.pop_front(); } }
}

// LED names by (column, bit) where identified from the schematic + experiments; "" = unknown
static const char *led_name(int col, int bit)
{
	static const char *col3[8] = {"SONG","PATTERN","TRANSPOSE","-OCT","OCT+","PTN SET","RPS SET","STEP REC"};
	if (col == 3) return col3[bit];
	if (col == 2) { static char b[8][8]; snprintf(b[bit], 8, "KEY%d", bit + 1); return b[bit]; }
	if (col == 4) { static char b[8][8]; snprintf(b[bit], 8, "KEY%d", bit + 9); return b[bit]; }
	return "";
}
static const char *sw_name(int col, int bit)
{
	if (col == 4 && bit == 3) return "OCT-";
	if (col == 4 && bit == 4) return "OCT+";
	if (col == 4 && bit == 5) return "TRANSP";
	if (col == 10 && bit == 6) return "PTN SET";
	if (col == 10 && bit == 7) return "RPS SET";
	if (col == 4 && bit == 2) return "PLAYMODE?";
	if (col == 8 && bit == 4) return "SCALE?";
	return "";
}

static void seg_rect(SDL_Renderer *r, int x, int y, int w, int h, bool lit)
{
	SDL_Rect rc{x, y, w, h};
	if (lit) SDL_SetRenderDrawColor(r, 255, 60, 30, 255); else SDL_SetRenderDrawColor(r, 60, 22, 18, 255);
	SDL_RenderFillRect(r, &rc);
}

// draw one digit: v bits 0..6 = A..G, bit7 = DP, lit when 0
static void draw_digit(SDL_Renderer *r, int x, int y, int s, u8 v)
{
	int t = s / 6, w = s, h = s * 2;
	auto lit = [&](int b) { return !((v >> b) & 1); };
	seg_rect(r, x + t, y, w - 2*t, t, lit(0));                    // A
	seg_rect(r, x + w - t, y + t, t, h/2 - t, lit(1));            // B
	seg_rect(r, x + w - t, y + h/2, t, h/2 - t, lit(2));          // C
	seg_rect(r, x + t, y + h - t, w - 2*t, t, lit(3));            // D
	seg_rect(r, x, y + h/2, t, h/2 - t, lit(4));                  // E
	seg_rect(r, x, y + t, t, h/2 - t, lit(5));                    // F
	seg_rect(r, x + t, y + h/2 - t/2, w - 2*t, t, lit(6));        // G
	seg_rect(r, x + w + t/2, y + h - t, t, t, lit(7));            // DP
}

static void text(SDL_Renderer *r, TTF_Font *f, int x, int y, const std::string &s, SDL_Color c = {200, 200, 200, 255})
{
	if (s.empty()) return;
	SDL_Surface *su = TTF_RenderUTF8_Blended(f, s.c_str(), c);
	if (!su) return;
	SDL_Texture *tx = SDL_CreateTextureFromSurface(r, su);
	SDL_Rect d{x, y, su->w, su->h};
	SDL_RenderCopy(r, tx, nullptr, &d);
	SDL_DestroyTexture(tx); SDL_FreeSurface(su);
}

int run_gui(MC303Board &board, f2mc16_device &cpu, const char *shot_path, double shot_at)
{
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0 || TTF_Init() != 0) { fprintf(stderr, "SDL init failed: %s\n", SDL_GetError()); return 1; }
	SDL_Window *win = SDL_CreateWindow("MC-303 simulator", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1000, 620, SDL_WINDOW_SHOWN);
	SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
	if (!ren) ren = SDL_CreateRenderer(win, -1, 0);
	TTF_Font *font = TTF_OpenFont("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 11);
	TTF_Font *big = TTF_OpenFont("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", 16);
	if (!font || !big) { fprintf(stderr, "font load failed\n"); return 1; }

	SDL_AudioSpec want{}, have{}; want.freq = PCM_RATE; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 1024; want.callback = audio_cb; want.userdata = &board;
	SDL_AudioDeviceID adev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
	if (adev) { SDL_PauseAudioDevice(adev, 0); fprintf(stderr, "audio: %d Hz, %d ch, buffer %d\n", have.freq, have.channels, have.samples); } else fprintf(stderr, "no audio device: %s\n", SDL_GetError());
	const int GX = 30, GY = 190, CW = 58, CH = 34;      // LED grid
	const int SX = 30, SY = 470, SW = 58, SH = 17;      // switch grid
	int held_col = -1, held_bit = -1;
	bool running = true;
	u64 last = SDL_GetPerformanceCounter();
	while (running) {
		SDL_Event e;
		while (SDL_PollEvent(&e)) {
			if (e.type == SDL_QUIT) running = false;
			else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) running = false;
			else if ((e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) && !e.key.repeat) {
				// computer keyboard as a MIDI keyboard: Z..M = C3.., Q..P = C4.., 1-8 = drums on channel 10
				static const char *low = "zsxdcvgbhnjm,l.;/", *high = "q2w3er5t6y7ui9o0p";
				int note = -1, chan = 0;
				SDL_Keycode k = e.key.keysym.sym;
				if (k >= '1' && k <= '8') { note = 36 + (k - '1'); chan = 9; }
				else if (k < 128) {
					const char *pl = strchr(low, (char)k), *ph = strchr(high, (char)k);
					if (pl) note = 48 + (pl - low); else if (ph) note = 60 + (ph - high);
				}
				if (note >= 0) {
					board.midi_in.push_back((e.type == SDL_KEYDOWN ? 0x90 : 0x80) | chan);
					board.midi_in.push_back(note); board.midi_in.push_back(e.type == SDL_KEYDOWN ? 100 : 0);
				}
			}
			else if (e.type == SDL_MOUSEBUTTONDOWN) {
				int x = e.button.x, y = e.button.y;
				if (x >= SX && x < SX + 16*SW && y >= SY && y < SY + 8*SH) {
					held_col = (x - SX) / SW; held_bit = (y - SY) / SH;
					board.switches[held_col] |= 1 << held_bit;
				}
			}
			else if (e.type == SDL_MOUSEBUTTONUP) {
				if (held_col >= 0) board.switches[held_col] &= ~(1 << held_bit);
				held_col = held_bit = -1;
			}
		}
		// run the emulation for the wall-clock time elapsed (capped)
		u64 now = SDL_GetPerformanceCounter();
		double dt = (double)(now - last) / SDL_GetPerformanceFrequency(); last = now;
		if (dt > 0.05) dt = 0.05;
		if (shot_path) dt = 0.02;                       // deterministic pacing when taking a screenshot
		u64 target = board.cycles + (u64)(dt * MACHINE_HZ);
		try {
			while (board.cycles < target) { int ran = cpu.run(100); board.step(ran > 0 ? ran : 100); }
		} catch (std::exception &ex) { fprintf(stderr, "STOP: %s\n", ex.what()); running = false; }

		SDL_SetRenderDrawColor(ren, 28, 28, 32, 255);
		SDL_RenderClear(ren);
		text(ren, big, 30, 12, "Roland MC-303   (emulated: " + std::to_string((int)(board.cycles / MACHINE_HZ)) + " s)");
		// displays
		int dx = 30, dy = 50, ds = 36;
		for (int i = 0; i < 3; i++) draw_digit(ren, dx + i * (ds + 16), dy, ds, board.leds[7 - i]);
		for (int i = 0; i < 3; i++) draw_digit(ren, dx + 260 + i * (ds + 16), dy, ds, board.leds[13 + i]);
		text(ren, font, dx, dy + 2*ds + 6, "PATTERN / value        " + board.display_text());
		text(ren, font, 560, 50, board.waverom_loaded ? "wave ROM: loaded" : "wave ROM: none (silent)");
		text(ren, font, 560, 66, "PCM samples: " + std::to_string(board.audio_samples) + "   audio: " + (adev ? "on" : "OFF") + "   MIDI out bytes: " + std::to_string(board.midi_out.size()));
		text(ren, font, 560, 82, "keys: Z-M / Q-P play notes (MIDI ch 1), 1-8 drums (ch 10)");
		text(ren, font, 560, 98, "Esc quits and saves SRAM");
		// LED grid
		text(ren, font, GX, GY - 18, "LED matrix (columns 0-15 as scanned; 5-7 and 13-15 are the digits)");
		for (int c = 0; c < 16; c++) for (int b = 0; b < 8; b++) {
			bool lit = !((board.leds[c] >> b) & 1);
			SDL_Rect rc{GX + c*CW, GY + b*CH, CW - 3, CH - 3};
			if (c >= 5 && c <= 7) { SDL_SetRenderDrawColor(ren, 40, 40, 44, 255); SDL_RenderFillRect(ren, &rc); continue; }
			if (c >= 13) { SDL_SetRenderDrawColor(ren, 40, 40, 44, 255); SDL_RenderFillRect(ren, &rc); continue; }
			SDL_SetRenderDrawColor(ren, lit ? 255 : 70, lit ? 80 : 30, lit ? 40 : 26, 255);
			SDL_RenderFillRect(ren, &rc);
			text(ren, font, GX + c*CW + 3, GY + b*CH + 2, led_name(c, b), {230, 230, 230, 255});
			if (!*led_name(c, b)) text(ren, font, GX + c*CW + 3, GY + b*CH + 10, std::to_string(c) + "/" + std::to_string(b), {120, 120, 120, 255});
		}
		// switch grid
		text(ren, font, SX, SY - 18, "Switch matrix: click and hold a cell (col/bit) to press that key");
		for (int c = 0; c < 16; c++) for (int b = 0; b < 8; b++) {
			bool on = (board.switches[c] >> b) & 1;
			SDL_Rect rc{SX + c*SW, SY + b*SH, SW - 2, SH - 2};
			SDL_SetRenderDrawColor(ren, on ? 90 : 50, on ? 160 : 60, on ? 90 : 70, 255);
			SDL_RenderFillRect(ren, &rc);
			const char *n = sw_name(c, b);
			text(ren, font, SX + c*SW + 2, SY + b*SH + 1, *n ? std::string(n) : std::to_string(c) + "/" + std::to_string(b), {210, 210, 210, 255});
		}
		SDL_RenderPresent(ren);
		if (shot_path && (double)board.cycles / MACHINE_HZ >= shot_at) {
			int w, h; SDL_GetRendererOutputSize(ren, &w, &h);
			SDL_Surface *sf = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
			SDL_RenderReadPixels(ren, nullptr, SDL_PIXELFORMAT_ARGB8888, sf->pixels, sf->pitch);
			SDL_SaveBMP(sf, shot_path); SDL_FreeSurface(sf);
			printf("saved %s\n", shot_path); running = false;
		}
	}
	if (adev) SDL_CloseAudioDevice(adev);
	TTF_CloseFont(font); TTF_CloseFont(big); SDL_DestroyRenderer(ren); SDL_DestroyWindow(win); TTF_Quit(); SDL_Quit();
	return 0;
}
