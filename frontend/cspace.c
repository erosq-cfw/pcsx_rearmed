/*
 * (C) Gražvydas "notaz" Ignotas, 2011,2012,2022
 *
 * This work is licensed under the terms of any of these licenses
 * (at your option):
 *  - GNU GPL, version 2 or later.
 *  - GNU LGPL, version 2.1 or later.
 * See the COPYING file in the top-level directory.
 */

#include <stdint.h>
#include <string.h>
#include "cspace.h"
#include "compiler_features.h"

/*
 * note: these are intended for testing and should be avoided
 * in favor of NEON version or platform-specific conversion
 */

#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define SWAP16(x) __builtin_bswap16(x)
#define LE16TOH(x) SWAP16(x)
#define LE16TOHx2(x) ((SWAP16((x) >> 16) << 16) | SWAP16(x))
#define LE32TOH(x) __builtin_bswap32(x)
#else
#define LE16TOH(x) (x)
#define LE16TOHx2(x) (x)
#define LE32TOH(x) (x)
#endif

static inline uint32_t bgr555_to_rgb565_pair(uint32_t p)
{
	uint32_t r, g, b;
	r = (p & 0x001f001f) << 11;
	g = (p & 0x03e003e0) << 1;
	b = (p & 0x7c007c00) >> 10;
	return r | g | b;
}

static inline uint32_t bgr888_to_rgb565_pair(const uint8_t * __restrict__ src,
	int o0, int o1)
{
	uint32_t r1, g1, b1, r2, g2, b2;
	r1 = src[o0 + 0] & 0xf8;
	g1 = src[o0 + 1] & 0xfc;
	b1 = src[o0 + 2] & 0xf8;
	r2 = src[o1 + 0] & 0xf8;
	g2 = src[o1 + 1] & 0xfc;
	b2 = src[o1 + 2] & 0xf8;
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
	return (r1 << 24) | (g1 << 19) | (b1 << 13) |
	       (r2 << 8)  | (g2 << 3)  | (b2 >> 3);
#else
	return (r2 << 24) | (g2 << 19) | (b2 << 13) |
	       (r1 << 8)  | (g1 << 3)  | (b1 >> 3);
#endif
}

#if defined(HAVE_bgr555_to_rgb565)

/* have bgr555_to_rgb565 somewhere else */

#elif defined(HAVE_bgr555_to_rgb565_mxu)

/* EROSQ X1000 MXU1 path. GCC's generic vector extensions (the branch below)
 * scalarize on this no-SIMD MIPS core to ~19 insns/pixel; this hand kernel
 * runs the same bgr555 (a bbbbb ggggg rrrrr) -> rgb565 (rrrrr gggggg bbbbb)
 * transform at ~3.25 insns/pixel, 4 pixels per iteration across two XR regs.
 *
 * Per 16-bit lane, matching the scalar reference exactly:
 *     s1 = s << 1;  d = (s1 & 0x07c0) | (s << 11) | (s1 >> 11);
 * i.e. red = s<<11, green = (s<<1)&0x07c0 (LSB left 0, no replication),
 * blue = (s<<1)>>11. Bit-exact vs the reference (test_cspace_mxu.cpp).
 *
 * MXU S32LDD/S32STD move 32 bits (two little-endian pixels) and need 4-byte
 * alignment. dst is aligned with one leading scalar pixel; src can only be
 * aligned in lockstep when it shares dst's 2-byte parity, so a differing
 * parity takes the scalar path for the whole row (rare for these buffers). */
#include "../plugins/gpu_unai/mxu_x1000.h"

#define BGR555_ONE(dp, sp) do { \
	unsigned s_ = (sp); \
	unsigned d_ = (s_ << 1) & 0xffff; \
	(dp) = (uint16_t)((d_ & 0x07c0) | ((s_ << 11) & 0xffff) | (d_ >> 11)); \
} while (0)

