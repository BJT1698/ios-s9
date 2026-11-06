/*
 * Just enough flattened-device-tree parsing for the shim: a root property
 * and words from /chosen/bootargs. Token walk from ios-sweet shim/fdt.c.
 * Big-endian, 4-byte aligned tokens; strings are read a byte at a time,
 * which is fine on Device memory.
 */
#include "fdt.h"
#include "hw.h"

#define FDT_MAGIC	0xd00dfeed
#define FDT_BEGIN_NODE	1
#define FDT_END_NODE	2
#define FDT_PROP	3
#define FDT_NOP		4

static uint32_t be32(uintptr_t a)
{
	return __builtin_bswap32(rd32(a));
}

static int streq(uintptr_t a, const char *b)
{
	while (rd8(a) && rd8(a) == (uint8_t)*b)
		a++, b++;
	return rd8(a) == (uint8_t)*b;
}

static uintptr_t align4(uintptr_t a)
{
	return (a + 3) & ~3UL;
}

static uintptr_t skip_str(uintptr_t p)
{
	while (rd8(p))
		p++;
	return p + 1;
}

/* node 0 = the root itself, otherwise a child of the root ("chosen"). */
static int find_prop(uintptr_t fdt, const char *node, const char *prop,
		     uintptr_t *val, uint32_t *len)
{
	if (!fdt || (fdt & 3) || be32(fdt) != FDT_MAGIC)
		return 0;
	uintptr_t p = fdt + be32(fdt + 8);		/* off_dt_struct */
	uintptr_t strings = fdt + be32(fdt + 12);	/* off_dt_strings */
	int depth = 0, in_node = 0;

	for (;;) {
		uint32_t tok = be32(p);
		p += 4;
		switch (tok) {
		case FDT_BEGIN_NODE:
			depth++;
			in_node = node ? depth == 2 && streq(p, node) : depth == 1;
			p = align4(skip_str(p));
			break;
		case FDT_END_NODE:
			if (in_node)
				return 0;
			depth--;
			break;
		case FDT_PROP: {
			uint32_t l = be32(p), nameoff = be32(p + 4);
			uintptr_t v = p + 8;
			if (in_node && streq(strings + nameoff, prop)) {
				*val = v;
				*len = l;
				return 1;
			}
			p = align4(v + l);
			break;
		}
		case FDT_NOP:
			break;
		default:	/* FDT_END or garbage */
			return 0;
		}
	}
}

static int copy_word(uintptr_t v, uintptr_t end, char *buf, int n)
{
	int i = 0;

	while (v < end && i < n - 1) {
		uint8_t c = rd8(v++);
		if (c == 0 || c == ' ')
			break;
		buf[i++] = c;
	}
	buf[i] = 0;
	return i;
}

int fdt_root_str(uintptr_t fdt, const char *prop, char *buf, int n)
{
	uintptr_t v;
	uint32_t len;

	buf[0] = 0;
	if (!find_prop(fdt, 0, prop, &v, &len))
		return 0;
	int i = 0;
	while (i < (int)len && i < n - 1 && rd8(v + i)) {
		buf[i] = rd8(v + i);
		i++;
	}
	buf[i] = 0;
	return i;
}

int fdt_bootarg(uintptr_t fdt, const char *key, char *buf, int n)
{
	uintptr_t v;
	uint32_t len;

	buf[0] = 0;
	if (!find_prop(fdt, "chosen", "bootargs", &v, &len))
		return 0;
	for (uintptr_t s = v; s < v + len; s++) {
		int k = 0;
		while (key[k] && s + k < v + len && rd8(s + k) == (uint8_t)key[k])
			k++;
		if (!key[k])
			return copy_word(s + k, v + len, buf, n);
	}
	return 0;
}

int fdt_chosen_u64(uintptr_t fdt, const char *prop, uint64_t *out)
{
	uintptr_t v;
	uint32_t len;

	if (!find_prop(fdt, "chosen", prop, &v, &len))
		return 0;
	if (len == 4)
		*out = be32(v);
	else if (len == 8)
		*out = (uint64_t)be32(v) << 32 | be32(v + 4);
	else
		return 0;
	return 1;
}
