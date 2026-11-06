/*
 * Galaxy S9 milestone 11 (GPU stage B): give the GMU its memory through the
 * GPU's own SMMU and get it talking to us over its message channel (HFI).
 *
 * M10 proved the GMU's firmware boots (0xBABEFACE) with no SMMU: the boot does
 * not reach RAM. HFI does: the GMU reads and writes a table of message queues
 * in memory of ours, which it reaches by the addresses 0x60000000.. through the
 * GPU's SMMU (a memory translator, like a CPU's MMU but for the GPU block). So
 * the SMMU must be programmed and the queue table told to the GMU (HFI_QTBL_ADDR)
 * BEFORE the processor is let go -- this stage therefore redoes M10's power +
 * firmware + boot, now with the SMMU in place, and then sends the GMU two
 * messages (INIT, FW_VER) and reads back its firmware version.
 *
 * The one S9-specific unknown is the hypervisor. On the Poco F1 (no Samsung RKP)
 * both reference projects programmed this SMMU directly. The S9's hypervisor may
 * emulate or refuse its global registers (stream table, context base). So stage
 * 3 READS the SMMU first (layout, and what the boot loader/hyp left) and stage 4
 * WRITES it and reads every write back -- if the hyp blocks it, the photo shows
 * exactly which register did not take, and HFI then faults (logged), never a brick.
 *
 * The GX rail (the 3D core's power) and the CP are NOT here: those are stage C
 * (M12), isolated because powering GX is the step that can reset the SoC.
 *
 * MMU off: all memory is Device, so our page tables and the GMU's queue memory
 * are static buffers whose physical address equals their address. Register
 * numbers and sequences from the S9's Android kernel (drivers/gpu/msm) and the
 * live device tree; the SMMU programming follows the smmu-v2 order of Linux
 * arm-smmu.c / io-pgtable-arm.c (facts, no code copied).
 */
#include <stdint.h>
#include "hw.h"
#include "fb.h"
#include "con.h"
#include "plat.h"

#if defined(__has_include)
#if __has_include("fw_a630_gmu.h")
#include "fw_a630_gmu.h"
#endif
#endif

#define END_SECONDS		60

/* ---- register blocks (absolute physical addresses; MMU off) ---------------------------------------- */

#define GPUCC			0x05090000UL
#define GCC			0x00100000UL
#define GMU_BASE		0x0506a000UL
#define PDC_BASE		0x0b200000UL
#define SMMU_BASE		0x05040000UL
#define SMMU_SIZE		0x10000UL

#define GMU(n)			(GMU_BASE + (((uint32_t)(n) - 0x1A800u) << 2))
#define PDC(w)			(PDC_BASE + ((uint32_t)(w) << 2))

/* gpucc / GCC (as M10) */
#define GPUCC_CX_GDSCR		0x106c
#define GPUCC_CX_HWCTRL		0x1540
#define GPUCC_GX_GDSCR		0x100c
#define GPUCC_GMU_RCG_CMD	0x1120
#define GPUCC_GMU_RCG_CFG	0x1124
#define GPUCC_CBCR_AHB		0x1078
#define GPUCC_CBCR_CX_GMU	0x1098
#define GPUCC_CBCR_CXO		0x109c
#define GCC_GPLL0_VOTE		0x52004
#define GCC_CFG_AHB		0x71004
#define GCC_MEMNOC_GFX		0x7100c
#define GCC_SNOC_DVM_GFX	0x71018
#define GCC_DDRSS_GPU_AXI	0x44038

#define SW_COLLAPSE		(1u << 0)
#define PWR_ON			(1u << 31)
#define CBCR_ENABLE		(1u << 0)
#define CBCR_OFF		(1u << 31)
#define CMD_UPDATE		(1u << 0)

