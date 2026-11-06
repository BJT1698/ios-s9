/*
 * Galaxy S9 milestone 10: wake the Adreno 630's GMU, step by step, up to the
 * point where its own firmware runs (0xBABEFACE). This is the FOUNDATION of
 * GPU acceleration: before the 3D core can draw, its power-management
 * processor (the "GMU", a Cortex-M3 inside the GPU block) has to be powered,
 * clocked and booted. Everything the 3D core, the secure "zap" shader and the
 * command processor need (stages 4-8, a later milestone) sits on top of this.
 *
 * Why in the shim and not in the iOS kernel: any access to a gated GPU block
 * hangs the bus with no panic, and a wrong GMU value resets the SoC silently.
 * The shim boots in ~10 s, shows each step before it touches anything, arms
 * the APSS watchdog (a full hang reboots by itself) and MMU is off (a secure
 * region faults into the on-screen panic). The iOS kernel would cost a 7-min
 * boot and a trip to TWRP per try. (Memory: test risky hardware in the shim.)
 *
 * Stages (adreno=N would cap it; here a compile-time MAX, run 1..MAX, stop at
 * the first failure):
 *   1  read-only: the GPU's clocks in GCC, the gpucc GDSCs and the GMU RCG, as
 *      the boot loader left them. Nothing written.
 *   2  power the CX half (GDSC + the GMU's clocks at 200 MHz), read the GMU's
 *      registers, and write/read-back two words of its data memory with its
 *      processor held in reset: proves the GMU is powered and reachable.
 *   3  load the GMU's firmware (fw/a630_gmu.bin) into its code memory through
 *      the AHB registers, program the RSC/PDC power sequencer micro-code, and
 *      let the processor out of reset: wait for FW_INIT_RESULT == 0xBABEFACE.
 *
 * The GX rail, the GPU SMMU, HFI, the zap shader and the CP are NOT here: those
 * are where the SoC can reset, and they come next, once this foundation holds.
 *
 * Every register number and sequence is from the Galaxy S9's own Android kernel
 * (LineageOS 20, drivers/gpu/msm/{adreno_a6xx.c,kgsl_gmu.c,a6xx_reg.h},
 * drivers/clk/qcom/{gpucc,gcc}-sdm845.c, gdsc.c, clk-rcg2.c) and checked
 * against the live device tree (dtb/live-twrp.dts): GPU 0x5000000 (chipid
 * 0x06030001 = A630 v2), GMU 0x506a000, gpucc 0x5090000, GCC 0x100000, PDC
 * 0xb200000. No code is copied; the numbers are facts.
 */
#include <stdint.h>
#include "hw.h"
#include "fb.h"
#include "con.h"
#include "plat.h"

#if defined(__has_include)
#if __has_include("fw_a630_gmu.h")
#include "fw_a630_gmu.h"	/* tools/gpu-fw-embed.py: static const uint32_t a630_gmu_fw[]; HAVE_GMU_FW 1 */
#endif
#endif

#define END_SECONDS		60
#define ADRENO_MAX_STAGE	3	/* this milestone: through "the GMU's firmware runs" */

/* ---- register blocks (all absolute physical addresses; MMU off) ------------------------------------ */

#define GPUCC			0x05090000UL
#define GCC			0x00100000UL
#define GMU_BASE		0x0506a000UL
#define PDC_BASE		0x0b200000UL

/* The GMU and the blocks around it are read through one window at GMU_BASE
 * using the GPU-space register numbers of a6xx_reg.h: byte = base + (n - 0x1A800)*4.
 * Checked: A6XX_GPU_CC_GX_GDSCR 0x24403 -> 0x509100c, the DT's gpu_gx_gdsc. */
#define GMU(n)			(GMU_BASE + (((uint32_t)(n) - 0x1A800u) << 2))
/* PDC registers: pdc_reg_virt + (offsetword << 2), as the kernel's _regwrite. */
#define PDC(w)			(PDC_BASE + ((uint32_t)(w) << 2))

