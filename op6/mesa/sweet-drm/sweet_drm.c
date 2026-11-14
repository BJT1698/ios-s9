/*
 * Minimal libdrm implementation over SweetGPUUserClient (IOKit user client for
 * the OnePlus 6 Adreno 630 kernel driver in xnu/patches/0108-op6-gpu-userclient.patch).
 *
 * Each msm DRM ioctl is dispatched via IOConnectCallStructMethod with the low byte
 * of the ioctl request number as the selector (0x09 GEM_CLOSE, 0x40..0x4c msm uAPI).
 * Buffer mapping uses IOConnectMapMemory64 with the GEM handle as memoryType.
 *
 * Adapted from HackiOS tools/mesa/redwood-drm/redwood_drm.c (Copyright (c) 2026
 * OOSEX GANG, MIT License) and ios-apollo userland/mesa/sweet-drm/sweet_drm.c
 * (Copyright (c) 2026 Leo Malik / dipa-studio, MIT License).
 */
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <mach/mach.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioccom.h>
#include <unistd.h>

#include "xf86drm.h"

typedef mach_port_t io_object_t;
typedef io_object_t io_service_t;
typedef io_object_t io_connect_t;
typedef struct __CFDictionary *CFMutableDictionaryRef;
typedef const struct __CFDictionary *CFDictionaryRef;
typedef const struct __CFAllocator *CFAllocatorRef;
typedef const struct __CFString *CFStringRef;
typedef const void *CFTypeRef;
typedef unsigned char Boolean;
typedef unsigned long CFTypeID;
typedef long CFIndex;
typedef uint32_t CFStringEncoding;
#define kCFStringEncodingUTF8 0x08000100u

extern CFMutableDictionaryRef IOServiceMatching(const char *name);
extern io_service_t IOServiceGetMatchingService(mach_port_t mainPort, CFDictionaryRef matching);
extern kern_return_t IOServiceOpen(io_service_t service, task_port_t owningTask, uint32_t type, io_connect_t *connect);
extern kern_return_t IOObjectRelease(io_object_t object);
extern CFTypeRef IORegistryEntryCreateCFProperty(io_object_t entry, CFStringRef key, CFAllocatorRef allocator, uint32_t options);
extern CFStringRef CFStringCreateWithCString(CFAllocatorRef alloc, const char *cStr, CFStringEncoding encoding);
extern Boolean CFStringGetCString(CFStringRef theString, char *buffer, CFIndex bufferSize, CFStringEncoding encoding);
extern CFTypeID CFGetTypeID(CFTypeRef cf);
extern CFTypeID CFStringGetTypeID(void);
extern void CFRelease(CFTypeRef cf);
extern kern_return_t IOConnectCallStructMethod(io_connect_t connection, uint32_t selector, const void *inputStruct,
    size_t inputStructCnt, void *outputStruct, size_t *outputStructCnt);
extern kern_return_t IOConnectMapMemory64(io_connect_t connect, uint32_t memoryType, task_port_t intoTask,
    mach_vm_address_t *atAddress, mach_vm_size_t *ofSize, uint32_t options);
extern kern_return_t IOConnectUnmapMemory64(io_connect_t connect, uint32_t memoryType, task_port_t fromTask,
    mach_vm_address_t atAddress);

#define kIOMapAnywhere          0x00000001
#define kIOReturnNoMemory       ((kern_return_t)0xe00002bd)
#define kIOReturnNoResources    ((kern_return_t)0xe00002be)
#define kIOReturnBadArgument    ((kern_return_t)0xe00002c2)
#define kIOReturnUnsupported    ((kern_return_t)0xe00002c7)
#define kIOReturnBusy           ((kern_return_t)0xe00002d5)
#define kIOReturnTimeout        ((kern_return_t)0xe00002d6)
#define kIOReturnNotFound       ((kern_return_t)0xe00002f0)

