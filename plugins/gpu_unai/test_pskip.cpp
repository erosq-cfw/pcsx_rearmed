/* EROSQ host differential test: hi-res horizontal pixel-skip (pskip_mask2).
 *
 * Property under test (display identity): running any textured span/sprite
 * kernel with the skip mask active must (a) leave skipped columns byte-
 * identical to the pre-run poison — never fetched, never written — and
 * (b) produce bit-identical results to a mask-off run on every rendered
 * column. Column phase is (address >> 1) & 7, exactly what the kernels use,
 * so all 8 phases (including the lane-1 wrap at phase 7) are exercised via
 * randomized dst offsets.
 *
 * Build & run (cross + qemu-user: gpu_inner.h pulls in gpu_inner_mxu.h only
 * under __mips__; GPU_MXU_HOST_TEST swaps the MXU ops for C models, so no MXU
 * hardware/emulation is needed; -fpermissive: the harness casts to const
 * le16_t* fields that are non-const):
 *   B=$EROSQ_ROOT/buildroot-2026.08/output/host
 *   $B/bin/mipsel-buildroot-linux-musl-g++ -O2 -fpermissive -Iinclude -o /tmp/test_pskip \
 *     plugins/gpu_unai/test_pskip.cpp && qemu-mipsel-static -L $B/mipsel-buildroot-linux-musl/sysroot /tmp/test_pskip
 *
 * Same host-model scheme as test_sprite_tile_kernels.cpp; additionally
 * models Q16ADD(WW,AA) and Q16SLR (per-16-bit-lane add / logical shift),
 * which the gouraud accumulator path executes.
 */
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

typedef uint32_t u32_pre;
static u32_pre host_xr[17];
static int host_raw_op_hit = 0;

#define GPU_MXU_HOST_TEST
#define GPU_UNAI_USE_MXU

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
static inline u32_pre lanes_add(u32_pre a, u32_pre b) {
	return (((a + b) & 0xffffu)) | ((((a >> 16) + (b >> 16)) & 0xffffu) << 16);
}
static inline u32_pre lanes_slr(u32_pre a, unsigned s) {
	return ((a & 0xffffu) >> s) | ((((a >> 16) & 0xffffu) >> s) << 16);
}
/* only the WW/AA form is used by the gouraud accumulators */
#define MXU_Q16ADD(a, b, c, d, p, m) \
	((void)(host_xr[(a)] = lanes_add(host_xr[(b)], host_xr[(c)])))
/* Q16SLR xa,xb,xc,xd,s: xa = xb lanes >> s, xd = xc lanes >> s */
#define MXU_Q16SLR(a, b, c, d, s) \
	((void)(host_xr[(a)] = lanes_slr(host_xr[(b)], (s)), \
	        host_xr[(d)] = lanes_slr(host_xr[(c)], (s))))
#define MXU_Q16SLL(a, b, c, d, s) MXU_HOST_TRAP
#define MXU_Q16SAR(a, b, c, d, s) MXU_HOST_TRAP
#define MXU_S32AND(a, b, c)      MXU_HOST_TRAP
#define MXU_S32OR(a, b, c)       MXU_HOST_TRAP
#define MXU_S32XOR(a, b, c)      MXU_HOST_TRAP
#define MXU_S32STD(v, b)         MXU_HOST_TRAP
#define MXU_D16MADL(a, b, c, d, p, m) MXU_HOST_TRAP
#define MXU_D16MIN(a, b, c)      MXU_HOST_TRAP

#include "../gpulib/gpu.h"
#include "gpu_unai.h"
#include "gpu_fixedpoint.h"
#include "gpu_inner.h"

static u32 rng_state = 0x20260717;
static u32 rnd(void) {
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 17;
	rng_state ^= rng_state << 5;
	return rng_state;
}

#define TEX_ROWS 256
#define VRAM_ROWS 20
static u8 tex[TEX_ROWS * 2048];
static le16_t clut[256];
static le16_t bufP[VRAM_ROWS * FRAME_WIDTH];  /* poison (pristine) */
static le16_t bufA[VRAM_ROWS * FRAME_WIDTH];  /* mask off (reference) */
static le16_t bufB[VRAM_ROWS * FRAME_WIDTH];  /* mask on */
static int fails = 0;

static void fill_random(void)
{
	for (unsigned i = 0; i < sizeof(tex); i++) tex[i] = (u8)rnd();
	for (unsigned i = 0; i < 256; i++) {
		u16 v = (u16)rnd();
		if ((rnd() & 3) == 0) v = 0;
		clut[i] = u16_to_le16(v);
	}
	for (unsigned i = 0; i < VRAM_ROWS * FRAME_WIDTH; i++)
		bufP[i] = u16_to_le16((u16)rnd());
	memcpy(bufA, bufP, sizeof(bufP));
	memcpy(bufB, bufP, sizeof(bufP));
}

/* (a) skipped columns of bufB untouched, (b) rendered columns == bufA */
static void check(const char *name, int trial, u32 mask8)
{
	for (unsigned i = 0; i < VRAM_ROWS * FRAME_WIDTH; i++) {
		unsigned phase = ((uintptr_t)&bufB[i] >> 1) & 7;
		int skipped = (mask8 >> phase) & 1;
		u16 want = skipped ? le16_to_u16(bufP[i]) : le16_to_u16(bufA[i]);
		if (le16_to_u16(bufB[i]) != want) {
			printf("FAIL %s trial %d idx %u (row %u col %u phase %u %s): "
			       "want %04x got %04x (poison %04x ref %04x)\n",
			       name, trial, i, i / FRAME_WIDTH, i % FRAME_WIDTH, phase,
			       skipped ? "SKIP" : "draw",
			       want, le16_to_u16(bufB[i]),
			       le16_to_u16(bufP[i]), le16_to_u16(bufA[i]));
			fails++;
			return;
		}
	}
	if (host_raw_op_hit) {
		printf("FAIL %s trial %d: unmodeled raw MXU op on host\n", name, trial);
		fails++;
	}
}

