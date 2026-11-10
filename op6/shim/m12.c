/*
 * OnePlus 6 milestone M12 (GPU stage C): power the GX domain (the Adreno 630
 * core) up through the GMU, read a few core registers, power it down again.
 * Everything M11 does (power, cmd-db votes, GPU SMMU for the GMU, firmware,
 * HFI v1) is m11.c itself, included below with its shim_main left out; then
 * the "legacy" a630 sequence of Linux v6.6 a6xx_gmu.c / a6xx_gpu.c (facts
 * only, no code copied):
 *
 *   a6xx_gmu_power_config   GMU_RPMH_CTRL = interface | LLC | DDR | MX | CX |
 *                           GFX vote enable (0xf11): without it the GMU
 *                           cannot vote gfx.lvl and GX has no rail. Written
 *                           before the CM3 is released, read back.
 *   a6xx_gmu_gfx_rail_on    BOOT_SLUMBER_OPTION = 0, GX_VOTE_IDX / MX_VOTE_IDX
 *                           from the perf vote of the highest OPP (Linux takes
 *                           gx_arc_votes[nr_gpu_freqs - 1]), OOB BOOT_SLUMBER:
 *                           HOST2GMU_INTR_SET bit 22, ack GMU2HOST_INTR_INFO
 *                           bit 30, cleared in GMU2HOST_INTR_CLR.
 *   a6xx_sptprac_enable     GX_SPTPRAC_POWER_CONTROL = 0x778000, status & 0x38
 *                           == 0x28 (Linux only logs a failure).
 *   a6xx_gmu_hfi_start + a6xx_hfi_start_v1   = M11's HFI part, AFTER GX (the
 *                           Linux order; M11 had no GX step in between).
 *   a6xx_gmu_set_initial_freq (legacy)  DCVS_ACK_OPTION 0, DCVS_PERF_SETTING
 *                           (3 << 28) | current_perf_index (= nr - 1 at boot),
 *                           DCVS_BW_SETTING 0xff, OOB DCVS (set 23, ack 31,
 *                           clear 31), DCVS_RETURN.
 *   hw_init                 OOB GPU_SET (set 16, ack 24) keeps GX on while the
 *                           CPU reads the core; at the end clear GPU_SET (24)
 *                           and BOOT_SLUMBER (30).
 *   a6xx_gmu_shutdown       wait POWER_STATE == 0 (idle level ACTIVE), VBIF XIN
 *                           halt (a630 has no GBIF), notify_slumber: power
 *                           counter off, sptprac_disable (GX GDSCR retain-FF,
 *                           0x778001, status bit 2), BOOT_SLUMBER_OPTION = 1 +
 *                           OOB BOOT_SLUMBER set/clear, POWER_STATE must be 0xf,
 *                           AHB fence to allow; CX busy poll; hfi_stop; GMU
 *                           interrupt masks; a6xx_rpmh_stop (GPU_RPMH=1 only);
 *                           a6xx_gmu_stop's GX GDSC disable (SW_COLLAPSE, wait
 *                           POWER_DOWN_COMPLETE, clamp IO) only if GX is still
 *                           on. A GMU that does not go idle gets the
 *                           a6xx_gmu_force_off subset that touches no GX reg.
 *   Then the CM3 goes back to reset and the screen counts down to a reboot.
 *
 * Core registers (gpu@5000000) are read only when the GMU acked BOOT_SLUMBER
 * and GPU_SET, its SPTPRAC status says GX head-switch on and the gpucc GX
 * GDSC (CFG_GDSCR, POLL_CFG_GDSCR as gpucc-sdm845) says POWER_UP_COMPLETE.
 * The only core writes are the VBIF halt/unhalt of the Linux shutdown.
 *
 * Outside the GPU block: what M11 writes (GCC branches, GPU PDC micro-code)
 * plus the RPMh votes the GMU itself sends through the GPU RSC once
 * GMU_RPMH_CTRL is set (gfx.lvl, mx.lvl, cx.lvl; DDR/LLC as its firmware
 * decides). The CPU sends no RPMh vote, writes no PMIC, no apps SMMU, no SCM.
 * GPU_RPMH=1 adds the GPU-RSC wake/sleep handshake around it, as Linux does
 * on every resume (the PDC then sends its wake/sleep TCS).
 */
