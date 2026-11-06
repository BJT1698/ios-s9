/*
 * What milestone 1 shows: proof of life, colour bands that reveal the byte
 * order, what we learned about the display pipe, and a seconds counter
 * that only keeps moving on the glass if repeated frames get through.
 * Text is ASCII only (the font has no accents).
 */
#include "fb.h"
#include "screen.h"

#define S	4		/* text scale: 32x64 px glyphs */
#define CW	(8 * S)
#define CH	(16 * S)
#define X0	32
#define VAL	(X0 + 8 * CW)	/* values start after an 8-column label */
#define BAR_H	48

static int y;			/* next text row */
static int tick_y;		/* row of the seconds counter */
static uint32_t bg = GREY;

static char *put(char *d, const char *s)
{
	while (*s)
		*d++ = *s++;
	*d = 0;
	return d;
}

static char *put_hex(char *d, uint64_t v, int digits)
{
	fmt_hex(d, v, digits);
	return d + digits;
}

static char *put_dec(char *d, uint32_t v)
{
	fmt_dec(d, v);
	while (*d)
		d++;
	return d;
}

/* label in yellow, value in white, wrapped onto as many rows as it needs */
static void line(const char *label, const char *val)
{
	int cols = ((int)fb.width - VAL) / CW;
	char chunk[64];

	if (cols < 1)
		cols = 1;
	fb_text(X0, y, S, label, YELLOW, bg);
	do {
		int n = 0;
		while (val[n] && n < cols && n < 63) {
			chunk[n] = val[n];
			n++;
		}
		chunk[n] = 0;
		fb_text(VAL, y, S, chunk, WHITE, bg);
		val += n;
		y += CH;
	} while (*val);
}

static void band(uint32_t rgb, const char *name, uint32_t fg)
{
	fb_rect(0, y, fb.width, 160, rgb);
	fb_text(X0, y + 48, S, name, fg, rgb);
	y += 160;
}

