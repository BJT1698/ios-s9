/*
 * OnePlus 6 milestone M5: the touchscreen before XNU, polled, MMU off.
 * Synaptics S3706B, RMI4 at 0x20 on i2c12's pins (DT i2c@a90000,
 * qup-i2c12-default-state: GPIO49 SDA, GPIO50 SCL), attention on GPIO125
 * (falling edge, active low), DT ts-int-state = GPIO99 + GPIO125 as GPIO
 * inputs with pull-up, 16 mA. Supplies: vdd = PM8998 LDO28 (RPMh, DT range
 * 2.856-3.008 V, HPM), vio = ts_1p8_supply, a fixed regulator on GPIO88
 * (enable-active-high). The OnePlus kernel (LineageOS
 * android_kernel_oneplus_sdm845, enchilada.dtsi + synaptics_driver_s3320.c)
 * names GPIO99 the reset line (active low) and powers up as: L28 3.008 V,
 * 10 ms, GPIO88 high, 10 ms, reset 1/0/1 10 ms apart, 100 ms.
 * The S9 found the GENI engines need SMMU and bus votes, so the two wires
 * are bit-banged here (i2c_bb.c), ~60-80 kHz.
 *   1. Report every pin as ABL left it, then probe 0x20 without touching
 *      power. Only if nobody answers: (a) the DT's ts-int-state on 99/125
 *      (releases a reset held low), (b) GPIO88 high + reset pulse, (c) an
 *      RPMh vote for ldoa28 + reset pulse, probing after each.
 *   2. RMI4: scan the PDT, F01 identity/status, F01 ctrl0 = no sleep |
 *      configured, enable the F12 interrupt bit, F12 register descriptors
 *      -> data1 records and max X/Y (control 8). F11 if there is no F12.
 *   3. 60 s: read whenever GPIO125 is low (or every 10 ms), a square under
 *      each finger, live coordinates.
 * Every step is on screen (PASSO) and in the ramoops console (ramlog) before
 * it runs; the APSS watchdog turns a hang into a reset.
 */
#include <stdint.h>
#include "hw.h"
#include "fb.h"
#include "con.h"
#include "plat.h"
#include "ramlog.h"
#include "i2c_bb.h"
#include "rmi4.h"
#include "rpmh.h"

#define TOUCH_SECONDS	60
#define END_SECONDS	30

#define TS_SDA		49
#define TS_SCL		50
#define TS_IRQ		125
#define TS_GPIO99	99
#define TS_VIO_EN	88
#define TS_ADDR		0x20
#define HALF_US		5

#define TS_INT_CTL	(TLMM_PULL_UP | 7u << 6)	/* gpio, pull-up, 16 mA, input */
#define STARTUP_MS	200				/* syna,startup-delay-ms */
#define LDO28_MV	3008		/* OEM avdd-voltage; mainline DT max */
#define VRM_MODE_HPM	7				/* PMIC4_LDO_MODE_HPM */

#define F01_NOSLEEP	(1u << 2)
#define F01_CONFIGURED	(1u << 7)
#define PKT_MAX		168

static const char *stage;	/* set at runtime: no absolute pointers in data */
static int step_row;
static struct rmi rmi;
static struct rmi_desc desc;
static int pinctrl_done;

void exc_handler(uint64_t slot, uint64_t *regs)
{
	ramlog_str("EXCECAO em ");
	ramlog_str(stage ? stage : "?");
	ramlog_str(" ESR=");
	ramlog_hex(rsr("esr_el1"), 8);
	ramlog_str(" ELR=");
	ramlog_hex(rsr("elr_el1"), 9);
	ramlog_str(" FAR=");
	ramlog_hex(rsr("far_el1"), 9);
	ramlog_str("\n");
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
	wdog_pet();
}

static char *pin_state(char *p, unsigned pin)
{
	uintptr_t c = tlmm_ctl(pin);

	p = put_dec(p, pin);
	p = put_hex(put(p, " c="), rd32(c) & 0xffff, 4);
	return put_hex(put(p, " io="), rd32(c + TLMM_IO) & 3, 1);
}