#define M11_LIB 1
#include "m11.c"

#ifndef M12_OPP
#define M12_OPP		7	/* gpu_khz[] index: 7 = 710 MHz, Linux's boot vote; 1 = 257 MHz */
#endif
#if M12_OPP < 1 || M12_OPP > 7
#error "M12_OPP must be 1..7"
#endif
#define OOB_MS		100	/* Linux waits 10 ms per OOB ack */

/* a6xx_gmu.xml.h (dword offsets in the GMU window) */
#define GMU_SPTPRAC_CTRL	0x0081
#define GMU_BOOT_SLUMBER_OPTION	0x23f8
#define GMU_GX_VOTE_IDX		0x23f9
#define GMU_MX_VOTE_IDX		0x23fa
#define GMU_DCVS_ACK_OPTION	0x23fc
#define GMU_DCVS_PERF_SETTING	0x23fd
#define GMU_DCVS_BW_SETTING	0x23fe
#define GMU_DCVS_RETURN		0x23ff
#define GMU_POWER_COUNTER_EN	0x5040
#define GMU_AO_COUNTER_H	0x5089
#define GMU_PWR_COL_KEEPALIVE	0x50c3
#define GMU_SPTPRAC_STATUS	0x50d0
#define GMU_AO_HOST_INTR_MASK	0x9306
#define GMU_CX_BUSY_STATUS	0x930c
#define GMU_AO_FENCE_CTRL	0x9310
#define GMU_FENCE_STATUS_CLR	0x9314

#define RPMH_CTRL_LINUX		0xf11u		/* INTERFACE | LLC | DDR | MX | CX | GFX vote enable */
#define SPT_GX_OFF		((1u << 6) | (1u << 7))	/* GX_HM_GDSC_POWER_OFF | GX_HM_CLK_OFF */
#define SPT_PWR_OFF		(1u << 2)	/* SPTPRAC_GDSC_POWER_OFF */
#define CX_BUSY_GPU		(1u << 23)	/* GPUBUSYIGNAHB */
#define PWR_SLUMBER		0xfu		/* GPU_GMU_CX_GMU_RPMH_POWER_STATE after slumber */

/* a6xx_gmu_oob_bits (legacy column) */
#define OOB_GPU_SET		16
#define OOB_GPU_ACK		24
#define OOB_BOOT_SET		22
#define OOB_BOOT_ACK		30
#define OOB_DCVS_SET		23
#define OOB_DCVS_ACK		31

/* gpucc-sdm845.c gpu_gx_gdsc (CLAMP_IO | AON_RESET | POLL_CFG_GDSCR), gdsc.c */
#define GPUCC_GX_CFG		0x1010		/* gdscr + CFG_GDSCR_OFFSET */
#define GPUCC_GX_CLAMP		0x1508
#define GDSC_UP			(1u << 16)	/* POWER_UP_COMPLETE */
#define GDSC_DOWN		(1u << 15)	/* POWER_DOWN_COMPLETE */
#define GDSC_RETAIN_FF		(1u << 11)
#define GDSC_COLLAPSE		(1u << 0)

/* GPU core (a6xx.xml.h, dword offsets in gpu@5000000) */
#define GPU_BASE		0x05000000UL
#define GPU(n)			(GPU_BASE + ((uintptr_t)(n) << 2))
#define RBBM_INT_0_STATUS	0x0201
#define RBBM_STATUS		0x0210		/* +1..+3: STATUS1..3 */
#define CP_SQE_CNTL		0x0808
#define CP_HW_FAULT		0x0821
#define CP_STATUS_1		0x0825
#define CP_SQE_INSTR_BASE	0x0830
#define CP_ALWAYS_ON		0x0980
#define VBIF_VERSION		0x3000
#define VBIF_XIN_HALT_CTRL0	0x3080
#define VBIF_XIN_HALT_CTRL1	0x3081

