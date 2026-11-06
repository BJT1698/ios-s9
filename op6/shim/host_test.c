/*
 * Host render of the M1 screen with made-up probe values, to check the
 * layout before a flash: writes a PPM of what the panel should show.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fb.h"
#include "screen.h"

int main(int argc, char **argv)
{
	static struct m1_info in;
	uint8_t *mem = calloc(1, (size_t)PANEL_W * PANEL_H * 4);
	FILE *f;

	if (argc != 2 || !mem)
		return 2;
	fb_default();
	fb.base = (uintptr_t)mem;

	strcpy(in.model, "Samsung STARQLTE CHN PROJECT Rev14");
	strcpy(in.panel, "ss_dsi_panel_S6E3HA8_AMB577PX01_WQHD:config0:cmd");
	strcpy(in.lcd_id, "4000A0");
	in.dtb = 0x83e00000;
	in.image = 0x80080000;
	in.el = 1;
	in.midr = 0x517f803c;
	in.mmfr0 = 0x00101122;
	in.psci = 0x00010001;
	in.mdss.hw_version = 0x40000000;
	in.mdss.ctl = 0;
	in.mdss.cmd_mode = 1;
	in.mdss.ctl_top[0] = 0x00020020;
	in.mdss.sspp_idx = 4;
	in.mdss.sspp[4] = (struct mdss_sspp){ 0x0b9005a0, 0x9d400000, 5760, 0x000236ff, 0x03020001 };
	in.fb_from_hw = 1;
	in.fb_addr = 0x9d400000;
	in.wdog_en = 0;
	screen_draw(&in);
	screen_tick(7, 60);

	if (!(f = fopen(argv[1], "wb")))
		return 1;
	fprintf(f, "P6\n%u %u\n255\n", fb.width, fb.height);
	for (uint32_t y = 0; y < fb.height; y++)
		for (uint32_t x = 0; x < fb.width; x++) {
			const uint8_t *p = mem + y * fb.stride + x * fb.bpp;
			fputc(p[fb.pos_r], f);
			fputc(p[fb.pos_g], f);
			fputc(p[fb.pos_b], f);
		}
	return fclose(f) ? 1 : 0;
}
