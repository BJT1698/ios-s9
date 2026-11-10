/*
 * OnePlus 6 milestone M7: USB device mode (CDC-ECM ep0 enumeration) from the
 * state ABL's fastboot leaves, MMU off, polled. Proof = the PC's dmesg shows
 * "OnePlus 6 iOS (USB Ethernet)" (cdc_ether) while this screen counts setups.
 * Derived from the Galaxy S9 M7.11 (s9/shim/m7.c: USB from cold), minus the
 * MAX77705 switch the OP6 does not have, plus what `fastboot boot` leaves:
 * sweetusb=1 under XNU (op6-B2 log) saw GDSC + all clocks on, DWC3 3.00a in
 * device mode, run/stop 0 and halted (ABL disconnected), last link
 * SuperSpeed, QUSB2 PHY powered down (PWR_CTRL1 0x23, PLL not ready).
 *   1. Read-only dump of GCC / DWC3 / QSCRATCH / QUSB2 as ABL left them.
 *   2. RPMh (rpmh.c, DT cmd-db): L1A 880 mV, L12A 1800, L24A 3088 (live DT
 *      ldo1/ldo12/ldo24 of the hsphy), bus votes SH3 SN11 SN0 SN6 CN0 SN12
 *      at the top (as the kernel's 0106), TBU1/TBU2 GDSC votes.
 *   3. SMMU stream 0x740 (live DT usb@a600000 iommus): if no SMR holds it,
 *      the U-Boot qcom-hyp-smmu.c recipe the S9 proved (M7_SMMU=0 at build
 *      time only reports).
 *   4. GCC: nothing if ABL left it on, else the S9 cold sequence.
 *   5. QUSB2: clear POWER_DOWN (last step of Linux qusb2_phy_init), PLL
 *      lock; full init (BCR + the S9's DT sequence) only if it does not lock.
 *   6. DWC3: stop if running (+200 ms so the PC sees the disconnect), UTMI
 *      clock for the pipe side, soft reset, HS-only global setup, ep0, VBUS
 *      override, run. Then USB_SECONDS of event polling, then reboot.
 * Every step is on screen (PASSO) and in the ramoops console (ramlog).
 */
#include <stdint.h>
#include "hw.h"
#include "fb.h"
#include "con.h"
#include "plat.h"
#include "ramlog.h"
#include "i2c_bb.h"
#include "rpmh.h"

void *memcpy(void *d, const void *s, unsigned long n);

#ifndef M7_SMMU
#define M7_SMMU		1
#endif
#define USB_SECONDS	180
#define END_SECONDS	30
#define VRM_MODE_HPM	7

/* ---- GCC (gcc-sdm845.c) ---- */
#define GCC		0x00100000UL
#define GDSC_USB30	0xf004
#define RCG_MASTER_CMD	0xf018
#define RCG_MASTER_CFG	0xf01c
#define BCR_USB30	0xf000
#define BCR_QUSB2	0x12000
#define TBU1_VOTE	0x7d034
#define TBU2_VOTE	0x7d038
#define CLK_OFF		(1u << 31)

/* master, sleep, mock_utmi, cfg_noc, aggre, ahb2phy, clkref */
static const uint32_t usb_cbcr[] = { 0xf00c, 0xf010, 0xf014, 0x502c, 0x8201c, 0x6a004, 0x8c008 };
#define N_BRANCH	6	/* the first six are enabled on a cold start */

/* ---- PHY / DWC3 ---- */
#define AHB2PHY		0x088ee000UL
#define QUSB2		0x088e2000UL
#define Q_PWR_CTRL1	0x210
#define Q_PLL_STATUS	0x1a0
#define Q_PORT_TUNE1	0x240
#define Q_POWER_DOWN	1u

