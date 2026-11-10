/*
 * OnePlus 6 milestone M11 (GPU stage B): give the GMU its memory through the
 * GPU's own SMMU and talk to it over HFI, the message queues it reads from
 * RAM. Port of the Galaxy S9 M11 (s9/shim/m11.c; never confirmed on the S9),
 * corrected to what mainline Linux runs on this phone (a6xx_gmu.c /
 * a6xx_hfi.c v6.6, a630 = "legacy" HFI v1): GMU SID 5 (DT iommus <&smmu 5>),
 * dummy page at GMU VA 0x60000000 and debug/log/HFI after it, two queues
 * 4 KiB apart, no 4-word padding (that is HFI v2), acks recognised by id 126
 * + the echoed sequence number, unprivileged-accessible pages (AP[1]), and
 * the v1 sequence INIT -> FW_VERSION -> PERF_TABLE -> BW_TABLE -> TEST.
 * The perf table is computed here from the OP6 cmd-db ARC lists and the OP6
 * DT OPP tables (a6xx_gmu_rpmh_arc_votes_init).
 *
 *   1. power (gpu.c) + cmd-db ARC lists (read only) -> GX / CX votes.
 *   2. GPU SMMU read: ids, sCR0, every valid SMR/S2CR, which CBs are in use.
 *   3. page tables (static, PA = VA, MMU off) for the four GMU buffers,
 *      a free context bank (S1 translate, S2 bypass, VMID 0, VA64), S2CR
 *      TRANSLATE + SMR for SID 5 and 4, every write read back; an ATS1PR
 *      translation of the HFI VA must give the buffer's PA before the GMU
 *      is allowed to touch memory (no rule => no GMU boot).
 *   4. firmware + boot with HFI_QTBL_ADDR set -> 0xBABEFACE.
 *   5. HFI_CTRL_INIT, then the five v1 messages, each one's ack on screen.
 *   Then the CM3 goes back to reset and the screen counts down to a reboot.
 *
 * Not here: GX GDSC / GMU OOB, SPTPRAC, GMU RPMh votes, zap, CP.
 */
#include <stdint.h>
#include "hw.h"
#include "fb.h"
#include "con.h"
#include "plat.h"
#include "rpmh.h"
#include "gpu.h"

#define END_SECONDS	60

/* ---- GPU SMMU (ARM SMMUv2 layout, arm-smmu.h) ---- */
#define S_CR0		0x000
#define S_CR0_CLIENTPD	(1u << 0)
#define S_IDR0		0x020
#define S_IDR1		0x024
#define S_IDR2		0x028
#define S_GFSR		0x048
#define S_GFSYNR1	0x054
#define S_TLBIALLNSNH	0x068
#define S_TLBGSYNC	0x070
#define S_TLBGSTATUS	0x074
#define S_SMR(n)	(0x800 + 4 * (n))
#define S_S2CR(n)	(0xc00 + 4 * (n))
#define S_CBAR(n)	(4 * (n))		/* GR1 */
#define S_CBA2R(n)	(0x800 + 4 * (n))	/* GR1 */
#define IDR0_SMS	(1u << 27)
#define IDR0_ATOSNS	(1u << 26)
#define IDR0_S1TS	(1u << 30)
#define SMR_VALID	(1u << 31)

#define CB_SCTLR	0x000
#define CB_TCR2		0x010
#define CB_TTBR0	0x020
#define CB_TTBR1	0x028
#define CB_TCR		0x030
#define CB_MAIR0	0x038
#define CB_MAIR1	0x03c
#define CB_PAR		0x050
#define CB_FSR		0x058
#define CB_FAR		0x060
#define CB_FSYNR0	0x068
#define CB_TLBIALL	0x618
#define CB_TLBSYNC	0x7f0
#define CB_TLBSTATUS	0x7f4
#define CB_ATS1PR	0x800
#define CB_ATSR		0x8f0

