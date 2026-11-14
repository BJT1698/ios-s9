/*
 * One line of our log into the kernel's: selector 0x7f (kSweetGpuLog) of the SweetGPU user client
 * (xnu patch 0111), printed by the kernel as "sweet-gl[<process>]: <text>", so it is in dmesg and in
 * the persistent log even when the process's stderr goes nowhere or the process dies right after.
 */
#ifndef SWEET_KLOG_H
#define SWEET_KLOG_H

#include <stddef.h>

/*
 * TEXT (LENGTH bytes, NUL not needed), a line per '\n', each cut at 511 bytes. The SweetGPU
 * service is opened (log-only, it does not wait for the GPU) on first use and, while it is not
 * there yet, tried again at most once a second; lines in between are not sent anywhere but stderr.
 */
__attribute__((visibility("hidden"))) void sweet_klog(const char *text, size_t length);
__attribute__((visibility("hidden"))) void sweet_klogf(const char *format, ...) __attribute__((format(printf, 1, 2)));

#endif