void bgr555_to_rgb565(void * __restrict__ dst_, const void * __restrict__ src_,
	int pixels)
{
	const uint16_t * __restrict__ src = src_;
	uint16_t * __restrict__ dst = dst_;

	/* src and dst can only both reach 4-byte alignment if they share parity */
	if (((uintptr_t)src ^ (uintptr_t)dst) & 3) {
		for (; pixels > 0; pixels--, src++, dst++)
			BGR555_ONE(*dst, *src);
		return;
	}

	MXU_S32I2M(MXU_XR16, 1);           /* enable MXU ALU (silent-nop otherwise) */
	MXU_S32I2M(MXU_XR15, 0x07c007c0);  /* green mask, both lanes */

	if (((uintptr_t)dst & 2) && pixels > 0) {   /* one leading pixel -> 4-align */
		BGR555_ONE(*dst, *src);
		dst++; src++; pixels--;
	}

	for (; pixels >= 4; pixels -= 4, src += 4, dst += 4) {
		MXU_S32LDD(MXU_XR1, src);      /* pixels 0,1 */
		MXU_S32LDD(MXU_XR2, src + 2);  /* pixels 2,3 */
		MXU_Q16SLL(MXU_XR3, MXU_XR1, MXU_XR2, MXU_XR4, 1);   /* s1  = s << 1  */
		MXU_Q16SLL(MXU_XR5, MXU_XR1, MXU_XR2, MXU_XR6, 11);  /* red = s << 11 */
		MXU_Q16SLR(MXU_XR7, MXU_XR3, MXU_XR4, MXU_XR8, 11);  /* blue= s1 >> 11*/
		MXU_S32AND(MXU_XR9,  MXU_XR3, MXU_XR15);              /* green (0,1) */
		MXU_S32AND(MXU_XR10, MXU_XR4, MXU_XR15);              /* green (2,3) */
		MXU_S32OR(MXU_XR5, MXU_XR5, MXU_XR9);
		MXU_S32OR(MXU_XR5, MXU_XR5, MXU_XR7);                 /* dst 0,1 */
		MXU_S32OR(MXU_XR6, MXU_XR6, MXU_XR10);
		MXU_S32OR(MXU_XR6, MXU_XR6, MXU_XR8);                 /* dst 2,3 */
		MXU_S32STD(MXU_XR5, dst);
		MXU_S32STD(MXU_XR6, dst + 2);
	}

	for (; pixels > 0; pixels--, src++, dst++)
		BGR555_ONE(*dst, *src);
}
#undef BGR555_ONE

#elif ((defined(__clang_major__) && __clang_major__ >= 4) \
        || (defined(__GNUC__) && __GNUC__ >= 5)) \
       && __BYTE_ORDER__ != __ORDER_BIG_ENDIAN__

#include <assert.h>

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#define gsli(d_, s_, n_) d_ = vsliq_n_u16(d_, s_, n_)
#define gsri(d_, s_, n_) d_ = vsriq_n_u16(d_, s_, n_)
#else
#define gsli(d_, s_, n_) d_ |= s_ << n_
#define gsri(d_, s_, n_) d_ |= s_ >> n_
#endif

typedef uint16_t gvu16  __attribute__((vector_size(16),aligned(16)));
typedef uint16_t gvu16u __attribute__((vector_size(16),aligned(2)));
#define gdup(v_) {v_, v_, v_, v_, v_, v_, v_, v_}
#define do_one(s) ({ \
  uint16_t d_ = (s) << 1; d_ = (d_ & 0x07c0) | (d_ << 10) | (d_ >> 11); d_; \
})
#define do_one_simd(d_, s_, c0x07c0_) { \
  gvu16 s1_ = s_ << 1; \
  d_ = s1_ & c0x07c0_; \
  gsli(d_, s_, 11); \
  gsri(d_, s1_, 11); \
}

void bgr555_to_rgb565(void * __restrict__ dst_, const void *  __restrict__ src_,
	int pixels)
{
	const uint16_t * __restrict__ src = src_;
	uint16_t * __restrict__ dst = dst_;
	gvu16 c0x07c0 = gdup(0x07c0);
	uintptr_t pfc_ofs;

	assert(!(((uintptr_t)dst | (uintptr_t)src) & 1));

	// align the destination
	if ((uintptr_t)dst & 0x0e)
	{
		uintptr_t left = 0x10 - ((uintptr_t)dst & 0x0e);
		gvu16 d, s = *(const gvu16u *)src;
		do_one_simd(d, s, c0x07c0);
		*(gvu16u *)dst = d;
		dst += left / 2;
		src += left / 2;
		pixels -= left / 2;
	}
	pfc_ofs = (128 + 64 - ((uintptr_t)src & 63l)) / 2;
	// go
	for (; pixels >= 8*2; dst += 8*2, src += 8*2, pixels -= 8*2)
	{
		gvu16 d0, s0 = ((const gvu16u *)src)[0];
		gvu16 d1, s1 = ((const gvu16u *)src)[1];
		do_one_simd(d0, s0, c0x07c0);
		do_one_simd(d1, s1, c0x07c0);
		//asm("stnp %q0, %q1, [%2]" :: "w"(d0), "w"(d1), "r"(dst) : "memory");
		((gvu16 *)dst)[0] = d0;
		((gvu16 *)dst)[1] = d1;
		__builtin_prefetch(src + pfc_ofs);
	}
	// finish it
	for (; pixels > 0; dst++, src++, pixels--)
		*dst = do_one(*src);
}
#undef do_one
#undef do_one_simd

#else

void bgr555_to_rgb565(void * __restrict__ dst_, const void * __restrict__ src_,
	int pixels)
{
	// source can be misaligned, but it's very rare, so just force
	const uint32_t * __restrict__ src = (const void *)((intptr_t)src_ & ~3);
	uint32_t x, * __restrict__ dst = dst_;

	for (x = 0; x < pixels / 2; x++)
		dst[x] = bgr555_to_rgb565_pair(LE16TOHx2(src[x]));
}