/* GMU registers (as M10, plus HFI) */
#define GMU_CM3_SYSRESET	0x1F800
#define GMU_CM3_BOOT_CONFIG	0x1F801
#define GMU_CX_GMU_WFI_CONFIG	0x1F802
#define GMU_CM3_FW_INIT_RESULT	0x1F81C
#define GMU_CM3_ITCM_START	0x1B400
#define GMU_SYS_BUS_CONFIG	0x1F40F
#define GMU_PWR_COL_FRAME_CTRL	0x1F8C0
#define GMU_RPMH_CTRL		0x1F8E8
#define GMU_RPMH_POWER_STATE	0x1F8EC
#define GMU_HFI_SFR_ADDR	0x1F982
#define GMU_HFI_MMAP_ADDR	0x1F983
#define GMU_HFI_QTBL_INFO	0x1F984
#define GMU_HFI_QTBL_ADDR	0x1F985
#define GMU_HFI_CTRL_INIT	0x1F986
#define GMU_HFI_CTRL_STATUS	0x1F980
#define GMU_HFI_VERSION_INFO	0x1F981
#define GMU_GMU2HOST_INTR_INFO	0x1F992
#define GMU_GMU2HOST_INTR_CLR	0x1F991
#define GMU_GMU2HOST_INTR_MASK	0x1F993
#define GMU_HOST2GMU_INTR_SET	0x1F994
#define GMU_GENERAL_7		0x1F9CC
#define GMU_AHB_FENCE_RANGE_0	0x23B11
#define GMU_AO_AHB_FENCE_CTRL	0x23B10
#define GMU_AO_HOST_INTR_STATUS	0x23B05

/* RSC/PDC sequencer (as M10) */
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

#define GMU_RCG_CFG_200MHZ	0x602
#define GMU_CHIPID		0x06030100
#define GMU_BOOT_CONFIG_TCM	2
#define FENCE_RANGE_MASK	((1u << 31) | ((0xAu << 2) << 18) | 0x8A0u)
#define GMU_BABEFACE		0xBABEFACEu

/* ---- GPU SMMU (arm,smmu-kgsl@5040000, qcom,smmu-v2) ------------------------------------------------- */

#define SMMU_SCR0		0x000
#define SMMU_IDR0		0x020		/* [7:0] stream match registers */
#define SMMU_IDR1		0x024		/* bit31 64K pages, [30:28] log2(pages)-1, [7:0] contexts */
#define SMMU_IDR2		0x028
#define SMMU_SGFSR		0x048
#define SMMU_SMR(n)		(0x800 + 4 * (n))	/* bit31 valid, [30:16] mask, [14:0] stream */
#define SMMU_S2CR(n)		(0xc00 + 4 * (n))	/* [17:16] 0 translate 1 bypass 2 fault, [7:0] context */
#define SMMU_CBAR(n)		(4 * (n))		/* second page */
#define SMMU_CBA2R(n)		(0x800 + 4 * (n))	/* second page */
#define SMMU_CB_SCTLR		0x00
#define SMMU_CB_TCR2		0x10
#define SMMU_CB_TTBR0		0x20
#define SMMU_CB_TTBR1		0x28
#define SMMU_CB_TCR		0x30
#define SMMU_CB_MAIR0		0x38
#define SMMU_CB_MAIR1		0x3c
#define SMMU_CB_FSR		0x58
#define SMMU_CB_FAR		0x60
#define SMMU_CB_FSYNR0		0x68
#define SMMU_CB_TLBIALL		0x618
#define SMMU_CB_TLBSYNC		0x7f0
#define SMMU_CB_TLBSTATUS	0x7f4

#define SMMU_SMR_VALID		(1u << 31)
#define SMMU_S2CR_FAULT		(2u << 16)
#define SMMU_CBAR_STAGE_1	((1u << 16) | (3u << 8) | (0xfu << 12))	/* type S1, weakest defaults */
#define SMMU_CBA2R_64BIT	1u
#define SMMU_CB_SCTLR_ON	((1u << 0) | (1u << 1) | (1u << 2) | (1u << 5) | (1u << 12)) /* on, faults recorded, no irq */
#define SMMU_TCR_39BITS		(25u | (1u << 8) | (1u << 10) | (3u << 12) | (1u << 23))
#define SMMU_TCR2_VAL		(1u | (1u << 4) | (7u << 15))	/* 36-bit PA, 16-bit ASID */
#define SMMU_MAIR0_VAL		0xf404ff44u	/* 0 uncached, 1 cached, 2 device */
#define SMMU_FSR_ALL		0xc00001feu

/* Page tables for the GMU's SMMU context (3-level, 4K pages, MMU off so PA = address). */
#define PTE_TABLE		3ull
#define PTE_PAGE		3ull
#define PTE_ACCESSED		(1ull << 10)
#define PTE_SHARED		(2ull << 8)
#define PTE_NOT_GLOBAL		(1ull << 11)
#define PTE_GMU			(PTE_SHARED | PTE_ACCESSED | PTE_NOT_GLOBAL)
#define PTE_ADDR		0x0000fffffffff000ull

