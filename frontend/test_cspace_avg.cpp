/* EROSQ host test for the averaging hi-res downscalers (cspace.c
 * bgr555_to_rgb565_{512,640}_to_320_avg[2] and bgr555_to_rgb565_avg2).
 *
 * Why they exist: the nearest-neighbour 512->320 path keeps source columns
 * {0,2,4,5,7} of every 8 and DROPS {1,3,6}. Gran Turismo 2 draws its 512x480
 * menu text with 1-source-pixel-wide glyph stems, so a dropped column deletes
 * a stem outright and letters change identity. The averaging variants must
 * therefore guarantee that EVERY source column influences the output, while
 * still being an exact per-channel average (no channel bleed, no drift).
 *
 * Pure logic, no deps:
 *   g++ -O2 -fpermissive -Iinclude -o /tmp/test_cspace_avg \
 *     frontend/test_cspace_avg.cpp && /tmp/test_cspace_avg
 * (-fpermissive: cspace.c is C, this harness is C++ -- only the C void* casts.)
 *
 * The production kernels are #included verbatim from cspace.c, so there is no
 * synced copy to drift out of date.
 */
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define assert_eq(a, b, ...) do { \
	if ((a) != (b)) { \
		printf("FAIL %s:%d: " #a " (%u) != " #b " (%u) ", \
			__FILE__, __LINE__, (unsigned)(a), (unsigned)(b)); \
		printf(__VA_ARGS__); printf("\n"); exit(1); \
	} \
} while (0)

#include "cspace.c"

#define VRAM_W 1024          /* the _avg2 kernels read src[i + 1024] (next row) */

static uint16_t mk(unsigned r, unsigned g, unsigned b)
{
	return (uint16_t)((b << 10) | (g << 5) | r);   /* PSX bgr555 */
}
static unsigned R565(uint32_t p) { return (p >> 11) & 0x1f; }
static unsigned G565(uint32_t p) { return (p >>  5) & 0x3f; }
static unsigned B565(uint32_t p) { return  p        & 0x1f; }

/* ---- 1. avg555x2 is an exact per-channel floor average, no channel bleed --- */
static void test_avg_exact(void)
{
	for (unsigned x = 0; x < 32; x++) {
		for (unsigned y = 0; y < 32; y++) {
			unsigned want = (x + y) / 2;
			/* each channel in isolation */
			assert_eq(avg555x2(mk(x,0,0), mk(y,0,0)) & 0x1f, want, "R %u,%u", x, y);
			assert_eq((avg555x2(mk(0,x,0), mk(0,y,0)) >> 5) & 0x1f, want, "G");
			assert_eq((avg555x2(mk(0,0,x), mk(0,0,y)) >> 10) & 0x1f, want, "B");
			/* all three at once: a bleed would corrupt a neighbour */
			uint32_t g = avg555x2(mk(x,y,x), mk(y,x,y));
			assert_eq(g & 0x1f, want, "RGB.R");
			assert_eq((g >> 5) & 0x1f, want, "RGB.G");
			assert_eq((g >> 10) & 0x1f, want, "RGB.B");
			/* both packed lanes independent, and no carry across bit15 */
			uint32_t a = mk(x,y,31) | ((uint32_t)mk(y,x,31) << 16);
			uint32_t b = mk(y,x,31) | ((uint32_t)mk(x,y,31) << 16);
			uint32_t p = avg555x2(a, b);
			assert_eq(p & 0x1f, want, "lane0.R");
			assert_eq((p >> 16) & 0x1f, want, "lane1.R");
			assert_eq((p >> 10) & 0x1f, 31u, "lane0.B saturated pair");
			assert_eq((p >> 26) & 0x1f, 31u, "lane1.B saturated pair");
		}
	}
	printf("  avg555x2 exact per-channel average, no bleed        OK\n");
}

/* ---- 2. a uniform field must survive unchanged (no drift) ----------------- */
static void test_no_drift(void)
{
	static uint16_t src[VRAM_W * 2];
	static uint32_t dst[160];
	const uint16_t v = mk(9, 22, 5);
	for (unsigned i = 0; i < VRAM_W * 2; i++) src[i] = v;
	uint32_t want = bgr555_to_rgb565_pair((uint32_t)v | ((uint32_t)v << 16));

	struct { const char *name; void (*fn)(void *, const void *, int); int px; } t[] = {
		{ "512->320 avg",  bgr555_to_rgb565_512_to_320_avg,  320 },
		{ "512->320 avg2", bgr555_to_rgb565_512_to_320_avg2, 320 },
		{ "640->320 avg",  bgr555_to_rgb565_640_to_320_avg,  320 },
		{ "640->320 avg2", bgr555_to_rgb565_640_to_320_avg2, 320 },
		{ "avg2 (1:1)",    bgr555_to_rgb565_avg2,            320 },
	};
	for (unsigned k = 0; k < sizeof t / sizeof *t; k++) {
		memset(dst, 0xa5, sizeof dst);
		t[k].fn(dst, src, t[k].px);
		for (int i = 0; i < t[k].px / 2; i++)
			assert_eq(dst[i], want, "%s at %d", t[k].name, i);
	}
	printf("  uniform input passes through unchanged (5 kernels)  OK\n");
}