/* gpucc */
#define GPUCC_CX_GDSCR		0x106c		/* gpu_cx_gdsc */
#define GPUCC_CX_HWCTRL		0x1540		/* syscon@0x5091540: CX power-on status */
#define GPUCC_GX_GDSCR		0x100c		/* gpu_gx_gdsc (NOT touched in stages 1-3) */
#define GPUCC_GMU_RCG_CMD	0x1120		/* CMD_RCGR */
#define GPUCC_GMU_RCG_CFG	0x1124		/* CFG_RCGR */
#define GPUCC_CBCR_AHB		0x1078		/* gpu_cc_ahb_clk */
#define GPUCC_CBCR_CX_GMU	0x1098		/* gpu_cc_cx_gmu_clk */
#define GPUCC_CBCR_CXO		0x109c		/* gpu_cc_cxo_clk */

/* GCC (branches that feed the GMU) */
#define GCC_GPLL0_VOTE		0x52004		/* bit 15 gpll0, bit 16 gpll0_div */
#define GCC_CFG_AHB		0x71004		/* gcc_gpu_cfg_ahb_clk */
#define GCC_MEMNOC_GFX		0x7100c		/* gcc_gpu_memnoc_gfx_clk */
#define GCC_SNOC_DVM_GFX	0x71018		/* gcc_gpu_snoc_dvm_gfx_clk */
#define GCC_DDRSS_GPU_AXI	0x44038		/* gcc_ddrss_gpu_axi_clk */

/* gdsc.c / clk-rcg2.c bits */
#define SW_COLLAPSE		(1u << 0)
#define PWR_ON			(1u << 31)
#define CBCR_ENABLE		(1u << 0)
#define CBCR_OFF		(1u << 31)	/* CLK_OFF: 1 = gated */
#define CMD_UPDATE		(1u << 0)

/* GMU registers (a6xx_reg.h numbers) */
#define GMU_CM3_SYSRESET	0x1F800
#define GMU_CM3_BOOT_CONFIG	0x1F801
#define GMU_CX_GMU_WFI_CONFIG	0x1F802
#define GMU_CM3_FW_INIT_RESULT	0x1F81C
#define GMU_CM3_ITCM_START	0x1B400
#define GMU_CM3_DTCM_START	0x1C400
#define GMU_SYS_BUS_CONFIG	0x1F40F
#define GMU_PWR_COL_FRAME_CTRL	0x1F8C0
#define GMU_RPMH_CTRL		0x1F8E8
#define GMU_RPMH_POWER_STATE	0x1F8EC
#define GMU_HFI_SFR_ADDR	0x1F982
#define GMU_HFI_QTBL_ADDR	0x1F985
#define GMU_HFI_QTBL_INFO	0x1F984
#define GMU_GMU2HOST_INTR_CLR	0x1F991
#define GMU_GENERAL_7		0x1F9CC
#define GMU_AHB_FENCE_RANGE_0	0x23B11
#define GMU_AO_AHB_FENCE_CTRL	0x23B10

/* RSC power sequencer (in the GMU window) */
#define GPU_RSCC_RSC_STATUS0	0x23404
#define RSCC_PDC_SEQ_START_ADDR	0x23408
#define RSCC_PDC_MATCH_VALUE_LO	0x23409
#define RSCC_PDC_MATCH_VALUE_HI	0x2340A
#define RSCC_PDC_SLAVE_ID_DRV0	0x2340B
#define RSCC_HIDDEN_TCS_CMD0_ADDR 0x2340D
#define RSCC_HIDDEN_TCS_CMD0_DATA 0x2340E
#define RSCC_OVERRIDE_START_ADDR 0x23500
#define RSCC_SEQ_MEM_0_DRV0	0x23580
#define RSC_CMD_OFFSET		2