#define CBAR_S1		((1u << 16) | (3u << 8) | (0xfu << 12))	/* S1 trans/S2 bypass, BPSHCFG NSH, MEMATTR WB, VMID 0 */
#define SCTLR_ON	((1u << 0) | (1u << 1) | (1u << 2) | (1u << 5) | (1u << 12))	/* M TRE AFE CFRE ASIDPNE; no IRQ */
#define TCR_VAL		(25u | (2u << 12) | (1u << 23))	/* T0SZ 25 (39 bit), walks NC/NC outer-shareable, TG0 4K, EPD1 */
#define TCR2_VAL	(1u | (1u << 4) | (7u << 15))	/* PASize 36 bit, AS (16-bit ASID), SEP upstream */
#define MAIR0_VAL	0xf404ff44u			/* 0 NC, 1 WBRWA, 2 device, 3 outer-WB (io-pgtable-arm) */
#define FSR_ALL		0xc00001feu

/* io-pgtable-arm, stage 1, 4K granule */
#define PTE_TABLE	3ull
#define PTE_PAGE	3ull
#define PTE_AP_UNPRIV	(1ull << 6)
#define PTE_SH_OS	(2ull << 8)
#define PTE_AF		(1ull << 10)
#define PTE_NG		(1ull << 11)
#define PTE_GMU		(PTE_PAGE | PTE_AP_UNPRIV | PTE_SH_OS | PTE_AF | PTE_NG)	/* AttrIndx 0 = NC */
#define PTE_ADDR	0x0000fffffffff000ull

/* GMU address map (a6xx_gmu_memory_probe, a630): dummy fixed at 0x60000000,
 * then debug / log / hfi from the uncached range 0x60000000 + 4K up. */
#define VA_DUMMY	0x60000000u
#define VA_DEBUG	0x60001000u
#define VA_LOG		0x60005000u
#define VA_HFI		0x60009000u
#define SID_GMU		5u	/* gmu@506a000 iommus = <0x108 0x05> */
#define SID_GMU_USER	4u	/* kgsl gmu_user (S9 DT); mapped too, harmless */

/* HFI v1 (a6xx_hfi.h) */
#define Q_WORDS		1024u
#define QH_STATUS	0
#define QH_IOVA		1
#define QH_TYPE		2
#define QH_SIZE		3
#define QH_RXWM		6
#define QH_TXWM		7
#define QH_RXREQ	8
#define QH_READ		10
#define QH_WRITE	11
#define MSG_INIT	0
#define MSG_FW_VERSION	1
#define MSG_BW_TABLE	3
#define MSG_PERF_TABLE	4
#define MSG_TEST	5
#define F2H_ERROR	100
#define F2H_ACK		126
#define INTR_MSGQ	1u

static uint8_t pt_pool[3][4096] __attribute__((aligned(4096)));
static uint8_t mem_dummy[0x1000] __attribute__((aligned(4096)));
static uint8_t mem_debug[0x4000] __attribute__((aligned(4096)));
static uint8_t mem_log[0x4000] __attribute__((aligned(4096)));
static uint8_t mem_hfi[0x4000] __attribute__((aligned(4096)));
static unsigned pt_used;
static uint64_t *pt_root;

static uint32_t pgsz = 0x1000, npage = 8, ncb, nsmr, idr0;
static int cb = -1;
static uint32_t cbbase;		/* byte offset of the chosen CB's page */

static uint64_t pa(const void *p)
{
	return (uint64_t)(uintptr_t)p;
}

static void zero(void *p, uint32_t n)
{
	for (uint32_t i = 0; i < n; i += 4)
		*(volatile uint32_t *)((uint8_t *)p + i) = 0;
}

static void wr(uintptr_t a, uint32_t v)
{
	wr32(a, v);
	dsb();
}

static void wr64(uintptr_t a, uint64_t v)
{
	*(volatile uint64_t *)a = v;
	dsb();
}

static uint64_t rd64(uintptr_t a)
{
	return *(volatile uint64_t *)a;
}

/* ---- 1. perf table from the OP6 OPPs + cmd-db (a6xx_gmu_rpmh_votes_init) ---- */

