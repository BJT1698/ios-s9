/*
 * Text console for the shim stages after M1: yellow labels, white values,
 * same API as ios-sweet's shim/console.c, on the runtime-sized fb.
 */
#include "fb.h"
#include "con.h"

#define S	3		/* 24x48 px glyphs: ~58 columns, ~60 rows */
#define CW	(8 * S)
#define CH	(16 * S)
#define X0	24
#define VAL	(X0 + 8 * CW)
#define TOP	24

static uint32_t bg = GREY;
static int y = TOP;

char *put(char *d, const char *s)
{
	while (*s)
		*d++ = *s++;
	*d = 0;
	return d;
}

char *put_hex(char *d, uint64_t v, int digits)
{
	fmt_hex(d, v, digits);
	return d + digits;
}

char *put_dec(char *d, uint32_t v)
{
	fmt_dec(d, v);
	while (*d)
		d++;
	return d;
}

void con_clear(uint32_t c)
{
	bg = c;
	fb_rect(0, 0, fb.width, fb.height, bg);
	y = TOP;
}

void con_title(const char *s)
{
	fb_text(X0, y, 2 * S, s, YELLOW, bg);
	y += 2 * CH + CH / 2;
}

void con_skip(void)
{
	y += CH / 2;
}

int con_row(void)
{
	return y;
}

void con_set_row(int row)
{
	y = row;
}

void con_str(const char *label, const char *s)
{
	int cols = ((int)fb.width - VAL) / CW;
	char chunk[64];

	if (cols < 1)
		cols = 1;
	fb_text(X0, y, S, label, YELLOW, bg);
	do {
		int n = 0;
		while (s[n] && n < cols && n < 63) {
			chunk[n] = s[n];
			n++;
		}
		chunk[n] = 0;
		fb_text(VAL, y, S, chunk, WHITE, bg);
		s += n;
		y += CH;
	} while (*s);
}

void con_hex(const char *label, uint64_t v)
{
	char b[17];

	fmt_hex(b, v, 16);
	con_str(label, b);
}

void con_hex2(const char *label, const char *a, uint32_t va, const char *b, uint32_t vb)
{
	char buf[64], *p = buf;

	p = put_hex(put(p, a), va, 8);
	if (b)
		put_hex(put(put(p, " "), b), vb, 8);
	con_str(label, buf);
}