/* PDC (byte window, offsetword << 2) */
#define PDC_GPU_ENABLE_PDC	0x21140
#define PDC_GPU_SEQ_START_ADDR	0x21148
#define PDC_GPU_SEQ_MEM_0	0xA0000
#define PDC_GPU_TCS1_CONTROL	0x21572
#define PDC_GPU_TCS1_CMD_ENABLE_BANK 0x21573
#define PDC_GPU_TCS1_CMD_WAIT_FOR_CMPL_BANK 0x21574
#define PDC_GPU_TCS1_CMD0_MSGID	0x21575
#define PDC_GPU_TCS1_CMD0_ADDR	0x21576
#define PDC_GPU_TCS1_CMD0_DATA	0x21577
#define PDC_GPU_TCS3_CONTROL	0x215D6
#define PDC_GPU_TCS3_CMD_ENABLE_BANK 0x215D7
#define PDC_GPU_TCS3_CMD_WAIT_FOR_CMPL_BANK 0x215D8
#define PDC_GPU_TCS3_CMD0_MSGID	0x215D9
#define PDC_GPU_TCS3_CMD0_ADDR	0x215DA
#define PDC_GPU_TCS3_CMD0_DATA	0x215DB
#define PDC_CMD_OFFSET		4

#define GMU_RCG_CFG_200MHZ	0x602		/* src 6 (GPLL0_OUT_MAIN_DIV), div 1.5 (field 2) */
#define GMU_CHIPID		0x06030100	/* a6xx_gmu_fw_start: (chipid&0xffff0000)|(minor<<12)|(patch<<8) */
#define GMU_BOOT_CONFIG_TCM	2		/* enum gmu_load_mode TCM_BOOT */
#define FENCE_RANGE_MASK	((1u << 31) | ((0xAu << 2) << 18) | 0x8A0u)
#define GMU_BABEFACE		0xBABEFACEu

static const char *stage;	/* set at run time: no absolute pointers in data */
static int step_row;

void exc_handler(uint64_t slot, uint64_t *regs)
{
	panic_screen(stage, slot, regs);
}

/* Overwrites one line: a hang leaves the last step in the photo. */
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
	wdog_pet();
}

static void showhex(const char *label, uint64_t v)
{
	con_hex(label, v);
	wdog_pet();
}

/* A clock branch: set enable, wait until CLK_OFF (bit 31) clears. */
static int clk_branch_on(uintptr_t cbcr, const char *name)
{
	char b[64], *p;

	wr32(cbcr, rd32(cbcr) | CBCR_ENABLE);
	dsb();
	for (int i = 0; i < 2000; i++) {
		if (!(rd32(cbcr) & CBCR_OFF))
			break;
		delay_ms(1);
	}
	p = put(put(b, name), " cbcr=");
	put_hex(p, rd32(cbcr), 8);
	con_str("clk", b);
	return !(rd32(cbcr) & CBCR_OFF);
}

/* ---- stage 1: read-only ----------------------------------------------------------------------------- */

static int cfg_ahb_on(void)
{
	/* The gpucc half cannot be read until its AHB clock runs; ABL leaves the
	 * GPU off on this family, so expect it off. */
	return !(rd32(GCC + GCC_CFG_AHB) & CBCR_OFF);
}

static void stage1(void)
{
	stage = "1 leitura";
	step("1 le GCC/gpucc (nada escrito)");
	showhex("gcc gpll0-vote", rd32(GCC + GCC_GPLL0_VOTE));
	showhex("gcc cfg_ahb", rd32(GCC + GCC_CFG_AHB));
	showhex("gcc memnoc_gfx", rd32(GCC + GCC_MEMNOC_GFX));
	showhex("gcc ddrss_axi", rd32(GCC + GCC_DDRSS_GPU_AXI));
	if (cfg_ahb_on()) {
		showhex("gpucc cx gdscr", rd32(GPUCC + GPUCC_CX_GDSCR));
		showhex("gpucc gx gdscr", rd32(GPUCC + GPUCC_GX_GDSCR));
		showhex("gpucc gmu rcg cfg", rd32(GPUCC + GPUCC_GMU_RCG_CFG));
		con_str("1", "gpucc ja estava acessivel (ABL deixou a GPU ligada?)");
	} else {
		con_str("1", "cfg_ahb desligado: gpucc nao lido ate a etapa 2 (esperado)");
	}
}