#define SWEET_GPU_SERVICE       "SweetGPU"
#define SWEET_GPU_MAX_BOS       1024
#define SWEET_SEL_GET_PARAM     0x40u
#define SWEET_SEL_GEM_INFO      0x43u
#define SWEET_SEL_GEM_CPU_PREP  0x44u
#define SWEET_SEL_WAIT_FENCE    0x47u
#define SWEET_GPU_SURFACE       0x7e
#define SWEET_GPU_DESCRIBE      0xffffffffu
#define SWEET_GPU_LOG           0x7f

#define SWEET_PARAM_GPU_ID      0x01u
#define SWEET_PARAM_CHIP_ID     0x03u
#define SWEET_PARAM_VA_START    0x07u
#define SWEET_PARAM_VA_SIZE     0x08u
#define SWEET_INFO_GET_IOVA     1u
#define SWEET_PREP_READ         1u
#define SWEET_PREP_WRITE        2u

static io_connect_t connection;
static int connection_fd = -1;

struct sweet_map_slot {
	uint32_t handle;
	uintptr_t addr;
	uint64_t size;
};

static pthread_mutex_t map_lock = PTHREAD_MUTEX_INITIALIZER;
static struct sweet_map_slot map_slots[SWEET_GPU_MAX_BOS];

static pthread_mutex_t wait_err_lock = PTHREAD_MUTEX_INITIALIZER;
static char last_wait_err[192];

static void
record_wait_error(unsigned selector, kern_return_t kr, int err)
{
	pthread_mutex_lock(&wait_err_lock);
	snprintf(last_wait_err, sizeof(last_wait_err),
	    "sel 0x%x kr=0x%x errno=%d (%s)", selector, (unsigned)kr, err, strerror(err));
	pthread_mutex_unlock(&wait_err_lock);
}

int
drmOpenWithType(const char *name, const char *busid, int type)
{
	io_service_t service;

	(void)busid;
	(void)type;
	if (strcmp(name, "msm") != 0) {
		return -1;
	}
	if (connection_fd >= 0) {
		return dup(connection_fd);
	}
	service = IOServiceGetMatchingService(0, IOServiceMatching(SWEET_GPU_SERVICE));
	if (service == 0) {
		return -1;
	}
	if (IOServiceOpen(service, mach_task_self(), 0, &connection) != 0) {
		IOObjectRelease(service);
		return -1;
	}
	IOObjectRelease(service);
	connection_fd = open("/dev/null", O_RDWR);
	return dup(connection_fd);
}

static int
call(unsigned number, void *data, size_t size)
{
	size_t returned = size;
	kern_return_t result = IOConnectCallStructMethod(connection, number, data, size, data, &returned);
	int err = 0;

	switch (result) {
	case 0:
		return 0;
	case kIOReturnNoMemory:         /* GEM_NEW: carve-out or GPU page-table pool exhausted */
	case kIOReturnNoResources:      /* GEM_NEW: this client's BO table (1024 handles) is full */
		err = ENOMEM;
		break;
	case kIOReturnBadArgument:
		err = EINVAL;
		break;
	case kIOReturnTimeout:
		err = ETIMEDOUT;
		break;
	case kIOReturnBusy:
		err = EBUSY;
		break;
	case kIOReturnNotFound:
		err = ENOENT;
		break;
	case kIOReturnUnsupported:
		err = ENOSYS;
		break;
	default:
		err = EIO;
		break;
	}
	if (number == SWEET_SEL_WAIT_FENCE || number == SWEET_SEL_GEM_CPU_PREP) {
		record_wait_error(number, result, err);
	}
	return err;
}

int
drmIoctl(int fd, unsigned long request, void *arg)
{
	int error = call((unsigned)(request & 0xff), arg, IOCPARM_LEN(request));

	(void)fd;
	if (error != 0) {
		errno = error;
		return -1;
	}
	return 0;
}

/* Like libdrm: -errno returned and errno set (Mesa logs strerror(errno) after a failure). */
static int
command(unsigned long index, void *data, unsigned long size)
{
	int error = call((unsigned)(DRM_COMMAND_BASE + index), data, size);

	if (error != 0) {
		errno = error;
	}
	return -error;
}

int
drmCommandRead(int fd, unsigned long index, void *data, unsigned long size)
{
	(void)fd;
	return command(index, data, size);
}

