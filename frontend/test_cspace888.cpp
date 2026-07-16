/* EROSQ host test: word-ized bgr888_to_rgb565 4px kernel (cspace.c) vs the
 * byte-load reference bgr888_to_rgb565_pair, on random 12-byte groups.
 *
 * Build & run (host, no deps):
 *   g++ -O2 -o /tmp/test_cspace888 frontend/test_cspace888.cpp && /tmp/test_cspace888
 */
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

/* reference — copied from frontend/cspace.c (LE branch) */
static uint32_t ref_pair(const uint8_t *src, int o0, int o1)
{
	uint32_t r1 = src[o0 + 0] & 0xf8, g1 = src[o0 + 1] & 0xfc, b1 = src[o0 + 2] & 0xf8;
	uint32_t r2 = src[o1 + 0] & 0xf8, g2 = src[o1 + 1] & 0xfc, b2 = src[o1 + 2] & 0xf8;
	return (r2 << 24) | (g2 << 19) | (b2 << 13) | (r1 << 8) | (g1 << 3) | (b1 >> 3);
}

/* kernel under test — must stay in sync with cspace.c's MIPS branch */
static void quad(const uint8_t *src, uint32_t *dst)
{
	uint32_t w0, w1, w2;
	memcpy(&w0, src + 0, 4);
	memcpy(&w1, src + 4, 4);
	memcpy(&w2, src + 8, 4);
	uint32_t px0 = ((w0 & 0xf8) << 8)    | ((w0 >>  5) & 0x07e0) | ((w0 >> 19) & 0x1f);
	uint32_t px1 = ((w0 >> 16) & 0xf800) | ((w1 & 0xfc) << 3)    | ((w1 >> 11) & 0x1f);
	uint32_t px2 = ((w1 >>  8) & 0xf800) | ((w1 >> 21) & 0x07e0) | ((w2 >>  3) & 0x1f);
	uint32_t px3 = ( w2        & 0xf800) | ((w2 >> 13) & 0x07e0) | ((w2 >> 27) & 0x1f);
	dst[0] = px0 | (px1 << 16);
	dst[1] = px2 | (px3 << 16);
}

int main(void)
{
	uint8_t src[12];
	uint32_t got[2], ref[2];

	srand(777);
	for (int i = 0; i < 5000000; i++) {
		for (int j = 0; j < 12; j++)
			src[j] = (i < 256) ? (uint8_t)i /* exhaust constant fills first */
			                   : (uint8_t)rand();
		quad(src, got);
		ref[0] = ref_pair(src, 0, 3);
		ref[1] = ref_pair(src + 6, 0, 3);
		if (got[0] != ref[0] || got[1] != ref[1]) {
			fprintf(stderr, "FAIL i=%d got=%08x,%08x ref=%08x,%08x\n",
			        i, got[0], got[1], ref[0], ref[1]);
			return 1;
		}
	}
	printf("bgr888 quad OK\n");
	return 0;
}