/* ---- stage 2: power the CX half, read the GMU, test its memory --------------------------------------- */

static int cx_power_on(void)
{
	uint32_t v = rd32(GPUCC + GPUCC_CX_GDSCR);

	if (!(v & SW_COLLAPSE)) {
		con_str("cx", "CX GDSC ja estava ligado");
	} else {
		wr32(GPUCC + GPUCC_CX_GDSCR, v & ~SW_COLLAPSE);
		dsb();
	}
	for (int i = 0; i < 2000; i++) {
		if ((rd32(GPUCC + GPUCC_CX_GDSCR) & PWR_ON) ||
		    (rd32(GPUCC + GPUCC_CX_HWCTRL) & PWR_ON))
			break;
		delay_ms(1);
	}
	return (rd32(GPUCC + GPUCC_CX_GDSCR) & PWR_ON) ||
	       (rd32(GPUCC + GPUCC_CX_HWCTRL) & PWR_ON);
}

/* GMU clock source RCG to 200 MHz (GPLL0_OUT_MAIN_DIV / 1.5). */
static void gmu_rcg_200mhz(void)
{
	wr32(GPUCC + GPUCC_GMU_RCG_CFG, GMU_RCG_CFG_200MHZ);
	dsb();
	wr32(GPUCC + GPUCC_GMU_RCG_CMD, rd32(GPUCC + GPUCC_GMU_RCG_CMD) | CMD_UPDATE);
	dsb();
	for (int i = 0; i < 2000; i++) {
		if (!(rd32(GPUCC + GPUCC_GMU_RCG_CMD) & CMD_UPDATE))
			break;
		delay_ms(1);
	}
}

static int cx_and_clocks(void)
{
	stage = "2 energia";
	step("2a GPLL0 vote + CX GDSC");
	/* Vote the shared GPLL0 and its /2 output for the GMU clock source. */
	wr32(GCC + GCC_GPLL0_VOTE, rd32(GCC + GCC_GPLL0_VOTE) | (1u << 15) | (1u << 16));
	dsb();
	if (!clk_branch_on(GCC + GCC_CFG_AHB, "gcc cfg_ahb"))
		return 0;
	if (!cx_power_on()) {
		con_str("2", "CX GDSC NAO liga: parando");
		return 0;
	}
	showhex("gpucc cx gdscr", rd32(GPUCC + GPUCC_CX_GDSCR));

	step("2b relogio do GMU a 200 MHz");
	gmu_rcg_200mhz();
	showhex("gpucc gmu rcg cfg", rd32(GPUCC + GPUCC_GMU_RCG_CFG));
	clk_branch_on(GPUCC + GPUCC_CBCR_CXO, "cxo");
	clk_branch_on(GPUCC + GPUCC_CBCR_AHB, "gpu ahb");
	if (!clk_branch_on(GPUCC + GPUCC_CBCR_CX_GMU, "cx_gmu"))
		return 0;
	clk_branch_on(GCC + GCC_MEMNOC_GFX, "gcc memnoc");
	clk_branch_on(GCC + GCC_DDRSS_GPU_AXI, "gcc ddrss_axi");
	clk_branch_on(GCC + GCC_SNOC_DVM_GFX, "gcc snoc_dvm");
	return 1;
}

