/* Host test: bgr555_to_rgb565_scaled_to_320 == convert the whole row with
 * bgr555_to_rgb565 (what used to go to the shadow) then pick x*w/320 (what
 * present_scaled sampled). Every width 321..507, aligned + misaligned dst,
 * full and short rows.
 *   cc -O2 -Iinclude -o t frontend/test_cspace_scaled.c && ./t */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cspace.c"

int main(void)
{
	static uint16_t src[1024], full[1024], ref[322], out[322];
	int w, fails = 0, cases = 0;
	srand(7);
	for (w = 321; w < 508; w++) {
		for (int t = 0; t < 20; t++) {
			for (int i = 0; i < 1024; i++) src[i] = rand();
			int dpx = t < 10 ? 320 : 1 + rand() % 320;
			int mis = t & 1;                  /* u16-misaligned dst */
			bgr555_to_rgb565(full, src, w);
			for (int x = 0; x < dpx; x++) ref[x] = full[x * w / 320];
			memset(out, 0xa5, sizeof(out));
			bgr555_to_rgb565_scaled_setup(w);
			bgr555_to_rgb565_scaled_to_320(out + mis, src, dpx);
			if (memcmp(out + mis, ref, dpx * 2) || out[mis + dpx] != 0xa5a5) {
				if (fails++ < 5) printf("FAIL w=%d dpx=%d mis=%d\n", w, dpx, mis);
			}
			cases++;
		}
	}
	if (fails) { printf("FAIL: %d of %d\n", fails, cases); return 1; }
	printf("PASS: %d cases (w 321..507) bit-exact vs convert+pick\n", cases);
	return 0;
}
