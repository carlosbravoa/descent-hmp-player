/*
 * hmpviz.cpp — SDL2 OPL3 channel-activity visualizer for hmpplay_opl3.
 * See hmpviz.h. Ported from tyrian-retrowave's rwgui.c.
 */
#include "hmpviz.h"
#include "opl.h"
#include "font5x7.h"

#include <SDL.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>

#define NCH 18                 // OPL3: 18 two-operator channels
#define TEE_RATE 49716         // software tee sample rate (matches adl_init)

/* ------------------------------------------------------------------ */
/* Software OPL3 tee fed by the register tap.                          */
/* ------------------------------------------------------------------ */

static std::mutex g_opl_mtx;                 // guards the tee (tap vs generate)
static volatile uint8_t fmchip[512];         // OPL3 register shadow (two sets)
static const HmpVizHooks *H;

void hmpviz_opl_tap(uint16_t addr, uint8_t data)
{
	if (addr >= 512) return;
	std::lock_guard<std::mutex> lk(g_opl_mtx);
	adlib_write(addr, data);
	fmchip[addr] = data;
}

static int chan_a0(int ch) { return ch < 9 ? 0xA0 + ch : 0x1A0 + (ch - 9); }
static int chan_b0(int ch) { return ch < 9 ? 0xB0 + ch : 0x1B0 + (ch - 9); }

/* ------------------------------------------------------------------ */
/* Visualizer state.                                                   */
/* ------------------------------------------------------------------ */

typedef struct { float level, peak, hue; bool on; } VizChan;
static VizChan viz[NCH];

static void viz_update(void)
{
	for (int ch = 0; ch < NCH; ++ch)
	{
		float amp;
		{
			std::lock_guard<std::mutex> lk(g_opl_mtx);
			amp = (float)opl_channel_level(ch);
		}
		amp = powf(amp, 0.55f);                 // perceptual curve

		int b0 = fmchip[chan_b0(ch)];
		int fnum = fmchip[chan_a0(ch)] | ((b0 & 3) << 8);
		int block = (b0 >> 2) & 7;
		float pitch = (block * 1024 + fnum) / (8.0f * 1024.0f);

		VizChan *v = &viz[ch];
		v->on = amp > 0.02f;
		v->level += (amp - v->level) * (amp > v->level ? 0.6f : 0.20f);
		if (v->level < 0) v->level = 0;
		if (v->level > v->peak) v->peak = v->level;
		else v->peak -= 0.012f;
		if (v->peak < v->level) v->peak = v->level;
		if (v->on) v->hue = pitch;
	}
}

/* ------------------------------------------------------------------ */
/* Drawing helpers.                                                    */
/* ------------------------------------------------------------------ */

typedef struct { Uint8 r, g, b; } RGB;

static SDL_Renderer *ren;

static RGB hsv(float h, float s, float v)
{
	h = fmodf(h, 1.0f); if (h < 0) h += 1.0f;
	float i = floorf(h * 6.0f), f = h * 6.0f - i;
	float p = v * (1 - s), q = v * (1 - f * s), t = v * (1 - (1 - f) * s);
	float r, g, b;
	switch (((int)i) % 6) {
	case 0: r = v; g = t; b = p; break;
	case 1: r = q; g = v; b = p; break;
	case 2: r = p; g = v; b = t; break;
	case 3: r = p; g = q; b = v; break;
	case 4: r = t; g = p; b = v; break;
	default: r = v; g = p; b = q; break;
	}
	RGB c = { (Uint8)(r * 255), (Uint8)(g * 255), (Uint8)(b * 255) };
	return c;
}

static void fill(int x, int y, int w, int h, RGB c, Uint8 a)
{
	SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, a);
	SDL_Rect q = { x, y, w, h };
	SDL_RenderFillRect(ren, &q);
}

static void draw_text(int x, int y, int s, RGB c, const char *txt)
{
	SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, 255);
	int cx = x;
	for (const char *p = txt; *p; ++p) {
		const uint8_t *g = font_glyph(*p);
		if (g)
			for (int ry = 0; ry < FONT_H; ++ry)
				for (int rx = 0; rx < FONT_W; ++rx)
					if (g[ry] & (1 << (FONT_W - 1 - rx))) {
						SDL_Rect q = { cx + rx * s, y + ry * s, s, s };
						SDL_RenderFillRect(ren, &q);
					}
		cx += (FONT_W + 1) * s;
	}
}
static int text_w(const char *t, int s) { return (int)strlen(t) * (FONT_W + 1) * s; }

