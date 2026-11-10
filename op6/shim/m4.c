/*
 * Galaxy S9 milestone 4: boot XNU. The ramdisk of the boot image holds the
 * SWEETXNU payload (tools/mkpayload.py: kernel + ADT + boot-args); loader.c
 * plays iBoot and jumps to it with the MMU off. The APSS watchdog stays off:
 * XNU does not feed it.
 * OnePlus 6: whether ABL leaves the APSS watchdog armed is not established,
 * so it is switched off explicitly right before the jump (EN = 0, as
 * watchdog_v2.c's disable); the S9 relied on ABL leaving it off.
 * OnePlus 6 touch: ABL leaves the Synaptics S3706B unpowered and XNU does
 * not power it (patch 0105 only probes 0x20). Before the jump, m4 repeats
 * the power-up shim M5 proved on the phone: GPIO88 high (vio 1.8 V), RPMh
 * ACTIVE vote ldoa28 3008 mV HPM on, reset pulse on GPIO99, 200 ms, then a
 * probe of 0x20 on the bit-banged pins (SDA 49, SCL 50), GPIO125 (attention)
 * left as input pull-up. Every step is logged on screen and in the ramlog;
 * a failure is logged and the boot goes on to XNU.
 */
#include <stdint.h>
#include "hw.h"
#include "fb.h"
#include "con.h"
#include "loader.h"
#include "plat.h"
#include "ramlog.h"
#include "i2c_bb.h"
#include "rpmh.h"

#define RUN_SECONDS	90
#define APSS_WDT	0x17980000UL	/* qcom,wdt@17980000 (plat.c) */
#define WDT0_EN		0x08

#define TS_SDA		49
#define TS_SCL		50
#define TS_IRQ		125
#define TS_RESET	99		/* active low */
#define TS_VIO_EN	88
#define TS_ADDR		0x20
#define TS_HALF_US	5
#define TS_INT_CTL	(TLMM_PULL_UP | 7u << 6)	/* gpio, pull-up, 16 mA, input */
#define TS_STARTUP_MS	200				/* syna,startup-delay-ms */
#define LDO28_MV	3008
#define VRM_MODE_HPM	7

static const char *stage;	/* set at runtime: no absolute pointers in data */

void exc_handler(uint64_t slot, uint64_t *regs)
{
	panic_screen(stage, slot, regs);
}

static void say(const char *label, const char *s)
{
	con_str(label, s);
	ramlog_str(label);
	ramlog_str(": ");
	ramlog_str(s);
	ramlog_str("\n");
}

/* 0 = ACK. Address-only write, up to three tries (m5.c probe). */
static int ts_probe(const struct bb *bus, const char *what)
{
	char b[64], *p = put(put(b, what), ": ");
	int err = E_SCL;

	if (!bb_sda(bus) && bb_scl(bus)) {
		p = put_dec(put(p, "rec="), (uint32_t)bb_recover(bus));
		p = put(p, " ");
	}
	if (bb_sda(bus) && bb_scl(bus))
		for (int i = 0; i < 3 && err; i++) {
			err = bb_probe(bus, TS_ADDR);
			if (err)
				delay_ms(2);
		}
	p = put(p, err == 0 ? "ACK" : err == E_ADDR ? "NACK" : "linha/SCL?");
	p = put_dec(put(p, " sda="), bb_sda(bus));
	p = put_dec(put(p, " scl="), bb_scl(bus));
	put_dec(put(p, " irq="), tlmm_in(TS_IRQ));
	say("TOUCH", b);
	return err;
}

/* The m5.c power-up that made 0x20 answer: vio, vdd, reset 1/0/1, 200 ms. */
static void touch_power(void)
{
	struct bb bus;
	char b[80], *p;
	uint32_t addr = 0;
	int rc;

	ramlog_str("m4: touch power-up\n");
	if (bb_init(&bus, TS_SDA, TS_SCL, TS_HALF_US)) {
		say("TOUCH", "pinos fora da tabela");
		return;
	}
	wr32(tlmm_ctl(TS_IRQ), TS_INT_CTL);
	if (ts_probe(&bus, "inicial") == 0)
		return;				/* already powered: leave it alone */

	uintptr_t vio = tlmm_ctl(TS_VIO_EN);
	wr32(vio + TLMM_IO, 2);
	wr32(vio, (rd32(vio) & ~TLMM_FUNC_MASK) | TLMM_OE);
	delay_ms(10);

	rc = rh_init(b);
	say("RPMH", b);
	if (!rc) {
		rc = rh_lookup("ldoa28", RH_VRM, &addr);
		p = put_hex(put(b, "ldoa28 addr="), addr, 5);
		put_dec(put(p, " rc="), (uint32_t)-rc);
		say("RPMH", b);
	}
	if (!rc) {
		rc = rh_vrm_on(addr, LDO28_MV, VRM_MODE_HPM);
		p = put_dec(put(b, "3008 mV modo 7 on: rc="), (uint32_t)-rc);
		say("RPMH", b);
	}
	if (rc)
		say("TOUCH", "voto ldoa28 falhou; segue sem vdd");
	delay_ms(10);

	uintptr_t rst = tlmm_ctl(TS_RESET);
	wr32(rst + TLMM_IO, 2);
	wr32(rst, TS_INT_CTL | TLMM_OE);
	delay_ms(10);
	wr32(rst + TLMM_IO, 0);
	delay_ms(10);
	wr32(rst + TLMM_IO, 2);			/* left driving 1 */
	delay_ms(TS_STARTUP_MS);
	if (ts_probe(&bus, "ligado") == 0)
		return;
	delay_ms(300);
	if (ts_probe(&bus, "ligado +300ms") != 0)
		say("TOUCH", "sem ACK em 0x20; segue para o XNU");
}

void shim_main(uint64_t dtb)
{
	/* OP6 M1: 0x9d400000, 1080x2280, stride 4320, B G R A, autorefresh every TE */
	fb_default();
	con_clear(GREY);
	con_title("SHIM M4: XNU");
	con_hex2("SHIM", "dtb=", dtb, "img=", (uint32_t)(uintptr_t)shim_main);

	con_hex("WDT EN", rd32(APSS_WDT + WDT0_EN));	/* what ABL left: 1 = armed */
	wr32(APSS_WDT + WDT0_EN, 0);

	ramlog_open();
	ramlog_str("\n=== OP6 shim M4 ===\n");
	stage = "touch";
	touch_power();			/* never fatal: logs and returns */

	stage = "loader";
	const char *why = loader_boot(dtb);	/* returns only if it could not jump */
	con_str("LOADER", why);
	con_skip();
	con_str("", "Tire uma foto desta tela!");
	countdown_reboot(con_row(), RUN_SECONDS);
}
