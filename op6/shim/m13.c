/*
 * OnePlus 6 milestone M13 (GPU stage C2): the zap shader through TrustZone,
 * then the Adreno 630 command processor runs its first ringbuffer and
 * writes a magic value to RAM through the GPU SMMU.
 *
 * Everything M12 does (M11's GPU SMMU for the GMU + HFI, GX on through the
 * GMU, OOB GPU_SET held) is m12.c itself, included below with its shim_main
 * left out; its lines go to the ramlog only (last 6 shown on a failure).
 * Then, facts from Linux v6.6 (GPL; nothing copied) drivers/soc/qcom/
 * mdt_loader.c, drivers/firmware/qcom_scm*.c, drivers/gpu/drm/msm/adreno/
 * adreno_gpu.c and a6xx_gpu.c:
 *
 *   Z  zap (adreno_zap_shader_load -> zap_shader_load_mdt, first load):
 *      qcom_mdt_get_size over the valid PT_LOAD segments (not the hash
 *      segment, memsz != 0) must fit the DT carve-out gpu@8c415000 (0x2000);
 *      metadata = ELF header (phdr[0].p_filesz bytes) + hash segment, in a
 *      4 KiB-aligned contiguous buffer (MMU off = non-cacheable);
 *      SMC PAS_INIT_IMAGE(13, meta), PAS_MEM_SETUP(13, 0x8c415000, size)
 *      (relocatable image), copy the segments relative to the lowest p_paddr,
 *      zero memsz - filesz, SMC PAS_AUTH_AND_RESET(13). No set_remote_state:
 *      v6.6 a6xx_zap_shader_init only loads once per boot (its "loaded"
 *      flag) and never calls SCM_GPU_ZAP_SHADER_RESUME. SMCCC: SMC64,
 *      owner SIP, x0 = 0x42000000 | svc << 8 | cmd (0xc2.. for the one fast
 *      call, the convention probe), x1 = arginfo, x2.. = args; x0 = 1
 *      (INTERRUPTED) is re-issued with x0 = 1 and x6 as returned, -12
 *      (EBUSY) retried after 30 ms. Success = x0 == 0 and x1 == 0.
 *      Any refusal: stop, slumber, GMU reset, countdown.
 *   C  CP (hw_init + a6xx_ucode_load + a6xx_cp_init):
 *      a630_sqe.fw minus its first word in a GPU buffer mapped read-only,
 *      version check (word 1 & 0xfff >= 0x190 => CP_WHERE_AM_I available);
 *      the GPU's own context bank for SID 0 (DT gpu iommus <&adreno_smmu 0>;
 *      CB 0 as arm-smmu-qcom gives the GPU, the GMU moves to the next free
 *      CB), 4 KiB PTEs, SCTLR.HUPCF as qcom_adreno_smmu_write_sctlr,
 *      sCR0.USFCFG (unmatched streams fault, never bypass to PA = VA);
 *      the a630 subset of hw_init's register writes (64-bit addressing,
 *      VBIF QoS/gate, UCHE ranges, ROQ, mem pool, ...); CP_SQE_INSTR_BASE,
 *      CP_RB_BASE, CP_RB_CNTL = BUFSZ(32 KiB) | BLKSZ(32) | NO_UPDATE,
 *      CP_RB_RPTR_ADDR = shadow; CP_SQE_CNTL = 1; CP_ME_INIT (8 words as
 *      a6xx_cp_init) + CP_WHERE_AM_I; CP_SET_SECURE_MODE 0 (the zap path);
 *      then ours: CP_NOP + CP_MEM_WRITE 0xC0FFEE01 to a scratch buffer.
 *      After each flush: CP_RB_RPTR == WPTR, shadow == WPTR, RBBM_STATUS
 *      only CP_AHB_BUSY_CX_MASTER.
 *   Then OOB GPU_SET / BOOT_SLUMBER cleared and M12's shutdown (VBIF halt,
 *   slumber, GMU reset), countdown.
 *
 * Writes outside the GPU block, beyond M12's: the SMCs above and the zap
 * carve-out 0x8c415000..+size (before AUTH_AND_RESET only; never touched
 * after, TZ locks it). Never touched: RBBM_SECVID_TRUST_CNTL (secure),
 * the apps SMMU, PMIC, RPMh from the CPU.
 *
 * -DM13_STOP=1: stop right after the zap is authenticated (no CP).
 */
#define M12_LIB 1
#include "m12.c"

#if defined(__has_include)
#if __has_include("fw_a630_sqe.h")
#include "fw_a630_sqe.h"	/* op6/tools/gpu-fw-embed-cp.py (git-ignored) */
#endif
#if __has_include("fw_a630_zap.h")
#include "fw_a630_zap.h"
#endif
#endif
#ifndef HAVE_SQE_FW
#define A630_SQE_FW_BYTES 0u
static const uint32_t a630_sqe_fw[4];	/* never read: m13 stops at "SEM FIRMWARE" */
#endif
#ifndef HAVE_ZAP_FW
#define A630_ZAP_FW_BYTES 0u
static const uint32_t a630_zap_fw[16];
#endif

#ifndef M13_STOP
#define M13_STOP	0
#endif

/* ---- zap ---- */
#define ZAP_PHYS	0x8c415000ull	/* DT reserved-memory gpu@8c415000 (no-map), build.sh checks */
#define ZAP_SIZE	0x2000u
#define PAS_GPU		13u		/* a6xx_gpu.h GPU_PAS_ID */
#define PT_LOAD		1u
#define MDT_TYPE_MASK	(7u << 24)
#define MDT_TYPE_HASH	(2u << 24)
#define MDT_RELOC	(1u << 27)

/* qcom_scm.h */
#define SVC_PIL		0x02u
#define PIL_INIT_IMAGE	0x01u
#define PIL_MEM_SETUP	0x02u
#define PIL_AUTH_RESET	0x05u
#define PIL_SUPPORTED	0x07u
#define SVC_INFO	0x06u
#define INFO_CALL_AVAIL	0x01u
#define SCM_OWNER_SIP	(2u << 24)
#define SCM_INTERRUPTED	1u
#define SCM_EBUSY	((uint64_t)-12)
#define SCM_WAITQ_SLEEP	2u
#define SCM_ARG_RW	2u

