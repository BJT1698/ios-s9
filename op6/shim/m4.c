/*
 * Galaxy S9 milestone 4: boot XNU. The ramdisk of the boot image holds the
 * SWEETXNU payload (tools/mkpayload.py: kernel + ADT + boot-args); loader.c
 * plays iBoot and jumps to it with the MMU off. The APSS watchdog stays off:
 * XNU does not feed it.
 * OnePlus 6: whether ABL leaves the APSS watchdog armed is not established,
 * so it is switched off explicitly right before the jump (EN = 0, as
 * watchdog_v2.c's disable); the S9 relied on ABL leaving it off.
 */
#include <stdint.h>
#include "hw.h"
#include "fb.h"
#include "con.h"
#include "loader.h"
#include "plat.h"

#define RUN_SECONDS	90
#define APSS_WDT	0x17980000UL	/* qcom,wdt@17980000 (plat.c) */
#define WDT0_EN		0x08

static const char *stage;	/* set at runtime: no absolute pointers in data */

void exc_handler(uint64_t slot, uint64_t *regs)
{
	panic_screen(stage, slot, regs);
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

	stage = "loader";
	const char *why = loader_boot(dtb);	/* returns only if it could not jump */
	con_str("LOADER", why);
	con_skip();
	con_str("", "Tire uma foto desta tela!");
	countdown_reboot(con_row(), RUN_SECONDS);
}