/* GMU memory layout (Samsung a630: HFI at 0x60000000, dump at +1 MiB). */
#define GMU_HFI_ADDR		0x60000000u
#define GMU_DUMP_ADDR		0x60100000u
#define GMU_MEM_SIZE		0x4000u		/* 16 KiB each */
#define GMU_STREAM_USER		4
#define GMU_STREAM_KERNEL	5
#define GMU_CB			0		/* context bank the GMU gets */

/* HFI message protocol (kgsl_hfi.h) */
#define HFI_QUEUES		3		/* CMD(0), MSG(1), DBG(2) */
#define HFI_QUEUE_WORDS		1024
#define HFI_MSG_CMD		0
#define H2F_MSG_INIT		0
#define H2F_MSG_FW_VER		1
#define F2H_MSG_ACK		126
#define HFI_IRQ_MSGQ		(1u << 0)

static const char *stage;
static int step_row;

/* Static memory, 4K-aligned; with MMU off, physical address == address. */
static uint8_t smmu_pool[8][4096] __attribute__((aligned(4096)));
static unsigned smmu_pool_used;
static uint8_t gmu_hfi_mem[GMU_MEM_SIZE] __attribute__((aligned(4096)));
static uint8_t gmu_dump_mem[GMU_MEM_SIZE] __attribute__((aligned(4096)));

static uint32_t smmu_page = 0x1000, smmu_pages = 8, smmu_contexts, smmu_smrs;

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
	wdog_pet();
}

static void showhex(const char *label, uint64_t v)
{
	con_hex(label, v);
	wdog_pet();
}

static void zero(void *p, uint32_t n)
{
	for (uint32_t i = 0; i < n; i++)
		((volatile uint8_t *)p)[i] = 0;
}

static uint64_t pa(const void *p)
{
	return (uint64_t)(uintptr_t)p;
}

/* ---- power (M10 stages 1-2, condensed) ------------------------------------------------------------- */

static int clk_branch_on(uintptr_t cbcr)
{
	wr32(cbcr, rd32(cbcr) | CBCR_ENABLE);
	dsb();
	for (int i = 0; i < 2000 && (rd32(cbcr) & CBCR_OFF); i++)
		delay_ms(1);
	return !(rd32(cbcr) & CBCR_OFF);
}

static int cx_power_on(void)
{
	uint32_t v = rd32(GPUCC + GPUCC_CX_GDSCR);

	if (v & SW_COLLAPSE) {
		wr32(GPUCC + GPUCC_CX_GDSCR, v & ~SW_COLLAPSE);
		dsb();
	}
	for (int i = 0; i < 2000; i++) {
		if ((rd32(GPUCC + GPUCC_CX_GDSCR) & PWR_ON) || (rd32(GPUCC + GPUCC_CX_HWCTRL) & PWR_ON))
			break;
		delay_ms(1);
	}
	return (rd32(GPUCC + GPUCC_CX_GDSCR) & PWR_ON) || (rd32(GPUCC + GPUCC_CX_HWCTRL) & PWR_ON);
}

static int power_on(void)
{
	stage = "1 energia";
	step("1 GPLL0 + CX GDSC + relogios");
	wr32(GCC + GCC_GPLL0_VOTE, rd32(GCC + GCC_GPLL0_VOTE) | (1u << 15) | (1u << 16));
	dsb();
	if (!clk_branch_on(GCC + GCC_CFG_AHB))
		return 0;
	if (!cx_power_on()) {
		con_str("1", "CX GDSC nao liga: parando");
		return 0;
	}
	showhex("gpucc cx gdscr", rd32(GPUCC + GPUCC_CX_GDSCR));
	/* GMU RCG 200 MHz */
	wr32(GPUCC + GPUCC_GMU_RCG_CFG, GMU_RCG_CFG_200MHZ);
	dsb();
	wr32(GPUCC + GPUCC_GMU_RCG_CMD, rd32(GPUCC + GPUCC_GMU_RCG_CMD) | CMD_UPDATE);
	dsb();
	for (int i = 0; i < 2000 && (rd32(GPUCC + GPUCC_GMU_RCG_CMD) & CMD_UPDATE); i++)
		delay_ms(1);
	clk_branch_on(GPUCC + GPUCC_CBCR_CXO);
	clk_branch_on(GPUCC + GPUCC_CBCR_AHB);
	if (!clk_branch_on(GPUCC + GPUCC_CBCR_CX_GMU))
		return 0;
	clk_branch_on(GCC + GCC_MEMNOC_GFX);
	clk_branch_on(GCC + GCC_DDRSS_GPU_AXI);
	clk_branch_on(GCC + GCC_SNOC_DVM_GFX);
	con_str("1", "GPU com energia e relogios");
	return 1;
}

