/* EROSQ host differential test: the X1000 MXU lit-textured POLY span kernels
 * (poly_mxu_tx_lit via PolySpanMaybeMXU) executed op-by-op through exact C
 * models of the raw MXU instructions, against the scalar gpuPolySpanFn.
 *
 * Unlike test_sprite_tile_kernels.cpp (which swaps mxu_light_pair for a C
 * model via GPU_MXU_HOST_TEST), this runs the PRODUCTION op sequence of
 * mxu_light_pair / mxu_gouraud_* / mxu_blend_pair_sub, so a reordering of
 * those ops is checked for real. Per-op semantics follow QEMU
 * mxu_translate.c, the reference the silicon kernels were validated against.
 *
 * Gouraud inputs are drawn so no 16-bit lane wraps within a span: there the
 * scalar gcol_t u32 adds and the MXU per-lane adds agree by construction
 * (they legitimately differ only on lane carry-spill, see gpu_inner_mxu.h).
 *
 * Build & run (host, no deps):
 *   g++ -O2 -w -o /tmp/test_mxu_raw_poly plugins/gpu_unai/test_mxu_raw_poly.cpp \
 *     && /tmp/test_mxu_raw_poly
 */
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

typedef uint32_t u32_pre;
static u32_pre host_xr[17];

#define GPU_UNAI_USE_MXU
#define MXU_X1000_H
#define MXU_XR0 0
#define MXU_XR1 1
#define MXU_XR2 2
#define MXU_XR3 3
#define MXU_XR4 4
#define MXU_XR5 5
#define MXU_XR6 6
#define MXU_XR7 7
#define MXU_XR8 8
#define MXU_XR9 9
#define MXU_XR10 10
#define MXU_XR11 11
#define MXU_XR12 12
#define MXU_XR13 13
#define MXU_XR14 14
#define MXU_XR15 15
#define MXU_XR16 16
#define MXU_WW 0
#define MXU_LW 1
#define MXU_HW 2
#define MXU_XW 3
#define MXU_AA 0
#define MXU_AS 1
#define MXU_SA 2
#define MXU_SS 3

static inline void hx_set(int r, u32_pre v) { if (r) host_xr[r] = v; }
static inline u32_pre hx_lane(u32_pre x, int l) { return (x >> (16 * l)) & 0xffff; }
static inline u32_pre hx_pack(u32_pre hi, u32_pre lo) { return ((hi & 0xffff) << 16) | (lo & 0xffff); }

#define MXU_S32I2M(r, v)  hx_set((r), (u32_pre)(v))
#define MXU_S32M2I(r)     (host_xr[(r)])
#define MXU_S32LDD(r, b)  do { u32_pre _v; memcpy(&_v, (const void *)(b), 4); hx_set((r), _v); } while (0)
#define MXU_S32STD(r, b)  do { u32_pre _v = host_xr[(r)]; memcpy((void *)(b), &_v, 4); } while (0)
#define MXU_S32LDDO(r, b, off) do { u32_pre _v; memcpy(&_v, (const char *)(b) + (off), 4); hx_set((r), _v); } while (0)
#define MXU_S32STDO(r, b, off) do { u32_pre _v = host_xr[(r)]; memcpy((char *)(b) + (off), &_v, 4); } while (0)
#define MXU_S32SFL(a, b, c, d, ptn2) do { \
	if ((ptn2) != 3) { fprintf(stderr, "model: only SFL ptn2=3\n"); exit(2); } \
	u32_pre _b = host_xr[(b)], _c = host_xr[(c)]; \
	hx_set((a), (_b & 0xffff0000u) | (_c >> 16)); hx_set((d), (_b << 16) | (_c & 0xffffu)); } while (0)
#define MXU_S32AND(a, b, c) hx_set((a), host_xr[(b)] & host_xr[(c)])
#define MXU_S32OR(a, b, c)  hx_set((a), host_xr[(b)] | host_xr[(c)])
#define MXU_S32XOR(a, b, c) hx_set((a), host_xr[(b)] ^ host_xr[(c)])

static inline u32_pre hx_q16sh(u32_pre x, int s, int kind)
{
	u32_pre out = 0;
	for (int l = 0; l < 2; l++) {
		u32_pre v = hx_lane(x, l), r;
		if (kind == 0)      r = (v << s) & 0xffff;              /* SLL */
		else if (kind == 1) r = v >> s;                         /* SLR */
		else                r = (u32_pre)((int16_t)v >> s) & 0xffff; /* SAR */
		out |= r << (16 * l);
	}
	return out;
}
#define HX_Q16SH(a, b, c, d, s, k) do { u32_pre _x = hx_q16sh(host_xr[(b)], (s), (k)), _y = hx_q16sh(host_xr[(c)], (s), (k)); \
	hx_set((a), _x); hx_set((d), _y); } while (0)