static int booted, gx_boot, gpu_set, gx_on;

/* say() even while gpu_quiet hides the M11 part */
static void tell(const char *label, const char *s)
{
	int q = gpu_quiet;

	gpu_quiet = 0;
	say(label, s);
	gpu_quiet = q;
}

/* Write, read back, refuse (0) when the masked bits differ. */
static int wrv(const char *what, uintptr_t a, uint32_t v, uint32_t mask)
{
	char b[80], *p;
	uint32_t got;

	wr(a, v);
	got = rd32(a);
	if ((got & mask) == (v & mask))
		return 1;
	p = put(b, what);
	p = put_hex(put(p, " w="), v, 8);
	put_hex(put(p, " r="), got, 8);
	tell("RECUSA", b);
	return 0;
}

/* Busy-poll (no sleep: the ack latency in us is part of the result). */
static int waitfor(uintptr_t a, uint32_t mask, uint32_t want, unsigned ms, uint32_t *us)
{
	uint64_t t0 = now(), lim = ticks_per_s() / 1000 * ms;
	int ok;

	for (;;) {
		ok = (rd32(a) & mask) == want;
		if (ok || now() - t0 > lim)
			break;
	}
	if (us)
		*us = ticks_to_us(now() - t0);
	wdog_pet();
	return ok;
}

static uint64_t rd64hl(uintptr_t lo, uintptr_t hi)
{
	uint32_t h = rd32(hi), l = rd32(lo), h2 = rd32(hi);

	if (h2 != h)
		l = rd32(lo);
	return ((uint64_t)h2 << 32) | l;
}

/* a6xx_gmu_set_oob, legacy bits. Stale ack cleared first. 1 = acked. */
static int oob_set(const char *name, unsigned req, unsigned ack)
{
	char b[96], *p;
	uint32_t us, before = rd32(GMU(GMU_GMU2HOST_INTR_INFO));

	if (before & (1u << ack))
		wr(GMU(GMU_GMU2HOST_INTR_CLR), 1u << ack);
	wr(GMU(GMU_HOST2GMU_INTR_SET), 1u << req);
	int ok = waitfor(GMU(GMU_GMU2HOST_INTR_INFO), 1u << ack, 1u << ack, OOB_MS, &us);
	uint32_t info = rd32(GMU(GMU_GMU2HOST_INTR_INFO));
	wr(GMU(GMU_GMU2HOST_INTR_CLR), 1u << ack);
	p = put(b, ok ? "ack " : "SEM ACK ");
	p = put(put_dec(p, us), "us");
	p = put_hex(put(p, " i="), info, 8);
	put_hex(put(p, " p="), rd32(GMU(GMU_RPMH_POWER_STATE)), 2);
	tell(name, b);
	p = put_hex(put(b, "antes i="), before, 8);
	put_hex(put(p, " depois do clr i="), rd32(GMU(GMU_GMU2HOST_INTR_INFO)), 8);
	saylog(name, b);
	return ok;
}

/* a6xx_gmu_clear_oob: the clear is a HOST2GMU bit too. */
static void oob_clear(unsigned bit)
{
	wr(GMU(GMU_HOST2GMU_INTR_SET), 1u << bit);
}

/* GX as gpucc and the GMU see it; 1 = both say on. Read-only (CX side). */
static int gx_state(const char *label, int visible)
{
	char b[120], *p;
	uint32_t gd = rd32(GPUCC + GPUCC_GX_GDSCR), cf = rd32(GPUCC + GPUCC_GX_CFG);
	uint32_t sp = rd32(GMU(GMU_SPTPRAC_STATUS)), pw = rd32(GMU(GMU_RPMH_POWER_STATE));

	p = put_hex(put(b, "gd="), gd, 8);
	p = put_hex(put(p, " cf="), cf, 8);
	p = put_hex(put(p, " sp="), sp, 2);
	put_hex(put(p, " p="), pw, 2);
	(visible ? tell : saylog)(label, b);
	p = put_hex(put(b, "busy="), rd32(GMU(GMU_CX_BUSY_STATUS)), 8);
	p = put_hex(put(p, " fence="), rd32(GMU(GMU_AO_FENCE_CTRL)), 8);
	put_hex(put(p, " aoint="), rd32(GMU(GMU_AO_HOST_INTR_STATUS)), 8);
	saylog(label, b);
	return (cf & GDSC_UP) && !(sp & SPT_GX_OFF);
}

