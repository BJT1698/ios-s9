/*
 * sweetgpu-test: Milestone G2 userland test for SweetGPUUserClient (Adreno 630).
 *
 * Opens the SweetGPU service, prints its IORegistry properties, queries
 * GET_PARAM (0x40), allocates command and target BOs via GEM_NEW (0x42),
 * queries their GPU VAs via GEM_INFO (0x43), maps both via IOConnectMapMemory64,
 * submits an indirect buffer containing CP_NOP + CP_MEM_WRITE 0xC0FFEE02 to
 * the target BO via GEM_SUBMIT (0x46), waits via WAIT_FENCE (0x47), verifies
 * the target BO contents in userland, and runs N back-to-back submissions
 * (default 100, or -n COUNT) to test ring wrap-around and fence progression.
 *
 *   sweetgpu-test [-n COUNT]
 */
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef mach_port_t io_object_t;
typedef io_object_t io_service_t;
typedef io_object_t io_connect_t;
typedef uint32_t IOOptionBits;

#ifndef kIOMapAnywhere
#define kIOMapAnywhere 0x00000001u
#endif

extern CFMutableDictionaryRef IOServiceMatching(const char *name);
extern io_service_t IOServiceGetMatchingService(mach_port_t mainPort, CFDictionaryRef matching);
extern kern_return_t IORegistryEntryCreateCFProperties(io_object_t entry, CFMutableDictionaryRef *properties,
    CFAllocatorRef allocator, uint32_t options);
extern kern_return_t IOObjectRelease(io_object_t object);
extern kern_return_t IOServiceOpen(io_service_t service, task_port_t owningTask, uint32_t type, io_connect_t *connect);
extern kern_return_t IOServiceClose(io_connect_t connect);
extern kern_return_t IOConnectCallStructMethod(mach_port_t connection, uint32_t selector, const void *inputStruct,
    size_t inputStructCnt, void *outputStruct, size_t *outputStructCnt);
extern kern_return_t IOConnectMapMemory64(io_connect_t connect, uint32_t memoryType, task_port_t intoTask,
    mach_vm_address_t *atAddress, mach_vm_size_t *ofSize, IOOptionBits options);
extern kern_return_t IOConnectUnmapMemory64(io_connect_t connect, uint32_t memoryType, task_port_t fromTask,
    mach_vm_address_t atAddress);

/* msm DRM uAPI selectors and structs (include/uapi/drm/msm_drm.h, MIT) */
enum {
	kDrmGemClose            = 0x09,
	kMsmGetParam            = 0x40,
	kMsmSetParam            = 0x41,
	kMsmGemNew              = 0x42,
	kMsmGemInfo             = 0x43,
	kMsmGemCpuPrep          = 0x44,
	kMsmGemCpuFini          = 0x45,
	kMsmGemSubmit           = 0x46,
	kMsmWaitFence           = 0x47,
	kMsmGemMadvise          = 0x48,
	kMsmSubmitQueueNew      = 0x4a,
	kMsmSubmitQueueClose    = 0x4b,
	kMsmSubmitQueueQuery    = 0x4c,
};

struct msm_timespec {
	int64_t tv_sec;
	int64_t tv_nsec;
};

struct drm_msm_param {
	uint32_t pipe;
	uint32_t param;
	uint64_t value;
	uint32_t len;
	uint32_t pad;
};

struct drm_msm_gem_new {
	uint64_t size;
	uint32_t flags;
	uint32_t handle;
};

struct drm_msm_gem_info {
	uint32_t handle;
	uint32_t info;
	uint64_t value;
	uint32_t len;
	uint32_t pad;
};

struct drm_msm_gem_cpu_prep {
	uint32_t handle;
	uint32_t op;
	struct msm_timespec timeout;
};

struct drm_msm_gem_submit_cmd {
	uint32_t type;
	uint32_t submit_idx;
	uint32_t submit_offset;
	uint32_t size;
	uint32_t pad;
	uint32_t nr_relocs;
	uint64_t relocs;
};

struct drm_msm_gem_submit_bo {
	uint32_t flags;
	uint32_t handle;
	uint64_t presumed;
};

struct drm_msm_gem_submit {
	uint32_t flags;
	uint32_t fence;
	uint32_t nr_bos;
	uint32_t nr_cmds;
	uint64_t bos;
	uint64_t cmds;
	int32_t fence_fd;
	uint32_t queueid;
	uint64_t in_syncobjs;
	uint64_t out_syncobjs;
	uint32_t nr_in_syncobjs;
	uint32_t nr_out_syncobjs;
	uint32_t syncobj_stride;
	uint32_t pad;
};

struct drm_msm_wait_fence {
	uint32_t fence;
	uint32_t flags;
	struct msm_timespec timeout;
	uint32_t queueid;
};