static void report_pins(void)
{
	uintptr_t irq = tlmm_ctl(TS_IRQ);
	char b[64], *p;

	p = pin_state(b, TS_SDA);
	pin_state(put(p, "  "), TS_SCL);
	say("GPIO", b);
	p = pin_state(b, TS_GPIO99);
	pin_state(put(p, "  "), TS_VIO_EN);
	say("GPIO", b);
	p = pin_state(b, TS_IRQ);
	p = put_hex(put(p, " cfg="), rd32(irq + TLMM_INTR_CFG) & 0xffff, 4);
	put_hex(put(p, " st="), rd32(irq + TLMM_INTR_ST) & 1, 1);
	say("GPIO", b);
}

static void lines(char *b)
{
	char *p = put_dec(put(b, "sda="), bb_sda(&rmi.bus));

	p = put_dec(put(p, " scl="), bb_scl(&rmi.bus));
	put_dec(put(p, " irq="), tlmm_in(TS_IRQ));
}

/* Address-only write to 0x20, up to three tries. Logs one SONDA line. */
static int probe(const char *what)
{
	char b[64], *p;
	int err = E_SCL;

	p = put(put(b, what), ": ");
	if (!bb_sda(&rmi.bus) && bb_scl(&rmi.bus)) {
		int r = bb_recover(&rmi.bus);
		p = put_dec(put(p, "rec="), (uint32_t)r);
		p = put(p, " ");
	}
	if (!bb_sda(&rmi.bus) || !bb_scl(&rmi.bus)) {
		p = put(p, "linha em 0 ");
		lines(p);
		say("SONDA", b);
		return E_SCL;
	}
	for (int i = 0; i < 3 && err; i++) {
		err = bb_probe(&rmi.bus, TS_ADDR);
		if (err)
			delay_ms(2);
	}
	p = put(p, err == 0 ? "ACK " : err == E_ADDR ? "NACK " : "SCL? ");
	lines(p);
	say("SONDA", b);
	return err;
}

/* The DT's ts-int-state, as Linux's pinctrl applies it before probe. */
static void ts_pinctrl(void)
{
	wr32(tlmm_ctl(TS_GPIO99), TS_INT_CTL);
	wr32(tlmm_ctl(TS_IRQ), TS_INT_CTL);
	pinctrl_done = 1;
}

/* GPIO88 as a GPIO output driving 1 (regulator-fixed, enable-active-high). */
static void vio_on(void)
{
	uintptr_t c = tlmm_ctl(TS_VIO_EN);

	wr32(c + TLMM_IO, 2);
	wr32(c, (rd32(c) & ~TLMM_FUNC_MASK) | TLMM_OE);
}

/*
 * GPIO99 as the OEM driver drives it (tpd_hw_pwron): output 1, 0, 1 with
 * 10 ms between, left driving 1; pull-up and 16 mA as ts_active.
 */
static void reset_pulse(void)
{
	uintptr_t c = tlmm_ctl(TS_GPIO99);

	wr32(c + TLMM_IO, 2);
	wr32(c, TS_INT_CTL | TLMM_OE);
	delay_ms(10);
	wr32(c + TLMM_IO, 0);
	delay_ms(10);
	wr32(c + TLMM_IO, 2);
}

static int ldo28_on(void)
{
	char b[80], *p;
	uint32_t addr = 0;
	int rc;

	step("rpmh: rsc + cmd-db");
	rc = rh_init(b);
	say("RPMH", b);
	if (rc)
		return rc;
	step("rpmh: lookup ldoa28");
	rc = rh_lookup("ldoa28", RH_VRM, &addr);
	p = put_hex(put(b, "ldoa28 addr="), addr, 5);
	p = put_dec(put(p, " rc="), (uint32_t)-rc);
	p = put_hex(put(p, " db="), rh_db_base(), 8);
	say("RPMH", b);
	if (rc)
		return rc;
	step("rpmh: voto ldoa28 3008mV HPM on");
	rc = rh_vrm_on(addr, LDO28_MV, VRM_MODE_HPM);
	p = put_dec(put(b, "3008 mV modo 7 on: "), (uint32_t)-rc);
	put(p, rc ? " FALHOU" : " ok");
	say("RPMH", b);
	return rc;
}