static const uint16_t qusb_init[] = {	/* S9 qcom,qusb-phy-init-seq: value, offset */
	0x23, 0x210, 0x03, 0x04, 0x7c, 0x18c, 0x80, 0x2c, 0x0a, 0x184, 0x19, 0xb4,
	0x40, 0x194, 0x1b, 0x198, 0x21, 0x214, 0x0e, 0x220, 0x58, 0x224, 0x75, 0x240,
	0x29, 0x244, 0xca, 0x248, 0x02, 0x24c, 0x03, 0x250, 0x00, 0x23c, 0x22, 0x210,
};

#define DWC		0x0a600000UL
#define QS		(DWC + 0xf8800)
#define QS_GENERAL_CFG	0x08
#define QS_HS_PHY_CTRL	0x10
#define QS_SS_PHY_CTRL	0x30
#define PIPE_UTMI_CLK_SEL	(1u << 0)
#define PIPE3_PHYSTATUS_SW	(1u << 3)
#define PIPE_UTMI_CLK_DIS	(1u << 8)
#define UTMI_OTG_VBUS_VALID	(1u << 20)
#define SW_SESSVLD_SEL		(1u << 28)
#define LANE0_PWR_PRESENT	(1u << 24)

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

/* ---- apps SMMU (MMU-500 behind Qualcomm's hypervisor) ---- */
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

static const char *stage;	/* set at runtime: no absolute pointers in data */
static int step_row;
static uint8_t dma[UD_SIZE] __attribute__((aligned(4096)));

