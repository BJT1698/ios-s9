/*
 * OnePlus 6 Adreno 630 bring-up, shared by shim stages M10 (GMU boot) and
 * M11 (GPU SMMU + HFI). Ported from the Galaxy S9 stages s9/shim/m10.c and
 * m11.c (same SDM845 / A630 v2), re-checked against what runs on the OP6
 * itself: the OP6 live DT is mainline sdm845.dtsi (pmOS), so the sequence
 * follows Linux drivers/gpu/drm/msm/adreno/a6xx_gmu.c (v6.6), "legacy" a630
 * path, where it differs from the S9's kgsl. Facts (addresses, register
 * numbers, values) only; no code copied (Linux is GPL, Bergamota has no
 * licence). Differences S9 -> OP6 are listed in op6/GPU-NOTES.md.
 *
 * What these helpers never do: touch the GX GDSC (gpucc 0x100c), enable the
 * GMU's RPMh interface (GMU_RPMH_CTRL stays 0, so the GMU votes nothing),
 * send any RPMh vote from the CPU (cmd-db is only read), or touch a PMIC.
 * GPU_RPMH=1 at build time adds the mainline GPU-RSC wake handshake, which
 * makes the GPU's PDC send its wake TCS (mx/cx/0x30080 level votes).
 */
#include <stdint.h>
#include "hw.h"
#include "fb.h"
#include "con.h"
#include "plat.h"
#include "ramlog.h"
#include "rpmh.h"
#include "gpu.h"

#if defined(__has_include)
#if __has_include("fw_a630_gmu.h")
#include "fw_a630_gmu.h"	/* op6/tools/gpu-fw-embed.py: a630_gmu_fw[], HAVE_GMU_FW (git-ignored) */
#endif
#endif

#ifndef GPU_RPMH
#define GPU_RPMH		0	/* 1 = mainline a6xx_rpmh_stop/start around the firmware load */
#endif
#ifndef GMU_CHIPID
/* a6xx_gmu_fw_start: chip_id 0x06030002 ("qcom,adreno-630.2", the OP6 DT)
 * repacked = 0x06030200. The S9's kgsl wrote 0x06030100 (qcom,chipid
 * 0x06030001) and booted too. */
#define GMU_CHIPID		0x06030200u
#endif
#ifndef GPU_PDC_GFX
#define GPU_PDC_GFX		0x30080u	/* mainline pdc_address_offset for a630 (and S9 kgsl) */
#endif

#define SW_COLLAPSE		(1u << 0)
#define PWR_ON			(1u << 31)
#define CBCR_ENABLE		(1u << 0)
#define CBCR_OFF		(1u << 31)
#define CMD_UPDATE		(1u << 0)
#define GMU_RCG_200MHZ		0x602u		/* src 6 = GPLL0_OUT_MAIN_DIV, div 1.5 (field 2) */
#define CX_GMU_WAKE_SLEEP	(0xffu << 4)	/* gpucc probe: wake [11:8] = sleep [7:4] = 0xf */
#define FENCE_RANGE		((1u << 31) | (0xau << 18) | 0xa0u)	/* mainline (kgsl: 0x8A0 | 0x28<<18) */

static const char *stage;
static int step_row;

void exc_handler(uint64_t slot, uint64_t *regs)
{
	panic_screen(stage, slot, regs);
}

void saylog(const char *label, const char *s)
{
	ramlog_str(label);
	ramlog_str(": ");
	ramlog_str(s);
	ramlog_str("\n");
}

void say(const char *label, const char *s)
{
	con_str(label, s);
	saylog(label, s);
	wdog_pet();
}

void sayhex(const char *label, uint32_t v)
{
	char b[12];

	put_hex(b, v, 8);
	say(label, b);
}

/* Overwrites one line: if the shim hangs, the photo shows the last step. */
void step(const char *s)
{
	char b[40], *p = b;
	int row = con_row();

	/* one screen row: 36 columns fit after the label */
	for (int i = 0; s[i] && i < 36; i++)
		*p++ = s[i];
	stage = s;
	ramlog_str("PASSO ");
	ramlog_str(s);
	ramlog_str("\n");
	while (p < b + 36)
		*p++ = ' ';
	*p = 0;
	con_set_row(step_row);
	con_str("PASSO", b);
	con_set_row(row);
	wdog_pet();
}