static void fill_tri(int x, int y, int w, int h, int dir, RGB c)
{
	SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, 255);
	for (int ry = 0; ry < h; ++ry) {
		float t = 1.0f - fabsf((ry - h / 2.0f) / (h / 2.0f));
		int len = (int)(w * t + 0.5f); if (len < 1) len = 1;
		int lx = (dir > 0) ? x : x + (w - len);
		SDL_Rect q = { lx, y + ry, len, 1 };
		SDL_RenderFillRect(ren, &q);
	}
}

/* ------------------------------------------------------------------ */
/* Visualizer styles.                                                  */
/* ------------------------------------------------------------------ */

enum { STYLE_LED, STYLE_NEON, STYLE_SPECTRUM, STYLE_COUNT };
static int style = STYLE_LED;

static void draw_bars(int ax, int ay, int aw, int ah)
{
	int gap = aw / (NCH * 5); if (gap < 1) gap = 1;
	int slot = (aw - gap) / NCH, bw = slot - gap;

	for (int ch = 0; ch < NCH; ++ch) {
		int x = ax + gap + ch * slot;
		float lv = viz[ch].level; if (lv > 1) lv = 1;
		float pk = viz[ch].peak;  if (pk > 1) pk = 1;
		int bh = (int)(lv * ah), by = ay + ah - bh;

		if (style == STYLE_LED) {
			int segs = 18, seg_h = ah / segs;
			int lit = (int)(lv * segs + 0.5f), pkseg = (int)(pk * segs + 0.5f);
			for (int s = 0; s < segs; ++s) {
				int sy = ay + ah - (s + 1) * seg_h + 1;
				RGB col = (s < segs * 0.6f) ? (RGB){ 40, 220, 70 }
				        : (s < segs * 0.85f) ? (RGB){ 240, 200, 40 }
				                             : (RGB){ 240, 60, 40 };
				if (s + 1 == pkseg)   fill(x, sy, bw, seg_h - 1, (RGB){ 255, 255, 255 }, 255);
				else if (s < lit)     fill(x, sy, bw, seg_h - 1, col, 255);
				else                  fill(x, sy, bw, seg_h - 1, col, 26);
			}
		} else if (style == STYLE_NEON) {
			SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_ADD);
			for (int g = 3; g >= 1; --g)
				fill(x - g * 3, by - g * 3, bw + g * 6, bh + g * 3, (RGB){ 60, 120, 255 }, (Uint8)(16 * lv));
			int slices = bh / 3 + 1;
			for (int s = 0; s < slices; ++s) {
				float t = (float)s / (slices > 1 ? slices - 1 : 1);
				RGB c = { (Uint8)(40 + t * 215), (Uint8)(220 - t * 140), 255 };
				fill(x, by + bh - (s + 1) * 3, bw, 3, c, 255);
			}
			SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
			fill(x, ay + ah - (int)(pk * ah) - 2, bw, 2, (RGB){ 255, 255, 255 }, 220);
		} else {
			int tw = bw * 7 / 10, tx = x + (bw - tw) / 2;
			RGB c = hsv(0.62f - viz[ch].hue * 0.62f, 0.85f, 1.0f);
			fill(tx, by, tw, bh, c, 255);
			fill(tx, ay + ah - (int)(pk * ah) - 2, tw, 2, (RGB){ 255, 255, 255 }, 230);
		}

		char lbl[4]; snprintf(lbl, sizeof lbl, "%d", ch + 1);
		draw_text(x + bw / 2 - text_w(lbl, 1) / 2, ay + ah + 4, 1,
		          viz[ch].on ? (RGB){ 220, 220, 220 } : (RGB){ 80, 80, 100 }, lbl);
	}
}

/* ------------------------------------------------------------------ */
/* Controls.                                                           */
/* ------------------------------------------------------------------ */

enum { ACT_PREV, ACT_PLAY, ACT_NEXT, ACT_LOOP, ACT_STYLE, ACT_TDN, ACT_TUP };
static const char *STYLE_SHORT[] = { "LED", "NEON", "SPEC" };

static void do_action(int a)
{
	switch (a) {
	case ACT_PREV:  *H->prev = true; *H->skip = true; break;
	case ACT_NEXT:  *H->skip = true; break;
	case ACT_PLAY:  *H->pause = !*H->pause; break;
	case ACT_LOOP:  *H->loop = !*H->loop; break;
	case ACT_STYLE: style = (style + 1) % STYLE_COUNT; break;
	case ACT_TUP:   *H->tempo = *H->tempo < 4.0 ? *H->tempo * 1.25 : 4.0; H->apply_tempo(*H->tempo); break;
	case ACT_TDN:   *H->tempo = *H->tempo > 0.25 ? *H->tempo / 1.25 : 0.25; H->apply_tempo(*H->tempo); break;
	}
}

typedef struct { SDL_Rect r; int act; } Button;
static Button g_btn[16];
static int g_nbtn;