/* OP6 DT gpu opp-table (no speed-bin / opp-supported-hw) and gmu opp-table,
 * ascending, with the "off" entry first as a6xx_gmu_build_freq_table does. */
static const uint32_t gpu_khz[8] = { 0, 257000, 342000, 414000, 520000, 596000, 675000, 710000 };
static const uint16_t gpu_lvl[8] = { 0, 0x40, 0x80, 0xc0, 0x100, 0x140, 0x180, 0x1a0 };
static const uint32_t gmu_khz[3] = { 0, 200000, 400000 };
static const uint16_t gmu_lvl[3] = { 0, 0x30, 0x80 };
static uint32_t gx_vote[8], cx_vote[3];

static int votes(void)
{
	char b[200], *p;
	int ok = 1;

	for (unsigned i = 0; i < 8; i++)
		gx_vote[i] = gpu_arc_vote(&arc_gfx, gpu_lvl[i], &ok);
	for (unsigned i = 0; i < 3; i++)
		cx_vote[i] = gpu_arc_vote(&arc_cx, gmu_lvl[i], &ok);
	p = put(b, "gx");
	for (unsigned i = 0; i < 8; i++)
		p = put_hex(put(p, " "), gx_vote[i], 6);
	say("VOTOS", b);
	p = put(b, "cx");
	for (unsigned i = 0; i < 3; i++)
		p = put_hex(put(p, " "), cx_vote[i], 6);
	say("VOTOS", b);
	if (!ok)
		say("VOTOS", "nivel de OPP sem ARC no cmd-db: parando");
	return ok;
}

/* ---- 2. GPU SMMU as left (read only) ---- */

static int smmu_probe(void)
{
	char b[160], *p;
	uint32_t idr1, used = 0, cbmask = 0;

	step("2 SMMU GPU: le ids/SMR/S2CR");
	idr0 = rd32(GPU_SMMU + S_IDR0);
	idr1 = rd32(GPU_SMMU + S_IDR1);
	nsmr = idr0 & 0xff;
	ncb = idr1 & 0xff;
	pgsz = (idr1 >> 31) ? 0x10000 : 0x1000;
	npage = 2u << ((idr1 >> 28) & 7);
	gpu_smmu_ids();
	p = put_dec(put(b, "smr="), nsmr);
	p = put_dec(put(p, " cb="), ncb);
	p = put_dec(put(p, " pg="), pgsz >> 10);
	p = put_dec(put(p, "K n="), npage);
	p = put(p, (idr0 & IDR0_SMS) ? " SMS" : " indexado");
	put(p, (idr0 & IDR0_S1TS) && !(idr0 & IDR0_ATOSNS) ? " ATOS" : " sem-ATOS");
	say("SMMU", b);
	if (2 * npage * pgsz != GPU_SMMU_SIZE || ncb == 0 || ncb > 32 || nsmr > 128) {
		say("SMMU", "layout nao bate com 0x10000 / ids estranhos: parando");
		return 0;
	}
	if (idr0 & IDR0_SMS) {
		for (uint32_t n = 0; n < nsmr; n++) {
			uint32_t smr = rd32(GPU_SMMU + S_SMR(n)), s2cr = rd32(GPU_SMMU + S_S2CR(n));
			if (!(smr & SMR_VALID))
				continue;
			used++;
			if (((s2cr >> 16) & 3) == 0)
				cbmask |= 1u << (s2cr & 0x1f);
			p = put_hex(put(put_dec(put(b, "smr"), n), "="), smr, 8);
			put_hex(put(p, " s2cr="), s2cr, 8);
			saylog("SMMU", b);
			if ((smr & 0x7fff) == SID_GMU || (smr & 0x7fff) == SID_GMU_USER)
				say("SMMU", b);
		}
	}
	for (uint32_t i = 0; i < ncb; i++)
		if (rd32(GPU_SMMU + npage * pgsz + i * pgsz + CB_SCTLR) & 1)
			cbmask |= 1u << i;
	for (uint32_t i = 0; i < ncb && cb < 0; i++)
		if (!(cbmask & (1u << i)))
			cb = (int)i;
	p = put_dec(put(b, "smr validos="), used);
	p = put_hex(put(p, " cb em uso="), cbmask, 4);
	p = put(p, " livre=");
	p = cb < 0 ? put(p, "nenhum") : put_dec(p, (uint32_t)cb);
	say("SMMU", b);
	return cb >= 0;
}

