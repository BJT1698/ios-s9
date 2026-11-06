/*
 * Galaxy S9 milestone 1 (rev 1.1): prove the shim runs and can draw on the
 * panel ABL left on.
 *
 * ios-sweet's panel is a video-mode panel that reads the framebuffer by
 * itself. The S9's S6E3HA8 is a DSC *command-mode* panel (stock DTB): it
 * keeps its own copy of the picture, so pixels we write only reach the
 * glass when the display pipe sends a new frame (see mdss.c).
 *
 * Nothing here may leave us blind again: every step goes to the RAM log
 * before it runs (ramlog.c), the APSS watchdog turns a hang into a reset
 * after ~18 s, and the run ends in a reboot that asks for recovery, where
 * the log is read back. Painting comes first, before any device register.
 */
#include <stdint.h>
#include "hw.h"
#include "fb.h"
#include "fdt.h"
#include "mdss.h"
#include "ramlog.h"
#include "screen.h"

#define IMEM_RESTART_REASON	0x146bf65cUL	/* qcom,msm-imem@146bf000 restart_reason@65c */
#define RESTART_REASON_RECOVERY	0x77665502U	/* include/linux/sec_debug.h */
#define PSHOLD			0x0c264000UL	/* qcom,pshold */

#define APSS_WDT		0x17980000UL	/* qcom,wdt@17980000 */
#define WDT0_RST		0x04		/* drivers/soc/qcom/watchdog_v2.c */
#define WDT0_EN			0x08
#define WDT0_BARK_TIME		0x10
#define WDT0_BITE_TIME		0x14
#define WDT_HZ			32765
#define WDT_BARK_S		15		/* bite 3 s later, as watchdog_v2 does */

#define PSCI_VERSION		0x84000000UL
#define PSCI_SYSTEM_RESET	0x84000009UL

#define RUN_SECONDS		60
#define PANIC_SECONDS		30

extern char _head[];

static struct m1_info info;
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
	return f ? f : 19200000;	/* Qualcomm's 19.2 MHz if firmware left it 0 */
}

static void delay_s(unsigned s)
{
	uint64_t end = now() + ticks_per_s() * s;
	while (now() < end)
		;
}

static void wdog_pet(void)
{
	wr32(APSS_WDT + WDT0_RST, 1);
}

/* Same sequence as watchdog_v2.c: from here a hang ends in a reset. */
static void wdog_arm(void)
{
	wr32(APSS_WDT + WDT0_EN, 0);
	wr32(APSS_WDT + WDT0_BARK_TIME, WDT_BARK_S * WDT_HZ);
	wr32(APSS_WDT + WDT0_BITE_TIME, (WDT_BARK_S + 3) * WDT_HZ);
	wr32(APSS_WDT + WDT0_EN, 1);
	wdog_pet();
}

/*
 * Reboot asking for recovery (TWRP). Samsung's ABL may only look at the
 * PMIC copy of the reason, which needs SPMI; the IMEM cookie is a free try.
 */
static void __attribute__((noreturn)) reboot(void)
{
	ramlog_str("reboot: pede recovery e chama PSCI SYSTEM_RESET\n");
	/* OP6: plain reset, ABL boots the normal slot (pmOS) */
	dsb();
	smc(PSCI_SYSTEM_RESET, 0, 0, 0);
	ramlog_str("PSCI voltou, tentando PS_HOLD\n");
	wr32(PSHOLD, 0);
	for (;;)
		__asm__ volatile("wfe");	/* the watchdog bites */
}

/* regs: x0-x28, a pad word, then the x29/x30 the vector stashed */
void exc_handler(uint64_t slot, uint64_t *regs)
{
	if (in_exc++)
		reboot();	/* a fault while handling a fault: get out */
	ramlog_str("EXCECAO slot=");
	ramlog_dec(slot);
	ramlog_str(" ESR=");
	ramlog_hex(rsr("esr_el1"), 8);
	ramlog_str(" ELR=");
	ramlog_hex(rsr("elr_el1"), 9);
	ramlog_str(" FAR=");
	ramlog_hex(rsr("far_el1"), 9);
	ramlog_str(" LR=");
	ramlog_hex(regs[31], 9);
	ramlog_str(" IMG=");
	ramlog_hex(info.image, 9);
	ramlog_str("\n");
	screen_panic(slot, rsr("esr_el1"), rsr("elr_el1"), rsr("far_el1"), regs[31]);
	mdss_kick(&info.mdss);
	for (int s = 0; s < PANIC_SECONDS; s++) {
		wdog_pet();
		delay_s(1);
	}
	reboot();
}

static void log_hex_field(const char *label, uint64_t v)
{
	ramlog_str(label);
	ramlog_hex(v, 8);
}