/* Probe; if silent, add what Linux would have set up, one thing at a time. */
static int power_up(void)
{
	char b[48];

	step("sonda 0x20 sem mexer em energia");
	if (probe("inicial") == 0)
		return 0;

	step("pinctrl ts-int-state 99/125");
	ts_pinctrl();
	delay_ms(STARTUP_MS);
	if (probe("pinctrl") == 0)
		return 1;

	step("gpio88=1 (vio 1.8V) + reset 99");
	vio_on();
	delay_ms(10);
	reset_pulse();
	delay_ms(STARTUP_MS);
	pin_state(b, TS_VIO_EN);
	say("VIO", b);
	if (probe("gpio88=1") == 0)
		return 2;

	if (ldo28_on())
		return -1;
	step("ldo28: 10ms, reset 99, 200ms");
	delay_ms(10);
	reset_pulse();
	delay_ms(STARTUP_MS);
	pin_state(b, TS_GPIO99);
	say("RESET", b);
	if (probe("ldo28") == 0)
		return 3;
	delay_ms(300);
	if (probe("ldo28 +300ms") == 0)
		return 3;
	return -1;
}

static char *fn_line(char *p, const struct rmi_fn *f)
{
	p = put_hex(put(p, "F"), f->num, 2);
	p = put_dec(put(p, " p"), f->page);
	p = put_hex(put(p, " q"), f->query, 2);
	p = put_hex(put(p, " k"), f->cmd, 2);
	p = put_hex(put(p, " c"), f->ctrl, 2);
	p = put_hex(put(p, " d"), f->data, 2);
	p = put_dec(put(p, " i"), f->irqs);
	p = put_dec(put(p, "@"), f->irq_pos);
	return put_dec(put(p, " v"), f->version);
}

static int f01_setup(const struct rmi_fn *f01, const struct rmi_fn *f2d)
{
	uint8_t q[21], st = 0, c0 = 0, c0b = 0, en[4] = { 0, 0, 0, 0 }, en2[4] = { 0, 0, 0, 0 };
	unsigned nen = (rmi.irq_count + 7) / 8;
	char b[64], *p;
	int err;

	step("f01: query 0-20");
	err = rmi_read(&rmi, rmi_addr(f01, f01->query), q, sizeof(q));
	if (err) {
		put_dec(put(b, "query erro="), (uint32_t)err);
		say("F01", b);
		return err;
	}
	p = put_hex(put(b, "fab="), q[0], 2);
	p = put_hex(put(p, " prop="), q[1], 2);
	p = put(p, " id=");
	for (int i = 11; i < 21 && q[i]; i++)
		*p++ = q[i] >= 0x20 && q[i] < 0x7f ? (char)q[i] : '.';
	*p = 0;
	say("F01", b);
	p = put_dec(put(b, "data 20"), q[4] & 0x1f);
	p = put_dec(put(p, "/"), q[5] & 0x0f);
	p = put_dec(put(p, "/"), q[6] & 0x1f);
	p = put_hex(put(p, " info="), (uint32_t)(q[2] & 0x7f) << 7 | (q[3] & 0x7f), 4);
	say("F01", b);

	step("f01: status e ctrl0");
	err = rmi_read(&rmi, rmi_addr(f01, f01->data), &st, 1);
	if (!err)
		err = rmi_read(&rmi, rmi_addr(f01, f01->ctrl), &c0, 1);
	if (err) {
		put_dec(put(b, "status/ctrl0 erro="), (uint32_t)err);
		say("F01", b);
		return err;
	}
	p = put_hex(put(b, "status="), st, 2);
	p = put_hex(put(p, " (cod "), st & 0xf, 1);
	p = put(p, st & 0x40 ? " BOOTLOADER" : "");
	p = put(p, st & 0x80 ? " desconfig" : "");
	p = put_hex(put(p, ") ctrl0="), c0, 2);
	say("F01", b);

	step("f01: ctrl0 nosleep|configured");
	uint8_t v = (uint8_t)((c0 & ~3u) | F01_NOSLEEP | F01_CONFIGURED);
	err = rmi_write(&rmi, rmi_addr(f01, f01->ctrl), &v, 1);
	if (!err)
		err = rmi_read(&rmi, rmi_addr(f01, f01->ctrl), &c0b, 1);
	p = put_hex(put(b, "ctrl0 "), c0, 2);
	p = put_hex(put(p, " -> "), v, 2);
	p = put_hex(put(p, " lido "), c0b, 2);
	put_dec(put(p, " erro="), (uint32_t)err);
	say("F01", b);

	if (!f2d || nen == 0 || nen > 4)
		return 0;
	step("f01: habilita irq do 2D");
	err = rmi_read(&rmi, (uint16_t)(rmi_addr(f01, f01->ctrl) + 1), en, nen);
	if (!err) {
		for (unsigned i = 0; i < nen; i++)
			en2[i] = en[i];
		en2[f2d->irq_pos / 8] |= (uint8_t)(1u << (f2d->irq_pos % 8));
		err = rmi_write(&rmi, (uint16_t)(rmi_addr(f01, f01->ctrl) + 1), en2, nen);
	}
	if (!err)
		err = rmi_read(&rmi, (uint16_t)(rmi_addr(f01, f01->ctrl) + 1), en2, nen);
	p = put(b, "irq en ");
	for (unsigned i = 0; i < nen; i++)
		p = put_hex(p, en[nen - 1 - i], 2);
	p = put(p, " -> ");
	for (unsigned i = 0; i < nen; i++)
		p = put_hex(p, en2[nen - 1 - i], 2);
	put_dec(put(p, " erro="), (uint32_t)err);
	say("F01", b);
	return 0;
}