struct drm_msm_submitqueue {
	uint32_t flags;
	uint32_t prio;
	uint32_t id;
};

struct drm_gem_close {
	uint32_t handle;
	uint32_t pad;
};

#define MSM_INFO_GET_OFFSET     0u
#define MSM_INFO_GET_IOVA       1u
#define MSM_BO_WC               0x00020000u
#define MSM_SUBMIT_CMD_BUF      1u
#define MSM_SUBMIT_BO_READ      0x00000001u
#define MSM_SUBMIT_BO_WRITE     0x00000002u

#define CP_NOP                  0x10u
#define CP_MEM_WRITE            0x3du
#define MAGIC_G2                0xC0FFEE02u

static uint32_t
par4(uint32_t v)
{
	v ^= v >> 16;
	v ^= v >> 8;
	v ^= v >> 4;
	return (0x9669u >> (v & 0xfu)) & 1u;
}

static uint32_t
pkt7_hdr(uint32_t op, uint32_t cnt)
{
	return 0x70000000u | cnt | (par4(cnt) << 15) | ((op & 0x7fu) << 16) | (par4(op) << 23);
}

static uint64_t
now_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

static int
call_ioctl(io_connect_t conn, uint32_t sel, void *buf, size_t len)
{
	size_t out_len = len;
	kern_return_t kr = IOConnectCallStructMethod(conn, sel, buf, len, buf, &out_len);
	return kr == KERN_SUCCESS ? 0 : (int)kr;
}

static void
print_registry_props(io_service_t svc)
{
	CFMutableDictionaryRef props = NULL;
	if (IORegistryEntryCreateCFProperties(svc, &props, NULL, 0) != KERN_SUCCESS || !props) {
		return;
	}
	const char *keys[] = {
		"SweetGPUState", "SweetGPUStage", "SweetGPUMagic",
		"SweetGPUFirmwareGMU", "SweetGPUFirmwareSQE"
	};
	printf("1. IORegistry SweetGPU properties:\n");
	for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
		CFStringRef k = CFStringCreateWithCString(NULL, keys[i], kCFStringEncodingUTF8);
		CFTypeRef v = CFDictionaryGetValue(props, k);
		CFRelease(k);
		if (!v) {
			continue;
		}
		if (CFGetTypeID(v) == CFStringGetTypeID()) {
			char s[128] = "";
			CFStringGetCString((CFStringRef)v, s, sizeof(s), kCFStringEncodingUTF8);
			printf("   %-22s %s\n", keys[i], s);
		} else if (CFGetTypeID(v) == CFNumberGetTypeID()) {
			long long n = 0;
			CFNumberGetValue((CFNumberRef)v, kCFNumberLongLongType, &n);
			printf("   %-22s %lld (0x%llx)\n", keys[i], n, (unsigned long long)n);
		}
	}
	CFRelease(props);
}

