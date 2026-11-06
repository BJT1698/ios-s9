/*
 * Galaxy S9 milestone 7: USB device mode from cold, MMU off, polled.
 * ios-sweet's XNU USB Ethernet (bsd/dev/sweet_usb.c) starts from a DWC3 that
 * the Redmi's ABL left running for fastboot. Samsung's ABL leaves it off, so
 * everything Linux does before the gadget is done here, then the same ep0
 * code enumerates as CDC-ECM. Proof = the PC's dmesg shows the device.
 *   1. RPMh votes (apps RSC DRV2 0x179e0000, same TCS layout as sweet's
 *      SM7150): the QUSB2 PHY's vdd = ldoa5 0.8 V, vdda18 = ldoa12 1.8 V,
 *      vdda33 = ldoa24 3.075 V (pm8998, DT qusb@88e2000).
 *   2. GCC (gcc-sdm845.c): usb30_prim GDSC 0xf004; master RCG 0xf018 at
 *      133.33 MHz (GPLL0 / 4.5); branches master 0xf00c, sleep 0xf010,
 *      mock_utmi 0xf014, cfg_noc 0x502c, aggre 0x8201c, ahb2phy 0x6a004;
 *      resets USB30_PRIM_BCR 0xf000, QUSB2PHY_PRIM_BCR 0x12000.
 *   3. QUSB2 PHY (phy-msm-qusb-v2.c): power down, DT init sequence, power
 *      up, PLL ready.
 *   4. DWC3 high speed only, no SS PHY (dwc3-msm.c POST_RESET: UTMI clock
 *      for the pipe side), ref clock 19.2 MHz, device mode, VBUS override.
 * MMU off: all memory is Device, so the DMA area needs no cache maintenance.
 */
#include <stdint.h>
#include "hw.h"
#include "fb.h"
#include "con.h"
#include "plat.h"

void *memcpy(void *d, const void *s, unsigned long n);

#define USB_SECONDS	90
#define END_SECONDS	30

/* ---- RPMh ---- */
#define RH_RSC		0x179e0000UL
#define RH_TCS		0x179e0d00UL
#define RH_DICT		0x0c3f000cUL
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
#define RH_WRITE_ACK	(8u | (1u << 8) | (1u << 16))
#define RH_DB_MAGIC	0x0c0330dbu
#define RH_DB_HEADER	144u
#define RH_DB_ENTRY	24u
#define RH_VRM		4u

/* ---- GCC ---- */
#define GCC		0x00100000UL
#define GDSC_USB30	0xf004
#define RCG_MASTER_CMD	0xf018
#define RCG_MASTER_CFG	0xf01c
#define BCR_USB30	0xf000
#define BCR_QUSB2	0x12000
#define CLK_OFF		(1u << 31)

static const uint32_t usb_cbcr[] = { 0xf00c, 0xf010, 0xf014, 0x502c, 0x8201c, 0x6a004 };

/* ---- PHY / DWC3 ---- */
#define AHB2PHY		0x088ee000UL
#define QUSB2		0x088e2000UL
#define Q_PWR_CTRL1	0x210
#define Q_PLL_STATUS	0x1a0

static const uint16_t qusb_init[] = {	/* qcom,qusb-phy-init-seq: value, offset */
	0x23, 0x210, 0x03, 0x04, 0x7c, 0x18c, 0x80, 0x2c, 0x0a, 0x184, 0x19, 0xb4,
	0x40, 0x194, 0x1b, 0x198, 0x21, 0x214, 0x0e, 0x220, 0x58, 0x224, 0x75, 0x240,
	0x29, 0x244, 0xca, 0x248, 0x02, 0x24c, 0x03, 0x250, 0x00, 0x23c, 0x22, 0x210,
};

#define DWC		0x0a600000UL
#define QS		(DWC + 0xf8800)
#define QS_GENERAL_CFG	0x08
#define QS_HS_PHY_CTRL	0x10
#define PIPE_UTMI_CLK_SEL	(1u << 0)
#define PIPE3_PHYSTATUS_SW	(1u << 3)
#define PIPE_UTMI_CLK_DIS	(1u << 8)
#define UTMI_OTG_VBUS_VALID	(1u << 20)
#define SW_SESSVLD_SEL		(1u << 28)

#define GCTL		0xc110
#define GSTS		0xc118
#define GSNPSID		0xc120
#define GUCTL		0xc12c
#define GUSB2PHYCFG	0xc200
#define GUSB3PIPECTL	0xc2c0
#define GEVNTADRLO	0xc400
#define GEVNTADRHI	0xc404
#define GEVNTSIZ	0xc408
#define GEVNTCOUNT	0xc40c
#define GFLADJ		0xc630
#define DCFG		0xc700
#define DCTL		0xc704
#define DEVTEN		0xc708
#define DSTS		0xc70c
#define DALEPENA	0xc720
#define DEPCMDPAR2(n)	(0xc800 + 0x10 * (n))
#define DEPCMDPAR1(n)	(0xc804 + 0x10 * (n))
#define DEPCMDPAR0(n)	(0xc808 + 0x10 * (n))
#define DEPCMD(n)	(0xc80c + 0x10 * (n))

#define DCTL_RUN_STOP	(1u << 31)
#define DCTL_CSFTRST	(1u << 30)
#define DSTS_DEVCTRLHLT	(1u << 22)

#define DEPCMD_SETEPCONFIG	1
#define DEPCMD_SETTRANSFRESOURCE 2
#define DEPCMD_SETSTALL		4
#define DEPCMD_STARTTRANSFER	6
#define DEPCMD_DEPSTARTCFG	9
#define DEPCMD_CMDACT		(1u << 10)

#define TRB_HWO		(1u << 0)
#define TRB_LST		(1u << 1)
#define TRB_ISP_IMI	(1u << 10)
#define TRB_IOC		(1u << 11)
#define TRBCTL_NORMAL	(1u << 4)
#define TRBCTL_SETUP	(2u << 4)
#define TRBCTL_STATUS2	(3u << 4)
#define TRBCTL_STATUS3	(4u << 4)
#define TRBCTL_DATA	(5u << 4)