void gpu_screen(const char *title, const char *logtitle)
{
	ramlog_open();
	ramlog_str(logtitle);
	fb_default();
	con_clear(GREY);
	con_title(title);
	stage = "watchdog";
	wdog_arm();
	step_row = con_row();
	con_skip();
}

int gpu_have_fw(void)
{
#ifdef HAVE_GMU_FW
	return 1;
#else
	return 0;
#endif
}

unsigned gpu_fw_words(void)
{
#ifdef HAVE_GMU_FW
	return sizeof(a630_gmu_fw) / sizeof(a630_gmu_fw[0]);
#else
	return 0;
#endif
}

static void wr(uintptr_t a, uint32_t v)
{
	wr32(a, v);
	dsb();
}

static int poll(uintptr_t a, uint32_t mask, uint32_t want, unsigned ms)
{
	for (unsigned i = 0; i <= ms; i++) {
		if ((rd32(a) & mask) == want)
			return 1;
		delay_ms(1);
		if ((i & 255) == 255)
			wdog_pet();
	}
	return 0;
}

/* ---- state as ABL left it (read-only) ---- */

void gpu_state(int verbose)
{
	char b[96], *p;

	p = put_hex(put(b, "gpll0v="), rd32(GCC + GCC_GPLL0_VOTE), 8);
	p = put_hex(put(p, " ahb="), rd32(GCC + GCC_GPU_CFG_AHB), 8);
	p = put_hex(put(p, " memnoc="), rd32(GCC + GCC_GPU_MEMNOC_GFX), 8);
	put_hex(put(p, " axi="), rd32(GCC + GCC_DDRSS_GPU_AXI), 8);
	(verbose ? say : saylog)("GCC", b);
	if (rd32(GCC + GCC_GPU_CFG_AHB) & CBCR_OFF) {
		say("GPUCC", "cfg_ahb desligado: gpucc nao lido (acesso travaria)");
		return;
	}
	p = put_hex(put(b, "cx="), rd32(GPUCC + GPUCC_CX_GDSCR), 8);
	p = put_hex(put(p, " gx="), rd32(GPUCC + GPUCC_GX_GDSCR), 8);
	p = put_hex(put(p, " rcg="), rd32(GPUCC + GPUCC_GMU_CFG_RCGR), 8);
	put_hex(put(p, " gmu="), rd32(GPUCC + GPUCC_CX_GMU_CBCR), 8);
	(verbose ? say : saylog)("GPUCC", b);
}

/* ---- power: CX GDSC + the GMU's clocks at 200 MHz (gmu opp-200000000) ---- */

static int branch_on(uintptr_t cbcr, const char *name, char **line)
{
	wr(cbcr, rd32(cbcr) | CBCR_ENABLE);
	int on = poll(cbcr, CBCR_OFF, 0, 200);
	if (line) {
		*line = put(put(*line, " "), name);
		*line = put_hex(put(*line, "="), rd32(cbcr), 8);
	}
	return on;
}

static int cx_gdsc_on(void)
{
	uint32_t v = rd32(GPUCC + GPUCC_CX_GDSCR);

	if (v & SW_COLLAPSE)
		wr(GPUCC + GPUCC_CX_GDSCR, v & ~SW_COLLAPSE);
	for (int i = 0; i < 2000; i++) {
		if ((rd32(GPUCC + GPUCC_CX_GDSCR) & PWR_ON) || (rd32(GPUCC + GPUCC_CX_HW_CTRL) & PWR_ON))
			return 1;
		delay_ms(1);
	}
	return 0;
}

