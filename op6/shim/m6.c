/*
 * Galaxy S9 milestone 6: CPU clocks before tuning XNU for speed.
 * The iOS GUI renders on the CPU, and nothing in XNU (or ios-sweet) ever sets
 * a CPU frequency: the cores run at whatever ABL left. This stage measures
 * that, then sets the clocks the way Linux's clk-cpu-osm does, and measures
 * again.
 *   - OSM (operating state manager, v2 on this SDM845): XBL already loaded the
 *     frequency table (FREQ_REG, 40 rows of 32 bytes) and the OSM firmware
 *     handles voltage by itself. Linux only writes a row index to
 *     DCVS_PERF_STATE_DESIRED (0x920 + 4 * core when CORE_DCVS_CTRL bit 0 says
 *     per-core, else core 0) and polls PSTATE_STATUS.
 *   - qcom,mx-turbo-freq: above 1478.4 MHz (L3) and 1689.6 MHz (silver) Linux
 *     also votes the MX rail to turbo over RPMh. We make no RPMh votes, so
 *     those two stay at or below that; gold has no limit (3300 MHz).
 *   - Speed: a chain of dependent adds (1 per cycle on A55 and A75) timed
 *     against the 19.2 MHz counter, on CPU0 and on CPU4 (gold, PSCI CPU_ON with
 *     CPU0's page tables); memory: 4 MB copies (beyond the 2 MB L3), and
 *     writes/reads of the framebuffer (non-cacheable, like XNU's panel copy).
 * Source: drivers/clk/qcom/clk-cpu-osm.c of the Samsung SDM845 kernel.
 */
#include <stdint.h>
#include "hw.h"
#include "fb.h"
#include "con.h"
#include "mmu.h"
#include "plat.h"

#define END_SECONDS	60

#define OSM_L3		0x17d41000UL
#define OSM_PWR		0x17d43000UL	/* silver, CPU0-3 */
#define OSM_PERF	0x17d45800UL	/* gold, CPU4-7 */
#define OSM_ENABLE	0x0
#define OSM_DCVS_CTRL	0xbc
#define OSM_FREQ(i)	(0x110 + 32 * (i))
#define OSM_PSTATE	0x700
#define OSM_DESIRED(c)	(0x920 + 4 * (c))
#define OSM_ROWS	40

#define XO_KHZ		19200
#define L3_MAX_MHZ	1478		/* qcom,mx-turbo-freq, no MX vote */
#define PWR_MAX_MHZ	1689
#define PERF_MAX_MHZ	3300

#define PSCI_CPU_ON	0xc4000003UL
#define PSCI_CPU_OFF	0x84000002UL
#define GOLD_CPU	4

#define SPIN_LOOPS	200000		/* x 64 adds */
#define BUF_SIZE	(4UL << 20)
#define FB_TEST_Y	2300
#define FB_TEST_ROWS	600

static const char *stage;	/* set at runtime: no absolute pointers in data */
static int step_row;
static uint8_t buf_a[BUF_SIZE] __attribute__((aligned(64)));
static uint8_t buf_b[BUF_SIZE] __attribute__((aligned(64)));

/* CPU0's translation registers for the gold core, cleaned to RAM. */
static struct {
	uint64_t mair, tcr, ttbr, sctlr;
} __attribute__((aligned(64))) mmu_regs;

/* Written by the gold core with the MMU on (coherent with CPU0). */
static volatile struct {
	uint64_t done, spin_ticks, copy_ticks, midr;
} __attribute__((aligned(64))) gold;

extern char secondary_entry[];

void exc_handler(uint64_t slot, uint64_t *regs)
{
	panic_screen(stage, slot, regs);
}

/* Overwrites one line: if the shim hangs, the photo shows the last step. */
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

/* ---- measurements ---- */

static uint64_t spin_ticks(void)
{
	uint64_t n = SPIN_LOOPS, acc = 0, t = now();

	__asm__ volatile(
		"1:\n"
		".rept 64\n"
		"add %1, %1, #1\n"
		".endr\n"
		"subs %0, %0, #1\n"
		"b.ne 1b\n"
		: "+r"(n), "+r"(acc) : : "cc");
	return now() - t;
}