/* ---- stage 2: read the SMMU as the boot loader / hypervisor left it --------------------------------- */

static void smmu_probe(void)
{
	uint32_t idr0, idr1;

	stage = "2 smmu leitura";
	step("2 le a SMMU da GPU (0x5040000)");
	idr0 = rd32(SMMU_BASE + SMMU_IDR0);
	idr1 = rd32(SMMU_BASE + SMMU_IDR1);
	smmu_smrs = idr0 & 0xff;
	smmu_page = (idr1 >> 31) ? 0x10000 : 0x1000;
	smmu_pages = 2u << ((idr1 >> 28) & 7);
	smmu_contexts = idr1 & 0xff;
	showhex("smmu idr0", idr0);
	showhex("smmu idr1", idr1);
	showhex("smmu scr0", rd32(SMMU_BASE + SMMU_SCR0));
	showhex("smmu sgfsr", rd32(SMMU_BASE + SMMU_SGFSR));
	{
		char b[80], *p = put(put_dec(put(b, "smr="), smmu_smrs), " cb=");
		p = put_dec(p, smmu_contexts);
		p = put(put_dec(put(p, " pages="), smmu_pages), " pg=");
		put_hex(p, smmu_page, 4);
		con_str("smmu", b);
	}
	if (2 * smmu_pages * smmu_page != SMMU_SIZE) {
		con_str("2", "layout nao bate com 0x10000: uso 4K/8 mesmo assim");
		smmu_page = 0x1000;
		smmu_pages = 8;
	}
	/* The stream table and context 0, as left (bypass/fault expected on the GPU's own, off, SMMU). */
	showhex("smr0", rd32(SMMU_BASE + SMMU_SMR(0)));
	showhex("s2cr0", rd32(SMMU_BASE + SMMU_S2CR(0)));
	showhex("cb0 sctlr", rd32(SMMU_BASE + smmu_pages * smmu_page + SMMU_CB_SCTLR));
}

/* ---- stage 3: page tables + the GMU's SMMU context -------------------------------------------------- */

static uint64_t *smmu_new_table(void)
{
	if (smmu_pool_used >= sizeof(smmu_pool) / sizeof(smmu_pool[0]))
		return 0;
	uint64_t *t = (uint64_t *)smmu_pool[smmu_pool_used++];
	zero(t, 4096);
	return t;
}

static uint64_t *smmu_root;

/* Map one 4K page of GMU address `va` to physical `phys`. */
static int smmu_map(uint64_t va, uint64_t phys)
{
	uint64_t *l1 = smmu_root, *l2, *l3;
	uint64_t e;

	e = l1[(va >> 30) & 0x1ff];
	if (e & 1)
		l2 = (uint64_t *)(uintptr_t)(e & PTE_ADDR);
	else {
		l2 = smmu_new_table();
		if (!l2)
			return 0;
		l1[(va >> 30) & 0x1ff] = pa(l2) | PTE_TABLE;
	}
	e = l2[(va >> 21) & 0x1ff];
	if (e & 1)
		l3 = (uint64_t *)(uintptr_t)(e & PTE_ADDR);
	else {
		l3 = smmu_new_table();
		if (!l3)
			return 0;
		l2[(va >> 21) & 0x1ff] = pa(l3) | PTE_TABLE;
	}
	l3[(va >> 12) & 0x1ff] = phys | PTE_GMU | PTE_PAGE;
	return 1;
}