#endif

static inline void bgr888_to_rgb888_one(uint8_t * __restrict__ dst,
	const uint8_t * __restrict__ src)
{
	dst[0] = src[2];
	dst[1] = src[1];
	dst[2] = src[0];
}

#ifndef HAVE_bgr888_to_x

void attr_weak bgr888_to_rgb565(void * __restrict__ dst_,
		const void * __restrict__ src_, int pixels)
{
	const uint8_t * __restrict__ src = src_;
	uint32_t * __restrict__ dst = dst_;

#if defined(__mips__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	//  EROSQ: this convert is 8-11% of frame time during 24bpp FMV and the
	// generic path costs 6 byte loads per 2 pixels. Take 3 word loads ->
	// 4 pixels instead; 24bpp rows can be arbitrarily byte-aligned so the
	// loads go through memcpy (gcc emits lwl/lwr when unaligned is
	// possible; still 2 loads for 4 pixels' worth of channel bytes).
	// Bit-exact vs bgr888_to_rgb565_pair (host-tested, test_cspace888.cpp).
	for (; pixels >= 4; pixels -= 4, src += 3*4, dst += 2) {
		uint32_t w0, w1, w2;
		memcpy(&w0, src + 0, 4);   /* r0 g0 b0 r1 */
		memcpy(&w1, src + 4, 4);   /* g1 b1 r2 g2 */
		memcpy(&w2, src + 8, 4);   /* b2 r3 g3 b3 */
		uint32_t px0 = ((w0 & 0xf8) << 8)      | ((w0 >>  5) & 0x07e0) | ((w0 >> 19) & 0x1f);
		uint32_t px1 = ((w0 >> 16) & 0xf800)   | ((w1 & 0xfc) << 3)    | ((w1 >> 11) & 0x1f);
		uint32_t px2 = ((w1 >>  8) & 0xf800)   | ((w1 >> 21) & 0x07e0) | ((w2 >>  3) & 0x1f);
		uint32_t px3 = ( w2        & 0xf800)   | ((w2 >> 13) & 0x07e0) | ((w2 >> 27) & 0x1f);
		dst[0] = px0 | (px1 << 16);
		dst[1] = px2 | (px3 << 16);
	}
#endif
	for (; pixels >= 2; pixels -= 2, src += 3*2, dst++)
		*dst = bgr888_to_rgb565_pair(src, 0, 3);
}

// TODO?
void rgb888_to_rgb565(void *dst, const void *src, int pixels) {}

void bgr888_to_rgb888(void * __restrict__ dst_,
	const void * __restrict__ src_, int pixels)
{
	const uint8_t * __restrict__ src = src_;
	uint8_t * __restrict__ dst = dst_;
	
	for (; pixels >= 1; pixels--, src += 3, dst += 3)
		bgr888_to_rgb888_one(dst, src);
}

#endif // HAVE_bgr888_to_x

static inline uint32_t bgr555_to_xrgb8888_one(uint16_t p)
{
	uint32_t t = ((p << 19) | (p >> 7)) & 0xf800f8;
	t |= (p << 6) & 0xf800;
	return t | ((t >> 5) & 0x070707);
}

static inline uint32_t bgr888_to_xrgb8888_one(const uint8_t * __restrict__ src)
{
	return (src[0] << 16) | (src[1] << 8) | src[2];
}

void bgr555_to_xrgb8888(void * __restrict__ dst_,
	const void * __restrict__ src_, int pixels)
{
	const uint16_t * __restrict__ src = src_;
	uint32_t * __restrict__ dst = dst_;

	for (; pixels >= 1; pixels--, src++, dst++)
		*dst = bgr555_to_xrgb8888_one(*src);
}

void bgr888_to_xrgb8888(void * __restrict__ dst_,
	const void * __restrict__ src_, int pixels)
{
	const uint8_t * __restrict__ src = src_;
	uint32_t * __restrict__ dst = dst_;

	for (; pixels >= 1; pixels--, src += 3, dst++)
		*dst = bgr888_to_xrgb8888_one(src);
}

/* downscale */
void bgr555_to_rgb565_640_to_320(void * __restrict__ dst_,
	const void * __restrict__ src_, int dpixels)
{
	const uint16_t * __restrict__ src = src_;
	uint32_t * __restrict__ dst = dst_;

	for (; dpixels >= 2; dpixels -= 2, src += 4, dst++) {
		uint32_t p = LE32TOH(src[0] | (src[2] << 16));
		*dst = bgr555_to_rgb565_pair(p);
	}
}

void bgr888_to_rgb565_640_to_320(void * __restrict__ dst_,
	const void * __restrict__ src_, int dpixels)
{
	const uint8_t * __restrict__ src = src_;
	uint32_t * __restrict__ dst = dst_;

	for (; dpixels >= 2; dpixels -= 2, src += 4*3, dst++)
		*dst = bgr888_to_rgb565_pair(src, 0*3, 2*3);
}