/* 64 bytes per round with general registers only (no SIMD in the shim). */
static void copy64(void *dst, const void *src, uint64_t bytes)
{
	__asm__ volatile(
		"1:\n"
		"ldp x4, x5, [%1], #16\n"
		"ldp x6, x7, [%1], #16\n"
		"ldp x8, x9, [%1], #16\n"
		"ldp x10, x11, [%1], #16\n"
		"stp x4, x5, [%0], #16\n"
		"stp x6, x7, [%0], #16\n"
		"stp x8, x9, [%0], #16\n"
		"stp x10, x11, [%0], #16\n"
		"subs %2, %2, #64\n"
		"b.ne 1b\n"
		: "+r"(dst), "+r"(src), "+r"(bytes)
		: : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "cc", "memory");
}

static uint64_t copy_ticks(void)
{
	uint64_t t = now();

	for (int i = 0; i < 4; i++)
		copy64(i & 1 ? buf_a : buf_b, i & 1 ? buf_b : buf_a, BUF_SIZE);
	return now() - t;
}

static uint32_t mhz(uint64_t ticks)
{
	return ticks ? (uint32_t)((uint64_t)SPIN_LOOPS * 64 * XO_KHZ / 1000 / ticks) : 0;
}

/* MB/s for bytes moved in ticks (copies count read + write once: bytes copied). */
static uint32_t mbs(uint64_t bytes, uint64_t ticks)
{
	uint32_t us = ticks_to_us(ticks);

	return us ? (uint32_t)(bytes / us) : 0;
}

static uint64_t fb_write_ticks(void)
{
	uint64_t pat = 0x0040404000404040ULL;	/* dark grey, both B G R A pixels */
	uint64_t t = now();

	for (int y = FB_TEST_Y; y < FB_TEST_Y + FB_TEST_ROWS; y++) {
		volatile uint64_t *row = (volatile uint64_t *)(fb.base + (uint64_t)y * fb.stride);
		for (uint32_t x = 0; x < fb.width / 2; x++)
			row[x] = pat;
	}
	dsb();
	return now() - t;
}

static uint64_t fb_read_ticks(uint64_t *sum)
{
	uint64_t s = 0, t = now();

	for (int y = FB_TEST_Y; y < FB_TEST_Y + FB_TEST_ROWS; y++) {
		volatile uint64_t *row = (volatile uint64_t *)(fb.base + (uint64_t)y * fb.stride);
		for (uint32_t x = 0; x < fb.width / 2; x++)
			s += row[x];
	}
	*sum = s;
	return now() - t;
}

/* ---- gold core ---- */

static void clean_range(uintptr_t a, uintptr_t end)
{
	for (a &= ~63UL; a < end; a += 64)
		__asm__ volatile("dc civac, %0" : : "r"(a) : "memory");
	dsb();
}

void secondary_main(uint64_t idx)
{
	(void)idx;
	/* MMU off here: mmu_regs comes straight from RAM (CPU0 cleaned it). */
	__asm__ volatile("msr mair_el1, %0" : : "r"(mmu_regs.mair));
	__asm__ volatile("msr tcr_el1, %0" : : "r"(mmu_regs.tcr));
	__asm__ volatile("msr ttbr0_el1, %0" : : "r"(mmu_regs.ttbr));
	__asm__ volatile("isb; tlbi vmalle1; dsb nsh; isb" ::: "memory");
	__asm__ volatile("ic iallu; dsb nsh; isb" ::: "memory");
	__asm__ volatile("msr sctlr_el1, %0; isb" : : "r"(mmu_regs.sctlr) : "memory");

	gold.midr = rsr("midr_el1");
	gold.spin_ticks = spin_ticks();
	gold.copy_ticks = copy_ticks();
	dsb();
	gold.done = 1;
	dsb();
	smc(PSCI_CPU_OFF, 0, 0, 0);
}

