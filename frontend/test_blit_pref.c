/* Host test for pl_vout_flip's pref-30 row pre-allocation: poison every 32 B
 * line lying wholly inside the destination row (worst-case PrepareForStore
 * semantics), run each 16bpp blit pl_vout_flip can select, and require the
 * row to equal an unpoisoned run. A blit that leaves any such byte unwritten
 * fails. (Host builds the C versions; the MXU twins write the same
 * dst_pixels -- covered by test_cspace_mxu.cpp.)
 *   cc -O2 -Iinclude -Ifrontend -o t frontend/test_blit_pref.c && ./t */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cspace.c"

typedef void (*blitfn)(void *, const void *, int);

static void poison_full_lines(void *d, int bytes)
{
	uintptr_t a = ((uintptr_t)d + 31) & ~(uintptr_t)31, e = ((uintptr_t)d + bytes) & ~(uintptr_t)31;
	for (; a < e; a += 32) memset((void *)a, 0xEE, 32);
}

int main(void)
{
	static uint16_t src[2048 + 1024];
	static uint8_t ref[4096] __attribute__((aligned(64))), got[4096] __attribute__((aligned(64)));
	struct { const char *n; blitfn f; int maxw; } b[] = {
		{ "1:1",        bgr555_to_rgb565, 1024 },
		{ "512->320",   bgr555_to_rgb565_512_to_320, 320 },
		{ "640->320",   bgr555_to_rgb565_640_to_320, 320 },
		{ "512 avg",    bgr555_to_rgb565_512_to_320_avg, 320 },
		{ "640 avg",    bgr555_to_rgb565_640_to_320_avg, 320 },
		{ "scaled368",  bgr555_to_rgb565_scaled_to_320, 320 },
	};
	int fails = 0, cases = 0;
	srand(3);
	bgr555_to_rgb565_scaled_setup(368);
	for (unsigned k = 0; k < sizeof(b) / sizeof(b[0]); k++)
		for (int t = 0; t < 3000; t++) {
			for (int i = 0; i < 2048 + 1024; i++) src[i] = rand();
			int w = 1 + rand() % b[k].maxw, off = (rand() % 16) * 2;  /* dst byte offset, 2-aligned */
			memset(ref, 0x55, sizeof(ref)); memset(got, 0x55, sizeof(got));
			b[k].f(ref + off, src, w);
			poison_full_lines(got + off, w * 2);
			b[k].f(got + off, src, w);
			if (memcmp(ref, got, sizeof(ref))) {
				if (fails++ < 5) printf("FAIL %s w=%d off=%d\n", b[k].n, w, off);
			}
			cases++;
		}
	if (fails) { printf("FAIL: %d of %d\n", fails, cases); return 1; }
	printf("PASS: %d cases, every pref'd line fully rewritten by all 6 blits\n", cases);
	return 0;
}
