#pragma once
#include <stdint.h>

/* Rows of "LABEL   value" text on the framebuffer, wrapped at the right edge. */
void con_clear(uint32_t bg);
void con_title(const char *s);
void con_skip(void);
void con_str(const char *label, const char *s);
void con_hex(const char *label, uint64_t v);
void con_hex2(const char *label, const char *a, uint32_t va, const char *b, uint32_t vb);
int con_row(void);
void con_set_row(int y);

/* string building without libc */
char *put(char *d, const char *s);
char *put_hex(char *d, uint64_t v, int digits);
char *put_dec(char *d, uint32_t v);
