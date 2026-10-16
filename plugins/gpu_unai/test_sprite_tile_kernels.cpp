/* EROSQ host differential test: X1000 sprite/tile fast paths vs the scalar
 * drivers, on randomized textures/CLUTs/windows/widths/alignments.
 *
 * Build & run (host, no deps):
 *   g++ -O2 -o /tmp/test_sprite_tile plugins/gpu_unai/test_sprite_tile_kernels.cpp \
 *     && /tmp/test_sprite_tile
 *
 * The two asm cores (mxu_light_pair, mxu_blend_pair_sub) are swapped for
 * their exact C models via GPU_MXU_HOST_TEST (models = the semantics the
 * silicon versions were validated against on-device, mxutest 2026-07-14).
 * Everything else — kernel walk logic, transparency, per-lane select,
 * head/tail handling, dispatcher preconditions — is the production code.
 * Both sides run through SpriteMaybeMXU/gpuTileSpanFn; the reference side
 * just sets mxu_spans_on=0 so the template's scalar body executes. Buffers
 * are compared in full, catching stray writes as well as wrong pixels.
 */
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

typedef uint32_t u32_pre;               /* before gpu_unai.h's #define u32 */
static u32_pre host_xr[17];
static int host_raw_op_hit = 0;

#define GPU_MXU_HOST_TEST
#define GPU_UNAI_USE_MXU

/* Suppress the raw-.word asm header; emulate the two GPR-interface ops the
 * tested kernels execute (S32I2M/S32M2I) and trap any other MXU op — the
 * tested paths must never reach one (they'd be silent wrong-answers). */
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
#define MXU_SS 1
#define MXU_S32I2M(r, v)         ((void)(host_xr[(r)] = (u32_pre)(v)))
#define MXU_S32M2I(r)            (host_xr[(r)])
#define MXU_HOST_TRAP            ((void)(host_raw_op_hit = 1))
#define MXU_S32AND(a, b, c)      MXU_HOST_TRAP
#define MXU_S32OR(a, b, c)       MXU_HOST_TRAP
#define MXU_S32XOR(a, b, c)      MXU_HOST_TRAP
#define MXU_S32STD(v, b)         MXU_HOST_TRAP
#define MXU_Q16SLR(a, b, c, d, s) MXU_HOST_TRAP
#define MXU_Q16SLL(a, b, c, d, s) MXU_HOST_TRAP
#define MXU_Q16SAR(a, b, c, d, s) MXU_HOST_TRAP
#define MXU_Q16ADD(a, b, c, d, p, m) MXU_HOST_TRAP
#define MXU_D16MADL(a, b, c, d, p, m) MXU_HOST_TRAP
#define MXU_D16MIN(a, b, c)      MXU_HOST_TRAP

#include "../gpulib/gpu.h"   /* HTOLE/LE*TOH + gpulib decls, as in gpulib_if.cpp */
#include "gpu_unai.h"
#include "gpu_fixedpoint.h"
#include "gpu_inner.h"
/* gpuClearImage pref-30 path: the stub POISONS the line (worst-case
 * PrepareForStore semantics) -- any line the clear does not overwrite whole
 * then mismatches the independently computed expected fill. */
static int pref_calls, pref_misaligned;
#define GPU_PREF_STORE(p) do { pref_calls++; if ((uintptr_t)(p) & 31) pref_misaligned++; \
	memset((void *)(p), 0xEE, 32); } while (0)
#include "gpu_raster_image.h"

/* deterministic PRNG (no libc rand variance across hosts) */
static u32 rng_state = 0x2026'0716;
static u32 rnd(void) {
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 17;
	rng_state ^= rng_state << 5;
	return rng_state;
}

#define TEX_ROWS 256   /* v_mask can address 256 rows of 2048 bytes */
#define VRAM_ROWS 20
static u8 tex[TEX_ROWS * 2048];
static le16_t clut[256];
static le16_t bufA[VRAM_ROWS * FRAME_WIDTH], bufB[VRAM_ROWS * FRAME_WIDTH];
static int fails = 0;