static int smmu_context_on(void)
{
	uint32_t cb = GMU_CB, ctx;
	uint64_t ttbr;

	stage = "3 smmu contexto";
	step("3a tabelas p/ HFI 0x60000000 + dump");
	smmu_root = smmu_new_table();
	if (!smmu_root)
		return 0;
	for (uint32_t off = 0; off < GMU_MEM_SIZE; off += 4096)
		if (!smmu_map(GMU_HFI_ADDR + off, pa(gmu_hfi_mem) + off) ||
		    !smmu_map(GMU_DUMP_ADDR + off, pa(gmu_dump_mem) + off)) {
			con_str("3", "sem tabelas");
			return 0;
		}
	dsb();	/* the SMMU reads the tables from memory (Device: no cache to flush) */

	step("3b programa o contexto da SMMU (SID 4/5 -> CB0)");
	ctx = smmu_pages * smmu_page + cb * smmu_page;		/* CB0 register page */
	ttbr = pa(smmu_root) | ((uint64_t)cb << 48);		/* tagged with the ASID */
	/* global enable: translation on (not client-pd bypass), fault handling */
	wr32(SMMU_BASE + SMMU_SCR0, rd32(SMMU_BASE + SMMU_SCR0) & ~(1u << 0));	/* CLIENTPD=0 */
	dsb();
	wr32(SMMU_BASE + SMMU_CB_SCTLR + ctx, 0);
	wr32(SMMU_BASE + smmu_page + SMMU_CBA2R(cb), SMMU_CBA2R_64BIT);
	wr32(SMMU_BASE + smmu_page + SMMU_CBAR(cb), SMMU_CBAR_STAGE_1);
	wr32(SMMU_BASE + SMMU_CB_TCR2 + ctx, SMMU_TCR2_VAL);
	wr32(SMMU_BASE + SMMU_CB_TCR + ctx, SMMU_TCR_39BITS);
	wr32(SMMU_BASE + SMMU_CB_TTBR0 + ctx, (uint32_t)ttbr);
	wr32(SMMU_BASE + SMMU_CB_TTBR0 + 4 + ctx, (uint32_t)(ttbr >> 32));
	wr32(SMMU_BASE + SMMU_CB_TTBR1 + ctx, 0);
	wr32(SMMU_BASE + SMMU_CB_TTBR1 + 4 + ctx, 0);
	wr32(SMMU_BASE + SMMU_CB_MAIR0 + ctx, SMMU_MAIR0_VAL);
	wr32(SMMU_BASE + SMMU_CB_MAIR1 + ctx, 0);
	wr32(SMMU_BASE + SMMU_CB_FSR + ctx, SMMU_FSR_ALL);
	wr32(SMMU_BASE + SMMU_CB_TLBIALL + ctx, 0);
	wr32(SMMU_BASE + SMMU_CB_TLBSYNC + ctx, 0);
	for (int i = 0; i < 100 && (rd32(SMMU_BASE + SMMU_CB_TLBSTATUS + ctx) & 1); i++)
		delay_ms(1);
	wr32(SMMU_BASE + SMMU_CB_SCTLR + ctx, SMMU_CB_SCTLR_ON);
	/* route the GMU's two streams to this context */
	wr32(SMMU_BASE + SMMU_S2CR(0), cb);
	wr32(SMMU_BASE + SMMU_SMR(0), SMMU_SMR_VALID | GMU_STREAM_KERNEL);
	wr32(SMMU_BASE + SMMU_S2CR(1), cb);
	wr32(SMMU_BASE + SMMU_SMR(1), SMMU_SMR_VALID | GMU_STREAM_USER);
	dsb();

	/* read every write back: if the hypervisor refused one, the photo shows it */
	showhex("cb0 sctlr<-", rd32(SMMU_BASE + SMMU_CB_SCTLR + ctx));
	showhex("cb0 ttbr0 lo", rd32(SMMU_BASE + SMMU_CB_TTBR0 + ctx));
	showhex("cbar0<-", rd32(SMMU_BASE + smmu_page + SMMU_CBAR(cb)));
	showhex("smr0<-", rd32(SMMU_BASE + SMMU_SMR(0)));
	showhex("s2cr0<-", rd32(SMMU_BASE + SMMU_S2CR(0)));
	if (!(rd32(SMMU_BASE + SMMU_CB_SCTLR + ctx) & 1) ||
	    (rd32(SMMU_BASE + SMMU_SMR(0)) != (SMMU_SMR_VALID | GMU_STREAM_KERNEL))) {
		con_str("3", "o hypervisor NAO deixou programar a SMMU (ver <- acima)");
		return 0;
	}
	con_str("3", "SMMU programada e confirmada (o hyp deixou)");
	return 1;
}

