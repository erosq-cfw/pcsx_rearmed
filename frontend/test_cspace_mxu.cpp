/* EROSQ host differential test: X1000 MXU1 bgr555_to_rgb565 (cspace.c) vs an
 * independent scalar reference, over randomized lengths and src/dst byte
 * alignments (including the mismatched-parity scalar-fallback path).
 *
 * Build & run (host, no deps):
 *   g++ -O2 -fpermissive -Iinclude -o /tmp/test_cspace_mxu \
 *     frontend/test_cspace_mxu.cpp && /tmp/test_cspace_mxu
 * (-fpermissive: cspace.c is C, this harness is C++; only the C void* casts.)
 *
 * The raw-.word MXU ops are swapped for scalar C models (host_xr[] + the
 * documented QEMU per-lane semantics) via the MXU_X1000_H guard, exactly as
 * plugins/gpu_unai/test_sprite_tile_kernels.cpp does — the .word encodings
 * themselves are validated on-device. Everything else here is the production
 * kernel from cspace.c, included verbatim (no synced copy to drift).
 */
#include <cstdint>
#include <cstdio>
#include <cstring>

/* ---- scalar model of the MXU ops the kernel uses ---- */
static uint32_t host_xr[17];
#define MXU_X1000_H          /* suppress the real raw-.word header */
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
#define MXU_XR15 15
#define MXU_XR16 16

#define HX_SET(r, v) do { if (r) host_xr[(r)] = (uint32_t)(v); } while (0)

static inline uint32_t hx_sll16(uint32_t x, int s) {
	uint32_t lo = (((x & 0xffff) << s) & 0xffff);
	uint32_t hi = ((((x >> 16) & 0xffff) << s) & 0xffff);
	return (hi << 16) | lo;
}
static inline uint32_t hx_slr16(uint32_t x, int s) {
	uint32_t lo = (x & 0xffff) >> s;
	uint32_t hi = ((x >> 16) & 0xffff) >> s;
	return (hi << 16) | lo;
}

#define MXU_S32I2M(r, v)   HX_SET(r, v)
#define MXU_S32M2I(r)      (host_xr[(r)])
#define MXU_S32LDD(r, base) do { uint32_t _v; memcpy(&_v, (const void *)(base), 4); HX_SET(r, _v); } while (0)
#define MXU_S32STD(r, base) do { uint32_t _v = host_xr[(r)]; memcpy((void *)(base), &_v, 4); } while (0)
#define MXU_Q16SLL(a, b, c, d, s) do { uint32_t _b = hx_sll16(host_xr[b], s), _c = hx_sll16(host_xr[c], s); HX_SET(a, _b); HX_SET(d, _c); } while (0)
#define MXU_Q16SLR(a, b, c, d, s) do { uint32_t _b = hx_slr16(host_xr[b], s), _c = hx_slr16(host_xr[c], s); HX_SET(a, _b); HX_SET(d, _c); } while (0)
#define MXU_S32AND(a, b, c) HX_SET(a, host_xr[b] & host_xr[c])
#define MXU_S32OR(a, b, c)  HX_SET(a, host_xr[b] | host_xr[c])

#define HAVE_bgr555_to_rgb565_mxu
#include "cspace.c"

/* independent reference — the upstream scalar do_one, written straight from
 * the bgr555->rgb565 definition, NOT restated from the kernel's shifts. */
static void ref_convert(uint16_t *dst, const uint16_t *src, int pixels) {
	for (int i = 0; i < pixels; i++) {
		unsigned s = src[i];
		unsigned d = (s << 1) & 0xffff;
		dst[i] = (uint16_t)((d & 0x07c0) | ((d << 10) & 0xffff) | (d >> 11));
	}
}

static uint32_t rng = 0x20260717u;
static uint32_t rnd(void) {
	rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
	return rng;
}

/* 8-byte guard on each side, one shared backing buffer, so a stray MXU store
 * past the range shows up as a mismatch too. */
#define GUARD 8
#define CAP   600
static uint16_t bufA[GUARD + CAP + GUARD];
static uint16_t bufB[GUARD + CAP + GUARD];
static uint16_t srcbuf[GUARD + CAP + GUARD];
static int fails = 0;

static void run_case(int pixels, int src_off, int dst_off) {
	for (int i = 0; i < GUARD + CAP + GUARD; i++) {
		srcbuf[i] = (uint16_t)rnd();
		uint16_t f = (uint16_t)rnd();
		bufA[i] = f; bufB[i] = f;      /* identical guards + init */
	}
	const uint16_t *src = srcbuf + GUARD + src_off;
	uint16_t *got = bufA + GUARD + dst_off;
	uint16_t *ref = bufB + GUARD + dst_off;

	bgr555_to_rgb565(got, src, pixels);
	ref_convert(ref, src, pixels);

	if (memcmp(bufA, bufB, sizeof(bufA)) != 0) {
		fprintf(stderr, "FAIL pixels=%d src_off=%d dst_off=%d\n",
			pixels, src_off, dst_off);
		for (int i = 0; i < GUARD + CAP + GUARD && fails < 8; i++)
			if (bufA[i] != bufB[i]) {
				fprintf(stderr, "  [%d] got=%04x ref=%04x\n",
					i - GUARD, bufA[i], bufB[i]);
				fails++;
			}
	}
}

int main(void) {
	/* every length 0..64 (covers head/tail + all pixels%4), each at the four
	 * src/dst alignment parities (matched -> MXU path, mismatched -> scalar
	 * fallback), then random larger lengths. */
	for (int px = 0; px <= 64; px++)
		for (int so = 0; so <= 1; so++)
			for (int doff = 0; doff <= 1; doff++)
				run_case(px, so, doff);

	for (int t = 0; t < 200000; t++) {
		int px = rnd() % (CAP - 1);
		run_case(px, rnd() & 1, rnd() & 1);
	}

	if (fails) { printf("bgr555 MXU: FAILED (%d mismatches shown)\n", fails); return 1; }
	printf("bgr555 MXU OK\n");
	return 0;
}
