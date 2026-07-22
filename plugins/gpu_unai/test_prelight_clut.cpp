/* EROSQ host differential test: flat-lit CLUT pre-lighting redirect.
 *
 * Property under test: for any CLUT/light-color where gpuPrelightCLUT16()
 * succeeds, rendering with the LIT driver on the raw CLUT is bit-identical
 * to rendering with the UNLIT twin driver (CF & ~1) on the pre-lit CLUT —
 * for poly spans and sprites, across blend modes and mask bits.  When the
 * builder bails, the caller keeps the lit driver (nothing to verify).
 *
 * Build & run (host, no deps):
 *   g++ -O2 -o /tmp/test_prelight plugins/gpu_unai/test_prelight_clut.cpp \
 *     && /tmp/test_prelight
 *
 * No __mips__/GPU_UNAI_USE_MXU here: both sides run the plain scalar
 * template bodies — exactly the code the redirect swaps between (the MXU
 * unlit sprite path is separately host-tested in test_sprite_tile_kernels).
 */
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
using std::ptrdiff_t;
using std::size_t;

#include "../gpulib/gpu.h"
#include "gpu_unai.h"
#include "gpu_fixedpoint.h"
#include "gpu_inner.h"
#include "gpu_prelight.h"

static u32 rng_state = 0x20260722;
static u32 rnd(void)
{
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 17;
	rng_state ^= rng_state << 5;
	return rng_state;
}

#define TEX_ROWS 256
static u8 tex[TEX_ROWS * 2048];
static le16_t clut[16], prelit[16];
#define VRAM_ROWS 4
static le16_t bufA[VRAM_ROWS * FRAME_WIDTH], bufB[VRAM_ROWS * FRAME_WIDTH];
static int fails = 0;
static long redirects = 0, bails = 0;

static void fill_random(u32 bgr)
{
	for (int i = 0; i < 16; i++) {
		u16 v;
		switch (rnd() % 5) {
		case 0: v = 0; break;                     /* transparent */
		case 1: v = (u16)(rnd() & 0x1f); break;   /* tiny color: lights to 0 under dark light */
		case 2: v = (u16)(rnd() | 0x8000); break; /* STP set */
		default: v = (u16)rnd(); break;
		}
		clut[i] = u16_to_le16(v);
	}
	for (unsigned i = 0; i < VRAM_ROWS * FRAME_WIDTH; i++)
		bufA[i] = u16_to_le16((u16)rnd());
	memcpy(bufB, bufA, sizeof(bufA));

	gpu_unai_inner_t &inn = gpu_unai.inn;
	memset(&inn, 0, sizeof(inn));
	inn.TBA = (le16_t *)tex;
	inn.CBA = clut;
	inn.r8 = bgr & 0xff;
	inn.g8 = (bgr >> 8) & 0xff;
	inn.b8 = (bgr >> 16) & 0xff;
	u32 um = (rnd() & 1) ? 0xff : 0x3f;
	u32 vm = (rnd() & 1) ? 0xff : 0x3f;
	inn.mask_v00u = (vm << 24) | um;
}

static u32 pick_bgr(void)
{
	switch (rnd() % 4) {
	case 0: return 0x808080;                       /* neutral */
	case 1: return rnd() & 0x0f0f0f;               /* dark: exercises bail */
	default: return rnd() & 0xffffff;
	}
}

typedef void (*PPfn)(const gpu_unai_t &, le16_t *, u32, s32);