static void fill_random(void)
{
	for (unsigned i = 0; i < sizeof(tex); i++) tex[i] = (u8)rnd();
	for (unsigned i = 0; i < 256; i++) {
		u16 v = (u16)rnd();
		if ((rnd() & 3) == 0) v = 0;            /* transparent texels */
		clut[i] = u16_to_le16(v);
	}
	for (unsigned i = 0; i < VRAM_ROWS * FRAME_WIDTH; i++)
		bufA[i] = u16_to_le16((u16)rnd());
	memcpy(bufB, bufA, sizeof(bufA));
}

typedef void (*spritefn)(le16_t *, u32, const u8 *, const gpu_unai_inner_t &);

static void run_sprite_case(const char *name, spritefn fn, int trial)
{
	fill_random();
	u32 umask_pool[5] = { 0xff, 0x3f, 0x1f, 0xef /* non-contiguous */, 0 };
	gpu_unai_inner_t &inn = gpu_unai.inn;
	memset(&inn, 0, sizeof(inn));
	u32 umask = umask_pool[rnd() % 5];
	u32 vmask = (rnd() & 1) ? 0xff : 0x3f;
	u32 count = 1 + rnd() % 40;
	inn.mask_v00u = (vmask << 24) | umask;
	/* ~70% of trials constructed to satisfy the no-wrap precondition */
	if (umask == 0xff && (rnd() % 10) < 7 && count < 200)
		inn.u = rnd() % (256 - count);
	else
		inn.u = rnd() % 512;
	inn.v = rnd() % 512;
	inn.y0 = rnd() % 4;
	inn.y1 = inn.y0 + 1 + rnd() % 12;
	inn.ilace_mask = (rnd() & 3) == 0 ? 1 : 0;
	inn.CBA = clut;
	inn.bgr0888 = rnd() & 0xffffff;
	u32 dst_off = (rnd() % FRAME_WIDTH) & 0x3ff;
	if (dst_off + count >= FRAME_WIDTH) dst_off = 0;

	host_raw_op_hit = 0;
	mxu_spans_on = 0;
	fn(&bufA[dst_off], count, tex, inn);
	mxu_spans_on = 1;
	fn(&bufB[dst_off], count, tex, inn);
	if (host_raw_op_hit) {
		printf("FAIL %s trial %d: raw MXU op executed on host\n", name, trial);
		fails++;
		return;
	}
	if (memcmp(bufA, bufB, sizeof(bufA)) != 0) {
		for (unsigned i = 0; i < VRAM_ROWS * FRAME_WIDTH; i++)
			if (le16_raw(bufA[i]) != le16_raw(bufB[i])) {
				printf("FAIL %s trial %d: first diff at %u (row %u col %u): "
				       "scalar %04x fast %04x  [u=%u count=%u umask=%02x y0=%d y1=%d il=%d]\n",
				       name, trial, i, i / FRAME_WIDTH, i % FRAME_WIDTH,
				       le16_to_u16(bufA[i]), le16_to_u16(bufB[i]),
				       inn.u, count, umask, inn.y0, inn.y1, inn.ilace_mask);
				break;
			}
		fails++;
	}
}

/* unlit textured poly spans: scalar gpuPolySpanFn<CF> vs the dispatcher
 * (poly_fast_unlit). Random 22.10 u/v + signed steps, masks incl. wrap,
 * transparent CLUT entries; 1 trial in 8 with pixel-skip on (must bail). */
template<int CF>
static void run_poly_case(const char *name, int trial)
{
	fill_random();
	gpu_unai_inner_t &inn = gpu_unai.inn;
	memset(&inn, 0, sizeof(inn));
	u32 umask_pool[4] = { 0xff, 0x3f, 0x1f, 0xef };
	u32 vmask = (rnd() & 1) ? 0xff : 0x3f;
	inn.mask_v00u = (vmask << 24) | umask_pool[rnd() % 4];
	inn.u = rnd() % (256 << 10);
	inn.v = rnd() % (256 << 10);
	inn.u_inc = (s32)(rnd() % (4 << 10)) - (2 << 10);
	inn.v_inc = (s32)(rnd() % (4 << 10)) - (2 << 10);
	inn.TBA = (const le16_t *)tex;
	inn.CBA = clut;
	u32 count = 1 + rnd() % 60;
	u32 dst_off = rnd() % (FRAME_WIDTH - 64);
	pskip_mask2 = (rnd() % 8 == 0) ? 0x4a : 0;
	mxu_spans_on = 0;
	gpuPolySpanFn<CF>(gpu_unai, &bufA[dst_off], count, 0);
	mxu_spans_on = 1;
	PolySpanMaybeMXU<CF>(gpu_unai, &bufB[dst_off], count, 0);
	pskip_mask2 = 0;
	if (memcmp(bufA, bufB, sizeof(bufA)) != 0) {
		printf("FAIL %s trial %d [u=%u v=%u du=%d dv=%d count=%u mask=%08x]\n",
		       name, trial, inn.u, inn.v, inn.u_inc, inn.v_inc, count, inn.mask_v00u);
		fails++;
	}
}

