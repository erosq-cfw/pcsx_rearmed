/* EROSQ host differential test: X1000 MXU "Sharp" 512->320 downscaler
 * (cspace.c bgr555_to_rgb565_512_to_320_avg, MXU path) vs an independent
 * per-channel reference written from the definition (output j = convert(
 * floor-average of source columns off[j] and off[j]+1), off =
 * {0,1,3,4,6,8,9,11,12,14} per 16-column group).
 *
 * Build & run (host, no deps):
 *   g++ -O2 -fpermissive -Iinclude -o /tmp/test_cspace_avg_mxu \
 *     frontend/test_cspace_avg_mxu.cpp && /tmp/test_cspace_avg_mxu
 *
 * The raw-.word MXU ops are swapped for scalar C models (QEMU per-lane
 * semantics, as in test_cspace_mxu.cpp); the kernel itself is #included
 * verbatim from cspace.c. A built-in mutation check proves the harness can
 * see a wrong lane shuffle.
 */
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>

static uint32_t host_xr[17];
#define MXU_X1000_H
#define MXU_XR0 0
#define MXU_XR1 1
#define MXU_XR2 2
#define MXU_XR3 3
#define MXU_XR4 4
#define MXU_XR5 5
#define MXU_XR6 6
#define MXU_XR7 7
#define MXU_XR8 8
#define MXU_XR9 9
#define MXU_XR10 10
#define MXU_XR11 11
#define MXU_XR12 12
#define MXU_XR13 13
#define MXU_XR14 14
#define MXU_XR15 15
#define MXU_XR16 16
#define MXU_WW 0
#define MXU_AA 0

#define HX_SET(r, v) do { if (r) host_xr[(r)] = (uint32_t)(v); } while (0)
static inline uint32_t hx_sll16(uint32_t x, int s) {
	return ((((x >> 16) << s) & 0xffff) << 16) | (((x & 0xffff) << s) & 0xffff);
}
static inline uint32_t hx_slr16(uint32_t x, int s) {
	return ((((x >> 16) & 0xffff) >> s) << 16) | ((x & 0xffff) >> s);
}
static inline uint32_t hx_add16(uint32_t b, uint32_t c) {
	return ((((b >> 16) + (c >> 16)) & 0xffff) << 16) | (((b & 0xffff) + (c & 0xffff)) & 0xffff);
}
static int mutate_sfl = 0;   /* harness self-check: break one shuffle */

#define MXU_S32I2M(r, v)   HX_SET(r, v)
#define MXU_S32M2I(r)      (host_xr[(r)])
#define MXU_S32LDD(r, base) do { uint32_t _v; memcpy(&_v, (const void *)(base), 4); HX_SET(r, _v); } while (0)
#define MXU_S32STD(r, base) do { uint32_t _v = host_xr[(r)]; memcpy((void *)(base), &_v, 4); } while (0)
/* immediate-offset forms (byte offset); model enforces word alignment */
#define MXU_S32LDDO(r, base, off) do { if (((uintptr_t)(base) + (off)) & 3) { fprintf(stderr, "LDDO misaligned\n"); exit(1); } \
	uint32_t _v; memcpy(&_v, (const char *)(base) + (off), 4); HX_SET(r, _v); } while (0)
#define MXU_S32STDO(r, base, off) do { if (((uintptr_t)(base) + (off)) & 3) { fprintf(stderr, "STDO misaligned\n"); exit(1); } \
	uint32_t _v = host_xr[(r)]; memcpy((char *)(base) + (off), &_v, 4); } while (0)
#define MXU_Q16SLL(a, b, c, d, s) do { uint32_t _b = hx_sll16(host_xr[b], s), _c = hx_sll16(host_xr[c], s); HX_SET(a, _b); HX_SET(d, _c); } while (0)
#define MXU_Q16SLR(a, b, c, d, s) do { uint32_t _b = hx_slr16(host_xr[b], s), _c = hx_slr16(host_xr[c], s); HX_SET(a, _b); HX_SET(d, _c); } while (0)
#define MXU_Q16ADD(a, b, c, d, optn2, aptn2) do { \
	if ((optn2) != 0 || (aptn2) != 0) { fprintf(stderr, "model: only WW/AA\n"); exit(1); } \
	uint32_t _s = hx_add16(host_xr[b], host_xr[c]); HX_SET(a, _s); HX_SET(d, _s); } while (0)