int
drmCommandWrite(int fd, unsigned long index, const void *data, unsigned long size)
{
	(void)fd;
	return command(index, (void *)(uintptr_t)data, size);
}

int
drmCommandWriteRead(int fd, unsigned long index, void *data, unsigned long size)
{
	(void)fd;
	return command(index, data, size);
}

/*
 * Report msm DRM version 1.7 (softpin IOVAs assigned by kernel, no syncobjs).
 */
drmVersionPtr
drmGetVersion(int fd)
{
	drmVersionPtr version = calloc(1, sizeof(*version));

	(void)fd;
	if (version != NULL) {
		version->version_major = 1;
		version->version_minor = 7;
		version->name = strdup("msm");
		version->name_len = 3;
		version->date = strdup("");
		version->desc = strdup("Adreno 630 on enchilada (SweetGPU)");
		version->desc_len = (int)strlen(version->desc);
	}
	return version;
}

void
drmFreeVersion(drmVersionPtr version)
{
	if (version != NULL) {
		free(version->name);
		free(version->date);
		free(version->desc);
		free(version);
	}
}

int
drmGetCap(int fd, uint64_t capability, uint64_t *value)
{
	(void)fd;
	(void)capability;
	*value = 0;
	return 0;
}

static void
record_map_slot(uint32_t handle, uintptr_t addr, uint64_t size)
{
	pthread_mutex_lock(&map_lock);
	int free_idx = -1;
	for (int i = 0; i < SWEET_GPU_MAX_BOS; i++) {
		if (map_slots[i].handle == handle) {
			map_slots[i].addr = addr;
			map_slots[i].size = size;
			pthread_mutex_unlock(&map_lock);
			return;
		}
		if (free_idx < 0 && map_slots[i].handle == 0) {
			free_idx = i;
		}
	}
	if (free_idx >= 0) {
		map_slots[free_idx].handle = handle;
		map_slots[free_idx].addr = addr;
		map_slots[free_idx].size = size;
	}
	pthread_mutex_unlock(&map_lock);
}

static void
clear_map_slot(uint32_t handle, uintptr_t addr)
{
	pthread_mutex_lock(&map_lock);
	for (int i = 0; i < SWEET_GPU_MAX_BOS; i++) {
		if (map_slots[i].handle == handle && map_slots[i].addr == addr) {
			map_slots[i].handle = 0;
			map_slots[i].addr = 0;
			map_slots[i].size = 0;
			break;
		}
	}
	pthread_mutex_unlock(&map_lock);
}

static void
flush_range_civac(uintptr_t start, uint64_t len)
{
	if (len == 0) {
		return;
	}
#ifdef __aarch64__
	uintptr_t end = start + (uintptr_t)len;
	uintptr_t p = start & ~(uintptr_t)63u;
	__asm__ volatile("dsb sy" ::: "memory");
	for (; p < end; p += 64) {
		__asm__ volatile("dc civac, %0" :: "r"(p) : "memory");
	}
	__asm__ volatile("dsb sy" ::: "memory");
#else
	(void)start;
#endif
}

void *
sweet_drm_map(int fd, uint32_t handle, uint64_t size)
{
	mach_vm_address_t address = 0;
	mach_vm_size_t mapped = 0;

	(void)fd;
	if (IOConnectMapMemory64(connection, handle, mach_task_self(), &address, &mapped, kIOMapAnywhere) != 0) {
		return (void *)-1;
	}
	record_map_slot(handle, (uintptr_t)address, mapped ? (uint64_t)mapped : size);
	return (void *)(uintptr_t)address;
}

void
sweet_drm_unmap(int fd, uint32_t handle, void *address)
{
	(void)fd;
	clear_map_slot(handle, (uintptr_t)address);
	IOConnectUnmapMemory64(connection, handle, mach_task_self(), (mach_vm_address_t)(uintptr_t)address);
}