static void stage2(void)
{
	static const uint32_t pat[2] = { 0x600d0001u, 0x0a110002u };
	uint32_t was[2], back[2];

	if (!cx_and_clocks())
		return;

	step("2c le os registradores do GMU");
	showhex("gmu fw_init_result", rd32(GMU(GMU_CM3_FW_INIT_RESULT)));
	showhex("gmu rpmh_power_state", rd32(GMU(GMU_RPMH_POWER_STATE)));

	step("2d escreve/le a memoria do GMU (em reset)");
	if (rd32(GMU(GMU_CM3_FW_INIT_RESULT)) == GMU_BABEFACE) {
		con_str("2", "o firmware do GMU ja roda: nao mexo na memoria");
		return;
	}
	wr32(GMU(GMU_CM3_SYSRESET), 1);		/* hold the CM3 in reset */
	dsb();
	for (int i = 0; i < 2; i++)
		was[i] = rd32(GMU(GMU_CM3_DTCM_START) + 4 * i);
	for (int i = 0; i < 2; i++)
		wr32(GMU(GMU_CM3_DTCM_START) + 4 * i, pat[i]);
	dsb();
	for (int i = 0; i < 2; i++)
		back[i] = rd32(GMU(GMU_CM3_DTCM_START) + 4 * i);
	for (int i = 0; i < 2; i++)
		wr32(GMU(GMU_CM3_DTCM_START) + 4 * i, was[i]);
	dsb();
	showhex("gmu dtcm back0", back[0]);
	showhex("gmu dtcm back1", back[1]);
	con_str("2", back[0] == pat[0] && back[1] == pat[1] ?
	    "A MEMORIA DO GMU ACEITA O QUE SE ESCREVE: pronto p/ firmware" :
	    "a memoria do GMU nao guarda o que foi escrito");
}

/* ---- stage 3: the RSC/PDC power sequencer and the GMU firmware --------------------------------------- */

#ifdef HAVE_GMU_FW
/* adreno_a6xx.c _load_gmu_rpmh_ucode (SDM845 values). RSC micro-code in the GMU
 * window, PDC micro-code and TCS command set in the PDC window. */