static struct {
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

static void say(const char *label, const char *s)
{
	con_str(label, s);
	ramlog_str(label);
	ramlog_str(": ");
	ramlog_str(s);
	ramlog_str("\n");
}

/* Overwrites one line: if the shim hangs, the photo shows the last step. */
static void step(const char *s)
{
	char b[52], *p = put(b, s);
	int row = con_row();

	stage = s;
	ramlog_str("PASSO ");
	ramlog_str(s);
	ramlog_str("\n");
	while (p < b + 36)
		*p++ = ' ';
	*p = 0;
	con_set_row(step_row);
	con_str("PASSO", b);
	con_set_row(row);
}

static int wait32(uintptr_t a, uint32_t mask, uint32_t want, unsigned timeout_us)
{
	uint64_t end = now() + ticks_per_s() / 1000000 * timeout_us + 1;

	while ((rd32(a) & mask) != want)
		if (now() > end)
			return 0;
	return 1;
}

static void wr(uintptr_t a, uint32_t v)
{
	wr32(a, v);
	dsb();
}

/* ---- 1. state as ABL left it (read only) ---- */

static int gcc_on(int n)
{
	if (!(rd32(GCC + GDSC_USB30) & (1u << 31)))
		return 0;
	for (int i = 0; i < n; i++)
		if (rd32(GCC + usb_cbcr[i]) & CLK_OFF)
			return 0;
	return 1;
}

static void report_state(const char *label)
{
	char b[160], *p;

	p = put_hex(put(b, "gdsc="), rd32(GCC + GDSC_USB30), 8);
	for (unsigned i = 0; i < sizeof(usb_cbcr) / sizeof(usb_cbcr[0]); i++)
		p = put_hex(put(p, " "), rd32(GCC + usb_cbcr[i]), 8);
	say(label, b);
	/* DWC3 needs GDSC, master and cfg-noc; QUSB2 needs ahb2phy (an access to a gated block hangs) */
	if (!(rd32(GCC + GDSC_USB30) & (1u << 31)) || (rd32(GCC + 0xf00c) & CLK_OFF) ||
	    (rd32(GCC + 0x502c) & CLK_OFF)) {
		say(label, "dwc3 sem energia/clock: nao lido");
		return;
	}
	p = put_hex(put(b, "id="), rd32(DWC + GSNPSID), 8);
	p = put_hex(put(p, " gctl="), rd32(DWC + GCTL), 8);
	p = put_hex(put(p, " gsts="), rd32(DWC + GSTS), 8);
	p = put_hex(put(p, " u2="), rd32(DWC + GUSB2PHYCFG), 8);
	put_hex(put(p, " u3="), rd32(DWC + GUSB3PIPECTL), 8);
	say(label, b);
	p = put_hex(put(b, "dcfg="), rd32(DWC + DCFG), 8);
	p = put_hex(put(p, " dctl="), rd32(DWC + DCTL), 8);
	p = put_hex(put(p, " dsts="), rd32(DWC + DSTS), 8);
	p = put_hex(put(p, " evadr="), rd32(DWC + GEVNTADRLO), 8);
	put_hex(put(p, " evcnt="), rd32(DWC + GEVNTCOUNT), 8);
	say(label, b);
	p = put_hex(put(b, "qs gen="), rd32(QS + QS_GENERAL_CFG), 8);
	p = put_hex(put(p, " hs="), rd32(QS + QS_HS_PHY_CTRL), 8);
	put_hex(put(p, " ss="), rd32(QS + QS_SS_PHY_CTRL), 8);
	say(label, b);
	if (rd32(GCC + 0x6a004) & CLK_OFF) {
		say(label, "ahb2phy desligado: qusb2 nao lido");
		return;
	}
	p = put_hex(put(b, "qusb2 pwr="), rd32(QUSB2 + Q_PWR_CTRL1) & 0xff, 2);
	p = put_hex(put(p, " pll="), rd32(QUSB2 + Q_PLL_STATUS) & 0xff, 2);
	put_hex(put(p, " tune1="), rd32(QUSB2 + Q_PORT_TUNE1) & 0xff, 2);
	say(label, b);
}

/* ---- 2. RPMh ---- */

static void vote(const char *res, unsigned mv)
{
	char b[64], *p;
	uint32_t addr = 0;
	int rc = rh_lookup(res, RH_VRM, &addr);

	if (rc == 0)
		rc = rh_vrm_on(addr, mv, VRM_MODE_HPM);
	p = put(put(b, res), " ");
	p = put_dec(p, mv);
	p = put_hex(put(p, "mV addr="), addr, 5);
	p = put(p, rc ? " FALHOU " : " ok");
	if (rc)
		put_dec(p, (uint32_t)-rc);
	say("RPMH", b);
}

static int bus_votes(void)
{
	static const char names[][5] = { "SH3", "SN11", "SN0", "SN6", "CN0", "SN12" };
	char line[120], *p = line;
	int bad = 0;

	for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		uint32_t addr = 0;
		int rc = rh_lookup(names[i], RH_BCM, &addr);
		if (rc == 0)
			rc = rh_bcm_on(addr, 0x3fff);
		p = put(p, names[i]);
		if (rc)
			p = put_dec(put(p, "=X"), (uint32_t)-rc);
		p = put(p, " ");
		bad |= rc;
	}
	say("BCM", line);
	return bad;
}

static void tbu_votes(void)
{
	static const uint32_t tbu[2] = { TBU1_VOTE, TBU2_VOTE };
	char b[96], *p = b;

	for (int i = 0; i < 2; i++) {
		uint32_t before = rd32(GCC + tbu[i]);
		wr(GCC + tbu[i], before & ~1u);
		int on = wait32(GCC + tbu[i], 1u << 31, 1u << 31, 20000);
		p = put_hex(put(p, i ? " tbu2 " : "tbu1 "), before, 8);
		p = put(put_hex(put(p, "->"), rd32(GCC + tbu[i]), 8), on ? "" : "!");
	}
	say("TBU", b);
}

/* ---- 3. SMMU ---- */