#define MXU_S32AND(a, b, c) HX_SET(a, host_xr[b] & host_xr[c])
#define MXU_S32OR(a, b, c)  HX_SET(a, host_xr[b] | host_xr[c])
#define MXU_S32XOR(a, b, c) HX_SET(a, host_xr[b] ^ host_xr[c])
#define MXU_S32SFL(a, b, c, d, ptn2) do { \
	uint32_t _b = host_xr[b], _c = host_xr[c]; \
	if ((ptn2) != 3) { fprintf(stderr, "model: only SFL ptn2=3\n"); exit(1); } \
	HX_SET(a, (_b & 0xffff0000u) | (_c >> 16)); \
	HX_SET(d, mutate_sfl ? ((_c << 16) | (_b & 0xffffu)) : ((_b << 16) | (_c & 0xffffu))); \
} while (0)
#define MXU_S32ALNI(a, b, c, ptn3) do { \
	if ((ptn3) != 2) { fprintf(stderr, "model: only ALNI ptn3=2\n"); exit(1); } \
	HX_SET(a, (host_xr[b] << 16) | (host_xr[c] >> 16)); \
} while (0)

#define HAVE_bgr555_to_rgb565_mxu
#include "cspace.c"

/* independent reference, straight from the definitions */
static uint16_t ref_avg(uint16_t a, uint16_t b) {
	unsigned r = ((a & 31) + (b & 31)) / 2;
	unsigned g = (((a >> 5) & 31) + ((b >> 5) & 31)) / 2;
	unsigned bl = (((a >> 10) & 31) + ((b >> 10) & 31)) / 2;
	return (uint16_t)((bl << 10) | (g << 5) | r);   /* bit15 always 0 */
}
static uint16_t ref_conv(uint16_t s) {
	unsigned r = s & 31, g = (s >> 5) & 31, b = (s >> 10) & 31;
	return (uint16_t)((r << 11) | (g << 6) | b);    /* green LSB 0 */
}
static const int off512[10] = { 0, 1, 3, 4, 6, 8, 9, 11, 12, 14 };

static uint32_t rng = 0x20261016u;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

#define GUARD 8
#define CAP 700
static uint16_t srcbuf[GUARD + CAP + GUARD];
static uint16_t bufA[GUARD + CAP + GUARD], bufB[GUARD + CAP + GUARD];

static int run_case(int dpixels, int src_off, int verbose) {
	for (int i = 0; i < GUARD + CAP + GUARD; i++) {
		srcbuf[i] = (uint16_t)rnd();            /* incl. bit15 set: mask bit is ignored */
		uint16_t f = (uint16_t)rnd();
		bufA[i] = f; bufB[i] = f;
	}
	const uint16_t *src = srcbuf + GUARD + src_off;
	bgr555_to_rgb565_512_to_320_avg(bufA + GUARD, src, dpixels);
	for (int g = 0; g + 10 <= dpixels; g += 10)
		for (int j = 0; j < 10; j++) {
			const uint16_t *s = src + (g / 10) * 16 + off512[j];
			bufB[GUARD + g + j] = ref_conv(ref_avg(s[0], s[1]));
		}
	if (memcmp(bufA, bufB, sizeof bufA)) {
		if (verbose) {
			fprintf(stderr, "FAIL dpixels=%d src_off=%d\n", dpixels, src_off);
			for (int i = 0, n = 0; i < GUARD + CAP + GUARD && n < 8; i++)
				if (bufA[i] != bufB[i]) {
					fprintf(stderr, "  [%d] got=%04x ref=%04x\n", i - GUARD, bufA[i], bufB[i]); n++;
				}
		}
		return 1;
	}
	return 0;
}