static int smmu_faulted(void)
{
	uint32_t ctx = smmu_pages * smmu_page + GMU_CB * smmu_page;
	uint32_t fsr = rd32(SMMU_BASE + SMMU_CB_FSR + ctx);

	if ((fsr & SMMU_FSR_ALL) == 0)
		return 0;
	showhex("SMMU RECUSOU fsr", fsr);
	showhex("  far lo", rd32(SMMU_BASE + SMMU_CB_FAR + ctx));
	return 1;
}

/* ---- the HFI queue table + polled send/receive ----------------------------------------------------- */

/* kgsl_hfi.h: 6-word table header, then one 12-word header per queue. */
#define HFI_TBL_HDR_WORDS	6
#define HFI_QHDR_WORDS		12
#define HFI_TABLE_WORDS		(HFI_TBL_HDR_WORDS + HFI_QUEUES * HFI_QHDR_WORDS)
#define HFI_QUEUE_OFFSET(i)	((HFI_TABLE_WORDS * 4u) + (i) * (4096u + 16u))	/* bytes */
/* queue header word indices */
#define QH_STATUS		0
#define QH_START		1
#define QH_TYPE			2
#define QH_SIZE			3
#define QH_MSG_SIZE		4
#define QH_RX_REQ		8
#define QH_READ			10
#define QH_WRITE		11

static volatile uint32_t *hfi;	/* = gmu_hfi_mem as words */

static uint32_t hfi_number;
static uint32_t hfi_payload0;	/* first payload word of the last answer (e.g. the firmware version) */

static volatile uint32_t *hfi_qhdr(unsigned q)
{
	return hfi + HFI_TBL_HDR_WORDS + q * HFI_QHDR_WORDS;
}

static void hfi_table_init(void)
{
	zero(gmu_hfi_mem, GMU_MEM_SIZE);
	hfi = (volatile uint32_t *)gmu_hfi_mem;
	hfi[0] = 0;				/* version */
	hfi[1] = HFI_TABLE_WORDS;		/* size (dwords) */
	hfi[2] = HFI_TBL_HDR_WORDS;		/* qhdr0 offset */
	hfi[3] = HFI_QHDR_WORDS;		/* qhdr size */
	hfi[4] = HFI_QUEUES;			/* num_q */
	hfi[5] = HFI_QUEUES;			/* num_active_q */
	{
		static const uint8_t qid[HFI_QUEUES] = { 0, 4, 5 };
		static const uint8_t qprio[HFI_QUEUES] = { 10, 10, 40 };
		for (unsigned i = 0; i < HFI_QUEUES; i++) {
			volatile uint32_t *h = hfi_qhdr(i);
			h[QH_STATUS] = 1;
			h[QH_START] = GMU_HFI_ADDR + HFI_QUEUE_OFFSET(i);
			h[QH_TYPE] = qid[i] | ((uint32_t)qprio[i] << 8);
			h[QH_SIZE] = HFI_QUEUE_WORDS;
			h[QH_MSG_SIZE] = 0;
			h[QH_RX_REQ] = 1;
			h[QH_READ] = 0;
			h[QH_WRITE] = 0;
		}
	}
	dsb();
}

/* Receive from queue `which`; true if the answer to `number` was seen. */
static int hfi_receive(unsigned which, uint32_t number, uint32_t *verdict)
{
	volatile uint32_t *h = hfi_qhdr(which);
	volatile uint32_t *q = (volatile uint32_t *)(gmu_hfi_mem + HFI_QUEUE_OFFSET(which));
	int answered = 0;

	while (h[QH_READ] != h[QH_WRITE]) {
		uint32_t at = h[QH_READ] % HFI_QUEUE_WORDS, msg0 = q[at], words = (msg0 >> 8) & 0xff;
		uint32_t m[8] = { 0 };

		for (uint32_t i = 0; i < words; i++) {
			if (i < 8)
				m[i] = q[at];
			at = (at + 1) % HFI_QUEUE_WORDS;
		}
		h[QH_READ] = (at + 3) / 4 * 4 % HFI_QUEUE_WORDS;
		if (((m[0] >> 16) & 0xf) == 1 /* ACK */ && (m[1] >> 20) == number) {
			*verdict = m[2];	/* error (0 = ok) */
			hfi_payload0 = m[3];	/* payload[0], e.g. the firmware version */
			answered = 1;
		}
		if (words == 0)
			break;
	}
	return answered;
}