/* ---- GPU memory (GPU VA = Linux's first global iova, SZ_16M) ---- */
#define VA_SQE		0x01000000u
#define VA_RING		0x01010000u
#define VA_SCRATCH	0x01020000u
#define SCR_SHADOW	0		/* scratch[] word: CP_WHERE_AM_I rptr */
#define SCR_MAGIC	64		/* scratch[] word: CP_MEM_WRITE target (+0x100) */
#define MAGIC		0xC0FFEE01u
#define SID_GPU		0u		/* gpu@5000000 iommus = <0x108 0x00> */
#define RING_DW		8192u		/* MSM_GPU_RINGBUFFER_SZ 32 KiB */
#define RB_CNTL_VAL	(12u | (2u << 8) | (1u << 27))	/* BUFSZ ilog2(32K/8), BLKSZ ilog2(32/8), NO_UPDATE */

#define S_CR0_USFCFG	(1u << 10)
#define S_GFSYNR0	0x050
#define SCTLR_HUPCF	(1u << 8)
#define PTE_RDONLY	(1ull << 7)	/* AP[2] */

/* a6xx.xml.h */
#define CP_RB_BASE		0x0800
#define CP_RB_CNTL		0x0802
#define CP_RB_RPTR_ADDR		0x0804
#define CP_RB_RPTR		0x0806
#define CP_RB_WPTR		0x0807
#define CP_INTERRUPT_STATUS	0x0823
#define CP_PROTECT_STATUS	0x0824
#define CP_PROTECT_CNTL		0x084f
#define RBBM_STATUS_CXMASTER	1u	/* CP_AHB_BUSY_CX_MASTER: the CPU reading */
#define INT0_HANG		(1u << 23)

/* adreno_pm4.xml.h opcodes */
#define CP_NOP			0x10u
#define CP_MEM_WRITE		0x3du
#define CP_ME_INIT		0x48u
#define CP_WHERE_AM_I		0x62u
#define CP_SET_SECURE_MODE	0x66u

/* The a630 subset of a6xx hw_init (v6.6), in Linux's order; 64-bit registers
 * as lo/hi pairs. Not done: hwcg (clock gating), perf counters, GMU busy /
 * power counters, CP protect (left as reset: off), RBBM_INT_0_MASK (polled). */
static const uint32_t hw_init_regs[][2] = {
	{ 0xf803, 0 },				/* RBBM_SECVID_TSB_CNTL */
	{ 0xf800, 0 }, { 0xf801, 0 },		/* RBBM_SECVID_TSB_TRUSTED_BASE */
	{ 0xf802, 0 },				/* RBBM_SECVID_TSB_TRUSTED_SIZE */
	{ 0x0842, 1 }, { 0x0c01, 1 }, { 0x8601, 1 }, { 0x8e05, 1 },	/* CP VSC GRAS RB _ADDR_MODE_CNTL */
	{ 0x9e01, 1 }, { 0xbe05, 1 }, { 0xa601, 1 }, { 0x9601, 1 },	/* PC HLSQ VFD VPC */
	{ 0x0e00, 1 }, { 0xae01, 1 }, { 0xb601, 1 }, { 0xf810, 1 },	/* UCHE SP TPL1 SECVID_TSB */
	{ 0x0010, 3 },				/* RBBM_VBIF_CLIENT_QOS_CNTL */
	{ 0x302a, 9 },				/* VBIF_GATE_OFF_WRREQ_EN (a630) */
	{ 0x0e05, 0xffffffc0u }, { 0x0e06, 0x0001ffffu },	/* UCHE_WRITE_RANGE_MAX */
	{ 0x0e09, 0xfffff000u }, { 0x0e0a, 0x0001ffffu },	/* UCHE_TRAP_BASE */
	{ 0x0e07, 0xfffff000u }, { 0x0e08, 0x0001ffffu },	/* UCHE_WRITE_THRU_BASE */
	{ 0x0e0b, 0x00100000u }, { 0x0e0c, 0 },	/* UCHE_GMEM_RANGE_MIN */
	{ 0x0e0d, 0x001fffffu }, { 0x0e0e, 0 },	/* UCHE_GMEM_RANGE_MAX (gmem 1 MiB) */
	{ 0x0e18, 0x804 },			/* UCHE_FILTER_CNTL */
	{ 0x0e17, 0x4 },			/* UCHE_CACHE_WAYS */
	{ 0x08c2, 0x010000c0u },		/* CP_ROQ_THRESHOLDS_2 */
	{ 0x08c1, 0x8040362cu },		/* CP_ROQ_THRESHOLDS_1 */
	{ 0x08c3, 128 },			/* CP_MEM_POOL_SIZE */
	{ 0x9e00, 0x00180000u },		/* PC_DBG_ECO_CNTL */
	{ 0x098d, 0x1 },			/* CP_AHB_CNTL: default slave response ERROR */
	{ 0x8e08, 2u << 1 },			/* RB_NC_MODE_CNTL (hbb_lo 2) */
	{ 0xb604, 2u << 1 },			/* TPL1_NC_MODE_CNTL */
	{ 0xae02, 2u << 1 },			/* SP_NC_MODE_CNTL */
	{ 0x0e01, 2u << 21 },			/* UCHE_MODE_CNTL */
	{ 0x001f, (1u << 30) | 0x1fffffu },	/* RBBM_INTERFACE_HANG_INT_CNTL */
	{ 0x0e19, 1 },				/* UCHE_CLIENT_PF */
};