static void setup_poly_inn(u32 count)
{
	gpu_unai_inner_t &inn = gpu_unai.inn;
	memset(&inn, 0, sizeof(inn));
	inn.TBA = (le16_t *)tex;
	inn.CBA = clut;
	inn.mask_v00u = 0xff000000u | 0xffu;
	inn.u = (rnd() % (200 << 10));
	inn.v = (rnd() % (200 << 10));
	inn.u_inc = (s32)(rnd() % (3 << 10)) - (1 << 10);
	inn.v_inc = (s32)(rnd() % (3 << 10)) - (1 << 10);
	inn.bgr0888 = rnd() & 0xffffff;
	inn.gCol.c.r = (u16)rnd(); inn.gCol.c.g = (u16)rnd(); inn.gCol.c.b = (u16)rnd();
	inn.gInc.c.r = (u16)(rnd() & 0x3ff); inn.gInc.c.g = (u16)(rnd() & 0x3ff);
	inn.gInc.c.b = (u16)(rnd() & 0x3ff);
	(void)count;
}

typedef void (*polyfn)(const gpu_unai_t &, le16_t *, u32, s32);

template <int TM, bool GOU, bool MSET, int BM>
static void mxu_poly_thunk(const gpu_unai_t &g, le16_t *d, u32 c, s32 y)
{
	(void)y;
	poly_mxu_tx_lit<TM, GOU, MSET, BM>(g, d, c);
}

static void run_poly_case(const char *name, polyfn fn, int trial, u32 mask8)
{
	fill_random();
	u32 count = 1 + rnd() % 40;
	setup_poly_inn(count);
	u32 dst_off = rnd() % (VRAM_ROWS * FRAME_WIDTH - 64);
	/* gouraud state is consumed identically by both runs (same inn) */
	pskip_mask2 = 0;
	fn(gpu_unai, &bufA[dst_off], count, 0);
	pskip_mask2 = mask8 | (mask8 << 8);
	fn(gpu_unai, &bufB[dst_off], count, 0);
	pskip_mask2 = 0;
	check(name, trial, mask8);
}

typedef void (*spritefn)(le16_t *, u32, const u8 *, const gpu_unai_inner_t &);

static void run_sprite_case(const char *name, spritefn fn, int trial, u32 mask8)
{
	fill_random();
	gpu_unai_inner_t &inn = gpu_unai.inn;
	memset(&inn, 0, sizeof(inn));
	u32 count = 1 + rnd() % 40;
	inn.mask_v00u = (0xffu << 24) | 0xffu;
	inn.u = rnd() % 256;
	inn.v = rnd() % 256;
	inn.y0 = rnd() % 4;
	inn.y1 = inn.y0 + 1 + rnd() % 8;
	inn.CBA = clut;
	inn.bgr0888 = rnd() & 0xffffff;
	u32 dst_off = rnd() % (FRAME_WIDTH - 64);

	pskip_mask2 = 0;
	fn(&bufA[dst_off], count, tex, inn);
	pskip_mask2 = mask8 | (mask8 << 8);
	fn(&bufB[dst_off], count, tex, inn);
	pskip_mask2 = 0;
	check(name, trial, mask8);
}

int main(void)
{
	const u32 masks[2] = { 0xa4, 0xaa };   /* 512-wide, 640-wide */
	const int TRIALS = 4000;
	for (int t = 0; t < TRIALS; t++) {
		u32 m = masks[rnd() & 1];
		/* hot MXU poly kernels from the Crash slot-0 histogram */
		run_poly_case("mxu 0xa1", mxu_poly_thunk<1, true,  false, -1>, t, m);
		run_poly_case("mxu 0xc1", mxu_poly_thunk<2, true,  false, -1>, t, m);
		run_poly_case("mxu 0xa3", mxu_poly_thunk<1, true,  false,  0>, t, m);
		run_poly_case("mxu 0x33", mxu_poly_thunk<1, false, false,  2>, t, m);
		run_poly_case("mxu 0x41", mxu_poly_thunk<2, false, false, -1>, t, m);
		run_poly_case("mxu 0x1c1", mxu_poly_thunk<2, true,  true, -1>, t, m);
		/* scalar textured poly spans (short-span / uncovered-CF fallback) */
		run_poly_case("scal 0xa1", gpuPolySpanFn<0x0a1>, t, m);
		run_poly_case("scal 0xc1", gpuPolySpanFn<0x0c1>, t, m);
		run_poly_case("scal 0x40", gpuPolySpanFn<0x040>, t, m);
		run_poly_case("scal 0x33", gpuPolySpanFn<0x033>, t, m);
		/* scalar sprites (SPR 0x042 is the big one in the Crash scene) */
		run_sprite_case("spr 0x42", gpuSpriteDriverFn<0x042>, t, m);
		run_sprite_case("spr 0x40", gpuSpriteDriverFn<0x040>, t, m);
		run_sprite_case("spr 0x21", gpuSpriteDriverFn<0x021>, t, m);
	}
	if (fails == 0)
		printf("PASS: %d trials x 13 cases, masks 0xa4/0xaa, all phases\n", TRIALS);
	return fails != 0;
}