static void load_rpmh_ucode(void)
{
	static const uint32_t rsc_seq[5] = {
		0xA7A506A0u, 0xA1E6A6E7u, 0xA2E081E1u, 0xE9A982E2u, 0x0020E8A8u
	};
	static const uint32_t pdc_seq[5] = {
		0xFEBEA1E1u, 0xA5A4A3A2u, 0x8382A6E0u, 0xBCE3E284u, 0x002081FCu
	};

	/* Disable SDE clock gating; RSC<->PDC handshake for sleep/wake. */
	wr32(GMU(GPU_RSCC_RSC_STATUS0), 1u << 24);
	wr32(GMU(RSCC_PDC_SLAVE_ID_DRV0), 1);
	wr32(GMU(RSCC_HIDDEN_TCS_CMD0_DATA), 0);
	wr32(GMU(RSCC_HIDDEN_TCS_CMD0_ADDR), 0);
	wr32(GMU(RSCC_HIDDEN_TCS_CMD0_DATA + RSC_CMD_OFFSET), 0);
	wr32(GMU(RSCC_HIDDEN_TCS_CMD0_ADDR + RSC_CMD_OFFSET), 0);
	wr32(GMU(RSCC_HIDDEN_TCS_CMD0_DATA + RSC_CMD_OFFSET * 2), 0x80000000u);
	wr32(GMU(RSCC_HIDDEN_TCS_CMD0_ADDR + RSC_CMD_OFFSET * 2), 0);
	wr32(GMU(RSCC_OVERRIDE_START_ADDR), 0);
	wr32(GMU(RSCC_PDC_SEQ_START_ADDR), 0x4520);
	wr32(GMU(RSCC_PDC_MATCH_VALUE_LO), 0x4510);
	wr32(GMU(RSCC_PDC_MATCH_VALUE_HI), 0x4514);
	/* a630 v2: no RSCC_TIMESTAMP_UNIT1_EN_DRV0 (that is v1 only). */
	for (unsigned i = 0; i < 5; i++)
		wr32(GMU(RSCC_SEQ_MEM_0_DRV0) + 4 * i, rsc_seq[i]);

	for (unsigned i = 0; i < 5; i++)
		wr32(PDC(PDC_GPU_SEQ_MEM_0) + 4 * i, pdc_seq[i]);

	/* TCS commands the PDC sequence uses for low-power modes (addresses from
	 * the SDM845 kernel: 0x30010/0x30000/0x30080). */
	wr32(PDC(PDC_GPU_TCS1_CMD_ENABLE_BANK), 7);
	wr32(PDC(PDC_GPU_TCS1_CMD_WAIT_FOR_CMPL_BANK), 0);
	wr32(PDC(PDC_GPU_TCS1_CONTROL), 0);
	wr32(PDC(PDC_GPU_TCS1_CMD0_MSGID), 0x10108);
	wr32(PDC(PDC_GPU_TCS1_CMD0_ADDR), 0x30010);
	wr32(PDC(PDC_GPU_TCS1_CMD0_DATA), 1);
	wr32(PDC(PDC_GPU_TCS1_CMD0_MSGID + PDC_CMD_OFFSET), 0x10108);
	wr32(PDC(PDC_GPU_TCS1_CMD0_ADDR + PDC_CMD_OFFSET), 0x30000);
	wr32(PDC(PDC_GPU_TCS1_CMD0_DATA + PDC_CMD_OFFSET), 0x0);
	wr32(PDC(PDC_GPU_TCS1_CMD0_MSGID + PDC_CMD_OFFSET * 2), 0x10108);
	wr32(PDC(PDC_GPU_TCS1_CMD0_ADDR + PDC_CMD_OFFSET * 2), 0x30080);
	wr32(PDC(PDC_GPU_TCS1_CMD0_DATA + PDC_CMD_OFFSET * 2), 0x0);
	wr32(PDC(PDC_GPU_TCS3_CMD_ENABLE_BANK), 7);
	wr32(PDC(PDC_GPU_TCS3_CMD_WAIT_FOR_CMPL_BANK), 0);
	wr32(PDC(PDC_GPU_TCS3_CONTROL), 0);
	wr32(PDC(PDC_GPU_TCS3_CMD0_MSGID), 0x10108);
	wr32(PDC(PDC_GPU_TCS3_CMD0_ADDR), 0x30010);
	wr32(PDC(PDC_GPU_TCS3_CMD0_DATA), 2);
	wr32(PDC(PDC_GPU_TCS3_CMD0_MSGID + PDC_CMD_OFFSET), 0x10108);
	wr32(PDC(PDC_GPU_TCS3_CMD0_ADDR + PDC_CMD_OFFSET), 0x30000);
	wr32(PDC(PDC_GPU_TCS3_CMD0_DATA + PDC_CMD_OFFSET), 0x3);
	wr32(PDC(PDC_GPU_TCS3_CMD0_MSGID + PDC_CMD_OFFSET * 2), 0x10108);
	wr32(PDC(PDC_GPU_TCS3_CMD0_ADDR + PDC_CMD_OFFSET * 2), 0x30080);
	wr32(PDC(PDC_GPU_TCS3_CMD0_DATA + PDC_CMD_OFFSET * 2), 0x3);

	wr32(PDC(PDC_GPU_SEQ_START_ADDR), 0);
	wr32(PDC(PDC_GPU_ENABLE_PDC), 0x80000001u);
	dsb();
}
#endif /* HAVE_GMU_FW */