/* ---- 3. tables + context bank + stream rules ---- */

static uint64_t *pt_new(void)
{
	if (pt_used >= 3)
		return 0;
	uint64_t *t = (uint64_t *)pt_pool[pt_used++];
	zero(t, 4096);
	return t;
}

static int pt_map(uint64_t va, uint64_t phys)
{
	uint64_t *t = pt_root;

	for (int shift = 30; shift >= 21; shift -= 9) {
		uint64_t *e = &t[(va >> shift) & 0x1ff];
		if (!(*e & 1)) {
			uint64_t *n = pt_new();
			if (!n)
				return 0;
			*e = pa(n) | PTE_TABLE;
		}
		t = (uint64_t *)(uintptr_t)(*e & PTE_ADDR);
	}
	t[(va >> 12) & 0x1ff] = phys | PTE_GMU;
	return 1;
}

static int map_range(uint32_t va, void *buf, uint32_t size)
{
	for (uint32_t off = 0; off < size; off += 4096)
		if (!pt_map(va + off, pa(buf) + off))
			return 0;
	return 1;
}

static int smr_slot(uint32_t sid, int avoid)
{
	if (!(idr0 & IDR0_SMS))
		return sid < 128 ? (int)sid : -1;	/* stream indexing: S2CR[sid] */
	for (uint32_t n = 0; n < nsmr; n++)
		if ((rd32(GPU_SMMU + S_SMR(n)) & (SMR_VALID | 0x7fff)) == (SMR_VALID | sid))
			return (int)n;			/* reuse an existing rule for this SID */
	for (uint32_t n = 0; n < nsmr; n++)
		if (!(rd32(GPU_SMMU + S_SMR(n)) & SMR_VALID) && (int)n != avoid)
			return (int)n;
	return -1;
}

static int smmu_route(uint32_t sid, int avoid)
{
	char b[96], *p;
	int n = smr_slot(sid, avoid);

	if (n < 0) {
		say("SMMU", "sem SMR livre");
		return -1;
	}
	wr(GPU_SMMU + S_S2CR(n), (uint32_t)cb);		/* type TRANSLATE, CBNDX */
	if (idr0 & IDR0_SMS)
		wr(GPU_SMMU + S_SMR(n), SMR_VALID | sid);	/* mask 0 */
	p = put_dec(put(b, "sid "), sid);
	p = put_dec(put(p, " smr"), (uint32_t)n);
	p = put_hex(put(p, "="), rd32(GPU_SMMU + S_SMR(n)), 8);
	put_hex(put(p, " s2cr="), rd32(GPU_SMMU + S_S2CR(n)), 8);
	say("SMMU", b);
	return (rd32(GPU_SMMU + S_S2CR(n)) & 0x300ff) == (uint32_t)cb &&
	    (!(idr0 & IDR0_SMS) || rd32(GPU_SMMU + S_SMR(n)) == (SMR_VALID | sid)) ? n : -1;
}

static int smmu_faulted(const char *when)
{
	uintptr_t c = GPU_SMMU + cbbase;
	uint32_t fsr = rd32(c + CB_FSR), gfsr = rd32(GPU_SMMU + S_GFSR);
	char b[120], *p;

	if (!(fsr & FSR_ALL) && !gfsr)
		return 0;
	p = put(put(b, when), " fsr=");
	p = put_hex(p, fsr, 8);
	p = put_hex(put(p, " far="), rd32(c + CB_FAR), 8);
	p = put_hex(put(p, " syn="), rd32(c + CB_FSYNR0), 8);
	p = put_hex(put(p, " gfsr="), gfsr, 8);
	put_hex(put(p, " gsyn1="), rd32(GPU_SMMU + S_GFSYNR1), 8);
	say("FALHA", b);
	return 1;
}