void bgr888_to_rgb888_640_to_320(void * __restrict__ dst_,
	const void * __restrict__ src_, int dpixels)
{
	const uint8_t * __restrict__ src = src_;
	uint8_t * __restrict__ dst = dst_;

	for (; dpixels >= 1; dpixels--, src += 2*3, dst += 3)
		bgr888_to_rgb888_one(dst, src);
}

void bgr555_to_xrgb8888_640_to_320(void * __restrict__ dst_,
	const void * __restrict__ src_, int dpixels)
{
	const uint16_t * __restrict__ src = src_;
	uint32_t * __restrict__ dst = dst_;

	for (; dpixels >= 1; dpixels--, src += 2, dst++)
		*dst = bgr555_to_xrgb8888_one(*src);
}

void bgr888_to_xrgb8888_640_to_320(void * __restrict__ dst_,
	const void * __restrict__ src_, int dpixels)
{
	const uint8_t * __restrict__ src = src_;
	uint32_t * __restrict__ dst = dst_;

	for (; dpixels >= 1; dpixels--, src += 3*2, dst++)
		*dst = (src[0] << 16) | (src[1] << 8) | src[2];
}

void bgr555_to_rgb565_512_to_320(void * __restrict__ dst_,
	const void * __restrict__ src_, int dpixels)
{
	const uint16_t * __restrict__ src = src_;
	uint32_t * __restrict__ dst = dst_;

#ifdef HAVE_bgr555_to_rgb565_mxu
	/* EROSQ X1000: fused decimate+convert, 16 src px -> 10 dst px per
	 * iteration. The pick pattern (0,2)(4,5)(7,8)(10,12)(13,15) maps to
	 * three S32SFL halfword packs, one S32ALNI mid-align, and one raw
	 * word; the convert math is the proven bgr555_to_rgb565 MXU pipeline
	 * (green LSB stays 0). ~4.6 vs 6.6 ins/px scalar. Bit-exact
	 * (test_cspace_mxu.cpp). dst (u32*) is always 4-aligned; src must be
	 * too, else the scalar loop below takes the whole row. */
	if (!((uintptr_t)src & 3)) {
		MXU_S32I2M(MXU_XR16, 1);          /* enable MXU ALU */
		MXU_S32I2M(MXU_XR15, 0x07c007c0); /* green mask */

		for (; dpixels >= 10; dpixels -= 10, src += 16, dst += 5) {
			__builtin_prefetch(src + 48);
			MXU_S32LDD(MXU_XR1, src);        /* px1 :px0  */
			MXU_S32LDD(MXU_XR2, src + 2);    /* px3 :px2  */
			MXU_S32LDD(MXU_XR3, src + 4);    /* px5 :px4  */
			MXU_S32LDD(MXU_XR4, src + 6);    /* px7 :px6  */
			MXU_S32LDD(MXU_XR5, src + 8);    /* px9 :px8  */
			MXU_S32LDD(MXU_XR6, src + 10);   /* px11:px10 */
			MXU_S32LDD(MXU_XR7, src + 12);   /* px13:px12 */
			MXU_S32LDD(MXU_XR8, src + 14);   /* px15:px14 */
			/* decimate: build the 5 output words */
			MXU_S32SFL(MXU_XR0, MXU_XR2, MXU_XR1, MXU_XR9, 3);   /* px2 :px0  */
			MXU_S32ALNI(MXU_XR10, MXU_XR5, MXU_XR4, 2);          /* px8 :px7  */
			MXU_S32SFL(MXU_XR0, MXU_XR7, MXU_XR6, MXU_XR11, 3);  /* px12:px10 */
			MXU_S32SFL(MXU_XR12, MXU_XR8, MXU_XR7, MXU_XR0, 3);  /* px15:px13 */
			/* convert pair (XR9, XR3=px5:px4) -> dst[0..1] */
			MXU_Q16SLL(MXU_XR1, MXU_XR9, MXU_XR3, MXU_XR2, 1);
			MXU_Q16SLL(MXU_XR4, MXU_XR9, MXU_XR3, MXU_XR5, 11);
			MXU_Q16SLR(MXU_XR6, MXU_XR1, MXU_XR2, MXU_XR7, 11);
			MXU_S32AND(MXU_XR1, MXU_XR1, MXU_XR15);
			MXU_S32AND(MXU_XR2, MXU_XR2, MXU_XR15);
			MXU_S32OR(MXU_XR4, MXU_XR4, MXU_XR1);
			MXU_S32OR(MXU_XR4, MXU_XR4, MXU_XR6);
			MXU_S32OR(MXU_XR5, MXU_XR5, MXU_XR2);
			MXU_S32OR(MXU_XR5, MXU_XR5, MXU_XR7);
			MXU_S32STD(MXU_XR4, dst);
			MXU_S32STD(MXU_XR5, dst + 1);
			/* convert pair (XR10, XR11) -> dst[2..3] */
			MXU_Q16SLL(MXU_XR1, MXU_XR10, MXU_XR11, MXU_XR2, 1);
			MXU_Q16SLL(MXU_XR4, MXU_XR10, MXU_XR11, MXU_XR5, 11);
			MXU_Q16SLR(MXU_XR6, MXU_XR1, MXU_XR2, MXU_XR7, 11);
			MXU_S32AND(MXU_XR1, MXU_XR1, MXU_XR15);
			MXU_S32AND(MXU_XR2, MXU_XR2, MXU_XR15);
			MXU_S32OR(MXU_XR4, MXU_XR4, MXU_XR1);
			MXU_S32OR(MXU_XR4, MXU_XR4, MXU_XR6);
			MXU_S32OR(MXU_XR5, MXU_XR5, MXU_XR2);
			MXU_S32OR(MXU_XR5, MXU_XR5, MXU_XR7);
			MXU_S32STD(MXU_XR4, dst + 2);
			MXU_S32STD(MXU_XR5, dst + 3);
			/* convert single (XR12) -> dst[4] */
			MXU_Q16SLL(MXU_XR1, MXU_XR12, MXU_XR0, MXU_XR0, 1);
			MXU_Q16SLL(MXU_XR4, MXU_XR12, MXU_XR0, MXU_XR0, 11);
			MXU_Q16SLR(MXU_XR6, MXU_XR1, MXU_XR0, MXU_XR0, 11);
			MXU_S32AND(MXU_XR1, MXU_XR1, MXU_XR15);
			MXU_S32OR(MXU_XR4, MXU_XR4, MXU_XR1);
			MXU_S32OR(MXU_XR4, MXU_XR4, MXU_XR6);
			MXU_S32STD(MXU_XR4, dst + 4);
		}
	}
#endif

	// 16 -> 10 to keep dst aligned
	for (; dpixels >= 10; dpixels -= 10, src += 16, dst += 5) {
		// picks a src pixel nearest to the center of the dst pixel
		dst[0] = bgr555_to_rgb565_pair(LE32TOH(src[0]  | (src[2] << 16)));
		dst[1] = bgr555_to_rgb565_pair(LE32TOH(src[4]  | (src[5] << 16)));
		dst[2] = bgr555_to_rgb565_pair(LE32TOH(src[7]  | (src[8] << 16)));
		dst[3] = bgr555_to_rgb565_pair(LE32TOH(src[10] | (src[12] << 16)));
		dst[4] = bgr555_to_rgb565_pair(LE32TOH(src[13] | (src[15] << 16)));
	}
}