/* 1 = stream 0x740 has a rule (found or made), 0 = not. */
static int smmu_usb(void)
{
	char b[96], *p;

	step("smmu: le ID0/ID1");
	uint32_t idr0 = rd32(SMMU + S_IDR0), idr1 = rd32(SMMU + S_IDR1);
	unsigned nsmr = idr0 & 0xff, ncb = idr1 & 0xff;
	unsigned pgshift = (idr1 >> 31) ? 16 : 12;
	unsigned cbpg = 1u << (((idr1 >> 28) & 7) + 1);
	uintptr_t gr1 = SMMU + (1UL << pgshift);
	p = put_hex(put(b, "idr0="), idr0, 8);
	p = put_hex(put(p, " idr1="), idr1, 8);
	p = put_dec(put(p, " smr="), nsmr);
	put_dec(put(p, " cb="), ncb);
	say("SMMU", b);

	step("smmu: procura smr 0x740");
	unsigned used = 0;
	for (unsigned n = 0; n < nsmr && n < 128; n++) {
		uint32_t smr = rd32(SMMU + S_SMR(n));
		if (!(smr & SMR_VALID))
			continue;
		used++;
		if ((smr & 0xffff) == SMMU_USB_SID) {
			p = put_dec(put(b, "ja existe smr "), n);
			p = put_hex(put(p, "="), smr, 8);
			put_hex(put(p, " s2cr="), rd32(SMMU + S_S2CR(n)), 8);
			say("SMMU", b);
			return 1;
		}
	}
	p = put_dec(put(b, "0x740 sem regra; smr em uso="), used);
	say("SMMU", b);
	if (!M7_SMMU)
		return 0;
	step("smmu: contexto livre");
	int cb = -1;
	for (unsigned i = 0; i < ncb && i < 128; i++) {
		uint32_t cbar = rd32(gr1 + S_CBAR(i));
		uint32_t type = (cbar >> 16) & 3, vmid = cbar & 0xff;
		if (type != 0 && (type != 1 || vmid != 0xff))
			continue;
		wr(gr1 + S_CBAR(i), (cbar & ~(3u << 16) & ~0xffu) | (1u << 16));	/* S1 translate, S2 bypass */
		wr(gr1 + S_CBA2R(i), rd32(gr1 + S_CBA2R(i)) | 1u);			/* VA64 */
		cb = (int)i;
		break;
	}
	if (cb < 0) {
		say("SMMU", "sem contexto livre");
		return 0;
	}
	uintptr_t cbb = SMMU + ((uintptr_t)(cbpg + (unsigned)cb) << pgshift);
	step("smmu: configura contexto");
	wr(cbb + CB_TTBR0, 0);
	wr(cbb + CB_TTBR0 + 4, 0);
	wr(cbb + CB_TTBR1, 0);
	wr(cbb + CB_TTBR1 + 4, 0);
	wr(cbb + CB_MAIR0, 0);
	wr(cbb + CB_MAIR1, 0);
	wr(cbb + CB_SCTLR, (1u << 6) | (1u << 5) | (1u << 7));	/* CFIE CFRE CFCFG, M = 0 */
	wr(cbb + CB_TCR, 0);
	step("smmu: smr + s2cr");
	int slot = -1;
	for (unsigned n = 0; n < nsmr && n < 128; n++)
		if (!(rd32(SMMU + S_SMR(n)) & SMR_VALID)) {
			slot = (int)n;
			break;
		}
	if (slot < 0) {
		say("SMMU", "sem smr livre");
		return 0;
	}
	wr(SMMU + S_SMR(slot), SMR_VALID | SMMU_USB_SID);
	wr(SMMU + S_S2CR(slot), (uint32_t)cb);	/* type TRANSLATE (BYPASS becomes FAULT), cbndx */
	p = put_dec(put(b, "usb: smr "), (uint32_t)slot);
	p = put_hex(put(p, "="), rd32(SMMU + S_SMR(slot)), 8);
	p = put_dec(put(p, " cb "), (uint32_t)cb);
	put_hex(put(p, " s2cr="), rd32(SMMU + S_S2CR(slot)), 8);
	say("SMMU", b);
	return 1;
}

/* ---- 4. GCC, only if ABL left it off (the S9 cold sequence) ---- */