/* Returns the PSCI result; 0 and gold.done = ran. */
static int64_t run_gold(void)
{
	gold.done = 0;
	gold.spin_ticks = gold.copy_ticks = gold.midr = 0;
	dsb();
	int64_t ret = (int64_t)smc(PSCI_CPU_ON, (uint64_t)GOLD_CPU << 8, (uintptr_t)secondary_entry, GOLD_CPU);
	uint64_t end = now() + 5 * ticks_per_s();

	while (ret == 0 && !gold.done && now() < end)
		;
	wdog_pet();
	delay_ms(20);		/* let it reach CPU_OFF before the next CPU_ON */
	return ret;
}

/* ---- OSM ---- */

struct cluster {
	const char *name;
	uintptr_t base;
	uint32_t max_mhz;
	int n;			/* rows before the repeated end row */
	uint32_t khz[OSM_ROWS];
	uint8_t lval[OSM_ROWS], cc[OSM_ROWS];
};

static void osm_read_lut(struct cluster *c)
{
	c->n = OSM_ROWS;
	for (int i = 0; i < OSM_ROWS; i++) {
		uint32_t v = rd32(c->base + OSM_FREQ(i));
		c->lval[i] = v & 0xff;
		c->cc[i] = (v >> 16) & 7;
		c->khz[i] = (v >> 30) ? XO_KHZ * c->lval[i] : 300000;
		if (i > 0 && c->khz[i] == c->khz[i - 1] && c->cc[i] == c->cc[i - 1]) {
			c->n = i;
			break;
		}
	}
}

static void osm_show(const struct cluster *c)
{
	char b[300], *p = b;
	uint32_t ctrl = rd32(c->base + OSM_DCVS_CTRL);

	p = put_hex(put(p, "en="), rd32(c->base + OSM_ENABLE), 1);
	p = put_dec(put(p, " porcore="), ctrl & 1);
	p = put_dec(put(p, " desej="), rd32(c->base + OSM_DESIRED(0)));
	p = put_dec(put(p, ","), rd32(c->base + OSM_DESIRED(1)));
	p = put_dec(put(p, " agora="), (rd32(c->base + OSM_PSTATE) >> 16) & 0xff);
	put(put_dec(put(p, "x19.2="), ((rd32(c->base + OSM_PSTATE) >> 16) & 0xff) * XO_KHZ / 1000), "MHz");
	con_str(c->name, b);
	p = b;
	for (int i = 0; i < c->n; i++) {
		p = put_dec(p, c->khz[i] / 1000);
		if (c->cc[i] != 4)
			p = put_dec(put(p, "c"), c->cc[i]);
		p = put(p, " ");
	}
	con_str("", b);
}

/* Highest row within the MX limit, preferring rows for all 4 cores. */
static int osm_pick(const struct cluster *c)
{
	int best = -1;

	for (int i = 0; i < c->n; i++)
		if (c->khz[i] / 1000 <= c->max_mhz && c->cc[i] == 4 &&
		    (best < 0 || c->khz[i] >= c->khz[best]))
			best = i;
	return best;
}

static void osm_set(const struct cluster *c, int idx)
{
	char b[64], *p;
	/* L3 has one vote register in Linux, whatever CORE_DCVS_CTRL says. */
	int per_core = c->base != OSM_L3 && (rd32(c->base + OSM_DCVS_CTRL) & 1);
	uint64_t t = now(), end = t + ticks_per_s() / 50;	/* 20 ms */
	uint32_t lval;

	if (idx < 0) {
		con_str(c->name, "sem linha valida, nao mexi");
		return;
	}
	for (int core = 0; core < (per_core ? 4 : 1); core++)
		wr32(c->base + OSM_DESIRED(core), (uint32_t)idx);
	dsb();
	do
		lval = (rd32(c->base + OSM_PSTATE) >> 16) & 0xff;
	while (lval != c->lval[idx] && now() < end);
	p = put_dec(put(b, "linha "), (uint32_t)idx);
	p = put_dec(put(p, " = "), c->khz[idx] / 1000);
	p = put_dec(put(p, " MHz, agora lval="), lval);
	put(put_dec(put(p, " em "), ticks_to_us(now() - t)), "us");
	con_str(c->name, b);
}

