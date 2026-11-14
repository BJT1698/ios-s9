/*
 * OnePlus 6 (enchilada) milestone 9: display-engine zero-copy + QSEED3 scaler.
 *
 * Unlike the Galaxy S9 (1440x2960 split across two 720-wide halves, LM0+LM1,
 * VIG0+VIG1, DSC, ABL PP0 autorefresh 0x80000002), the OP6 drives its 1080x2280
 * samsung,sofef00 command-mode panel over single DSI0 with a single 1080-wide
 * Layer Mixer (LM0), single PingPong (PP0, ABL autorefresh already 0x80000001
 * = 60 Hz), and single VIG0 pipe (0x0ae05000, BGRA fmt 0x000237ff).
 *
 * apps_smmu (0x15000000, MDSS SIDs 0x880/0xc80) is left untouched in ABL's
 * hypervisor-compatible M=0 identity state: every scan-out test uses
 * physically contiguous RAM verified free by M8 (0xb0000000, 0xb2000000).
 *
 * Steps (every step printed on screen + ramlog BEFORE running; every register
 * write is read back and checked; mismatch -> RECUSA + restore ABL layout):
 *   0. Read-only MDSS dump: hw_version, CTL0, LM0/1, active pipes, QSEED3 ver, PP0..4.
 *   A. Zero-copy 1080x2280: copy splash to 0xb0000000, draw purple banner, point
 *      active pipe(s) to 0xb0000000 with readback + CTL0_FLUSH, wait 8 s.
 *   B. QSEED3 scale 828x1792 -> 1080x2280 with exact 'b3a8' plane-0 stride 3328
 *      at 0xb2000000: draw test pattern + borders + color bars, program VIG0 +
 *      QSEED3 with full readback check, flush, then run a live frame counter
 *      inside the 828x1792 buffer for 15 s (~900 frames at 60 Hz).
 *   Restore ABL layout with readback + flush.
 *   C. Measure PP0 frames/s and TE/s at autorefresh 0x80000001 (~60 Hz).
 */
#include <stdint.h>
#include "hw.h"
#include "fb.h"
#include "con.h"
#include "plat.h"
#include "mdss.h"
#include "ramlog.h"

#define END_SECONDS	90
#define CYAN		0x00e0ffU

#define MDSS_BASE	0x0ae00000UL
#define CTL_BASE(i)	(MDSS_BASE + 0x2000 + 0x200 * (i))
#define CTL_LAYER(lm)	(0x000 + 4 * (lm))
#define CTL_LAYER_EXT(lm)	(0x040 + 4 * (lm))
#define CTL_LAYER_EXT2(lm)	(0x070 + 4 * (lm))
#define CTL_TOP		0x014
#define CTL_FLUSH	0x018
#define CTL_FLUSH_CTL	(1U << 17)

#define SSPP_SRC_SIZE	0x00
#define SSPP_SRC_XY	0x08
#define SSPP_OUT_SIZE	0x0c
#define SSPP_OUT_XY	0x10
#define SSPP_SRC0_ADDR	0x14
#define SSPP_YSTRIDE0	0x24
#define SSPP_FORMAT	0x30
#define SSPP_UNPACK	0x34
#define SSPP_OP_MODE	0x38
#define SSPP_DECIMATION	0xb4
#define SSPP_PE_C0_LR	0x100
#define SSPP_PE_C0_TB	0x104
#define SSPP_PE_C0_REQ	0x108
#define SSPP_PE_C12_LR	0x110
#define SSPP_PE_C12_TB	0x114
#define SSPP_PE_C12_REQ	0x118
#define SSPP_PE_C3_LR	0x120
#define SSPP_PE_C3_TB	0x124
#define SSPP_PE_C3_REQ	0x128
#define OP_PE_OVERRIDE	(1U << 31)