int gpu_power_on(int verbose)
{
	char b[160], *p = b;

	step("energia: voto GPLL0 + gcc cfg_ahb");
	wr(GCC + GCC_GPLL0_VOTE, rd32(GCC + GCC_GPLL0_VOTE) | (1u << 15) | (1u << 16));
	if (!branch_on(GCC + GCC_GPU_CFG_AHB, "cfg_ahb", 0)) {
		say("CX", "gcc cfg_ahb nao liga: parando");
		return 0;
	}
	step("energia: CX GDSC (gpucc 0x106c)");
	if (!cx_gdsc_on()) {
		sayhex("CX GDSC NAO liga", rd32(GPUCC + GPUCC_CX_GDSCR));
		return 0;
	}
	p = put_hex(put(b, "gdscr="), rd32(GPUCC + GPUCC_CX_GDSCR), 8);
	put_hex(put(p, " hw="), rd32(GPUCC + GPUCC_CX_HW_CTRL), 8);
	say("CX", b);

	step("energia: RCG do GMU 200 MHz");
	wr(GPUCC + GPUCC_GMU_CFG_RCGR, GMU_RCG_200MHZ);
	wr(GPUCC + GPUCC_GMU_CMD_RCGR, rd32(GPUCC + GPUCC_GMU_CMD_RCGR) | CMD_UPDATE);
	poll(GPUCC + GPUCC_GMU_CMD_RCGR, CMD_UPDATE, 0, 200);
	step("energia: relogios do GMU / SMMU");
	/* gpucc probe: cx_gmu wake/sleep counters = 0xf, before the branch runs */
	wr(GPUCC + GPUCC_CX_GMU_CBCR, rd32(GPUCC + GPUCC_CX_GMU_CBCR) | CX_GMU_WAKE_SLEEP);
	p = put_hex(put(b, "rcg="), rd32(GPUCC + GPUCC_GMU_CFG_RCGR), 4);
	branch_on(GPUCC + GPUCC_CXO_CBCR, "cxo", &p);
	branch_on(GPUCC + GPUCC_AHB_CBCR, "ahb", &p);
	int gmu = branch_on(GPUCC + GPUCC_CX_GMU_CBCR, "gmu", &p);
	branch_on(GCC + GCC_GPU_MEMNOC_GFX, "memnoc", &p);
	branch_on(GCC + GCC_DDRSS_GPU_AXI, "axi", &p);
	branch_on(GCC + GCC_GPU_SNOC_DVM, "dvm", &p);
	if (verbose)
		say("CLK", b);
	else
		saylog("CLK", b);
	if (!gmu) {
		say("CLK", "gpu_cc_cx_gmu_clk nao liga: parando");
		return 0;
	}
	return 1;
}

/* ---- cmd-db: ARC level lists and the names behind fixed addresses ---- */

struct arc arc_cx, arc_mx, arc_gfx;

static void arc_line(const char *name, struct arc *a, int verbose)
{
	char b[160], *p;

	a->n = rh_aux(name, &a->addr, a->lvl, 16);
	p = put(b, name);
	if (a->n < 0) {
		put_dec(put(p, " ERRO "), (uint32_t)-a->n);
	} else {
		p = put_hex(put(p, " @"), a->addr, 5);
		for (int i = 0; i < a->n; i++)
			p = put_hex(put(p, i ? "," : ":"), a->lvl[i], 3);
	}
	if (verbose)
		say("ARC", b);
	else
		saylog("ARC", b);
}

static void name_line(const char *label, const uint32_t *addrs, unsigned n, int verbose)
{
	char b[160], nm[9], *p = b;

	*p = 0;
	for (unsigned i = 0; i < n; i++) {
		p = put_hex(put(p, i ? " " : ""), addrs[i], 5);
		p = put(put(p, "="), rh_name(addrs[i], nm) ? "?" : nm);
	}
	if (verbose)
		say(label, b);
	else
		saylog(label, b);
}