int
sweet_drm_info(int fd, struct sweet_drm_gpu_info *out)
{
	io_service_t service;
	CFStringRef key;
	CFTypeRef val;
	char state[32] = "";

	(void)fd;
	if (out == NULL) {
		errno = EINVAL;
		return -1;
	}
	memset(out, 0, sizeof(*out));
	service = IOServiceGetMatchingService(0, IOServiceMatching(SWEET_GPU_SERVICE));
	if (service == 0) {
		errno = ENOENT;
		return -1;
	}
	key = CFStringCreateWithCString(NULL, "SweetGPUState", kCFStringEncodingUTF8);
	val = key ? IORegistryEntryCreateCFProperty(service, key, NULL, 0) : NULL;
	if (key) {
		CFRelease(key);
	}
	IOObjectRelease(service);
	if (val != NULL) {
		if (CFGetTypeID(val) == CFStringGetTypeID()) {
			CFStringGetCString((CFStringRef)val, state, sizeof(state), kCFStringEncodingUTF8);
		}
		CFRelease(val);
	}
	out->abi_version = 1;
	out->abi_minor = 0;
	if (strcmp(state, "on") == 0) {
		out->flags = 1;
	} else if (strcmp(state, "failed") == 0) {
		out->flags = 2;
	}
	if (connection != 0) {
		struct {
			uint32_t pipe, param;
			uint64_t value;
			uint32_t len, pad;
		} p;
		memset(&p, 0, sizeof(p));
		p.param = SWEET_PARAM_GPU_ID;
		if (call(SWEET_SEL_GET_PARAM, &p, sizeof(p)) == 0) {
			out->gpu_id = (uint32_t)p.value;
		}
		memset(&p, 0, sizeof(p));
		p.param = SWEET_PARAM_CHIP_ID;
		if (call(SWEET_SEL_GET_PARAM, &p, sizeof(p)) == 0) {
			out->chip_id = (uint32_t)p.value;
		}
		memset(&p, 0, sizeof(p));
		p.param = SWEET_PARAM_VA_START;
		if (call(SWEET_SEL_GET_PARAM, &p, sizeof(p)) == 0) {
			out->va_start = p.value;
		}
		memset(&p, 0, sizeof(p));
		p.param = SWEET_PARAM_VA_SIZE;
		if (call(SWEET_SEL_GET_PARAM, &p, sizeof(p)) == 0) {
			out->va_size = p.value;
		}
	}
	return 0;
}

int
sweet_drm_surface_bo(int fd, struct sweet_drm_sbo *io)
{
	struct sweet_drm_surface req;
	struct {
		uint32_t handle, info;
		uint64_t value;
		uint32_t len, pad;
	} gem_info;
	int err;

	(void)fd;
	if (io == NULL) {
		return -EINVAL;
	}
	if (connection == 0) {
		return -ENOTCONN;
	}
	memset(&req, 0, sizeof(req));
	req.surface = io->surface_id;
	req.handle = 0; /* import (not describe-only) */
	err = call(SWEET_GPU_SURFACE, &req, sizeof(req));
	if (err != 0) {
		return -err;
	}
	io->handle = req.handle;
	io->size = req.size;
	io->offset = (uint32_t)req.offset;
	io->bytes_per_row = req.row_bytes;
	io->width = req.width;
	io->height = req.height;
	io->pixel_format = req.pixel_format;
	io->planes = req.planes;

	memset(&gem_info, 0, sizeof(gem_info));
	gem_info.handle = req.handle;
	gem_info.info = SWEET_INFO_GET_IOVA;
	if (call(SWEET_SEL_GEM_INFO, &gem_info, sizeof(gem_info)) == 0) {
		io->iova = gem_info.value;
	} else {
		io->iova = 0;
	}
	return 0;
}