/* Spaces to 35 columns from start (end = the string's NUL), so a shorter
 * value overwrites a longer one on the same row. */
static void pad(char *start, char *end)
{
	while (end < start + 35)
		*end++ = ' ';
	*end = 0;
}

static uint32_t finger_color(unsigned i)
{
	switch (i & 3) {
	case 0: return GREEN;
	case 1: return RED;
	case 2: return YELLOW;
	default: return BLUE;
	}
}

struct sensor {
	int is_f12;
	uint16_t data_addr, rec_off, max_x, max_y;
	uint8_t fingers, rec_size, state_bytes;
	unsigned read_len;
};

static int scale(unsigned v, unsigned max, unsigned panel)
{
	unsigned p = max ? v * (panel - 1) / max : v;

	return (int)(p >= panel ? panel - 1 : p);
}

/* Decodes object i of a packet: 1 if a finger (or other object) is down. */
static int object(const struct sensor *s, const uint8_t *pkt, unsigned i,
		  unsigned *x, unsigned *y, unsigned *z, unsigned *type)
{
	if (s->is_f12) {
		const uint8_t *r = pkt + s->rec_off + 8 * i;
		*type = r[0];
		*x = r[1] | (unsigned)r[2] << 8;
		*y = r[3] | (unsigned)r[4] << 8;
		*z = r[5];
		return r[0] != 0;
	}
	unsigned state = pkt[i / 4] >> (2 * (i % 4)) & 3;
	const uint8_t *r = pkt + s->state_bytes + 5 * i;
	*type = state;
	*x = (unsigned)r[0] << 4 | (r[2] & 0xf);
	*y = (unsigned)r[1] << 4 | r[2] >> 4;
	*z = r[4];
	return state != 0;
}