void screen_draw(const struct m1_info *in)
{
	const struct mdss_state *m = &in->mdss;
	char b[96], *p;

	bg = GREY;
	fb_rect(0, 0, fb.width, fb.height, GREY);
	fb_text(X0, 32, 6, "OP6 SHIM M1 OK!", YELLOW, GREY);
	fb_text(X0, 160, S, "OnePlus 6 -> iOS, milestone 1", WHITE, GREY);

	/* If a label does not match its colour, red and blue are swapped. */
	y = 260;
	band(RED, "VERMELHO", WHITE);
	band(GREEN, "VERDE", BLACK);
	band(BLUE, "AZUL", WHITE);
	y += 32;

	line("MODELO", in->model[0] ? in->model : "(sem DTB)");
	line("PAINEL", in->panel[0] ? in->panel : "?");
	put(put(b, "0x"), in->lcd_id[0] ? in->lcd_id : "?");
	line("LCD_ID", b);
	p = put_dec(put(b, "EL"), in->el);
	put_hex(put(p, "  MIDR "), in->midr, 8);
	line("CPU", b);
	p = put_hex(put(b, "IMG "), in->image, 9);
	put_hex(put(p, " DTB "), in->dtb, 9);
	line("ONDE", b);
	p = put(b, (in->mmfr0 >> 20) & 0xf ? "SIM" : "NAO");
	put_hex(put(p, "   PSCI "), in->psci, 8);
	line("16K", b);
	line("WDOG", in->wdog_en & 1 ? "ligado pelo ABL (alimentando)" : "desligado");
	y += CH / 2;

	put_hex(put(b, "VER "), m->hw_version, 8);
	line("MDSS", b);
	p = put_hex(b, in->fb_addr, 9);
	put(p, in->fb_from_hw ? " (lido do HW)" : " (padrao)");
	line("FB", b);
	p = put(put_dec(b, fb.width), "x");
	p = put(put_dec(p, fb.height), " ");
	p = put(put_dec(p, fb.stride), "B/L ");
	put(put_dec(p, fb.bpp), "B/px");
	line("LAYOUT", b);
	p = put_dec(put(b, "R"), fb.pos_r);
	p = put_dec(put(p, " G"), fb.pos_g);
	p = put_dec(put(p, " B"), fb.pos_b);
	put_dec(put(p, " A"), fb.pos_a);
	line("BYTES", b);
	if (m->sspp_idx >= 0) {
		const struct mdss_sspp *s = &m->sspp[m->sspp_idx];
		p = put_dec(put(b, "n="), m->sspp_idx);
		p = put_hex(put(p, " FMT "), s->format, 8);
		put_hex(put(p, " UNP "), s->unpack, 8);
		line("SSPP", b);
		p = put_hex(put(b, "SIZE "), s->size, 8);
		put_hex(put(p, " STR "), s->ystride, 8);
		line("", b);
	} else {
		line("SSPP", "nenhum pipe lendo da RAM");
	}
	if (m->ctl >= 0) {
		p = put_dec(put(b, "n="), m->ctl);
		p = put_hex(put(p, " TOP "), m->ctl_top[m->ctl], 8);
		put(p, m->cmd_mode ? " CMD" : " VIDEO");
		line("CTL", b);
	} else {
		line("CTL", "nenhum ligado a interface");
	}
	p = b;
	for (int i = 0; i < MDSS_NPP; i++)
		p = put(p, m->pp_autorefresh[i] >> 31 ? "1" : "0");
	for (int i = 0; i < MDSS_NPP; i++)
		if (m->pp_autorefresh[i] >> 31) {
			p = put_dec(put(p, "  PP"), i);
			put_hex(put(p, "="), m->pp_autorefresh[i], 8);
			break;
		}
	line("AUTOREF", b);
	if (m->ctl < 0)
		line("MODO", "sem CTL: nao sei dar kick");
	else if (!m->cmd_mode)
		line("MODO", "video: tela le a RAM sozinha");
	else if (m->autorefresh)
		line("MODO", "autorefresh deixado pelo ABL");
	else
		line("MODO", "kick manual (CTL_START)");
	y += CH / 2;

	tick_y = y;
	y += CH;
	line("", "Se o TEMPO andar, o kick funciona.");
	line("", "Tire uma foto desta tela!");
}

void screen_tick(unsigned secs, unsigned total)
{
	char b[48], *p;
	int save = y, w = (int)fb.width - 2 * X0;
	int done = w * (int)secs / (int)total;

	p = put(put_dec(b, secs), "s  reinicia em ");
	put(put_dec(p, total - secs), "s  ");
	y = tick_y;
	line("TEMPO", b);
	y = save;

	/* heartbeat bar near the bottom edge */
	fb_rect(X0, fb.height - 160, done, BAR_H, GREEN);
	fb_rect(X0 + done, fb.height - 160, w - done, BAR_H, BLACK);
}

void screen_panic(uint64_t slot, uint64_t esr, uint64_t elr, uint64_t far, uint64_t lr)
{
	static const char kind[4][8] = { "SYNC", "IRQ", "FIQ", "SERROR" };
	char b[48];

	bg = PANICBG;
	fb_rect(0, 0, fb.width, fb.height, PANICBG);
	fb_text(X0, 32, 6, "PANICO NO SHIM", YELLOW, PANICBG);
	y = 260;
	put_dec(put(put(b, kind[slot & 3]), " slot "), slot);
	line("EXCECAO", b);
	put_hex(b, esr, 16);
	line("ESR", b);
	put_hex(b, elr, 16);
	line("ELR", b);
	put_hex(b, far, 16);
	line("FAR", b);
	put_hex(b, lr, 16);
	line("LR", b);
	tick_y = y + CH / 2;
	y = tick_y + CH;
	line("", "Tire uma foto desta tela!");
}
