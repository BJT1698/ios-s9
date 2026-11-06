/*
 * Galaxy S9 milestone 5: the touchscreen before XNU, polled, MMU off.
 *   1. Power: the S6SY761's rails are S2DOS05 LDO2 (dvdd 1.8 V) and LDO3
 *      (avdd 3.0 V). Their DT nodes say regulator-boot-on, which makes Linux's
 *      regulator core enable them when the S2DOS05 driver probes; ABL leaves
 *      them off (EN 0x19, vsel already 0x8c). The PMIC sits at 0x60 on a
 *      bit-banged bus (i2c-gpio: SDA GPIO127, SCL GPIO128).
 *   2. Bus: SE11 runs its I2C firmware GSI-only, and GSI DMA needs the apps
 *      SMMU (stream 0x6d6), whose registers do not even answer without the
 *      bus votes Linux makes (M5.3 hung reading them). So the touch's own two
 *      wires, GPIO31/32, are bit-banged here too, the same way.
 *   3. sec_ts at 0x48: boot status, id, sense on, then events whenever the IRQ
 *      line (GPIO120, active low) is down, a square under each finger.
 * Sources: the Samsung kernel's regulator/s2dos05.c, i2c-gpio, input/
 * touchscreen/sec_ts, and the starqltechn device tree.
 */
#include <stdint.h>
#include "hw.h"
#include "fb.h"
#include "con.h"
#include "plat.h"

#define TOUCH_SECONDS	60
#define END_SECONDS	30

#define TLMM_NORTH	0x03900000UL	/* GPIO31/32/120/127/128 */

#define TS_SDA		31
#define TS_SCL		32
#define TS_IRQ		120		/* active low while events are pending */
#define TS_ADDR		0x48
#define TS_SENSE_ON	0x10
#define TS_DEVICE_ID	0x22
#define TS_SETFUNC	0x30
#define TS_BOOT_STATUS	0x55
#define TS_ONE_EVENT	0x60
#define TS_ALL_EVENT	0x61
#define TS_CLEAR_STACK	0x62

#define PM_SDA		127
#define PM_SCL		128
#define PM_ADDR		0x60
#define PM_EN		0x03		/* bit 1 LDO2, bit 2 LDO3 */
#define PM_LDO2		0x05		/* vsel 0-6 + 25 mV from 1.5 V, bit 7 discharge */
#define PM_LDO3		0x06		/* vsel 0-6 + 25 mV from 2.7 V */
#define PM_VSEL		12		/* 1.8 V and 3.0 V, the DT's min = max */

#define E_ADDR		1		/* address not acknowledged */
#define E_DATA		2		/* data byte not acknowledged */
#define E_SCL		3		/* clock held low */

struct bus {
	unsigned sda, scl;
};

static const struct bus pm_bus = { PM_SDA, PM_SCL };
static const struct bus ts_bus = { TS_SDA, TS_SCL };
static const char *stage;	/* set at runtime: no absolute pointers in data */
static int step_row;

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

static uintptr_t gpio(unsigned pin)
{
	return TLMM_NORTH + 0x1000UL * pin;
}

static unsigned level(unsigned pin)
{
	return rd32(gpio(pin) + 4) & 1;
}

static void lines(const char *label)
{
	char b[48], *p;

	p = put_dec(put(b, "sda="), level(TS_SDA));
	p = put_dec(put(p, " scl="), level(TS_SCL));
	put_dec(put(p, " irq="), level(TS_IRQ));
	con_str(label, b);
}

/*
 * i2c-gpio as Linux runs it: open drain by output enable (the output latch
 * stays 0), released lines float up on the board's pull-ups, ~100 kHz.
 */
static void bb_init(const struct bus *b)
{
	wr32(gpio(b->sda) + 4, 0);	/* output latch 0: OE alone pulls low */
	wr32(gpio(b->scl) + 4, 0);
	wr32(gpio(b->sda), 0);		/* GPIO function, input, no pull, 2 mA */
	wr32(gpio(b->scl), 0);
	us(20);
}

static void bb_line(unsigned pin, int high)
{
	wr32(gpio(pin), high ? 0 : 1u << 9);
	us(5);
}