/* plain 1:1 convert and the Fast (pick) 512->320 kernel, same harness */
static const int pick512[10] = { 0, 2, 4, 5, 7, 8, 10, 12, 13, 15 };
static int run_plain(int pixels, int so, int doff) {
	for (int i = 0; i < GUARD + CAP + GUARD; i++) {
		srcbuf[i] = (uint16_t)rnd(); uint16_t f = (uint16_t)rnd(); bufA[i] = f; bufB[i] = f;
	}
	const uint16_t *src = srcbuf + GUARD + so;
	bgr555_to_rgb565(bufA + GUARD + doff, src, pixels);
	for (int i = 0; i < pixels; i++) {
		unsigned v = src[i], d = (v << 1) & 0xffff;   /* upstream do_one */
		bufB[GUARD + doff + i] = (uint16_t)((d & 0x07c0) | ((v << 11) & 0xffff) | (d >> 11));
	}
	return memcmp(bufA, bufB, sizeof bufA) != 0;
}
static int run_fast(int dpixels, int so) {
	for (int i = 0; i < GUARD + CAP + GUARD; i++) {
		srcbuf[i] = (uint16_t)rnd(); uint16_t f = (uint16_t)rnd(); bufA[i] = f; bufB[i] = f;
	}
	const uint16_t *src = srcbuf + GUARD + so;
	bgr555_to_rgb565_512_to_320(bufA + GUARD, src, dpixels);
	for (int g = 0; g + 10 <= dpixels; g += 10)
		for (int j = 0; j < 10; j++) {
			unsigned v = src[(g / 10) * 16 + pick512[j]] & 0x7fff;
			bufB[GUARD + g + j] = ref_conv((uint16_t)v);
		}
	return memcmp(bufA, bufB, sizeof bufA) != 0;
}

int main(void) {
	int fails = 0, cases = 0;
	int pf = 0, ff = 0;
	for (int px = 0; px <= 80; px++)
		for (int so = 0; so <= 1; so++)
			for (int doff = 0; doff <= 1; doff++) { pf += run_plain(px, so, doff); cases++; }
	for (int t = 0; t < 100000; t++) { pf += run_plain((int)(rnd() % (CAP - 2)), rnd() & 1, rnd() & 1); cases++; }
	for (int dp = 0; dp <= 340; dp++)
		for (int so = 0; so <= 3; so++) { ff += run_fast(dp, so); cases++; }
	for (int t = 0; t < 100000; t++) { ff += run_fast((int)(rnd() % 420), rnd() & 3); cases++; }
	printf("plain 1:1 MXU: %d FAIL; Fast 512->320 MXU: %d FAIL\n", pf, ff);
	fails += pf + ff;
	for (int dp = 0; dp <= 340; dp++)
		for (int so = 0; so <= 3; so++) { fails += run_case(dp, so, fails < 3); cases++; }
	for (int t = 0; t < 200000; t++) {
		fails += run_case((int)(rnd() % 420), (int)(rnd() & 3), fails < 3); cases++;
	}
	/* uniform-field and all-extremes sweeps (saturated channels) */
	for (int v = 0; v < 0x10000; v += 0x111) {
		for (int i = 0; i < GUARD + CAP + GUARD; i++) srcbuf[i] = (uint16_t)(v ^ ((i & 1) ? 0x7fff : 0));
		memset(bufA, 0, sizeof bufA); memset(bufB, 0, sizeof bufB);
		bgr555_to_rgb565_512_to_320_avg(bufA + GUARD, srcbuf + GUARD, 320);
		for (int j = 0; j < 320; j++) {
			const uint16_t *s = srcbuf + GUARD + (j / 10) * 16 + off512[j % 10];
			bufB[GUARD + j] = ref_conv(ref_avg(s[0], s[1]));
		}
		if (memcmp(bufA, bufB, sizeof bufA)) { fails++; fprintf(stderr, "FAIL extremes v=%04x\n", v); }
		cases++;
	}
	/* mutation: a broken shuffle must be caught (aligned src only) */
	mutate_sfl = 1;
	int caught = 0;
	for (int t = 0; t < 100; t++) caught += run_case(320, 0, 0);
	mutate_sfl = 0;
	printf("512->320 avg MXU: %d cases, %d FAIL; mutation caught %d/100\n", cases, fails, caught);
	return (fails || caught != 100) ? 1 : 0;
}