void bgr888_to_rgb565_512_to_320(void * __restrict__ dst_,
	const void * __restrict__ src_, int dpixels)
{
	const uint8_t * __restrict__ src = src_;
	uint32_t * __restrict__ dst = dst_;

	for (; dpixels >= 10; dpixels -= 10, src += 16*3, dst += 5) {
		dst[0] = bgr888_to_rgb565_pair(src, 3*0, 3*2);
		dst[1] = bgr888_to_rgb565_pair(src, 3*4, 3*5);
		dst[2] = bgr888_to_rgb565_pair(src, 3*7, 3*8);
		dst[3] = bgr888_to_rgb565_pair(src, 3*10, 3*12);
		dst[4] = bgr888_to_rgb565_pair(src, 3*13, 3*15);
	}
}

void bgr888_to_rgb888_512_to_320(void * __restrict__ dst_,
	const void * __restrict__ src_, int dpixels)
{
	const uint8_t * __restrict__ src = src_;
	uint8_t * __restrict__ dst = dst_;

	for (; dpixels >= 5; dpixels -= 5, src += 8*3, dst += 5*3) {
		bgr888_to_rgb888_one(dst + 3*0, src + 3*0);
		bgr888_to_rgb888_one(dst + 3*1, src + 3*2);
		bgr888_to_rgb888_one(dst + 3*2, src + 3*4);
		bgr888_to_rgb888_one(dst + 3*3, src + 3*5);
		bgr888_to_rgb888_one(dst + 3*4, src + 3*7);
	}
}

void bgr555_to_xrgb8888_512_to_320(void * __restrict__ dst_,
	const void * __restrict__ src_, int dpixels)
{
	const uint16_t * __restrict__ src = src_;
	uint32_t * __restrict__ dst = dst_;

	// 8 -> 5
	for (; dpixels >= 5; dpixels -= 5, src += 8, dst += 5) {
		dst[0] = bgr555_to_xrgb8888_one(src[0]);
		dst[1] = bgr555_to_xrgb8888_one(src[2]);
		dst[2] = bgr555_to_xrgb8888_one(src[4]);
		dst[3] = bgr555_to_xrgb8888_one(src[5]);
		dst[4] = bgr555_to_xrgb8888_one(src[7]);
	}
}

