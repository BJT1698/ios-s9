#pragma once
#include <stdint.h>
#include "i2c_bb.h"

/*
 * Synaptics RMI4 over I2C, polled. Own implementation (MIT); register
 * semantics from the RMI4 layout Linux's drivers/input/rmi4 also follows.
 */
#define RMI_MAX_FN	16
#define RMI_DESC_REGS	64	/* register numbers we track per descriptor */

struct rmi_fn {
	uint8_t num, page, query, cmd, ctrl, data;
	uint8_t irqs, version, irq_pos;
};

/* One query/control/data register descriptor, for registers 0..63. */
struct rmi_desc {
	uint16_t struct_size;
	uint8_t nregs;			/* present, non-zero size, < RMI_DESC_REGS */
	uint8_t present[RMI_DESC_REGS];
	uint16_t size[RMI_DESC_REGS];
	uint8_t nsub[RMI_DESC_REGS];	/* subpackets in the register */
	uint8_t sub0[RMI_DESC_REGS];	/* bits of subpackets 0..6 */
};

struct rmi {
	struct bb bus;
	uint8_t addr;
	int page;			/* -1: unknown, always select */
	unsigned nfn, irq_count;
	struct rmi_fn fn[RMI_MAX_FN];
};

int rmi_read(struct rmi *r, uint16_t a, uint8_t *buf, unsigned n);
int rmi_write(struct rmi *r, uint16_t a, const uint8_t *buf, unsigned n);
int rmi_scan(struct rmi *r);
const struct rmi_fn *rmi_find(const struct rmi *r, uint8_t num);
uint16_t rmi_addr(const struct rmi_fn *f, uint8_t base);
int rmi_read_desc(struct rmi *r, uint16_t a, struct rmi_desc *d);
int rmi_desc_index(const struct rmi_desc *d, unsigned reg);
int rmi_desc_offset(const struct rmi_desc *d, unsigned reg);

/* F12 (2D sensor): where the finger records are and the coordinate range. */
struct f12 {
	uint16_t data_addr;		/* first data register */
	uint16_t data1_off;		/* byte offset of data1 in a read from data_addr */
	uint16_t data1_size;
	uint8_t fingers;		/* data1 subpackets: 8-byte object records */
	uint16_t max_x, max_y;		/* control 8 subpacket 0 */
	uint8_t q0;			/* general info: bit 0 descriptors present */
	uint8_t nq, nc, nd;		/* registers in each descriptor */
	int err;			/* where it stopped: see rmi4.c */
};
int f12_setup(struct rmi *r, const struct rmi_fn *f, struct f12 *t, struct rmi_desc *scratch);

/* F11 (older 2D sensor) fallback: sensor 0 only, absolute data. */
struct f11 {
	uint16_t data_addr;
	uint8_t fingers, state_bytes;
	uint16_t max_x, max_y;
	int err;
};
int f11_setup(struct rmi *r, const struct rmi_fn *f, struct f11 *t);
