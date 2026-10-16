/***************************************************************************
*   MXU (Ingenic XBurst SIMD) poly span fillers for the X1000 (erosq).
*
*   Mirrors the ARM PolySpanMaybeAsm pattern: a dispatcher covers the hot
*   CF variants with 2-pixels-per-iteration MXU code and falls back to the
*   C template for everything else. Bit-exact vs the LightLUT scalar path
*   (validated on silicon: see the mxutest harness, 2026-07-14).
*
*   Covered (no blend, no mask, no dither):
*     0x081                untextured gouraud
*     0x021 / 0x041 / 0x061  flat-lit textured 4/8/16bpp
*     0x0a1 / 0x0c1 / 0x0e1  gouraud-lit textured 4/8/16bpp
*
*   gpu_unai's gouraud accumulator (gcol_t) update on non-ARM C uses u32
*   adds that can carry-spill between the r and g lanes; MXU Q16ADD is
*   per-lane clean, matching the ARM uadd16 path instead. That is the
*   intended semantics per the comment in gpu_unai.h.
*
*   MXU ALU ops silently do nothing unless MXU_CR (xr16) bit 0 is set, and
*   the kernel does not context-switch XR state — so each span function
*   re-enables MXU on entry (1 cycle) and keeps no XR state across calls.
***************************************************************************/

#ifndef __GPU_UNAI_GPU_INNER_MXU_H__
#define __GPU_UNAI_GPU_INNER_MXU_H__

#include "mxu_x1000.h"
#include "gpu_blend_swar.h"

static int mxu_spans_on = 1;      /* EROSQ_MXU=0 disables at runtime (A/B) */
static int pskip_on = 1;          /* EROSQ_PSKIP=0 disables at runtime (A/B) */
static int prelight_mode = 3;     /* EROSQ_PRELIGHT bit0=polys bit1=sprites (A/B) */
/* Hi-res horizontal column-skip mask (pcsx4all "blit_mask" lineage). Nonzero
 * only in 512/640-wide modes: bit (vram_column & 7) set = present_scaled's
 * nearest-neighbour xmap (x*hres/320) never samples that column, so textured
 * span/sprite kernels skip its texel fetch and store. Displayed output stays
 * pixel-identical for drawn prims; only VRAM columns nothing reads differ.
 * Doubled to 16 bits so a lane-1 lookup at phase 7 reads bit 8, not garbage. */
static u32 pskip_mask2 = 0;
static int gpu_hist_on = 0;       /* EROSQ_GPU_HIST=1 + SIGUSR1 dumps */
static unsigned long long gpu_hist_span[1024], gpu_hist_px[1024];
/* sprites/tiles were invisible to the poly-span histogram (their pixels are
 * 100% scalar C on MIPS, and prim_try_simplify_quad_t silently converts
 * axis-aligned textured quads into the sprite path) — count them too.
 * Indexed by the sprite/tile CF space; px is width*lines (ignores the rare
 * interlace line skip, fine for share measurement). */
static unsigned long long gpu_hist_sprite_prim[1024], gpu_hist_sprite_px[1024];
static unsigned long long gpu_hist_tile_prim[1024], gpu_hist_tile_px[1024];

#ifdef GPU_MXU_HOST_TEST
/* Host-test models of the two asm cores, with the exact semantics the
 * silicon versions were validated against (mxutest harness 2026-07-14):
 * lighting = LightLUT per channel with lanes from host_xr[7..9], blend
 * mode 2 = per-lane mod-2^16 Blargg. test_sprite_tile_kernels.cpp
 * pre-defines host_xr[] and stubs the raw MXU_* macros, then diffs the
 * sprite/tile kernels against the scalar drivers on the host. */
static inline u32 mxu_light_pair(u32 tw)
{
	u32 out = 0;
	for (int lane = 0; lane < 2; lane++) {
		u32 t  = (tw >> (lane*16)) & 0xffff;
		u32 r5 = (host_xr[7] >> (lane*16)) & 0x1f;
		u32 g5 = (host_xr[8] >> (lane*16)) & 0x1f;
		u32 b5 = (host_xr[9] >> (lane*16)) & 0x1f;
		u32 s = ((u32)gpu_unai.LightLUT[((t & 0x7c00) >> 5) | b5] << 10)
		      | ((u32)gpu_unai.LightLUT[ (t & 0x03e0)       | g5] << 5)
		      |  (u32)gpu_unai.LightLUT[((t & 0x001f) << 5) | r5]
		      | (t & 0x8000);
		out |= s << (lane*16);
	}
	return out;
}
static inline u32 mxu_blend_pair_sub(u32 uSrc, u32 uDst)
{
	u32 out = 0;
	for (int lane = 0; lane < 2; lane++) {
		u32 s = (uSrc >> (lane*16)) & 0x7fff, d = (uDst >> (lane*16)) & 0x7fff;
		u32 diff    = (d - s + 0x8420) & 0xffff;
		u32 low     = (d ^ s) & 0x8420;
		u32 borrows = (diff - low) & 0x8420;
		u32 modulo  = (diff - borrows) & 0xffff;
		u32 clamp   = (borrows - (borrows >> 5)) & 0xffff;
		out |= (modulo & clamp) << (lane*16);
	}
	return out;
}
#else /* !GPU_MXU_HOST_TEST — the real silicon cores (light here, blend-2
       * further down; the gouraud helpers between them are raw-macro-only
       * and stay common, compiled against the test's stub macros) */
/* Shared 2-pixel lighting core. Texel pair in xr10, 5-bit light lanes in
 * xr7/8/9 (r/g/b), xr14 = 0x001f001f, xr15 = 0x80008000.
 * Returns lit bgr555 pair; equals LightLUT[t5*32+l5] per channel. */
/* Op order is hand-scheduled for the X1000 interlocks (PM section 2.3):
 * a D16MADL result read by the next / 2nd-next SIMD op costs +2 / +1, a
 * D16MIN result read by the next op +1. gcc cannot reorder these (every
 * MXU macro is asm volatile), so: the texel mask-bit AND (independent)
 * sits between the last MADL and the first SAR, and the MINs run g, b, r
 * so the SLL never reads the MIN just before it. Same 19 MXU ops and the
 * same dataflow as before (XR10 is consumed by the mask AND instead of a
 * temp), so bit-identical; ~3 interlock cycles fewer per pixel pair. */
