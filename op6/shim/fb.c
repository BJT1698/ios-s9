/*
 * Drawing into the framebuffer ABL left on. The pixel layout is read from
 * the display pipe at runtime (fb_set_format); until then we assume what
 * UEFI GOP uses. With the MMU off everything is Device memory: stores stay
 * aligned (-mstrict-align) and go through volatile pointers, so the
 * compiler cannot turn the loops into memset calls.
 */
#include "fb.h"
#include "font.h"

struct fb fb;

void fb_default(void)
{
	fb.base = SPLASH_BASE;
	fb.width = PANEL_W;
	fb.height = PANEL_H;
	fb.stride = PANEL_W * 4;
	fb.bpp = 4;
	/* GOP PixelBlueGreenRedReserved8BitPerColor: B, G, R, X in memory */
	fb.pos_b = 0;
	fb.pos_g = 1;
	fb.pos_r = 2;
	fb.pos_a = 3;
}

/* SDE unpack pattern: memory byte i holds component element[i] (0 G, 1 B, 2 R, 3 A). */
int fb_set_format(uint32_t bpp, uint32_t unpack)
{
	uint32_t pos[4] = { 4, 4, 4, 4 };	/* indexed by component */

	if (bpp != 3 && bpp != 4)
		return -1;
	for (uint32_t i = 0; i < bpp; i++) {
		uint32_t e = (unpack >> (8 * i)) & 0xff;
		if (e > 3 || pos[e] != 4)
			return -1;
		pos[e] = i;
	}
	if (pos[0] == 4 || pos[1] == 4 || pos[2] == 4)
		return -1;
	fb.bpp = bpp;
	fb.pos_g = pos[0];
	fb.pos_b = pos[1];
	fb.pos_r = pos[2];
	fb.pos_a = pos[3];
	return 0;
}

/* 0xRRGGBB -> the bytes the scanout expects, packed little-endian */
static uint32_t encode(uint32_t rgb)
{
	uint32_t w = ((rgb >> 16) & 0xff) << (8 * fb.pos_r) |
		     ((rgb >> 8) & 0xff) << (8 * fb.pos_g) |
		     (rgb & 0xff) << (8 * fb.pos_b);

	if (fb.pos_a < 4)
		w |= 0xffU << (8 * fb.pos_a);
	return w;
}

static void span(uintptr_t p, int n, uint32_t w)
{
	if (fb.bpp == 4) {
		volatile uint32_t *q = (volatile uint32_t *)p;
		for (int i = 0; i < n; i++)
			q[i] = w;
	} else {
		volatile uint8_t *q = (volatile uint8_t *)p;
		for (int i = 0; i < n; i++, q += 3) {
			q[0] = w;
			q[1] = w >> 8;
			q[2] = w >> 16;
		}
	}
}

static uintptr_t pixel(int x, int y)
{
	return fb.base + (uintptr_t)y * fb.stride + (uintptr_t)x * fb.bpp;
}

void fb_rect(int x, int y, int w, int h, uint32_t rgb)
{
	if (x < 0)
		w += x, x = 0;
	if (y < 0)
		h += y, y = 0;
	if (x + w > (int)fb.width)
		w = fb.width - x;
	if (y + h > (int)fb.height)
		h = fb.height - y;
	if (w <= 0 || h <= 0)
		return;
	uint32_t c = encode(rgb);
	for (int r = 0; r < h; r++)
		span(pixel(x, y + r), w, c);
}

/* 8x16 glyphs scaled up; returns the x after the last glyph drawn. */
int fb_text(int x, int y, int scale, const char *s, uint32_t fg, uint32_t bg)
{
	uint32_t cf = encode(fg), cb = encode(bg);
	int cw = 8 * scale;

	if (x < 0 || y < 0 || y + 16 * scale > (int)fb.height)
		return x;
	for (; *s && x + cw <= (int)fb.width; s++, x += cw) {
		unsigned char ch = *s;
		if (ch < 0x20 || ch > 0x7e)
			ch = '?';
		const unsigned char *g = font8x16[ch - 0x20];
		for (int gy = 0; gy < 16; gy++)
			for (int sy = 0; sy < scale; sy++)
				for (int gx = 0; gx < 8; gx++)
					span(pixel(x + gx * scale, y + gy * scale + sy), scale,
					     (g[gy] & (0x80 >> gx)) ? cf : cb);
	}
	return x;
}

void fmt_hex(char *buf, uint64_t v, int digits)
{
	for (int i = 0; i < digits; i++) {
		uint32_t d = (v >> (4 * (digits - 1 - i))) & 0xf;
		buf[i] = d < 10 ? '0' + d : 'A' + d - 10;
	}
	buf[digits] = 0;
}

void fmt_dec(char *buf, uint32_t v)
{
	char t[10];
	int n = 0;

	do
		t[n++] = '0' + v % 10;
	while (v /= 10);
	for (int i = 0; i < n; i++)
		buf[i] = t[n - 1 - i];
	buf[n] = 0;
}
