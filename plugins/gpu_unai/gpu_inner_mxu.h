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

static int mxu_spans_on = 1;      /* EROSQ_MXU=0 disables at runtime (A/B) */
static int gpu_hist_on = 0;       /* EROSQ_GPU_HIST=1 + SIGUSR1 dumps */
static unsigned long long gpu_hist_span[1024], gpu_hist_px[1024];

/* Shared 2-pixel lighting core. Texel pair in xr10, 5-bit light lanes in
 * xr7/8/9 (r/g/b), xr14 = 0x001f001f, xr15 = 0x80008000.
 * Returns lit bgr555 pair; equals LightLUT[t5*32+l5] per channel. */
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
	MXU_Q16SAR(MXU_XR11, MXU_XR11, MXU_XR12, MXU_XR12, 4);
	MXU_Q16SAR(MXU_XR13, MXU_XR13, MXU_XR0, MXU_XR0, 4);
	MXU_D16MIN(MXU_XR11, MXU_XR11, MXU_XR14);
	MXU_D16MIN(MXU_XR12, MXU_XR12, MXU_XR14);
	MXU_D16MIN(MXU_XR13, MXU_XR13, MXU_XR14);
	MXU_Q16SLL(MXU_XR12, MXU_XR12, MXU_XR13, MXU_XR13, 5);
	MXU_Q16SLL(MXU_XR13, MXU_XR13, MXU_XR0, MXU_XR0, 5);
	MXU_S32OR(MXU_XR11, MXU_XR11, MXU_XR12);
	MXU_S32OR(MXU_XR11, MXU_XR11, MXU_XR13);
	MXU_S32AND(MXU_XR12, MXU_XR10, MXU_XR15);
	MXU_S32OR(MXU_XR11, MXU_XR11, MXU_XR12);
	return MXU_S32M2I(MXU_XR11);
}

/* load gouraud pair-accumulators: xr1/2/3 = [c+dc : c], xr4/5/6 = [2dc : 2dc] */
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

static inline void mxu_gouraud_step_pair(void)
{
	MXU_Q16ADD(MXU_XR1, MXU_XR1, MXU_XR4, MXU_XR0, MXU_WW, MXU_AA);
	MXU_Q16ADD(MXU_XR2, MXU_XR2, MXU_XR5, MXU_XR0, MXU_WW, MXU_AA);
	MXU_Q16ADD(MXU_XR3, MXU_XR3, MXU_XR6, MXU_XR0, MXU_WW, MXU_AA);
}

/* CF 0x000: untextured flat fill, no blend/mask — 24% of Crash's pixels.
 * Word stores via MXU (avoids u16*->u32* strict-aliasing UB). */
static void poly_mxu_flat_fill(const gpu_unai_t &gpu_unai, le16_t *pDst, u32 count)
{
	u32 pix = gpu_unai.inn.PixelData;
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

/* CF 0x081: untextured gouraud, no blend/mask/dither */
static void poly_mxu_utx_gouraud(const gpu_unai_t &gpu_unai, le16_t *pDst, u32 count)
{
	const gpu_unai_inner_t &inn = gpu_unai.inn;
	u32 r = inn.gCol.c.r, g = inn.gCol.c.g, b = inn.gCol.c.b;

	MXU_S32I2M(MXU_XR16, 1);
	if ((uintptr_t)pDst & 2) {
		*pDst++ = u16_to_le16((r >> 11) | ((g >> 6) & 0x3e0) | ((b >> 1) & 0x7c00));
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

	while (count >= 2) {
		MXU_Q16SLR(MXU_XR7, MXU_XR1, MXU_XR0, MXU_XR0, 11);
		MXU_Q16SLR(MXU_XR8, MXU_XR2, MXU_XR0, MXU_XR0, 6);
		MXU_Q16SLR(MXU_XR9, MXU_XR3, MXU_XR0, MXU_XR0, 1);
		MXU_S32AND(MXU_XR8, MXU_XR8, MXU_XR14);
		MXU_S32AND(MXU_XR9, MXU_XR9, MXU_XR15);
		MXU_S32OR(MXU_XR7, MXU_XR7, MXU_XR8);
		MXU_S32OR(MXU_XR7, MXU_XR7, MXU_XR9);
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
		*pDst = u16_to_le16((lr >> 11) | ((lg >> 6) & 0x3e0) | ((lb >> 1) & 0x7c00));
	}
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
 * lit textured, no blend/mask/dither */
template<int TM, bool GOU>
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

	while (count >= 2) {
		uint_fast16_t t0 = mxu_fetch_texel<TM>(l_u, l_v, mask_v00u, TBA_, CBA_);
		l_u += l_u_inc; l_v += l_v_inc;
		uint_fast16_t t1 = mxu_fetch_texel<TM>(l_u, l_v, mask_v00u, TBA_, CBA_);
		l_u += l_u_inc; l_v += l_v_inc;

		if (GOU) {
			MXU_Q16SLR(MXU_XR7, MXU_XR1, MXU_XR3, MXU_XR9, 11);
			MXU_Q16SLR(MXU_XR8, MXU_XR2, MXU_XR0, MXU_XR0, 11);
		}
		if (t0 | t1) {
			u32 res = mxu_light_pair(((u32)t1 << 16) | t0);
			if (t0) pDst[0] = u16_to_le16((u16)res);
			if (t1) pDst[1] = u16_to_le16((u16)(res >> 16));
		}
		if (GOU)
			mxu_gouraud_step_pair();
		pDst += 2;
		count -= 2;
	}
	if (count) { /* odd tail, scalar via LightLUT (bit-identical) */
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
			*pDst = u16_to_le16(
				(gpu_unai.LightLUT[((t & 0x7c00) >> 5) | lb] << 10) |
				(gpu_unai.LightLUT[ (t & 0x03e0)       | lg] << 5) |
				(gpu_unai.LightLUT[((t & 0x001f) << 5) | lr]) |
				(t & 0x8000));
		}
	}
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
		case 0x000: MXU_TAKE(poly_mxu_flat_fill(gpu_unai, pDst, count));
		case 0x081: MXU_TAKE(poly_mxu_utx_gouraud(gpu_unai, pDst, count));
		case 0x021: MXU_TAKE((poly_mxu_tx_lit<1, false>(gpu_unai, pDst, count)));
		case 0x041: MXU_TAKE((poly_mxu_tx_lit<2, false>(gpu_unai, pDst, count)));
		case 0x061: MXU_TAKE((poly_mxu_tx_lit<3, false>(gpu_unai, pDst, count)));
		case 0x0a1: MXU_TAKE((poly_mxu_tx_lit<1, true>(gpu_unai, pDst, count)));
		case 0x0c1: MXU_TAKE((poly_mxu_tx_lit<2, true>(gpu_unai, pDst, count)));
		case 0x0e1: MXU_TAKE((poly_mxu_tx_lit<3, true>(gpu_unai, pDst, count)));
		}
#undef MXU_TAKE
	}
	gpuPolySpanFn<CF>(gpu_unai, pDst, count, y);
}

#endif /* __GPU_UNAI_GPU_INNER_MXU_H__ */