/* 0 = cmd-db readable and the three ARC lists found. */
int gpu_cmddb(int verbose)
{
	static const uint32_t pdc[3] = { 0x30000, 0x30010, GPU_PDC_GFX };
	static const uint32_t bwa[6] = { 0x50000, 0x5005c, 0x5000c, 0x50034, 0x5007c, 0x5004c };
	char b[96];

	step("cmd-db: le (nenhum voto)");
	int rc = rh_init(b);
	saylog("RSC", b);
	if (rc) {
		say("RSC", b);
		return -1;
	}
	arc_line("cx.lvl", &arc_cx, verbose);
	arc_line("mx.lvl", &arc_mx, verbose);
	arc_line("gfx.lvl", &arc_gfx, verbose);
	/* the PDC's sleep/wake TCS targets (a6xx_gmu_rpmh_init) */
	name_line("PDC TCS", pdc, 3, verbose);
	/* BCMs behind the fixed sdm845 BW-table addresses (a6xx_build_bw_table) */
	name_line("BW BCM", bwa, 6, verbose);
	return (arc_cx.n > 0 && arc_mx.n > 0 && arc_gfx.n > 0) ? 0 : -2;
}

/*
 * a6xx_gmu_rpmh_arc_votes_init for one OPP: primary index = first level
 * >= the OPP's opp-level; secondary (mx.lvl) = first >= or else the highest
 * non-zero; vote = pri[idx] << 16 | sec_idx << 8 | pri_idx.
 */
uint32_t gpu_arc_vote(const struct arc *pri, unsigned level, int *ok)
{
	int pi = -1, si = 0;

	for (int j = 0; j < pri->n; j++)
		if (pri->lvl[j] >= level) {
			pi = j;
			break;
		}
	if (pi < 0) {
		*ok = 0;
		return 0;
	}
	for (int j = 0; j < arc_mx.n; j++) {
		if (arc_mx.lvl[j] >= level) {
			si = j;
			break;
		} else if (arc_mx.lvl[j]) {
			si = j;
		}
	}
	return ((uint32_t)pri->lvl[pi] << 16) | ((uint32_t)si << 8) | (uint32_t)pi;
}

/* GPU SMMU id registers (read-only; needs CX GDSC + cfg_ahb + memnoc on). */
void gpu_smmu_ids(void)
{
	char b[96], *p;

	p = put_hex(put(b, "idr0="), rd32(GPU_SMMU + 0x20), 8);
	p = put_hex(put(p, " idr1="), rd32(GPU_SMMU + 0x24), 8);
	p = put_hex(put(p, " scr0="), rd32(GPU_SMMU + 0x0), 8);
	put_hex(put(p, " gfsr="), rd32(GPU_SMMU + 0x48), 8);
	say("SMMU", b);
}

/* ---- the GPU RSC / PDC micro-code (a6xx_gmu_rpmh_init, a630 values) ---- */

void gpu_rpmh_ucode(void)
{
	static const uint32_t rsc_seq[5] = { 0xa7a506a0u, 0xa1e6a6e7u, 0xa2e081e1u, 0xe9a982e2u, 0x0020e8a8u };
	static const uint32_t pdc_seq[5] = { 0xfebea1e1u, 0xa5a4a3a2u, 0x8382a6e0u, 0xbce3e284u, 0x002081fcu };
	/* TCS1 (sleep) and TCS3 (wake): mx 0x30010, cx 0x30000, GPU_PDC_GFX */
	static const uint32_t tcs_addr[3] = { 0x30010, 0x30000, GPU_PDC_GFX };
	static const uint32_t tcs1_data[3] = { 1, 0, 0 };
	static const uint32_t tcs3_data[3] = { 2, 3, 3 };

	wr32(RSCC(0x004), 1u << 24);		/* RSC_STATUS0_DRV0: disable SDE clock gating */
	wr32(RSCC(0x00b), 1);			/* PDC_SLAVE_ID_DRV0 */
	wr32(RSCC(0x00e), 0);			/* HIDDEN_TCS_CMD0_DATA / ADDR, three commands */
	wr32(RSCC(0x00d), 0);
	wr32(RSCC(0x00e + 2), 0);
	wr32(RSCC(0x00d + 2), 0);
	wr32(RSCC(0x00e + 4), 0x80000000u);
	wr32(RSCC(0x00d + 4), 0);
	wr32(RSCC(0x100), 0);			/* OVERRIDE_START_ADDR */
	wr32(RSCC(0x008), 0x4520);		/* PDC_SEQ_START_ADDR */
	wr32(RSCC(0x009), 0x4510);		/* PDC_MATCH_VALUE_LO */
	wr32(RSCC(0x00a), 0x4514);		/* PDC_MATCH_VALUE_HI */
	for (unsigned i = 0; i < 5; i++)
		wr32(RSCC(0x180 + i), rsc_seq[i]);	/* SEQ_MEM_0_DRV0 */
	for (unsigned i = 0; i < 5; i++)
		wr32(PDCSEQ(i), pdc_seq[i]);		/* PDC_GPU_SEQ_MEM_0 */
	for (unsigned t = 0; t < 2; t++) {
		uint32_t ctl = t ? 0x15d6 : 0x1572;	/* TCS3 / TCS1 CONTROL */
		const uint32_t *d = t ? tcs3_data : tcs1_data;
		wr32(PDC(ctl + 1), 7);			/* CMD_ENABLE_BANK */
		wr32(PDC(ctl + 2), 0);			/* CMD_WAIT_FOR_CMPL_BANK */
		wr32(PDC(ctl), 0);
		for (unsigned c = 0; c < 3; c++) {
			wr32(PDC(ctl + 3 + 4 * c), 0x10108);	/* CMDn_MSGID */
			wr32(PDC(ctl + 4 + 4 * c), tcs_addr[c]);	/* CMDn_ADDR */
			wr32(PDC(ctl + 5 + 4 * c), d[c]);	/* CMDn_DATA */
		}
	}
	wr32(PDC(0x1148), 0);			/* PDC_GPU_SEQ_START_ADDR */
	wr32(PDC(0x1140), 0x80000001u);		/* PDC_GPU_ENABLE_PDC */
	dsb();
}

