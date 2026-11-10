/*
 * Bit-banged I2C on SDM845 TLMM pins, the way Linux's i2c-gpio drives an
 * open-drain bus: the output latch stays 0 and a line is pulled low by
 * setting its output enable, released (floats up on the board pull-ups) by
 * clearing it. SCL is read back after every release, so a slave may stretch
 * the clock (10 ms limit). Same engine as the Galaxy S9 shim M5.4
 * (s9/shim/m5.c), which ran sec_ts and the S2DOS05 PMIC this way.
 */
#include "hw.h"
#include "plat.h"
#include "i2c_bb.h"

/*
 * Tiles from Linux drivers/pinctrl/qcom/pinctrl-sdm845.c (NORTH 0x500000,
 * EAST 0x100000 off the TLMM at 0x03400000): PINGROUP(49/50/99, NORTH),
 * PINGROUP(88/125, EAST). Any other pin is refused rather than guessed.
 */
uintptr_t tlmm_ctl(unsigned pin)
{
	switch (pin) {
	case 49:
	case 50:
	case 99:
		return TLMM_NORTH + 0x1000UL * pin;
	case 88:
	case 125:
		return TLMM_EAST + 0x1000UL * pin;
	default:
		return 0;
	}
}

unsigned tlmm_in(unsigned pin)
{
	uintptr_t c = tlmm_ctl(pin);

	return c ? rd32(c + TLMM_IO) & 1 : 0;
}

void us_delay(unsigned n)
{
	uint64_t end = now() + ticks_per_s() / 1000000 * n + 1;

	while (now() < end)
		;
}

unsigned bb_sda(const struct bb *b)
{
	return rd32(b->sda + TLMM_IO) & 1;
}

unsigned bb_scl(const struct bb *b)
{
	return rd32(b->scl + TLMM_IO) & 1;
}

static void line(const struct bb *b, uintptr_t ctl, int high)
{
	wr32(ctl, high ? 0 : TLMM_OE);
	us_delay(b->half_us);
}

static int scl_high(const struct bb *b)
{
	uint64_t end = now() + ticks_per_s() / 100;

	wr32(b->scl, 0);
	while (!bb_scl(b))
		if (now() > end)
			return 0;
	us_delay(b->half_us);
	return 1;
}

/*
 * GPIO function, input, no pull (the DT's qup-i2c12 state is bias-disable:
 * the pull-ups are on the board), 2 mA, output latch 0.
 */
int bb_init(struct bb *b, unsigned sda, unsigned scl, unsigned half_us)
{
	b->sda = tlmm_ctl(sda);
	b->scl = tlmm_ctl(scl);
	b->half_us = half_us;
	if (!b->sda || !b->scl)
		return E_PIN;
	wr32(b->sda + TLMM_IO, 0);
	wr32(b->scl + TLMM_IO, 0);
	wr32(b->sda, 0);
	wr32(b->scl, 0);
	us_delay(20);
	return 0;
}

/* SDA held low by a slave stuck mid-byte: up to 9 clocks, then a STOP. */
int bb_recover(const struct bb *b)
{
	if (!bb_scl(b))
		return E_SCL;
	for (int i = 0; i < 9 && !bb_sda(b); i++) {
		line(b, b->scl, 0);
		if (!scl_high(b))
			return E_SCL;
	}
	line(b, b->scl, 0);
	line(b, b->sda, 0);
	if (!scl_high(b))
		return E_SCL;
	line(b, b->sda, 1);
	return bb_sda(b) ? 0 : E_DATA;
}

static int start(const struct bb *b)
{
	line(b, b->sda, 1);
	if (!scl_high(b))
		return 0;
	line(b, b->sda, 0);
	line(b, b->scl, 0);
	return 1;
}

static void stop(const struct bb *b)
{
	line(b, b->sda, 0);
	scl_high(b);
	line(b, b->sda, 1);
}

/* 0 acknowledged, E_DATA not, E_SCL clock stuck */
static int wbyte(const struct bb *b, uint8_t v)
{
	int nack;

	for (int i = 7; i >= 0; i--) {
		line(b, b->sda, v >> i & 1);
		if (!scl_high(b))
			return E_SCL;
		line(b, b->scl, 0);
	}
	line(b, b->sda, 1);
	if (!scl_high(b))
		return E_SCL;
	nack = bb_sda(b);
	line(b, b->scl, 0);
	return nack ? E_DATA : 0;
}

/* One byte, then ACK (more wanted) or NACK (last). -1: clock stuck. */
static int rbyte(const struct bb *b, int ack)
{
	unsigned v = 0;

	line(b, b->sda, 1);
	for (int i = 0; i < 8; i++) {
		if (!scl_high(b))
			return -1;
		v = v << 1 | bb_sda(b);
		line(b, b->scl, 0);
	}
	line(b, b->sda, !ack);
	if (!scl_high(b))
		return -1;
	line(b, b->scl, 0);
	line(b, b->sda, 1);
	return (int)v;
}

static int address(const struct bb *b, uint8_t addr, int rd)
{
	int err;

	if (!start(b))
		return E_SCL;
	err = wbyte(b, (uint8_t)(addr << 1 | (rd ? 1 : 0)));
	return err == E_DATA ? E_ADDR : err;
}

/* START, address + W, STOP: does anyone answer at addr? */
int bb_probe(const struct bb *b, uint8_t addr)
{
	int err = address(b, addr, 0);

	stop(b);
	return err;
}

/* Register read with a repeated START, like i2c_transfer of a write + a read. */
int bb_rd(const struct bb *b, uint8_t addr, uint8_t reg, uint8_t *buf, unsigned n)
{
	int err = address(b, addr, 0);

	if (!err)
		err = wbyte(b, reg);
	if (!err && n == 0) {		/* nothing to read: no read phase to end */
		stop(b);
		return 0;
	}
	if (!err)
		err = address(b, addr, 1);
	for (unsigned i = 0; !err && i < n; i++) {
		int v = rbyte(b, i + 1 < n);
		if (v < 0)
			err = E_SCL;
		else
			buf[i] = (uint8_t)v;
	}
	stop(b);
	return err;
}

int bb_wr(const struct bb *b, uint8_t addr, const uint8_t *buf, unsigned n)
{
	int err = address(b, addr, 0);

	for (unsigned i = 0; !err && i < n; i++)
		err = wbyte(b, buf[i]);
	stop(b);
	return err;
}