template<int CF>
static void poly_pair_case(int trial)
{
	u32 bgr = pick_bgr();
	fill_random(bgr);
	gpu_unai_inner_t &inn = gpu_unai.inn;
	inn.u = (rnd() % 256) << 10;
	inn.v = (rnd() % 256) << 10;
	inn.u_inc = (s32)(rnd() % (3 << 10)) - (1 << 10);
	inn.v_inc = (s32)(rnd() % (2 << 10)) - (1 << 9);
	u32 count = 1 + rnd() % 100;
	u32 off = rnd() % (FRAME_WIDTH - 128) + FRAME_WIDTH; /* row 1: guard rows around */
	s32 y = rnd() % 512;

	gpuPolySpanFn<CF>(gpu_unai, &bufA[off], count, y);
	if (!gpuPrelightCLUT16(bgr & 0xffffff, clut, gpu_unai.LightLUT, prelit)) {
		bails++;
		return;
	}
	redirects++;
	inn.CBA = prelit;
	gpuPolySpanFn<CF & ~1>(gpu_unai, &bufB[off], count, y);

	if (memcmp(bufA, bufB, sizeof(bufA)) != 0) {
		for (unsigned i = 0; i < VRAM_ROWS * FRAME_WIDTH; i++)
			if (le16_raw(bufA[i]) != le16_raw(bufB[i])) {
				printf("FAIL poly CF %x trial %d: diff at %u lit %04x unlit %04x (bgr %06x)\n",
				       CF, trial, i, le16_to_u16(bufA[i]), le16_to_u16(bufB[i]), bgr);
				break;
			}
		fails++;
	}
}

template<int CF>
static void sprite_pair_case(int trial)
{
	u32 bgr = pick_bgr();
	fill_random(bgr);
	gpu_unai_inner_t &inn = gpu_unai.inn;
	inn.u = rnd() % 256;
	inn.v = rnd() % 256;
	inn.y0 = rnd() % 2;
	inn.y1 = inn.y0 + 1 + rnd() % 2;
	inn.ilace_mask = (rnd() & 3) == 0 ? 1 : 0;
	u32 count = 1 + rnd() % 100;
	u32 off = rnd() % (FRAME_WIDTH - 128);

	gpuSpriteDriverFn<CF>(&bufA[off], count, tex, inn);
	if (!gpuPrelightCLUT16(bgr & 0xffffff, clut, gpu_unai.LightLUT, prelit)) {
		bails++;
		return;
	}
	redirects++;
	inn.CBA = prelit;
	gpuSpriteDriverFn<CF & ~1>(&bufB[off], count, tex, inn);

	if (memcmp(bufA, bufB, sizeof(bufA)) != 0) {
		for (unsigned i = 0; i < VRAM_ROWS * FRAME_WIDTH; i++)
			if (le16_raw(bufA[i]) != le16_raw(bufB[i])) {
				printf("FAIL sprite CF %x trial %d: diff at %u lit %04x unlit %04x (bgr %06x)\n",
				       CF, trial, i, le16_to_u16(bufA[i]), le16_to_u16(bufB[i]), bgr);
				break;
			}
		fails++;
	}
}

int main(void)
{
	SetupLightLUT();
	for (unsigned i = 0; i < sizeof(tex); i++) tex[i] = (u8)rnd();

	for (int t = 0; t < 20000 && fails < 8; t++) {
		poly_pair_case<0x021>(t);   /* plain lit 4bpp */
		poly_pair_case<0x02b>(t);   /* + blend mode 1 (Ape/Spyro hot) */
		poly_pair_case<0x023>(t);   /* + blend mode 0 */
		poly_pair_case<0x033>(t);   /* + blend mode 2 */
		poly_pair_case<0x03b>(t);   /* + blend mode 3 */
		poly_pair_case<0x025>(t);   /* + maskcheck */
		poly_pair_case<0x121>(t);   /* + maskset */
		poly_pair_case<0x12f>(t);   /* blend+maskcheck+maskset */
		sprite_pair_case<0x021>(t); /* lit 4bpp sprite (MGS hot) */
		sprite_pair_case<0x02b>(t); /* + blend mode 1 */
		sprite_pair_case<0x121>(t); /* + maskset */
	}

	printf("prelight: %ld redirects, %ld bails, %d fails\n", redirects, bails, fails);
	if (fails || !redirects || !bails) {
		printf("prelight CLUT: FAILED%s\n",
		       fails ? "" : " (coverage: need both redirects and bails)");
		return 1;
	}
	printf("prelight CLUT OK\n");
	return 0;
}