template<int CF>
static void run_tile_case(const char *name, int trial)
{
	fill_random();
	u32 count = 1 + rnd() % 60;
	u16 data = (u16)(rnd() & 0x7fff);       /* GPU_RGB16 output: bit15 clear */
	u32 dst_off = rnd() % (FRAME_WIDTH - 64);
	u32 rows = 1 + rnd() % 4;

	host_raw_op_hit = 0;
	mxu_spans_on = 0;
	for (u32 r = 0; r < rows; r++)
		gpuTileSpanFn<CF>(&bufA[dst_off + r * FRAME_WIDTH], data, count);
	mxu_spans_on = 1;
	for (u32 r = 0; r < rows; r++)
		gpuTileSpanFn<CF>(&bufB[dst_off + r * FRAME_WIDTH], data, count);
	if (host_raw_op_hit) {
		printf("FAIL %s trial %d: raw MXU op executed on host\n", name, trial);
		fails++;
		return;
	}
	if (memcmp(bufA, bufB, sizeof(bufA)) != 0) {
		for (unsigned i = 0; i < VRAM_ROWS * FRAME_WIDTH; i++)
			if (le16_raw(bufA[i]) != le16_raw(bufB[i])) {
				printf("FAIL %s trial %d: first diff at %u: scalar %04x fast %04x "
				       "[data=%04x count=%u off=%u]\n",
				       name, trial, i, le16_to_u16(bufA[i]), le16_to_u16(bufB[i]),
				       data, count, dst_off);
				break;
			}
		fails++;
	}
}

/* VRAM-sized, page-aligned buffer like gpulib's */
static le16_t vram_t[FRAME_WIDTH * FRAME_HEIGHT] __attribute__((aligned(4096)));
static u16 expect_t[FRAME_WIDTH * FRAME_HEIGHT];
static void run_clear_case(int trial)
{
	for (int i = 0; i < FRAME_WIDTH * FRAME_HEIGHT; i++)
		expect_t[i] = (u16)rnd(), vram_t[i] = u16_to_le16(expect_t[i]);
	gpu_unai.vram = vram_t;
	le32_t pk[4];
	s32 x = (s32)(rnd() % 1100) - 40, y = (s32)(rnd() % 560) - 30;
	s32 w = rnd() % 1024, h = rnd() % 512;
	u32 col = rnd() & 0xffffff;
	pk[0] = u32_to_le32(col);
	pk[1] = u32_to_le32((u32)(u16)x | ((u32)(u16)y << 16));
	pk[2] = u32_to_le32((u32)(u16)w | ((u32)(u16)h << 16));
	PtrUnion packet = { .ptr = (void *)pk };
	/* expected: same clipping rules, plain per-pixel fill */
	s32 x0 = x & ~0xF, w0 = ((w & 0x3ff) + 0xF) & ~0xF, y0 = y, h0 = h & 0x1ff;
	w0 += x0; if (x0 < 0) x0 = 0; if (w0 > FRAME_WIDTH) w0 = FRAME_WIDTH; w0 -= x0;
	h0 += y0; if (y0 < 0) y0 = 0; if (h0 > FRAME_HEIGHT) h0 = FRAME_HEIGHT; h0 -= y0;
	u16 c16 = (u16)GPU_RGB16(col);
	if (w0 > 0 && h0 > 0)
		for (int yy = y0; yy < y0 + h0; yy++)
			for (int xx = x0; xx < x0 + w0; xx++)
				expect_t[yy * FRAME_WIDTH + xx] = c16;
	gpuClearImage(packet);
	for (int i = 0; i < FRAME_WIDTH * FRAME_HEIGHT; i++)
		if (le16_to_u16(vram_t[i]) != expect_t[i]) {
			printf("FAIL clear trial %d at %d (x=%d y=%d w=%d h=%d)\n", trial, i, x, y, w, h);
			fails++; return;
		}
}