static int bb_scl_high(const struct bus *b)
{
	uint64_t end = now() + ticks_per_s() / 100;

	wr32(gpio(b->scl), 0);
	while (!level(b->scl))		/* clock stretching */
		if (now() > end)
			return 0;
	us(5);
	return 1;
}

static int bb_start(const struct bus *b)
{
	bb_line(b->sda, 1);
	if (!bb_scl_high(b))
		return 0;
	bb_line(b->sda, 0);
	bb_line(b->scl, 0);
	return 1;
}

static void bb_stop(const struct bus *b)
{
	bb_line(b->sda, 0);
	bb_scl_high(b);
	bb_line(b->sda, 1);
}

/* 0 acknowledged, E_DATA not, E_SCL clock stuck */
static int bb_write(const struct bus *b, uint8_t v)
{
	int nack;

	for (int i = 7; i >= 0; i--) {
		bb_line(b->sda, v >> i & 1);
		if (!bb_scl_high(b))
			return E_SCL;
		bb_line(b->scl, 0);
	}
	bb_line(b->sda, 1);
	if (!bb_scl_high(b))
		return E_SCL;
	nack = level(b->sda);
	bb_line(b->scl, 0);
	return nack ? E_DATA : 0;
}

/* one byte, then ACK (more to come) or NACK (last); -1 if the clock stuck */
static int bb_read(const struct bus *b, int ack)
{
	unsigned v = 0;

	bb_line(b->sda, 1);
	for (int i = 0; i < 8; i++) {
		if (!bb_scl_high(b))
			return -1;
		v = v << 1 | level(b->sda);
		bb_line(b->scl, 0);
	}
	bb_line(b->sda, !ack);
	if (!bb_scl_high(b))
		return -1;
	bb_line(b->scl, 0);
	bb_line(b->sda, 1);
	return v;
}

/* Register read with a repeated START, like i2c_transfer of a write + a read. */
static int i2c_rd(const struct bus *b, uint8_t addr, uint8_t reg, uint8_t *buf, unsigned n)
{
	int err = E_SCL;

	if (!bb_start(b))
		goto out;
	err = bb_write(b, addr << 1);
	if (err) {
		err = err == E_DATA ? E_ADDR : err;
		goto out;
	}
	err = bb_write(b, reg);
	if (err)
		goto out;
	err = E_SCL;
	if (!bb_start(b))
		goto out;
	err = bb_write(b, addr << 1 | 1);
	if (err) {
		err = err == E_DATA ? E_ADDR : err;
		goto out;
	}
	for (unsigned i = 0; i < n; i++) {
		int v = bb_read(b, i + 1 < n);
		if (v < 0) {
			err = E_SCL;
			goto out;
		}
		buf[i] = (uint8_t)v;
	}
out:
	bb_stop(b);
	return err;
}

static int i2c_wr(const struct bus *b, uint8_t addr, const uint8_t *buf, unsigned n)
{
	int err = E_SCL;

	if (!bb_start(b))
		goto out;
	err = bb_write(b, addr << 1);
	if (err) {
		err = err == E_DATA ? E_ADDR : err;
		goto out;
	}
	for (unsigned i = 0; i < n && !err; i++)
		err = bb_write(b, buf[i]);
out:
	bb_stop(b);
	return err;
}

static int pm_rd(uint8_t reg)
{
	uint8_t v;

	return i2c_rd(&pm_bus, PM_ADDR, reg, &v, 1) ? -1 : v;
}

static int pm_wr(uint8_t reg, uint8_t val)
{
	uint8_t out[2];

	out[0] = reg;
	out[1] = val;
	return i2c_wr(&pm_bus, PM_ADDR, out, 2) == 0;
}

