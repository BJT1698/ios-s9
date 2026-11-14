/*
 * Minimal libdrm xf86drm.h stand-in for Mesa freedreno on iOS (Darwin/XNU),
 * backed by SweetGPUUserClient (xnu/patches/0108-op6-gpu-userclient.patch).
 *
 * Adapted from HackiOS tools/mesa/redwood-drm (Copyright (c) 2026 OOSEX GANG, MIT License)
 * and ios-apollo userland/mesa/sweet-drm (Copyright (c) 2026 Leo Malik / dipa-studio, MIT).
 */
#ifndef SWEET_XF86DRM_H
#define SWEET_XF86DRM_H

#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "drm-uapi/drm.h"

#define DRM_NODE_PRIMARY        0
#define DRM_NODE_RENDER         2
#define DRM_CLOEXEC             O_CLOEXEC
#define DRM_RDWR                O_RDWR

typedef struct _drmVersion {
	int version_major, version_minor, version_patchlevel;
	int name_len;
	char *name;
	int date_len;
	char *date;
	int desc_len;
	char *desc;
} drmVersion, *drmVersionPtr;

int drmOpenWithType(const char *name, const char *busid, int type);
int drmIoctl(int fd, unsigned long request, void *arg);
int drmCommandRead(int fd, unsigned long index, void *data, unsigned long size);
int drmCommandWrite(int fd, unsigned long index, const void *data, unsigned long size);
int drmCommandWriteRead(int fd, unsigned long index, void *data, unsigned long size);
drmVersionPtr drmGetVersion(int fd);
void drmFreeVersion(drmVersionPtr version);
int drmGetCap(int fd, uint64_t capability, uint64_t *value);

int drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd);
int drmPrimeFDToHandle(int fd, int surface, uint32_t *handle);
int drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle);
int drmSyncobjDestroy(int fd, uint32_t handle);
int drmSyncobjReset(int fd, const uint32_t *handles, uint32_t count);
int drmSyncobjFDToHandle(int fd, int obj_fd, uint32_t *handle);
int drmSyncobjHandleToFD(int fd, uint32_t handle, int *obj_fd);
int drmSyncobjImportSyncFile(int fd, uint32_t handle, int sync_file_fd);
int drmSyncobjExportSyncFile(int fd, uint32_t handle, int *sync_file_fd);

/* BO memory mapping via SweetGPUUserClient::clientMemoryForType */
void *sweet_drm_map(int fd, uint32_t handle, uint64_t size);
void sweet_drm_unmap(int fd, uint32_t handle, void *address);

/*
 * IOSurface import descriptor (selector 0x7e): the kernel's struct sweet_surface_request
 * (xnu/patches/0109, 0112), 48 bytes, offset 64-bit. Up to G4d this was 40 bytes with a 32-bit
 * offset and the kernel's answer was read shifted by 4 bytes from row_bytes on (pixel_format
 * came back as the height), so Mesa refused every surface and never imported one; 0112 kernels
 * also answer the old 40-byte layout in kind.
 */
struct sweet_drm_surface {
	uint32_t surface;
	uint32_t handle;                /* 0xffffffff: describe only; otherwise import, the BO handle out */
	uint64_t size;
	uint64_t offset;
	uint32_t row_bytes;
	uint32_t width, height, pixel_format, planes;
	uint32_t pad;
};
#ifdef __cplusplus
static_assert(sizeof(struct sweet_drm_surface) == 48, "selector 0x7e takes 48 bytes");
#else
_Static_assert(sizeof(struct sweet_drm_surface) == 48, "selector 0x7e takes 48 bytes");
#endif
int sweet_drm_surface(int fd, struct sweet_drm_surface *surface);

#endif
