#pragma once
#include <stdint.h>

/* Pieces every shim stage after M2 shares: SMC, time, watchdog, reboot, panic. */
uint64_t smc(uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3);
uint64_t now(void);
uint64_t ticks_per_s(void);
void delay_ms(unsigned ms);
uint32_t ticks_to_us(uint64_t ticks);
void wdog_arm(void);
void wdog_pet(void);
void __attribute__((noreturn)) reboot(void);
/* Blue screen with the exception state; stays 60 s (watchdog fed), then reboots. */
void __attribute__((noreturn)) panic_screen(const char *stage, uint64_t slot, uint64_t *regs);
/* "TEMPO" line counting to secs at row, feeding the watchdog, then reboot. */
void __attribute__((noreturn)) countdown_reboot(int row, unsigned secs);