/* a6xx_rpmh_stop + a6xx_rpmh_start (only with GPU_RPMH=1). 1 = ok or skipped. */
int gpu_rpmh_start(void)
{
	char b[96], *p;

	if (!GPU_RPMH) {
		p = put_hex(put(b, "nao feito (GPU_RPMH=0) ack="), rd32(GMU(GMU_RSCC_CONTROL_ACK)), 8);
		put_hex(put(p, " busy="), rd32(RSCC(0x101)), 8);
		saylog("RSC GPU", b);
		return 1;
	}
	step("RSC GPU: stop (pede sleep)");
	wr(GMU(GMU_RSCC_CONTROL_REQ), 1);
	int s = poll(RSCC(0x004), 1u << 16, 1u << 16, 10);
	wr(GMU(GMU_RSCC_CONTROL_REQ), 0);
	step("RSC GPU: start (PDC vota wake)");
	wr(GMU(GMU_RSCC_CONTROL_REQ), 1u << 1);
	int a = poll(GMU(GMU_RSCC_CONTROL_ACK), 1u << 1, 1u << 1, 10);
	int idle = a && poll(RSCC(0x101), 0xffffffffu, 0, 10);
	wr(GMU(GMU_RSCC_CONTROL_REQ), 0);
	p = put(b, s ? "stop ok" : "stop sem ack");
	p = put(p, a ? ", start ack" : ", start SEM ACK");
	put(p, idle ? ", seq livre" : ", seq ocupado");
	say("RSC GPU", b);
	return a && idle;
}

/* ---- GMU firmware + boot (a6xx_gmu_fw_start, legacy a630) ---- */

void gpu_gmu_halt(void)
{
	wr(GMU(GMU_CM3_SYSRESET), 1);
}

/*
 * Loads a630_gmu.bin into ITCM with the CM3 held in reset, programs the boot
 * registers, releases it and waits for 0xBABEFACE. qtbl = GMU VA of the HFI
 * table (0 = none: M10), log_msg = PWR_COL_CP_MSG (log VA | pages-1, or 0).
 * 1 = the firmware runs.
 */