static inline u32 mxu_light_pair(u32 tw)
{
	MXU_S32I2M(MXU_XR10, tw);
	MXU_S32AND(MXU_XR11, MXU_XR10, MXU_XR14);
	MXU_Q16SLR(MXU_XR12, MXU_XR10, MXU_XR0, MXU_XR0, 5);
	MXU_Q16SLR(MXU_XR13, MXU_XR10, MXU_XR0, MXU_XR0, 10);
	MXU_S32AND(MXU_XR12, MXU_XR12, MXU_XR14);
	MXU_S32AND(MXU_XR13, MXU_XR13, MXU_XR14);
	MXU_D16MADL(MXU_XR0, MXU_XR11, MXU_XR7, MXU_XR11, MXU_AA, MXU_WW);
	MXU_D16MADL(MXU_XR0, MXU_XR12, MXU_XR8, MXU_XR12, MXU_AA, MXU_WW);
	MXU_D16MADL(MXU_XR0, MXU_XR13, MXU_XR9, MXU_XR13, MXU_AA, MXU_WW);
	MXU_S32AND(MXU_XR10, MXU_XR10, MXU_XR15);           /* mask bit */
	MXU_Q16SAR(MXU_XR11, MXU_XR11, MXU_XR12, MXU_XR12, 4);
	MXU_Q16SAR(MXU_XR13, MXU_XR13, MXU_XR0, MXU_XR0, 4);
	MXU_D16MIN(MXU_XR12, MXU_XR12, MXU_XR14);
	MXU_D16MIN(MXU_XR13, MXU_XR13, MXU_XR14);
	MXU_D16MIN(MXU_XR11, MXU_XR11, MXU_XR14);
	MXU_Q16SLL(MXU_XR12, MXU_XR12, MXU_XR13, MXU_XR13, 5);
	MXU_Q16SLL(MXU_XR13, MXU_XR13, MXU_XR0, MXU_XR0, 5);
	MXU_S32OR(MXU_XR11, MXU_XR11, MXU_XR12);
	MXU_S32OR(MXU_XR11, MXU_XR11, MXU_XR13);
	MXU_S32OR(MXU_XR11, MXU_XR11, MXU_XR10);
	return MXU_S32M2I(MXU_XR11);
}
#endif /* GPU_MXU_HOST_TEST (light core) */

/* load gouraud pair-accumulators: xr1/2/3 = [c+dc : c], xr4/5/6 = [2dc : 2dc] */
#if defined(MXU_S32LDDO) && defined(MXU_S32SFL)
static_assert(offsetof(gpu_unai_inner_t, gInc) == offsetof(gpu_unai_inner_t, gCol) + 8,
	      "mxu_gouraud_setup loads gCol/gInc as 4 consecutive words");
/* EROSQ: built in XR from the packed gcol_t words (raw32[0] = g:r,
 * raw32[1] = x:b) with 4 loads, 4 per-lane adds and 4 halfword shuffles --
 * ~11 instructions instead of ~34 GPR pack ops + 6 S32I2M per gouraud span
 * (Crash averages ~9 px/span, so this is per-span fixed cost). Lane adds
 * are mod 2^16, exactly the (x & 0xffff) of the GPR version; XR7-13 are
 * free here (light lanes are rebuilt per pair from XR1-3). */
static inline void mxu_gouraud_setup(const gpu_unai_inner_t &inn)
{
	/* one base register for all four words (gCol/gInc are adjacent) */
	const char *base = (const char *)&inn.gCol;
	MXU_S32LDDO(MXU_XR10, base, 0);      /* g  : r  */
	MXU_S32LDDO(MXU_XR11, base, 4);      /* .. : b  */
	MXU_S32LDDO(MXU_XR12, base, 8);      /* dg : dr */
	MXU_S32LDDO(MXU_XR13, base, 12);     /* .. : db */
	MXU_Q16ADD(MXU_XR8, MXU_XR10, MXU_XR12, MXU_XR0, MXU_WW, MXU_AA);  /* g+dg : r+dr */
	MXU_Q16ADD(MXU_XR9, MXU_XR11, MXU_XR13, MXU_XR0, MXU_WW, MXU_AA);  /* ..   : b+db */
	MXU_Q16ADD(MXU_XR7, MXU_XR12, MXU_XR12, MXU_XR0, MXU_WW, MXU_AA);  /* 2dg  : 2dr  */
	MXU_Q16ADD(MXU_XR12, MXU_XR13, MXU_XR13, MXU_XR0, MXU_WW, MXU_AA); /* ..   : 2db  */
	MXU_S32SFL(MXU_XR2, MXU_XR8, MXU_XR10, MXU_XR1, 3);  /* [g+dg:g], [r+dr:r] */
	MXU_S32SFL(MXU_XR0, MXU_XR9, MXU_XR11, MXU_XR3, 3);  /*           [b+db:b] */
	MXU_S32SFL(MXU_XR5, MXU_XR7, MXU_XR7, MXU_XR4, 3);   /* [2dg:2dg], [2dr:2dr] */
	MXU_S32SFL(MXU_XR0, MXU_XR12, MXU_XR12, MXU_XR6, 3); /*           [2db:2db] */
}
#else
static inline void mxu_gouraud_setup(const gpu_unai_inner_t &inn)
{
	u32 r = inn.gCol.c.r, g = inn.gCol.c.g, b = inn.gCol.c.b;
	u32 dr = inn.gInc.c.r, dg = inn.gInc.c.g, db = inn.gInc.c.b;
	MXU_S32I2M(MXU_XR1, (((r + dr) & 0xffff) << 16) | r);
	MXU_S32I2M(MXU_XR2, (((g + dg) & 0xffff) << 16) | g);
	MXU_S32I2M(MXU_XR3, (((b + db) & 0xffff) << 16) | b);
	MXU_S32I2M(MXU_XR4, (((2 * dr) & 0xffff) << 16) | ((2 * dr) & 0xffff));
	MXU_S32I2M(MXU_XR5, (((2 * dg) & 0xffff) << 16) | ((2 * dg) & 0xffff));
	MXU_S32I2M(MXU_XR6, (((2 * db) & 0xffff) << 16) | ((2 * db) & 0xffff));
}
#endif