static void touch_loop(const struct sensor *s, const struct rmi_fn *f01, const struct rmi_fn *f2d)
{
	uint64_t f = ticks_per_s(), t0 = now(), last = 0, shown_at = 0;
	unsigned reads = 0, irq_low = 0, irq_hits = 0, objs = 0, errors = 0;
	unsigned nst = (rmi.irq_count + 7) / 8, logged = 0;
	uint8_t pkt[PKT_MAX], st[4];
	char b[64], d0[64], d1[64], *p;
	int row = con_row();

	pad(d0, put(d0, "(encoste o dedo na tela)"));
	pad(d1, d1);
	if (nst > 4)
		nst = 4;
	while (now() - t0 < f * TOUCH_SECONDS) {
		int low = !tlmm_in(TS_IRQ);

		if (!low && now() - last < f / 100) {
			us_delay(200);
			goto status;
		}
		last = now();
		irq_low += low;
		int err = rmi_read(&rmi, (uint16_t)(rmi_addr(f01, f01->data) + 1), st, nst);
		int hit = !err && f2d->irq_pos / 8 < nst &&
			  (st[f2d->irq_pos / 8] >> (f2d->irq_pos % 8) & 1);
		irq_hits += hit;
		if (!err)
			err = rmi_read(&rmi, s->data_addr, pkt, s->read_len);
		reads++;
		if (err) {
			errors++;
			goto status;
		}
		unsigned shown = 0;
		for (unsigned i = 0; i < s->fingers; i++) {
			unsigned x, y, z, type;
			if (!object(s, pkt, i, &x, &y, &z, &type))
				continue;
			int px = scale(x, s->max_x, PANEL_W), py = scale(y, s->max_y, PANEL_H);
			uint32_t c = finger_color(i);
			fb_rect(px - 12, py - 12, 24, 24, c);
			fb_rect(px - 40, py - 1, 80, 3, c);
			fb_rect(px - 1, py - 40, 3, 80, c);
			objs++;
			if (shown < 2) {
				char *q0 = shown ? d1 : d0, *q = q0;
				q = put_dec(q, i);
				q = put_dec(put(q, " t"), type);
				q = put_dec(put(q, " x"), x);
				q = put_dec(put(q, " y"), y);
				q = put_dec(put(q, " z"), z);
				q = put_dec(put(q, " >"), (uint32_t)px);
				pad(q0, put_dec(put(q, ","), (uint32_t)py));
				shown++;
			}
			if (logged < 40) {
				logged++;
				ramlog_str("dedo ");
				ramlog_dec(i);
				ramlog_str(" x=");
				ramlog_dec(x);
				ramlog_str(" y=");
				ramlog_dec(y);
				ramlog_str(" z=");
				ramlog_dec(z);
				ramlog_str("\n");
			}
		}
status:
		wdog_pet();
		if (now() - shown_at >= f / 4) {
			shown_at = now();
			con_set_row(row);
			p = put_dec(put(b, "lei="), reads);
			p = put_dec(put(p, " bit="), irq_hits);
			pad(b, put_dec(put(p, " err="), errors));
			con_str("TOUCH", b);
			p = put_dec(put(b, "irq0="), irq_low);
			p = put_dec(put(p, " obj="), objs);
			pad(b, put(put_dec(put(p, " t="), (uint32_t)((now() - t0) / f)), "s"));
			con_str("TOUCH", b);
			con_str("DEDO", d0);
			con_str("DEDO", d1);
		}
	}
	p = put_dec(put(b, "fim: lei="), reads);
	p = put_dec(put(p, " irq0="), irq_low);
	p = put_dec(put(p, " bit="), irq_hits);
	p = put_dec(put(p, " obj="), objs);
	put_dec(put(p, " err="), errors);
	ramlog_str(b);
	ramlog_str("\n");
}

