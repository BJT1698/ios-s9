#pragma once
#include <stdint.h>

/*
 * Adreno 630 / GMU pieces shared by the OP6 GPU stages (m10, m11). All
 * addresses from the OP6 live DT (op6-live.dts, mainline sdm845.dtsi as pmOS
 * boots it); register numbers are the dword offsets of Linux
 * a6xx_gmu.xml.h / a6xx.xml.h (v6.6). The S9 port used the kgsl numbering of
 * the same registers: kgsl = mainline + 0x1A800 (window at GMU_BASE).
 */

/* ---- blocks ---- */
#define GCC			0x00100000UL	/* clock-controller@100000 */
#define GPUCC			0x05090000UL	/* clock-controller@5090000 (qcom,sdm845-gpucc) */
#define GMU_BASE		0x0506a000UL	/* gmu@506a000 reg "gmu" (0x30000) */
#define GMU_RSCC_BASE		(GMU_BASE + 0x23000UL)	/* a6xx_gmu.c: rscc = mmio + 0x23000 (pre-a650) */
#define GPU_PDC			0x0b280000UL	/* gmu reg "gmu_pdc" (0x10000) */
#define GPU_PDC_SEQ		0x0b480000UL	/* gmu reg "gmu_pdc_seq" (0x10000) */
#define GPU_SMMU		0x05040000UL	/* iommu@5040000 qcom,adreno-smmu smmu-v2 (0x10000) */
#define GPU_SMMU_SIZE		0x10000UL

#define GMU(n)			(GMU_BASE + ((uintptr_t)(n) << 2))
#define RSCC(n)			(GMU_RSCC_BASE + ((uintptr_t)(n) << 2))
#define PDC(n)			(GPU_PDC + ((uintptr_t)(n) << 2))
#define PDCSEQ(n)		(GPU_PDC_SEQ + ((uintptr_t)(n) << 2))

/* ---- gpucc-sdm845.c / gcc-sdm845.c ---- */
#define GPUCC_GX_GDSCR		0x100c		/* gpu_gx_gdsc: NOT written by m10/m11 */
#define GPUCC_CX_GDSCR		0x106c		/* gpu_cx_gdsc (VOTABLE) */
#define GPUCC_CX_HW_CTRL	0x1540		/* gpu_cx_gdsc gds_hw_ctrl: status */
#define GPUCC_GMU_CMD_RCGR	0x1120		/* gpu_cc_gmu_clk_src */
#define GPUCC_GMU_CFG_RCGR	0x1124
#define GPUCC_AHB_CBCR		0x1078		/* gpu_cc_ahb_clk (kgsl/S9 only, not in mainline) */
#define GPUCC_CX_GMU_CBCR	0x1098		/* gpu_cc_cx_gmu_clk */
#define GPUCC_CXO_CBCR		0x109c		/* gpu_cc_cxo_clk */
#define GCC_GPLL0_VOTE		0x52004		/* bit 15 gcc_gpu_gpll0_clk_src, bit 16 gpll0_div */
#define GCC_DDRSS_GPU_AXI	0x44038
#define GCC_GPU_CFG_AHB		0x71004		/* CLK_IS_CRITICAL in mainline */
#define GCC_GPU_MEMNOC_GFX	0x7100c
#define GCC_GPU_SNOC_DVM	0x71018

/* ---- GMU (a6xx_gmu.xml.h) ---- */
#define GMU_CM3_ITCM_START	0x0c00
#define GMU_CM3_DTCM_START	0x1c00
#define GMU_ICACHE_CONFIG	0x4c00
#define GMU_DCACHE_CONFIG	0x4c01
#define GMU_SYS_BUS_CONFIG	0x4c0f
#define GMU_CM3_SYSRESET	0x5000
#define GMU_CM3_BOOT_CONFIG	0x5001
#define GMU_CX_GMU_WFI_CONFIG	0x5002		/* kgsl 0x1F802 */
#define GMU_CM3_FW_INIT_RESULT	0x501c
#define GMU_CM3_CFG		0x502d
#define GMU_AO_COUNTER_L	0x5088
#define GMU_PWR_COL_INTER_FRAME_CTRL 0x50c0
#define GMU_RPMH_CTRL		0x50e8
#define GMU_RPMH_POWER_STATE	0x50ec		/* kgsl 0x1F8EC */
#define GMU_PWR_COL_CP_MSG	0x5100
#define GMU_PWR_COL_CP_RESP	0x5101
#define GMU_HFI_CTRL_STATUS	0x5180
#define GMU_HFI_VERSION_INFO	0x5181
#define GMU_HFI_SFR_ADDR	0x5182
#define GMU_HFI_QTBL_INFO	0x5184
#define GMU_HFI_QTBL_ADDR	0x5185
#define GMU_HFI_CTRL_INIT	0x5186
#define GMU_GMU2HOST_INTR_CLR	0x5191
#define GMU_GMU2HOST_INTR_INFO	0x5192
#define GMU_GMU2HOST_INTR_MASK	0x5193
#define GMU_HOST2GMU_INTR_SET	0x5194
#define GMU_GENERAL_7		0x51cc
#define GMU_AO_HOST_INTR_STATUS	0x9305
#define GMU_RSCC_CONTROL_REQ	0x9307
#define GMU_RSCC_CONTROL_ACK	0x9308
#define GMU_AHB_FENCE_RANGE_0	0x9311

#define GMU_BABEFACE		0xbabefaceu

/* ---- results the stages show ---- */
struct arc {
	uint32_t addr;
	int n;			/* levels read, < 0 = cmd-db error */
	uint16_t lvl[16];
};

/* screen + ramlog idioms (every step written BEFORE it runs) */
void gpu_screen(const char *title, const char *logtitle);
void step(const char *s);
void say(const char *label, const char *s);
void sayhex(const char *label, uint32_t v);
void saylog(const char *label, const char *s);	/* ramlog only */

int gpu_have_fw(void);
unsigned gpu_fw_words(void);

int gpu_power_on(int verbose);
void gpu_state(int verbose);
int gpu_cmddb(int verbose);
extern struct arc arc_cx, arc_mx, arc_gfx;
uint32_t gpu_arc_vote(const struct arc *pri, unsigned level, int *ok);
void gpu_smmu_ids(void);

void gpu_rpmh_ucode(void);
int gpu_rpmh_start(void);
int gpu_gmu_boot(uint32_t qtbl, uint32_t log_msg, uint32_t msgq_mask);
void gpu_gmu_halt(void);