static inline void mxu_gouraud_step_pair(void)
{
	MXU_Q16ADD(MXU_XR1, MXU_XR1, MXU_XR4, MXU_XR0, MXU_WW, MXU_AA);
	MXU_Q16ADD(MXU_XR2, MXU_XR2, MXU_XR5, MXU_XR0, MXU_WW, MXU_AA);
	MXU_Q16ADD(MXU_XR3, MXU_XR3, MXU_XR6, MXU_XR0, MXU_WW, MXU_AA);
}

#ifndef GPU_MXU_HOST_TEST
/* Blend a packed 2-pixel pair per gpuBlendingGeneric<BM, false> semantics.
 * Modes 0/1/3 are cross-lane-safe in plain GPR SWAR (gpu_blend_swar.h,
 * host-tested bit-exact). Mode 2 computes dst - src + 0x8420 (17 bits per
 * lane) so it needs the MXU's true per-lane mod-2^16 lanes; the mod-2^16
 * wrap is exact because every consumer of 'diff' masks to bits <= 15.
 * Clobbers XR10..XR13 only (light lanes XR7-9 / consts XR14-15 / gouraud
 * accumulators XR1-6 survive). */
static inline u32 mxu_blend_pair_sub(u32 uSrc, u32 uDst)
{
	MXU_S32I2M(MXU_XR10, uSrc & 0x7fff7fff);
	MXU_S32I2M(MXU_XR11, uDst & 0x7fff7fff);
	MXU_S32I2M(MXU_XR12, 0x84208420);
	/* diff = dst - src + 0x8420, per lane */
	MXU_Q16ADD(MXU_XR13, MXU_XR11, MXU_XR10, MXU_XR0, MXU_WW, MXU_SS);
	MXU_Q16ADD(MXU_XR13, MXU_XR13, MXU_XR12, MXU_XR0, MXU_WW, MXU_AA);
	/* low_bits = (dst ^ src) & 0x8420 */
	MXU_S32XOR(MXU_XR10, MXU_XR10, MXU_XR11);
	MXU_S32AND(MXU_XR10, MXU_XR10, MXU_XR12);
	/* borrows = (diff - low_bits) & 0x8420 */
	MXU_Q16ADD(MXU_XR10, MXU_XR13, MXU_XR10, MXU_XR0, MXU_WW, MXU_SS);
	MXU_S32AND(MXU_XR10, MXU_XR10, MXU_XR12);
	/* modulo = diff - borrows */
	MXU_Q16ADD(MXU_XR13, MXU_XR13, MXU_XR10, MXU_XR0, MXU_WW, MXU_SS);
	/* clamp = borrows - (borrows >> 5) */
	MXU_Q16SLR(MXU_XR11, MXU_XR10, MXU_XR0, MXU_XR0, 5);
	MXU_Q16ADD(MXU_XR10, MXU_XR10, MXU_XR11, MXU_XR0, MXU_WW, MXU_SS);
	/* mix = modulo & clamp */
	MXU_S32AND(MXU_XR13, MXU_XR13, MXU_XR10);
	return MXU_S32M2I(MXU_XR13);
}
#endif /* GPU_MXU_HOST_TEST */

/* BM dispatch for pair blends; BM is clamped so BM==-1 (no blend) callers
 * never instantiate an undefined gpuBlendingGeneric<-1> body. */
template<int BM>
static inline u32 mxu_blend_pair(u32 uSrc, u32 uDst)
{
	if (BM == 2)
		return mxu_blend_pair_sub(uSrc, uDst);
	return gpuBlendPairSWAR<(BM < 0 ? 0 : BM)>(uSrc, uDst);
}

/* Single-pixel blend for span heads/tails — scalar reference directly. */
template<int BM>
static inline uint_fast16_t mxu_blend_one(uint_fast16_t uSrc, uint_fast16_t uDst)
{
	if (BM < 0)
		return uSrc;
	return gpuBlendingGeneric<(BM < 0 ? 0 : BM), false>(uSrc, uDst);
}

/* CF 0x000 (+0x100 MASKSET twin): untextured flat fill, no blend/mask —
 * 24% of Crash's pixels. Word stores via MXU (avoids u16*->u32*
 * strict-aliasing UB). */
template<bool MSET>
static void poly_mxu_flat_fill(const gpu_unai_t &gpu_unai, le16_t *pDst, u32 count)
{
	u32 pix = gpu_unai.inn.PixelData;
	if (MSET) pix |= 0x8000;
	MXU_S32I2M(MXU_XR16, 1);
	MXU_S32I2M(MXU_XR1, pix | (pix << 16));
	if ((uintptr_t)pDst & 2) {
		*pDst++ = u16_to_le16(pix);
		count--;
	}
	for (; count >= 2; count -= 2) {
		MXU_S32STD(MXU_XR1, pDst);
		pDst += 2;
	}
	if (count)
		*pDst = u16_to_le16(pix);
}