/* QSEED3 inside a VIG pipe (qcom,sde-vig-qseed-off = <0xa00>) */
#define QS		0xa00
#define QS_VERSION	0x00
#define QS_OP_MODE	0x04
#define QS_PHASE_INIT	0x0c	/* version 0x1002 only */
#define QS_STEP_Y_H	0x10
#define QS_STEP_Y_V	0x14
#define QS_STEP_UV_H	0x18
#define QS_STEP_UV_V	0x1c
#define QS_PRELOAD	0x20
#define QS_SRC_SIZE	0x40
#define QS_SRC_SIZE_UV	0x44
#define QS_DST_SIZE	0x48
#define QS_INIT_Y_H	0x90
#define QS_INIT_Y_V	0x94
#define QS_INIT_UV_H	0x98
#define QS_INIT_UV_V	0x9c
#define PHASE_STEP_SHIFT 21

#define BUF_B		0xb0000000UL	/* panel-sized copy: 1080*2280*4 = 9.4 MiB */
#define BUF_C		0xb2000000UL	/* 828x1792 test card, stride 3328 = 5.7 MiB */
#define C_W		828U
#define C_H		1792U
#define C_STRIDE	3328U		/* 'b3a8' plane-0 bytesPerRow (52 * 64 B) */

/* Pingpong blocks (sde_hw_pingpong.c) */
#define PP_INT_COUNT_VAL	0x014	/* [31:16] read-pointer (TE) frame count */
#define PP_LINE_COUNT		0x02c	/* write-pointer line: wraps once per frame */
#define PP_AUTOREFRESH_CONFIG	0x030
static const uint32_t pp_off[MDSS_NPP] = { 0x71000, 0x71800, 0x72000, 0x72800, 0x73000 };

static const uint32_t sspp_off[MDSS_NSSPP] = {
	0x5000, 0x7000, 0x9000, 0xb000, 0x25000, 0x27000, 0x29000, 0x2b000,
};

enum {
	R_SRC_SIZE = 0, R_SRC_XY, R_OUT_SIZE, R_OUT_XY, R_SRC0_ADDR, R_YSTRIDE0,
	R_OP_MODE, R_DECIMATION,
	R_PE_C0_LR, R_PE_C0_TB, R_PE_C0_REQ,
	R_PE_C12_LR, R_PE_C12_TB, R_PE_C12_REQ,
	R_PE_C3_LR, R_PE_C3_TB, R_PE_C3_REQ,
	R_QS_OP_MODE, R_QS_PHASE_INIT,
	R_QS_STEP_YH, R_QS_STEP_YV, R_QS_STEP_UVH, R_QS_STEP_UVV,
	R_QS_PRELOAD, R_QS_SRC_SIZE, R_QS_SRC_SIZE_UV, R_QS_DST_SIZE,
	R_QS_INIT_YH, R_QS_INIT_YV, R_QS_INIT_UVH, R_QS_INIT_UVV,
	NSAVED
};

static const uint16_t saved_regs[NSAVED] = {
	SSPP_SRC_SIZE, SSPP_SRC_XY, SSPP_OUT_SIZE, SSPP_OUT_XY, SSPP_SRC0_ADDR,
	SSPP_YSTRIDE0, SSPP_OP_MODE, SSPP_DECIMATION,
	SSPP_PE_C0_LR, SSPP_PE_C0_TB, SSPP_PE_C0_REQ,
	SSPP_PE_C12_LR, SSPP_PE_C12_TB, SSPP_PE_C12_REQ,
	SSPP_PE_C3_LR, SSPP_PE_C3_TB, SSPP_PE_C3_REQ,
	QS + QS_OP_MODE, QS + QS_PHASE_INIT,
	QS + QS_STEP_Y_H, QS + QS_STEP_Y_V, QS + QS_STEP_UV_H, QS + QS_STEP_UV_V,
	QS + QS_PRELOAD, QS + QS_SRC_SIZE, QS + QS_SRC_SIZE_UV, QS + QS_DST_SIZE,
	QS + QS_INIT_Y_H, QS + QS_INIT_Y_V, QS + QS_INIT_UV_H, QS + QS_INIT_UV_V,
};

static const char *stage;
static int step_row;

void exc_handler(uint64_t slot, uint64_t *regs)
{
	panic_screen(stage, slot, regs);
}

static void say(const char *label, const char *s)
{
	con_str(label, s);
	if (label[0]) {
		ramlog_str(label);
		ramlog_str(": ");
	}
	ramlog_str(s);
	ramlog_str("\n");
}

