#pragma once
#include <stdint.h>
#include "mdss.h"

struct m1_info {
	uintptr_t dtb, image, fb_addr;
	uint64_t el, midr, mmfr0, psci;
	char model[48], panel[64], lcd_id[16];
	struct mdss_state mdss;
	int fb_from_hw;		/* fb layout read from the scanout pipe */
	uint32_t wdog_en;	/* APSS watchdog WDT0_EN as ABL left it */
};

void screen_draw(const struct m1_info *in);
void screen_tick(unsigned secs, unsigned total);
void screen_panic(uint64_t slot, uint64_t esr, uint64_t elr, uint64_t far, uint64_t lr);
