#pragma once
#include <stdint.h>

#define MDSS_NCTL	5
#define MDSS_NPP	5
#define MDSS_NSSPP	8

struct mdss_sspp {
	uint32_t size, addr, ystride, format, unpack;
};

struct mdss_state {
	uint32_t hw_version;
	uint32_t ctl_top[MDSS_NCTL];
	uint32_t pp_autorefresh[MDSS_NPP];
	struct mdss_sspp sspp[MDSS_NSSPP];
	int ctl;		/* CTL driving an interface, -1 if none */
	int cmd_mode;		/* that CTL runs a command-mode panel */
	int autorefresh;	/* a pingpong already re-sends frames by itself */
	int sspp_idx;		/* pipe fetching the splash buffer, -1 if none */
};

void mdss_probe(struct mdss_state *s);
int mdss_fb_from_sspp(const struct mdss_state *s);
void mdss_kick(const struct mdss_state *s);
