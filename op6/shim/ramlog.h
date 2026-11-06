#pragma once
#include <stdint.h>

void ramlog_open(void);
void ramlog_str(const char *s);
void ramlog_hex(uint64_t v, int digits);
void ramlog_dec(uint32_t v);
