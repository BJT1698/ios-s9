#pragma once
#include <stdint.h>

/*
 * SDM845 TLMM pins and an i2c-gpio style bit-banged I2C master on them.
 * Only the pins this port uses are known (tlmm_ctl returns 0 otherwise):
 * Linux pinctrl-sdm845.c puts each pin in one tile, ctl at tile + 0x1000 * n.
 */
#define TLMM_NORTH	0x03900000UL	/* 0x03400000 + 0x500000 */
#define TLMM_EAST	0x03500000UL	/* 0x03400000 + 0x100000 */

#define TLMM_IO		0x4		/* bit 0 input level, bit 1 output value */
#define TLMM_INTR_CFG	0x8
#define TLMM_INTR_ST	0xc
#define TLMM_OE		(1u << 9)
#define TLMM_FUNC_MASK	(0xfu << 2)
#define TLMM_PULL_MASK	3u
#define TLMM_PULL_UP	3u

#define E_ADDR		1		/* address not acknowledged */
#define E_DATA		2		/* data byte not acknowledged */
#define E_SCL		3		/* clock held low (stretch timeout) */
#define E_PIN		4		/* pin not in our tile table */

struct bb {
	uintptr_t sda, scl;		/* ctl register of each pin */
	unsigned half_us;		/* half clock period */
};

uintptr_t tlmm_ctl(unsigned pin);
unsigned tlmm_in(unsigned pin);
void us_delay(unsigned n);

int bb_init(struct bb *b, unsigned sda, unsigned scl, unsigned half_us);
unsigned bb_sda(const struct bb *b);
unsigned bb_scl(const struct bb *b);
int bb_recover(const struct bb *b);
int bb_probe(const struct bb *b, uint8_t addr);
int bb_rd(const struct bb *b, uint8_t addr, uint8_t reg, uint8_t *buf, unsigned n);
int bb_wr(const struct bb *b, uint8_t addr, const uint8_t *buf, unsigned n);