static int smmu_program(void)
{
	char b[120], *p;
	uintptr_t c;
	uint64_t ttbr;

	step("3 tabelas da SMMU (4 buffers)");
	if ((pa(mem_hfi) | pa(mem_dummy) | pa(mem_log) | pa(mem_debug) | pa(pt_pool)) & 0xfff) {
		say("SMMU", "buffers fora de 4K (carga do ABL?): parando");
		return 0;
	}
	pt_root = pt_new();
	if (!pt_root || !map_range(VA_DUMMY, mem_dummy, sizeof(mem_dummy)) ||
	    !map_range(VA_DEBUG, mem_debug, sizeof(mem_debug)) ||
	    !map_range(VA_LOG, mem_log, sizeof(mem_log)) || !map_range(VA_HFI, mem_hfi, sizeof(mem_hfi))) {
		say("SMMU", "sem tabelas: parando");
		return 0;
	}
	dsb();
	p = put_hex(put(b, "hfi pa="), pa(mem_hfi), 8);
	put_hex(put(p, " ttbr="), pa(pt_root), 8);
	say("SMMU", b);

	step("3 contexto (S1, VMID 0, VA64)");
	cbbase = npage * pgsz + (uint32_t)cb * pgsz;
	c = GPU_SMMU + cbbase;
	ttbr = pa(pt_root) | ((uint64_t)(uint32_t)cb << 48);
	wr(c + CB_SCTLR, 0);
	wr(GPU_SMMU + pgsz + S_CBA2R(cb), 1);		/* VA64 */
	wr(GPU_SMMU + pgsz + S_CBAR(cb), CBAR_S1);
	wr(c + CB_TCR2, TCR2_VAL);
	wr(c + CB_TCR, TCR_VAL);
	wr(c + CB_TTBR0, (uint32_t)ttbr);
	wr(c + CB_TTBR0 + 4, (uint32_t)(ttbr >> 32));
	wr(c + CB_TTBR1, 0);
	wr(c + CB_TTBR1 + 4, 0);
	wr(c + CB_MAIR0, MAIR0_VAL);
	wr(c + CB_MAIR1, 0);
	wr(c + CB_FSR, FSR_ALL);
	wr(c + CB_TLBIALL, 0);
	wr(c + CB_TLBSYNC, 0);
	for (int i = 0; i < 100 && (rd32(c + CB_TLBSTATUS) & 1); i++)
		delay_ms(1);
	wr(c + CB_SCTLR, SCTLR_ON);
	p = put_hex(put(b, "cbar="), rd32(GPU_SMMU + pgsz + S_CBAR(cb)), 8);
	p = put_hex(put(p, " sctlr="), rd32(c + CB_SCTLR), 4);
	p = put_hex(put(p, " ttbr0="), rd32(c + CB_TTBR0), 8);
	put_hex(put(p, " tcr="), rd32(c + CB_TCR), 8);
	say("SMMU", b);
	if (!(rd32(c + CB_SCTLR) & 1) || rd32(c + CB_TTBR0) != (uint32_t)ttbr ||
	    ((rd32(GPU_SMMU + pgsz + S_CBAR(cb)) >> 16) & 3) != 1) {
		say("SMMU", "o contexto NAO aceitou as escritas (hyp?): parando");
		return 0;
	}

	step("3 regras SMR/S2CR p/ SID 5 e 4");
	int s5 = smmu_route(SID_GMU, -1);
	if (s5 < 0) {
		say("SMMU", "regra do SID 5 NAO ficou: parando");
		return 0;
	}
	if (smmu_route(SID_GMU_USER, s5) < 0)
		say("SMMU", "regra do SID 4 nao ficou (segue: o DT so usa o 5)");
	wr(GPU_SMMU + S_TLBIALLNSNH, 0);
	wr(GPU_SMMU + S_TLBGSYNC, 0);
	for (int i = 0; i < 100 && (rd32(GPU_SMMU + S_TLBGSTATUS) & 1); i++)
		delay_ms(1);
	uint32_t cr0 = rd32(GPU_SMMU + S_CR0);
	if (cr0 & S_CR0_CLIENTPD) {
		step("3 sCR0: liga a traducao");
		wr(GPU_SMMU + S_CR0, cr0 & ~S_CR0_CLIENTPD);
	}
	sayhex("sCR0", rd32(GPU_SMMU + S_CR0));
	if (rd32(GPU_SMMU + S_CR0) & S_CR0_CLIENTPD) {
		say("SMMU", "SMMU continua em bypass (CLIENTPD): parando");
		return 0;
	}

	if ((idr0 & IDR0_S1TS) && !(idr0 & IDR0_ATOSNS)) {
		step("3 ATS1PR: traduz 0x60009000");
		wr64(c + CB_ATS1PR, VA_HFI);
		for (int i = 0; i < 100 && (rd32(c + CB_ATSR) & 1); i++)
			delay_ms(1);
		uint64_t par = rd64(c + CB_PAR);
		p = put_hex(put(b, "par="), par, 16);
		say("ATS", b);
		if ((par & 1) || (par & 0xfffffff000ull) != pa(mem_hfi)) {
			say("ATS", "a SMMU nao traduz para o buffer: parando");
			smmu_faulted("ats");
			return 0;
		}
		say("ATS", "VA 0x60009000 -> buffer HFI: OK");
	} else {
		say("ATS", "sem operacao de traducao: confio nas leituras acima");
	}
	return !smmu_faulted("antes do boot");
}

