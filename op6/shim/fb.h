#pragma once
#include <stdint.h>

/* Stock DTB: reserved-memory cont_splash_region; OnePlus 6 sofef00 1080x2280. */
#define SPLASH_BASE	0x9d400000UL
#define SPLASH_SIZE	0x02400000UL
#define PANEL_W		1080
#define PANEL_H		2280

/* 0xRRGGBB; fb_* encode them into whatever layout the scanout uses */
#define BLACK	0x000000U
#define WHITE	0xffffffU
#define RED	0xff0000U
#define GREEN	0x00ff00U
#define BLUE	0x0000ffU
#define YELLOW	0xffff00U
#define GREY	0x202020U
#define PANICBG	0x0000a0U

struct fb {
	uintptr_t base;
	uint32_t width, height;
	uint32_t stride;			/* bytes per line */
	uint32_t bpp;				/* bytes per pixel: 3 or 4 */
	uint32_t pos_r, pos_g, pos_b, pos_a;	/* byte of each component, 4 = none */
};

extern struct fb fb;

void fb_default(void);
int fb_set_format(uint32_t bpp, uint32_t unpack);
void fb_rect(int x, int y, int w, int h, uint32_t rgb);
int fb_text(int x, int y, int scale, const char *s, uint32_t fg, uint32_t bg);
void fmt_hex(char *buf, uint64_t v, int digits);
void fmt_dec(char *buf, uint32_t v);
