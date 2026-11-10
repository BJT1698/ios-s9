/*
 * OnePlus 6 milestone M10 (GPU stage A): wake the Adreno 630's GMU (the
 * Cortex-M3 power controller inside the GPU block) up to the point where its
 * own firmware runs: GMU_CM3_FW_INIT_RESULT == 0xBABEFACE. Port of the Galaxy
 * S9 M10 (s9/shim/m10.c, passed first time on the S9 2026-10-25), re-checked
 * against the OP6 live DT and mainline a6xx_gmu.c; shared code in gpu.c.
 *
 *   1. read-only: GCC/gpucc as ABL left them; cmd-db (no vote): the ARC
 *      level lists cx.lvl / mx.lvl / gfx.lvl and the names behind the PDC's
 *      fixed TCS addresses 0x30000 / 0x30010 / 0x30080.
 *   2. CX GDSC + GMU clocks (200 MHz), the GPU SMMU id registers (read),
 *      GMU registers, and a write/read-back of two DTCM words with the CM3
 *      held in reset.
 *   3. RSC/PDC micro-code, a630_gmu.bin into ITCM, release the CM3, wait for
 *      0xBABEFACE; then the CM3 goes back to reset (no HFI memory exists in
 *      this stage, so it is not left running).
 *
 * Not here: GX GDSC, GPU SMMU writes, HFI, GMU RPMh votes, zap, CP.
 * Every step is on screen (PASSO) and in the ramoops console (ramlog) before
 * it runs; the APSS watchdog is armed (15 s bark).
 */
#include <stdint.h>
#include "hw.h"
#include "fb.h"
#include "con.h"
#include "plat.h"
#include "gpu.h"

#define END_SECONDS	60

static int stage2(void)
{
	static const uint32_t pat[2] = { 0x600d0001u, 0x0a110002u };
	uint32_t was[2], back[2];
	char b[96], *p;

	if (!gpu_power_on(1))
		return 0;
	step("2 SMMU da GPU: so leitura");
	gpu_smmu_ids();

	step("2 registradores do GMU");
	p = put_hex(put(b, "init="), rd32(GMU(GMU_CM3_FW_INIT_RESULT)), 8);
	p = put_hex(put(p, " pwr="), rd32(GMU(GMU_RPMH_POWER_STATE)), 2);
	p = put_hex(put(p, " gen7="), rd32(GMU(GMU_GENERAL_7)), 2);
	put_hex(put(p, " ack="), rd32(GMU(GMU_RSCC_CONTROL_ACK)), 8);
	say("GMU", b);
	if (rd32(GMU(GMU_CM3_FW_INIT_RESULT)) == GMU_BABEFACE)
		say("GMU", "o firmware ja rodava (nao esperado apos o ABL)");

	step("2 DTCM: escreve/le (CM3 em reset)");
	gpu_gmu_halt();
	for (int i = 0; i < 2; i++)
		was[i] = rd32(GMU(GMU_CM3_DTCM_START + i));
	for (int i = 0; i < 2; i++)
		wr32(GMU(GMU_CM3_DTCM_START + i), pat[i]);
	dsb();
	for (int i = 0; i < 2; i++)
		back[i] = rd32(GMU(GMU_CM3_DTCM_START + i));
	for (int i = 0; i < 2; i++)
		wr32(GMU(GMU_CM3_DTCM_START + i), was[i]);
	dsb();
	p = put_hex(put(b, "back="), back[0], 8);
	put_hex(put(p, " "), back[1], 8);
	say("DTCM", b);
	if (back[0] != pat[0] || back[1] != pat[1]) {
		say("2", "a memoria do GMU nao guarda o que se escreve: parando");
		return 0;
	}
	say("2", "GMU com energia e memoria OK");
	return 1;
}

void shim_main(uint64_t dtb)
{
	(void)dtb;
	gpu_screen("OP6 SHIM M10: GMU DA ADRENO 630", "\n=== OP6 shim M10 gmu ===\n");

	step("1 estado do ABL (so leitura)");
	gpu_state(1);
	if (gpu_cmddb(1))
		say("1", "cmd-db incompleto (so informativo nesta etapa)");
	con_skip();

	if (stage2()) {
		con_skip();
		if (!gpu_have_fw()) {
			step("3 firmware do GMU");
			say("3", "SEM FIRMWARE: a630_gmu.bin nao embutido");
			say("3", "op6/tools/gpu-fw-embed.py <a630_gmu.bin>, build.sh m10");
		} else if (gpu_gmu_boot(0, 0, 0xffffffffu)) {
			say("3", "O FIRMWARE DO GMU RODA (0xBABEFACE)");
			sayhex("aon", rd32(GMU(GMU_AO_COUNTER_L)));
			step("3 CM3 de volta ao reset");
			gpu_gmu_halt();
		} else {
			say("3", "o firmware do GMU nao subiu (ver init= acima)");
			gpu_gmu_halt();
		}
	}

	step("fim");
	con_skip();
	say("", "Tire uma foto desta tela!");
	countdown_reboot(con_row(), END_SECONDS);
}