int gpu_gmu_boot(uint32_t qtbl, uint32_t log_msg, uint32_t msgq_mask)
{
#ifndef HAVE_GMU_FW
	(void)qtbl;
	(void)log_msg;
	(void)msgq_mask;
	say("FW", "sem a630_gmu.bin embutido: rode op6/tools/gpu-fw-embed.py e recompile");
	return 0;
#else
	char b[96], *p;
	unsigned words = gpu_fw_words(), wrong = 0;
	uint32_t result = 0, dtcm_ver, mask = 0xffffffffu, want = GMU_BABEFACE;

	step("GMU: CM3 em reset + retencao");
	wr(GMU(GMU_CM3_SYSRESET), 1);
	wr(GMU(GMU_GENERAL_7), 1);		/* register retention (cold boot) */
	step("GMU: micro-codigo RSC/PDC da GPU");
	gpu_rpmh_ucode();
	if (!gpu_rpmh_start())
		return 0;

	step("GMU: firmware no ITCM (AHB)");
	for (unsigned i = 0; i < words; i++)
		wr32(GMU(GMU_CM3_ITCM_START + i), a630_gmu_fw[i]);
	dsb();
	for (unsigned i = 0; i < words; i++)
		if (rd32(GMU(GMU_CM3_ITCM_START + i)) != a630_gmu_fw[i])
			wrong++;
	p = put_dec(put(b, "palavras="), words);
	p = put_dec(put(p, " erradas="), wrong);
	put_hex(put(p, " w0="), a630_gmu_fw[0], 8);
	say("FW", b);
	if (wrong) {
		say("FW", "o ITCM nao guarda o firmware: parando");
		return 0;
	}

	step("GMU: registradores de boot");
	wr32(GMU(GMU_CM3_FW_INIT_RESULT), 0);
	wr32(GMU(GMU_CM3_BOOT_CONFIG), 2);	/* TCM boot */
	wr32(GMU(GMU_HFI_QTBL_ADDR), qtbl);
	wr32(GMU(GMU_HFI_QTBL_INFO), qtbl ? 1 : 0);
	wr32(GMU(GMU_AHB_FENCE_RANGE_0), FENCE_RANGE);
	wr32(GMU(GMU_CM3_CFG), 0x4052);		/* no NMI entry instead of __main */
	wr32(GMU(GMU_HFI_SFR_ADDR), GMU_CHIPID);
	wr32(GMU(GMU_PWR_COL_CP_MSG), log_msg);
	/* a6xx_gmu_power_config, minus the RPMh enable: the GMU votes nothing */
	wr32(GMU(GMU_SYS_BUS_CONFIG), 1);
	wr32(GMU(GMU_ICACHE_CONFIG), 1);
	wr32(GMU(GMU_DCACHE_CONFIG), 1);
	wr32(GMU(GMU_PWR_COL_INTER_FRAME_CTRL), 0x9c40400u);
	wr32(GMU(GMU_RPMH_CTRL), 0);
	wr32(GMU(GMU_CX_GMU_WFI_CONFIG), 0);
	wr32(GMU(GMU_GMU2HOST_INTR_CLR), 0xffffffffu);
	wr32(GMU(GMU_GMU2HOST_INTR_MASK), msgq_mask);
	dsb();

	/* a6xx_gmu_start: newer firmware answers 0x100 in the low 9 bits */
	dtcm_ver = rd32(GMU(GMU_CM3_DTCM_START + 0xff8));
	if (dtcm_ver > 0x20010004u) {
		mask = 0x1ff;
		want = 0x100;
	}
	step("GMU: solta o CM3, espera 0xBABEFACE");
	wr(GMU(GMU_CM3_SYSRESET), 1);
	wr(GMU(GMU_PWR_COL_CP_RESP), 0);
	wr(GMU(GMU_CM3_SYSRESET), 0);
	for (int i = 0; i < 1000; i++) {
		result = rd32(GMU(GMU_CM3_FW_INIT_RESULT));
		if ((result & mask) == want)
			break;
		delay_ms(1);
		if ((i & 255) == 255)
			wdog_pet();
	}
	p = put_hex(put(b, "init="), result, 8);
	p = put_hex(put(p, " dtcm[ff8]="), dtcm_ver, 8);
	put_hex(put(p, " pwr="), rd32(GMU(GMU_RPMH_POWER_STATE)), 2);
	say("GMU", b);
	return (result & mask) == want;
#endif
}