/* CF 0x081 (+0x181 MASKSET twin): untextured gouraud, no blend/mask/dither */
template<bool MSET>
static void poly_mxu_utx_gouraud(const gpu_unai_t &gpu_unai, le16_t *pDst, u32 count)
{
	const gpu_unai_inner_t &inn = gpu_unai.inn;
	u32 r = inn.gCol.c.r, g = inn.gCol.c.g, b = inn.gCol.c.b;

	MXU_S32I2M(MXU_XR16, 1);
	if ((uintptr_t)pDst & 2) {
		*pDst++ = u16_to_le16(((r >> 11) | ((g >> 6) & 0x3e0) | ((b >> 1) & 0x7c00))
		                      | (MSET ? 0x8000 : 0));
		r = (u16)(r + inn.gInc.c.r);
		g = (u16)(g + inn.gInc.c.g);
		b = (u16)(b + inn.gInc.c.b);
		if (!--count) return;
	}
	u32 dr = inn.gInc.c.r, dg = inn.gInc.c.g, db = inn.gInc.c.b;
	MXU_S32I2M(MXU_XR1, (((r + dr) & 0xffff) << 16) | r);
	MXU_S32I2M(MXU_XR2, (((g + dg) & 0xffff) << 16) | g);
	MXU_S32I2M(MXU_XR3, (((b + db) & 0xffff) << 16) | b);
	MXU_S32I2M(MXU_XR4, (((2 * dr) & 0xffff) << 16) | ((2 * dr) & 0xffff));
	MXU_S32I2M(MXU_XR5, (((2 * dg) & 0xffff) << 16) | ((2 * dg) & 0xffff));
	MXU_S32I2M(MXU_XR6, (((2 * db) & 0xffff) << 16) | ((2 * db) & 0xffff));
	MXU_S32I2M(MXU_XR14, 0x03e003e0);
	MXU_S32I2M(MXU_XR15, 0x7c007c00);
	if (MSET)
		MXU_S32I2M(MXU_XR10, 0x80008000);

	while (count >= 2) {
		MXU_Q16SLR(MXU_XR7, MXU_XR1, MXU_XR0, MXU_XR0, 11);
		MXU_Q16SLR(MXU_XR8, MXU_XR2, MXU_XR0, MXU_XR0, 6);
		MXU_Q16SLR(MXU_XR9, MXU_XR3, MXU_XR0, MXU_XR0, 1);
		MXU_S32AND(MXU_XR8, MXU_XR8, MXU_XR14);
		MXU_S32AND(MXU_XR9, MXU_XR9, MXU_XR15);
		MXU_S32OR(MXU_XR7, MXU_XR7, MXU_XR8);
		MXU_S32OR(MXU_XR7, MXU_XR7, MXU_XR9);
		if (MSET)
			MXU_S32OR(MXU_XR7, MXU_XR7, MXU_XR10);
		MXU_S32STD(MXU_XR7, pDst);
		MXU_Q16ADD(MXU_XR1, MXU_XR1, MXU_XR4, MXU_XR0, MXU_WW, MXU_AA);
		MXU_Q16ADD(MXU_XR2, MXU_XR2, MXU_XR5, MXU_XR0, MXU_WW, MXU_AA);
		MXU_Q16ADD(MXU_XR3, MXU_XR3, MXU_XR6, MXU_XR0, MXU_WW, MXU_AA);
		pDst += 2;
		count -= 2;
	}
	if (count) {
		u32 lr = MXU_S32M2I(MXU_XR1) & 0xffff;
		u32 lg = MXU_S32M2I(MXU_XR2) & 0xffff;
		u32 lb = MXU_S32M2I(MXU_XR3) & 0xffff;
		*pDst = u16_to_le16(((lr >> 11) | ((lg >> 6) & 0x3e0) | ((lb >> 1) & 0x7c00))
		                    | (MSET ? 0x8000 : 0));
	}
}

/* Untextured BLEND spans: flat (GOU=0, CF 0x002|BM<<3) and gouraud
 * (GOU=1, CF 0x083|BM<<3), + MASKSET twins (|0x100). Scalar semantics
 * (gpuPolySpanFn untextured branches): source color has bit15 clear by
 * construction (GPU_RGB16 / gpuLightingRGB top out at bit14), every pixel
 * blends with dst unconditionally, every pixel is stored (no MASKCHECK). */
template<bool GOU, int BM, bool MSET>
static void poly_mxu_utx_blend(const gpu_unai_t &gpu_unai, le16_t *pDst, u32 count)
{
	const gpu_unai_inner_t &inn = gpu_unai.inn;
	u32 r = 0, g = 0, b = 0, pix = 0;
	if (GOU) { r = inn.gCol.c.r; g = inn.gCol.c.g; b = inn.gCol.c.b; }
	else       pix = inn.PixelData;

	MXU_S32I2M(MXU_XR16, 1);

	/* scalar single pixel (head/tail); steps the gouraud accumulators */
#define UTXB_ONE() do { \
		uint_fast16_t s_ = GOU \
			? ((r >> 11) | ((g >> 6) & 0x3e0) | ((b >> 1) & 0x7c00)) \
			: pix; \
		s_ = mxu_blend_one<BM>(s_, le16_to_u16(*pDst)); \
		*pDst = u16_to_le16((u16)(MSET ? (s_ | 0x8000) : s_)); \
		pDst++; \
		if (GOU) { \
			r = (u16)(r + inn.gInc.c.r); \
			g = (u16)(g + inn.gInc.c.g); \
			b = (u16)(b + inn.gInc.c.b); \
		} \
	} while (0)

	if ((uintptr_t)pDst & 2) {
		UTXB_ONE();
		if (!--count) return;
	}

	u32 src_pair = pix | (pix << 16);
	if (GOU) {
		/* NOT mxu_gouraud_setup(inn): the head pixel may have stepped the
		 * local accumulators past inn.gCol — load from the locals. */
		u32 dr = inn.gInc.c.r, dg = inn.gInc.c.g, db = inn.gInc.c.b;
		MXU_S32I2M(MXU_XR1, (((r + dr) & 0xffff) << 16) | r);
		MXU_S32I2M(MXU_XR2, (((g + dg) & 0xffff) << 16) | g);
		MXU_S32I2M(MXU_XR3, (((b + db) & 0xffff) << 16) | b);
		MXU_S32I2M(MXU_XR4, (((2 * dr) & 0xffff) << 16) | ((2 * dr) & 0xffff));
		MXU_S32I2M(MXU_XR5, (((2 * dg) & 0xffff) << 16) | ((2 * dg) & 0xffff));
		MXU_S32I2M(MXU_XR6, (((2 * db) & 0xffff) << 16) | ((2 * db) & 0xffff));
		MXU_S32I2M(MXU_XR14, 0x03e003e0);
		MXU_S32I2M(MXU_XR15, 0x7c007c00);
	}

	while (count >= 2) {
		if (GOU) {
			MXU_Q16SLR(MXU_XR7, MXU_XR1, MXU_XR0, MXU_XR0, 11);
			MXU_Q16SLR(MXU_XR8, MXU_XR2, MXU_XR0, MXU_XR0, 6);
			MXU_Q16SLR(MXU_XR9, MXU_XR3, MXU_XR0, MXU_XR0, 1);
			MXU_S32AND(MXU_XR8, MXU_XR8, MXU_XR14);
			MXU_S32AND(MXU_XR9, MXU_XR9, MXU_XR15);
			MXU_S32OR(MXU_XR7, MXU_XR7, MXU_XR8);
			MXU_S32OR(MXU_XR7, MXU_XR7, MXU_XR9);
			src_pair = MXU_S32M2I(MXU_XR7);
			mxu_gouraud_step_pair();
		}
		u32 dst;
		memcpy(&dst, pDst, 4);  /* word-aligned after head; sw/lw via memcpy
		                           sidesteps le16_t* aliasing UB */
		u32 out = mxu_blend_pair<BM>(src_pair, dst);
		if (MSET) out |= 0x80008000;
		memcpy(pDst, &out, 4);
		pDst += 2;
		count -= 2;
	}
	if (count) {
		if (GOU) {  /* pull live accumulators back for the scalar tail */
			r = MXU_S32M2I(MXU_XR1) & 0xffff;
			g = MXU_S32M2I(MXU_XR2) & 0xffff;
			b = MXU_S32M2I(MXU_XR3) & 0xffff;
		}
		UTXB_ONE();
	}
