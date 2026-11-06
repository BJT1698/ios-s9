/*
 * Galaxy S9 milestone 3: storage and address translation, both as
 * ios-sweet found them on SM7150 (its shim milestones 3 and 5).
 *   - UFS at 0x1d84000 (same address as sweet): with the MMU off, take over
 *     the transfer list ABL left and issue READ-ONLY SCSI commands on LUN 0:
 *     test unit ready, capacity, the GPT, the big partitions.
 *   - Stage-1 MMU at EL1 with XNU's 16K granule. On the S9 this also asks
 *     whether Samsung's EL2 hypervisor (RKP/uH) lets EL1 own its translation
 *     registers before any kernel has talked to it.
 */
#include <stdint.h>
#include "hw.h"
#include "fb.h"
#include "con.h"
#include "mmu.h"
#include "plat.h"
#include "ufs.h"

#define RUN_SECONDS	90

static const char *stage;	/* set at runtime: no absolute pointers in data */
static uint64_t scratch[8192];	/* 64KB of our image: Device, then write-back */

void exc_handler(uint64_t slot, uint64_t *regs)
{
	panic_screen(stage, slot, regs);
}

static uint64_t ticks_mem(void)
{
	uint64_t t = now();
	for (int pass = 0; pass < 16; pass++)
		for (int i = 0; i < 8192; i++)
			scratch[i] = scratch[i] * 3 + i;
	return now() - t;
}

static void par_line(const char *label, uintptr_t va)
{
	uint64_t par = mmu_probe(va);
	char b[48], *p = put_hex(put(b, "va="), va, 9);

	if (par & 1)
		put_hex(put(p, " FALTA fst="), (par >> 1) & 0x3f, 2);
	else
		put_hex(put(put_hex(put(p, " pa="), par & 0xfffffffff000UL, 9), " attr="), par >> 56, 2);
	con_str(label, b);
}

void shim_main(uint64_t dtb)
{
	struct mmu_info mi;
	char b[64], *p;

	(void)dtb;
	/* M1.1: 0x9d400000, 1440x2960, stride 5760, B G R A, autorefresh on */
	fb_default();
	con_clear(GREY);
	con_title("SHIM M3: UFS + MMU");
	stage = "watchdog";
	wdog_arm();

	stage = "ufs";
	test_ufs();
	wdog_pet();
	con_skip();

	stage = "mem off";
	uint64_t mem_off = ticks_mem();
	stage = "mmu enable";
	int rc = mmu_enable(&mi);
	stage = "mem on";
	uint64_t mem_on = rc ? 0 : ticks_mem();
	stage = "mmu report";

	p = put_dec(put(b, rc ? "FALHOU rc=" : "ligada rc="), (uint32_t)rc);
	put_dec(put(p, "  tabelas L3="), mi.n_l3);
	con_str("MMU 16K", b);
	con_hex2("IMAGEM", "lo=", mi.img_lo, "hi=", mi.img_hi);
	con_hex("TTBR0", mi.ttbr);
	con_hex2("SCTLR", "antes=", mi.sctlr_before, "depois=", mi.sctlr_after);
	con_hex("TCR", rsr("tcr_el1"));
	par_line("PAR img", (uintptr_t)shim_main);
	par_line("PAR fb", FB_PHYS + 0x1000);
	par_line("PAR gic", 0x17a00000UL);
	par_line("PAR 12G", 0x300000000UL);
	p = put_dec(put(b, "sem MMU "), ticks_to_us(mem_off));
	put(put_dec(put(p, "us, com "), ticks_to_us(mem_on)), "us");
	con_str("MEMORIA", b);
	wdog_pet();

	stage = "fim";
	con_skip();
	con_str("", "Tire uma foto desta tela!");
	countdown_reboot(con_row(), RUN_SECONDS);
}