int
sweet_drm_bo_sync(int fd, uint32_t handle, uint64_t offset, uint64_t size)
{
	struct {
		uint32_t handle, op;
		int64_t tv_sec, tv_nsec;
	} prep;
	int err;

	(void)fd;
	if (handle == 0) {
		return 0;
	}
	if (connection == 0) {
		return -ENOTCONN;
	}
	memset(&prep, 0, sizeof(prep));
	prep.handle = handle;
	prep.op = SWEET_PREP_READ | SWEET_PREP_WRITE;
	prep.tv_sec = 2; /* 0108 treats 1..25 as relative seconds */
	prep.tv_nsec = 0;
	err = call(SWEET_SEL_GEM_CPU_PREP, &prep, sizeof(prep));
	if (err != 0) {
		return -err;
	}

	uintptr_t addr = 0;
	uint64_t mapped_size = 0;
	pthread_mutex_lock(&map_lock);
	for (int i = 0; i < SWEET_GPU_MAX_BOS; i++) {
		if (map_slots[i].handle == handle) {
			addr = map_slots[i].addr;
			mapped_size = map_slots[i].size;
			break;
		}
	}
	pthread_mutex_unlock(&map_lock);

	if (addr != 0 && offset < mapped_size) {
		uint64_t avail = mapped_size - offset;
		uint64_t len = (size == 0 || size > avail) ? avail : size;
		flush_range_civac(addr + (uintptr_t)offset, len);
	}
	return 0;
}

void
sweet_drm_take_wait_error(char *buf, size_t size)
{
	if (buf == NULL || size == 0) {
		return;
	}
	pthread_mutex_lock(&wait_err_lock);
	snprintf(buf, size, "%s", last_wait_err);
	last_wait_err[0] = '\0';
	pthread_mutex_unlock(&wait_err_lock);
}

int
sweet_drm_surface(int fd, struct sweet_drm_surface *surface)
{
	int error;

	(void)fd;
	surface->handle = SWEET_GPU_DESCRIBE;
	error = call(SWEET_GPU_SURFACE, surface, sizeof(*surface));
	if (error != 0) {
		errno = error;
		return -1;
	}
	return 0;
}

void
sweet_drm_log(const char *format, ...)
{
	static int unsupported;
	char line[512];
	va_list ap;
	int n;

	if (connection == 0 || unsupported) {
		return;
	}
	va_start(ap, format);
	n = vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	if (n <= 0) {
		return;
	}
	if ((size_t)n >= sizeof(line)) {
		n = (int)sizeof(line) - 1;
	}
	/* (a kernel without 0111 answers kIOReturnUnsupported: then never again) */
	if (IOConnectCallStructMethod(connection, SWEET_GPU_LOG, line, (size_t)n, NULL, NULL) == kIOReturnUnsupported) {
		unsupported = 1;
	}
}

int
drmPrimeFDToHandle(int fd, int surface, uint32_t *handle)
{
	struct sweet_drm_surface request = { .surface = (uint32_t)surface };
	int error = call(SWEET_GPU_SURFACE, &request, sizeof(request));

	(void)fd;
	if (error != 0) {
		errno = error;
		return -1;
	}
	*handle = request.handle;
	return 0;
}

static int
none(void)
{
	errno = ENOSYS;
	return -1;
}

#define NONE(name, ...) int name(__VA_ARGS__) { return none(); }
NONE(drmPrimeHandleToFD, __attribute__((unused)) int fd, __attribute__((unused)) uint32_t handle,
    __attribute__((unused)) uint32_t flags, __attribute__((unused)) int *prime_fd)
NONE(drmSyncobjCreate, __attribute__((unused)) int fd, __attribute__((unused)) uint32_t flags,
    __attribute__((unused)) uint32_t *handle)
NONE(drmSyncobjDestroy, __attribute__((unused)) int fd, __attribute__((unused)) uint32_t handle)
NONE(drmSyncobjReset, __attribute__((unused)) int fd, __attribute__((unused)) const uint32_t *handles,
    __attribute__((unused)) uint32_t count)
NONE(drmSyncobjFDToHandle, __attribute__((unused)) int fd, __attribute__((unused)) int obj_fd,
    __attribute__((unused)) uint32_t *handle)
NONE(drmSyncobjHandleToFD, __attribute__((unused)) int fd, __attribute__((unused)) uint32_t handle,
    __attribute__((unused)) int *obj_fd)
NONE(drmSyncobjImportSyncFile, __attribute__((unused)) int fd, __attribute__((unused)) uint32_t handle,
    __attribute__((unused)) int sync_file_fd)
NONE(drmSyncobjExportSyncFile, __attribute__((unused)) int fd, __attribute__((unused)) uint32_t handle,
    __attribute__((unused)) int *sync_file_fd)