static void step(const char *s)
{
	char b[52], *p = put(b, s);
	int row = con_row();

	while (p < b + 50)
		*p++ = ' ';
	*p = 0;
	con_set_row(step_row);
	con_str("PASSO", b);
	con_set_row(row);
	ramlog_str("PASSO: ");
	ramlog_str(s);
	ramlog_str("\n");
}

/* "VIG0".."DMA3" built at run time: string-pointer tables create ABS64 relocs. */
static char pname_buf[8];
static const char *pname(int i)
{
	char *p = put(pname_buf, i <= 3 ? "VIG" : "DMA");
	p[0] = (char)('0' + (i & 3));
	p[1] = 0;
	return pname_buf;
}

static uint32_t flush_bit(int i)
{
	switch (i) {
	case 0: return 1U << 0;
	case 1: return 1U << 1;
	case 2: return 1U << 2;
	case 3: return 1U << 18;
	case 4: return 1U << 11;
	case 5: return 1U << 12;
	case 6: return 1U << 24;
	default: return 1U << 25;
	}
}

static void wait_ms(uint32_t ms)
{
	uint64_t f = ticks_per_s(), t0 = now();
	while (now() - t0 < (f / 1000) * ms) {
		if (((now() - t0) & 0xfffff) == 0)
			wdog_pet();
	}
}

/* Write a 32-bit register, read it back, refuse (0) if masked bits mismatch. */
static int wr32_chk(const char *what, uintptr_t addr, uint32_t val, uint32_t mask)
{
	char b[96], *p;
	uint32_t got;

	wr32(addr, val);
	dsb();
	got = rd32(addr);
	if ((got & mask) == (val & mask))
		return 1;
	p = put(b, what);
	p = put_hex(put(p, " w="), val, 8);
	put_hex(put(p, " r="), got, 8);
	say("RECUSA", b);
	return 0;
}

/* Trigger CTL_FLUSH and verify hardware latches it within timeout_ms. */
static int ctl_flush_wait(uintptr_t c, uint32_t flush, unsigned timeout_ms)
{
	uint64_t t0 = now(), lim = (ticks_per_s() / 1000) * timeout_ms;
	char b[80], *p;

	dsb();
	wr32(c + CTL_FLUSH, flush);
	dsb();
	while ((rd32(c + CTL_FLUSH) & flush) != 0) {
		if (now() - t0 > lim) {
			p = put_hex(put(b, "CTL_FLUSH timeout pending="), rd32(c + CTL_FLUSH), 8);
			put_hex(put(p, " mask="), flush, 8);
			say("RECUSA", b);
			return 0;
		}
		wdog_pet();
	}
	return 1;
}

/* Redirect fb_* drawing helpers to an off-screen/custom stride buffer. */
static struct fb saved_fb;
static void fb_target(uintptr_t base, uint32_t w, uint32_t h, uint32_t stride)
{
	saved_fb = fb;
	fb.base = base;
	fb.width = w;
	fb.height = h;
	fb.stride = stride;
}
static void fb_restore(void)
{
	fb = saved_fb;
}

static void count_frames(uintptr_t pp, uint32_t *sent, uint32_t *te)
{
	uint64_t f = ticks_per_s(), t0 = now();
	uint32_t te0 = rd32(pp + PP_INT_COUNT_VAL) >> 16;
	uint32_t last = rd32(pp + PP_LINE_COUNT) & 0xffff;

	*sent = 0;
	while (now() - t0 < f) {
		uint32_t l = rd32(pp + PP_LINE_COUNT) & 0xffff;
		if (l < last)
			(*sent)++;
		last = l;
		if (((now() - t0) & 0xfffff) == 0)
			wdog_pet();
	}
	*te = ((rd32(pp + PP_INT_COUNT_VAL) >> 16) - te0) & 0xffff;
}

static void show_rate(const char *what, uintptr_t pp)
{
	char b[96], *p;
	uint32_t sent, te;

	count_frames(pp, &sent, &te);
	p = put_dec(put(b, "enviados/s="), sent);
	p = put_dec(put(p, " TE/s="), te);
	put_hex(put(p, " autorefresh="), rd32(pp + PP_AUTOREFRESH_CONFIG), 8);
	say(what, b);
}

