/*
 * EROSQ: flat-lit CLUT pre-lighting.
 *
 * For flat-shaded (non-gouraud), non-dithered, texture-blended prims the
 * light color is constant across the whole prim, and gpuLightingTXT() is
 * a pure per-entry function of the CLUT value that preserves bit15.  The
 * lighting can therefore be folded into a snapshot of the CLUT taken once
 * per prim, and the prim rendered with the (faster) unlit drivers.
 *
 * Bit-exactness: the unlit drivers' transparency test is (CLUT entry ==
 * 0); the lit drivers test the raw entry before lighting.  The fold is
 * exact iff lighting never moves an entry onto/off zero — verified per
 * entry below; on violation the caller keeps the plain lit driver.
 */
#ifndef GPU_PRELIGHT_H
#define GPU_PRELIGHT_H

// Fill out[16] with lit CLUT entries; false if the fold would change the
// transparency test.  bgr0888/lightlut exactly as gpuLightingTXTGeneric()
// (production bgr0888 always has byte 3 == 0, so >>19 is 5 bits).
static inline bool gpuPrelightCLUT16(u32 bgr0888, const le16_t *cba,
	const u8 *lightlut, le16_t *out)
{
	u32 r5 = (bgr0888 >>  3) & 0x1f;
	u32 g5 = (bgr0888 >> 11) & 0x1f;
	u32 b5 = (bgr0888 >> 19) & 0x1f;
	for (int i = 0; i < 16; i++) {
		u32 raw = le16_to_u16(cba[i]);
		u32 lit = (lightlut[((raw & 0x7C00) >> 5) | b5] << 10) |
		          (lightlut[ (raw & 0x03E0)       | g5] <<  5) |
		          (lightlut[((raw & 0x001F) << 5) | r5])       |
		          (raw & 0x8000);
		if ((raw == 0) != (lit == 0))
			return false;
		out[i] = u16_to_le16(lit);
	}
	return true;
}

#endif /* GPU_PRELIGHT_H */