enum { ICON_PREV, ICON_PLAY, ICON_PAUSE, ICON_NEXT };

static void draw_icon(int type, int x, int y, int sz, RGB c)
{
	int g = sz * 6 / 10, gx = x + (sz - g) / 2, gy = y + (sz - g) / 2;
	int bar = g / 4; if (bar < 2) bar = 2;
	switch (type) {
	case ICON_PLAY:  fill_tri(gx + g / 8, gy, g - g / 8, g, +1, c); break;
	case ICON_PAUSE: fill(gx + g / 8, gy, bar, g, c, 255);
	                 fill(gx + g - g / 8 - bar, gy, bar, g, c, 255); break;
	case ICON_PREV:  fill(gx, gy, bar, g, c, 255);
	                 fill_tri(gx + bar + 1, gy, g - bar - 1, g, -1, c); break;
	case ICON_NEXT:  fill_tri(gx, gy, g - bar - 1, g, +1, c);
	                 fill(gx + g - bar, gy, bar, g, c, 255); break;
	}
}

static void reg_button(SDL_Rect b, int act)
{
	if (g_nbtn < 16) { g_btn[g_nbtn].r = b; g_btn[g_nbtn].act = act; g_nbtn++; }
}

static int add_button(int x, int y, int h, const char *label, int act, int mx, int my)
{
	int s = 2, pad = 6, w = text_w(label, s) + pad * 2;
	bool hover = mx >= x && mx < x + w && my >= y && my < y + h;
	fill(x, y, w, h, hover ? (RGB){ 58, 64, 96 } : (RGB){ 34, 36, 54 }, 255);
	SDL_SetRenderDrawColor(ren, 95, 105, 150, 255);
	SDL_Rect b = { x, y, w, h }; SDL_RenderDrawRect(ren, &b);
	draw_text(x + pad, y + (h - FONT_H * s) / 2, s, (RGB){ 225, 230, 250 }, label);
	reg_button(b, act);
	return x + w + 6;
}

static int add_icon_button(int x, int y, int h, int type, int act, int mx, int my)
{
	int w = h + 6;
	bool hover = mx >= x && mx < x + w && my >= y && my < y + h;
	fill(x, y, w, h, hover ? (RGB){ 58, 64, 96 } : (RGB){ 34, 36, 54 }, 255);
	SDL_SetRenderDrawColor(ren, 95, 105, 150, 255);
	SDL_Rect b = { x, y, w, h }; SDL_RenderDrawRect(ren, &b);
	draw_icon(type, x + (w - h) / 2, y, h, (RGB){ 225, 230, 250 });
	reg_button(b, act);
	return x + w + 6;
}

static void draw_controls(int H_)
{
	int mx, my; SDL_GetMouseState(&mx, &my);
	int by = H_ - 38, bh = 28, bx = 12;
	char b[32];
	g_nbtn = 0;
	bx = add_icon_button(bx, by, bh, ICON_PREV, ACT_PREV, mx, my);
	bx = add_icon_button(bx, by, bh, *H->pause ? ICON_PLAY : ICON_PAUSE, ACT_PLAY, mx, my);
	bx = add_icon_button(bx, by, bh, ICON_NEXT, ACT_NEXT, mx, my);
	snprintf(b, sizeof b, "LOOP:%s", *H->loop ? "ON" : "OFF");
	bx = add_button(bx, by, bh, b, ACT_LOOP, mx, my);
	bx = add_button(bx, by, bh, STYLE_SHORT[style], ACT_STYLE, mx, my);
	bx = add_button(bx, by, bh, "T-", ACT_TDN, mx, my);
	snprintf(b, sizeof b, "%.2fx", *H->tempo);
	draw_text(bx + 2, by + (bh - FONT_H * 2) / 2, 2, (RGB){ 150, 170, 210 }, b);
	bx += text_w(b, 2) + 8;
	add_button(bx, by, bh, "T+", ACT_TUP, mx, my);
}

static void handle_click(int x, int y)
{
	for (int i = 0; i < g_nbtn; ++i)
		if (x >= g_btn[i].r.x && x < g_btn[i].r.x + g_btn[i].r.w &&
		    y >= g_btn[i].r.y && y < g_btn[i].r.y + g_btn[i].r.h) { do_action(g_btn[i].act); return; }
}

/* ------------------------------------------------------------------ */
/* Public API.                                                         */
/* ------------------------------------------------------------------ */

static SDL_Window *win;
static char g_status[160] = "";
static std::mutex g_status_mtx;
static Uint64 g_last_perf;