void bgr888_to_xrgb8888_512_to_320(void * __restrict__ dst_,
	const void * __restrict__ src_, int dpixels)
{
	const uint8_t * __restrict__ src = src_;
	uint32_t * __restrict__ dst = dst_;

	for (; dpixels >= 5; dpixels -= 5, src += 8*3, dst += 5) {
		dst[0] = bgr888_to_xrgb8888_one(src + 0*3);
		dst[1] = bgr888_to_xrgb8888_one(src + 2*3);
		dst[2] = bgr888_to_xrgb8888_one(src + 4*3);
		dst[3] = bgr888_to_xrgb8888_one(src + 5*3);
		dst[4] = bgr888_to_xrgb8888_one(src + 7*3);
	}
}

/* ---- EROSQ: averaging hi-res downscalers -------------------------------- */
/*
 * The nearest-neighbour downscalers above pick ONE source column per output
 * pixel, so in 512 mode 3 of every 8 columns are discarded entirely. Games
 * that switch to a hi-res mode specifically to draw text (Gran Turismo 2's
 * dealership/menu screens are 512x480) draw glyph stems one source pixel
 * wide, so a discarded column does not dim a stem -- it deletes it, and
 * letters change identity ("firm" -> "flnm"). These variants average instead,
 * so every source column contributes to some output pixel.
 *
 * Selected by scale_hires >= 2 (see menu.c / plugin_lib.c pl_vout_set_mode):
 *   2 "Sharp"  -> _avg  : average columns; still one interlace field.
 *   3 "Sharp+" -> _avg2 : also average the two source rows of each output
 *                         row, which requires the renderer to draw BOTH
 *                         fields (gpulib_if.cpp renderer_notify_screen_change).
 * The _avg2 forms read src[i] and src[i + 1024]; pl_vout_flip already walks
 * two VRAM rows per output row in this mode (sstride 4096), and the deepest
 * row touched is still inside the display area, so no extra clamping.
 *
 * 16bpp (bgr555) only -- 24bpp is FMV, where this does not matter and motion
 * hides it; the cspace table points the bgr888 rows at the plain variants.
 */

/* exact per-channel floor((a+b)/2) for two packed bgr555 pixels.
 * 0x7bde7bde clears each 5-bit channel's LSB before the shift so no channel
 * carries into the next; ((x^y)>>1) + (x&y) == floor((x+y)/2) per channel.
 * Per-channel results are <= 31, so nothing carries out of bit15 either. */
static inline uint32_t avg555x2(uint32_t a, uint32_t b)
{
	return (((a ^ b) & 0x7bde7bde) >> 1) + (a & b);
}

/* 16 source columns -> 10 output pixels (the 8:5 ratio, doubled to keep dst
 * word-aligned). Output j samples source floor(j*1.6) and the column after
 * it; across a group that covers all 16 columns exactly once or twice. */
static const unsigned char avg512_off[10] = { 0, 1, 3, 4, 6, 8, 9, 11, 12, 14 };

#define AVG_PAIR(s, k) \
	avg555x2((uint32_t)LE16TOH((s)[avg512_off[2*(k)]]) | \
	         ((uint32_t)LE16TOH((s)[avg512_off[2*(k)+1]]) << 16), \
	         (uint32_t)LE16TOH((s)[avg512_off[2*(k)] + 1]) | \
	         ((uint32_t)LE16TOH((s)[avg512_off[2*(k)+1] + 1]) << 16))

void bgr555_to_rgb565_512_to_320_avg(void * __restrict__ dst_,
	const void * __restrict__ src_, int dpixels)
{
	const uint16_t * __restrict__ src = src_;
	uint32_t * __restrict__ dst = dst_;
	int k;

	for (; dpixels >= 10; dpixels -= 10, src += 16, dst += 5)
		for (k = 0; k < 5; k++)
			dst[k] = bgr555_to_rgb565_pair(AVG_PAIR(src, k));
}

void bgr555_to_rgb565_512_to_320_avg2(void * __restrict__ dst_,
	const void * __restrict__ src_, int dpixels)
{
	const uint16_t * __restrict__ src = src_;
	uint32_t * __restrict__ dst = dst_;
	int k;

	for (; dpixels >= 10; dpixels -= 10, src += 16, dst += 5)
		for (k = 0; k < 5; k++)
			dst[k] = bgr555_to_rgb565_pair(
				avg555x2(AVG_PAIR(src, k), AVG_PAIR(src + 1024, k)));
}

#undef AVG_PAIR

/* 640 -> 320 is a clean 2:1, so this is an exact box filter. */
void bgr555_to_rgb565_640_to_320_avg(void * __restrict__ dst_,
	const void * __restrict__ src_, int dpixels)
{
	const uint32_t * __restrict__ src = (const void *)((intptr_t)src_ & ~3);
	uint32_t * __restrict__ dst = dst_;

	for (; dpixels >= 2; dpixels -= 2, src += 2, dst += 1) {
		uint32_t p0 = LE16TOHx2(src[0]), p1 = LE16TOHx2(src[1]);
		/* p0 = (s1<<16)|s0, p1 = (s3<<16)|s2 -> out (avg(s2,s3) << 16) | avg(s0,s1) */
		uint32_t lo = avg555x2(p0, p0 >> 16) & 0xffff;
		uint32_t hi = avg555x2(p1, p1 >> 16) & 0xffff;
		dst[0] = bgr555_to_rgb565_pair(lo | (hi << 16));
	}
}