#undef UTXB_ONE
}

/* textured fetch, one texel; TM = CF_TEXTMODE. Copied verbatim from
 * gpuPolySpanFn's textured branch (l_v is the pre-<<1 form). */
template<int TM>
static inline uint_fast16_t mxu_fetch_texel(u32 l_u, u32 l_v, u32 mask_v00u,
	const le16_t *TBA_, const le16_t *CBA_)
{
	if (TM == 1) {
		u32 tu = (l_u >> 10) & mask_v00u;
		u32 tv =  l_v & (mask_v00u >> 13);
		u8 rgb = ((const u8 *)TBA_)[tv + (tu >> 1)];
		return le16_to_u16(CBA_[(rgb >> ((tu & 1) << 2)) & 0xf]);
	}
	if (TM == 2) {
		u32 tu = (l_u >> 10) & mask_v00u;
		u32 tv =  l_v & (mask_v00u >> 13);
		return le16_to_u16(CBA_[((const u8 *)TBA_)[tv + tu]]);
	}
	/* TM == 3, 16bpp */
	{
		u32 tu = (l_u >> 10) & mask_v00u;
		u32 tv = (l_v >> 1) & (mask_v00u >> 14);
		return le16_to_u16(TBA_[tv + tu]);
	}
}

/* CF 0x021/0x041/0x061 (GOURAUD=0) and 0x0a1/0x0c1/0x0e1 (GOURAUD=1):
 * lit textured, no maskcheck/dither. MSET=CF_MASKSET (bit8): force the
 * PS1 mask bit (0x8000) set on every written pixel, exactly as the scalar path
 * does (uSrc | 0x8000) — cheap OR, unlocks the 0x1a1/0x121/0x141 buckets.
 * BM = CF_BLENDMODE when CF_BLEND (bit1) is set, else -1: per scalar
 * semantics, only texels whose LIT result has bit15 (the texture mask bit,
 * preserved by lighting) blend with dst; others store the lit color as-is.
 * The per-lane select uses the SWAR sign-mask trick (gpuMaskLanesSWAR). */
