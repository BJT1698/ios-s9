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
extern CFMutableDictionaryRef IOServiceMatching(const char *name);
extern io_service_t IOServiceGetMatchingService(mach_port_t mainPort, CFDictionaryRef matching);
extern kern_return_t IOServiceOpen(io_service_t service, task_port_t owningTask, uint32_t type, io_connect_t *connect);
extern kern_return_t IOObjectRelease(io_object_t object);
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
#define SWEET_GPU_SURFACE       0x7e
#define SWEET_GPU_DESCRIBE      0xffffffffu
#define SWEET_GPU_LOG           0x7f

static io_connect_t connection;
static int connection_fd = -1;

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

	switch (result) {
	case 0:
		return 0;
	case kIOReturnNoMemory:         /* GEM_NEW: carve-out or GPU page-table pool exhausted */
	case kIOReturnNoResources:      /* GEM_NEW: this client's BO table (1024 handles) is full */
		return ENOMEM;
	case kIOReturnBadArgument:
		return EINVAL;
	case kIOReturnTimeout:
		return ETIMEDOUT;
	case kIOReturnBusy:
		return EBUSY;
	case kIOReturnNotFound:
		return ENOENT;
	case kIOReturnUnsupported:
		return ENOSYS;
	default:
		return EIO;
	}
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

void *
sweet_drm_map(int fd, uint32_t handle, uint64_t size)
{
	mach_vm_address_t address = 0;
	mach_vm_size_t mapped = 0;

	(void)fd;
	(void)size;
	if (IOConnectMapMemory64(connection, handle, mach_task_self(), &address, &mapped, kIOMapAnywhere) != 0) {
		return (void *)-1;
	}
	return (void *)(uintptr_t)address;
}

void
sweet_drm_unmap(int fd, uint32_t handle, void *address)
{
	(void)fd;
	IOConnectUnmapMemory64(connection, handle, mach_task_self(), (mach_vm_address_t)(uintptr_t)address);
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
