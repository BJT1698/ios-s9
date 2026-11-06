/*
 * Galaxy S9 milestone 9: can the display hardware do iOS's presentation work?
 *
 * Today the kernel copies every iOS frame (1125x2436) into the panel memory
 * and stretches it to 1440x2960 on the CPU: ~10 ms a frame, ~30 % of a core
 * at 30 fps plus memory bandwidth taken from the renderer. The SDM845 display
 * engine (SDE) fetches pixels from RAM by itself through its source pipes
 * (SSPP) and the VIG pipes have a QSEED3 scaler. If both work from our side:
 *   A. ZERO-COPY: a pipe can scan out a buffer outside the ABL's splash
 *      region (its SMMU stream must let it read any RAM) -> the kernel can
 *      point the pipe at iOS's own IOSurface instead of copying it.
 *   B. HARDWARE SCALE: a VIG pipe enlarges a 828x1792 (@2x canvas, iPhone XR
 *      size) buffer to the panel -> no CPU work at all for presentation.
 *   C. 60 HZ: ABL leaves pingpong autorefresh at 0x80000002 (a frame every 2nd
 *      TE = 30 Hz). Count the frames the pipe sends per second (write-pointer
 *      wraps) and the panel TEs (read-pointer frame count), switch to every
 *      TE (0x80000001) and count again.
 *
 * Steps (each shown on screen first, so a hang leaves the step in the photo):
 *   0. read-only dump: CTL (which pipes blend into which mixer), each active
 *      pipe's rectangles/address/format, VIG scaler version and mode;
 *   A. copy the screen to buffer B (0xb0000000, free RAM per M8), mark it,
 *      point the active pipes at B, flush;
 *   B. draw a 828x1792 test card at C (0xb2000000), point the VIG pipes at C
 *      with QSEED3 bilinear 414x1792 -> 720x2960 per mixer half, flush;
 *   C. after restoring the ABL layout: frame rate at autorefresh 2, then 1.
 * Register layouts: sde_hw_ctl.c, sde_hw_sspp.c, sde_hw_util.c
 * (sde_hw_setup_scaler3) and sde_plane.c (_sde_plane_setup_scaler3) of the
 * Samsung/LineageOS kernel. MMU off: everything is Device memory.
 */
#include <stdint.h>
#include "hw.h"
#include "fb.h"
#include "con.h"
#include "plat.h"
#include "mdss.h"

#define END_SECONDS	90

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
#define SSPP_PE_C0_LR	0x100
#define SSPP_PE_C0_TB	0x104
#define SSPP_PE_C0_REQ	0x108
#define SSPP_PE_C12_LR	0x110
#define SSPP_PE_C12_TB	0x114
#define SSPP_PE_C12_REQ	0x118
#define SSPP_PE_C3_LR	0x120
#define SSPP_PE_C3_TB	0x124
#define SSPP_PE_C3_REQ	0x128
#define SSPP_DECIMATION	0xb4
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

#define BUF_B		0xb0000000UL	/* panel-sized copy, 17 MiB */
#define BUF_C		0xb2000000UL	/* 828x1792 test card, 5.7 MiB */
#define C_W		828
#define C_H		1792

/* Pingpong blocks (sde_hw_pingpong.c) */
#define PP_INT_COUNT_VAL	0x014	/* [31:16] read-pointer (TE) frame count */
#define PP_LINE_COUNT		0x02c	/* write-pointer line: wraps once per frame sent */
#define PP_AUTOREFRESH_CONFIG	0x030
static const uint32_t pp_off[MDSS_NPP] = { 0x71000, 0x71800, 0x72000, 0x72800, 0x73000 };

static const uint32_t sspp_off[MDSS_NSSPP] = {
	0x5000, 0x7000, 0x9000, 0xb000, 0x25000, 0x27000, 0x29000, 0x2b000,
};

static const char *stage;
static int step_row;

void exc_handler(uint64_t slot, uint64_t *regs)
{
	panic_screen(stage, slot, regs);
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
}

/* "VIG0".."DMA3" built at run time: a table of string pointers would need ABS relocations. */
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

/* About a millisecond per unit, from the architected counter (19.2 MHz). */
static void wait_ms(uint32_t ms)
{
	uint64_t f = rsr("cntfrq_el0"), t0 = rsr("cntpct_el0");
	while (rsr("cntpct_el0") - t0 < f / 1000 * ms) {
		if (((rsr("cntpct_el0") - t0) & 0xfffff) == 0)
			wdog_pet();
	}
}

/* Draw on another buffer with the fb_* helpers, then come back. */
static struct fb saved;
static void fb_target(uintptr_t base, uint32_t w, uint32_t h)
{
	saved = fb;
	fb.base = base;
	fb.width = w;
	fb.height = h;
	fb.stride = w * fb.bpp;
}
static void fb_restore(void)
{
	fb = saved;
}