static int clocks_cold(void)
{
	char b[96], *p;
	int ok = 1;

	wr(GCC + GDSC_USB30, rd32(GCC + GDSC_USB30) & ~1u);	/* SW_COLLAPSE off */
	ok &= wait32(GCC + GDSC_USB30, 1u << 31, 1u << 31, 20000);
	/* block reset with the clocks still off (S9 M7: with them on the first DWC3 read hung) */
	wr(GCC + BCR_USB30, 1);
	delay_ms(1);
	wr(GCC + BCR_USB30, 0);
	us_delay(10);
	wr(GCC + RCG_MASTER_CFG, (1u << 8) | 8u);	/* GPLL0 main / 4.5 = 133.33 MHz */
	wr(GCC + RCG_MASTER_CMD, rd32(GCC + RCG_MASTER_CMD) | 1u);
	ok &= wait32(GCC + RCG_MASTER_CMD, 1u, 0, 10000);
	for (int i = 0; i < N_BRANCH; i++) {
		wr(GCC + usb_cbcr[i], rd32(GCC + usb_cbcr[i]) | 1u);
		wait32(GCC + usb_cbcr[i], CLK_OFF, 0, 10000);
	}
	ok &= gcc_on(N_BRANCH);
	p = put_hex(put(b, "frio: gdsc="), rd32(GCC + GDSC_USB30), 8);
	put(p, ok ? " ok" : " FALHOU");
	say("GCC", b);
	return ok;
}

/* ---- 5. QUSB2 ---- */

static int phy_full_init(void)
{
	wr(GCC + BCR_QUSB2, 1);
	us_delay(150);
	wr(GCC + BCR_QUSB2, 0);
	us_delay(100);
	wr(AHB2PHY + 0x10, 0x11);	/* one read/write wait state, as dwc3-msm */
	wr(QUSB2 + Q_PWR_CTRL1, (rd32(QUSB2 + Q_PWR_CTRL1) & 0xff) | Q_POWER_DOWN);
	for (unsigned i = 0; i < sizeof(qusb_init) / sizeof(qusb_init[0]); i += 2)
		wr32(QUSB2 + qusb_init[i + 1], qusb_init[i]);
	dsb();
	wr(QUSB2 + Q_PWR_CTRL1, rd32(QUSB2 + Q_PWR_CTRL1) & 0xfe);
	us_delay(150);
	return wait32(QUSB2 + Q_PLL_STATUS, 1, 1, 5000);
}

static int phy_on(int cold)
{
	char b[96], *p;
	uint32_t pwr = rd32(QUSB2 + Q_PWR_CTRL1) & 0xff;
	int ok, full = 0;

	if (cold) {
		full = 1;
		ok = phy_full_init();
	} else if (!(pwr & Q_POWER_DOWN) && (rd32(QUSB2 + Q_PLL_STATUS) & 1)) {
		ok = 1;
	} else {
		wr(QUSB2 + Q_PWR_CTRL1, pwr & ~Q_POWER_DOWN);
		us_delay(150);
		ok = wait32(QUSB2 + Q_PLL_STATUS, 1, 1, 5000);
		if (!ok) {
			step("qusb2: init completo");
			full = 1;
			ok = phy_full_init();
		}
	}
	p = put_hex(put(b, "antes pwr="), pwr, 2);
	p = put_hex(put(p, " agora pwr="), rd32(QUSB2 + Q_PWR_CTRL1) & 0xff, 2);
	p = put_hex(put(p, " pll="), rd32(QUSB2 + Q_PLL_STATUS) & 0xff, 2);
	p = put_hex(put(p, " tune1="), rd32(QUSB2 + Q_PORT_TUNE1) & 0xff, 2);
	p = put(p, full ? " (init completo)" : " (so power-up)");
	put(p, ok ? " PRONTO" : " SEM LOCK");
	say("QUSB2", b);
	return ok;
}