void bgr555_to_rgb565_640_to_320_avg2(void * __restrict__ dst_,
	const void * __restrict__ src_, int dpixels)
{
	const uint32_t * __restrict__ src = (const void *)((intptr_t)src_ & ~3);
	uint32_t * __restrict__ dst = dst_;

	for (; dpixels >= 2; dpixels -= 2, src += 2, dst += 1) {
		uint32_t a0 = LE16TOHx2(src[0]),     a1 = LE16TOHx2(src[1]);
		uint32_t b0 = LE16TOHx2(src[512]),   b1 = LE16TOHx2(src[513]);
		uint32_t lo = avg555x2(avg555x2(a0, a0 >> 16),
				       avg555x2(b0, b0 >> 16)) & 0xffff;
		uint32_t hi = avg555x2(avg555x2(a1, a1 >> 16),
				       avg555x2(b1, b1 >> 16)) & 0xffff;
		dst[0] = bgr555_to_rgb565_pair(lo | (hi << 16));
	}
}

/* No horizontal change (source is already <= panel width, e.g. GT2's 368x480
 * and 320x480 screens), vertical field pair averaged. scale_hires == 3 only. */
void bgr555_to_rgb565_avg2(void * __restrict__ dst_,
	const void * __restrict__ src_, int pixels)
{
	const uint32_t * __restrict__ src = (const void *)((intptr_t)src_ & ~3);
	uint32_t x, * __restrict__ dst = dst_;

	for (x = 0; x < pixels / 2; x++)
		dst[x] = bgr555_to_rgb565_pair(
			avg555x2(LE16TOHx2(src[x]), LE16TOHx2(src[x + 512])));
}

/* YUV stuff */
static int yuv_ry[32], yuv_gy[32], yuv_by[32];
static unsigned char yuv_u[32 * 2], yuv_v[32 * 2];
static struct uyvy { uint32_t y:8; uint32_t vyu:24; } yuv_uyvy[32768];

void bgr_to_uyvy_init(void)
{
	unsigned char yuv_y[256];
	int i, v;

	/* init yuv converter:
	   y0 = (int)((0.299f * r0) + (0.587f * g0) + (0.114f * b0));
	   y1 = (int)((0.299f * r1) + (0.587f * g1) + (0.114f * b1));
	   u = (int)(8 * 0.565f * (b0 - y0)) + 128;
	   v = (int)(8 * 0.713f * (r0 - y0)) + 128;
	   */
	for (i = 0; i < 32; i++) {
		yuv_ry[i] = (int)(0.299f * i * 65536.0f + 0.5f);
		yuv_gy[i] = (int)(0.587f * i * 65536.0f + 0.5f);
		yuv_by[i] = (int)(0.114f * i * 65536.0f + 0.5f);
	}
	for (i = -32; i < 32; i++) {
		v = (int)(8 * 0.565f * i) + 128;
		if (v < 0)
			v = 0;
		if (v > 255)
			v = 255;
		yuv_u[i + 32] = v;
		v = (int)(8 * 0.713f * i) + 128;
		if (v < 0)
			v = 0;
		if (v > 255)
			v = 255;
		yuv_v[i + 32] = v;
	}
	// valid Y range seems to be 16..235
	for (i = 0; i < 256; i++) {
		yuv_y[i] = 16 + 219 * i / 32;
	}
	// everything combined into one large array for speed
	for (i = 0; i < 32768; i++) {
		int r = (i >> 0) & 0x1f, g = (i >> 5) & 0x1f, b = (i >> 10) & 0x1f;
		int y = (yuv_ry[r] + yuv_gy[g] + yuv_by[b]) >> 16;
		yuv_uyvy[i].y = yuv_y[y];
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
		yuv_uyvy[i].vyu = (yuv_v[b-y + 32] << 16) | (yuv_y[y] << 8) | yuv_u[r-y + 32];
#else
		yuv_uyvy[i].vyu = (yuv_v[r-y + 32] << 16) | (yuv_y[y] << 8) | yuv_u[b-y + 32];
#endif
	}
}