/* Frames the pingpong sent and panel TEs seen in one second. */
static void count_frames(uintptr_t pp, uint32_t *sent, uint32_t *te)
{
	uint64_t f = rsr("cntfrq_el0"), t0 = rsr("cntpct_el0");
	uint32_t te0 = rd32(pp + PP_INT_COUNT_VAL) >> 16, last = rd32(pp + PP_LINE_COUNT) & 0xffff;

	*sent = 0;
	while (rsr("cntpct_el0") - t0 < f) {
		uint32_t l = rd32(pp + PP_LINE_COUNT) & 0xffff;
		if (l < last)
			(*sent)++;
		last = l;
		if (((rsr("cntpct_el0") - t0) & 0xfffff) == 0)
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
	con_str(what, b);
}

void shim_main(uint64_t dtb)
{
	struct mdss_state st;
	char b[96], *p;
	int active[MDSS_NSSPP], nact = 0, vig_all = 1;
	uint32_t orig_addr[MDSS_NSSPP];
	/* Every register A/B touch, to put the ABL layout back at the end. */
	static const uint16_t saved_regs[] = {
		SSPP_SRC_SIZE, SSPP_SRC_XY, SSPP_OUT_SIZE, SSPP_OUT_XY, SSPP_SRC0_ADDR,
		SSPP_YSTRIDE0, SSPP_OP_MODE, SSPP_DECIMATION,
		SSPP_PE_C0_LR, SSPP_PE_C0_TB, SSPP_PE_C0_REQ, SSPP_PE_C12_LR, SSPP_PE_C12_TB,
		SSPP_PE_C12_REQ, SSPP_PE_C3_LR, SSPP_PE_C3_TB, SSPP_PE_C3_REQ,
		QS + QS_OP_MODE, QS + QS_PHASE_INIT, QS + QS_STEP_Y_H, QS + QS_STEP_Y_V,
		QS + QS_STEP_UV_H, QS + QS_STEP_UV_V, QS + QS_PRELOAD, QS + QS_SRC_SIZE,
		QS + QS_SRC_SIZE_UV, QS + QS_DST_SIZE, QS + QS_INIT_Y_H, QS + QS_INIT_Y_V,
		QS + QS_INIT_UV_H, QS + QS_INIT_UV_V,
	};
#define NSAVED (sizeof(saved_regs) / sizeof(saved_regs[0]))
	static uint32_t saved_val[MDSS_NSSPP][NSAVED];

	(void)dtb;
	fb_default();
	con_clear(GREY);
	con_title("SHIM M9: DISPLAY POR HARDWARE");
	stage = "watchdog";
	wdog_arm();
	step_row = con_row();
	con_skip();

	/* 0. Read-only dump. */
	stage = "dump";
	step("0 leitura do MDSS");
	mdss_probe(&st);
	mdss_fb_from_sspp(&st);
	con_hex("MDSS hw", st.hw_version);
	if (st.ctl < 0) {
		con_str("CTL", "nenhum CTL ativo: nada a testar");
		goto end;
	}
	uintptr_t c = CTL_BASE(st.ctl);
	p = put_dec(put(b, "CTL"), (uint32_t)st.ctl);
	p = put_hex(put(p, " top="), rd32(c + CTL_TOP), 8);
	con_str("CTL", b);
	for (int lm = 0; lm < 2; lm++) {
		p = put_dec(put(b, "LM"), (uint32_t)lm);
		p = put_hex(put(p, " layer="), rd32(c + CTL_LAYER(lm)), 8);
		p = put_hex(put(p, " ext="), rd32(c + CTL_LAYER_EXT(lm)), 8);
		put_hex(put(p, " ext2="), rd32(c + CTL_LAYER_EXT2(lm)), 8);
		con_str("MIXER", b);
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
		con_str("PIPE", b);
		p = put_hex(put(b, "  addr="), rd32(s + SSPP_SRC0_ADDR), 8);
		p = put_hex(put(p, " ystr="), rd32(s + SSPP_YSTRIDE0), 8);
		p = put_hex(put(p, " fmt="), rd32(s + SSPP_FORMAT), 8);
		put_hex(put(p, " op="), rd32(s + SSPP_OP_MODE), 8);
		con_str("", b);
		if (i <= 3) {
			p = put_hex(put(b, "  qseed ver="), rd32(s + QS + QS_VERSION), 8);
			put_hex(put(p, " op="), rd32(s + QS + QS_OP_MODE), 8);
			con_str("", b);
		}
		wdog_pet();
	}
	p = put_dec(put(b, "pipes ativos="), (uint32_t)nact);
	put(p, vig_all ? " (todos VIG: tem escalador)" : " (ha pipe DMA: sem escalador)");
	con_str("", b);
	if (nact == 0)
		goto end;

	/*
	 * A. Zero-copy: copy what the pipes show into B (same layout), mark it,
	 * point each pipe at the same offset inside B. The mark says it worked.
	 */
	stage = "A copia";
	step("A copia a tela para 0xb0000000");
	uint32_t panel_bytes = PANEL_H * (uint32_t)(fb.stride);
	for (uint32_t off = 0; off < panel_bytes; off += 4) {
		wr32(BUF_B + off, rd32(SPLASH_BASE + off));
		if ((off & 0xfffff) == 0)
			wdog_pet();
	}
	fb_target(BUF_B, PANEL_W, PANEL_H);
	fb.stride = saved.stride;
	fb_rect(0, 0, PANEL_W, 200, 0xc000c0);
	fb_text(40, 40, 6, "A: ZERO-COPY OK", WHITE, 0xc000c0);
	fb_restore();
	dsb();

	stage = "A troca endereco";
	step("A pipes -> buffer B (se travar: SMMU)");
	uint32_t flush = CTL_FLUSH_CTL;
	for (int k = 0; k < nact; k++) {
		int i = active[k];
		uint32_t off = orig_addr[i] - (uint32_t)SPLASH_BASE;
		if (orig_addr[i] < SPLASH_BASE || off >= panel_bytes)
			continue;
		wr32(MDSS_BASE + sspp_off[i] + SSPP_SRC0_ADDR, (uint32_t)BUF_B + off);
		flush |= flush_bit(i);
	}
	dsb();
	wr32(c + CTL_FLUSH, flush);
	wait_ms(300);
	p = put_hex(put(b, "flush="), flush, 8);
	put_hex(put(p, " pendente="), rd32(c + CTL_FLUSH), 8);
	con_str("A", b);
	/* The console keeps drawing on the splash copy: the photo tells which one shows. */
	con_str("A", "Se a faixa ROXA 'ZERO-COPY OK' aparece no topo, A funcionou.");
	con_str("A", "(este texto so aparece se a tela voltou ao splash)");
	wait_ms(20000);

	/* B. Hardware scaling on the VIG pipes. */
	if (!vig_all) {
		con_str("B", "pulado: um pipe ativo e DMA (sem escalador)");
		goto restore;
	}
	stage = "B cartao";
	step("B desenha 828x1792 em 0xb2000000");
	fb_target(BUF_C, C_W, C_H);
	for (int y = 0; y < C_H; y += 64)
		fb_rect(0, y, C_W, 64, ((y / 64) & 1) ? 0x203060 : 0x406090);
	for (int x = 0; x < C_W; x += 69)
		fb_rect(x, 0, 3, C_H, YELLOW);
	fb_rect(0, 0, C_W, 4, RED);
	fb_rect(0, C_H - 4, C_W, 4, RED);
	fb_rect(0, 0, 4, C_H, RED);
	fb_rect(C_W - 4, 0, 4, C_H, RED);
	fb_text(30, 300, 4, "B: ESCALA POR", WHITE, BLACK);
	fb_text(30, 380, 4, "HARDWARE OK", WHITE, BLACK);
	fb_text(30, 500, 2, "828x1792 -> 1440x2960, 0% CPU", WHITE, BLACK);
	fb_restore();
	dsb();

	stage = "B escalador";
	step("B QSEED3 414x1792 -> 720x2960 por metade");
	flush = CTL_FLUSH_CTL;
	for (int k = 0; k < nact; k++) {
		int i = active[k];
		uintptr_t s = MDSS_BASE + sspp_off[i];
		uintptr_t q = s + QS;
		/*
		 * The panel columns this pipe covered (its fetch offset in the
		 * splash and its output width) -> the matching card columns.
		 */
		uint32_t px0 = ((orig_addr[i] - (uint32_t)SPLASH_BASE) % (uint32_t)saved.stride) / 4;
		uint32_t dw = st.sspp[i].size & 0xffff, dh = PANEL_H;
		uint32_t out = saved_val[i][2];
		if ((out & 0xffff) != 0)
			dw = out & 0xffff;
		uint32_t cx0 = px0 * C_W / PANEL_W;
		uint32_t sw = dw * C_W / PANEL_W, sh = C_H;
		uint32_t half = px0 ? 1 : 0;
		uint32_t ver = rd32(q + QS_VERSION);

		wr32(s + SSPP_SRC0_ADDR, (uint32_t)BUF_C + cx0 * 4);
		wr32(s + SSPP_YSTRIDE0, C_W * 4);
		wr32(s + SSPP_SRC_XY, 0);
		wr32(s + SSPP_SRC_SIZE, (sh << 16) | sw);
		wr32(s + SSPP_OUT_SIZE, (dh << 16) | dw);
		wr32(s + SSPP_DECIMATION, 0);
		/* Pixel extension: the scaler fetches the whole source rectangle. */
		wr32(s + SSPP_PE_C0_LR, 0);
		wr32(s + SSPP_PE_C0_TB, 0);
		wr32(s + SSPP_PE_C0_REQ, (sh << 16) | sw);
		wr32(s + SSPP_PE_C12_LR, 0);
		wr32(s + SSPP_PE_C12_TB, 0);
		wr32(s + SSPP_PE_C12_REQ, (sh << 16) | sw);
		wr32(s + SSPP_PE_C3_LR, 0);
		wr32(s + SSPP_PE_C3_TB, 0);
		wr32(s + SSPP_PE_C3_REQ, (sh << 16) | sw);
		wr32(s + SSPP_OP_MODE, rd32(s + SSPP_OP_MODE) | OP_PE_OVERRIDE);

		uint32_t stx = (uint32_t)(((uint64_t)sw << PHASE_STEP_SHIFT) / dw);
		uint32_t sty = (uint32_t)(((uint64_t)sh << PHASE_STEP_SHIFT) / dh);
		if (ver == 0x1002) {
			wr32(q + QS_PHASE_INIT, 0);
		} else {
			wr32(q + QS_INIT_Y_H, 0);
			wr32(q + QS_INIT_Y_V, 0);
			wr32(q + QS_INIT_UV_H, 0);
			wr32(q + QS_INIT_UV_V, 0);
		}
		wr32(q + QS_STEP_Y_H, stx);
		wr32(q + QS_STEP_Y_V, sty);
		wr32(q + QS_STEP_UV_H, stx);
		wr32(q + QS_STEP_UV_V, sty);
		wr32(q + QS_PRELOAD, 4 | (3 << 8) | (4 << 16) | (3 << 24));
		wr32(q + QS_SRC_SIZE, (sh << 16) | sw);
		wr32(q + QS_SRC_SIZE_UV, (sh << 16) | sw);
		wr32(q + QS_DST_SIZE, (dh << 16) | dw);
		/* enable, bilinear Y/RGB, not 10-bit, alpha on + bilinear alpha */
		uint32_t op = 1U | (3U << 16) | (1U << 14) | (1U << 10);
		op |= ver == 0x1002 ? (1U << 30) : (1U << 29);
		wr32(q + QS_OP_MODE, op);
		flush |= flush_bit(i);

		p = put(b, pname(i));
		p = put_dec(put(p, " metade="), half);
		p = put_hex(put(p, " ver="), ver, 4);
		p = put_hex(put(p, " passo="), stx, 6);
		put_hex(put(p, "/"), sty, 6);
		con_str("B", b);
		wdog_pet();
	}
	dsb();
	wr32(c + CTL_FLUSH, flush);
	wait_ms(300);
	put_hex(put(b, "pendente="), rd32(c + CTL_FLUSH), 8);
	con_str("B", b);
	wait_ms(20000);

restore:
	/* Back to the ABL layout so the countdown is visible. */
	stage = "volta";
	step("volta ao splash");
	flush = CTL_FLUSH_CTL;
	for (int k = 0; k < nact; k++) {
		int i = active[k];
		uintptr_t s = MDSS_BASE + sspp_off[i];
		for (unsigned r = 0; r < NSAVED; r++)
			if (i <= 3 || saved_regs[r] < QS)
				wr32(s + saved_regs[r], saved_val[i][r]);
		flush |= flush_bit(i);
	}
	dsb();
	wr32(c + CTL_FLUSH, flush);
	con_str("", "Registradores do ABL restaurados.");

	/* C. 60 Hz: autorefresh on every TE instead of every other one. */
	stage = "C 60 Hz";
	step("C mede quadros/s com autorefresh do ABL");
	int npp = 0;
	uintptr_t pp0 = 0;
	for (int i = 0; i < MDSS_NPP; i++) {
		if (!(st.pp_autorefresh[i] & (1U << 31)))
			continue;
		if (!npp++)
			pp0 = MDSS_BASE + pp_off[i];
	}
	if (!npp) {
		con_str("C", "pulado: nenhum pingpong com autorefresh");
		goto end;
	}
	show_rate("C antes", pp0);
	step("C autorefresh a cada TE (0x80000001)");
	for (int i = 0; i < MDSS_NPP; i++)
		if (st.pp_autorefresh[i] & (1U << 31))
			wr32(MDSS_BASE + pp_off[i] + PP_AUTOREFRESH_CONFIG, (1U << 31) | 1);
	dsb();
	wait_ms(200);
	show_rate("C depois", pp0);
	show_rate("C depois", pp0);
	con_str("C", "60 Hz OK se 'depois' mostra ~60 enviados/s e a tela segue normal.");

end:
	stage = "fim";
	step("fim");
	con_skip();
	con_str("", "Tire uma foto desta tela!");
	countdown_reboot(con_row(), END_SECONDS);
}