static void draw_b_card(void)
{
	fb_target(BUF_C, C_W, C_H, C_STRIDE);

	for (uint32_t y = 0; y < C_H; y += 64)
		fb_rect(0, (int)y, (int)C_W, 64, ((y / 64) & 1) ? 0x182848 : 0x305080);
	for (uint32_t x = 0; x < C_W; x += 69)
		fb_rect((int)x, 0, 3, (int)C_H, YELLOW);

	/* 6 px RED frame along exact source edges to prove 0 crop / 0 stride skew */
	fb_rect(0, 0, (int)C_W, 6, RED);
	fb_rect(0, (int)C_H - 6, (int)C_W, 6, RED);
	fb_rect(0, 0, 6, (int)C_H, RED);
	fb_rect((int)C_W - 6, 0, 6, (int)C_H, RED);

	/* R / G / B / W swatches to verify BGRA unpack visually */
	fb_rect(40,  40, 170, 80, RED);
	fb_rect(230, 40, 170, 80, GREEN);
	fb_rect(420, 40, 170, 80, 0x0040ff);
	fb_rect(610, 40, 170, 80, WHITE);
	fb_text(95,  68, 3, "R", WHITE, RED);
	fb_text(285, 68, 3, "G", BLACK, GREEN);
	fb_text(475, 68, 3, "B", WHITE, 0x0040ff);
	fb_text(665, 68, 3, "W", BLACK, WHITE);

	/* Main header + live frame counter box */
	fb_rect(36, 220, (int)C_W - 72, 620, BLACK);
	fb_rect(36, 220, (int)C_W - 72, 4, CYAN);
	fb_rect(36, 836, (int)C_W - 72, 4, CYAN);
	fb_text(60, 260, 4, "B: QSEED3 OP6 OK", WHITE, BLACK);
	fb_text(60, 340, 3, "828x1792 -> 1080x2280", CYAN, BLACK);
	fb_text(60, 410, 3, "stride=3328 (b3a8)", YELLOW, BLACK);
	fb_text(60, 480, 2, "1 pipe VIG0 + LM0 + PP0, 0% CPU", WHITE, BLACK);
	fb_rect(56, 560, (int)C_W - 112, 220, 0x101820);
	fb_text(72, 585, 5, "FRAME ", GREEN, 0x101820);

	/* Lower corner marker so full bottom scan-out is unmistakable */
	fb_rect(40, (int)C_H - 120, (int)C_W - 80, 80, BLACK);
	fb_text(60, (int)C_H - 96, 3, "BORDA INFERIOR y=1791", GREEN, BLACK);

	fb_restore();
	dsb();
}

static void draw_b_counter(uint32_t frame, uint32_t wraps, uint32_t secs)
{
	char l1[16], l2[36], *p;

	p = put_dec(l1, frame);
	while (p < l1 + 6)
		*p++ = ' ';
	*p = 0;

	p = put_dec(put(l2, "PP_WRAP "), wraps);
	p = put_dec(put(p, "  t="), secs);
	p = put(p, "s");
	while (p < l2 + 24)
		*p++ = ' ';
	*p = 0;

	fb_target(BUF_C, C_W, C_H, C_STRIDE);
	fb_text(72 + 6 * 8 * 5, 585, 5, l1, GREEN, 0x101820);
	fb_text(72, 695, 3, l2, WHITE, 0x101820);
	fb_restore();
	dsb();
}