void rgb565_to_uyvy(void *d, const void *s, int pixels)
{
  unsigned int *dst = d;
  const unsigned short *src = s;
  const unsigned char *yu = yuv_u + 32;
  const unsigned char *yv = yuv_v + 32;
  int r0, g0, b0, r1, g1, b1;
  int y0, y1, u, v;

  for (; pixels > 0; src += 2, dst++, pixels -= 2)
  {
    r0 = (src[0] >> 11) & 0x1f;
    g0 = (src[0] >> 6) & 0x1f;
    b0 =  src[0] & 0x1f;
    r1 = (src[1] >> 11) & 0x1f;
    g1 = (src[1] >> 6) & 0x1f;
    b1 =  src[1] & 0x1f;
    y0 = (yuv_ry[r0] + yuv_gy[g0] + yuv_by[b0]) >> 16;
    y1 = (yuv_ry[r1] + yuv_gy[g1] + yuv_by[b1]) >> 16;
    u = yu[b0 - y0];
    v = yv[r0 - y0];
    // valid Y range seems to be 16..235
    y0 = 16 + 219 * y0 / 31;
    y1 = 16 + 219 * y1 / 31;

    *dst = (y1 << 24) | (v << 16) | (y0 << 8) | u;
  }
}

void bgr555_to_uyvy(void *d, const void *s, int pixels, int x2)
{
	uint32_t *dst = d;
	const uint16_t *src = s;
	int i;

	if (x2) {
		for (i = pixels; i >= 4; src += 4, dst += 4, i -= 4)
		{
			const struct uyvy *uyvy0 = yuv_uyvy + (src[0] & 0x7fff);
			const struct uyvy *uyvy1 = yuv_uyvy + (src[1] & 0x7fff);
			const struct uyvy *uyvy2 = yuv_uyvy + (src[2] & 0x7fff);
			const struct uyvy *uyvy3 = yuv_uyvy + (src[3] & 0x7fff);
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
			dst[0] = uyvy0->y | (uyvy0->vyu << 8);
			dst[1] = uyvy1->y | (uyvy1->vyu << 8);
			dst[2] = uyvy2->y | (uyvy2->vyu << 8);
			dst[3] = uyvy3->y | (uyvy3->vyu << 8);
#else
			dst[0] = (uyvy0->y << 24) | uyvy0->vyu;
			dst[1] = (uyvy1->y << 24) | uyvy1->vyu;
			dst[2] = (uyvy2->y << 24) | uyvy2->vyu;
			dst[3] = (uyvy3->y << 24) | uyvy3->vyu;
#endif
		}
	} else {
		for (i = pixels; i >= 4; src += 4, dst += 2, i -= 4)
		{
			const struct uyvy *uyvy0 = yuv_uyvy + (src[0] & 0x7fff);
			const struct uyvy *uyvy1 = yuv_uyvy + (src[1] & 0x7fff);
			const struct uyvy *uyvy2 = yuv_uyvy + (src[2] & 0x7fff);
			const struct uyvy *uyvy3 = yuv_uyvy + (src[3] & 0x7fff);
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
			dst[0] = uyvy1->y | (uyvy0->vyu << 8);
			dst[1] = uyvy3->y | (uyvy2->vyu << 8);
#else
			dst[0] = (uyvy1->y << 24) | uyvy0->vyu;
			dst[1] = (uyvy3->y << 24) | uyvy2->vyu;
#endif
		}
	}
}

void bgr888_to_uyvy(void *d, const void *s, int pixels, int x2)
{
	unsigned int *dst = d;
	const unsigned char *src8 = s;
	const unsigned char *yu = yuv_u + 32;
	const unsigned char *yv = yuv_v + 32;
	int r0, g0, b0, r1, g1, b1;
	int y0, y1, u0, u1, v0, v1;

	if (x2) {
		for (; pixels >= 2; src8 += 3*2, pixels -= 2)
		{
			r0 = src8[0], g0 = src8[1], b0 = src8[2];
			r1 = src8[3], g1 = src8[4], b1 = src8[5];
			y0 = (r0 * 19595 + g0 * 38470 + b0 * 7471) >> 16;
			y1 = (r1 * 19595 + g1 * 38470 + b1 * 7471) >> 16;
			u0 = yu[(b0 - y0) / 8];
			u1 = yu[(b1 - y1) / 8];
			v0 = yv[(r0 - y0) / 8];
			v1 = yv[(r1 - y1) / 8];
			y0 = 16 + 219 * y0 / 255;
			y1 = 16 + 219 * y1 / 255;

			*dst++ = (y0 << 24) | (v0 << 16) | (y0 << 8) | u0;
			*dst++ = (y1 << 24) | (v1 << 16) | (y1 << 8) | u1;
		}
	}
	else {
		for (; pixels >= 2; src8 += 3*2, dst++, pixels -= 2)
		{
			r0 = src8[0], g0 = src8[1], b0 = src8[2];
			r1 = src8[3], g1 = src8[4], b1 = src8[5];
			y0 = (r0 * 19595 + g0 * 38470 + b0 * 7471) >> 16;
			y1 = (r1 * 19595 + g1 * 38470 + b1 * 7471) >> 16;
			u0 = yu[(b0 - y0) / 8];
			v0 = yv[(r0 - y0) / 8];
			y0 = 16 + 219 * y0 / 255;
			y1 = 16 + 219 * y1 / 255;

			*dst = (y1 << 24) | (v0 << 16) | (y0 << 8) | u0;
		}
	}
}