#define UD_EVT		0x0000
#define UD_EVT_SIZE	0x1000
#define UD_TRB_OUT	0x1000
#define UD_TRB_IN	0x1010
#define UD_SETUP	0x1040
#define UD_DATA		0x1100
#define UD_SIZE		0x2000
#define EP_RX		2
#define EP_TX		3
#define EP_INT		5

static const char *stage;	/* set at runtime: no absolute pointers in data */
static int step_row;
static uint8_t dma[UD_SIZE] __attribute__((aligned(4096)));

static struct {
	uintptr_t db;
	uint32_t db_size, ncpt;
	uint32_t evt_pos;
	enum { EP0_SETUP, EP0_DATA, EP0_WAIT_STATUS2, EP0_WAIT_STATUS3, EP0_STATUS } state;
	uint32_t events, resets, connects, disconnects, setups, cmd_err;
	uint32_t speed, address, configured;
	uint8_t last[8];
} u;

void exc_handler(uint64_t slot, uint64_t *regs)
{
	panic_screen(stage, slot, regs);
}

static void us(unsigned n)
{
	uint64_t end = now() + ticks_per_s() / 1000000 * n + 1;

	while (now() < end)
		;
}

/* Overwrites one line: if the shim hangs, the photo shows the last step. */
static void step(const char *s)
{
	char b[52], *p = put(b, s);
	int row = con_row();

	while (p < b + 50)
		*p++ = ' ';
	*p = 0;
	con_set_row(step_row);
	con_str("PASSO", b);
	con_set_row(row);
}

/* Poll until (reg & mask) == want or timeout; returns 1 on success. */
static int wait32(uintptr_t a, uint32_t mask, uint32_t want, unsigned timeout_us)
{
	uint64_t end = now() + ticks_per_s() / 1000000 * timeout_us + 1;

	while ((rd32(a) & mask) != want)
		if (now() > end)
			return 0;
	return 1;
}

/* ---- RPMh (from sweet_rpmh.c, MMU off) ---- */

static int rh_sync(uintptr_t a, uint32_t v)
{
	wr32(a, v);
	dsb();
	return wait32(a, 0xffffffffu, v, 1000);
}

static uint16_t rh_db16(uint32_t off)
{
	return (uint16_t)(rd32(u.db + (off & ~3u)) >> (8u * (off & 3u)));
}

static int rh_range(uint32_t off, uint32_t size)
{
	return off <= u.db_size && size <= u.db_size - off;
}

static int rh_init(char *b)
{
	uint32_t id = rd32(RH_RSC), config = rd32(RH_RSC + 0x0c);
	unsigned major = (id >> 16) & 0xff, count = (config >> (6 * RH_DRV)) & 0x3f;
	uint32_t pa = rd32(RH_DICT), size = rd32(RH_DICT + 4);
	char *p;

	u.ncpt = (config >> 27) & 0x1f;
	p = put_hex(put(b, "id="), id, 8);
	p = put_dec(put(p, " tcs="), count);
	p = put_dec(put(p, " ncpt="), u.ncpt);
	p = put_hex(put(p, " db="), pa, 8);
	if ((major != 1 && major != 2) || count < RH_NORMAL_N || count > 0x3000 / RH_TCS_STRIDE ||
	    u.ncpt < 3 || u.ncpt > 16) {
		put(p, " RSC?");
		return -1;
	}
	if (pa < 0x80000000u || (pa & 3) || size < RH_DB_HEADER || size > 0x100000) {
		put(p, " locator?");
		return -1;
	}
	u.db = pa;
	u.db_size = size;
	if (rd32(u.db + 4) != RH_DB_MAGIC || rd32(u.db) != 1 || (rd32(u.db + 140) & 1)) {
		put(p, " cmd-db?");
		return -1;
	}
	return 0;
}

static int rh_lookup(const char *res, unsigned want, uint32_t *addr)
{
	uint32_t key[2] = { 0, 0 };
	unsigned matches = 0;

	for (unsigned i = 0; i < 8 && res[i]; i++)
		key[i / 4] |= (uint32_t)(uint8_t)res[i] << (8 * (i % 4));
	for (unsigned i = 0; i < 8; i++) {
		uint32_t h = 8 + 16 * i;
		unsigned slave = rh_db16(h);
		if (!slave)
			break;
		uint32_t entries = RH_DB_HEADER + rh_db16(h + 2);
		unsigned count = rh_db16(h + 6);
		if (slave < 3 || slave > 5 || (entries & 3) || !rh_range(entries, count * RH_DB_ENTRY))
			return -1;
		for (unsigned j = 0; j < count; j++) {
			uint32_t e = entries + RH_DB_ENTRY * j;
			if (rd32(u.db + e) != key[0] || rd32(u.db + e + 4) != key[1])
				continue;
			uint32_t a = rd32(u.db + e + 16);
			if (slave != want || (a >> 16) != want || (want == RH_VRM && (a & 15)))
				return -2;
			*addr = a;
			matches++;
		}
	}
	return matches == 1 ? 0 : -3;
}

static int rh_free(unsigned t)
{
	uintptr_t b = RH_TCS + t * RH_TCS_STRIDE;

	return rd32(b + RH_STATUS) != 0 && rd32(b + RH_STATUS) != 0xffffffffu &&
	    rd32(b + RH_CONTROL) == 0 && rd32(b + RH_ENABLE) == 0 && rd32(b + RH_WAIT) == 0 &&
	    !(rd32(RH_TCS + RH_IRQ_STATUS) & (1u << t));
}