#define MXU_Q16SLL(a, b, c, d, s) HX_Q16SH(a, b, c, d, s, 0)
#define MXU_Q16SLR(a, b, c, d, s) HX_Q16SH(a, b, c, d, s, 1)
#define MXU_Q16SAR(a, b, c, d, s) HX_Q16SH(a, b, c, d, s, 2)

/* Q16ADD optn2=WW: XRa = b (+/-) c per aptn2's first op, XRd = b (+/-) c per
 * the second op (AA: both add, SS: both subtract, AS/SA mixed). */
static inline u32_pre hx_q16addsub(u32_pre b, u32_pre c, int sub)
{
	u32_pre out = 0;
	for (int l = 0; l < 2; l++) {
		u32_pre v = sub ? hx_lane(b, l) - hx_lane(c, l) : hx_lane(b, l) + hx_lane(c, l);
		out |= (v & 0xffff) << (16 * l);
	}
	return out;
}
#define MXU_Q16ADD(a, b, c, d, optn2, aptn2) do { \
	if ((optn2) != MXU_WW) { fprintf(stderr, "model: Q16ADD only WW\n"); exit(2); } \
	u32_pre _x = hx_q16addsub(host_xr[(b)], host_xr[(c)], (aptn2) == MXU_SS || (aptn2) == MXU_SA); \
	u32_pre _y = hx_q16addsub(host_xr[(b)], host_xr[(c)], (aptn2) == MXU_SS || (aptn2) == MXU_AS); \
	hx_set((a), _x); hx_set((d), _y); } while (0)

/* D16MADL aptn2=AA optn2=WW: per lane XRd = XRa + low16(XRb * XRc) (signed) */
#define MXU_D16MADL(a, b, c, d, aptn2, optn2) do { \
	if ((aptn2) != MXU_AA || (optn2) != MXU_WW) { fprintf(stderr, "model: D16MADL only AA/WW\n"); exit(2); } \
	u32_pre _o = 0; \
	for (int _l = 0; _l < 2; _l++) { \
		int32_t _p = (int32_t)(int16_t)hx_lane(host_xr[(b)], _l) * (int16_t)hx_lane(host_xr[(c)], _l); \
		_o |= ((hx_lane(host_xr[(a)], _l) + (u32_pre)_p) & 0xffff) << (16 * _l); \
	} \
	hx_set((d), _o); } while (0)
#define MXU_D16MIN(a, b, c) do { \
	u32_pre _o = 0; \
	for (int _l = 0; _l < 2; _l++) { \
		int16_t _x = (int16_t)hx_lane(host_xr[(b)], _l), _y = (int16_t)hx_lane(host_xr[(c)], _l); \
		_o |= ((u32_pre)(uint16_t)(_x < _y ? _x : _y)) << (16 * _l); \
	} \
	hx_set((a), _o); } while (0)
#define MXU_D16MAX(a, b, c) do { fprintf(stderr, "model: D16MAX unused\n"); exit(2); } while (0)

#include "../gpulib/gpu.h"
#include "gpu_unai.h"
#include "gpu_fixedpoint.h"
#include "gpu_inner.h"

static u32 rng_state = 0x20261016;
static u32 rnd(void) { rng_state ^= rng_state << 13; rng_state ^= rng_state >> 17; rng_state ^= rng_state << 5; return rng_state; }

static u8 tex[256 * 2048];
static le16_t clut[256];
#define ROWLEN 1100
static le16_t bufA[ROWLEN], bufB[ROWLEN];
static int fails = 0, cases = 0;
static u32 out_hash = 2166136261u;   /* FNV over every MXU-side output buffer */

/* --- 1. mxu_light_pair == LightLUT, exhaustive over channel x light ---- */
static void test_light_pair(void)
{
	int bad = 0;
	host_xr[14] = 0x001f001f; host_xr[15] = 0x80008000;
	for (u32 l = 0; l < 32; l++)
		for (u32 t = 0; t < 0x10000; t += 7) {
			u32 l2 = (l * 7 + 3) & 31;
			host_xr[7] = (l2 << 16) | l; host_xr[8] = (l << 16) | l2; host_xr[9] = ((31 - l) << 16) | l;
			u32 t1 = (t * 2654435761u) >> 16;
			u32 got = mxu_light_pair((t1 << 16) | t);
			for (int lane = 0; lane < 2; lane++) {
				u32 tt = lane ? t1 : t;
				u32 r5 = (host_xr[7] >> (16 * lane)) & 31, g5 = (host_xr[8] >> (16 * lane)) & 31, b5 = (host_xr[9] >> (16 * lane)) & 31;
				u32 want = ((u32)gpu_unai.LightLUT[((tt & 0x7c00) >> 5) | b5] << 10)
				         | ((u32)gpu_unai.LightLUT[(tt & 0x03e0) | g5] << 5)
				         |  (u32)gpu_unai.LightLUT[((tt & 0x001f) << 5) | r5]
				         | (tt & 0x8000);
				if (((got >> (16 * lane)) & 0xffff) != want && bad++ < 5)
					printf("FAIL light t=%04x l=(%u,%u,%u) got %04x want %04x\n", tt, r5, g5, b5,
					       (got >> (16 * lane)) & 0xffff, want);
			}
			cases++;
		}
	fails += bad;
	printf("  mxu_light_pair vs LightLUT: %s\n", bad ? "FAIL" : "OK");
}