static void stage3(void)
{
#ifndef HAVE_GMU_FW
	stage = "3 firmware";
	step("3 firmware do GMU");
	con_str("3", "sem fw/a630_gmu.bin embutido: rode tools/gpu-fw-embed.py e recompile");
	return;
#else
	uint32_t result = 0, wrong = 0;
	unsigned words = sizeof(a630_gmu_fw) / sizeof(a630_gmu_fw[0]);

	stage = "3 firmware";
	step("3a sequenciador RSC/PDC");
	if (rd32(GMU(GMU_CM3_FW_INIT_RESULT)) == GMU_BABEFACE) {
		con_str("3", "o firmware do GMU ja roda");
		return;
	}
	load_rpmh_ucode();

	step("3b carrega o firmware no ITCM");
	wr32(GMU(GMU_CM3_SYSRESET), 1);		/* hold the CM3 in reset while loading */
	dsb();
	wr32(GMU(GMU_GENERAL_7), 1);		/* TCM retention (cold boot) */
	for (unsigned i = 0; i < words; i++)
		wr32(GMU(GMU_CM3_ITCM_START) + 4 * i, a630_gmu_fw[i]);
	dsb();
	for (unsigned i = 0; i < words; i++)
		if (rd32(GMU(GMU_CM3_ITCM_START) + 4 * i) != a630_gmu_fw[i])
			wrong++;
	showhex("gmu fw words", words);
	showhex("gmu fw words errados", wrong);
	if (wrong) {
		con_str("3", "o ITCM nao guarda o firmware: parando");
		return;
	}

	step("3c configura e solta o processador do GMU");
	wr32(GMU(GMU_CM3_FW_INIT_RESULT), 0);
	wr32(GMU(GMU_CM3_BOOT_CONFIG), GMU_BOOT_CONFIG_TCM);
	wr32(GMU(GMU_HFI_QTBL_ADDR), 0);	/* no HFI memory yet (stage 4); the boot does not need it */
	wr32(GMU(GMU_HFI_QTBL_INFO), 0);
	wr32(GMU(GMU_AHB_FENCE_RANGE_0), FENCE_RANGE_MASK);
	wr32(GMU(GMU_HFI_SFR_ADDR), GMU_CHIPID);
	/* power config (a6xx_gmu_power_config, minimum) */
	wr32(GMU(GMU_SYS_BUS_CONFIG), 1);
	wr32(GMU(GMU_PWR_COL_FRAME_CTRL), 0x9C40400u);
	wr32(GMU(GMU_RPMH_CTRL), 0);		/* RPMh interface left off for this boot (GX comes later) */
	wr32(GMU(GMU_GMU2HOST_INTR_CLR), 0xFFFFFFFFu);
	wr32(GMU(GMU_CX_GMU_WFI_CONFIG), 0);
	dsb();

	wr32(GMU(GMU_CM3_SYSRESET), 0);		/* let the processor run */
	dsb();
	for (int i = 0; i < 1000; i++) {
		result = rd32(GMU(GMU_CM3_FW_INIT_RESULT));
		if (result == GMU_BABEFACE)
			break;
		wdog_pet();
		delay_ms(1);
	}
	showhex("gmu fw_init_result", result);
	con_str("3", result == GMU_BABEFACE ?
	    "O FIRMWARE DO GMU RODA (0xBABEFACE): fundacao da GPU pronta" :
	    "o firmware do GMU nao subiu (ver fw_init_result acima)");
#endif
}

void shim_main(uint64_t dtb)
{
	(void)dtb;
	fb_default();
	con_clear(GREY);
	con_title("SHIM M10: GMU DA ADRENO 630");
	stage = "watchdog";
	wdog_arm();
	step_row = con_row();
	con_skip();

	stage1();
	con_skip();
	if (ADRENO_MAX_STAGE >= 2)
		stage2();
	if (ADRENO_MAX_STAGE >= 3) {
		con_skip();
		stage3();
	}

	stage = "fim";
	step("fim");
	con_skip();
	con_str("", "Tire uma foto desta tela!");
	countdown_reboot(con_row(), END_SECONDS);
}