void shim_main(uint64_t dtb)
{
	struct mdss_state st;
	char b[96], *p;
	int active[MDSS_NSSPP], nact = 0, vig_all = 1;
	uint32_t orig_addr[MDSS_NSSPP];
	static uint32_t saved_val[MDSS_NSSPP][NSAVED];
	int a_ok = 0, b_ok = 0, need_restore = 0;
	uint32_t b_frames = 0, b_wraps = 0;

	(void)dtb;
	fb_default();
	ramlog_open();
	con_clear(GREY);
	con_title("SHIM M9 (OP6): DISPLAY + QSEED3");

	stage = "watchdog";
	wdog_arm();
	step_row = con_row();
	con_skip();

	/* 0. Read-only MDSS dump. */
	stage = "dump";
	step("0 leitura do MDSS (sem tocar SMMU)");
	mdss_probe(&st);
	mdss_fb_from_sspp(&st);
	p = put_hex(put(b, "0x"), st.hw_version, 8);
	say("MDSS hw", b);

	if (st.ctl < 0 || st.ctl >= MDSS_NCTL) {
		say("RECUSA", "nenhum CTL ativo");
		goto end;
	}
	uintptr_t c = CTL_BASE(st.ctl);
	p = put_dec(put(b, "CTL"), (uint32_t)st.ctl);
	p = put_hex(put(p, " top="), rd32(c + CTL_TOP), 8);
	say("CTL", b);

	for (int lm = 0; lm < 2; lm++) {
		p = put_dec(put(b, "LM"), (uint32_t)lm);
		p = put_hex(put(p, " layer="), rd32(c + CTL_LAYER(lm)), 8);
		p = put_hex(put(p, " ext="), rd32(c + CTL_LAYER_EXT(lm)), 8);
		put_hex(put(p, " ext2="), rd32(c + CTL_LAYER_EXT2(lm)), 8);
		say("MIXER", b);
	}

	for (int i = 0; i < MDSS_NSSPP; i++) {
		uintptr_t s = MDSS_BASE + sspp_off[i];
		if (!st.sspp[i].size || st.sspp[i].addr < 0x80000000U)
			continue;
		active[nact++] = i;
		orig_addr[i] = st.sspp[i].addr;
		for (unsigned r = 0; r < NSAVED; r++)
			saved_val[i][r] = (i <= 3 || saved_regs[r] < QS) ? rd32(s + saved_regs[r]) : 0;
		if (i > 3)
			vig_all = 0;

		p = put(b, pname(i));
		p = put_hex(put(p, " src="), rd32(s + SSPP_SRC_SIZE), 8);
		p = put_hex(put(p, " xy="), rd32(s + SSPP_SRC_XY), 8);
		p = put_hex(put(p, " out="), rd32(s + SSPP_OUT_SIZE), 8);
		put_hex(put(p, " oxy="), rd32(s + SSPP_OUT_XY), 8);
		say("PIPE", b);

		p = put_hex(put(b, "  addr="), rd32(s + SSPP_SRC0_ADDR), 8);
		p = put_hex(put(p, " ystr="), rd32(s + SSPP_YSTRIDE0), 8);
		p = put_hex(put(p, " fmt="), rd32(s + SSPP_FORMAT), 8);
		put_hex(put(p, " unp="), rd32(s + SSPP_UNPACK), 8);
		say("", b);

		p = put_hex(put(b, "  op="), rd32(s + SSPP_OP_MODE), 8);
		if (i <= 3) {
			p = put_hex(put(p, " qs_ver="), rd32(s + QS + QS_VERSION), 8);
			put_hex(put(p, " qs_op="), rd32(s + QS + QS_OP_MODE), 8);
		}
		say("", b);
		wdog_pet();
	}

	uintptr_t pp0 = 0;
	for (int i = 0; i < MDSS_NPP; i++) {
		uintptr_t pp = MDSS_BASE + pp_off[i];
		uint32_t ar = st.pp_autorefresh[i];
		if (!pp0 && (ar & (1U << 31)))
			pp0 = pp;
		if (ar & (1U << 31)) {
			p = put_dec(put(b, "PP"), (uint32_t)i);
			p = put_hex(put(p, " ar="), ar, 8);
			p = put_hex(put(p, " line="), rd32(pp + PP_LINE_COUNT), 8);
			put_hex(put(p, " int="), rd32(pp + PP_INT_COUNT_VAL), 8);
			say("PP", b);
		}
	}

	p = put_dec(put(b, "pipes ativos="), (uint32_t)nact);
	put(p, vig_all ? " (VIG: com QSEED3)" : " (DMA: sem QSEED3)");
	say("INFO", b);

	if (nact == 0) {
		say("RECUSA", "nenhum pipe ativo");
		goto end;
	}
	if (!pp0)
		pp0 = MDSS_BASE + pp_off[0];

	/*
	 * A. Zero-copy: copy splash into BUF_B (0xb0000000), mark top+bottom,
	 * point active pipe(s) at BUF_B, readback-verify and flush.
	 */
	stage = "A copia";
	step("A copia splash 1080x2280 -> 0xb0000000");
	uint32_t panel_bytes = PANEL_H * (uint32_t)(fb.stride);
	for (uint32_t off = 0; off < panel_bytes; off += 4) {
		wr32(BUF_B + off, rd32(SPLASH_BASE + off));
		if ((off & 0xfffff) == 0)
			wdog_pet();
	}
	fb_target(BUF_B, PANEL_W, PANEL_H, fb.stride);
	fb_rect(0, 0, PANEL_W, 200, 0xc000c0);
	fb_text(40, 36, 5, "A: OP6 ZERO-COPY OK", WHITE, 0xc000c0);
	fb_text(40, 120, 3, "SCANOUT 0xb0000000 1080x2280", YELLOW, 0xc000c0);
	fb_rect(0, PANEL_H - 80, PANEL_W, 80, 0x00a000);
	fb_text(40, PANEL_H - 64, 3, "BUF_B RODAPE VISIVEL", WHITE, 0x00a000);
	fb_restore();
	dsb();

	stage = "A troca endereco";
	step("A pipes -> BUF_B + verifica + flush");
	need_restore = 1;
	uint32_t flush = CTL_FLUSH_CTL;
	for (int k = 0; k < nact; k++) {
		int i = active[k];
		uint32_t off = orig_addr[i] - (uint32_t)SPLASH_BASE;
		if (orig_addr[i] < SPLASH_BASE || off >= panel_bytes) {
			say("RECUSA", "SRC0 fora da janela splash");
			goto restore;
		}
		if (!wr32_chk("A SRC0", MDSS_BASE + sspp_off[i] + SSPP_SRC0_ADDR,
			      (uint32_t)BUF_B + off, 0xffffffffU))
			goto restore;
		flush |= flush_bit(i);
	}
	if (!ctl_flush_wait(c, flush, 500))
		goto restore;
	a_ok = 1;
	wait_ms(8000);

	/*
	 * B. Hardware scaling on VIG0 (828x1792, stride 3328 -> 1080x2280) with
	 * readback verification on every register and a live frame counter.
	 */
	if (!vig_all || nact != 1) {
		say("RECUSA", !vig_all ? "ha pipe DMA (sem QSEED3)" : "esperado 1 pipe VIG0 no OP6");
		goto restore;
	}
	int pi = active[0];
	uintptr_t s = MDSS_BASE + sspp_off[pi];
	uintptr_t q = s + QS;
	uint32_t ver = rd32(q + QS_VERSION);
	if (ver == 0 || ver == 0xffffffffU) {
		p = put_hex(put(b, "QS_VERSION invalida="), ver, 8);
		say("RECUSA", b);
		goto restore;
	}

	stage = "B cartao";
	step("B desenha 828x1792 stride=3328 em 0xb2000000");
	draw_b_card();
	draw_b_counter(0, 0, 0);

	stage = "B escalador";
	step("B programa VIG0 + QSEED3 + readback verify");
	uint32_t dw = PANEL_W, dh = PANEL_H;
	uint32_t sw = C_W, sh = C_H;
	uint32_t stx = (uint32_t)(((uint64_t)sw << PHASE_STEP_SHIFT) / dw);
	uint32_t sty = (uint32_t)(((uint64_t)sh << PHASE_STEP_SHIFT) / dh);
	uint32_t op = 1U | (3U << 16) | (1U << 14) | (1U << 10);
	op |= (ver == 0x1002) ? (1U << 30) : (1U << 29);

	if (!wr32_chk("B SRC0", s + SSPP_SRC0_ADDR, (uint32_t)BUF_C, 0xffffffffU)) goto restore;
	if (!wr32_chk("B YSTR0", s + SSPP_YSTRIDE0, C_STRIDE, 0xffffffffU)) goto restore;
	if (!wr32_chk("B SRC_XY", s + SSPP_SRC_XY, 0, 0xffffffffU)) goto restore;
	if (!wr32_chk("B SRC_SZ", s + SSPP_SRC_SIZE, (sh << 16) | sw, 0xffffffffU)) goto restore;
	if (!wr32_chk("B OUT_SZ", s + SSPP_OUT_SIZE, (dh << 16) | dw, 0xffffffffU)) goto restore;
	if (!wr32_chk("B OUT_XY", s + SSPP_OUT_XY, 0, 0xffffffffU)) goto restore;
	if (!wr32_chk("B DECIM", s + SSPP_DECIMATION, 0, 0xffffffffU)) goto restore;

	if (!wr32_chk("B PE0_LR", s + SSPP_PE_C0_LR, 0, 0xffffffffU)) goto restore;
	if (!wr32_chk("B PE0_TB", s + SSPP_PE_C0_TB, 0, 0xffffffffU)) goto restore;
	if (!wr32_chk("B PE0_RQ", s + SSPP_PE_C0_REQ, (sh << 16) | sw, 0xffffffffU)) goto restore;
	if (!wr32_chk("B PE12_LR", s + SSPP_PE_C12_LR, 0, 0xffffffffU)) goto restore;
	if (!wr32_chk("B PE12_TB", s + SSPP_PE_C12_TB, 0, 0xffffffffU)) goto restore;
	if (!wr32_chk("B PE12_RQ", s + SSPP_PE_C12_REQ, (sh << 16) | sw, 0xffffffffU)) goto restore;
	if (!wr32_chk("B PE3_LR", s + SSPP_PE_C3_LR, 0, 0xffffffffU)) goto restore;
	if (!wr32_chk("B PE3_TB", s + SSPP_PE_C3_TB, 0, 0xffffffffU)) goto restore;
	if (!wr32_chk("B PE3_RQ", s + SSPP_PE_C3_REQ, (sh << 16) | sw, 0xffffffffU)) goto restore;
	if (!wr32_chk("B SSPP_OP", s + SSPP_OP_MODE,
		      saved_val[pi][R_OP_MODE] | OP_PE_OVERRIDE, OP_PE_OVERRIDE))
		goto restore;

	if (ver == 0x1002) {
		if (!wr32_chk("B QS_PHINIT", q + QS_PHASE_INIT, 0, 0xffffffffU)) goto restore;
	} else {
		if (!wr32_chk("B QS_IYH", q + QS_INIT_Y_H, 0, 0xffffffffU)) goto restore;
		if (!wr32_chk("B QS_IYV", q + QS_INIT_Y_V, 0, 0xffffffffU)) goto restore;
		if (!wr32_chk("B QS_IUVH", q + QS_INIT_UV_H, 0, 0xffffffffU)) goto restore;
		if (!wr32_chk("B QS_IUVV", q + QS_INIT_UV_V, 0, 0xffffffffU)) goto restore;
	}
	if (!wr32_chk("B QS_SYH", q + QS_STEP_Y_H, stx, 0xffffffffU)) goto restore;
	if (!wr32_chk("B QS_SYV", q + QS_STEP_Y_V, sty, 0xffffffffU)) goto restore;
	if (!wr32_chk("B QS_SUVH", q + QS_STEP_UV_H, stx, 0xffffffffU)) goto restore;
	if (!wr32_chk("B QS_SUVV", q + QS_STEP_UV_V, sty, 0xffffffffU)) goto restore;
	if (!wr32_chk("B QS_PRE", q + QS_PRELOAD, 4U | (3U << 8) | (4U << 16) | (3U << 24), 0xffffffffU))
		goto restore;
	if (!wr32_chk("B QS_SRC", q + QS_SRC_SIZE, (sh << 16) | sw, 0xffffffffU)) goto restore;
	if (!wr32_chk("B QS_SUV", q + QS_SRC_SIZE_UV, (sh << 16) | sw, 0xffffffffU)) goto restore;
	if (!wr32_chk("B QS_DST", q + QS_DST_SIZE, (dh << 16) | dw, 0xffffffffU)) goto restore;
	if (!wr32_chk("B QS_OP", q + QS_OP_MODE, op, op)) goto restore;

	p = put(b, pname(pi));
	p = put_hex(put(p, " ver="), ver, 4);
	p = put_hex(put(p, " st="), stx, 6);
	put_hex(put(p, "/"), sty, 6);
	say("B", b);

	flush = CTL_FLUSH_CTL | flush_bit(pi);
	if (!ctl_flush_wait(c, flush, 500))
		goto restore;
	b_ok = 1;

	/* Live frame counter inside BUF_C for 15 s (autorefresh scans BUF_C every TE). */
	stage = "B contador";
	step("B contador ao vivo em 828x1792 (15 s)");
	uint64_t f = ticks_per_s(), t_start = now(), t_end = t_start + f * 15;
	uint64_t t_last_draw = t_start;
	uint32_t last_line = rd32(pp0 + PP_LINE_COUNT) & 0xffff;
	for (;;) {
		uint64_t t = now();
		if (t >= t_end)
			break;
		uint32_t line = rd32(pp0 + PP_LINE_COUNT) & 0xffff;
		int wrapped = (line < last_line);
		last_line = line;
		if (wrapped) {
			b_wraps++;
			b_frames++;
			draw_b_counter(b_frames, b_wraps, (uint32_t)((t - t_start) / f));
			t_last_draw = t;
		} else if (t - t_last_draw >= f / 30) {
			/* Fallback tick (~30 Hz) if PP_LINE_COUNT is static on a given panel */
			b_frames++;
			draw_b_counter(b_frames, b_wraps, (uint32_t)((t - t_start) / f));
			t_last_draw = t;
		}
		wdog_pet();
	}

restore:
	if (need_restore) {
		stage = "volta";
		step("restaura layout do ABL + verifica");
		flush = CTL_FLUSH_CTL;
		for (int k = 0; k < nact; k++) {
			int i = active[k];
			uintptr_t sp = MDSS_BASE + sspp_off[i];
			for (unsigned r = 0; r < NSAVED; r++) {
				if (i <= 3 || saved_regs[r] < QS)
					wr32(sp + saved_regs[r], saved_val[i][r]);
			}
			dsb();
			if (rd32(sp + SSPP_SRC0_ADDR) != saved_val[i][R_SRC0_ADDR])
				say("RECUSA", "falha ao restaurar SSPP_SRC0_ADDR");
			if (i <= 3 && rd32(sp + QS + QS_OP_MODE) != saved_val[i][R_QS_OP_MODE])
				say("RECUSA", "falha ao restaurar QS_OP_MODE");
			flush |= flush_bit(i);
		}
		ctl_flush_wait(c, flush, 500);
		say("RESTAURA", "Registradores do ABL restaurados.");
	}

	p = put(b, a_ok ? "A=OK" : "A=FALHA");
	p = put(p, b_ok ? "  B=OK" : "  B=FALHA");
	p = put_dec(put(p, "  frames="), b_frames);
	put_dec(put(p, "  wraps="), b_wraps);
	say("RESUMO", b);

	/* C. Measure PP0 frame rate and TE rate at OP6 autorefresh. */
	stage = "C 60 Hz";
	step("C mede quadros/s em PP0");
	uint32_t ar0 = rd32(pp0 + PP_AUTOREFRESH_CONFIG);
	if (!(ar0 & (1U << 31))) {
		say("C", "pulado: autorefresh desligado em PP0");
		goto end;
	}
	show_rate("C atual", pp0);
	if ((ar0 & 0xffffU) != 1U) {
		step("C ajusta PP0 autorefresh -> 0x80000001");
		if (wr32_chk("C PP0_AR", pp0 + PP_AUTOREFRESH_CONFIG,
			     (1U << 31) | 1U, 0xffffffffU)) {
			wait_ms(200);
			show_rate("C 60Hz", pp0);
		}
	} else {
		show_rate("C confir", pp0);
	}
	say("C", "60 Hz OK se enviados/s ~60 com autorefresh=80000001.");

end:
	stage = "fim";
	step("fim");
	con_skip();
	say("", "Tire uma foto desta tela!");
	countdown_reboot(con_row(), END_SECONDS);
}
