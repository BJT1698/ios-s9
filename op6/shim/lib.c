/*
 * The compiler may still emit calls to these for initialisers or struct
 * copies, even with -ffreestanding. Byte-wise through volatile: slow, but
 * safe on Device memory and never turned back into a call to itself.
 */
#include <stddef.h>
#include <stdint.h>

void *memset(void *d, int c, size_t n)
{
	volatile unsigned char *p = d;

	while (n--)
		*p++ = (unsigned char)c;
	return d;
}

void *memcpy(void *d, const void *s, size_t n)
{
	volatile unsigned char *p = d;
	const volatile unsigned char *q = s;

	while (n--)
		*p++ = *q++;
	return d;
}

/* Stages that start no secondary CPUs link this one. */
__attribute__((weak)) void secondary_main(uint64_t idx)
{
	(void)idx;
	for (;;)
		__asm__ volatile("wfe");
}