static void log_mdss(const struct mdss_state *m)
{
	log_hex_field("MDSS ver=", m->hw_version);
	ramlog_str("\nCTL_TOP:");
	for (int i = 0; i < MDSS_NCTL; i++)
		log_hex_field(" ", m->ctl_top[i]);
	ramlog_str("\nPP autorefresh:");
	for (int i = 0; i < MDSS_NPP; i++)
		log_hex_field(" ", m->pp_autorefresh[i]);
	ramlog_str("\n");
	for (int i = 0; i < MDSS_NSSPP; i++) {
		const struct mdss_sspp *p = &m->sspp[i];
		ramlog_str("SSPP");
		ramlog_dec(i);
		log_hex_field(" size=", p->size);
		log_hex_field(" addr=", p->addr);
		log_hex_field(" stride=", p->ystride);
		log_hex_field(" fmt=", p->format);
		log_hex_field(" unpack=", p->unpack);
		ramlog_str("\n");
	}
	ramlog_str("escolhas (0 = nenhum): ctl=");
	ramlog_dec(m->ctl + 1);
	ramlog_str(" cmd=");
	ramlog_dec(m->cmd_mode);
	ramlog_str(" autorefresh=");
	ramlog_dec(m->autorefresh);
	ramlog_str(" sspp=");
	ramlog_dec(m->sspp_idx + 1);
	ramlog_str("\n");
}

void shim_main(uint64_t dtb)
{
	info.mdss.ctl = -1;
	info.mdss.sspp_idx = -1;
	info.dtb = dtb;
	info.image = (uintptr_t)_head;

	/* 0. Flight recorder: plain RAM writes, nothing here can hang. */
	ramlog_open();
	ramlog_str("\n=== OP6 shim M1 ===\nIMG=");
	ramlog_hex(info.image, 9);
	ramlog_str(" DTB=");
	ramlog_hex(dtb, 9);
	ramlog_str(" EL=");
	ramlog_dec((rsr("CurrentEL") >> 2) & 3);
	ramlog_str(" CNTFRQ=");
	ramlog_dec(rsr("cntfrq_el0"));
	ramlog_str("\n");

	/* 1. Proof of life with safe defaults, before any device register. */
	fb_default();
	fb_rect(0, 0, fb.width, fb.height, GREY);
	fb_text(32, 32, 6, "OP6 SHIM M1 OK!", YELLOW, GREY);
	ramlog_str("pintou (layout padrao do GOP)\n");

	/* 2. From here on a hang turns into a reset after ~18 s. */
	ramlog_str("watchdog: lendo EN\n");
	info.wdog_en = rd32(APSS_WDT + WDT0_EN);
	wdog_arm();
	log_hex_field("watchdog: EN era ", info.wdog_en);
	ramlog_str(", armado\n");

	info.el = (rsr("CurrentEL") >> 2) & 3;
	info.midr = rsr("midr_el1");
	info.mmfr0 = rsr("id_aa64mmfr0_el1");
	ramlog_str("lendo DTB\n");
	fdt_root_str(dtb, "model", info.model, sizeof(info.model));
	fdt_bootarg(dtb, "msm_drm.dsi_display0=", info.panel, sizeof(info.panel));
	fdt_bootarg(dtb, "lcd_id=0x", info.lcd_id, sizeof(info.lcd_id));
	ramlog_str("modelo: ");
	ramlog_str(info.model);
	ramlog_str("\npainel: ");
	ramlog_str(info.panel);
	ramlog_str("\nlcd_id: 0x");
	ramlog_str(info.lcd_id);
	log_hex_field("\nMIDR=", info.midr);
	log_hex_field(" MMFR0=", info.mmfr0);
	ramlog_str("\n");
	wdog_pet();

	/* 3. The display pipe ABL left running. */
	mdss_probe(&info.mdss);
	log_mdss(&info.mdss);
	info.fb_from_hw = mdss_fb_from_sspp(&info.mdss) == 0;
	info.fb_addr = fb.base;
	ramlog_str("FB=");
	ramlog_hex(fb.base, 9);
	ramlog_str(info.fb_from_hw ? " (do HW) " : " (padrao) ");
	ramlog_dec(fb.width);
	ramlog_str("x");
	ramlog_dec(fb.height);
	ramlog_str(" stride=");
	ramlog_dec(fb.stride);
	ramlog_str(" bpp=");
	ramlog_dec(fb.bpp);
	ramlog_str("\nPSCI: versao\n");
	info.psci = smc(PSCI_VERSION, 0, 0, 0);
	log_hex_field("PSCI=", info.psci);
	ramlog_str("\n");
	wdog_pet();

	/* 4. Full screen, then make the panel take it. */
	screen_draw(&info);
	ramlog_str("desenhou a tela, kick\n");
	mdss_kick(&info.mdss);
	ramlog_str("kick feito\n");

	/* 5. Heartbeat: if the count moves on the glass, every new frame gets through. */
	uint64_t f = ticks_per_s(), t0 = now();
	for (unsigned s = 0; s <= RUN_SECONDS; s++) {
		while (now() - t0 < f * s)
			;
		wdog_pet();
		screen_tick(s, RUN_SECONDS);
		mdss_kick(&info.mdss);
		if (s % 10 == 0) {
			ramlog_str("t=");
			ramlog_dec(s);
			ramlog_str("s\n");
		}
	}
	reboot();
}
