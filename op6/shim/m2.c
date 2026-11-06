/*
 * Galaxy S9 milestone 2: the questions XNU's CPU bring-up depends on,
 * answered on screen in one boot. ios-sweet's milestone 2 shim (MIT,
 * dipa-studio) ported to the S9:
 *   - CPU feature registers (16K granule, GIC system-register interface)
 *   - PSCI/SMCCC versions and which PSCI calls the Qualcomm TZ offers
 *   - SMP: PSCI CPU_ON for the 7 secondaries, each reports MPIDR and MIDR
 *     (Kryo 385: cores 0-3 "silver" Cortex-A55, 4-7 "gold" Cortex-A75)
 *   - GICv3 Group 1 IRQs: virtual timer (INTID 19 on Qualcomm) and SGI 1
 * Live DTB (TWRP) matches sweet: PSCI 1.0 over smc, MPIDR Aff1 = core,
 * GICD 0x17a00000, GICR 0x17a60000 stride 0x20000, timer PPIs 1/2/3/0.
 * M1.1 showed ABL leaves pingpong autorefresh on, so writes to the
 * framebuffer reach the glass with no kick.
 */
#include <stdint.h>
#include "hw.h"
#include "fb.h"
#include "con.h"
#include "ramlog.h"

#define APSS_WDT		0x17980000UL	/* qcom,wdt@17980000 */
#define WDT0_RST		0x04		/* drivers/soc/qcom/watchdog_v2.c */
#define WDT0_EN			0x08
#define WDT0_BARK_TIME		0x10
#define WDT0_BITE_TIME		0x14
#define WDT_HZ			32765
#define WDT_BARK_S		15

#define GICD_BASE		0x17a00000UL
#define GICR_BASE		0x17a60000UL
#define GICR_SIZE		0x100000UL
#define GICR_PE_SIZE		0x20000UL
#define GICD_TYPER		0x04
#define GICR_TYPER		0x08
#define GICR_WAKER		0x14
#define GICR_SGI		0x10000
#define GICR_IGROUPR0		(GICR_SGI + 0x080)
#define GICR_ISENABLER0		(GICR_SGI + 0x100)
#define GICR_ICENABLER0		(GICR_SGI + 0x180)
#define GICR_ISPENDR0		(GICR_SGI + 0x200)
#define GICR_ISACTIVER0		(GICR_SGI + 0x300)
#define GICR_IPRIORITYR		(GICR_SGI + 0x400)
#define VTIMER_INTID		19	/* DT: <GIC_PPI 3 ...>, not the usual 27 */
#define PTIMER_INTID		18	/* DT: <GIC_PPI 2 ...> */
#define SGI_INTID		1

#define PSCI_VERSION		0x84000000UL
#define PSCI_CPU_SUSPEND	0xc4000001UL
#define PSCI_CPU_OFF		0x84000002UL
#define PSCI_CPU_ON		0xc4000003UL
#define PSCI_MIGRATE_INFO_TYPE	0x84000006UL
#define PSCI_SYSTEM_OFF		0x84000008UL
#define PSCI_SYSTEM_RESET	0x84000009UL
#define PSCI_FEATURES		0x8400000aUL
#define PSCI_SYSTEM_SUSPEND	0xc400000eUL
#define PSCI_SYSTEM_RESET2	0xc4000012UL
#define SMCCC_VERSION		0x80000000UL

#define NR_CPUS			8
#define RUN_SECONDS		90
#define PANIC_SECONDS		60

#define wsr(r, v) __asm__ volatile("msr " r ", %0" : : "r"((uint64_t)(v)))

static inline void wr8(uintptr_t a, uint8_t v) { *(volatile uint8_t *)a = v; }
static inline uint64_t rd64(uintptr_t a) { return *(volatile uint64_t *)a; }

extern char secondary_entry[];

static const char *stage;	/* set at runtime: no absolute pointers in data */
static volatile uint64_t sec_mpidr[NR_CPUS], sec_midr[NR_CPUS];
static volatile uint32_t irq_count, irq_other, irq_last;
static volatile int irq_src;
static volatile int in_exc;

