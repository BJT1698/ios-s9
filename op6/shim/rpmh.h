#pragma once
#include <stdint.h>

/* RPMh through the apps RSC DRV2, MMU off, polled (from shim/m7.c, S9 M7.11). */
#define RH_VRM		4u	/* cmd-db slave id of voltage regulators */
#define RH_BCM		5u	/* cmd-db slave id of bus clock managers */

int rh_init(char *line);
uintptr_t rh_db_base(void);
int rh_lookup(const char *res, unsigned slave, uint32_t *addr);
int rh_vrm_on(uint32_t addr, unsigned mv, unsigned mode);
int rh_bcm_on(uint32_t addr, uint32_t vote);