/* ---- report ---- */

static void measure(const char *when)
{
	char b[96], *p;
	uint64_t sum;

	step(when);
	stage = "medindo cpu0";
	uint64_t s0 = spin_ticks();
	uint64_t c0 = copy_ticks();
	wdog_pet();
	stage = "framebuffer";
	uint64_t fw = fb_write_ticks();
	uint64_t fr = fb_read_ticks(&sum);
	wdog_pet();
	stage = "cpu4 ouro";
	int64_t ret = run_gold();

	p = put_dec(put(b, "cpu0 "), mhz(s0));
	p = put_dec(put(p, " MHz, copia "), mbs(4 * BUF_SIZE, c0));
	put(p, " MB/s");
	con_str(when, b);
	if (ret != 0 || !gold.done) {
		p = put_hex(put(b, "cpu4 nao rodou ret="), (uint64_t)ret, 8);
		con_str("", b);
	} else {
		p = put_dec(put(b, "cpu4 "), mhz(gold.spin_ticks));
		p = put_dec(put(p, " MHz, copia "), mbs(4 * BUF_SIZE, gold.copy_ticks));
		put_hex(put(p, " MB/s midr="), gold.midr, 8);
		con_str("", b);
	}
	uint64_t fb_bytes = (uint64_t)FB_TEST_ROWS * fb.width * 4;
	p = put_dec(put(b, "tela escreve "), mbs(fb_bytes, fw));
	p = put_dec(put(p, " MB/s, le "), mbs(fb_bytes, fr));
	put(p, " MB/s");
	con_str("", b);
}

void shim_main(uint64_t dtb)
{
	struct mmu_info mi;
	struct cluster cl[3] = {
		{ .name = "L3", .base = OSM_L3, .max_mhz = L3_MAX_MHZ },
		{ .name = "PRATA", .base = OSM_PWR, .max_mhz = PWR_MAX_MHZ },
		{ .name = "OURO", .base = OSM_PERF, .max_mhz = PERF_MAX_MHZ },
	};
	char b[64];

	(void)dtb;
	/* M1.1: 0x9d400000, 1440x2960, stride 5760, B G R A, autorefresh on */
	fb_default();
	con_clear(GREY);
	con_title("SHIM M6: CLOCK DAS CPUS");
	stage = "watchdog";
	wdog_arm();
	step_row = con_row();
	con_skip();

	step("mmu");
	stage = "mmu";
	if (mmu_enable(&mi) != 0) {
		con_str("ERRO", "mmu");
		goto out;
	}
	mmu_regs.mair = rsr("mair_el1");
	mmu_regs.tcr = rsr("tcr_el1");
	mmu_regs.ttbr = rsr("ttbr0_el1");
	mmu_regs.sctlr = rsr("sctlr_el1");
	clean_range((uintptr_t)&mmu_regs, (uintptr_t)&mmu_regs + sizeof(mmu_regs));

	step("le tabelas do OSM");
	stage = "osm leitura";
	for (int i = 0; i < 3; i++) {
		osm_read_lut(&cl[i]);
		osm_show(&cl[i]);
	}
	wdog_pet();
	con_skip();

	measure("ANTES");
	con_skip();

	step("sobe os clocks");
	stage = "osm escrita";
	for (int i = 0; i < 3; i++)
		osm_set(&cl[i], osm_pick(&cl[i]));
	wdog_pet();
	delay_ms(10);

	measure("DEPOIS");
	con_skip();
	for (int i = 0; i < 3; i++) {
		char *p = put_dec(put(b, "agora lval="), (rd32(cl[i].base + OSM_PSTATE) >> 16) & 0xff);
		put_dec(put(p, " desej="), rd32(cl[i].base + OSM_DESIRED(0)));
		con_str(cl[i].name, b);
	}
out:
	stage = "fim";
	step("fim");
	con_skip();
	con_str("", "Tire uma foto desta tela!");
	countdown_reboot(con_row(), END_SECONDS);
}