template<int TM, bool GOU, bool MSET, int BM = -1>
static void poly_mxu_tx_lit(const gpu_unai_t &gpu_unai, le16_t *pDst, u32 count)
{
	const gpu_unai_inner_t &inn = gpu_unai.inn;
	u32 l_u = inn.u,           l_v = inn.v << 1;
	s32 l_u_inc = inn.u_inc,   l_v_inc = inn.v_inc << 1;
	u32 mask_v00u = inn.mask_v00u;
	const le16_t *TBA_ = inn.TBA;
	const le16_t *CBA_ = inn.CBA;

	MXU_S32I2M(MXU_XR16, 1);
	MXU_S32I2M(MXU_XR14, 0x001f001f);
	MXU_S32I2M(MXU_XR15, 0x80008000);
	if (GOU) {
		mxu_gouraud_setup(inn);
	} else {
		u32 bgr = inn.bgr0888;
		u32 r5 = (bgr >> 3) & 0x1f, g5 = (bgr >> 11) & 0x1f, b5 = bgr >> 19;
		MXU_S32I2M(MXU_XR7, (r5 << 16) | r5);
		MXU_S32I2M(MXU_XR8, (g5 << 16) | g5);
		MXU_S32I2M(MXU_XR9, (b5 << 16) | b5);
	}

	const u32 bm2 = pskip_mask2;
	while (count >= 2) {
		uint_fast16_t t0 = 0, t1 = 0;
		/* hi-res column skip: never fetch a column present won't sample;
		 * lane stays 0 so the existing t0/t1 store guards suppress it */
		u32 sk = unlikely(bm2) ? (bm2 >> (((uintptr_t)pDst >> 1) & 7)) : 0;
		if (!(sk & 1))
			t0 = mxu_fetch_texel<TM>(l_u, l_v, mask_v00u, TBA_, CBA_);
		l_u += l_u_inc; l_v += l_v_inc;
		if (!(sk & 2))
			t1 = mxu_fetch_texel<TM>(l_u, l_v, mask_v00u, TBA_, CBA_);
		l_u += l_u_inc; l_v += l_v_inc;

		if (GOU) {
			MXU_Q16SLR(MXU_XR7, MXU_XR1, MXU_XR3, MXU_XR9, 11);
			MXU_Q16SLR(MXU_XR8, MXU_XR2, MXU_XR0, MXU_XR0, 11);
		}
		if (likely(t0 | t1)) {   /* (the CFG differs from the PGO profile
					  * now, so say which side is hot) */
			u32 res = mxu_light_pair(((u32)t1 << 16) | t0);
			/* step here, not after the stores: the 3 Q16ADDs fill the
			 * S32M2I->GPR latency (+3/+2/+1) of 'res'. The light lanes
			 * XR7-9 were already taken from XR1-3 above, and blend and
			 * the stores never touch XR1-6, so this is bit-identical. */
			if (GOU)
				mxu_gouraud_step_pair();
			if (BM >= 0) {
				u32 m = res & 0x80008000;
				if (m) {
					/* pDst may be 2-aligned only: two lhu, not lw */
					u32 dst = ((u32)le16_to_u16(pDst[1]) << 16)
					        |       le16_to_u16(pDst[0]);
					u32 bl  = mxu_blend_pair<BM>(res, dst) | 0x80008000;
					u32 sel = gpuMaskLanesSWAR(res);
					res = (bl & sel) | (res & ~sel);
				}
			}
			if (MSET) res |= 0x80008000;
			if (t0) pDst[0] = u16_to_le16((u16)res);
			if (t1) pDst[1] = u16_to_le16((u16)(res >> 16));
		} else if (GOU) {
			mxu_gouraud_step_pair();
		}
		pDst += 2;
		count -= 2;
	}
	if (count) { /* odd tail, scalar via LightLUT (bit-identical) */
		if (unlikely(bm2) && ((bm2 >> (((uintptr_t)pDst >> 1) & 7)) & 1))
			return;
		uint_fast16_t t = mxu_fetch_texel<TM>(l_u, l_v, mask_v00u, TBA_, CBA_);
		if (t) {
			u32 lr, lg, lb;
			if (GOU) {
				lr = (MXU_S32M2I(MXU_XR1) & 0xffff) >> 11;
				lg = (MXU_S32M2I(MXU_XR2) & 0xffff) >> 11;
				lb = (MXU_S32M2I(MXU_XR3) & 0xffff) >> 11;
			} else {
				u32 bgr = inn.bgr0888;
				lr = (bgr >> 3) & 0x1f; lg = (bgr >> 11) & 0x1f; lb = bgr >> 19;
			}
			uint_fast16_t s =
				(gpu_unai.LightLUT[((t & 0x7c00) >> 5) | lb] << 10) |
				(gpu_unai.LightLUT[ (t & 0x03e0)       | lg] << 5) |
				(gpu_unai.LightLUT[((t & 0x001f) << 5) | lr]) |
				(t & 0x8000);
			if (BM >= 0 && (s & 0x8000))
				s = mxu_blend_one<BM>(s, le16_to_u16(*pDst)) | 0x8000;
			if (MSET) s |= 0x8000;
			*pDst = u16_to_le16((u16)s);
		}
	}
}

/* ------------------------------------------------------------------
 * Sprite fast paths (CF 0x061/0x161 lit, 0x063/0x163 lit+blend-0).
 * Sprites walk u+1 per pixel, so 16bpp texels are plain sequential lhu
 * pairs — no gather, unlike poly spans — and the LightLUT cost (3
 * lookups/px) collapses into mxu_light_pair (bit-exact, silicon-
 * validated). Caller guarantees the whole row stays inside the texture
 * window (contiguous u mask, no wrap); v wrap is handled per row.
 * MGS attract measured these CFs at 98.7M px/125s, 100% scalar before.
 * TM: 2 = 8bpp CLUT, 3 = 16bpp (instantiate per histogram only). */
template<int TM, int BM, bool MSET>
static void sprite_mxu_lit(le16_t *pPixel, u32 count, const u8 *pTxt_base,
	const gpu_unai_inner_t &inn)
{
	u32 bgr = inn.bgr0888;
	u32 r5 = (bgr >> 3) & 0x1f, g5 = (bgr >> 11) & 0x1f, b5 = bgr >> 19;
	MXU_S32I2M(MXU_XR16, 1);
	MXU_S32I2M(MXU_XR14, 0x001f001f);
	MXU_S32I2M(MXU_XR15, 0x80008000);
	MXU_S32I2M(MXU_XR7, (r5 << 16) | r5);
	MXU_S32I2M(MXU_XR8, (g5 << 16) | g5);
	MXU_S32I2M(MXU_XR9, (b5 << 16) | b5);

	const u32 u_base = inn.u & (inn.mask_v00u & 0xff);
	const u32 v_mask = inn.mask_v00u >> 24;
	const le16_t *CBA_; if (TM != 3) CBA_ = inn.CBA;
	s32 y0 = inn.y0, y1 = inn.y1, li = inn.ilace_mask;
	u32 v0 = inn.v;

	for (; y0 < y1; ++y0, pPixel += FRAME_WIDTH, ++v0) {
		if (y0 & li) continue;
		const u8 *pT = pTxt_base + (v0 & v_mask) * 2048
		             + (TM == 3 ? u_base * 2 : u_base);
		le16_t *pDst = pPixel;
		u32 n = count;
		for (; n >= 2; n -= 2, pDst += 2) {
			u32 t0, t1;
			if (TM == 3) {
				t0 = le16_to_u16(((const le16_t *)pT)[0]);
				t1 = le16_to_u16(((const le16_t *)pT)[1]);
				pT += 4;
			} else { /* TM == 2, 8bpp CLUT */
				t0 = le16_to_u16(CBA_[pT[0]]);
				t1 = le16_to_u16(CBA_[pT[1]]);
				pT += 2;
			}
			if (!(t0 | t1)) continue;
			u32 res = mxu_light_pair((t1 << 16) | t0);
			if (BM >= 0) {
				u32 m = res & 0x80008000;
				if (m) {
					/* pDst may be 2-aligned only: two lhu, not lw */
					u32 dst = ((u32)le16_to_u16(pDst[1]) << 16)
					        |       le16_to_u16(pDst[0]);
					u32 bl  = mxu_blend_pair<BM>(res, dst) | 0x80008000;
					u32 sel = gpuMaskLanesSWAR(res);
					res = (bl & sel) | (res & ~sel);
				}
			}
			if (MSET) res |= 0x80008000;
			if (t0) pDst[0] = u16_to_le16((u16)res);
			if (t1) pDst[1] = u16_to_le16((u16)(res >> 16));
		}
		if (n) { /* odd width: scalar tail via LightLUT (bit-identical) */
			u32 t = (TM == 3) ? le16_to_u16(*(const le16_t *)pT)
			                  : le16_to_u16(CBA_[*pT]);
			if (t) {
				uint_fast16_t s =
					(gpu_unai.LightLUT[((t & 0x7c00) >> 5) | b5] << 10) |
					(gpu_unai.LightLUT[ (t & 0x03e0)       | g5] << 5) |
					(gpu_unai.LightLUT[((t & 0x001f) << 5) | r5]) |
					(t & 0x8000);
				if (BM >= 0 && (s & 0x8000))
					s = mxu_blend_one<BM>(s, le16_to_u16(*pDst)) | 0x8000;
				if (MSET) s |= 0x8000;
				*pDst = u16_to_le16((u16)s);
			}
		}
	}
}