/* Send `words` to the CMD queue and wait for the GMU's verdict. */
static int hfi_send(const char *what, uint32_t id, uint32_t *msg, uint32_t words, uint32_t *verdict)
{
	volatile uint32_t *h = hfi_qhdr(0);
	volatile uint32_t *q = (volatile uint32_t *)(gmu_hfi_mem + HFI_QUEUE_OFFSET(0));
	uint32_t at = h[QH_WRITE] % HFI_QUEUE_WORDS, number = ++hfi_number & 0xfff;
	int answered = 0;

	msg[0] = id | (words << 8) | (number << 20);
	for (uint32_t i = 0; i < (words + 3) / 4 * 4; i++) {
		q[at] = i < words ? msg[i] : 0xfafafafau;
		at = (at + 1) % HFI_QUEUE_WORDS;
	}
	h[QH_WRITE] = at;
	dsb();
	wr32(GMU(GMU_HOST2GMU_INTR_SET), 1);
	for (int i = 0; i < 5000 && !answered; i++) {
		if (rd32(GMU(GMU_GMU2HOST_INTR_INFO)) & HFI_IRQ_MSGQ) {
			wr32(GMU(GMU_GMU2HOST_INTR_CLR), HFI_IRQ_MSGQ);
			answered = hfi_receive(1, number, verdict);
		}
		if (smmu_faulted())
			break;
		wdog_pet();
		delay_ms(1);
	}
	con_str(what, answered ? (*verdict ? "GMU respondeu NAO" : "GMU aceitou") : "GMU nao respondeu");
	return answered && *verdict == 0;
}

/* ---- the RSC/PDC micro-code (M10) ------------------------------------------------------------------- */

