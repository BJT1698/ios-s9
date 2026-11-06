/*
 * Galaxy S9 milestone 8: is the lower RAM really free, so iOS can have ~3.2 GB?
 * XNU manages one contiguous block and today gets only the top 2 GB
 * ([0x100000000, 0x17e1c0000)). The lower 2 GB is full of Qualcomm/Samsung
 * carveouts (modem, hyp, IPA firmware, SPSS, the framebuffer, debug), BUT the
 * reserved-memory map of the live DTB leaves two big holes that look free:
 *   R1 [0xa1800000, 0xc0000000)  487 MiB
 *   R2 [0xd4000000, 0xff000000)  688 MiB
 * If both are real, writable, independent RAM (not aliases, not secure
 * regions that fault/hang), iOS can span [0xa1800000, 0x17e1c0000) with the
 * two carveouts in between (0xc0000000-0xd4000000 IPA, 0xff000000-0x100000000
 * SPSS) marked reserved -> ~3.2 GB. This stage PROVES that on the hardware
 * before any iOS change: MMU off (all Device, so a secure region faults into
 * the on-screen panic or the watchdog reboots, never a silent brick).
 *
 * Method: write an address-derived value at every 16 MiB sample across both
 * regions, then read them ALL back. Matching everywhere = real independent
 * RAM. A mismatch = aliasing/not-RAM. A fault = secure region (panic screen
 * names the address). A hang = watchdog, last PASSO on screen.
 */
#include <stdint.h>
#include "hw.h"
#include "fb.h"
#include "con.h"
#include "plat.h"

#define END_SECONDS	60
#define STEP		0x1000000UL	/* 16 MiB between samples */

/* Numbers only: a static array of string pointers would need ABS relocations. */
#define NREG 2
static const uint64_t r_start[NREG] = { 0xa1800000UL, 0xd4000000UL };
static const uint64_t r_end[NREG]   = { 0xc0000000UL, 0xff000000UL };

/* Name chosen at runtime (PC-relative literal load, no relocation). */
static const char *rname(unsigned r)
{
	return r == 0 ? "R1 487M" : "R2 688M";
}

static const char *stage;	/* set at runtime: no absolute pointers in data */
static int step_row;

void exc_handler(uint64_t slot, uint64_t *regs)
{
	panic_screen(stage, slot, regs);
}

/* Overwrites one line: if the shim hangs, the photo shows the last address. */
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

/* Value this address must hold: unique per address, both halves mixed in. */
static uint32_t pattern(uint64_t a)
{
	return (uint32_t)a ^ (uint32_t)(a >> 32) ^ 0x53395252u;	/* "S9RR" */
}

void shim_main(uint64_t dtb)
{
	char b[80], *p;
	uint64_t a;
	unsigned written = 0, ok = 0, bad = 0;

	(void)dtb;
	/* M1.1: 0x9d400000, 1440x2960, stride 5760, B G R A, autorefresh on */
	fb_default();
	con_clear(GREY);
	con_title("SHIM M8: RAM LIVRE (3.2G?)");
	stage = "watchdog";
	wdog_arm();
	step_row = con_row();
	con_skip();

	/* Write phase: a unique value at each 16 MiB sample of both regions. */
	for (unsigned r = 0; r < NREG; r++) {
		stage = rname(r);
		for (a = r_start[r]; a < r_end[r]; a += STEP) {
			p = put_hex(put(b, "escreve "), a, 9);
			step(b);
			wr32((uintptr_t)a, pattern(a));
			written++;
			if ((written & 7) == 0)
				wdog_pet();
		}
	}
	dsb();

	/* Read-back phase: every sample must still hold its own value (no alias). */
	for (unsigned r = 0; r < NREG; r++) {
		stage = rname(r);
		unsigned rok = 0, rbad = 0;
		uint64_t firstbad = 0;
		for (a = r_start[r]; a < r_end[r]; a += STEP) {
			p = put_hex(put(b, "le "), a, 9);
			step(b);
			uint32_t got = rd32((uintptr_t)a), want = pattern(a);
			if (got == want) {
				rok++;
				ok++;
			} else {
				rbad++;
				bad++;
				if (!firstbad)
					firstbad = a;
			}
			if (((rok + rbad) & 7) == 0)
				wdog_pet();
		}
		p = put(b, rname(r));
		p = put_dec(put(p, ": ok="), rok);
		p = put_dec(put(p, " falha="), rbad);
		if (rbad)
			put_hex(put(p, " 1a falha="), firstbad, 9);
		else
			put(p, "  -> RAM LIVRE");
		con_str("REGIAO", b);
	}

	con_skip();
	p = put_dec(put(b, "amostras ok="), ok);
	put_dec(put(p, " falha="), bad);
	con_str("TOTAL", b);
	if (bad == 0) {
		con_str("", "As duas regioes sao RAM livre: 3.2G viavel.");
	} else {
		con_str("", "Alguma regiao nao e RAM livre (ver 1a falha).");
	}

	stage = "fim";
	step("fim");
	con_skip();
	con_str("", "Tire uma foto desta tela!");
	countdown_reboot(con_row(), END_SECONDS);
}
