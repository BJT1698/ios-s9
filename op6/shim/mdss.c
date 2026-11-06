/*
 * The SDM845 display pipe (SDE) as ABL leaves it for the continuous splash.
 * Block offsets: qcom,mdss_mdp@ae00000 in the stock S9 DTB. Register
 * layouts: drivers/gpu/drm/msm/sde/sde_hw_{ctl,sspp,pingpong}.c and the
 * cont-splash readback in sde_rm.c of the Samsung/LineageOS kernel.
 *
 * The S6E3HA8 runs in command mode: the panel keeps its own copy of the
 * image and only gets a new one when the pipe sends a frame. ABL may leave
 * pingpong autorefresh on (sde_rm.c turns it off at handoff); if not, we
 * send each frame ourselves with CTL_START, like sde_hw_ctl_trigger_start.
 */
#include "hw.h"
#include "fb.h"
#include "mdss.h"
#include "ramlog.h"

#define MDSS_BASE		0x0ae00000UL
#define MDSS_HW_VERSION		0x000

#define CTL_BASE(i)		(MDSS_BASE + 0x2000 + 0x200 * (i))
#define CTL_TOP			0x014
#define CTL_FLUSH		0x018
#define CTL_START		0x01c
#define CTL_FLUSH_MASK_CTL	(1U << 17)

#define PP_AUTOREFRESH_CONFIG	0x030

#define SSPP_SRC_SIZE		0x00
#define SSPP_SRC0_ADDR		0x14
#define SSPP_SRC_YSTRIDE0	0x24
#define SSPP_SRC_FORMAT		0x30
#define SSPP_SRC_UNPACK_PATTERN	0x34

#define DRAM_LO			0x80000000UL

static const uint32_t pp_off[MDSS_NPP] = { 0x71000, 0x71800, 0x72000, 0x72800, 0x73000 };
/* vig0-3, dma0-3 */
static const uint32_t sspp_off[MDSS_NSSPP] = {
	0x5000, 0x7000, 0x9000, 0xb000, 0x25000, 0x27000, 0x29000, 0x2b000,
};

static int in_splash(uint32_t a)
{
	return a >= SPLASH_BASE && a < SPLASH_BASE + SPLASH_SIZE;
}

void mdss_probe(struct mdss_state *s)
{
	/* One line before each block: if a read hangs, the log says which. */
	ramlog_str("MDSS: versao\n");
	s->hw_version = rd32(MDSS_BASE + MDSS_HW_VERSION);

	ramlog_str("MDSS: CTL_TOP\n");
	s->ctl = -1;
	s->cmd_mode = 0;
	for (int i = 0; i < MDSS_NCTL; i++) {
		uint32_t top = rd32(CTL_BASE(i) + CTL_TOP);
		s->ctl_top[i] = top;
		/* sde_rm.c: intf_sel [7:4], mode_sel bit 17 (1 = command mode) */
		if (s->ctl < 0 && ((top >> 4) & 0xf)) {
			s->ctl = i;
			s->cmd_mode = (top >> 17) & 1;
		}
	}

	ramlog_str("MDSS: autorefresh\n");
	s->autorefresh = 0;
	for (int i = 0; i < MDSS_NPP; i++) {
		s->pp_autorefresh[i] = rd32(MDSS_BASE + pp_off[i] + PP_AUTOREFRESH_CONFIG);
		if (s->pp_autorefresh[i] & (1U << 31))
			s->autorefresh = 1;
	}

	/* Prefer a pipe fetching from the reserved splash region, else any DRAM buffer. */
	ramlog_str("MDSS: SSPP\n");
	s->sspp_idx = -1;
	for (int i = 0; i < MDSS_NSSPP; i++) {
		uintptr_t b = MDSS_BASE + sspp_off[i];
		struct mdss_sspp *p = &s->sspp[i];
		p->size = rd32(b + SSPP_SRC_SIZE);
		p->addr = rd32(b + SSPP_SRC0_ADDR);
		p->ystride = rd32(b + SSPP_SRC_YSTRIDE0);
		p->format = rd32(b + SSPP_SRC_FORMAT);
		p->unpack = rd32(b + SSPP_SRC_UNPACK_PATTERN);
		if (!p->size || p->addr < DRAM_LO)
			continue;
		if (s->sspp_idx < 0 ||
		    (in_splash(p->addr) && !in_splash(s->sspp[s->sspp_idx].addr)))
			s->sspp_idx = i;
	}
}

/* Point fb at the buffer and layout the pipe really scans out; 0 if applied. */
int mdss_fb_from_sspp(const struct mdss_state *s)
{
	if (s->sspp_idx < 0)
		return -1;
	const struct mdss_sspp *p = &s->sspp[s->sspp_idx];
	uint32_t bpp = ((p->format >> 9) & 3) + 1;	/* sde_hw_sspp_setup_format */
	uint32_t stride = p->ystride & 0xffff;
	uint32_t height = p->size >> 16;

	if ((bpp != 3 && bpp != 4) || stride < 64 * bpp || stride % bpp)
		return -1;
	if (height < 64 || height > 4096)
		height = PANEL_H;
	if (in_splash(p->addr) && p->addr + (uint64_t)stride * height > SPLASH_BASE + SPLASH_SIZE)
		return -1;
	if (fb_set_format(bpp, p->unpack))
		return -1;
	fb.base = p->addr;
	fb.stride = stride;
	fb.width = stride / bpp;
	fb.height = height;
	return 0;
}

void mdss_kick(const struct mdss_state *s)
{
	if (s->ctl < 0 || !s->cmd_mode || s->autorefresh)
		return;
	uintptr_t c = CTL_BASE(s->ctl);
	dsb();	/* every pixel store is done before the pipe fetches */
	wr32(c + CTL_FLUSH, CTL_FLUSH_MASK_CTL);
	wr32(c + CTL_START, 1);
}
