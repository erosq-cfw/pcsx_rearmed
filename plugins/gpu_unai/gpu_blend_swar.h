/***************************************************************************
*   EROSQ: 2-pixel SWAR versions of gpuBlendingGeneric (gpu_inner_blend.h)
*   for the MXU span kernels: two bgr555 pixels packed in one u32, one
*   blend per pair, in plain 32-bit GPR code.
*
*   Why this is per-lane exact: Blargg's carry-mask arithmetic never
*   carries across a 16-bit lane boundary for modes 0/1/3 —
*     mode 0: lane sums of 0x7bde-masked values are <= 0xf7bc, and lane
*             bit0 of each sum is 0 (both operands have bit0 masked), so
*             the word >>1 can't leak a bit between lanes either.
*     mode 1: lane sums of 0x7fff-masked values are <= 0xfffe.
*     mode 3: src lanes are <= 0x1ce7 after the >>2/mask, sums <= 0x9ce6.
*             ((uSrc >> 2) at word level smears lane1 bits into lane0
*             positions 14-15, but 0x1ce7's top bit is 12 — masked away.)
*   All interior subtractions have per-lane minuend >= subtrahend, so no
*   cross-lane borrows.
*
*   Mode 2 (1.0*B - 1.0*F) computes dst - src + 0x8420, which spans 17
*   bits per lane and DOES cross lanes: it needs true per-lane mod-2^16
*   arithmetic and lives in gpu_inner_mxu.h (mxu_blend_pair_sub) instead.
*
*   Per-lane contract (verified by test_blend_swar.cpp on host):
*     lane(result) == gpuBlendingGeneric<BM, false>(lane(src), lane(dst))
*   for every 16-bit lane value including bit15 set (masked internally).
***************************************************************************/

#ifndef GPU_BLEND_SWAR_H
#define GPU_BLEND_SWAR_H

template <int BM>
static inline u32 gpuBlendPairSWAR(u32 uSrc, u32 uDst)
{
	// 0.5 x Back + 0.5 x Forward
	if (BM == 0) {
		return ((uDst & 0x7bde7bde) + (uSrc & 0x7bde7bde)) >> 1;
	}

	// 1.0 x Back + 1.0 x Forward  /  1.0 x Back + 0.25 x Forward
	if (BM == 1 || BM == 3) {
		uDst &= 0x7fff7fff;
		if (BM == 3)
			uSrc = (uSrc >> 2) & 0x1ce71ce7;
		else
			uSrc &= 0x7fff7fff;
		u32 sum      = uSrc + uDst;
		u32 low_bits = (uSrc ^ uDst) & 0x04210421;
		u32 carries  = (sum - low_bits) & 0x84208420;
		u32 modulo   = sum - carries;
		u32 clamp    = carries - (carries >> 5);
		return modulo | clamp;
	}

	return 0;  /* mode 2: see mxu_blend_pair_sub in gpu_inner_mxu.h */
}

/* Per-lane select mask from the bgr555 mask bit: lanes of 'pair' with
 * bit15 set -> 0xffff, clear -> 0x0000. The subtraction borrows only
 * within a lane (minuend 0x8000 > subtrahend 0x0001 whenever nonzero). */
static inline u32 gpuMaskLanesSWAR(u32 pair)
{
	u32 m = pair & 0x80008000;
	return m | (m - (m >> 15));
}

#endif /* GPU_BLEND_SWAR_H */