/* ---- 6. DWC3 + ep0 (sweet_usb.c's state machine) ---- */

static void dwc_wr(uint32_t off, uint32_t v)
{
	wr(DWC + off, v);
}

static uint32_t dwc_rd(uint32_t off)
{
	return rd32(DWC + off);
}

static void qs_set(uint32_t off, uint32_t set, uint32_t clr)
{
	wr(QS + off, (rd32(QS + off) & ~clr) | set);
	us_delay(5);
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
		us_delay(1);
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

/* Same descriptors as sweet_usb.c: CDC-ECM, pid.codes test VID/PID 1209:0001. */
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
	case 1: s = "ios-op6"; break;
	case 2: s = "OnePlus 6 iOS (USB Ethernet)"; break;
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

	step("dwc3: GSNPSID");
	uint32_t id = dwc_rd(GSNPSID);
	p = put_hex(put(b, "id="), id, 8);
	if ((id >> 16) != 0x5533) {
		put(p, " ?");
		say("DWC3", b);
		return 0;
	}
	if (dwc_rd(DCTL) & DCTL_RUN_STOP) {
		/* ABL's gadget still connected: drop the pull-up, let the PC see it go */
		step("dwc3: para o gadget do ABL");
		dwc_wr(DEVTEN, 0);	/* events into our buffer, not ABL's */
		dwc_wr(GEVNTADRLO, (uint32_t)(uintptr_t)(dma + UD_EVT));
		dwc_wr(GEVNTADRHI, (uint32_t)((uint64_t)(uintptr_t)(dma + UD_EVT) >> 32));
		dwc_wr(GEVNTSIZ, (1u << 31) | UD_EVT_SIZE);
		dwc_wr(GEVNTCOUNT, dwc_rd(GEVNTCOUNT) & 0xffff);
		dwc_wr(DCTL, dwc_rd(DCTL) & ~DCTL_RUN_STOP);
		int h = wait32(DWC + DSTS, DSTS_DEVCTRLHLT, DSTS_DEVCTRLHLT, 500000);
		p = put(p, h ? " parado" : " NAO PAROU");
		delay_ms(200);
	}
	step("dwc3: utmi p/ pipe + soft reset");
	utmi_for_pipe();
	dwc_wr(DCTL, DCTL_CSFTRST);
	int rs = wait32(DWC + DCTL, DCTL_CSFTRST, 0, 500000);
	p = put(p, rs ? " reset ok" : " reset PRESO");
	if (!rs) {
		say("DWC3", b);
		return 0;
	}
	step("dwc3: config so HS");
	utmi_for_pipe();
	dwc_wr(GCTL, (dwc_rd(GCTL) & ~(3u << 12)) | (2u << 12) | 1u);	/* device, no clock gating */
	dwc_wr(GUSB3PIPECTL, dwc_rd(GUSB3PIPECTL) | (1u << 17));	/* SS side unused: suspend */
	dwc_wr(GUCTL, (dwc_rd(GUCTL) & ~(0x3ffu << 22)) | (52u << 22));	/* 19.2 MHz ref clock */
	dwc_wr(GFLADJ, (dwc_rd(GFLADJ) & ~(0xffu << 24 | 1u << 23 | 0x3fffu << 8)) |
	    (12u << 24) | (1u << 31) | (1u << 23) | (200u << 8));

	step("dwc3: ep0");
	dwc_wr(GEVNTADRLO, (uint32_t)(uintptr_t)(dma + UD_EVT));
	dwc_wr(GEVNTADRHI, (uint32_t)((uint64_t)(uintptr_t)(dma + UD_EVT) >> 32));
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
		say("DWC3", b);
		return 0;
	}
	dwc_wr(DALEPENA, 3);
	step("dwc3: 1o DMA (trb setup)");
	start_setup();
	step("dwc3: vbus override");
	qs_set(QS_HS_PHY_CTRL, UTMI_OTG_VBUS_VALID | SW_SESSVLD_SEL, 0);
	qs_set(QS_SS_PHY_CTRL, LANE0_PWR_PRESENT, 0);
	step("dwc3: run_stop");
	dwc_wr(DCTL, dwc_rd(DCTL) | DCTL_RUN_STOP);
	int run = wait32(DWC + DSTS, DSTS_DEVCTRLHLT, 0, 500000);
	p = put_dec(put(p, run ? " rodando erro=" : " PARADO erro="), u.cmd_err);
	say("DWC3", b);
	p = put_hex(put(b, "gctl="), dwc_rd(GCTL), 8);
	p = put_hex(put(p, " dcfg="), dwc_rd(DCFG), 8);
	put_hex(put(p, " dsts="), dwc_rd(DSTS), 8);
	say("DWC3", b);
	return run;
}