/* -1: could not tell; 0: rails off and left off; 1: rails on */
static int touch_power(void)
{
	uint32_t ctl_sda = rd32(gpio(PM_SDA)), ctl_scl = rd32(gpio(PM_SCL));
	uint32_t io_sda = rd32(gpio(PM_SDA) + 4), io_scl = rd32(gpio(PM_SCL) + 4);
	char b[64], *p;
	int ret = -1;

	stage = "s2dos05";
	bb_init(&pm_bus);
	if (!level(PM_SDA) || !level(PM_SCL)) {
		con_str("S2DOS05", "barramento preso em 0: nao mexi");
		goto out;
	}
	int id = pm_rd(0), en = pm_rd(PM_EN), l2 = pm_rd(PM_LDO2), l3 = pm_rd(PM_LDO3);
	p = put_hex(put(b, "id="), (uint32_t)id, 2);
	p = put_hex(put(p, " en="), (uint32_t)en, 2);
	p = put_hex(put(p, " ldo2="), (uint32_t)l2, 2);
	put_hex(put(p, " ldo3="), (uint32_t)l3, 2);
	con_str("S2DOS05", b);
	if (id < 0 || en < 0 || l2 < 0 || l3 < 0 || pm_rd(PM_EN) != en ||
	    pm_rd(PM_LDO2) != l2 || pm_rd(PM_LDO3) != l3) {
		con_str("S2DOS05", "leitura falhou ou instavel: nao mexi");
		goto out;
	}
	if ((en & 6) == 6) {
		con_str("ENERGIA", "LDO2/LDO3 ja estavam ligadas");
		ret = 1;
		goto out;
	}
	/* What the regulator core does for these nodes: apply min_uV, then enable
	 * (dvdd, 1 ms, avdd, as sec_ts_power orders them). Other bits untouched. */
	if ((l2 & 0x7f) != PM_VSEL)
		pm_wr(PM_LDO2, (uint8_t)((l2 & 0x80) | PM_VSEL));
	if ((l3 & 0x7f) != PM_VSEL)
		pm_wr(PM_LDO3, (uint8_t)((l3 & 0x80) | PM_VSEL));
	l2 = pm_rd(PM_LDO2);
	l3 = pm_rd(PM_LDO3);
	if (l2 < 0 || l3 < 0 || (l2 & 0x7f) != PM_VSEL || (l3 & 0x7f) != PM_VSEL) {
		con_str("ENERGIA", "tensao nao confirmou 1,8/3,0 V: nao liguei");
		ret = 0;
		goto out;
	}
	pm_wr(PM_EN, (uint8_t)(en | 2));
	delay_ms(1);
	pm_wr(PM_EN, (uint8_t)(en | 6));
	delay_ms(5);
	int now_en = pm_rd(PM_EN);
	p = put_hex(put(b, "en="), (uint32_t)now_en, 2);
	p = put_hex(put(p, " ldo2="), (uint32_t)l2, 2);
	put(put_hex(put(p, " ldo3="), (uint32_t)l3, 2), (now_en & 6) == 6 ? " LIGOU" : " FALHOU");
	con_str("ENERGIA", b);
	ret = now_en >= 0 && (now_en & 6) == 6;
out:
	wr32(gpio(PM_SDA) + 4, io_sda);
	wr32(gpio(PM_SCL) + 4, io_scl);
	wr32(gpio(PM_SDA), ctl_sda);
	wr32(gpio(PM_SCL), ctl_scl);
	return ret;
}

static int ts_read(uint8_t reg, uint8_t *buf, unsigned n)
{
	return i2c_rd(&ts_bus, TS_ADDR, reg, buf, n);
}

static int ts_cmd(uint8_t c, uint8_t a, uint8_t b, unsigned n)
{
	uint8_t out[3];

	out[0] = c;
	out[1] = a;
	out[2] = b;
	return i2c_wr(&ts_bus, TS_ADDR, out, 1 + n);
}

/* One sec_ts event: a square under the finger, 12-bit coordinates over the panel. */
static int finger(const uint8_t *e, char *b)
{
	static const uint32_t color[4] = { GREEN, RED, YELLOW, BLUE };
	unsigned tid = e[0] >> 2 & 0xf, action = e[0] >> 6 & 3;
	int x = e[1] << 4 | e[3] >> 4, y = e[2] << 4 | (e[3] & 0xf);
	int px = x * PANEL_W / 4096, py = y * PANEL_H / 4096;
	char *p;

	if ((e[0] & 3) != 0 || tid == 0 || tid > 10)
		return 0;	/* status or gesture events, no finger */
	if (action != 3)
		fb_rect(px - 20, py - 20, 40, 40, color[tid & 3]);
	p = put_dec(put(b, "id="), tid);
	p = put_dec(put(p, " x="), x);
	p = put_dec(put(p, " y="), y);
	p = put_dec(put(p, " -> "), px);
	p = put_dec(put(p, ","), py);
	put(put_dec(put(p, " acao="), action), "    ");
	return 1;
}