/* ACTIVE AMC with n commands (addr, data). Returns 0 when it completed. */
static int rh_amc(unsigned n, const uint32_t *addrs, const uint32_t *vals)
{
	int t = -1;

	for (unsigned i = 0; i < RH_ACTIVE_N; i++)
		if (rh_free(i)) {
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
	uint64_t deadline = now() + 2 * ticks_per_s();	/* a BCM clock change can be slow */
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
			us(1);
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

/* VRM: voltage, mode 7 (HPM), enable last. */
static int rh_send(uint32_t addr, unsigned mv)
{
	const uint32_t a[3] = { addr, addr + 8, addr + 4 }, v[3] = { mv, 7, 1 };

	return rh_amc(3, a, v);
}

/*
 * Bus clock managers (cmd-db slave 5). Data as Linux's BCM_TCS_CMD: commit,
 * valid, vote_x (ab) << 14, vote_y (ib). Any non-zero vote turns the BCM on.
 */
#define RH_BCM		5u
static int bcm(const char *name, char *line)
{
	uint32_t addr = 0, val = (1u << 30) | (1u << 29) | (1u << 14) | 1u;
	int rc = rh_lookup(name, RH_BCM, &addr);

	if (rc == 0)
		rc = rh_amc(1, &addr, &val);
	line = put(line, name);
	if (rc)
		line = put_dec(put(line, "=X"), (uint32_t)-rc);
	line = put(line, " ");
	*line = 0;
	return rc;
}

static int vote(const char *res, unsigned mv)
{
	char b[64], *p;
	uint32_t addr = 0;
	int rc = rh_lookup(res, RH_VRM, &addr);

	if (rc == 0)
		rc = rh_send(addr, mv);
	p = put(put(b, res), " ");
	p = put_dec(p, mv);
	p = put_hex(put(p, "mV addr="), addr, 5);
	p = put(p, rc ? " FALHOU " : " ok");
	if (rc)
		put_dec(p, (uint32_t)-rc);
	con_str("RPMH", b);
	return rc;
}

/* ---- apps SMMU (MMU-500 behind Qualcomm's hypervisor) ---- */

/*
 * M7.8: with every bus vote in place the controller's first DMA still never
 * completes: ABL never uses USB on a normal boot, so the SMMU has no rule
 * for its stream (0x740). M7.9: reading sCR0 hangs: the hypervisor only
 * emulates part of the SMMU. Recipe from U-Boot drivers/iommu/qcom-hyp-smmu.c
 * (made for SDM845 USB): read only ID0/ID1, take a free context bank as
 * "S1 translate, S2 bypass" with translation off (SCTLR.M = 0, TTBR 0), and
 * map the stream to it with S2CR type TRANSLATE (the hypervisor turns
 * BYPASS into FAULT).
 */
#define SMMU		0x15000000UL
#define SMMU_USB_SID	0x740u
#define S_IDR0		0x020
#define S_IDR1		0x024
#define S_SMR(n)	(0x800 + 4 * (n))
#define S_S2CR(n)	(0xc00 + 4 * (n))
#define S_CBAR(n)	(0x000 + 4 * (n))	/* GR1 page */
#define S_CBA2R(n)	(0x800 + 4 * (n))	/* GR1 page */
#define SMR_VALID	(1u << 31)
#define CB_SCTLR	0x00
#define CB_TTBR0	0x20
#define CB_TTBR1	0x28
#define CB_TCR		0x30
#define CB_MAIR0	0x38
#define CB_MAIR1	0x3c

static int smmu_usb_bypass(void)
{
	char b[96], *p;

	step("le smmu ID0/ID1 (como o u-boot)");
	stage = "smmu";
	uint32_t idr0 = rd32(SMMU + S_IDR0), idr1 = rd32(SMMU + S_IDR1);
	unsigned nsmr = idr0 & 0xff, ncb = idr1 & 0xff;
	unsigned pgshift = (idr1 >> 31) ? 16 : 12;
	unsigned cbpg = 1u << (((idr1 >> 28) & 7) + 1);
	uintptr_t gr1 = SMMU + (1UL << pgshift);
	p = put_hex(put(b, "idr0="), idr0, 8);
	p = put_hex(put(p, " idr1="), idr1, 8);
	p = put_dec(put(p, " smr="), nsmr);
	put_dec(put(p, " cb="), ncb);
	con_str("SMMU", b);

	step("procura smr 0x740");
	for (unsigned n = 0; n < nsmr && n < 128; n++) {
		uint32_t smr = rd32(SMMU + S_SMR(n));
		if ((smr & SMR_VALID) && (smr & 0xffff) == SMMU_USB_SID) {
			p = put_dec(put(b, "ja existe smr "), n);
			put_hex(put(p, " s2cr="), rd32(SMMU + S_S2CR(n)), 8);
			con_str("SMMU", b);
			return 1;
		}
	}
	step("contexto livre");
	int cb = -1;
	for (unsigned i = 0; i < ncb && i < 128; i++) {
		uint32_t cbar = rd32(gr1 + S_CBAR(i));
		uint32_t type = (cbar >> 16) & 3, vmid = cbar & 0xff;
		if (type != 0 && (type != 1 || vmid != 0xff))
			continue;
		wr32(gr1 + S_CBAR(i), (cbar & ~(3u << 16) & ~0xffu) | (1u << 16));
		wr32(gr1 + S_CBA2R(i), rd32(gr1 + S_CBA2R(i)) | 1u);	/* VA64 */
		cb = (int)i;
		break;
	}
	if (cb < 0) {
		con_str("SMMU", "sem contexto livre");
		return 0;
	}
	uintptr_t cbb = SMMU + ((uintptr_t)(cbpg + (unsigned)cb) << pgshift);
	step("configura contexto");
	wr32(cbb + CB_TTBR0, 0);
	wr32(cbb + CB_TTBR0 + 4, 0);
	wr32(cbb + CB_TTBR1, 0);
	wr32(cbb + CB_TTBR1 + 4, 0);
	wr32(cbb + CB_MAIR0, 0);
	wr32(cbb + CB_MAIR1, 0);
	wr32(cbb + CB_SCTLR, (1u << 6) | (1u << 5) | (1u << 7));	/* CFIE CFRE CFCFG, M = 0 */
	wr32(cbb + CB_TCR, 0);
	dsb();
	step("smr + s2cr");
	int slot = -1;
	for (unsigned n = 0; n < nsmr && n < 128; n++)
		if (!(rd32(SMMU + S_SMR(n)) & SMR_VALID)) {
			slot = (int)n;
			break;
		}
	if (slot < 0) {
		con_str("SMMU", "sem smr livre");
		return 0;
	}
	wr32(SMMU + S_SMR(slot), SMR_VALID | SMMU_USB_SID);
	wr32(SMMU + S_S2CR(slot), (uint32_t)cb);	/* type TRANSLATE, cbndx */
	dsb();
	p = put_dec(put(b, "usb: smr "), (uint32_t)slot);
	p = put_hex(put(p, "="), rd32(SMMU + S_SMR(slot)), 8);
	p = put_dec(put(p, " cb "), (uint32_t)cb);
	put_hex(put(p, " s2cr="), rd32(SMMU + S_S2CR(slot)), 8);
	con_str("SMMU", b);
	return 1;
}

/* ---- MAX77705 USB switch ---- */

/*
 * M7.10: controller running, DMA fine, yet the PC never saw an attach.
 * The connector's D+/D- reach the SoC through the MAX77705's switch, and
 * Samsung's Linux driver opens it on every shutdown (max77705_muic_shutdown
 * -> com_to_open), so after TWRP reboots us the lines are disconnected.
 * Close it to "USB to AP" the way com_to_usb_ap does: USBC opcode
 * COMMAND_CONTROL1_WRITE (0x06) = COM_USB (0x09), written at 0x21 and
 * terminated by 0x41 = 0, on the I2C at 0x25 (QUP SE14 pins: GPIO33 SDA,
 * GPIO34 SCL), bit-banged like M5. Replies appear at 0x51.
 */
#define TLMM_NORTH	0x03900000UL
#define MX_SDA		33
#define MX_SCL		34
#define MX_ADDR		0x25
#define MX_OPCODE_W	0x21
#define MX_OPCODE_END	0x41
#define MX_OPCODE_R	0x51
#define MX_CTRL1_R	0x05
#define MX_CTRL1_W	0x06
#define MX_COM_USB	0x09

static uintptr_t gpio(unsigned pin)
{
	return TLMM_NORTH + 0x1000UL * pin;
}

static unsigned level(unsigned pin)
{
	return rd32(gpio(pin) + 4) & 1;
}

/* open drain by output enable, output latch 0, ~100 kHz (i2c-gpio style) */
static void bb_line(unsigned pin, int high)
{
	wr32(gpio(pin), high ? 0 : 1u << 9);
	us(5);
}

static int bb_scl_high(void)
{
	uint64_t end = now() + ticks_per_s() / 100;

	wr32(gpio(MX_SCL), 0);
	while (!level(MX_SCL))
		if (now() > end)
			return 0;
	us(5);
	return 1;
}

static int bb_start(void)
{
	bb_line(MX_SDA, 1);
	if (!bb_scl_high())
		return 0;
	bb_line(MX_SDA, 0);
	bb_line(MX_SCL, 0);
	return 1;
}

static void bb_stop(void)
{
	bb_line(MX_SDA, 0);
	bb_scl_high();
	bb_line(MX_SDA, 1);
}

/* 0 acked, 1 nacked, 2 clock stuck */
static int bb_write(uint8_t v)
{
	for (int i = 7; i >= 0; i--) {
		bb_line(MX_SDA, v >> i & 1);
		if (!bb_scl_high())
			return 2;
		bb_line(MX_SCL, 0);
	}
	bb_line(MX_SDA, 1);
	if (!bb_scl_high())
		return 2;
	int nack = level(MX_SDA);
	bb_line(MX_SCL, 0);
	return nack;
}

static int bb_read(int ack)
{
	unsigned v = 0;

	bb_line(MX_SDA, 1);
	for (int i = 0; i < 8; i++) {
		if (!bb_scl_high())
			return -1;
		v = v << 1 | level(MX_SDA);
		bb_line(MX_SCL, 0);
	}
	bb_line(MX_SDA, !ack);
	if (!bb_scl_high())
		return -1;
	bb_line(MX_SCL, 0);
	bb_line(MX_SDA, 1);
	return (int)v;
}

static int mx_wr(const uint8_t *buf, unsigned n)
{
	int err = 2;

	if (bb_start()) {
		err = bb_write(MX_ADDR << 1);
		for (unsigned i = 0; i < n && !err; i++)
			err = bb_write(buf[i]);
	}
	bb_stop();
	return err;
}

static int mx_rd(uint8_t reg, uint8_t *buf, unsigned n)
{
	int err = 2;

	if (!bb_start())
		goto out;
	err = bb_write(MX_ADDR << 1);
	if (!err)
		err = bb_write(reg);
	if (err)
		goto out;
	err = 2;
	if (!bb_start())
		goto out;
	err = bb_write(MX_ADDR << 1 | 1);
	for (unsigned i = 0; i < n && !err; i++) {
		int v = bb_read(i + 1 < n);
		if (v < 0)
			err = 2;
		else
			buf[i] = (uint8_t)v;
	}
out:
	bb_stop();
	return err;
}

static int mx_opcode(uint8_t op, int has_data, uint8_t data)
{
	uint8_t w[3] = { MX_OPCODE_W, op, data }, end[2] = { MX_OPCODE_END, 0 };
	int err = mx_wr(w, has_data ? 3 : 2);

	if (!err)
		err = mx_wr(end, 2);
	return err;
}

static int muic_usb(void)
{
	char b[96], *p;
	uint8_t id[2] = { 0, 0 }, r[2] = { 0, 0 };

	step("max77705: chave usb -> ap");
	stage = "max77705";
	wr32(gpio(MX_SDA) + 4, 0);	/* output latch 0: OE alone pulls low */
	wr32(gpio(MX_SCL) + 4, 0);
	wr32(gpio(MX_SDA), 0);		/* GPIO function, input, no pull */
	wr32(gpio(MX_SCL), 0);
	us(20);
	p = put_dec(put(b, "sda="), level(MX_SDA));
	p = put_dec(put(p, " scl="), level(MX_SCL));
	int e0 = mx_rd(0x00, id, 2);
	p = put_hex(put(p, " hw/fw="), (uint32_t)id[0] << 8 | id[1], 4);
	p = put_dec(put(p, " e="), (uint32_t)e0);
	int e1 = mx_opcode(MX_CTRL1_W, 1, MX_COM_USB);
	p = put_dec(put(p, " w="), (uint32_t)e1);
	delay_ms(50);
	int e2 = mx_opcode(MX_CTRL1_R, 0, 0);
	delay_ms(50);
	int e3 = e2 ? e2 : mx_rd(MX_OPCODE_R, r, 2);
	p = put_hex(put(p, " ctrl1="), (uint32_t)r[0] << 8 | r[1], 4);
	put_dec(put(p, " e="), (uint32_t)e3);
	con_str("MAX77705", b);
	return e1 == 0;
}

/* ---- GCC ---- */

static int clocks_on(void)
{
	char b[96], *p;
	int ok = 1;

	wr32(GCC + GDSC_USB30, rd32(GCC + GDSC_USB30) & ~1u);	/* SW_COLLAPSE off */
	dsb();
	int g = wait32(GCC + GDSC_USB30, 1u << 31, 1u << 31, 20000);
	/*
	 * Block reset while the clocks are still off, as dwc3_msm_link_clk_reset
	 * does (it gates iface/core/sleep/utmi around the BCR); M7 reset with the
	 * clocks running and the first DWC3 read hung.
	 */
	wr32(GCC + BCR_USB30, 1);
	dsb();
	delay_ms(1);
	wr32(GCC + BCR_USB30, 0);
	dsb();
	us(10);
	p = put_hex(put(b, "gdsc="), rd32(GCC + GDSC_USB30), 8);
	ok &= g;
	/* master RCG: source 1 (GPLL0 main 600 MHz), divider 4.5 (field 2*4.5-1) */
	wr32(GCC + RCG_MASTER_CFG, (1u << 8) | 8u);
	wr32(GCC + RCG_MASTER_CMD, rd32(GCC + RCG_MASTER_CMD) | 1u);
	dsb();
	int r = wait32(GCC + RCG_MASTER_CMD, 1u, 0, 10000);
	p = put_hex(put(p, " rcg="), rd32(GCC + RCG_MASTER_CMD), 8);
	ok &= r;
	for (unsigned i = 0; i < sizeof(usb_cbcr) / sizeof(usb_cbcr[0]); i++) {
		wr32(GCC + usb_cbcr[i], rd32(GCC + usb_cbcr[i]) | 1u);
		dsb();
		wait32(GCC + usb_cbcr[i], CLK_OFF, 0, 10000);
	}
	con_str("GCC", b);
	p = b;
	for (unsigned i = 0; i < sizeof(usb_cbcr) / sizeof(usb_cbcr[0]); i++) {
		uint32_t v = rd32(GCC + usb_cbcr[i]);
		p = put(put_hex(p, v, 8), " ");
		if (v & CLK_OFF)
			ok = 0;
	}
	con_str("CLOCKS", b);
	return ok;
}

/* ---- QUSB2 PHY ---- */

static int phy_on(void)
{
	char b[64], *p;

	wr32(GCC + BCR_QUSB2, 1);
	dsb();
	us(150);
	wr32(GCC + BCR_QUSB2, 0);
	dsb();
	us(100);
	wr32(AHB2PHY + 0x10, 0x11);	/* one read/write wait state, as dwc3-msm */
	wr32(QUSB2 + Q_PWR_CTRL1, rd32(QUSB2 + Q_PWR_CTRL1) | 1u);
	for (unsigned i = 0; i < sizeof(qusb_init) / sizeof(qusb_init[0]); i += 2)
		wr32(QUSB2 + qusb_init[i + 1], qusb_init[i]);
	dsb();
	wr32(QUSB2 + Q_PWR_CTRL1, rd32(QUSB2 + Q_PWR_CTRL1) & ~1u);
	dsb();
	int ok = wait32(QUSB2 + Q_PLL_STATUS, 1, 1, 5000);
	p = put_hex(put(b, "pll="), rd32(QUSB2 + Q_PLL_STATUS), 2);
	p = put_hex(put(p, " pwr="), rd32(QUSB2 + Q_PWR_CTRL1), 2);
	p = put_hex(put(p, " tune1="), rd32(QUSB2 + 0x240), 2);
	put(p, ok ? " PRONTO" : " SEM LOCK");
	con_str("QUSB2", b);
	return ok;
}

/* ---- DWC3 ---- */

static void dwc_wr(uint32_t off, uint32_t v)
{
	wr32(DWC + off, v);
	dsb();
}

static uint32_t dwc_rd(uint32_t off)
{
	return rd32(DWC + off);
}

static void qs_set(uint32_t off, uint32_t set, uint32_t clr)
{
	wr32(QS + off, (rd32(QS + off) & ~clr) | set);
	dsb();
	us(5);
}

static void utmi_for_pipe(void)
{
	qs_set(QS_GENERAL_CFG, PIPE_UTMI_CLK_DIS, 0);
	qs_set(QS_GENERAL_CFG, PIPE_UTMI_CLK_SEL | PIPE3_PHYSTATUS_SW, 0);
	qs_set(QS_GENERAL_CFG, 0, PIPE_UTMI_CLK_DIS);
}

static int epcmd(unsigned ep, uint32_t cmd, uint32_t p0, uint32_t p1, uint32_t p2)
{
	dwc_wr(DEPCMDPAR0(ep), p0);
	dwc_wr(DEPCMDPAR1(ep), p1);
	dwc_wr(DEPCMDPAR2(ep), p2);
	dwc_wr(DEPCMD(ep), cmd | DEPCMD_CMDACT);
	for (int i = 0; i < 1000; i++) {
		uint32_t v = dwc_rd(DEPCMD(ep));
		if (!(v & DEPCMD_CMDACT)) {
			if ((v >> 12) & 0xf) {
				u.cmd_err++;
				return -1;
			}
			return 0;
		}
		us(1);
	}
	u.cmd_err++;
	return -1;
}

static void start_trb(unsigned ep, uint32_t trb, uint32_t buf, uint32_t len, uint32_t ctl)
{
	volatile uint32_t *t = (volatile uint32_t *)(dma + trb);
	uint64_t bpa = (uintptr_t)dma + buf, tpa = (uintptr_t)dma + trb;

	t[0] = (uint32_t)bpa;
	t[1] = (uint32_t)(bpa >> 32);
	t[2] = len;
	t[3] = ctl | TRB_HWO | TRB_LST | TRB_ISP_IMI | TRB_IOC;
	dsb();
	epcmd(ep, DEPCMD_STARTTRANSFER, (uint32_t)(tpa >> 32), (uint32_t)tpa, 0);
}

static void start_ep0(unsigned ep, uint32_t buf, uint32_t len, uint32_t ctl)
{
	start_trb(ep, ep ? UD_TRB_IN : UD_TRB_OUT, buf, len, ctl);
}

static void start_setup(void)
{
	u.state = EP0_SETUP;
	start_ep0(0, UD_SETUP, 8, TRBCTL_SETUP);
}

/* Same descriptors as sweet_usb.c: CDC-ECM, pid.codes test VID/PID. */
static const uint8_t dev_desc[18] = {
	18, 1, 0x00, 0x02, 0x02, 0x00, 0x00, 64,
	0x09, 0x12, 0x01, 0x00, 0x00, 0x01, 1, 2, 3, 1,
};
static const uint8_t cfg_desc[80] = {
	9, 2, 80, 0, 2, 1, 0, 0x80, 250,
	9, 4, 0, 0, 1, 0x02, 0x06, 0x00, 0,
	5, 0x24, 0x00, 0x10, 0x01,
	5, 0x24, 0x06, 0, 1,
	13, 0x24, 0x0f, 4, 0, 0, 0, 0, 0xea, 0x05, 0, 0, 0,
	7, 5, 0x82, 0x03, 16, 0, 9,
	9, 4, 1, 0, 0, 0x0a, 0x00, 0x00, 0,
	9, 4, 1, 1, 2, 0x0a, 0x00, 0x00, 0,
	7, 5, 0x81, 0x02, 0x00, 0x02, 0,
	7, 5, 0x01, 0x02, 0x00, 0x02, 0,
};

static uint32_t string_desc(unsigned idx, uint8_t *out)
{
	const char *s;
	uint32_t n = 2;

	switch (idx) {
	case 0:
		out[0] = 4; out[1] = 3; out[2] = 0x09; out[3] = 0x04;
		return 4;
	case 1: s = "ios-s9"; break;
	case 2: s = "Galaxy S9 iOS (USB Ethernet)"; break;
	case 3: s = "0001"; break;
	case 4: s = "02005E77EE01"; break;
	default: return 0;
	}
	for (; *s && n < 126; s++) {
		out[n++] = (uint8_t)*s;
		out[n++] = 0;
	}
	out[0] = (uint8_t)n;
	out[1] = 3;
	return n;
}

static void configure(void)
{
	if (u.configured)
		return;
	if (epcmd(0, DEPCMD_DEPSTARTCFG | (2u << 16), 0, 0, 0) ||
	    epcmd(EP_RX, DEPCMD_SETEPCONFIG, (2u << 1) | (512u << 3), (1u << 8) | ((uint32_t)EP_RX << 25), 0) ||
	    epcmd(EP_RX, DEPCMD_SETTRANSFRESOURCE, 1, 0, 0) ||
	    epcmd(EP_TX, DEPCMD_SETEPCONFIG, (2u << 1) | (512u << 3) | (1u << 17), (1u << 8) | ((uint32_t)EP_TX << 25), 0) ||
	    epcmd(EP_TX, DEPCMD_SETTRANSFRESOURCE, 1, 0, 0) ||
	    epcmd(EP_INT, DEPCMD_SETEPCONFIG, (3u << 1) | (16u << 3) | (2u << 17),
	    (1u << 8) | (8u << 16) | ((uint32_t)EP_INT << 25), 0) ||
	    epcmd(EP_INT, DEPCMD_SETTRANSFRESOURCE, 1, 0, 0))
		return;
	dwc_wr(DALEPENA, dwc_rd(DALEPENA) | (1u << EP_RX) | (1u << EP_TX) | (1u << EP_INT));
	u.configured = 1;
}

static void setup(void)
{
	const uint8_t *st = dma + UD_SETUP;
	uint8_t *data = dma + UD_DATA;
	uint8_t type = st[0], req = st[1];
	uint16_t value = (uint16_t)(st[2] | st[3] << 8);
	uint16_t index = (uint16_t)(st[4] | st[5] << 8);
	uint16_t length = (uint16_t)(st[6] | st[7] << 8);
	uint32_t n = 0;
	int ok = 1, in = (type & 0x80) != 0;

	u.setups++;
	for (int i = 0; i < 8; i++)
		u.last[i] = st[i];
	if ((type & 0x60) == 0x20) {
		ok = length == 0;
	} else if (type & 0x60) {
		ok = 0;
	} else if (req == 6) {
		switch (value >> 8) {
		case 1: memcpy(data, dev_desc, sizeof(dev_desc)); n = sizeof(dev_desc); break;
		case 2: memcpy(data, cfg_desc, sizeof(cfg_desc)); n = sizeof(cfg_desc); break;
		case 3: n = string_desc(value & 0xff, data); ok = n != 0; break;
		default: ok = 0; break;
		}
	} else if (req == 5) {
		dwc_wr(DCFG, (dwc_rd(DCFG) & ~(0x7fu << 3)) | ((uint32_t)(value & 0x7f) << 3));
		u.address = value & 0x7f;
	} else if (req == 9) {
		if (value == 1)
			configure();
	} else if (req == 8) {
		data[0] = (uint8_t)u.configured; n = 1;
	} else if (req == 0) {
		data[0] = 0; data[1] = 0; n = 2;
	} else if (req == 10) {
		data[0] = 0; n = 1;
	} else if (req != 11 && req != 1 && req != 3) {
		ok = 0;
	}
	(void)index;
	if (!ok) {
		epcmd(0, DEPCMD_SETSTALL, 0, 0, 0);
		start_setup();
		return;
	}
	if (length == 0) {
		u.state = EP0_WAIT_STATUS2;
		return;
	}
	u.state = EP0_DATA;
	if (in)
		start_ep0(1, UD_DATA, n > length ? length : n, TRBCTL_DATA);
	else
		start_ep0(0, UD_DATA, 64, TRBCTL_DATA);
}

static void event(uint32_t ev)
{
	if (ev & 1) {
		uint32_t type = (ev >> 8) & 0xf;
		if (type == 1) {
			dwc_wr(DCFG, dwc_rd(DCFG) & ~(0x7fu << 3));
			u.resets++;
		} else if (type == 2) {
			u.speed = dwc_rd(DSTS) & 7;
			epcmd(0, DEPCMD_SETEPCONFIG, (64u << 3) | (2u << 30), (1u << 8) | (1u << 10), 0);
			epcmd(1, DEPCMD_SETEPCONFIG, (64u << 3) | (2u << 30), (1u << 8) | (1u << 10) | (1u << 25), 0);
			u.connects++;
		} else if (type == 0) {
			u.disconnects++;
		}
		return;
	}
	unsigned ep = (ev >> 1) & 0x1f, type = (ev >> 6) & 0xf, status = (ev >> 12) & 0xf;

	if (ep > 1)
		return;
	if (type == 1) {
		if (u.state == EP0_SETUP)
			setup();
		else if (u.state == EP0_DATA)
			u.state = EP0_WAIT_STATUS3;
		else if (u.state == EP0_STATUS)
			start_setup();
	} else if (type == 3 && status == 2 &&
	    (u.state == EP0_WAIT_STATUS2 || u.state == EP0_WAIT_STATUS3)) {
		uint32_t ctl = u.state == EP0_WAIT_STATUS3 ? TRBCTL_STATUS3 : TRBCTL_STATUS2;
		u.state = EP0_STATUS;
		start_ep0(ep, UD_DATA, 0, ctl);
	}
}

static int dwc_on(void)
{
	char b[96], *p;

	delay_ms(10);
	step("le qscratch 0xa6f8808");
	uint32_t gen = rd32(QS + QS_GENERAL_CFG);
	step("le GSNPSID 0xa60c120");
	uint32_t id = dwc_rd(GSNPSID);
	step("dwc3 soft reset");
	p = put_hex(put(b, "id="), id, 8);
	p = put_hex(put(p, " qs="), gen, 8);
	if ((id >> 16) != 0x5533) {
		put(p, " ?");
		con_str("DWC3", b);
		return 0;
	}
	utmi_for_pipe();
	dwc_wr(DCTL, DCTL_CSFTRST);
	int rs = wait32(DWC + DCTL, DCTL_CSFTRST, 0, 500000);
	p = put(p, rs ? " reset ok" : " reset PRESO");
	if (!rs) {
		con_str("DWC3", b);
		return 0;
	}
	step("dwc3 config");
	utmi_for_pipe();
	/* device mode, no clock gating (snps,disable-clk-gating) */
	dwc_wr(GCTL, (dwc_rd(GCTL) & ~(3u << 12)) | (2u << 12) | 1u);
	dwc_wr(GUSB3PIPECTL, dwc_rd(GUSB3PIPECTL) | (1u << 17));	/* SS PHY absent: suspend */
	/* 19.2 MHz reference clock (dwc3_msm_update_ref_clk) */
	dwc_wr(GUCTL, (dwc_rd(GUCTL) & ~(0x3ffu << 22)) | (52u << 22));
	dwc_wr(GFLADJ, (dwc_rd(GFLADJ) & ~(0xffu << 24 | 1u << 23 | 0x3fffu << 8)) |
	    (12u << 24) | (1u << 31) | (1u << 23) | (200u << 8));

	step("dwc3 ep0");
	dwc_wr(GEVNTADRLO, (uint32_t)(uintptr_t)(dma + UD_EVT));
	dwc_wr(GEVNTADRHI, 0);
	dwc_wr(GEVNTSIZ, (1u << 31) | UD_EVT_SIZE);
	dwc_wr(GEVNTCOUNT, dwc_rd(GEVNTCOUNT) & 0xffff);
	u.evt_pos = 0;
	dwc_wr(DCFG, dwc_rd(DCFG) & ~(7u | (0x7fu << 3)));	/* high speed, address 0 */
	dwc_wr(DEVTEN, 0x7);
	if (epcmd(0, DEPCMD_DEPSTARTCFG, 0, 0, 0) ||
	    epcmd(0, DEPCMD_SETEPCONFIG, 512u << 3, (1u << 8) | (1u << 10), 0) ||
	    epcmd(0, DEPCMD_SETTRANSFRESOURCE, 1, 0, 0) ||
	    epcmd(1, DEPCMD_SETEPCONFIG, 512u << 3, (1u << 8) | (1u << 10) | (1u << 25), 0) ||
	    epcmd(1, DEPCMD_SETTRANSFRESOURCE, 1, 0, 0)) {
		put(p, " ep0 FALHOU");
		con_str("DWC3", b);
		return 0;
	}
	dwc_wr(DALEPENA, 3);
	step("run A: trb setup (1o DMA)");
	start_setup();
	step("run B: le depcmd/evnt");
	{
		char *q = put_hex(put(b, "depcmd0="), dwc_rd(DEPCMD(0)), 8);
		q = put_hex(put(q, " evcnt="), dwc_rd(GEVNTCOUNT), 8);
		put_dec(put(q, " erro="), u.cmd_err);
		con_str("RUN", b);
	}
	step("run C: vbus override");
	qs_set(QS_HS_PHY_CTRL, UTMI_OTG_VBUS_VALID | SW_SESSVLD_SEL, 0);
	step("run D: run_stop");
	dwc_wr(DCTL, dwc_rd(DCTL) | DCTL_RUN_STOP);
	step("run E: espera halt=0");
	int run = wait32(DWC + DSTS, DSTS_DEVCTRLHLT, 0, 500000);
	step("run F: rodando");
	put(p, run ? " rodando" : " PARADO");
	con_str("DWC3", b);
	p = put_hex(put(b, "gctl="), dwc_rd(GCTL), 8);
	p = put_hex(put(p, " gsts="), dwc_rd(GSTS), 8);
	put_hex(put(p, " dsts="), dwc_rd(DSTS), 8);
	con_str("", b);
	return run;
}

static void usb_loop(void)
{
	uint64_t f = ticks_per_s(), t0 = now(), next = 0;
	int row = con_row();
	char b[96], *p;

	con_skip();
	con_skip();
	for (;;) {	/* M7.11: stays up, no reboot loop; the PC keeps seeing the device */
		uint32_t count = dwc_rd(GEVNTCOUNT) & 0xffff;
		if (count) {
			for (uint32_t done = 0; done < count; done += 4) {
				uint32_t ev = *(volatile uint32_t *)(dma + UD_EVT + u.evt_pos);
				u.evt_pos = (u.evt_pos + 4) % UD_EVT_SIZE;
				u.events++;
				event(ev);
			}
			dwc_wr(GEVNTCOUNT, count);
		}
		if (now() >= next) {
			next = now() + f / 5;
			con_set_row(row);
			p = put_dec(put(b, "eventos="), u.events);
			p = put_dec(put(p, " reset="), u.resets);
			p = put_dec(put(p, " conexao="), u.connects);
			p = put_dec(put(p, " vel="), u.speed);
			p = put_dec(put(p, " setup="), u.setups);
			p = put_dec(put(p, " end="), u.address);
			p = put_dec(put(p, " config="), u.configured);
			p = put_dec(put(p, " erro="), u.cmd_err);
			put(put_dec(put(p, " t="), (uint32_t)((now() - t0) / f)), "s");
			con_str("USB", b);
			p = b;
			for (int i = 0; i < 8; i++)
				p = put(put_hex(p, u.last[i], 2), " ");
			put_hex(put(p, " dsts="), dwc_rd(DSTS), 8);
			con_str("ULTIMO", b);
			wdog_pet();
		}
	}
	con_set_row(row + 3);
}

void shim_main(uint64_t dtb)
{
	char b[96];
	int ok = 0;

	(void)dtb;
	/* M1.1: 0x9d400000, 1440x2960, stride 5760, B G R A, autorefresh on */
	fb_default();
	con_clear(GREY);
	con_title("SHIM M7.11: CHAVE USB");
	stage = "watchdog";
	wdog_arm();
	step_row = con_row();
	con_skip();

	step("rpmh");
	stage = "rpmh";
	int rc = rh_init(b);
	con_str("RSC", b);
	if (rc)
		goto out;
	if (vote("ldoa5", 800) || vote("ldoa12", 1800) || vote("ldoa24", 3075))
		goto out;
	/*
	 * Bus paths Linux votes and ABL does not: USB DMA goes aggre2 -> system
	 * NoC (SN11) -> gem NoC (SN0) -> DDR (MC0/SH0); the SMMU's TCU config
	 * port is on SN6 (M5.3 hung reading it); the TBU's own vote is gladiator
	 * -> snoc (SN12) -> config NoC (CN0).
	 */
	step("votos de barramento");
	{
		/* + the SMMU TCU's own walks: mas-acm-tcu -> mem NoC = SH3 (M7.5 hung at the first TRB fetch) */
		/* M7.6: everything after SH1 failed in a cascade; SH3 first, essentials only */
		/* M7.7: SH3..SN0 ok, then SN6/SN11 timed out; the DMA path needs SH3 and SN11 together */
		static const char names[][5] = { "SH3", "SN11", "SN0", "SN6", "CN0", "MC0", "SH0", "SN12" };
		char line[160], *q = line;
		int bad = 0;
		for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
			bad |= bcm(names[i], q);
			while (*q)
				q++;
		}
		con_str("BCM", line);
		(void)bad;	/* a missing name shows as NAME=X; the rest still counts */
	}
	/* Reading the SMMU (0x15000000) still hangs with these votes (M7.2): not needed. */
	/*
	 * SMMU TBUs in front of aggre1/aggre2 (USB stream 0x740 is in tbu2's
	 * 0x400-0x7ff): Linux's arm-smmu votes their GDSCs on (DT vdd-supply
	 * hlos1_vote_aggre_noc_mmu_tbu1/2_gdsc); ABL leaves our vote off. M7.3
	 * hung at the controller's first DMA without them.
	 */
	step("votos tbu1/tbu2");
	{
		static const uint32_t tbu[2] = { 0x17d034, 0x17d038 };
		char *p = b;
		for (int i = 0; i < 2; i++) {
			uint32_t before = rd32(tbu[i]);
			wr32(tbu[i], before & ~1u);
			dsb();
			int on = wait32(tbu[i], 1u << 31, 1u << 31, 20000);
			p = put_hex(put(p, i ? " tbu2 " : "tbu1 "), before, 8);
			p = put(put_hex(put(p, "->"), rd32(tbu[i]), 8), on ? "" : "!");
		}
		con_str("TBU", b);
	}
	delay_ms(5);
	wdog_pet();

	if (!smmu_usb_bypass())
		goto out;
	step("gdsc e clocks");
	stage = "gcc";
	if (!clocks_on())
		goto out;
	step("phy qusb2");
	stage = "qusb2";
	if (!phy_on())
		goto out;
	muic_usb();
	wdog_pet();
	step("dwc3");
	stage = "dwc3";
	if (!dwc_on())
		goto out;
	wdog_pet();
	ok = 1;
	step("ligue o cabo no PC!");
	stage = "usb";
	usb_loop();
out:
	stage = "fim";
	step("fim");
	con_skip();
	con_str("", u.configured ? "USB OK! Tire uma foto." : ok ? "Sem enumerar. Foto!" : "Tire uma foto desta tela!");
	countdown_reboot(con_row(), END_SECONDS);
}
