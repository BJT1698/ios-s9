/*
 * Synaptics RMI4 over I2C for the OnePlus 6 shim (MIT, written for this
 * port). The register layout it relies on, as documented by Synaptics'
 * RMI4 spec and followed by Linux's drivers/input/rmi4:
 *   - 16-bit register space in 256-byte pages; register 0xFF of every page
 *     selects the page. On I2C: write [0xFF, page], then [reg & 0xff, ...]
 *     or a register byte followed by a repeated-START read.
 *   - Page Description Table: 6-byte entries from 0xE9 downward in each page
 *     (query, command, control, data base, irq count | version << 5,
 *     function number); function 0x00/0xFF ends the page; two empty pages
 *     in a row end the scan. Interrupt bits are handed out in scan order.
 *   - Register descriptors (F12): a size byte, a presence register (byte 0
 *     is the structure size, or 0 then a 16-bit size), a bitmap of which
 *     packet registers exist, then per present register its size (8-bit, or
 *     0 + 16-bit, or 0 + 0 + 32-bit) and subpacket bitmap bytes (7 bits each,
 *     bit 7 = another byte follows). Packet register k sits at base + (number
 *     of present registers below k); a block read from the base returns the
 *     present registers back to back.
 */
#include "rmi4.h"

#define PAGE_SELECT	0xff
#define PDT_TOP		0xe9
#define PDT_BOTTOM	0x05
#define PDT_ENTRY	6
#define MAX_PAGES	16

int rmi_read(struct rmi *r, uint16_t a, uint8_t *buf, unsigned n)
{
	unsigned page = a >> 8;
	int err;

	if (r->page != (int)page) {
		uint8_t sel[2];
		sel[0] = PAGE_SELECT;
		sel[1] = (uint8_t)page;
		err = bb_wr(&r->bus, r->addr, sel, 2);
		if (err) {
			r->page = -1;
			return err;
		}
		r->page = (int)page;
	}
	return bb_rd(&r->bus, r->addr, (uint8_t)a, buf, n);
}

int rmi_write(struct rmi *r, uint16_t a, const uint8_t *buf, unsigned n)
{
	uint8_t out[17];
	unsigned page = a >> 8;
	int err;

	if (n > 16)
		return E_DATA;
	if (r->page != (int)page) {
		out[0] = PAGE_SELECT;
		out[1] = (uint8_t)page;
		err = bb_wr(&r->bus, r->addr, out, 2);
		if (err) {
			r->page = -1;
			return err;
		}
		r->page = (int)page;
	}
	out[0] = (uint8_t)a;
	for (unsigned i = 0; i < n; i++)
		out[1 + i] = buf[i];
	return bb_wr(&r->bus, r->addr, out, n + 1);
}

uint16_t rmi_addr(const struct rmi_fn *f, uint8_t base)
{
	return (uint16_t)(f->page << 8 | base);
}

int rmi_scan(struct rmi *r)
{
	unsigned empty = 0;

	r->nfn = 0;
	r->irq_count = 0;
	for (unsigned page = 0; page < MAX_PAGES && empty < 2; page++) {
		unsigned found = 0;
		for (int a = PDT_TOP; a >= PDT_BOTTOM; a -= PDT_ENTRY) {
			uint8_t e[PDT_ENTRY];
			int err = rmi_read(r, (uint16_t)(page << 8 | a), e, PDT_ENTRY);
			if (err)
				return err;
			if (e[5] == 0x00 || e[5] == 0xff)
				break;
			found++;
			if (r->nfn < RMI_MAX_FN) {
				struct rmi_fn *f = &r->fn[r->nfn++];
				f->num = e[5];
				f->page = (uint8_t)page;
				f->query = e[0];
				f->cmd = e[1];
				f->ctrl = e[2];
				f->data = e[3];
				f->irqs = e[4] & 7;
				f->version = (e[4] >> 5) & 3;
				f->irq_pos = (uint8_t)r->irq_count;
			}
			r->irq_count += e[4] & 7;
		}
		empty = found ? 0 : empty + 1;
	}
	return 0;
}