static void touch_loop(void)
{
	uint64_t f = ticks_per_s(), t0 = now();
	unsigned reads = 0, touches = 0, errors = 0, polls = 0;
	uint8_t buf[8 * 8];
	char b[64], last[64], *p;
	int row = con_row();

	put(last, "(encoste o dedo na tela)");
	con_skip();
	con_skip();
	while (now() - t0 < f * TOUCH_SECONDS) {
		polls++;
		if (!level(TS_IRQ)) {
			int err = ts_read(TS_ONE_EVENT, buf, 8);

			reads++;
			if (err) {
				errors++;
			} else {
				unsigned left = buf[7] & 0x3f;
				touches += finger(buf, last);
				while (left && !err) {
					/* sec,i2c-burstmax = 64: 8 events per read */
					unsigned n = left > 8 ? 8 : left;
					err = ts_read(TS_ALL_EVENT, buf, n * 8);
					for (unsigned i = 0; !err && i < n; i++)
						touches += finger(buf + 8 * i, last);
					left -= n;
				}
			}
		}
		if (polls % 50 == 0) {
			con_set_row(row);
			p = put_dec(put(b, "leituras="), reads);
			p = put_dec(put(p, " toques="), touches);
			p = put_dec(put(p, " erros="), errors);
			put(put_dec(put(p, " t="), (uint32_t)((now() - t0) / f)), "s ");
			con_str("TOUCH", b);
			con_str("DEDO", last);
		}
		wdog_pet();
		delay_ms(5);
	}
}

void shim_main(uint64_t dtb)
{
	uint8_t boot = 0, id[3] = { 0, 0, 0 };
	char b[96], *p;
	int ok = 0;

	(void)dtb;
	/* M1.1: 0x9d400000, 1440x2960, stride 5760, B G R A, autorefresh on */
	fb_default();
	con_clear(GREY);
	con_title("SHIM M5.4: TOUCH BIT-BANG");
	stage = "watchdog";
	wdog_arm();
	step_row = con_row();
	con_skip();

	lines("LINHAS");
	step("s2dos05");
	int pwr = touch_power();
	wdog_pet();
	if (pwr <= 0)
		goto out;
	delay_ms(200);		/* sec_ts boot after power on */
	step("i2c do touch");
	stage = "i2c touch";
	bb_init(&ts_bus);
	lines("LINHAS GPIO");
	if (!level(TS_SDA) || !level(TS_SCL)) {
		con_str("ERRO", "linhas do touch em 0");
		goto out;
	}
	step("le boot 0x55 e id 0x22");
	int e1 = ts_read(TS_BOOT_STATUS, &boot, 1);
	int e2 = ts_read(TS_DEVICE_ID, id, 3);
	p = put_hex(put(b, "boot="), boot, 2);
	p = put_dec(put(p, " erro="), e1);
	p = put_hex(put(p, "  id="), (uint32_t)id[0] << 16 | id[1] << 8 | id[2], 6);
	put_dec(put(p, " erro="), e2);
	con_str("SEC_TS", b);
	wdog_pet();
	if (e1 || e2)
		goto out;
	step("sense on");
	stage = "sec_ts sense on";
	int e = ts_cmd(TS_CLEAR_STACK, 0, 0, 0);
	if (!e)
		e = ts_cmd(TS_SETFUNC, 0x61, 0x00, 2);
	if (!e)
		e = ts_cmd(TS_SENSE_ON, 0, 0, 0);
	put_dec(put(b, "erro="), e);
	con_str("SENSE ON", b);
	if (e)
		goto out;
	ok = 1;
	stage = "touch";
	step("toque na tela!");
	touch_loop();
out:
	stage = "fim";
	step("fim");
	con_skip();
	con_str("", ok ? "TOUCH OK! Tire uma foto." : "Tire uma foto desta tela!");
	countdown_reboot(con_row(), END_SECONDS);
}