/* 4bpp unlit sprites (CF 0x020/0x120): the scalar loop re-loads the same
 * texture byte for both nibbles — one lbu serves two texels here (the
 * measured 13→8 ins/px unroll). No MXU involved; sequential fetch only,
 * so it shares SpriteMaybeMXU's no-wrap precondition. Ape: 8.1M px. */
template<bool MSET>
static void sprite_fast_unlit_4bpp(le16_t *pPixel, u32 count, const u8 *pTxt_base,
	const gpu_unai_inner_t &inn)
{
	const u32 u_base = inn.u & (inn.mask_v00u & 0xff);
	const u32 v_mask = inn.mask_v00u >> 24;
	const le16_t *CBA_ = inn.CBA;
	s32 y0 = inn.y0, y1 = inn.y1, li = inn.ilace_mask;
	u32 v0 = inn.v;

	for (; y0 < y1; ++y0, pPixel += FRAME_WIDTH, ++v0) {
		if (y0 & li) continue;
		const u8 *pT = pTxt_base + (v0 & v_mask) * 2048 + (u_base >> 1);
		le16_t *pDst = pPixel;
		u32 n = count;
		if (u_base & 1) { /* head: odd u = high nibble of first byte */
			u32 t = le16_to_u16(CBA_[(*pT++ >> 4) & 0xf]);
			if (t) *pDst = u16_to_le16((u16)(MSET ? (t | 0x8000) : t));
			pDst++;
			if (!--n) continue;
		}
		for (; n >= 2; n -= 2, pDst += 2) {
			u32 b  = *pT++;
			u32 t0 = le16_to_u16(CBA_[b & 0xf]);
			u32 t1 = le16_to_u16(CBA_[(b >> 4) & 0xf]);
			if (t0) pDst[0] = u16_to_le16((u16)(MSET ? (t0 | 0x8000) : t0));
			if (t1) pDst[1] = u16_to_le16((u16)(MSET ? (t1 | 0x8000) : t1));
		}
		if (n) { /* tail: even u = low nibble */
			u32 t = le16_to_u16(CBA_[*pT & 0xf]);
			if (t) *pDst = u16_to_le16((u16)(MSET ? (t | 0x8000) : t));
		}
	}
}

/* forward decl; template defined below in gpu_inner.h */
template<int CF>
static noinline void gpuSpriteDriverFn(le16_t *pPixel, u32 count, const u8 *pTxt_base,
	const gpu_unai_inner_t &inn);

template<int CF>
static void SpriteMaybeMXU(le16_t *pPixel, u32 count, const u8 *pTxt_base,
	const gpu_unai_inner_t &inn)
{
	/* Fast path needs the whole row inside the texture window: the u
	 * mask must be contiguous low bits and (u & mask)+count-1 must not
	 * wrap — the scalar loop masks u per pixel, the kernel doesn't. */
	u32 u_mask = inn.mask_v00u & 0xff;
	if (mxu_spans_on && count >= 2 &&
	    (u_mask & (u_mask + 1)) == 0 &&
	    (inn.u & u_mask) + count - 1 <= u_mask) {
#define MXU_STAKE(fn) do { \
		if (unlikely(gpu_hist_on)) { \
			gpu_hist_sprite_prim[CF & 0x3ff]++; \
			gpu_hist_sprite_px[CF & 0x3ff] += \
				(unsigned long long)count * (inn.y1 > inn.y0 ? inn.y1 - inn.y0 : 0); \
		} \
		fn; return; \
	} while (0)
		switch (CF) {
		case 0x061: MXU_STAKE((sprite_mxu_lit<3, -1, false>(pPixel, count, pTxt_base, inn)));
		case 0x161: MXU_STAKE((sprite_mxu_lit<3, -1, true >(pPixel, count, pTxt_base, inn)));
		case 0x063: MXU_STAKE((sprite_mxu_lit<3,  0, false>(pPixel, count, pTxt_base, inn)));
		case 0x163: MXU_STAKE((sprite_mxu_lit<3,  0, true >(pPixel, count, pTxt_base, inn)));
		case 0x020: MXU_STAKE((sprite_fast_unlit_4bpp<false>(pPixel, count, pTxt_base, inn)));
		case 0x120: MXU_STAKE((sprite_fast_unlit_4bpp<true >(pPixel, count, pTxt_base, inn)));
		}
#undef MXU_STAKE
	}
	gpuSpriteDriverFn<CF>(pPixel, count, pTxt_base, inn);
}

/* forward decl; template defined below in gpu_inner.h */
template<int CF>
static noinline void gpuPolySpanFn(const gpu_unai_t &gpu_unai, le16_t *pDst, u32 count, s32 y);

