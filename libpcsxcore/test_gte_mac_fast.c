/* Host assert-test for the erosq guarded flag-free GTE MAC fast path.
 * Verifies: whenever MAC123_FAST_OK(base) holds, the stepped 44-bit
 * reference (a) sets no FLAG bits and (b) equals the plain s64 sum,
 * for all three helpers. Build: cc -O2 test_gte_mac_fast.c && ./a.out */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

typedef int32_t s32; typedef int64_t s64; typedef uint32_t u32; typedef uint64_t u64;
#define MAC123_SHIFT (32-12)
#define MAC123_FAST_LIM 0x7ff40000u
#define MAC123_FAST_OK(v) ((u32)((v) ^ ((v) >> 31)) < MAC123_FAST_LIM)

/* reference: verbatim upstream stepped path (generic branch) */
static s64 ref_add(u32 id, u32 *flags, s64 in, s32 addend) {
	s64 a;
	int o = __builtin_add_overflow(in, (s64)addend << MAC123_SHIFT, &a);
	*flags |= (u32)(o && a <  0) << (31 - id);
	*flags |= (u32)(o && a >= 0) << (28 - id);
	return a;
}
static s64 ref_add4(u32 id, u32 *flags, s32 a1, s32 a2, s32 a3, s32 a4, int shift) {
	s64 a = (s64)a1 << (12 + MAC123_SHIFT);
	a = ref_add(id, flags, a, a2);
	a = ref_add(id, flags, a, a3);
	a = ref_add(id, flags, a, a4);
	return a >> (shift + MAC123_SHIFT);
}
static s32 ref_add_s12(u32 id, u32 *flags, s32 in12, s32 addend, int shift) {
	return ref_add(id, flags, (s64)in12 << (12+MAC123_SHIFT), addend) >> (shift+MAC123_SHIFT);
}
static s64 ref_sub_s12(u32 id, u32 *flags, s32 in12, s32 subtrahend, int shift) {
	s64 a;
	int o = __builtin_sub_overflow((s64)in12 << (12+MAC123_SHIFT), (s64)subtrahend << MAC123_SHIFT, &a);
	*flags |= (u32)(o && subtrahend <  0) << (31 - id);
	*flags |= (u32)(o && subtrahend >= 0) << (28 - id);
	return a >> (shift+MAC123_SHIFT);
}

static u64 rng = 0x243f6a8885a308d3ull;
static u32 rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (u32)rng; }
static s32 rnd_s16(void) { return (s32)(int16_t)rnd(); }

static void check_add4(s32 a1, s32 p2, s32 p3, s32 p4, int shift) {
	if (!MAC123_FAST_OK(a1)) return;
	u32 flags = 0;
	s64 ref = ref_add4(1, &flags, a1, p2, p3, p4, shift);
	s64 fast = (((s64)a1 << 12) + p2 + p3 + p4) >> shift;
	assert(flags == 0);
	assert(ref == fast);
}
static void check_s12(s32 in12, s32 ad, int shift) {
	if (!MAC123_FAST_OK(in12)) return;
	u32 f1 = 0, f2 = 0;
	s32 ra = ref_add_s12(2, &f1, in12, ad, shift);
	s64 rs = ref_sub_s12(3, &f2, in12, ad, shift);
	s32 fa = (s32)((((s64)in12 << 12) + ad) >> shift);
	s64 fs = (((s64)in12 << 12) - ad) >> shift;
	assert(f1 == 0 && f2 == 0);
	assert(ra == fa && rs == fs);
}

int main(void) {
	static const s32 pext[] = { 0, 1, -1, (1<<30)-32768, -(1<<30), 32767*32767, -32768*32767, (s32)0x80000000, 0x7fffffff };
	/* boundary sweep: base near +/-LIM, worst-case s16xs16 product extremes */
	for (s64 d = -8192; d <= 8192; d++) {
		for (int s = 0; s <= 1; s++) {
			int shift = s ? 12 : 0;
			s32 a1p = (s32)((s64)MAC123_FAST_LIM - 1 - d);
			s32 a1n = (s32)(-(s64)MAC123_FAST_LIM + d);
			for (unsigned i = 0; i < 4; i++) {
				s32 p = (i & 1) ? 32767*32767 : -32768*32767;
				s32 q = (i & 2) ? 32767*32767 : -32768*32767;
				check_add4(a1p, p, q, p, shift);
				check_add4(a1n, p, q, q, shift);
			}
			/* s12 helpers admit FULL s32 addend — hit the extremes */
			for (unsigned i = 0; i < sizeof pext / sizeof pext[0]; i++) {
				check_s12(a1p, pext[i], shift);
				check_s12(a1n, pext[i], shift);
			}
		}
	}
	/* random: s16xs16 products for add4 (call-site invariant), full s32 for s12 */
	for (long n = 0; n < 20000000; n++) {
		s32 a1 = (s32)rnd();
		s32 p2 = rnd_s16() * rnd_s16();
		s32 p3 = rnd_s16() * rnd_s16();
		s32 p4 = rnd_s16() * rnd_s16();
		int shift = (rnd() & 1) ? 12 : 0;
		check_add4(a1, p2, p3, p4, shift);
		check_s12(a1, (s32)rnd(), shift);
	}
	printf("gte mac fast-path: ALL PASS\n");
	return 0;
}