bool hmpviz_init(const HmpVizHooks *hooks)
{
	H = hooks;
	font_init();
	if (SDL_Init(SDL_INIT_VIDEO) != 0) {
		fprintf(stderr, "hmpviz: SDL_Init failed: %s\n", SDL_GetError());
		return false;
	}
	win = SDL_CreateWindow("Descent \xc2\xb7 RetroWave OPL3",
	        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 760, 400,
	        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
	ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
	if (!win || !ren) { fprintf(stderr, "hmpviz: window/renderer failed\n"); return false; }
	SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
	if (getenv("RWSTYLE")) style = atoi(getenv("RWSTYLE")) % STYLE_COUNT;   // testing
	adlib_init(TEE_RATE);
	g_last_perf = SDL_GetPerformanceCounter();
	return true;
}

void hmpviz_set_status(int idx, int total, const char *name)
{
	std::lock_guard<std::mutex> lk(g_status_mtx);
	snprintf(g_status, sizeof g_status, "%d/%d  %s", idx, total, name ? name : "");
}

bool hmpviz_frame(void)
{
	SDL_Event e;
	while (SDL_PollEvent(&e)) {
		if (e.type == SDL_QUIT) { *H->stop = true; return false; }
		else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT)
			handle_click(e.button.x, e.button.y);
		else if (e.type == SDL_KEYDOWN) {
			switch (e.key.keysym.sym) {
			case SDLK_q: case SDLK_ESCAPE: *H->stop = true; return false;
			case SDLK_SPACE: do_action(ACT_PLAY); break;
			case SDLK_n: case SDLK_RIGHT: do_action(ACT_NEXT); break;
			case SDLK_p: case SDLK_LEFT:  do_action(ACT_PREV); break;
			case SDLK_l: do_action(ACT_LOOP); break;
			case SDLK_v: do_action(ACT_STYLE); break;
			case SDLK_EQUALS: case SDLK_PLUS: case SDLK_KP_PLUS: do_action(ACT_TUP); break;
			case SDLK_MINUS: case SDLK_KP_MINUS: do_action(ACT_TDN); break;
			}
		}
	}

	// Advance the software tee by the real elapsed time so its envelopes track
	// the board (which libADLMIDI clocks in real wall-clock time).
	Uint64 now = SDL_GetPerformanceCounter();
	double dt = (double)(now - g_last_perf) / SDL_GetPerformanceFrequency();
	g_last_perf = now;
	if (dt > 0.1) dt = 0.1;
	int frames = (int)(dt * TEE_RATE);
	if (frames > 0 && !*H->pause) {
		static Bit16s scratch[16384];           // stereo: 2 int16 per frame
		int cap = (int)(sizeof(scratch) / sizeof(Bit16s)) / 2;
		std::lock_guard<std::mutex> lk(g_opl_mtx);
		while (frames > 0) {
			int n = frames > cap ? cap : frames;
			adlib_getsample(scratch, n);
			frames -= n;
		}
	}

	viz_update();

	int W, Hh; SDL_GetRendererOutputSize(ren, &W, &Hh);
	fill(0, 0, W, Hh, (RGB){ 12, 12, 20 }, 255);
	fill(0, 0, W, 34, (RGB){ 22, 22, 38 }, 255);

	{
		std::lock_guard<std::mutex> lk(g_status_mtx);
		char hdr[180];
		snprintf(hdr, sizeof hdr, "%s%s", g_status, *H->pause ? "   -PAUSED-" : "");
		draw_text(12, 10, 2, (RGB){ 240, 240, 255 }, hdr);
	}

	int ax = 16, ay = 46, aw = W - 32, ah = Hh - 46 - 62;
	fill(ax - 5, ay - 5, aw + 10, ah + 10 + 18, (RGB){ 8, 8, 14 }, 255);
	draw_bars(ax, ay, aw, ah);
	draw_controls(Hh);

	SDL_RenderPresent(ren);

	// Optional headless self-capture for testing: RWSHOT=path RWSHOT_FRAME=N
	const char *shot = getenv("RWSHOT");
	if (shot) {
		static long fr = 0;
		long target = getenv("RWSHOT_FRAME") ? atol(getenv("RWSHOT_FRAME")) : 200;
		if (++fr >= target) {
			SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, W, Hh, 32, SDL_PIXELFORMAT_ARGB8888);
			if (s && SDL_RenderReadPixels(ren, NULL, SDL_PIXELFORMAT_ARGB8888, s->pixels, s->pitch) == 0)
				SDL_SaveBMP(s, shot);
			if (s) SDL_FreeSurface(s);
			*H->stop = true;
			return false;
		}
	}
	return !*H->stop;
}

void hmpviz_shutdown(void)
{
	if (ren) SDL_DestroyRenderer(ren);
	if (win) SDL_DestroyWindow(win);
	SDL_Quit();
}