const struct rmi_fn *rmi_find(const struct rmi *r, uint8_t num)
{
	for (unsigned i = 0; i < r->nfn; i++)
		if (r->fn[i].num == num)
			return &r->fn[i];
	return 0;
}

/*
 * Error codes for the descriptor parser (negative, apart from I2C's):
 * -1 presence size out of range, -2 structure too big for the buffer,
 * -3 an item ran past the structure.
 */
#define DESC_BUF	255

int rmi_read_desc(struct rmi *r, uint16_t a, struct rmi_desc *d)
{
	uint8_t psize, pres[35], st[DESC_BUF];
	unsigned first, bit = 0, off = 0;
	int err;

	for (unsigned i = 0; i < RMI_DESC_REGS; i++) {
		d->present[i] = 0;
		d->size[i] = 0;
		d->nsub[i] = 0;
		d->sub0[i] = 0;
	}
	d->nregs = 0;
	d->struct_size = 0;
	err = rmi_read(r, a, &psize, 1);
	if (err)
		return err;
	if (psize < 1 || psize > sizeof(pres))
		return -1;
	err = rmi_read(r, (uint16_t)(a + 1), pres, psize);
	if (err)
		return err;
	if (pres[0] == 0) {
		if (psize < 3)
			return -1;
		d->struct_size = (uint16_t)(pres[1] | pres[2] << 8);
		first = 3;
	} else {
		d->struct_size = pres[0];
		first = 1;
	}
	if (d->struct_size > DESC_BUF)
		return -2;
	err = rmi_read(r, (uint16_t)(a + 2), st, d->struct_size);
	if (err)
		return err;
	for (unsigned i = first; i < psize; i++) {
		for (unsigned b = 0; b < 8; b++, bit++) {
			uint32_t size;
			unsigned nsub = 0, sub0 = 0, sb = 0;

			if (!(pres[i] >> b & 1))
				continue;
			if (off >= d->struct_size)
				return 0;	/* structure shorter than the map: stop */
			size = st[off++];
			if (size == 0) {
				if (off + 2 > d->struct_size)
					return -3;
				size = st[off] | (uint32_t)st[off + 1] << 8;
				off += 2;
			}
			if (size == 0) {
				if (off + 4 > d->struct_size)
					return -3;
				size = st[off] | (uint32_t)st[off + 1] << 8 |
				       (uint32_t)st[off + 2] << 16 | (uint32_t)st[off + 3] << 24;
				off += 4;
			}
			for (;;) {
				if (off >= d->struct_size)
					return -3;
				uint8_t v = st[off++];
				for (unsigned k = 0; k < 7; k++, sb++)
					if (v >> k & 1) {
						nsub++;
						if (sb < 7)
							sub0 |= 1u << sb;
					}
				if (!(v & 0x80))
					break;
			}
			if (size == 0 || bit >= RMI_DESC_REGS)
				continue;	/* zero-sized registers do not exist */
			d->present[bit] = 1;
			d->size[bit] = size > 0xffff ? 0xffff : (uint16_t)size;
			d->nsub[bit] = nsub > 255 ? 255 : (uint8_t)nsub;
			d->sub0[bit] = (uint8_t)sub0;
			d->nregs++;
		}
	}
	return 0;
}

/* Address offset of register reg from its base, -1 if absent. */
int rmi_desc_index(const struct rmi_desc *d, unsigned reg)
{
	int idx = 0;

	if (reg >= RMI_DESC_REGS || !d->present[reg])
		return -1;
	for (unsigned i = 0; i < reg; i++)
		idx += d->present[i];
	return idx;
}