int main(void)
{
	/* differential test: both sides read the same LUT, values just need
	 * to be valid 5-bit entries */
	for (unsigned i = 0; i < 32 * 32; i++)
		gpu_unai.LightLUT[i] = (u8)(rnd() & 0x1f);

	const int N = 4000;
	for (int t = 0; t < N; t++) {
		run_sprite_case("spr-0x061", SpriteMaybeMXU<0x061>, t);
		run_sprite_case("spr-0x161", SpriteMaybeMXU<0x161>, t);
		run_sprite_case("spr-0x063", SpriteMaybeMXU<0x063>, t);
		run_sprite_case("spr-0x163", SpriteMaybeMXU<0x163>, t);
		run_sprite_case("spr-0x020", SpriteMaybeMXU<0x020>, t);
		run_sprite_case("spr-0x120", SpriteMaybeMXU<0x120>, t);
		run_sprite_case("spr-0x040", SpriteMaybeMXU<0x040>, t);
		run_sprite_case("spr-0x140", SpriteMaybeMXU<0x140>, t);
		run_poly_case<0x020>("poly-0x020", t);
		run_poly_case<0x040>("poly-0x040", t);
		run_poly_case<0x060>("poly-0x060", t);
		run_poly_case<0x120>("poly-0x120", t);
		run_poly_case<0x140>("poly-0x140", t);
		run_poly_case<0x160>("poly-0x160", t);
		run_poly_case<0x022>("poly-0x022", t);
		run_poly_case<0x02a>("poly-0x02a", t);
		run_poly_case<0x032>("poly-0x032", t);
		run_poly_case<0x03a>("poly-0x03a", t);
		run_poly_case<0x122>("poly-0x122", t);
		run_poly_case<0x12a>("poly-0x12a", t);
		run_poly_case<0x132>("poly-0x132", t);
		run_poly_case<0x13a>("poly-0x13a", t);
		run_poly_case<0x042>("poly-0x042", t);
		run_poly_case<0x04a>("poly-0x04a", t);
		run_poly_case<0x052>("poly-0x052", t);
		run_poly_case<0x05a>("poly-0x05a", t);
		run_poly_case<0x142>("poly-0x142", t);
		run_poly_case<0x14a>("poly-0x14a", t);
		run_poly_case<0x152>("poly-0x152", t);
		run_poly_case<0x15a>("poly-0x15a", t);
		run_poly_case<0x062>("poly-0x062", t);
		run_poly_case<0x06a>("poly-0x06a", t);
		run_poly_case<0x072>("poly-0x072", t);
		run_poly_case<0x07a>("poly-0x07a", t);
		run_poly_case<0x162>("poly-0x162", t);
		run_poly_case<0x16a>("poly-0x16a", t);
		run_poly_case<0x172>("poly-0x172", t);
		run_poly_case<0x17a>("poly-0x17a", t);
		run_tile_case<0x002>("tile-0x002", t);
		run_tile_case<0x00a>("tile-0x00a", t);
		run_tile_case<0x012>("tile-0x012", t);
		run_tile_case<0x01a>("tile-0x01a", t);
		run_tile_case<0x102>("tile-0x102", t);
		run_tile_case<0x10a>("tile-0x10a", t);
		run_tile_case<0x112>("tile-0x112", t);
		run_tile_case<0x11a>("tile-0x11a", t);
		if (t < 600) run_clear_case(t);
		if (fails > 10) break;
	}
	if (pref_misaligned) { printf("FAIL: %d misaligned pref 30\n", pref_misaligned); return 1; }
	printf("clear: 600 trials, %d pref-30 lines (poisoned, all fully overwritten)\n", pref_calls);
	if (fails) { printf("FAILED: %d mismatching trials\n", fails); return 1; }
	printf("PASS: %d trials x 8 sprite CFs + 30 poly CFs + 8 tile CFs, all bit-exact\n", N);
	return 0;
}
