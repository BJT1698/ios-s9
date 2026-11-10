/*
 * RPMh votes from the apps RSC, MMU off, polled. Taken from shim/m7.c (the
 * Galaxy S9 M7.11 USB cold start, which voted PM8998 ldoa5/12/24 this way on
 * the same SDM845), itself from ios-sweet's sweet_rpmh.c. Register layout as
 * Linux drivers/soc/qcom/rpmh-rsc.c (v2: TCS stride 672, command stride 20),
 * cmd-db layout as drivers/soc/qcom/cmd-db.c. OP6 live DT:
 * rsc@179c0000 drv-2 reg 0x179e0000, qcom,drv-id 2, qcom,tcs-offset 0xd00,
 * qcom,tcs-config: 2 ACTIVE TCS first (TCS 0 and 1), 3 sleep, 3 wake,
 * 1 control; aop-cmd-db-mem@85fe0000 size 0x20000.
 */
#include "hw.h"
#include "plat.h"
#include "con.h"
#include "i2c_bb.h"
#include "rpmh.h"

#define RH_RSC		0x179e0000UL
#define RH_TCS		0x179e0d00UL
#define RH_DICT		0x0c3f000cUL	/* AOP msg RAM: cmd-db address, size */
#define RH_DB_DT	0x85fe0000UL	/* reserved-memory aop-cmd-db-mem */
#define RH_DB_DT_SIZE	0x20000u
#define RH_DRV		2u
#define RH_ACTIVE_N	2u
#define RH_NORMAL_N	8u
#define RH_TCS_STRIDE	0x2a0u
#define RH_CMD_STRIDE	0x14u
#define RH_IRQ_EN	0x00u
#define RH_IRQ_STATUS	0x04u
#define RH_IRQ_CLEAR	0x08u
#define RH_WAIT		0x10u
#define RH_CONTROL	0x14u
#define RH_STATUS	0x18u
#define RH_ENABLE	0x1cu
#define RH_MSGID	0x30u
#define RH_ADDR		0x34u
#define RH_DATA		0x38u
#define RH_CMD_STATUS	0x3cu
#define RH_AMC_EN	(1u << 16)
#define RH_TRIGGER	(1u << 24)
#define RH_ISSUED	(1u << 8)
#define RH_COMPLETE	(1u << 16)
#define RH_WRITE_ACK	(8u | (1u << 8) | (1u << 16))	/* len 8, response, write */
#define RH_DB_MAGIC	0x0c0330dbu			/* bytes db 30 03 0c */
#define RH_DB_HEADER	144u
#define RH_DB_ENTRY	24u

/* VRM registers (qcom-rpmh-regulator.c): voltage mV, enable, mode */
#define VRM_VOLTAGE	0x0u
#define VRM_ENABLE	0x4u
#define VRM_MODE	0x8u

static uintptr_t db;
static uint32_t db_size;

static int wait32(uintptr_t a, uint32_t mask, uint32_t want, unsigned timeout_us)
{
	uint64_t end = now() + ticks_per_s() / 1000000 * timeout_us + 1;

	while ((rd32(a) & mask) != want)
		if (now() > end)
			return 0;
	return 1;
}

static int rh_sync(uintptr_t a, uint32_t v)
{
	wr32(a, v);
	dsb();
	return wait32(a, 0xffffffffu, v, 1000);
}

static uint16_t db16(uint32_t off)
{
	return (uint16_t)(rd32(db + (off & ~3u)) >> (8u * (off & 3u)));
}

static int db_range(uint32_t off, uint32_t size)
{
	return off <= db_size && size <= db_size - off;
}

uintptr_t rh_db_base(void)
{
	return db;
}

/*
 * Sanity-checks the RSC (version, TCS count of DRV2) and finds cmd-db: the
 * reserved-memory address of the live DT, cross-checked against the AOP
 * locator the S9 code used. Fills line with what it saw. 0 = usable.
 */
int rh_init(char *b)
{
	uint32_t id = rd32(RH_RSC), config = rd32(RH_RSC + 0x0c);
	unsigned major = (id >> 16) & 0xff, count = (config >> (6 * RH_DRV)) & 0x3f;
	unsigned ncpt = (config >> 27) & 0x1f;
	uint32_t pa = rd32(RH_DICT), size = rd32(RH_DICT + 4);
	char *p;

	db = 0;
	p = put_hex(put(b, "id="), id, 8);
	p = put_dec(put(p, " tcs="), count);
	p = put_dec(put(p, " ncpt="), ncpt);
	p = put_hex(put(p, " loc="), pa, 8);
	if ((major != 1 && major != 2) || count < RH_NORMAL_N || count > 0x3000 / RH_TCS_STRIDE ||
	    ncpt < 3 || ncpt > 16) {
		put(p, " RSC?");
		return -1;
	}
	if (pa != RH_DB_DT || size < RH_DB_HEADER || size > RH_DB_DT_SIZE)
		p = put(p, " loc!=DT");
	db = RH_DB_DT;
	db_size = RH_DB_DT_SIZE;
	if (rd32(db + 4) != RH_DB_MAGIC) {
		db = 0;
		put(p, " cmd-db?");
		return -1;
	}
	put_dec(put(p, " v"), rd32(RH_DB_DT));
	return 0;
}