#ifdef HAVE_GMU_FW
static void load_rpmh_ucode(void)
{
	static const uint32_t rsc_seq[5] = { 0xA7A506A0u, 0xA1E6A6E7u, 0xA2E081E1u, 0xE9A982E2u, 0x0020E8A8u };
	static const uint32_t pdc_seq[5] = { 0xFEBEA1E1u, 0xA5A4A3A2u, 0x8382A6E0u, 0xBCE3E284u, 0x002081FCu };

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
	for (unsigned i = 0; i < 5; i++)
		wr32(GMU(RSCC_SEQ_MEM_0_DRV0) + 4 * i, rsc_seq[i]);
	for (unsigned i = 0; i < 5; i++)
		wr32(PDC(PDC_GPU_SEQ_MEM_0) + 4 * i, pdc_seq[i]);
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

/* ---- stage 4: firmware + boot (with HFI_QTBL set) --------------------------------------------------- */

static int gmu_boot_with_hfi(void)
{
	uint32_t result = 0, wrong = 0;
	unsigned words = sizeof(a630_gmu_fw) / sizeof(a630_gmu_fw[0]);

	stage = "4 firmware+boot";
	step("4a RSC/PDC + firmware no ITCM");
	load_rpmh_ucode();
	wr32(GMU(GMU_CM3_SYSRESET), 1);
	dsb();
	wr32(GMU(GMU_GENERAL_7), 1);
	for (unsigned i = 0; i < words; i++)
		wr32(GMU(GMU_CM3_ITCM_START) + 4 * i, a630_gmu_fw[i]);
	dsb();
	for (unsigned i = 0; i < words; i++)
		if (rd32(GMU(GMU_CM3_ITCM_START) + 4 * i) != a630_gmu_fw[i])
			wrong++;
	if (wrong) {
		showhex("itcm errados", wrong);
		con_str("4", "ITCM nao guarda o firmware");
		return 0;
	}

	step("4b config (HFI_QTBL=0x60000000) e solta o GMU");
	hfi_table_init();
	wr32(GMU(GMU_CM3_FW_INIT_RESULT), 0);
	wr32(GMU(GMU_CM3_BOOT_CONFIG), GMU_BOOT_CONFIG_TCM);
	wr32(GMU(GMU_HFI_QTBL_ADDR), GMU_HFI_ADDR);
	wr32(GMU(GMU_HFI_QTBL_INFO), 1);
	wr32(GMU(GMU_AHB_FENCE_RANGE_0), FENCE_RANGE_MASK);
	wr32(GMU(GMU_HFI_SFR_ADDR), GMU_CHIPID);
	wr32(GMU(GMU_SYS_BUS_CONFIG), 1);
	wr32(GMU(GMU_PWR_COL_FRAME_CTRL), 0x9C40400u);
	wr32(GMU(GMU_RPMH_CTRL), 0);
	wr32(GMU(GMU_GMU2HOST_INTR_CLR), 0xFFFFFFFFu);
	wr32(GMU(GMU_GMU2HOST_INTR_MASK), (uint32_t)~HFI_IRQ_MSGQ);	/* let the message-queue interrupt through */
	wr32(GMU(GMU_CX_GMU_WFI_CONFIG), 0);
	dsb();
	wr32(GMU(GMU_CM3_SYSRESET), 0);
	dsb();
	for (int i = 0; i < 1000 && result != GMU_BABEFACE; i++) {
		result = rd32(GMU(GMU_CM3_FW_INIT_RESULT));
		wdog_pet();
		delay_ms(1);
	}
	showhex("gmu fw_init_result", result);
	if (result != GMU_BABEFACE) {
		con_str("4", "o GMU nao subiu");
		return 0;
	}
	con_str("4", "GMU rodando (0xBABEFACE), agora com a SMMU");
	return 1;
}

/* ---- stage 5: HFI up + INIT + FW_VER ---------------------------------------------------------------- */

static void hfi_talk(void)
{
	uint32_t msg[16], verdict = 0;

	stage = "5 HFI";
	step("5a liga as filas de mensagem (HFI_CTRL_INIT)");
	wr32(GMU(GMU_HFI_CTRL_INIT), 1);
	dsb();
	{
		int up = 0;
		for (int i = 0; i < 500 && !up; i++) {
			if (rd32(GMU(GMU_HFI_CTRL_STATUS)) & 1)
				up = 1;
			else
				delay_ms(1);
		}
		showhex("hfi_ctrl_status", rd32(GMU(GMU_HFI_CTRL_STATUS)));
		showhex("hfi version_info", rd32(GMU(GMU_HFI_VERSION_INFO)));
		if (!up) {
			con_str("5", "as filas HFI nao sobem");
			smmu_faulted();
			return;
		}
	}

	step("5b manda H2F_MSG_INIT");
	zero(msg, sizeof(msg));
	/* hfi_gmu_init_cmd: seg_id, dbg_buffer_addr, dbg_buffer_size, boot_state (cold=1) */
	msg[1] = 0;
	msg[2] = GMU_DUMP_ADDR;
	msg[3] = GMU_MEM_SIZE;
	msg[4] = 1;
	if (!hfi_send("5b INIT", H2F_MSG_INIT, msg, 5, &verdict))
		return;

	step("5c pede a versao do firmware (FW_VER)");
	zero(msg, sizeof(msg));
	msg[1] = (1u << 28) | (3u << 16) | 0;	/* expected FW_VERSION(major=1,minor=3) */
	if (hfi_send("5c FW_VER", H2F_MSG_FW_VER, msg, 2, &verdict))
		showhex("GMU fw version", hfi_payload0);
	con_str("5", "O GMU CONVERSA POR HFI: canal de mensagens de pe");
}
#endif /* HAVE_GMU_FW */

void shim_main(uint64_t dtb)
{
	(void)dtb;
	fb_default();
	con_clear(GREY);
	con_title("SHIM M11: SMMU + HFI DA ADRENO 630");
	stage = "watchdog";
	wdog_arm();
	step_row = con_row();
	con_skip();

	if (power_on()) {
		con_skip();
		smmu_probe();
		con_skip();
#ifdef HAVE_GMU_FW
		if (smmu_context_on() && gmu_boot_with_hfi()) {
			con_skip();
			hfi_talk();
		}
#else
		stage = "firmware";
		step("firmware do GMU");
		con_str("", "sem fw/a630_gmu.bin: rode tools/gpu-fw-embed.py e recompile");
#endif
	}

	stage = "fim";
	step("fim");
	con_skip();
	con_str("", "Tire uma foto desta tela!");
	countdown_reboot(con_row(), END_SECONDS);
}