template<int CF>
static void PolySpanMaybeMXU(const gpu_unai_t &gpu_unai, le16_t *pDst, u32 count, s32 y)
{
	/* ponytail: fixed count threshold — XR setup doesn't amortize on
	 * tiny spans; tune later if profiling says so (Crash gouraud spans
	 * average only ~6px, so keep this low) */
	if (mxu_spans_on && count >= 4) {
#define MXU_TAKE(fn) do { \
		if (unlikely(gpu_hist_on)) { \
			gpu_hist_span[CF & 0x3ff]++; \
			gpu_hist_px[CF & 0x3ff] += count; \
		} \
		fn; return; \
	} while (0)
		switch (CF) {
		case 0x000: MXU_TAKE((poly_mxu_flat_fill<false>(gpu_unai, pDst, count)));
		case 0x100: MXU_TAKE((poly_mxu_flat_fill<true >(gpu_unai, pDst, count)));
		case 0x081: MXU_TAKE((poly_mxu_utx_gouraud<false>(gpu_unai, pDst, count)));
		case 0x181: MXU_TAKE((poly_mxu_utx_gouraud<true >(gpu_unai, pDst, count)));
		case 0x021: MXU_TAKE((poly_mxu_tx_lit<1, false, false>(gpu_unai, pDst, count)));
		case 0x041: MXU_TAKE((poly_mxu_tx_lit<2, false, false>(gpu_unai, pDst, count)));
		case 0x061: MXU_TAKE((poly_mxu_tx_lit<3, false, false>(gpu_unai, pDst, count)));
		case 0x0a1: MXU_TAKE((poly_mxu_tx_lit<1, true,  false>(gpu_unai, pDst, count)));
		case 0x0c1: MXU_TAKE((poly_mxu_tx_lit<2, true,  false>(gpu_unai, pDst, count)));
		case 0x0e1: MXU_TAKE((poly_mxu_tx_lit<3, true,  false>(gpu_unai, pDst, count)));
		/* MASKSET (bit8) variants of the above — same kernels, force 0x8000.
		 * 0x1a1/0x121/0x141 dominate mask-heavy engines (MGS ~38% of pixels). */
		case 0x121: MXU_TAKE((poly_mxu_tx_lit<1, false, true>(gpu_unai, pDst, count)));
		case 0x141: MXU_TAKE((poly_mxu_tx_lit<2, false, true>(gpu_unai, pDst, count)));
		case 0x161: MXU_TAKE((poly_mxu_tx_lit<3, false, true>(gpu_unai, pDst, count)));
		case 0x1a1: MXU_TAKE((poly_mxu_tx_lit<1, true,  true>(gpu_unai, pDst, count)));
		case 0x1c1: MXU_TAKE((poly_mxu_tx_lit<2, true,  true>(gpu_unai, pDst, count)));
		case 0x1e1: MXU_TAKE((poly_mxu_tx_lit<3, true,  true>(gpu_unai, pDst, count)));
		/* BLEND (bit1) variants, CF = base | 0x002 | BM<<3 (+0x100 MASKSET).
		 * Textured-lit: SWAR pair blend (modes 0/1/3) / MXU lanes (mode 2),
		 * per-lane selected on the texture mask bit. MGS blend spans were
		 * 33% of its rasterized pixels, Ape Escape 18%, CTR 9%. */
#define MXU_TXB(base, TM, GOU) \
		case (base)|0x002: MXU_TAKE((poly_mxu_tx_lit<TM, GOU, false, 0>(gpu_unai, pDst, count))); \
		case (base)|0x00a: MXU_TAKE((poly_mxu_tx_lit<TM, GOU, false, 1>(gpu_unai, pDst, count))); \
		case (base)|0x012: MXU_TAKE((poly_mxu_tx_lit<TM, GOU, false, 2>(gpu_unai, pDst, count))); \
		case (base)|0x01a: MXU_TAKE((poly_mxu_tx_lit<TM, GOU, false, 3>(gpu_unai, pDst, count))); \
		case (base)|0x102: MXU_TAKE((poly_mxu_tx_lit<TM, GOU, true,  0>(gpu_unai, pDst, count))); \
		case (base)|0x10a: MXU_TAKE((poly_mxu_tx_lit<TM, GOU, true,  1>(gpu_unai, pDst, count))); \
		case (base)|0x112: MXU_TAKE((poly_mxu_tx_lit<TM, GOU, true,  2>(gpu_unai, pDst, count))); \
		case (base)|0x11a: MXU_TAKE((poly_mxu_tx_lit<TM, GOU, true,  3>(gpu_unai, pDst, count)));
		MXU_TXB(0x021, 1, false)
		MXU_TXB(0x041, 2, false)
		MXU_TXB(0x061, 3, false)
		MXU_TXB(0x0a1, 1, true)
		MXU_TXB(0x0c1, 2, true)
		MXU_TXB(0x0e1, 3, true)
#undef MXU_TXB
		/* Untextured blend: flat 0x002|BM<<3, gouraud 0x083|BM<<3 (+0x100) */
#define MXU_UTXB(base, GOU) \
		case (base):       MXU_TAKE((poly_mxu_utx_blend<GOU, 0, false>(gpu_unai, pDst, count))); \
		case (base)|0x008: MXU_TAKE((poly_mxu_utx_blend<GOU, 1, false>(gpu_unai, pDst, count))); \
		case (base)|0x010: MXU_TAKE((poly_mxu_utx_blend<GOU, 2, false>(gpu_unai, pDst, count))); \
		case (base)|0x018: MXU_TAKE((poly_mxu_utx_blend<GOU, 3, false>(gpu_unai, pDst, count))); \
		case (base)|0x100: MXU_TAKE((poly_mxu_utx_blend<GOU, 0, true >(gpu_unai, pDst, count))); \
		case (base)|0x108: MXU_TAKE((poly_mxu_utx_blend<GOU, 1, true >(gpu_unai, pDst, count))); \
		case (base)|0x110: MXU_TAKE((poly_mxu_utx_blend<GOU, 2, true >(gpu_unai, pDst, count))); \
		case (base)|0x118: MXU_TAKE((poly_mxu_utx_blend<GOU, 3, true >(gpu_unai, pDst, count)));
		MXU_UTXB(0x002, false)
		MXU_UTXB(0x083, true)
#undef MXU_UTXB
		}
#undef MXU_TAKE
	}
	gpuPolySpanFn<CF>(gpu_unai, pDst, count, y);
}

#endif /* __GPU_UNAI_GPU_INNER_MXU_H__ */