int
main(int argc, char **argv)
{
	int loops = 100, ch;
	while ((ch = getopt(argc, argv, "n:")) != -1) {
		switch (ch) {
		case 'n':
			loops = atoi(optarg);
			if (loops < 1) {
				loops = 1;
			}
			break;
		default:
			fprintf(stderr, "usage: sweetgpu-test [-n COUNT]\n");
			return 2;
		}
	}

	io_service_t svc = IOServiceGetMatchingService(MACH_PORT_NULL, IOServiceMatching("SweetGPU"));
	if (svc == MACH_PORT_NULL) {
		fprintf(stderr, "sweetgpu-test: SweetGPU service not found in IORegistry\n");
		return 1;
	}
	print_registry_props(svc);

	io_connect_t conn = MACH_PORT_NULL;
	kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &conn);
	IOObjectRelease(svc);
	if (kr != KERN_SUCCESS || conn == MACH_PORT_NULL) {
		fprintf(stderr, "sweetgpu-test: IOServiceOpen failed: 0x%x (is sweetgpu=2 set?)\n", kr);
		return 1;
	}

	printf("2. msm GET_PARAM (0x40):\n");
	static const struct {
		uint32_t id;
		const char *name;
	} params[] = {
		{ 0x01, "GPU_ID" },
		{ 0x02, "GMEM_SIZE" },
		{ 0x03, "CHIP_ID" },
		{ 0x04, "MAX_FREQ" },
		{ 0x05, "TIMESTAMP" },
		{ 0x06, "GMEM_BASE" },
		{ 0x07, "PRIORITIES" },
		{ 0x08, "PP_PGTABLE" },
		{ 0x09, "FAULTS" },
		{ 0x10, "HIGHEST_BANK_BIT" },
	};
	for (size_t i = 0; i < sizeof(params) / sizeof(params[0]); i++) {
		struct drm_msm_param p;
		memset(&p, 0, sizeof(p));
		p.param = params[i].id;
		int rc = call_ioctl(conn, kMsmGetParam, &p, sizeof(p));
		if (rc != 0) {
			fprintf(stderr, "sweetgpu-test: GET_PARAM %s failed: 0x%x\n", params[i].name, rc);
			IOServiceClose(conn);
			return 1;
		}
		printf("   %-22s %llu (0x%llx)\n", params[i].name,
		    (unsigned long long)p.value, (unsigned long long)p.value);
	}

	struct drm_msm_submitqueue sq;
	memset(&sq, 0, sizeof(sq));
	if (call_ioctl(conn, kMsmSubmitQueueNew, &sq, sizeof(sq)) != 0) {
		fprintf(stderr, "sweetgpu-test: SUBMITQUEUE_NEW failed\n");
		IOServiceClose(conn);
		return 1;
	}

	struct drm_msm_gem_new bo_cmd, bo_dst;
	memset(&bo_cmd, 0, sizeof(bo_cmd));
	memset(&bo_dst, 0, sizeof(bo_dst));
	bo_cmd.size = 4096;
	bo_cmd.flags = MSM_BO_WC;
	bo_dst.size = 4096;
	bo_dst.flags = MSM_BO_WC;
	if (call_ioctl(conn, kMsmGemNew, &bo_cmd, sizeof(bo_cmd)) != 0 ||
	    call_ioctl(conn, kMsmGemNew, &bo_dst, sizeof(bo_dst)) != 0) {
		fprintf(stderr, "sweetgpu-test: GEM_NEW failed\n");
		IOServiceClose(conn);
		return 1;
	}

	struct drm_msm_gem_info info_cmd, info_dst;
	memset(&info_cmd, 0, sizeof(info_cmd));
	memset(&info_dst, 0, sizeof(info_dst));
	info_cmd.handle = bo_cmd.handle;
	info_cmd.info = MSM_INFO_GET_IOVA;
	info_dst.handle = bo_dst.handle;
	info_dst.info = MSM_INFO_GET_IOVA;
	if (call_ioctl(conn, kMsmGemInfo, &info_cmd, sizeof(info_cmd)) != 0 ||
	    call_ioctl(conn, kMsmGemInfo, &info_dst, sizeof(info_dst)) != 0) {
		fprintf(stderr, "sweetgpu-test: GEM_INFO(GET_IOVA) failed\n");
		IOServiceClose(conn);
		return 1;
	}

	mach_vm_address_t cmd_va = 0, dst_va = 0;
	mach_vm_size_t cmd_sz = 0, dst_sz = 0;
	if (IOConnectMapMemory64(conn, bo_cmd.handle, mach_task_self(), &cmd_va, &cmd_sz, kIOMapAnywhere) != KERN_SUCCESS ||
	    IOConnectMapMemory64(conn, bo_dst.handle, mach_task_self(), &dst_va, &dst_sz, kIOMapAnywhere) != KERN_SUCCESS) {
		fprintf(stderr, "sweetgpu-test: IOConnectMapMemory64 failed\n");
		IOServiceClose(conn);
		return 1;
	}
	printf("3. BOs allocated and mapped:\n");
	printf("   cmd BO: handle=%u iova=0x%llx user_va=0x%llx map_size=0x%llx\n",
	    bo_cmd.handle, (unsigned long long)info_cmd.value,
	    (unsigned long long)cmd_va, (unsigned long long)cmd_sz);
	printf("   dst BO: handle=%u iova=0x%llx user_va=0x%llx map_size=0x%llx\n",
	    bo_dst.handle, (unsigned long long)info_dst.value,
	    (unsigned long long)dst_va, (unsigned long long)dst_sz);

	volatile uint32_t *cmd = (volatile uint32_t *)(uintptr_t)cmd_va;
	volatile uint32_t *dst = (volatile uint32_t *)(uintptr_t)dst_va;
	dst[0] = 0xdeadbeefu;
	dst[1] = 0xdeadbeefu;
	__asm__ volatile ("dsb sy" : : : "memory");

	/* Build IB1: CP_NOP + CP_MEM_WRITE 0xC0FFEE02 to dst_iova */
	cmd[0] = pkt7_hdr(CP_NOP, 1);
	cmd[1] = 0x4f503632u;
	cmd[2] = pkt7_hdr(CP_MEM_WRITE, 3);
	cmd[3] = (uint32_t)info_dst.value;
	cmd[4] = (uint32_t)(info_dst.value >> 32);
	cmd[5] = MAGIC_G2;
	__asm__ volatile ("dsb sy" : : : "memory");

	struct drm_msm_gem_submit_bo bos[2];
	memset(bos, 0, sizeof(bos));
	bos[0].handle = bo_cmd.handle;
	bos[0].flags = MSM_SUBMIT_BO_READ;
	bos[0].presumed = info_cmd.value;
	bos[1].handle = bo_dst.handle;
	bos[1].flags = MSM_SUBMIT_BO_WRITE;
	bos[1].presumed = info_dst.value;

	struct drm_msm_gem_submit_cmd scmd;
	memset(&scmd, 0, sizeof(scmd));
	scmd.type = MSM_SUBMIT_CMD_BUF;
	scmd.submit_idx = 0;
	scmd.submit_offset = 0;
	scmd.size = 6 * 4;

	struct drm_msm_gem_submit sub;
	memset(&sub, 0, sizeof(sub));
	sub.queueid = sq.id;
	sub.nr_bos = 2;
	sub.nr_cmds = 1;
	sub.bos = (uint64_t)(uintptr_t)bos;
	sub.cmds = (uint64_t)(uintptr_t)&scmd;

	uint64_t t0 = now_us();
	int rc = call_ioctl(conn, kMsmGemSubmit, &sub, sizeof(sub));
	if (rc != 0) {
		fprintf(stderr, "sweetgpu-test: GEM_SUBMIT failed: 0x%x\n", rc);
		return 1;
	}

	struct drm_msm_wait_fence wf;
	memset(&wf, 0, sizeof(wf));
	wf.fence = sub.fence;
	wf.queueid = sq.id;
	wf.timeout.tv_sec = 2;
	wf.timeout.tv_nsec = 0;
	rc = call_ioctl(conn, kMsmWaitFence, &wf, sizeof(wf));
	uint64_t dt = now_us() - t0;
	__asm__ volatile ("dmb ld" : : : "memory");
	uint32_t got = dst[0];

	printf("4. Single IB submit + WAIT_FENCE:\n");
	printf("   fence=%u wait_rc=0x%x time=%lluus dst[0]=0x%08x (want 0x%08x)\n",
	    sub.fence, rc, (unsigned long long)dt, got, MAGIC_G2);
	if (rc != 0 || got != MAGIC_G2) {
		fprintf(stderr, "SWEETGPU-TEST FAIL: single submit mismatch\n");
		return 1;
	}

	/* Stress / benchmark loop: N submits writing 0xC0FFEE00 + i */
	uint64_t loop_t0 = now_us();
	uint32_t first_fence = 0, last_fence = 0;
	for (int i = 1; i <= loops; i++) {
		uint32_t want = 0xC0FFEE00u + (uint32_t)i;
		cmd[5] = want;
		__asm__ volatile ("dsb sy" : : : "memory");
		sub.fence = 0;
		if (call_ioctl(conn, kMsmGemSubmit, &sub, sizeof(sub)) != 0) {
			fprintf(stderr, "sweetgpu-test: loop %d GEM_SUBMIT failed\n", i);
			return 1;
		}
		if (i == 1) {
			first_fence = sub.fence;
		}
		last_fence = sub.fence;
		wf.fence = sub.fence;
		wf.timeout.tv_sec = 2;
		wf.timeout.tv_nsec = 0;
		if (call_ioctl(conn, kMsmWaitFence, &wf, sizeof(wf)) != 0) {
			fprintf(stderr, "sweetgpu-test: loop %d WAIT_FENCE(%u) timed out\n", i, sub.fence);
			return 1;
		}
		__asm__ volatile ("dmb ld" : : : "memory");
		if (dst[0] != want) {
			fprintf(stderr, "sweetgpu-test: loop %d dst[0]=0x%08x != 0x%08x\n", i, dst[0], want);
			return 1;
		}
	}
	uint64_t loop_us = now_us() - loop_t0;
	printf("5. Stress loop (%d submits):\n", loops);
	printf("   fences=%u..%u total=%lluus avg=%lluus/submit\n",
	    first_fence, last_fence, (unsigned long long)loop_us,
	    (unsigned long long)(loop_us / (unsigned)loops));

	(void)IOConnectUnmapMemory64(conn, bo_cmd.handle, mach_task_self(), cmd_va);
	(void)IOConnectUnmapMemory64(conn, bo_dst.handle, mach_task_self(), dst_va);
	struct drm_gem_close cl;
	memset(&cl, 0, sizeof(cl));
	cl.handle = bo_cmd.handle;
	(void)call_ioctl(conn, kDrmGemClose, &cl, sizeof(cl));
	cl.handle = bo_dst.handle;
	(void)call_ioctl(conn, kDrmGemClose, &cl, sizeof(cl));
	(void)call_ioctl(conn, kMsmSubmitQueueClose, &sq, sizeof(sq));
	IOServiceClose(conn);

	printf("SWEETGPU-TEST PASS: magic=0x%08x loops=%d fences=%u..%u\n",
	    MAGIC_G2, loops, first_fence, last_fence);
	return 0;
}
