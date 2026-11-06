#pragma once
#include <stdint.h>

/* Copies the root string property into buf; returns its length, 0 if absent. */
int fdt_root_str(uintptr_t fdt, const char *prop, char *buf, int n);
/* /chosen property of 4 or 8 bytes; 1 if found. */
int fdt_chosen_u64(uintptr_t fdt, const char *prop, uint64_t *out);
/* Copies the /chosen/bootargs word that follows key ("lcd_id=0x"); 0 if absent. */
int fdt_bootarg(uintptr_t fdt, const char *key, char *buf, int n);