/* Byte offset of register reg in a block read from the base, -1 if absent. */
int rmi_desc_offset(const struct rmi_desc *d, unsigned reg)
{
	int off = 0;

	if (reg >= RMI_DESC_REGS || !d->present[reg])
		return -1;
	for (unsigned i = 0; i < reg; i++)
		if (d->present[i])
			off += d->size[i];
	return off;
}

/*
 * F12: query 0 = general info (bit 0: register descriptors exist); the
 * query, control and data descriptors start at query + 1, + 4 and + 7.
 * Control 8 subpacket 0 = max X, max Y (LE16). Data 1 = one 8-byte record
 * per subpacket. t->err: 0 ok, 1 no descriptors, 2/3/4 query/control/data
 * descriptor failed, 5 no control 8, 6 no data 1, 7 control 8 read failed,
 * 8 query 0 read failed.
 */
int f12_setup(struct rmi *r, const struct rmi_fn *f, struct f12 *t, struct rmi_desc *d)
{
	uint16_t q = rmi_addr(f, f->query);
	uint8_t buf[16];
	int idx, err;

	t->err = 0;
	t->max_x = t->max_y = 0;
	t->fingers = 0;
	err = rmi_read(r, q, &t->q0, 1);
	if (err) {
		t->err = 8;
		return err;
	}
	if (!(t->q0 & 1))
		return t->err = 1;

	if (rmi_read_desc(r, (uint16_t)(q + 1), d))
		return t->err = 2;
	t->nq = d->nregs;

	if (rmi_read_desc(r, (uint16_t)(q + 4), d))
		return t->err = 3;
	t->nc = d->nregs;
	idx = rmi_desc_index(d, 8);
	if (idx < 0)
		return t->err = 5;
	unsigned n = d->size[8] > sizeof(buf) ? sizeof(buf) : d->size[8];
	if (rmi_read(r, (uint16_t)(rmi_addr(f, f->ctrl) + idx), buf, n))
		return t->err = 7;
	if ((d->sub0[8] & 1) && n >= 4) {
		t->max_x = (uint16_t)(buf[0] | buf[1] << 8);
		t->max_y = (uint16_t)(buf[2] | buf[3] << 8);
	}

	if (rmi_read_desc(r, (uint16_t)(q + 7), d))
		return t->err = 4;
	t->nd = d->nregs;
	int off = rmi_desc_offset(d, 1);
	if (off < 0)
		return t->err = 6;
	t->data_addr = rmi_addr(f, f->data);
	t->data1_off = (uint16_t)off;
	t->data1_size = d->size[1];
	t->fingers = d->nsub[1];
	return 0;
}

/*
 * F11 sensor 0: query 1 bits 0-2 fingers (5 means 10), bit 4 absolute
 * data; control 6/7 and 8/9 max X/Y (LE16); data = 2 state bits per finger,
 * then 5 bytes per finger (x hi, y hi, y lo << 4 | x lo, wy << 4 | wx, z).
 * t->err: 1 no absolute data, 2 query read, 3 control read.
 */
int f11_setup(struct rmi *r, const struct rmi_fn *f, struct f11 *t)
{
	uint8_t q1, c[4];

	t->err = 0;
	if (rmi_read(r, (uint16_t)(rmi_addr(f, f->query) + 1), &q1, 1))
		return t->err = 2;
	if (!(q1 & 0x10))
		return t->err = 1;
	t->fingers = (q1 & 7) == 5 ? 10 : (q1 & 7) + 1;
	t->state_bytes = (uint8_t)((t->fingers + 3) / 4);
	if (rmi_read(r, (uint16_t)(rmi_addr(f, f->ctrl) + 6), c, 4))
		return t->err = 3;
	t->max_x = (uint16_t)(c[0] | (c[1] & 0x0f) << 8);	/* 12 bits */
	t->max_y = (uint16_t)(c[2] | (c[3] & 0x0f) << 8);
	t->data_addr = rmi_addr(f, f->data);
	return 0;
}
