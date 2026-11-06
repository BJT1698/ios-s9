/*
 * Shared by the shim stages: PSCI calls, the generic timer, the APSS
 * watchdog (programmed like drivers/soc/qcom/watchdog_v2.c) and the ways a
 * stage ends: a countdown, or a blue panic screen, then a reboot.
 */
#include "hw.h"
#include "fb.h"
#include "con.h"
#include "plat.h"

#define APSS_WDT		0x17980000UL	/* qcom,wdt@17980000 */
#define WDT0_RST		0x04
#define WDT0_EN			0x08
#define WDT0_BARK_TIME		0x10
#define WDT0_BITE_TIME		0x14
#define WDT_HZ			32765
#define WDT_BARK_S		15		/* bite 3 s later, as watchdog_v2 does */

#define PSCI_SYSTEM_RESET	0x84000009UL
#define PANIC_SECONDS		60

static volatile int in_panic;

uint64_t smc(uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3)
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

uint64_t now(void)
{
	isb();
	return rsr("cntpct_el0");
}

uint64_t ticks_per_s(void)
{
	uint64_t f = rsr("cntfrq_el0");
	return f ? f : 19200000;	/* Qualcomm's 19.2 MHz if firmware left it 0 */
}

void delay_ms(unsigned ms)
{
	uint64_t end = now() + ticks_per_s() / 1000 * ms;
	while (now() < end)
		;
}

uint32_t ticks_to_us(uint64_t ticks)
{
	return ticks * 1000000 / ticks_per_s();
}

void wdog_pet(void)
{
	wr32(APSS_WDT + WDT0_RST, 1);
}

void wdog_arm(void)
{
	wr32(APSS_WDT + WDT0_EN, 0);
	wr32(APSS_WDT + WDT0_BARK_TIME, WDT_BARK_S * WDT_HZ);
	wr32(APSS_WDT + WDT0_BITE_TIME, (WDT_BARK_S + 3) * WDT_HZ);
	wr32(APSS_WDT + WDT0_EN, 1);
	wdog_pet();
}

void reboot(void)
{
	smc(PSCI_SYSTEM_RESET, 0, 0, 0);
	for (;;)
		__asm__ volatile("wfe");	/* the watchdog bites */
}

/* regs: x0-x28, a pad word, then the x29/x30 the vector stashed */
void panic_screen(const char *stage, uint64_t slot, uint64_t *regs)
{
	static const char kind[4][8] = { "SYNC", "IRQ", "FIQ", "SERROR" };
	char b[48];

	if (in_panic++)
		reboot();	/* a fault while drawing the last one */
	con_clear(PANICBG);
	con_title("PANICO NO SHIM");
	put_dec(put(put(b, kind[slot & 3]), " slot "), slot);
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

void countdown_reboot(int row, unsigned secs)
{
	uint64_t f = ticks_per_s(), t0 = now();
	char b[48], *p;

	for (unsigned s = 0; s <= secs; s++) {
		while (now() - t0 < f * s)
			;
		wdog_pet();
		p = put(put_dec(b, s), "s  reinicia em ");
		put(put_dec(p, secs - s), "s  ");
		con_set_row(row);
		con_str("TEMPO", b);
	}
	reboot();
}