static uint8_t zap_meta[0x2000] __attribute__((aligned(4096)));
static uint32_t sqe_buf[9 * 1024] __attribute__((aligned(4096)));	/* 36 KiB >= 34184 */
static uint32_t ring[RING_DW] __attribute__((aligned(4096)));
static uint32_t scratch[1024] __attribute__((aligned(4096)));
static uint8_t gpt_pool[3][4096] __attribute__((aligned(4096)));
static unsigned gpt_used;
static uint64_t *gpt_root;
static int gcb = -1;		/* GPU context bank */
static uint32_t gcbbase, wp;

static uint32_t zw(uint32_t off)	/* zap image word at byte offset (4-aligned) */
{
	return a630_zap_fw[off / 4];
}

static uint32_t gr(uint32_t n)
{
	return rd32(GPU(n));
}

/* ---- SCM (SMCCC, Qualcomm SIP) ---- */


static uint64_t scm_r1;	/* x1 of the last call (Linux res.result[0]) */

/* Prints the call in the ramlog before it runs and its result after. 0 = ok. */
static int scm(const char *label, int fast, uint32_t svc, uint32_t cmd, uint32_t arginfo,
    uint64_t a, uint64_t b, uint64_t c, int want_a1_nonzero)
{
	char m[120], *p;
	uint64_t fn = (fast ? 1ull << 31 : 0) | (1u << 30) | SCM_OWNER_SIP | (svc << 8) | cmd;
	uint64_t x0 = fn, x6 = 0, r0, r1 = 0, r2 = 0, r3 = 0;
	unsigned spins = 0, busy = 0;
	uint64_t t0 = now();

	p = put_hex(put(m, "x0="), fn, 8);
	p = put_hex(put(p, " x1="), arginfo, 2);
	p = put_hex(put(p, " x2="), a, 8);
	p = put_hex(put(p, " x3="), b, 8);
	put_hex(put(p, " x4="), c, 8);
	saylog(label, m);
	for (;;) {
		register uint64_t q0 __asm__("x0") = x0;
		register uint64_t q1 __asm__("x1") = arginfo;
		register uint64_t q2 __asm__("x2") = a;
		register uint64_t q3 __asm__("x3") = b;
		register uint64_t q4 __asm__("x4") = c;
		register uint64_t q5 __asm__("x5") = 0;
		register uint64_t q6 __asm__("x6") = x6;
		register uint64_t q7 __asm__("x7") = 0;
		__asm__ volatile("smc #0"
				 : "+r"(q0), "+r"(q1), "+r"(q2), "+r"(q3), "+r"(q4), "+r"(q5), "+r"(q6), "+r"(q7)
				 :
				 : "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
		r0 = q0;
		r1 = q1;
		r2 = q2;
		r3 = q3;
		if (r0 == SCM_INTERRUPTED && spins < 1000000) {
			x0 = SCM_INTERRUPTED;	/* resume: x0 = 1, x6 = TZ's session */
			x6 = q6;
			if ((++spins & 1023) == 0)
				wdog_pet();
			continue;
		}
		if (r0 == SCM_EBUSY && busy < 20) {
			busy++;
			delay_ms(30);
			x0 = fn;
			x6 = 0;
			continue;
		}
		break;
	}
	scm_r1 = r1;
	int ok = r0 == 0 && (want_a1_nonzero ? r1 != 0 : r1 == 0);
	p = put_hex(put(m, "a0="), (uint32_t)r0, 8);
	p = put_hex(put(p, " a1="), (uint32_t)r1, 8);
	p = put_dec(put(p, " i="), spins);
	p = put_dec(put(p, " b="), busy);
	p = put_dec(put(p, " "), ticks_to_us(now() - t0));
	p = put(p, "us");
	if (r0 == SCM_WAITQ_SLEEP)
		p = put(p, " WAITQ");
	put(p, ok ? " ok" : " RECUSA");
	tell(label, m);
	p = put_hex(put(m, "a2="), r2, 16);
	put_hex(put(p, " a3="), r3, 16);
	saylog(label, m);
	return ok ? 0 : -1;
}

/* ---- Z: zap shader ---- */

struct seg {
	uint32_t off, pa, fsz, msz, fl;
};

static struct seg zseg(uint32_t i)
{
	uint32_t o = zw(28) + 32 * i;
	struct seg s = { zw(o + 4), zw(o + 12), zw(o + 16), zw(o + 20), zw(o + 24) };

	return s;
}

static int seg_valid(const struct seg *s, uint32_t type)
{
	return type == PT_LOAD && (s->fl & MDT_TYPE_MASK) != MDT_TYPE_HASH && s->msz;
}

static int zap_load(void)
{
	char b[120], *p;
	uint32_t fsize = A630_ZAP_FW_BYTES, phnum, hash = 0, lo = ~0u, hi = 0, reloc = 0, nload = 0;

	step("Z1 zap: le o ELF a630_zap.mbn");
	if (fsize < 52 || zw(0) != 0x464c457fu || (zw(4) & 0xffff) != 0x0101 || zw(28) != 52 ||
	    (zw(40) >> 16) != 32) {
		tell("ZAP", "nao e ELF32 LE com phdr em 52: parando");
		return 0;
	}
	phnum = zw(44) & 0xffff;
	if (phnum < 2 || phnum > 16 || 52 + 32 * phnum > fsize || zw(52) == PT_LOAD) {
		tell("ZAP", "phdrs estranhos (MDT exige phdr0 nao-LOAD)");
		return 0;
	}
	for (uint32_t i = 1; i < phnum && !hash; i++)
		if ((zseg(i).fl & MDT_TYPE_MASK) == MDT_TYPE_HASH)
			hash = i;
	for (uint32_t i = 0; i < phnum; i++) {
		struct seg s = zseg(i);
		if (!seg_valid(&s, zw(52 + 32 * i)))
			continue;
		nload++;
		if (s.fl & MDT_RELOC)
			reloc = 1;
		if (s.pa < lo)
			lo = s.pa;
		if (s.pa + s.msz > hi)
			hi = (s.pa + s.msz + 0xfffu) & ~0xfffu;
		p = put_dec(put(b, "seg "), i);
		p = put_hex(put(p, " pa="), s.pa, 8);
		p = put_hex(put(p, " f="), s.fsz, 4);
		p = put_hex(put(p, " m="), s.msz, 4);
		put_hex(put(p, " fl="), s.fl, 8);
		saylog("ZAP", b);
	}
	uint32_t size = lo < hi ? hi - lo : 0;
	p = put_dec(put(b, "n="), phnum);
	p = put_dec(put(p, " hash="), hash);
	p = put_dec(put(p, " load="), nload);
	p = put(p, reloc ? " rel" : " abs");
	p = put_hex(put(p, " tam="), size, 4);
	put_hex(put(p, "/"), ZAP_SIZE, 4);
	tell("ZAP", b);
	if (!hash || !size || size > ZAP_SIZE) {
		tell("ZAP", "sem hash / nao cabe em gpu@8c415000: parando");
		return 0;
	}

	/* qcom_mdt_read_metadata: ELF header (phdr0 filesz) + hash segment */
	struct seg h = zseg(hash);
	uint32_t ehsz = zseg(0).fsz, hoff;
	if (ehsz + h.fsz == fsize)
		hoff = ehsz;
	else if (h.off + h.fsz <= fsize)
		hoff = h.off;
	else
		hoff = ~0u;
	if (hoff == ~0u || ehsz + h.fsz > sizeof(zap_meta) || ((ehsz | h.fsz | hoff) & 3) ||
	    (pa(zap_meta) & 0xfff)) {
		tell("ZAP", "metadados fora do arquivo/buffer: parando");
		return 0;
	}
	zero(zap_meta, sizeof(zap_meta));
	for (uint32_t i = 0; i < ehsz; i += 4)
		*(volatile uint32_t *)(zap_meta + i) = zw(i);
	for (uint32_t i = 0; i < h.fsz; i += 4)
		*(volatile uint32_t *)(zap_meta + ehsz + i) = zw(hoff + i);
	dsb();
	p = put_hex(put(b, "meta "), ehsz + h.fsz, 4);
	p = put_hex(put(p, " pa="), pa(zap_meta), 8);
	put_hex(put(p, " w0="), *(volatile uint32_t *)zap_meta, 8);
	tell("ZAP", b);

	/* check every segment before the first SMC */
	uint32_t reloc_base = reloc ? lo : (uint32_t)ZAP_PHYS;
	for (uint32_t i = 0; i < phnum; i++) {
		struct seg s = zseg(i);
		if (!seg_valid(&s, zw(52 + 32 * i)))
			continue;
		uint32_t off = s.pa - reloc_base;
		if (s.pa < reloc_base || off + s.msz > size || s.fsz > s.msz || s.off + s.fsz > fsize ||
		    ((off | s.fsz | s.msz | s.off) & 3)) {
			tell("ZAP", "segmento fora da regiao/arquivo: parando");
			return 0;
		}
	}

	step("Z2 SCM: convencao SMC64 (fast)");
	if (scm("SCM conv", 1, SVC_INFO, INFO_CALL_AVAIL, 1,
		SCM_OWNER_SIP | (SVC_INFO << 8) | INFO_CALL_AVAIL, 0, 0, 1) || scm_r1 != 1) {
		tell("ZAP", "TZ nao responde SMC64 SIP: parando");
		return 0;
	}
	step("Z3 SCM: PAS 13 suportado? (so info)");
	if (!scm("SCM disp", 0, SVC_INFO, INFO_CALL_AVAIL, 1,
		 SCM_OWNER_SIP | (SVC_PIL << 8) | PIL_SUPPORTED, 0, 0, 1))
		scm("SCM sup13", 0, SVC_PIL, PIL_SUPPORTED, 1, PAS_GPU, 0, 0, 1);

	step("Z4 SCM: pas_init_image(13, meta)");
	if (scm("SCM init", 0, SVC_PIL, PIL_INIT_IMAGE, 2 | (SCM_ARG_RW << 6), PAS_GPU, pa(zap_meta), 0, 0)) {
		tell("ZAP", "o TZ recusou os metadados (assinatura?)");
		return 0;
	}
	if (reloc) {
		step("Z5 SCM: pas_mem_setup 8c415000");
		if (scm("SCM mem", 0, SVC_PIL, PIL_MEM_SETUP, 3, PAS_GPU, ZAP_PHYS, size, 0)) {
			tell("ZAP", "o TZ recusou a regiao");
			return 0;
		}
	}

	step("Z6 copia segmentos -> 0x8c415000");
	uint32_t bad = 0, bytes = 0;
	for (uint32_t i = 0; i < phnum; i++) {
		struct seg s = zseg(i);
		if (!seg_valid(&s, zw(52 + 32 * i)))
			continue;
		uintptr_t dst = (uintptr_t)ZAP_PHYS + (s.pa - reloc_base);
		for (uint32_t k = 0; k < s.msz; k += 4)
			wr32(dst + k, k < s.fsz ? zw(s.off + k) : 0);
		dsb();
		for (uint32_t k = 0; k < s.msz; k += 4)
			bad += rd32(dst + k) != (k < s.fsz ? zw(s.off + k) : 0);
		bytes += s.msz;
	}
	p = put_hex(put(b, "copiado "), bytes, 4);
	p = put_dec(put(p, " bytes, erradas="), bad);
	tell("ZAP", b);
	if (bad) {
		tell("ZAP", "a regiao nao guarda o que escrevi: parando");
		return 0;
	}

	step("Z7 SCM: pas_auth_and_reset(13)");
	if (scm("SCM auth", 0, SVC_PIL, PIL_AUTH_RESET, 1, PAS_GPU, 0, 0, 0)) {
		tell("ZAP", "o TZ RECUSOU o zap (assinatura/estado)");
		return 0;
	}
	tell("ZAP", "ZAP ACEITO PELO TRUSTZONE (PAS 13)");
	return 1;
}

/* GPU SMMU seen after TZ ran (read only): did it add rules of its own? */
static void smmu_after_zap(void)
{
	char b[96], *p;
	uint32_t valid = 0, on = 0;

	for (uint32_t n = 0; n < nsmr && (idr0 & IDR0_SMS); n++)
		if (rd32(GPU_SMMU + S_SMR(n)) & SMR_VALID)
			valid++;
	for (uint32_t i = 0; i < ncb; i++)
		if (rd32(GPU_SMMU + npage * pgsz + i * pgsz + CB_SCTLR) & 1)
			on |= 1u << i;
	p = put_dec(put(b, "pos-zap smr validos="), valid);
	p = put_hex(put(p, " cb on="), on, 2);
	put_hex(put(p, " cr0="), rd32(GPU_SMMU + S_CR0), 8);
	tell("SMMU", b);
}

/* ---- context banks: GPU on CB 0 (arm-smmu-qcom), GMU on the next free ---- */

static int pick_cbs(void)
{
	char b[64], *p;
	uint32_t busy = 0;

	for (uint32_t n = 0; n < nsmr && (idr0 & IDR0_SMS); n++) {
		uint32_t smr = rd32(GPU_SMMU + S_SMR(n)), s2cr = rd32(GPU_SMMU + S_S2CR(n));
		if ((smr & SMR_VALID) && ((s2cr >> 16) & 3) == 0)
			busy |= 1u << (s2cr & 0x1f);
	}
	for (uint32_t i = 0; i < ncb; i++)
		if (rd32(GPU_SMMU + npage * pgsz + i * pgsz + CB_SCTLR) & 1)
			busy |= 1u << i;
	gcb = -1;
	cb = -1;
	for (uint32_t i = 0; i < ncb && gcb < 0; i++)
		if (!(busy & (1u << i)))
			gcb = (int)i;
	for (uint32_t i = 0; i < ncb && cb < 0; i++)
		if (!(busy & (1u << i)) && (int)i != gcb)
			cb = (int)i;
	p = put_hex(put(b, "ocupados="), busy, 2);
	p = put(p, " gpu=");
	p = gcb < 0 ? put(p, "-") : put_dec(p, (uint32_t)gcb);
	p = put(p, " gmu=");
	p = cb < 0 ? put(p, "-") : put_dec(p, (uint32_t)cb);
	saylog("CB", b);
	return gcb >= 0 && cb >= 0;
}

/* ---- C: CP ---- */

static uint64_t *gpt_new(void)
{
	if (gpt_used >= 3)
		return 0;
	uint64_t *t = (uint64_t *)gpt_pool[gpt_used++];
	zero(t, 4096);
	return t;
}

static int gpt_map(uint64_t va, const void *buf, uint32_t size, uint64_t attr)
{
	for (uint32_t off = 0; off < size; off += 4096) {
		uint64_t v = va + off, *t = gpt_root;
		for (int shift = 30; shift >= 21; shift -= 9) {
			uint64_t *e = &t[(v >> shift) & 0x1ff];
			if (!(*e & 1)) {
				uint64_t *n = gpt_new();
				if (!n)
					return 0;
				*e = pa(n) | PTE_TABLE;
			}
			t = (uint64_t *)(uintptr_t)(*e & PTE_ADDR);
		}
		t[(v >> 12) & 0x1ff] = (pa(buf) + off) | attr;
	}
	return 1;
}

static int gpu_faulted(const char *when)
{
	uintptr_t c = GPU_SMMU + gcbbase;
	uint32_t fsr, gfsr;
	char b[120], *p;

	if (!gcbbase)		/* gpu_ctx() never ran: no GPU context bank to read */
		return 0;
	fsr = rd32(c + CB_FSR);
	gfsr = rd32(GPU_SMMU + S_GFSR);
	if (!(fsr & FSR_ALL) && !gfsr)
		return 0;
	p = put(put(b, when), " fsr=");
	p = put_hex(p, fsr, 8);
	p = put_hex(put(p, " far="), rd64(c + CB_FAR), 10);
	put_hex(put(p, " syn="), rd32(c + CB_FSYNR0), 8);
	tell("FALHA", b);
	p = put_hex(put(b, "gfsr="), gfsr, 8);
	p = put_hex(put(p, " gsyn0="), rd32(GPU_SMMU + S_GFSYNR0), 8);
	put_hex(put(p, " gsyn1(sid)="), rd32(GPU_SMMU + S_GFSYNR1), 8);
	tell("FALHA", b);
	return 1;
}

static uint64_t ats(uintptr_t c, uint32_t va)
{
	wr64(c + CB_ATS1PR, va);
	for (int i = 0; i < 100 && (rd32(c + CB_ATSR) & 1); i++)
		delay_ms(1);
	return rd64(c + CB_PAR);
}

static int sqe_load(void)
{
	char b[96], *p;
	uint32_t words = A630_SQE_FW_BYTES / 4 - 1, bad = 0, ver = a630_sqe_fw[1];

	step("C1 SQE: a630_sqe.fw no buffer");
	if (words * 4 > sizeof(sqe_buf) || (pa(sqe_buf) | pa(ring) | pa(scratch) | pa(gpt_pool)) & 0xfff) {
		tell("SQE", "buffer pequeno/desalinhado: parando");
		return 0;
	}
	zero(sqe_buf, sizeof(sqe_buf));
	for (uint32_t i = 0; i < words; i++)		/* adreno_fw_create_bo: skip word 0 */
		((volatile uint32_t *)sqe_buf)[i] = a630_sqe_fw[i + 1];
	dsb();
	for (uint32_t i = 0; i < words; i++)
		bad += ((volatile uint32_t *)sqe_buf)[i] != a630_sqe_fw[i + 1];
	int patched = (ver & 0xf) == 0xa && (a630_sqe_fw[3] & 0xf) >= 1;
	p = put_hex(put(b, "ver="), ver & 0xfff, 3);
	p = put_dec(put(p, " palavras="), words);
	p = put_dec(put(p, " erradas="), bad);
	put_hex(put(p, " pa="), pa(sqe_buf), 8);
	tell("SQE", b);
	if (bad || (!patched && (ver & 0xfff) < 0x190)) {
		tell("SQE", "copia errada / versao < 0x190 (Linux recusa)");
		return 0;
	}
	return 1;
}

static int gpu_ctx(void)
{
	char b[120], *p;
	uintptr_t c;
	uint64_t ttbr;

	step("C2 SMMU: contexto da GPU (SID 0)");
	gpt_root = gpt_new();
	if (!gpt_root || !gpt_map(VA_SQE, sqe_buf, sizeof(sqe_buf), PTE_GMU | PTE_RDONLY) ||
	    !gpt_map(VA_RING, ring, sizeof(ring), PTE_GMU) ||
	    !gpt_map(VA_SCRATCH, scratch, sizeof(scratch), PTE_GMU)) {
		tell("SMMU", "sem tabelas p/ a GPU: parando");
		return 0;
	}
	dsb();
	gcbbase = npage * pgsz + (uint32_t)gcb * pgsz;
	c = GPU_SMMU + gcbbase;
	if (rd32(c + CB_SCTLR) & 1) {
		tell("SMMU", "CB da GPU ja ligado por alguem: parando");
		return 0;
	}
	ttbr = pa(gpt_root) | ((uint64_t)(uint32_t)gcb << 48);
	wr(c + CB_SCTLR, 0);
	wr(GPU_SMMU + pgsz + S_CBA2R(gcb), 1);		/* VA64 */
	wr(GPU_SMMU + pgsz + S_CBAR(gcb), CBAR_S1);
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
	wr(c + CB_SCTLR, SCTLR_ON | SCTLR_HUPCF);
	if (!(rd32(c + CB_SCTLR) & 1) || rd32(c + CB_TTBR0) != (uint32_t)ttbr ||
	    ((rd32(GPU_SMMU + pgsz + S_CBAR(gcb)) >> 16) & 3) != 1) {
		p = put_hex(put(b, "sctlr="), rd32(c + CB_SCTLR), 4);
		p = put_hex(put(p, " ttbr0="), rd32(c + CB_TTBR0), 8);
		put(p, " NAO aceitou: parando");
		tell("SMMU", b);
		return 0;
	}

	int saved = cb, n;
	cb = gcb;				/* smmu_route() routes to the global cb */
	gpu_quiet = 1;
	n = smmu_route(SID_GPU, -1);
	gpu_quiet = 0;
	cb = saved;
	if (n < 0) {
		tell("SMMU", "regra do SID 0 NAO ficou: parando");
		return 0;
	}
	wr(GPU_SMMU + S_TLBIALLNSNH, 0);
	wr(GPU_SMMU + S_TLBGSYNC, 0);
	for (int i = 0; i < 100 && (rd32(GPU_SMMU + S_TLBGSTATUS) & 1); i++)
		delay_ms(1);
	uint32_t cr0 = rd32(GPU_SMMU + S_CR0);
	/* only when SID 5 and 4 both have rules: m11 tolerates a missing SID 4 rule, which would then fault */
	int both = smr_slot(SID_GMU, -1) >= 0 && smr_slot(SID_GMU_USER, -1) >= 0 &&
	    (rd32(GPU_SMMU + S_SMR(smr_slot(SID_GMU_USER, -1))) & SMR_VALID);
	if (!(cr0 & S_CR0_USFCFG) && both)
		wr(GPU_SMMU + S_CR0, cr0 | S_CR0_USFCFG);	/* arm-smmu disable_bypass: unmatched SIDs fault */
	p = put_dec(put(b, "cb"), (uint32_t)gcb);
	p = put_dec(put(p, " sid0 smr"), (uint32_t)n);
	p = put_hex(put(p, " gmu cb"), (uint32_t)cb, 1);
	p = put_hex(put(p, " cr0 "), cr0, 8);
	put_hex(put(p, ">"), rd32(GPU_SMMU + S_CR0), 8);
	tell("SMMU", b);
	if ((both && !(rd32(GPU_SMMU + S_CR0) & S_CR0_USFCFG)) || (rd32(GPU_SMMU + S_CR0) & S_CR0_CLIENTPD)) {
		tell("SMMU", "sCR0 USFCFG/CLIENTPD nao como pedi: parando");
		return 0;
	}

	if (!(idr0 & IDR0_S1TS) || (idr0 & IDR0_ATOSNS)) {
		tell("ATS", "sem operacao de traducao: confio nas leituras");
		goto ats_done;
	}
	uint64_t par_r = ats(c, VA_RING), par_s = ats(c, VA_SQE + 0x8000);
	p = put_hex(put(b, "anel "), par_r, 10);
	put_hex(put(p, " sqe+8000 "), par_s, 10);
	tell("ATS", b);
	if ((par_r & 1) || (par_r & 0xfffffff000ull) != pa(ring) ||
	    (par_s & 1) || (par_s & 0xfffffff000ull) != pa(sqe_buf) + 0x8000) {
		tell("ATS", "a SMMU nao traduz VA da GPU: parando");
		gpu_faulted("ats");
		return 0;
	}
ats_done:
	wr(c + CB_FSR, FSR_ALL);
	wr(GPU_SMMU + S_GFSR, rd32(GPU_SMMU + S_GFSR));
	return 1;
}

static int hw_init(void)
{
	char b[96], *p;
	uint32_t diff = 0, unread = 0, n = sizeof(hw_init_regs) / sizeof(hw_init_regs[0]);

	step("C3 hw_init a630 (VBIF/UCHE/64 bit)");
	for (uint32_t i = 0; i < n; i++) {
		uint32_t r = hw_init_regs[i][0], v = hw_init_regs[i][1];
		wr(GPU(r), v);
		if (r >= 0xf000) {	/* RBBM_SECVID_TSB_*: written as Linux does, never read */
			unread++;
			continue;
		}
		uint32_t got = gr(r);
		if (got != v) {
			diff++;
			p = put_hex(put(b, "reg "), r, 4);
			p = put_hex(put(p, " w="), v, 8);
			put_hex(put(p, " r="), got, 8);
			saylog("INIT", b);
		}
	}
	p = put_dec(put(b, "escritas="), n);
	p = put_dec(put(p, " nao lidas="), unread);
	p = put_dec(put(p, " diferentes="), diff);
	p = put_hex(put(p, " prot="), gr(CP_PROTECT_CNTL), 8);
	put_hex(put(p, " st="), gr(RBBM_STATUS), 8);
	tell("INIT", b);
	/* the CP's own 64-bit addressing is what the next steps need */
	if (gr(0x0842) != 1) {
		tell("INIT", "CP_ADDR_MODE_CNTL nao ficou 1: parando");
		return 0;
	}
	return 1;
}

static uint32_t par4(uint32_t v)
{
	v ^= v >> 16;
	v ^= v >> 8;
	v ^= v >> 4;
	return (0x9669u >> (v & 0xf)) & 1;
}

static void emit(uint32_t v)
{
	((volatile uint32_t *)ring)[wp++] = v;
}

static void pkt7(uint32_t op, uint32_t cnt)
{
	emit(0x70000000u | cnt | (par4(cnt) << 15) | ((op & 0x7f) << 16) | (par4(op) << 23));
}

/* a6xx_flush: CP_WHERE_AM_I (shadow rptr) then the WPTR write. */
static void flush(void)
{
	pkt7(CP_WHERE_AM_I, 2);
	emit(VA_SCRATCH + 4 * SCR_SHADOW);
	emit(0);
	dsb();
	wr(GPU(CP_RB_WPTR), wp);
}

/* a6xx_idle: rptr (register and shadow) == wptr, RBBM idle but the CX master bit, no hang irq. */
static int cp_idle(const char *label, unsigned ms)
{
	char b[120], *p;
	uint64_t t0 = now(), lim = ticks_per_s() / 1000 * ms;
	uint32_t r, sh, st, i0;
	int ok;

	for (;;) {
		r = gr(CP_RB_RPTR);
		sh = ((volatile uint32_t *)scratch)[SCR_SHADOW];
		st = gr(RBBM_STATUS);
		i0 = gr(RBBM_INT_0_STATUS);
		ok = r == wp && sh == wp && !(st & ~RBBM_STATUS_CXMASTER) && !(i0 & INT0_HANG);
		if (ok || now() - t0 > lim)
			break;
	}
	uint32_t us = ticks_to_us(now() - t0);
	wdog_pet();
	p = put_dec(put(b, "rptr="), r);
	p = put_dec(put(p, " sombra="), sh);
	p = put_dec(put(p, " wptr="), wp);
	p = put_hex(put(p, " st="), st, 8);
	p = put_dec(put(p, " "), us);
	put(p, ok ? "us ok" : "us TEMPO");
	tell(label, b);
	p = put_hex(put(b, "flt="), gr(CP_HW_FAULT), 8);
	p = put_hex(put(p, " ci="), gr(CP_INTERRUPT_STATUS), 8);
	p = put_hex(put(p, " pr="), gr(CP_PROTECT_STATUS), 8);
	put_hex(put(p, " i0="), i0, 8);
	(ok ? saylog : tell)(label, b);
	p = put_hex(put(b, "st1="), gr(CP_STATUS_1), 8);
	p = put_hex(put(p, " st3="), gr(RBBM_STATUS + 3), 8);
	put_hex(put(p, " sqe="), gr(CP_SQE_CNTL), 8);
	saylog(label, b);
	gpu_faulted(label);
	return ok;
}

static int cp_run(void)
{
	char b[120], *p;

	step("C4 CP: SQE base, anel 32K, sombra");
	zero(ring, sizeof(ring));
	zero(scratch, sizeof(scratch));
	wp = 0;
	if (!wrv("sqe_base", GPU(CP_SQE_INSTR_BASE), VA_SQE, ~0u) ||
	    !wrv("sqe_base_hi", GPU(CP_SQE_INSTR_BASE + 1), 0, ~0u) ||
	    !wrv("rb_base", GPU(CP_RB_BASE), VA_RING, ~0u) ||
	    !wrv("rb_base_hi", GPU(CP_RB_BASE + 1), 0, ~0u) ||
	    !wrv("rb_cntl", GPU(CP_RB_CNTL), RB_CNTL_VAL, 0x08003f3fu) ||
	    !wrv("rptr_addr", GPU(CP_RB_RPTR_ADDR), VA_SCRATCH + 4 * SCR_SHADOW, ~0u) ||
	    !wrv("rptr_addr_hi", GPU(CP_RB_RPTR_ADDR + 1), 0, ~0u))
		return 0;
	p = put_dec(put(b, "rptr="), gr(CP_RB_RPTR));
	p = put_dec(put(p, " wptr="), gr(CP_RB_WPTR));
	p = put_hex(put(p, " cntl="), gr(CP_RB_CNTL), 8);
	put_hex(put(p, " sqe="), gr(CP_SQE_CNTL), 8);
	tell("CP", b);
	if (gr(CP_RB_RPTR) || gr(CP_RB_WPTR)) {
		tell("CP", "ponteiros do anel nao zerados: parando");
		return 0;
	}

	step("C5 CP_SQE_CNTL=1 + CP_ME_INIT");
	wr(GPU(CP_SQE_CNTL), 1);
	pkt7(CP_ME_INIT, 8);
	emit(0x0000002f);	/* a6xx_cp_init: 6 fields follow */
	emit(0x00000003);	/* multiple hardware contexts */
	emit(0x20000000);	/* error detection */
	emit(0);			/* no header dump */
	emit(0);
	emit(0);			/* no workarounds */
	emit(0);
	emit(0);
	flush();
	if (!cp_idle("ME_INIT", 500))
		return 0;

	step("C6 CP_SET_SECURE_MODE 0");
	pkt7(CP_SET_SECURE_MODE, 1);
	emit(0);
	flush();
	if (!cp_idle("SECURE0", 500))
		return 0;

	step("C7 CP_NOP + CP_MEM_WRITE magico");
	pkt7(CP_NOP, 1);
	emit(0x4f503633u);	/* "OP63", ignored */
	pkt7(CP_MEM_WRITE, 3);
	emit(VA_SCRATCH + 4 * SCR_MAGIC);
	emit(0);
	emit(MAGIC);
	flush();
	int idle = cp_idle("MEMWRITE", 500);

	step("C8 le o buffer escrito pelo CP");
	uint64_t t0 = now(), lim = ticks_per_s() / 10;
	uint32_t v;
	while ((v = ((volatile uint32_t *)scratch)[SCR_MAGIC]) != MAGIC && now() - t0 <= lim)
		;
	p = put_hex(put(b, "valor="), v, 8);
	p = put_dec(put(p, " +"), ticks_to_us(now() - t0));
	p = put_hex(put(p, "us va="), VA_SCRATCH + 4 * SCR_MAGIC, 8);
	put_hex(put(p, " pa="), pa(scratch) + 4 * SCR_MAGIC, 8);
	tell("MEM", b);
	if (v != MAGIC) {
		tell("MEM", "o CP NAO escreveu o valor");
		return 0;
	}
	tell("13", idle ? "O CP ESCREVEU C0FFEE01 NA RAM PELA SMMU" : "valor escrito, mas o CP nao ficou ocioso");
	return 1;
}

/* The M12 part failed: show its last lines and speak again. */
static void loud(void)
{
	m12_hush = 0;
	gpu_quiet_dump();
}

void shim_main(uint64_t dtb)
{
	char b[96], *p;

	(void)dtb;
	gpu_screen(M13_STOP ? "OP6 SHIM M13: ZAP (PARA APOS AUTH)" : "OP6 SHIM M13: ZAP + CP (MEM_WRITE)",
	    "\n=== OP6 shim M13 zap cp ===\n");

	step("0 firmware embutido?");
	if (!gpu_have_fw() || !A630_SQE_FW_BYTES || !A630_ZAP_FW_BYTES) {
		p = put(b, "SEM FIRMWARE:");
		if (!gpu_have_fw())
			p = put(p, " a630_gmu.bin");
		if (!A630_SQE_FW_BYTES)
			p = put(p, " a630_sqe.fw");
		if (!A630_ZAP_FW_BYTES)
			put(p, " a630_zap.mbn");
		say("FW", b);
		say("FW", "nada foi ligado nem escrito");
		say("FW", "tools/gpu-fw-embed.py + gpu-fw-embed-cp.py");
		goto out;
	}

	step("1 estado do ABL (so leitura)");
	gpu_state(0);
	gpu_quiet = 1;		/* M11 + M12 part: ramlog only, last lines shown on failure */
	m12_hush = 1;
	if (gpu_cmddb(0) || !votes()) {
		loud();
		tell("1", "sem tabela de votos: parando");
		goto out;
	}
	if (!gpu_power_on(0)) {
		loud();
		goto out;
	}
	gx_state("GX antes", 1);
	if (!smmu_probe() || !pick_cbs()) {
		loud();
		tell("SMMU", "sem 2 context banks livres: parando");
		goto out;
	}
	if (!smmu_program()) {
		loud();
		goto out;
	}
	hfi_table();
	gpu_rpmh_ctrl = RPMH_CTRL_LINUX;
	if (!gpu_gmu_boot(VA_HFI, VA_LOG | (sizeof(mem_log) / 4096 - 1), ~INTR_MSGQ)) {
		smmu_faulted("boot");
		loud();
		gpu_gmu_halt();
		goto out;
	}
	booted = 1;
	if (!gx_rail_on()) {
		loud();
		goto down;
	}
	step("7 HFI (INIT/VER/PERF/BW/TEST)");
	if (!hfi_talk()) {
		loud();
		goto down;
	}
	dcvs();
	step("9 OOB GPU_SET: segura a GX");
	gpu_set = oob_set("OOB GPU", OOB_GPU_SET, OOB_GPU_ACK);
	gx_on = gx_state("GX", 1) && gpu_set;
	if (!gx_on || !probes()) {
		loud();
		tell("GX", "nao confirmada / RBBM FFFFFFFF: sem zap");
		goto clear;
	}
	gpu_quiet = 0;
	m12_hush = 0;
	p = put_dec(put(b, "GX "), gpu_khz[M12_OPP] / 1000);
	p = put_hex(put(p, "MHz GPU_SET ok RBBM st="), gr(RBBM_STATUS), 8);
	put_dec(put(p, " gmu cb"), (uint32_t)cb);
	tell("M12", b);
	if (smmu_faulted("M12")) {
		tell("M12", "falha na SMMU do GMU: sem zap");
		goto clear;
	}

	if (!zap_load())
		goto clear;
	smmu_after_zap();
	if (M13_STOP) {
		tell("13", "M13_STOP: CP nao iniciado");
		goto clear;
	}
	if (sqe_load() && gpu_ctx() && hw_init())
		cp_run();
	gpu_faulted("fim CP");
	smmu_faulted("fim CP");
	p = put_hex(put(b, "st="), gr(RBBM_STATUS), 8);
	p = put_hex(put(p, " rptr="), gr(CP_RB_RPTR), 4);
	put_hex(put(p, " ao="), rd32(GMU(GMU_AO_COUNTER_L)), 8);
	saylog("CP fim", b);
clear:
	step("9 OOB clear GPU_SET + BOOT_SLUMBER");
	if (gpu_set)
		oob_clear(OOB_GPU_ACK);
	oob_clear(OOB_BOOT_ACK);
down:
	gpu_quiet = 0;
	m12_hush = 0;
	drop();
	smmu_faulted("fim");
	step("CM3 de volta ao reset");
	if (booted)
		gpu_gmu_halt();
out:
	gpu_quiet = 0;
	m12_hush = 0;
	step("fim");
	con_skip();
	say("", "Tire uma foto desta tela!");
	countdown_reboot(con_row(), END_SECONDS);
}
