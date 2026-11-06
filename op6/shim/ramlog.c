/*
 * Flight recorder: text in the ramoops console zone of the stock DTB
 * (ramoops@a1300000: one 0x40000 dump record, then console, ftrace and
 * pmsg zones of 0x40000 each, no ECC), laid out like the
 * persistent_ram_buffer of fs/pstore/ram_core.c. After a reset that keeps
 * RAM, a kernel with pstore (TWRP's) shows it in /sys/fs/pstore. Runs
 * append to a log that is still ours, so a boot loop keeps its history.
 */
#include "hw.h"
#include "fb.h"
#include "ramlog.h"

#define ZONE		0xac480000UL	/* OP6 ramoops@ac300000: 0x180000 of dump records, then console */
#define ZONE_SIZE	0x40000UL
#define SIG		(ZONE + 0)
#define START		(ZONE + 4)
#define SIZE		(ZONE + 8)
#define DATA		(ZONE + 12)
#define CAP		(ZONE_SIZE - 12)
#define PERSISTENT_RAM_SIG	0x43474244U	/* "DBGC"; console zone uses sig 0 ^ this */

static uint32_t len;

void ramlog_open(void)
{
	uint32_t start = rd32(START), size = rd32(SIZE);

	/* Extend an earlier log only if it is valid and never wrapped. */
	if (rd32(SIG) == PERSISTENT_RAM_SIG && start == size && size < CAP - 0x4000)
		len = size;
	else
		len = 0;
	wr32(SIG, PERSISTENT_RAM_SIG);
	wr32(START, len);
	wr32(SIZE, len);
}

void ramlog_str(const char *s)
{
	while (*s && len < CAP)
		*(volatile uint8_t *)(DATA + len++) = *s++;
	wr32(SIZE, len);
	wr32(START, len);
}

void ramlog_hex(uint64_t v, int digits)
{
	char b[17];

	fmt_hex(b, v, digits > 16 ? 16 : digits);
	ramlog_str(b);
}

void ramlog_dec(uint32_t v)
{
	char b[11];

	fmt_dec(b, v);
	ramlog_str(b);
}