/* ---- 6. GX on (gfx_rail_on + sptprac_enable) ---- */

static int gx_rail_on(void)
{
	char b[96], *p;
	uint32_t v = gx_vote[M12_OPP], us;

	step("6 GX: BOOT_SLUMBER_OPTION=0 + votos");
	if (!wrv("slumber_opt", GMU(GMU_BOOT_SLUMBER_OPTION), 0, ~0u) ||
	    !wrv("gx_vote_idx", GMU(GMU_GX_VOTE_IDX), v & 0xff, ~0u) ||
	    !wrv("mx_vote_idx", GMU(GMU_MX_VOTE_IDX), (v >> 8) & 0xff, ~0u))
		return 0;
	p = put_dec(put(b, "opp "), gpu_khz[M12_OPP] / 1000);
	p = put_dec(put(p, "MHz gx="), v & 0xff);
	p = put_dec(put(p, " mx="), (v >> 8) & 0xff);
	put_hex(put(p, " lvl="), v >> 16, 3);
	tell("VOTO", b);

	step("6 OOB BOOT_SLUMBER: o GMU liga a GX");
	gx_boot = oob_set("OOB BOOT", OOB_BOOT_SET, OOB_BOOT_ACK);
	gx_state("GX", 1);
	if (!gx_boot) {
		tell("GX", "o GMU nao confirmou o boot da GX");
		return 0;
	}

	step("6 SPTPRAC liga (0x778000)");
	wr(GMU(GMU_SPTPRAC_CTRL), 0x778000u);
	int ok = waitfor(GMU(GMU_SPTPRAC_STATUS), 0x38, 0x28, 20, &us);
	p = put_hex(put(b, "st="), rd32(GMU(GMU_SPTPRAC_STATUS)), 2);
	p = put_hex(put(p, " ctl="), rd32(GMU(GMU_SPTPRAC_CTRL)), 8);
	put(p, ok ? " ligado" : " NAO (Linux segue)");
	tell("SPTPRAC", b);
	return 1;
}

/* ---- 8. GPU frequency vote (a6xx_gmu_set_freq, legacy) ---- */

static void dcvs(void)
{
	char b[64], *p;

	step("8 DCVS: frequencia pelo GMU");
	if (!wrv("dcvs_ack", GMU(GMU_DCVS_ACK_OPTION), 0, ~0u) ||
	    !wrv("dcvs_perf", GMU(GMU_DCVS_PERF_SETTING), (3u << 28) | M12_OPP, ~0u) ||
	    !wrv("dcvs_bw", GMU(GMU_DCVS_BW_SETTING), 0xff, ~0u))
		return;
	oob_set("OOB DCVS", OOB_DCVS_SET, OOB_DCVS_ACK);
	oob_clear(OOB_DCVS_ACK);
	p = put_hex(put(b, "return="), rd32(GMU(GMU_DCVS_RETURN)), 8);
	put(p, rd32(GMU(GMU_DCVS_RETURN)) ? " (erro do GMU)" : " ok");
	tell("DCVS", b);
}

/* ---- 10. core reads (GX confirmed on, GPU_SET held) ---- */

