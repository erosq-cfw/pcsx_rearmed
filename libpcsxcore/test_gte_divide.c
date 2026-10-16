/* EROSQ host test: DIVIDE (gte_divider.c, multu-form rounding) == the
 * original ((u64)a*b + 0x8000) >> 16 form, for every input the GTE can pass
 * (callers guarantee n < 2*d, d != 0):
 * exhaustive for d < 4096, all d with n in {0, 1, d-1, d, 2d-1} and 200M
 * random (n < 2d) pairs above that, plus a mutation check.
 *
 *   cc -O2 -Iinclude -o /tmp/test_gte_div libpcsxcore/test_gte_divide.c \
 *     libpcsxcore/gte_divider.c && /tmp/test_gte_div
 */
#include <stdio.h>
#include <stdint.h>
#include "psxcommon.h"
#include "gte_divider.h"

/* the original (pre-EROSQ) formulation, verbatim */
static u32 DIVIDE_ref(u16 numerator, u16 denominator)
{
	int shift = __builtin_clz(denominator) - 16;
	int r1 = (denominator << shift) & 0x7fff;
	int r2 = gte_divider_tab[(r1 + 0x40) >> 7] + 0x101;
	int r3 = ((0x80 - r2 * (r1 + 0x8000)) >> 8) & 0x1ffff;
	u32 reciprocal = (r2 * r3 + 0x80) >> 8;
	return ((u64)reciprocal * (numerator << shift) + 0x8000) >> 16;
}

static uint32_t s = 0x20261016;
static uint32_t rnd(void) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }

static int check(u16 n, u16 d, unsigned long long *bad)
{
	u32 a = DIVIDE_ref(n, d), b = DIVIDE(n, d);
	if (a != b) { if ((*bad)++ < 5) printf("FAIL n=%u d=%u ref=%u inl=%u\n", n, d, a, b); return 1; }
	return 0;
}

int main(void)
{
	unsigned long long bad = 0, n_cases = 0;
	for (u32 d = 1; d < 4096; d++)
		for (u32 n = 0; n < 2 * d && n < 0x10000; n++, n_cases++) check(n, d, &bad);
	for (u32 d = 1; d < 0x10000; d++) {
		u32 ns[5] = { 0, 1, d - 1, d, 2 * d - 1 };
		for (int k = 0; k < 5; k++) if (ns[k] < 0x10000) { check(ns[k], d, &bad); n_cases++; }
	}
	for (long i = 0; i < 200000000L; i++) {
		u32 d = 4096 + rnd() % (0x10000 - 4096);
		u32 lim = 2 * d < 0x10000 ? 2 * d : 0x10000;
		u32 n = rnd() % lim;
		check(n, d, &bad); n_cases++;
	}
	/* mutation: dropping the rounding bit must be detected */
	unsigned long long mut = 0;
	for (int i = 0; i < 100000; i++) {
		u32 d = 1 + rnd() % 0xffff, lim = 2 * d < 0x10000 ? 2 * d : 0x10000, n = rnd() % lim;
		int shift = __builtin_clz(d) - 16;
		(void)shift;
		/* emulate the broken form: truncate instead of round */
		unsigned long long p;
		{ int sh = __builtin_clz(d) - 16; int r1 = (d << sh) & 0x7fff;
		  int r2 = gte_divider_tab[(r1 + 0x40) >> 7] + 0x101;
		  int r3 = ((0x80 - r2 * (r1 + 0x8000)) >> 8) & 0x1ffff;
		  u32 rc = (r2 * r3 + 0x80) >> 8; p = (unsigned long long)rc * (u32)(n << sh); }
		if ((u32)(p >> 16) != DIVIDE_ref(n, d)) mut++;
	}
	printf("DIVIDE (multu form): %llu cases, %llu mismatches; truncation mutant differs in %llu/100000\n",
	       n_cases, bad, mut);
	return (bad || mut == 0) ? 1 : 0;
}