/* ---- 3. THE POINT: every source column must reach the output -------------- */
/* Feed a black row with exactly one lit column and check the output is not
 * black. The nearest kernel fails this for {1,3,6,9,11,14} of every 16. */
static int lit_columns_reaching_output(void (*fn)(void *, const void *, int),
	int src_w, int group)
{
	static uint16_t src[VRAM_W * 2];
	static uint32_t dst[160];
	int reached = 0;

	for (int c = 0; c < group; c++) {
		memset(src, 0, sizeof src);
		src[c] = mk(31, 31, 31);
		src[c + VRAM_W] = mk(31, 31, 31);   /* same column in the paired row */
		memset(dst, 0, sizeof dst);
		fn(dst, src, 320);
		int any = 0;
		for (int i = 0; i < 320 / 2; i++)
			if (dst[i] != 0) { any = 1; break; }
		reached += any;
	}
	(void)src_w;
	return reached;
}

static void test_column_coverage(void)
{
	/* the historical nearest kernel: 6 of every 16 columns are lost */
	int near512 = lit_columns_reaching_output(bgr555_to_rgb565_512_to_320, 512, 16);
	assert_eq(near512, 10, "nearest 512->320 column coverage");
	printf("  nearest 512->320 loses %2d of 16 columns (the bug)   OK\n", 16 - near512);

	int avg512 = lit_columns_reaching_output(bgr555_to_rgb565_512_to_320_avg, 512, 16);
	assert_eq(avg512, 16, "avg 512->320 column coverage");
	int avg512b = lit_columns_reaching_output(bgr555_to_rgb565_512_to_320_avg2, 512, 16);
	assert_eq(avg512b, 16, "avg2 512->320 column coverage");

	int avg640 = lit_columns_reaching_output(bgr555_to_rgb565_640_to_320_avg, 640, 4);
	assert_eq(avg640, 4, "avg 640->320 column coverage");
	int avg640b = lit_columns_reaching_output(bgr555_to_rgb565_640_to_320_avg2, 640, 4);
	assert_eq(avg640b, 4, "avg2 640->320 column coverage");
	printf("  averaging kernels lose 0 columns (512 and 640)      OK\n");
}

/* ---- 4. the _avg2 kernels really do average the two source rows ----------- */
static void test_row_pairing(void)
{
	static uint16_t src[VRAM_W * 2];
	static uint32_t dst[160];
	const unsigned a = 4, b = 28;                 /* avg = 16 */

	for (unsigned i = 0; i < VRAM_W; i++)         src[i]          = mk(a, a, a);
	for (unsigned i = 0; i < VRAM_W; i++)         src[i + VRAM_W] = mk(b, b, b);

	struct { const char *name; void (*fn)(void *, const void *, int); } t[] = {
		{ "512->320 avg2", bgr555_to_rgb565_512_to_320_avg2 },
		{ "640->320 avg2", bgr555_to_rgb565_640_to_320_avg2 },
		{ "avg2 (1:1)",    bgr555_to_rgb565_avg2 },
	};
	for (unsigned k = 0; k < sizeof t / sizeof *t; k++) {
		memset(dst, 0, sizeof dst);
		t[k].fn(dst, src, 320);
		/* 5-bit (a+b)/2 = 16, promoted to 565: R/B keep 5 bits, G gains a 0 LSB */
		assert_eq(R565(dst[0]), (a + b) / 2, "%s R", t[k].name);
		assert_eq(B565(dst[0]), (a + b) / 2, "%s B", t[k].name);
		assert_eq(G565(dst[0]), ((a + b) / 2) << 1, "%s G", t[k].name);
	}

	/* and the non-avg2 kernels must NOT touch the second row */
	for (unsigned i = 0; i < VRAM_W; i++) src[i + VRAM_W] = mk(31, 31, 31);
	memset(dst, 0, sizeof dst);
	bgr555_to_rgb565_512_to_320_avg(dst, src, 320);
	assert_eq(R565(dst[0]), a, "512 avg must ignore row 2");
	printf("  avg2 averages the row pair; avg ignores it          OK\n");
}

int main(void)
{
	printf("cspace averaging downscalers:\n");
	test_avg_exact();
	test_no_drift();
	test_column_coverage();
	test_row_pairing();
	printf("all OK\n");
	return 0;
}