static int probes(void)
{
	char b[96], *p;
	uint64_t g0, g1, c0, c1, t0, t1;

	step("10 nucleo: RBBM/CP/VBIF (leitura)");
	uint32_t st = rd32(GPU(RBBM_STATUS));
	p = put_hex(put(b, "st="), st, 8);
	put_hex(put(p, " int0="), rd32(GPU(RBBM_INT_0_STATUS)), 8);
	tell("RBBM", b);
	p = put_hex(b, rd32(GPU(RBBM_STATUS + 1)), 8);
	p = put_hex(put(p, " "), rd32(GPU(RBBM_STATUS + 2)), 8);
	put_hex(put(p, " "), rd32(GPU(RBBM_STATUS + 3)), 8);
	tell("RBBM1-3", b);
	p = put_hex(put(b, "flt="), rd32(GPU(CP_HW_FAULT)), 8);
	p = put_hex(put(p, " st1="), rd32(GPU(CP_STATUS_1)), 8);
	put_hex(put(p, " vbif="), rd32(GPU(VBIF_VERSION)), 8);
	tell("CP", b);
	/* no SQE version register: Linux reads it from the firmware image; base 0 = no microcode */
	p = put_hex(put(b, "cntl="), rd32(GPU(CP_SQE_CNTL)), 8);
	put_hex(put(p, " base="), rd64hl(GPU(CP_SQE_INSTR_BASE), GPU(CP_SQE_INSTR_BASE + 1)), 16);
	tell("SQE", b);

	step("10 contadores always-on, 100 ms");
	t0 = now();
	g0 = rd64hl(GMU(GMU_AO_COUNTER_L), GMU(GMU_AO_COUNTER_H));
	c0 = rd64hl(GPU(CP_ALWAYS_ON), GPU(CP_ALWAYS_ON + 1));
	delay_ms(100);
	t1 = now();
	g1 = rd64hl(GMU(GMU_AO_COUNTER_L), GMU(GMU_AO_COUNTER_H));
	c1 = rd64hl(GPU(CP_ALWAYS_ON), GPU(CP_ALWAYS_ON + 1));
	p = put_dec(put(b, "d="), (uint32_t)(g1 - g0));
	p = put_dec(put(p, " em "), ticks_to_us(t1 - t0));
	put(p, g1 != g0 ? "us conta" : "us PARADO");
	tell("AO GMU", b);
	p = put_hex(put(b, "v="), c1, 12);
	p = put_dec(put(p, " d="), (uint32_t)(c1 - c0));
	put(p, c1 != c0 ? " conta" : " PARADO");
	tell("AO CP", b);
	p = put_hex(put(b, "gmu "), g0, 16);
	p = put_hex(put(p, ">"), g1, 16);
	p = put_hex(put(p, " cp "), c0, 16);
	put_hex(put(p, ">"), c1, 16);
	saylog("AO", b);
	return st != 0xffffffffu;
}

/* ---- 11. GX off, the a6xx_gmu_shutdown way ---- */

static int vbif_halt(void)
{
	step("11 VBIF: XIN halt (sem transacoes)");
	if (!wrv("vbif_halt0", GPU(VBIF_XIN_HALT_CTRL0), 0xf, 0xf))
		return 0;
	int ok = waitfor(GPU(VBIF_XIN_HALT_CTRL1), 0xf, 0xf, 100, 0);
	int un = wrv("vbif_halt0", GPU(VBIF_XIN_HALT_CTRL0), 0, 0xf);
	tell("VBIF", ok ? "halt ack, solto" : "halt SEM ack (segue)");
	return un;
}

static void sptprac_off(void)
{
	char b[64];

	wrv("gx_retain", GPUCC + GPUCC_GX_GDSCR, rd32(GPUCC + GPUCC_GX_GDSCR) | GDSC_RETAIN_FF, GDSC_RETAIN_FF);
	wr(GMU(GMU_SPTPRAC_CTRL), 0x778001u);
	int ok = waitfor(GMU(GMU_SPTPRAC_STATUS), SPT_PWR_OFF, SPT_PWR_OFF, 10, 0);
	put(put_hex(put(b, "off st="), rd32(GMU(GMU_SPTPRAC_STATUS)), 2), ok ? " ok" : " NAO desligou");
	saylog("SPTPRAC", b);
}

