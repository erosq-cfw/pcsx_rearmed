/* EROSQ host test: the hi-res pixel-skip mask must never skip a source column
 * that the ACTIVE downscaler reads (skipping a read column leaves it stale ->
 * period-N vertical stripes). Regression for the 512-mode bug where the mask
 * (0xa4, skip {2,5,7}) assumed present_scaled's floor(x*8/5) sampling while the
 * default scale_hires path uses cspace bgr555_to_rgb565_512_to_320's
 * center-nearest {0,2,4,5,7} — so it skipped exactly the columns that path
 * reads and garbaged every 512-wide hi-res scene (e.g. Spyro's intro).
 *
 * Pure logic, no deps:  g++ -O2 -o /tmp/t test_pskip_mask.cpp && /tmp/t
 *
 * Keep the sampled sets in sync with:
 *   - frontend/cspace.c  bgr555_to_rgb565_512/640_to_320   (scale_hires ON)
 *   - frontend/plat_erosq.c present_scaled x*hres/320       (scale_hires OFF)
 *   - plugins/gpu_unai/gpulib_if.cpp renderer_notify_screen_change (the mask)
 */
#include <cstdio>
#include <cstdlib>
#include <set>

/* The mask exactly as gpulib_if.cpp computes it. */
static unsigned pskip_mask(int hres, int downscale_enable)
{
	if (hres == 512) return downscale_enable ? 0x4a : 0xa4;
	if (hres == 640) return 0xaa;
	return 0;
}

/* Columns (phase 0..7) the active downscaler READS for a 320-wide panel. */
static std::set<int> read_cols(int hres, int downscale_enable)
{
	std::set<int> s;
	if (hres == 512) {
		if (downscale_enable)
			/* cspace 512->320: dst {0..4} <- src {0,2,4,5,7} per 8 */
			s = {0, 2, 4, 5, 7};
		else
			/* present_scaled: floor(x*512/320) for x=0..4 = {0,1,3,4,6} */
			for (int x = 0; x < 5; x++) s.insert((x * 512 / 320) & 7);
	} else if (hres == 640) {
		/* 2:1, both samplers take even columns */
		for (int x = 0; x < 4; x++) s.insert((x * 2) & 7);
	}
	return s;
}

int main()
{
	int fails = 0;
	int cases[][2] = { {512, 1}, {512, 0}, {640, 1}, {640, 0} };
	for (auto &c : cases) {
		int hres = c[0], ds = c[1];
		unsigned m = pskip_mask(hres, ds);
		std::set<int> rd = read_cols(hres, ds);
		/* SAFETY: no read column may be skipped. */
		for (int col : rd) {
			if ((m >> col) & 1) {
				printf("FAIL hres=%d ds=%d: mask 0x%02x SKIPS read col %d\n",
				       hres, ds, m, col);
				fails++;
			}
		}
		/* PERF: every skipped column must be one the downscaler drops
		 * (i.e. not read) — skipping a non-dropped col would be a bug too. */
		for (int col = 0; col < 8; col++)
			if (((m >> col) & 1) && rd.count(col)) fails++;   /* covered above */
		printf("hres=%d ds=%d mask=0x%02x read={", hres, ds, m);
		for (int col : rd) printf("%d,", col);
		printf("} OK\n");
	}
	if (fails) { printf("\n%d FAILURES\n", fails); return 1; }
	printf("\nall pskip-mask invariants hold\n");
	return 0;
}