static void usb_loop(void)
{
	uint64_t f = ticks_per_s(), t0 = now(), next = 0, end = t0 + f * USB_SECONDS;
	uint32_t logged = 0;
	int row = con_row();
	char b[120], *p;

	con_skip();
	con_skip();
	while (now() < end) {
		uint32_t count = dwc_rd(GEVNTCOUNT) & 0xffff;
		if (count) {
			for (uint32_t done = 0; done < count; done += 4) {
				uint32_t ev = *(volatile uint32_t *)(dma + UD_EVT + u.evt_pos);
				u.evt_pos = (u.evt_pos + 4) % UD_EVT_SIZE;
				u.events++;
				if (logged++ < 64) {
					ramlog_str("EV ");
					ramlog_hex(ev, 8);
					ramlog_str("\n");
				}
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
	p = put_dec(put(b, "fim: setup="), u.setups);
	p = put_dec(put(p, " end="), u.address);
	put_dec(put(p, " config="), u.configured);
	say("USB", b);
}

void shim_main(uint64_t dtb)
{
	char b[96];
	int ok = 0, cold = 0;

	(void)dtb;
	ramlog_open();
	ramlog_str("\n=== OP6 shim M7 usb ===\n");
	fb_default();
	con_clear(GREY);
	con_title("OP6 SHIM M7: USB (ESTADO DO ABL)");
	stage = "watchdog";
	wdog_arm();
	step_row = con_row();
	con_skip();

	step("estado do abl (so leitura)");
	report_state("ABL");

	step("rpmh");
	int rc = rh_init(b);
	say("RSC", b);
	if (rc)
		goto out;
	vote("ldoa1", 880);
	vote("ldoa12", 1800);
	vote("ldoa24", 3088);
	step("rpmh: votos de barramento");
	if (bus_votes()) {
		say("BCM", "voto falhou: smmu nao tocada (S9: leitura sem votos trava)");
		goto out;
	}
	step("gcc: votos tbu1/tbu2");
	tbu_votes();
	wdog_pet();

	if (!smmu_usb() && M7_SMMU)
		goto out;
	wdog_pet();

	step("gcc");
	if (gcc_on(N_BRANCH)) {
		say("GCC", "ligado como o ABL deixou (quente)");
	} else if (rd32(GCC + GDSC_USB30) & (1u << 31)) {
		/* BCR with clocks running hung the S9: not handled */
		say("GCC", "gdsc ligado mas clocks parciais: parado");
		goto out;
	} else {
		cold = 1;
		if (!clocks_cold())
			goto out;
	}
	step("qusb2");
	if (!phy_on(cold))
		goto out;
	wdog_pet();
	if (!dwc_on())
		goto out;
	wdog_pet();
	ok = 1;
	step("olhe o dmesg do PC!");
	usb_loop();
out:
	step("fim");
	con_skip();
	say("", u.configured ? "USB OK! Tire uma foto." : ok ? "Sem enumerar. Foto!" : "Tire uma foto desta tela!");
	countdown_reboot(con_row(), END_SECONDS);
}