static int sensor_setup(struct sensor *s, const struct rmi_fn *f12, const struct rmi_fn *f11)
{
	char b[64], *p;

	if (f12) {
		struct f12 t;
		step("f12: descritores");
		int err = f12_setup(&rmi, f12, &t, &desc);
		p = put_hex(put(b, "q0="), t.q0, 2);
		p = put_dec(put(p, " regs q/c/d="), t.nq);
		p = put_dec(put(p, "/"), t.nc);
		p = put_dec(put(p, "/"), t.nd);
		put_dec(put(p, " erro="), (uint32_t)t.err);
		say("F12", b);
		if (err || t.err)
			return -1;
		p = put_dec(put(b, "data1 off="), t.data1_off);
		p = put_dec(put(p, " tam="), t.data1_size);
		p = put_dec(put(p, " dedos="), t.fingers);
		say("F12", b);
		p = put_dec(put(b, "max x="), t.max_x);
		put_dec(put(p, " y="), t.max_y);
		say("F12", b);
		s->is_f12 = 1;
		s->data_addr = t.data_addr;
		s->rec_off = t.data1_off;
		s->fingers = t.fingers;
		s->max_x = t.max_x;
		s->max_y = t.max_y;
		s->rec_size = 8;
	} else {
		struct f11 t;
		step("f11: query/ctrl (sem F12)");
		f11_setup(&rmi, f11, &t);
		p = put_dec(put(b, "dedos="), t.fingers);
		p = put_dec(put(p, " max x="), t.max_x);
		p = put_dec(put(p, " y="), t.max_y);
		put_dec(put(p, " erro="), (uint32_t)t.err);
		say("F11", b);
		if (t.err)
			return -1;
		s->is_f12 = 0;
		s->data_addr = t.data_addr;
		s->state_bytes = t.state_bytes;
		s->rec_off = t.state_bytes;
		s->fingers = t.fingers;
		s->max_x = t.max_x;
		s->max_y = t.max_y;
		s->rec_size = 5;
	}
	while (s->fingers && s->rec_off + s->rec_size * s->fingers > PKT_MAX)
		s->fingers--;
	s->read_len = s->rec_off + s->rec_size * s->fingers;
	if (!s->fingers) {
		say("ERRO", "nenhum registro de dedo cabe");
		return -1;
	}
	return 0;
}

void shim_main(uint64_t dtb)
{
	const struct rmi_fn *f01, *f12, *f11, *f2d;
	struct sensor s;
	char b[64], *p;
	int ok = 0, pw, err;

	(void)dtb;
	ramlog_open();
	ramlog_str("\n=== OP6 shim M5 touch ===\n");
	fb_default();
	con_clear(GREY);
	con_title("OP6 SHIM M5: TOUCH RMI4");
	stage = "watchdog";
	wdog_arm();
	step_row = con_row();
	con_skip();

	step("estado dos pinos (so leitura)");
	report_pins();

	step("i2c bit-bang gpio49/50");
	rmi.addr = TS_ADDR;
	rmi.page = -1;
	if (bb_init(&rmi.bus, TS_SDA, TS_SCL, HALF_US)) {
		say("ERRO", "pino fora da tabela");
		goto out;
	}
	lines(b);
	say("LINHAS", b);

	pw = power_up();
	p = put(b, pw < 0 ? "sem resposta em 0x20" : "0x20 responde apos ");
	if (pw >= 0)
		put(p, pw == 0 ? "nada (ABL)" : pw == 1 ? "pinctrl" : pw == 2 ? "gpio88" : "ldo28");
	say("ENERGIA", b);
	if (pw < 0)
		goto out;
	if (!pinctrl_done) {
		/* It answered as ABL left it: leave GPIO99 (reset) alone. */
		step("pinctrl gpio125 entrada pull-up");
		wr32(tlmm_ctl(TS_IRQ), TS_INT_CTL);
	}

	step("rmi4: varre PDT");
	err = rmi_scan(&rmi);
	p = put_dec(put(b, "funcoes="), rmi.nfn);
	p = put_dec(put(p, " irqs="), rmi.irq_count);
	put_dec(put(p, " erro="), (uint32_t)err);
	say("PDT", b);
	for (unsigned i = 0; i < rmi.nfn; i++) {
		fn_line(b, &rmi.fn[i]);
		say("PDT", b);
	}
	if (err)
		goto out;
	f01 = rmi_find(&rmi, 0x01);
	f12 = rmi_find(&rmi, 0x12);
	f11 = rmi_find(&rmi, 0x11);
	f2d = f12 ? f12 : f11;
	if (!f01 || !f2d) {
		say("ERRO", f01 ? "sem F12/F11" : "sem F01");
		goto out;
	}
	if (f01_setup(f01, f2d))
		goto out;
	if (sensor_setup(&s, f12, f11))
		goto out;

	ok = 1;
	con_skip();
	step("toque na tela! (60 s)");
	touch_loop(&s, f01, f2d);
out:
	step("fim");
	con_skip();
	say("", ok ? "TOUCH LIDO. Tire uma foto." : "Tire uma foto desta tela!");
	countdown_reboot(con_row(), END_SECONDS);
}
