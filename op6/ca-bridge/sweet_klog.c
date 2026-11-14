/*
 * sweet_klog.h: our log lines through the SweetGPU user client into the kernel's log (selector 0x7f).
 * Built into both libsweet-ca.dylib and libsweet-gl.dylib (each keeps its own connection).
 */
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "sweet_klog.h"

/* IOKit/IOKitLib.h is not in the iPhoneOS SDK */
typedef mach_port_t io_object_t;
typedef io_object_t io_service_t;
typedef io_object_t io_connect_t;
typedef struct __CFDictionary *CFMutableDictionaryRef;
typedef const struct __CFDictionary *CFDictionaryRef;
extern CFMutableDictionaryRef IOServiceMatching(const char *name);
extern io_service_t IOServiceGetMatchingService(mach_port_t mainPort, CFDictionaryRef matching);
extern kern_return_t IOServiceOpen(io_service_t service, task_port_t owningTask, uint32_t type, io_connect_t *connect);
extern kern_return_t IOServiceClose(io_connect_t connect);
extern kern_return_t IOObjectRelease(io_object_t object);
extern kern_return_t IOConnectCallStructMethod(mach_port_t connection, uint32_t selector, const void *inputStruct,
    size_t inputStructCnt, void *outputStruct, size_t *outputStructCnt);

#define KSWEET_GPU_LOG          0x7fu           /* selector (xnu patch 0111) */
#define KSWEET_GPU_LOG_CLIENT   0x534c4f47u     /* IOServiceOpen type 'SLOG': log-only client */
#define KLOG_MAX                511u            /* the kernel takes 512 bytes */

static io_connect_t klog_connection;            /* 0: not open (yet) */
static uint64_t klog_next_try;                  /* mach_absolute_time() of the next attempt to open */
static int klog_broken;                         /* the kernel has no selector 0x7f (an older kernel) */

static io_connect_t
klog_open(void)
{
	io_connect_t connection = __atomic_load_n(&klog_connection, __ATOMIC_ACQUIRE);
	static mach_timebase_info_data_t base;
	uint64_t now = mach_absolute_time(), next = __atomic_load_n(&klog_next_try, __ATOMIC_RELAXED);
	io_service_t gpu;

	if (connection != 0 || klog_broken || now < next) {
		return connection;
	}
	if (base.denom == 0) {
		mach_timebase_info(&base);
	}
	/* one thread tries at a time; the others carry on without the kernel log */
	if (!__atomic_compare_exchange_n(&klog_next_try, &next, now + 1000000000ull * base.denom / base.numer, 0,
	    __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
		return 0;
	}
	if ((gpu = IOServiceGetMatchingService(0, IOServiceMatching("SweetGPU"))) == 0) {
		return 0;
	}
	if (IOServiceOpen(gpu, mach_task_self(), KSWEET_GPU_LOG_CLIENT, &connection) != KERN_SUCCESS) {
		connection = 0;
	}
	IOObjectRelease(gpu);
	if (connection != 0) {
		io_connect_t none = 0;

		if (!__atomic_compare_exchange_n(&klog_connection, &none, connection, 0, __ATOMIC_RELEASE, __ATOMIC_ACQUIRE)) {
			IOServiceClose(connection);
			connection = none;
		}
	}
	return connection;
}

void
sweet_klog(const char *text, size_t length)
{
	while (length > 0) {
		const char *end = memchr(text, '\n', length);
		size_t line = end != NULL ? (size_t)(end - text) : length;

		if (line > 0) {
			io_connect_t connection = klog_open();

			if (connection == 0) {
				return;
			}
			kern_return_t kr = IOConnectCallStructMethod(connection, KSWEET_GPU_LOG, text, line > KLOG_MAX ? KLOG_MAX : line,
			    NULL, NULL);
			if (kr != KERN_SUCCESS) {
				char why[96];
				int n = snprintf(why, sizeof(why), "sweet-klog: selector 0x7f failed (%#x): no kernel log\n", kr);

				write(2, why, n > 0 && n < (int)sizeof(why) ? (size_t)n : 0);
				klog_broken = 1;
				return;
			}
		}
		if (end == NULL) {
			break;
		}
		length -= line + 1;
		text = end + 1;
	}
}

void
sweet_klogf(const char *format, ...)
{
	char line[KLOG_MAX + 1];
	va_list ap;
	int n;

	va_start(ap, format);
	n = vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	if (n > 0) {
		sweet_klog(line, n > (int)KLOG_MAX ? KLOG_MAX : (size_t)n);
	}
}