/* ---- 5. HFI v1 ---- */

static volatile uint32_t *const_hfi(void)
{
	return (volatile uint32_t *)mem_hfi;
}

static volatile uint32_t *qhdr(unsigned q)
{
	return const_hfi() + 6 + 12 * q;
}

static volatile uint32_t *qdata(unsigned q)
{
	return (volatile uint32_t *)(mem_hfi + 0x1000 * (q + 1));
}

static void hfi_table(void)
{
	volatile uint32_t *t = const_hfi();

	zero(mem_hfi, sizeof(mem_hfi));
	zero(mem_debug, sizeof(mem_debug));
	zero(mem_log, sizeof(mem_log));
	zero(mem_dummy, sizeof(mem_dummy));
	t[0] = 0;			/* version */
	t[1] = 24 + 2 * 48;		/* size: a6xx_hfi_init stores bytes (table + 2 headers) */
	t[2] = 6;			/* qhdr0 offset, dwords */
	t[3] = 12;			/* qhdr size, dwords */
	t[4] = 2;			/* queues */
	t[5] = 2;			/* active */
	for (unsigned q = 0; q < 2; q++) {
		volatile uint32_t *h = qhdr(q);
		h[QH_IOVA] = VA_HFI + 0x1000 * (q + 1);
		h[QH_TYPE] = (10u << 8) | (q ? 4u : 0u);	/* cmd id 0; legacy response id 4 */
		h[QH_STATUS] = 1;
		h[QH_SIZE] = Q_WORDS;
		h[QH_RXWM] = 1;
		h[QH_TXWM] = 1;
		h[QH_RXREQ] = 1;
	}
	dsb();
}

static uint32_t seqnum, ack_payload0;

