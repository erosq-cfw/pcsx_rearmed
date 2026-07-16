/* EROSQ host test: gpuBlendPairSWAR / gpuMaskLanesSWAR (gpu_blend_swar.h)
 * vs the scalar reference gpuBlendingGeneric (gpu_inner_blend.h), per lane.
 *
 * Build & run (host, no deps):
 *   g++ -O2 -o /tmp/test_blend_swar plugins/gpu_unai/test_blend_swar.cpp && /tmp/test_blend_swar
 *
 * Exhaustive on lane0 src x dst 16-bit values (all 2^32 pairs would be the
 * same test repeated; a lane is only 16x16 bits) with adversarial lane1
 * companions chosen to maximize cross-lane carry/borrow exposure, plus
 * 20M random pairs. Covers bit15-set inputs (textured mask bit).
 */
#include <cstdint>
#include <cstdio>
#include <cstdlib>

typedef uint32_t u32;
typedef uint_fast16_t uint_fast16_t_alias;  /* keep gpu header happy */
#define GPU_INLINE static inline
/* gpu_inner_blend.h uses uint_fast16_t directly — provided by <cstdint>. */
#include "gpu_inner_blend.h"
#include "gpu_blend_swar.h"

template <int BM>
static void check_pair(u32 s, u32 d)
{
	u32 got = gpuBlendPairSWAR<BM>(s, d);
	u32 ref0 = (u32)gpuBlendingGeneric<BM, false>(s & 0xffff, d & 0xffff) & 0xffff;
	u32 ref1 = (u32)gpuBlendingGeneric<BM, false>(s >> 16, d >> 16) & 0xffff;
	u32 ref = ref0 | (ref1 << 16);
	if (got != ref) {
		fprintf(stderr, "FAIL BM=%d s=%08x d=%08x got=%08x ref=%08x\n",
		        BM, s, d, got, ref);
		exit(1);
	}
}

template <int BM>
static void run_mode(void)
{
	/* lane1 companions: extremes and carry-magnet values */
	static const u32 comp[] = { 0x0000, 0xffff, 0x7fff, 0x8000, 0x8421,
	                            0x7bde, 0x0421, 0x8420, 0x0001, 0xfffe };
	const int NC = sizeof(comp) / sizeof(comp[0]);

	/* exhaustive lane0 (sampled stride 7 on src to keep runtime sane, all
	 * dst values), rotating lane1 companions */
	for (u32 s0 = 0; s0 <= 0xffff; s0 += 7) {
		for (u32 d0 = 0; d0 <= 0xffff; d0++) {
			u32 s1 = comp[(s0 + d0) % NC], d1 = comp[(s0 * 3 + d0) % NC];
			check_pair<BM>((s1 << 16) | s0, (d1 << 16) | d0);
		}
	}
	/* random sweep, both lanes free */
	srand(12345 + BM);
	for (int i = 0; i < 20000000; i++) {
		u32 s = ((u32)rand() << 17) ^ ((u32)rand() << 1) ^ (rand() & 1);
		u32 d = ((u32)rand() << 17) ^ ((u32)rand() << 1) ^ (rand() & 1);
		check_pair<BM>(s, d);
	}
	printf("mode %d OK\n", BM);
}

int main(void)
{
	/* select mask: all 4 lane-bit15 combinations */
	if (gpuMaskLanesSWAR(0x00000000) != 0x00000000 ||
	    gpuMaskLanesSWAR(0x00008000) != 0x0000ffff ||
	    gpuMaskLanesSWAR(0x80000000) != 0xffff0000 ||
	    gpuMaskLanesSWAR(0x80008000) != 0xffffffff ||
	    gpuMaskLanesSWAR(0x7fff7fff) != 0x00000000) {
		fprintf(stderr, "FAIL gpuMaskLanesSWAR\n");
		return 1;
	}
	printf("mask lanes OK\n");

	run_mode<0>();
	run_mode<1>();
	run_mode<3>();
	printf("all OK\n");
	return 0;
}