/* --- 2. poly span kernels vs scalar ----------------------------------- */
template<int CF>
static void run_poly(const char *name, int trials)
{
	int bad = 0;
	for (int t = 0; t < trials; t++) {
		for (unsigned i = 0; i < sizeof tex; i++) tex[i] = (u8)rnd();
		for (int i = 0; i < 256; i++) { u16 v = (u16)rnd(); if ((rnd() & 3) == 0) v = 0; clut[i] = u16_to_le16(v); }
		for (int i = 0; i < ROWLEN; i++) bufA[i] = u16_to_le16((u16)rnd());
		memcpy(bufB, bufA, sizeof bufA);

		gpu_unai_inner_t &inn = gpu_unai.inn;
		memset(&inn, 0, sizeof inn);
		u32 count = 1 + rnd() % 64;
		u32 umask = (rnd() & 1) ? 0xff : (0xff >> (rnd() % 5));
		u32 vmask = (rnd() & 1) ? 0xff : 0x7f;
		inn.mask_v00u = (vmask << 24) | umask;
		inn.u = rnd() & 0x3ffff; inn.v = rnd() & 0x3ffff;
		inn.u_inc = (s32)(rnd() % 4096) - 2048; inn.v_inc = (s32)(rnd() % 4096) - 2048;
		inn.TBA = (le16_t *)tex;
		inn.CBA = clut;
		inn.bgr0888 = rnd() & 0xffffff;
		/* non-spilling gouraud. The scalar gcol_t += adds whole words (two
		 * u32 on MIPS32, ONE u64 on a 64-bit host), so a negative r or g
		 * increment (0xffxx as u16) carries into the next lane -- the spill
		 * the MXU path deliberately does not reproduce. Keep dr, dg >= 0 and
		 * every lane inside [0, 0xffff] for 64 steps of inc < 0x100; b is the
		 * top used lane, so it may step either way. */
		u32 r = 0x0800 + rnd() % 0xa800, g = 0x0800 + rnd() % 0xa800, b = 0x4000 + rnd() % 0x8000;
		s32 dr = (s32)(rnd() % 0x100), dg = (s32)(rnd() % 0x100), db = (s32)(rnd() % 0x200) - 0x100;
		inn.gCol.c.r = (u16)r; inn.gCol.c.g = (u16)g; inn.gCol.c.b = (u16)b;
		inn.gInc.c.r = (u16)dr; inn.gInc.c.g = (u16)dg; inn.gInc.c.b = (u16)db;
		u32 off = rnd() % (ROWLEN - 70);

		mxu_spans_on = 0;
		PolySpanMaybeMXU<CF>(gpu_unai, &bufA[off], count, 0);
		mxu_spans_on = 1;
		PolySpanMaybeMXU<CF>(gpu_unai, &bufB[off], count, 0);
		cases++;
		for (int i = 0; i < ROWLEN; i++) out_hash = (out_hash ^ le16_raw(bufB[i])) * 16777619u;
		if (memcmp(bufA, bufB, sizeof bufA)) {
			if (bad++ < 3)
				for (int i = 0; i < ROWLEN; i++)
					if (le16_raw(bufA[i]) != le16_raw(bufB[i])) {
						printf("FAIL %s trial %d: px %d scalar %04x mxu %04x [count=%u off=%u]\n", name, t,
						       i - (int)off, le16_to_u16(bufA[i]), le16_to_u16(bufB[i]), count, off);
						break;
					}
		}
	}
	fails += bad;
	printf("  %-8s %d trials: %s\n", name, trials, bad ? "FAIL" : "OK");
}

int main(void)
{
	SetupLightLUT();
	test_light_pair();
	const int N = 3000;
	run_poly<0x021>("0x021", N); run_poly<0x041>("0x041", N); run_poly<0x061>("0x061", N);
	run_poly<0x0a1>("0x0a1", N); run_poly<0x0c1>("0x0c1", N); run_poly<0x0e1>("0x0e1", N);
	run_poly<0x121>("0x121", N); run_poly<0x1a1>("0x1a1", N); run_poly<0x1c1>("0x1c1", N);
	run_poly<0x0a3>("0x0a3", N); run_poly<0x0ab>("0x0ab", N); run_poly<0x0b3>("0x0b3", N); run_poly<0x0bb>("0x0bb", N);
	run_poly<0x023>("0x023", N); run_poly<0x033>("0x033", N); run_poly<0x1b3>("0x1b3", N);
	printf("mxu raw-op poly test: %d cases, %d FAIL, mxu output hash %08x\n", cases, fails, out_hash);
	return fails ? 1 : 0;
}