/* Send msg (header filled here), wait for its ack. 0 = accepted. */
static int hfi_send(const char *what, uint32_t id, uint32_t *msg, uint32_t words)
{
	volatile uint32_t *h = qhdr(0), *q = qdata(0);
	volatile uint32_t *rh = qhdr(1), *rq = qdata(1);
	uint32_t seq = ++seqnum % 0xfff, w = h[QH_WRITE], r = h[QH_READ];
	uint32_t space = (r + Q_WORDS - w - 1) % Q_WORDS;
	char b[120], *p;
	int verdict = -1;

	step(what);
	if (space < words) {
		say(what, "fila de comandos cheia");
		return -1;
	}
	msg[0] = (seq << 20) | (words << 8) | id;		/* HFI_MSG_CMD = 0 */
	for (uint32_t i = 0; i < words; i++) {
		q[w] = msg[i];
		w = (w + 1) % Q_WORDS;
	}
	h[QH_WRITE] = w;					/* legacy: no 0xfafafafa padding */
	dsb();
	wr(GMU(GMU_HOST2GMU_INTR_SET), 1);

	for (int t = 0; t < 1000 && verdict < 0; t++) {
		if (!(rd32(GMU(GMU_GMU2HOST_INTR_INFO)) & INTR_MSGQ)) {
			if (smmu_faulted(what))
				break;
			delay_ms(1);
			if ((t & 255) == 255)
				wdog_pet();
			continue;
		}
		wr(GMU(GMU_GMU2HOST_INTR_CLR), INTR_MSGQ);
		while (rh[QH_READ] != rh[QH_WRITE]) {
			uint32_t at = rh[QH_READ] % Q_WORDS, m[20], n = (rq[at] >> 8) & 0xff;
			if (n == 0 || n > 19) {
				p = put_hex(put(b, "resposta corrompida hdr="), rq[at], 8);
				say(what, b);
				rh[QH_READ] = rh[QH_WRITE];
				break;
			}
			for (uint32_t i = 0; i < n; i++) {
				m[i] = rq[at];
				at = (at + 1) % Q_WORDS;
			}
			rh[QH_READ] = at;
			dsb();
			if ((m[0] & 0xff) == F2H_ERROR) {
				p = put_dec(put(b, "F2H_MSG_ERROR code="), m[1]);
				say(what, b);
				continue;
			}
			if ((m[0] & 0xff) == F2H_ACK && n >= 3 && ((m[1] >> 20) & 0xfff) == seq) {
				verdict = (int)m[2];
				ack_payload0 = n >= 4 ? m[3] : 0;
			}
		}
	}
	p = put(b, verdict < 0 ? "SEM RESPOSTA" : verdict ? "RECUSOU err=" : "ACK ok");
	if (verdict > 0)
		p = put_dec(p, (uint32_t)verdict);
	put_dec(put(p, " seq="), seq);
	say(what, b);
	return verdict < 0 ? -1 : verdict;
}