static uint64_t smc(uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3)
{
	register uint64_t x0 __asm__("x0") = fn;
	register uint64_t x1 __asm__("x1") = a1;
	register uint64_t x2 __asm__("x2") = a2;
	register uint64_t x3 __asm__("x3") = a3;
	__asm__ volatile("smc #0"
			 : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
			 :
			 : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11",
			   "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	return x0;
}

static uint64_t now(void)
{
	isb();
	return rsr("cntpct_el0");
}

static uint64_t ticks_per_s(void)
{
	uint64_t f = rsr("cntfrq_el0");
	return f ? f : 19200000;
}

static void delay_ms(unsigned ms)
{
	uint64_t end = now() + ticks_per_s() / 1000 * ms;
	while (now() < end)
		;
}

static void wdog_pet(void)
{
	wr32(APSS_WDT + WDT0_RST, 1);
}

static void wdog_arm(void)
{
	wr32(APSS_WDT + WDT0_EN, 0);
	wr32(APSS_WDT + WDT0_BARK_TIME, WDT_BARK_S * WDT_HZ);
	wr32(APSS_WDT + WDT0_BITE_TIME, (WDT_BARK_S + 3) * WDT_HZ);
	wr32(APSS_WDT + WDT0_EN, 1);
	wdog_pet();
}

static void __attribute__((noreturn)) reboot(void)
{
	smc(PSCI_SYSTEM_RESET, 0, 0, 0);
	for (;;)
		__asm__ volatile("wfe");	/* the watchdog bites */
}

/* PSCI return codes are small negative numbers */
static char *put_ret(char *d, uint64_t r)
{
	int64_t v = (int64_t)r;

	if (v < 0) {
		*d++ = '-';
		v = -v;
	}
	return put_dec(d, (uint32_t)v);
}

/* ICC_SGI1R_EL1 value targeting the calling core only. */
static uint64_t sgi_self(unsigned id)
{
	uint64_t m = rsr("mpidr_el1");
	return ((m >> 16) & 0xff) << 32 |	/* Aff2 */
	       ((m >> 8) & 0xff) << 16 |	/* Aff1 */
	       (uint64_t)id << 24 |
	       (1U << (m & 0xf));		/* target list: Aff0 */
}

/* ---- exceptions ---- */

static void irq_handler(void)
{
	uint64_t iar = rsr("icc_iar1_el1");
	uint32_t id = iar & 0xffffff;

	if (id >= 1020 && id <= 1023)
		return;	/* spurious */
	irq_last = id;
	if (id == (uint32_t)irq_src)
		irq_count++;
	else
		irq_other++;
	if (id == VTIMER_INTID)
		wsr("cntv_tval_el0", ticks_per_s() / 100);	/* 10 ms */
	else if (id == PTIMER_INTID)
		wsr("cntp_tval_el0", ticks_per_s() / 100);
	wsr("icc_eoir1_el1", iar);
	isb();
}

/* regs: x0-x28, a pad word, then the x29/x30 the vector stashed */
void exc_handler(uint64_t slot, uint64_t *regs)
{
	char b[48];

	if (slot == 5) {	/* IRQ from current EL, SPx */
		irq_handler();
		return;
	}
	if (in_exc++)
		reboot();
	con_clear(PANICBG);
	con_title("PANICO NO SHIM");
	put_dec(put(b, (slot & 3) == 0 ? "SYNC slot " : (slot & 3) == 1 ? "IRQ slot " :
		    (slot & 3) == 2 ? "FIQ slot " : "SERROR slot "), slot);
	con_str("EXCECAO", b);
	con_str("ETAPA", stage ? stage : "?");
	con_hex2("CPU", "mpidr=", rsr("mpidr_el1"), 0, 0);
	con_hex("ESR", rsr("esr_el1"));
	con_hex("ELR", rsr("elr_el1"));
	con_hex("FAR", rsr("far_el1"));
	con_hex("LR", regs[31]);
	con_skip();
	con_str("", "Tire uma foto desta tela!");
	for (int s = 0; s < PANIC_SECONDS; s++) {
		wdog_pet();
		delay_ms(1000);
	}
	reboot();
}

/* ---- CPU features and PSCI ---- */

static void test_features(void)
{
	char b[64], *p;
	uint64_t pfr0 = rsr("id_aa64pfr0_el1"), mmfr0 = rsr("id_aa64mmfr0_el1");

	stage = "features";
	con_hex2("CPU0", "mpidr=", rsr("mpidr_el1"), "midr=", rsr("midr_el1"));
	p = put_dec(put(b, "EL"), (rsr("CurrentEL") >> 2) & 3);
	p = put(p, (mmfr0 >> 20) & 0xf ? "  16K sim" : "  16K NAO");
	p = put_dec(put(p, "  PARange "), mmfr0 & 0xf);
	put_dec(put(p, "  GICsys "), (pfr0 >> 24) & 0xf);
	con_str("FEATURES", b);
	con_hex2("ICC_SRE", "el1=", rsr("icc_sre_el1"), "cntfrq=", rsr("cntfrq_el0"));

	stage = "psci";
	con_hex2("PSCI", "ver=", smc(PSCI_VERSION, 0, 0, 0), "smccc=", smc(SMCCC_VERSION, 0, 0, 0));
	p = put_ret(put(b, "ON "), smc(PSCI_FEATURES, PSCI_CPU_ON, 0, 0));
	p = put_ret(put(p, " OFF "), smc(PSCI_FEATURES, PSCI_CPU_OFF, 0, 0));
	p = put_ret(put(p, " SUSP "), smc(PSCI_FEATURES, PSCI_CPU_SUSPEND, 0, 0));
	put_ret(put(p, " MIGR "), smc(PSCI_FEATURES, PSCI_MIGRATE_INFO_TYPE, 0, 0));
	con_str("PSCI FEA", b);
	p = put_ret(put(b, "SYSOFF "), smc(PSCI_FEATURES, PSCI_SYSTEM_OFF, 0, 0));
	p = put_ret(put(p, " RST "), smc(PSCI_FEATURES, PSCI_SYSTEM_RESET, 0, 0));
	p = put_ret(put(p, " RST2 "), smc(PSCI_FEATURES, PSCI_SYSTEM_RESET2, 0, 0));
	put_ret(put(p, " SYSSUSP "), smc(PSCI_FEATURES, PSCI_SYSTEM_SUSPEND, 0, 0));
	con_str("", b);
}

/* ---- SMP ---- */

void secondary_main(uint64_t idx)
{
	if (idx < NR_CPUS) {
		sec_midr[idx] = rsr("midr_el1");
		sec_mpidr[idx] = rsr("mpidr_el1") | (1ULL << 63);	/* "arrived" */
	}
	dsb();
	smc(PSCI_CPU_OFF, 0, 0, 0);
}

static void test_smp(void)
{
	uint64_t self = rsr("mpidr_el1") & 0xffffff;
	uint64_t ret[NR_CPUS];
	char b[64], *p;

	stage = "smp";
	for (int i = 1; i < NR_CPUS; i++)
		ret[i] = smc(PSCI_CPU_ON, (uint64_t)i << 8, (uintptr_t)secondary_entry, i);
	delay_ms(500);
	for (int i = 1; i < NR_CPUS; i++) {
		char label[] = "CPU0";
		label[3] = '0' + i;
		if (((uint64_t)i << 8) == self) {
			con_str(label, "e a CPU de boot");
			continue;
		}
		p = put_ret(put(b, "ret="), ret[i]);
		if (sec_mpidr[i]) {
			uint32_t part = (sec_midr[i] >> 4) & 0xfff;
			p = put_hex(put(p, " mpidr="), (uint32_t)sec_mpidr[i], 8);
			p = put_hex(put(p, " midr="), (uint32_t)sec_midr[i], 8);
			put(p, part == 0x803 ? " A55" : part == 0x802 ? " A75" : " ?");
		} else {
			put(p, " nao chegou");
		}
		con_str(label, b);
	}
}

/* ---- GIC + timer ---- */

static uintptr_t find_gicr(void)
{
	uint64_t aff = rsr("mpidr_el1");
	uint32_t want = (aff & 0xffffff) | ((aff >> 8) & 0xff000000);

	for (uintptr_t off = 0; off < GICR_SIZE; off += GICR_PE_SIZE) {
		uint64_t typer = rd64(GICR_BASE + off + GICR_TYPER);
		if ((uint32_t)(typer >> 32) == want)
			return GICR_BASE + off;
		if (typer & (1 << 4))	/* Last */
			break;
	}
	return 0;
}

static void gicd_wait_rwp(void)
{
	for (int i = 0; i < 1000000 && (rd32(GICD_BASE) & (1U << 31)); i++)
		;
}

/* Run one interrupt source for ~300 ms and report what the GIC saw. */
static void irq_window(const char *label, uintptr_t r, int src, int expect)
{
	uint64_t f = ticks_per_s();
	uint32_t pend, act;
	char b[64], *p;

	irq_count = irq_other = 0;
	irq_last = 0x3ff;
	irq_src = src;
	if (src == VTIMER_INTID) {
		wsr("cntv_tval_el0", f / 100);
		wsr("cntv_ctl_el0", 1);
	}
	isb();
	/* sample state with IRQs still masked, after the first period */
	uint64_t t = now() + f / 50;
	while (now() < t)
		;
	if (src == SGI_INTID)
		wsr("icc_sgi1r_el1", sgi_self(SGI_INTID));
	isb();
	pend = rd32(r + GICR_ISPENDR0);
	act = rd32(r + GICR_ISACTIVER0);

	__asm__ volatile("msr daifclr, #2" ::: "memory");
	t = now() + f * 3 / 10;
	/* SGI: one already sent above, send 9 more 10 ms apart -> expect 10 */
	uint64_t next = now() + f / 100;
	for (int sent = 1; now() < t;)
		if (src == SGI_INTID && sent < 10 && now() >= next) {
			wsr("icc_sgi1r_el1", sgi_self(SGI_INTID));
			isb();
			sent++;
			next += f / 100;
		}
	__asm__ volatile("msr daifset, #2" ::: "memory");
	wsr("cntv_ctl_el0", 0);
	isb();

	p = put_dec(put(b, "irqs="), irq_count);
	p = put_dec(put(p, " (espera "), expect);
	p = put_dec(put(p, ") outras="), irq_other);
	put_dec(put(p, " ultima="), irq_last);
	con_str(label, b);
	con_hex2("", "pend=", pend, "act=", act);
}

static void test_gic(void)
{
	stage = "gic sre";
	uint64_t sre = rsr("icc_sre_el1");
	if (!(sre & 1)) {
		wsr("icc_sre_el1", sre | 1);
		isb();
	}
	if (!(rsr("icc_sre_el1") & 1)) {
		con_str("GIC", "sem interface de sysreg, pulei");
		return;
	}

	stage = "gic dist";
	uint32_t dctlr = rd32(GICD_BASE);
	/* Non-secure view (DS=0): bit0 EnableGrp1, bit1 EnableGrp1A, bit4 ARE_NS; Linux writes 0x13 */
	wr32(GICD_BASE, dctlr | 0x13);
	gicd_wait_rwp();
	con_hex2("GICD", "ctlr era=", dctlr, "agora=", rd32(GICD_BASE));
	con_hex2("", "typer=", rd32(GICD_BASE + GICD_TYPER), 0, 0);

	stage = "gic redist";
	uintptr_t r = find_gicr();
	if (!r) {
		con_str("GICR", "nao achei o da CPU de boot");
		return;
	}
	uint32_t waker = rd32(r + GICR_WAKER);
	if (waker & 2) {
		wr32(r + GICR_WAKER, waker & ~2U);
		for (int i = 0; i < 100000 && (rd32(r + GICR_WAKER) & 4); i++)
			;
	}
	con_hex2("GICR", "base=", r, "waker=", rd32(r + GICR_WAKER));

	stage = "gic config";
	wr32(r + GICR_IGROUPR0, 0xffffffff);	/* NS view: secure bits are WI */
	uint32_t en_before = rd32(r + GICR_ISENABLER0);
	for (int id = 0; id < 32; id++)
		wr8(r + GICR_IPRIORITYR + id, 0xa0);
	wr32(r + GICR_ISENABLER0, (1U << VTIMER_INTID) | (1U << SGI_INTID));
	con_hex2("ENABLE", "era=", en_before, "agora=", rd32(r + GICR_ISENABLER0));

	uint64_t ctlr = rsr("icc_ctlr_el1");
	wsr("icc_ctlr_el1", ctlr & ~2ULL);	/* EOImode 0: EOIR also deactivates */
	wsr("icc_pmr_el1", 0xf0);
	wsr("icc_bpr1_el1", 0);
	wsr("icc_igrpen1_el1", 1);
	isb();
	con_hex2("ICC", "ctlr=", ctlr, "grpen1=", rsr("icc_igrpen1_el1"));

	stage = "gic vtimer";
	irq_window("VTIMER", r, VTIMER_INTID, 30);
	wdog_pet();
	stage = "gic sgi";
	irq_window("SGI1", r, SGI_INTID, 10);
	wr32(r + GICR_ICENABLER0, 0xffffffff);
	isb();
}

/* ---- main ---- */

void shim_main(uint64_t dtb)
{
	char b[48], *p;

	(void)dtb;
	ramlog_open();
	ramlog_str("\n=== S9 shim M2 ===\n");

	/* M1.1: 0x9d400000, 1440x2960, stride 5760, B G R A, autorefresh on */
	fb_default();
	con_clear(GREY);
	con_title("SHIM M2: CPU");
	stage = "watchdog";
	wdog_arm();

	test_features();
	con_skip();
	wdog_pet();
	test_smp();
	con_skip();
	wdog_pet();
	test_gic();
	wdog_pet();

	stage = "fim";
	con_skip();
	con_str("", "Tire uma foto desta tela!");
	int row = con_row();
	uint64_t f = ticks_per_s(), t0 = now();
	for (unsigned s = 0; s <= RUN_SECONDS; s++) {
		while (now() - t0 < f * s)
			;
		wdog_pet();
		p = put(put_dec(b, s), "s  reinicia em ");
		put(put_dec(p, RUN_SECONDS - s), "s  ");
		con_set_row(row);
		con_str("TEMPO", b);
	}
	reboot();
}