/* a6xx_gmu_notify_slumber (legacy). 1 = the GMU reached slumber (0xf). */
static int notify_slumber(void)
{
	step("11 slumber: contador + SPTPRAC off");
	wrv("pwr_counter", GMU(GMU_POWER_COUNTER_EN), 0, ~0u);
	sptprac_off();
	step("11 OOB BOOT_SLUMBER: o GMU desliga a GX");
	int ok = wrv("slumber_opt", GMU(GMU_BOOT_SLUMBER_OPTION), 1, ~0u) &&
	    oob_set("SLUMBER", OOB_BOOT_SET, OOB_BOOT_ACK);
	oob_clear(OOB_BOOT_ACK);
	int slept = rd32(GMU(GMU_RPMH_POWER_STATE)) == PWR_SLUMBER;
	wrv("fence_allow", GMU(GMU_AO_FENCE_CTRL), 0, ~0u);
	if (ok && !slept)
		tell("SLUMBER", "ack mas power_state != 0xf");
	return ok && slept;
}

static void hfi_stop_masks(void)
{
	char b[64], *p;

	for (unsigned q = 0; q < 2; q++) {
		qhdr(q)[QH_READ] = 0;
		qhdr(q)[QH_WRITE] = 0;
	}
	dsb();
	wr(GMU(GMU_AO_HOST_INTR_MASK), ~0u);
	wr(GMU(GMU_GMU2HOST_INTR_MASK), ~0u);
	p = put_hex(put(b, "ao_mask="), rd32(GMU(GMU_AO_HOST_INTR_MASK)), 8);
	put_hex(put(p, " g2h_mask="), rd32(GMU(GMU_GMU2HOST_INTR_MASK)), 8);
	saylog("MASK", b);
}

/* a6xx_gmu_force_off minus its GX writes (bus clear, sw reset). */
static void force_off(void)
{
	step("11 GMU nao dormiu: force_off");
	tell("DESLIGA", "force_off (GMU nao ocioso/sem slumber)");
	wr(GMU(GMU_PWR_COL_KEEPALIVE), 0);
	hfi_stop_masks();
	sptprac_off();
	wr(GMU(GMU_FENCE_STATUS_CLR), 7);
	wr(GMU(GMU_AO_FENCE_CTRL), 0);
}

/* a6xx_gmu_stop: gxpd put = gdsc_disable, effective only if GX is still on. */
static void gx_gdsc_off(void)
{
	char b[64], *p;

	if (!(rd32(GPUCC + GPUCC_GX_CFG) & GDSC_UP)) {	/* gdsc_check_status, POLL_CFG_GDSCR */
		gx_state("GX fim", 1);
		return;
	}
	step("12 GX GDSC ainda ligado: desliga");
	wr(GPUCC + GPUCC_GX_GDSCR, rd32(GPUCC + GPUCC_GX_GDSCR) | GDSC_COLLAPSE);
	int ok = waitfor(GPUCC + GPUCC_GX_CFG, GDSC_DOWN, GDSC_DOWN, 100, 0);
	if (ok)
		wrv("gx_clamp", GPUCC + GPUCC_GX_CLAMP, rd32(GPUCC + GPUCC_GX_CLAMP) | 1, 1);
	p = put(b, ok ? "GDSC desligado pela CPU" : "GDSC NAO desliga");
	put_hex(put(p, " gd="), rd32(GPUCC + GPUCC_GX_GDSCR), 8);
	tell("GX fim", b);
}

static void drop(void)
{
	if (!booted)
		return;
	if (gx_boot && rd32(GMU(GMU_RPMH_POWER_STATE)) != PWR_SLUMBER) {
		step("11 desliga: espera o GMU ocioso");
		int idle = waitfor(GMU(GMU_RPMH_POWER_STATE), ~0u, 0, 100, 0);
		int slept = 0;
		if (idle) {
			if (gx_on)
				vbif_halt();
			slept = notify_slumber();
		}
		if (!slept) {
			force_off();
		} else {
			uint32_t us;
			int quiet = waitfor(GMU(GMU_CX_BUSY_STATUS), CX_BUSY_GPU, 0, 10, &us);
			tell("DESLIGA", quiet ? "slumber ok (0xf), GPU ociosa" : "slumber ok, CX ainda ocupado");
			hfi_stop_masks();
			gpu_rpmh_stop();
		}
	} else {
		/* boot failed or already in slumber: a6xx_gmu_resume's error path */
		hfi_stop_masks();
		gpu_rpmh_stop();
	}
	gx_gdsc_off();
}