static int hfi_talk(void)
{
	static uint32_t msg[160];
	char b[96], *p;

	step("5 HFI_CTRL_INIT");
	wr(GMU(GMU_HFI_CTRL_INIT), 1);
	int up = 0;
	for (int i = 0; i < 100 && !up; i++) {
		up = rd32(GMU(GMU_HFI_CTRL_STATUS)) & 1;
		if (!up)
			delay_ms(1);
	}
	p = put_hex(put(b, "status="), rd32(GMU(GMU_HFI_CTRL_STATUS)), 8);
	put_hex(put(p, " ver="), rd32(GMU(GMU_HFI_VERSION_INFO)), 8);
	say("HFI", b);
	if (!up) {
		say("HFI", "as filas nao sobem");
		smmu_faulted("ctrl_init");
		return 0;
	}

	zero(msg, sizeof(msg));
	msg[1] = 0;			/* seg_id */
	msg[2] = VA_DEBUG;		/* dbg_buffer_addr */
	msg[3] = sizeof(mem_debug);	/* dbg_buffer_size */
	msg[4] = 1;			/* boot_state GMU_COLD_BOOT */
	if (hfi_send("5a H2F INIT", MSG_INIT, msg, 5))
		return 0;

	zero(msg, sizeof(msg));
	msg[1] = (1u << 28) | (1u << 19) | (1u << 17);	/* a6xx_hfi_get_fw_version: supported 1.10 */
	if (hfi_send("5b FW_VERSION", MSG_FW_VERSION, msg, 2))
		return 0;
	sayhex("GMU fw versao", ack_payload0);

	/* a6xx_hfi_msg_perf_table_v1: header, nr gpu, nr gmu, gx[16]{vote,khz}, cx[4]{vote,khz} */
	zero(msg, sizeof(msg));
	msg[1] = 8;
	msg[2] = 3;
	for (unsigned i = 0; i < 8; i++) {
		msg[3 + 2 * i] = gx_vote[i];
		msg[4 + 2 * i] = gpu_khz[i];
	}
	for (unsigned i = 0; i < 3; i++) {
		msg[35 + 2 * i] = cx_vote[i];
		msg[36 + 2 * i] = gmu_khz[i];
	}
	if (hfi_send("5c PERF_TABLE", MSG_PERF_TABLE, msg, 43))
		return 0;

	/* a6xx_hfi_msg_bw_table, a6xx_build_bw_table (sdm845/a630): one "off" level */
	zero(msg, sizeof(msg));
	msg[1] = 1;			/* bw_level_num */
	msg[2] = 3;			/* cnoc_cmds_num */
	msg[3] = 3;			/* ddr_cmds_num */
	msg[4] = 0x05;			/* cnoc_wait_bitmask */
	msg[5] = 0x07;			/* ddr_wait_bitmask */
	msg[6] = 0x50034;		/* cnoc_cmds_addrs[0..2] (words 6..11) */
	msg[7] = 0x5007c;
	msg[8] = 0x5004c;
	msg[12] = 0x40000000;		/* cnoc_cmds_data[0][0..2] (words 12..17) */
	msg[13] = 0x00000000;
	msg[14] = 0x40000000;
	msg[18] = 0x60000001;		/* cnoc_cmds_data[1][0..2] (words 18..23) */
	msg[19] = 0x20000001;
	msg[20] = 0x60000001;
	msg[24] = 0x50000;		/* ddr_cmds_addrs[0..2] (words 24..31) */
	msg[25] = 0x5005c;
	msg[26] = 0x5000c;
	msg[32] = 0x40000000;		/* ddr_cmds_data[0][0..2] (words 32..) */
	msg[33] = 0x40000000;
	msg[34] = 0x40000000;
	if (hfi_send("5d BW_TABLE", MSG_BW_TABLE, msg, 160))
		return 0;

	zero(msg, sizeof(msg));
	hfi_send("5e TEST", MSG_TEST, msg, 1);
	return 1;
}

void shim_main(uint64_t dtb)
{
	(void)dtb;
	gpu_screen("OP6 SHIM M11: SMMU + HFI DO GMU", "\n=== OP6 shim M11 gmu hfi ===\n");

	step("1 estado do ABL (so leitura)");
	gpu_state(0);
	if (gpu_cmddb(0) || !votes()) {
		say("1", "sem tabela de votos: parando antes de ligar algo");
		goto out;
	}
	if (!gpu_power_on(0))
		goto out;
	con_skip();
	if (!smmu_probe())
		goto out;
	if (!gpu_have_fw()) {
		step("firmware do GMU");
		say("FW", "SEM FIRMWARE: a630_gmu.bin nao embutido; SMMU nao programada");
		say("FW", "op6/tools/gpu-fw-embed.py <a630_gmu.bin>, build.sh m11");
		goto out;
	}
	if (!smmu_program())
		goto out;
	con_skip();
	hfi_table();
	if (!gpu_gmu_boot(VA_HFI, VA_LOG | (sizeof(mem_log) / 4096 - 1), ~INTR_MSGQ)) {
		smmu_faulted("boot");
		gpu_gmu_halt();
		goto out;
	}
	say("4", "GMU rodando (0xBABEFACE) com a SMMU");
	con_skip();
	if (hfi_talk())
		say("5", "O GMU CONVERSA POR HFI (INIT/VER/PERF/BW)");
	smmu_faulted("fim");
	step("CM3 de volta ao reset");
	gpu_gmu_halt();
out:
	step("fim");
	con_skip();
	say("", "Tire uma foto desta tela!");
	countdown_reboot(con_row(), END_SECONDS);
}