/* cmd-db: 8 resource headers at +8 (slave, entry offset, data offset, count). */
int rh_lookup(const char *res, unsigned want, uint32_t *addr)
{
	uint32_t key[2] = { 0, 0 };
	unsigned matches = 0;

	if (!db)
		return -4;
	for (unsigned i = 0; i < 8 && res[i]; i++)
		key[i / 4] |= (uint32_t)(uint8_t)res[i] << (8 * (i % 4));
	for (unsigned i = 0; i < 8; i++) {
		uint32_t h = 8 + 16 * i;
		unsigned slave = db16(h);
		if (!slave)
			break;
		uint32_t entries = RH_DB_HEADER + db16(h + 2);
		unsigned count = db16(h + 6);
		if (slave < 3 || slave > 5 || (entries & 3) || !db_range(entries, count * RH_DB_ENTRY))
			return -1;
		for (unsigned j = 0; j < count; j++) {
			uint32_t e = entries + RH_DB_ENTRY * j;
			if (rd32(db + e) != key[0] || rd32(db + e + 4) != key[1])
				continue;
			uint32_t a = rd32(db + e + 16);
			if (slave != want || (a >> 16) != want || (want == RH_VRM && (a & 15)))
				return -2;
			*addr = a;
			matches++;
		}
	}
	return matches == 1 ? 0 : -3;
}

static int tcs_free(unsigned t)
{
	uintptr_t b = RH_TCS + t * RH_TCS_STRIDE;

	return rd32(b + RH_STATUS) != 0 && rd32(b + RH_STATUS) != 0xffffffffu &&
	    rd32(b + RH_CONTROL) == 0 && rd32(b + RH_ENABLE) == 0 && rd32(b + RH_WAIT) == 0 &&
	    !(rd32(RH_TCS + RH_IRQ_STATUS) & (1u << t));
}

/* One ACTIVE TCS as AMC with n commands (addr, data). 0 = completed. */
static int amc(unsigned n, const uint32_t *addrs, const uint32_t *vals)
{
	int t = -1;

	for (unsigned i = 0; i < RH_ACTIVE_N; i++)
		if (tcs_free(i)) {
			t = (int)i;
			break;
		}
	if (t < 0)
		return -10;
	uintptr_t b = RH_TCS + (uint32_t)t * RH_TCS_STRIDE;
	uint32_t bit = 1u << t, irq_en = rd32(RH_TCS + RH_IRQ_EN);
	uint32_t mask = (1u << n) - 1;

	if (!rh_sync(RH_TCS + RH_IRQ_EN, irq_en & ~bit))
		return -11;
	for (unsigned c = 0; c < n; c++) {
		uintptr_t cmd = b + c * RH_CMD_STRIDE;
		if (!rh_sync(cmd + RH_MSGID, RH_WRITE_ACK) || !rh_sync(cmd + RH_ADDR, addrs[c]) ||
		    !rh_sync(cmd + RH_DATA, vals[c]))
			return -12;
	}
	if (!rh_sync(b + RH_WAIT, mask) || !rh_sync(b + RH_ENABLE, mask) || !rh_sync(b + RH_CONTROL, 0) ||
	    !rh_sync(b + RH_CONTROL, RH_AMC_EN))
		return -13;
	wr32(b + RH_CONTROL, RH_AMC_EN | RH_TRIGGER);
	dsb();
	int done = 0, progress = 0;
	uint64_t deadline = now() + 2 * ticks_per_s();
	while (!done && now() < deadline) {
		uint32_t irq = rd32(RH_TCS + RH_IRQ_STATUS), idle = rd32(b + RH_STATUS);
		int complete = 1;
		for (unsigned c = 0; c < n; c++) {
			uint32_t s = rd32(b + RH_CMD_STATUS + c * RH_CMD_STRIDE);
			complete &= s != 0xffffffffu && (s & (RH_ISSUED | RH_COMPLETE)) == (RH_ISSUED | RH_COMPLETE);
		}
		progress |= idle == 0 || !complete;
		if (idle && idle != 0xffffffffu && ((irq & bit) || progress) && complete)
			done = 1;
		else
			us_delay(1);
	}
	if (!done)
		return -14;	/* TCS left as is: never reused in this run */
	if (!rh_sync(b + RH_CONTROL, RH_AMC_EN) || !rh_sync(b + RH_CONTROL, 0) ||
	    !rh_sync(b + RH_ENABLE, 0) || !rh_sync(b + RH_WAIT, 0))
		return -15;
	wr32(RH_TCS + RH_IRQ_CLEAR, bit);
	rh_sync(RH_TCS + RH_IRQ_EN, (rd32(RH_TCS + RH_IRQ_EN) & ~bit) | (irq_en & bit));
	return 0;
}

/* VRM vote, as m7.c sends it: voltage, then mode, enable last. */
int rh_vrm_on(uint32_t addr, unsigned mv, unsigned mode)
{
	uint32_t a[3], v[3];

	a[0] = addr + VRM_VOLTAGE;
	v[0] = mv;
	a[1] = addr + VRM_MODE;
	v[1] = mode;
	a[2] = addr + VRM_ENABLE;
	v[2] = 1;
	return amc(3, a, v);
}

/*
 * Bus clock manager vote (cmd-db slave 5), as m7.c's bcm() and the kernel's
 * s9_bcm: BCM_TCS_CMD commit, valid, vote_x (ab) << 14, vote_y (ib).
 */
int rh_bcm_on(uint32_t addr, uint32_t vote)
{
	uint32_t v = (1u << 30) | (1u << 29) | ((vote & 0x3fffu) << 14) | (vote & 0x3fffu);

	return amc(1, &addr, &v);
}