void shim_main(uint64_t dtb)
{
	char b[96], *p;

	(void)dtb;
	gpu_screen(gpu_rpmh_enabled() ? "OP6 SHIM M12: GX PELO GMU (RPMH)" : "OP6 SHIM M12: GX PELO GMU",
	    "\n=== OP6 shim M12 gx ===\n");

	step("1 estado do ABL (so leitura)");
	gpu_state(0);
	gpu_quiet = 1;		/* M11 part: ramlog only, last lines shown on failure */
	if (gpu_cmddb(0) || !votes()) {
		gpu_quiet_dump();
		tell("1", "sem tabela de votos: parando");
		goto out;
	}
	if (!gpu_power_on(0)) {
		gpu_quiet_dump();
		goto out;
	}
	gx_state("GX antes", 1);
	if (!smmu_probe()) {
		gpu_quiet_dump();
		goto out;
	}
	if (!gpu_have_fw()) {
		gpu_quiet_dump();
		tell("FW", "SEM FIRMWARE: a630_gmu.bin nao embutido");
		goto out;
	}
	if (!smmu_program()) {
		gpu_quiet_dump();
		goto out;
	}
	hfi_table();
	gpu_rpmh_ctrl = RPMH_CTRL_LINUX;
	if (!gpu_gmu_boot(VA_HFI, VA_LOG | (sizeof(mem_log) / 4096 - 1), ~INTR_MSGQ)) {
		smmu_faulted("boot");
		gpu_quiet_dump();
		gpu_gmu_halt();
		goto out;
	}
	booted = 1;
	tell("M11", "SMMU+ATS ok, GMU 0xBABEFACE");
	p = put_hex(put(b, "ctrl="), rd32(GMU(GMU_RPMH_CTRL)), 8);
	p = put_hex(put(p, " pwr="), rd32(GMU(GMU_RPMH_POWER_STATE)), 2);
	put(p, gpu_rpmh_enabled() ? " rsc=wake" : " rsc=-");
	tell("RPMH", b);
	con_skip();

	if (!gx_rail_on())
		goto down;

	step("7 HFI (INIT/VER/PERF/BW/TEST)");
	if (!hfi_talk()) {
		gpu_quiet_dump();
		goto down;
	}
	gpu_quiet = 0;
	tell("HFI", "INIT/VER/PERF/BW ok");
	dcvs();

	step("9 OOB GPU_SET: segura a GX");
	gpu_set = oob_set("OOB GPU", OOB_GPU_SET, OOB_GPU_ACK);
	gx_on = gx_state("GX", 1) && gpu_set;
	if (!gx_on) {
		tell("GX", "nao confirmada: nucleo NAO lido");
	} else if (probes()) {
		tell("10", "A GX LIGOU: O NUCLEO DA GPU RESPONDE");
	} else {
		tell("10", "GX ligada mas RBBM le FFFFFFFF");
	}
	smmu_faulted("nucleo");
	step("9 OOB clear GPU_SET + BOOT_SLUMBER");
	if (gpu_set)
		oob_clear(OOB_GPU_ACK);
	oob_clear(OOB_BOOT_ACK);
	con_skip();
down:
	gpu_quiet = 0;
	drop();
	smmu_faulted("fim");
	step("CM3 de volta ao reset");
	if (booted)
		gpu_gmu_halt();
out:
	gpu_quiet = 0;
	step("fim");
	con_skip();
	say("", "Tire uma foto desta tela!");
	countdown_reboot(con_row(), END_SECONDS);
}
