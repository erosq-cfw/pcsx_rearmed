/*
 * erosq perf/gte-rtp: bit-exact fast RTPS/RTPT (sf=1, lm=0) for the X1000,
 * selected at startup by EROSQ_GTERTP (see gtertp_select() at the end):
 *   0 / unset  off: mipsrec keeps gte.c's handlers (dispatch as before)
 *   1          fast RTPS + fast RTPT, vertex loop unrolled
 *   2          fast RTPS + fast RTPT, vertex loop rolled (smaller, I-cache)
 * Built only into mipsrec builds, as the LAST object (Makefile), so all of its
 * code lands after every pre-existing .text symbol: the PGO layout of the
 * shipped binary does not move.
 *
 * Same results as gte.c gteRTPS_sf1lm0/gteRTPT_sf1lm0 for ALL written GTE
 * state (FLAG, MAC0-3, IR0-3, SXY/SZ FIFOs; untouched halves stay untouched).
 * What changes vs gte.c is only how the arithmetic is formed:
 *
 * 1. MAC1-3. One per-call guard (TR and R are constant across RTPT's 3
 *    vertices):  |TR| < 2^31 - 3*2^18  (the shipped MAC123_FAST_OK bound: no
 *    44-bit step overflow => FLAG bits 30-25 provably 0)  AND  every R in
 *    [-16384, 16383]  (=> |R*V| <= 2^29, |p1+p2+p3| <= 3*2^29 < 2^31), or
 *    failing that every row has |r1|+|r2|+|r3| <= 65535 (=> |P| < 2^31): the
 *    dot product is EXACT in 32 bits. Then
 *        (((s64)TR << 12) + p1 + p2 + p3) >> 12  ==  TR + ((p1 + p2 + p3) >> 12)
 *    exactly (TR << 12 has no low bits), i.e. 3 mul + 2 addu + sra + addu per
 *    row instead of a 64-bit carry chain. A call failing the guard runs the
 *    unchanged gte.c handler (0 of the 1.6M captured CTR/Spyro/GT2/Crash calls).
 * 2. Matrix/TR loaded once per call (gcc keeps them in registers).
 * 3. UNR divide inlined (no jal/jr, no caller-saved spills around it).
 * 4. SX/SY/MAC0: OF + IR*Q via an explicit `mult` (inline asm): gcc 14 was
 *    emitting a full 64x64 multiply emulation (multu + 2 mul + sign fixups)
 *    for these (s64)s32*s32 products, in gte.c and here alike.
 * 5. Saturations stay exact: each clamp is a compare + rare branch that ORs
 *    the same FLAG bit gte.c sets.
 */
#include <stdio.h>
#include <stdlib.h>
#include "gte.h"
#include "gte_divider.h"
#include "../include/compiler_features.h"

#define RTP_PRAGMA_(x) _Pragma(#x)
#define RTP_PRAGMA(x) RTP_PRAGMA_(x)

#define DP(i)  (regs->CP2D.p[i])
#define DR(i)  (regs->CP2D.r[i])
#define CP(i)  (regs->CP2C.p[i])
#define CRS(i) (((s32 *)regs->CP2C.r)[i])

#define RTP_FAST_LIM 0x7ff40000u /* == gte.c MAC123_FAST_LIM */
#define RTP_FAST_OK(v) ((u32)((v) ^ ((v) >> 31)) < RTP_FAST_LIM)

/* the unchanged exact handlers (gte.c) */
void gteRTPS_sf1lm0(psxCP2Regs *regs, u32 code);
void gteRTPT_sf1lm0(psxCP2Regs *regs, u32 code);

/* signed 32x32->64 in HI/LO (gcc otherwise emulates 64x64 here) */
static inline force_inline s64 rtp_smul(s32 a, s32 b)
{
#ifdef __mips__
	s64 r;
	__asm__("mult %1,%2" : "=x"(r) : "r"(a), "r"(b));
	return r;
#else
	return (s64)a * b;
#endif
}

/* exact under the per-call guard (|R| < 2^14, |V| <= 2^15) */
#define RTP_DOT(tr, a1, b1, a2, b2, a3, b3) ((tr) + (((a1) * (b1) + (a2) * (b2) + (a3) * (b3)) >> 12))

/* gte_divider.c DIVIDE(), inlined; caller guarantees h < 2*d (so d >= 1) */
static inline force_inline u32 rtp_unr(u32 numerator, u32 denominator)
{
	int shift = __builtin_clz(denominator) - 16;
	int r1 = (denominator << shift) & 0x7fff;
	int r2 = gte_divider_tab[(r1 + 0x40) >> 7] + 0x101;
	int r3 = ((0x80 - r2 * (r1 + 0x8000)) >> 8) & 0x1ffff;
	u32 reciprocal = (r2 * r3 + 0x80) >> 8;
	u64 p = (u64)reciprocal * (u32)(numerator << shift);
	u32 lo = (u32)p, hi = (u32)(p >> 32);
	return (hi << 16) + (lo >> 16) + ((lo >> 15) & 1);
}

/* OF + IR*Q (s64) -> mac0flags + limG on (s32)(v >> 16). |IR| <= 2^15, Q <= 0x1ffff */
static inline force_inline s32 rtp_sxy(u32 *flags, s32 of, s32 ir, s32 q, u32 gflag)
{
	s64 v = (s64)of + rtp_smul(ir, q);
	s32 r;
	if (unlikely(v != (s32)v))
		*flags |= 1u << (16 + (s32)(v >> 63));
	r = (s32)(v >> 16);
	if (unlikely((u32)(r + 0x400) > 0x7ffu)) {
		*flags |= gflag;
		r = r < 0 ? -0x400 : 0x3ff;
	}
	return r;
}

struct rtp_m { s32 r11, r12, r13, r21, r22, r23, r31, r32, r33, trx, try_, trz; };

/* one vertex: returns quotient; writes sz3 (u16 store value), packed sxy */
static inline force_inline s32 rtp_vertex(const struct rtp_m *m, u32 *flags, u32 h,
	s32 ofx, s32 ofy, s32 vx, s32 vy, s32 vz,
	s32 *mac1o, s32 *mac2o, s32 *mac3o, s32 *ir1o, s32 *ir2o, u32 *sz3o, u32 *sxyo)
{
	s32 mac1 = RTP_DOT(m->trx,  m->r11, vx, m->r12, vy, m->r13, vz);
	s32 mac2 = RTP_DOT(m->try_, m->r21, vx, m->r22, vy, m->r23, vz);
	s32 mac3 = RTP_DOT(m->trz,  m->r31, vx, m->r32, vy, m->r33, vz);
	s32 ir1 = mac1, ir2 = mac2, sz3 = mac3, q;

	if (unlikely((u32)(ir1 + 0x8000) > 0xffffu)) { *flags |= 1u << 24; ir1 = ir1 < 0 ? -0x8000 : 0x7fff; }
	if (unlikely((u32)(ir2 + 0x8000) > 0xffffu)) { *flags |= 1u << 23; ir2 = ir2 < 0 ? -0x8000 : 0x7fff; }
	if (unlikely((u32)(sz3 + 0x8000) > 0xffffu)) *flags |= 1u << 22;   /* limBF3 */
	if (unlikely((u32)sz3 > 0xffffu)) { *flags |= 1u << 18; sz3 = sz3 < 0 ? 0 : 0xffff; } /* limD */

	if (likely(h < (u32)sz3 * 2u)) {
		q = rtp_unr(h, sz3);
		if (unlikely(q > 0x1ffff)) q = 0x1ffff;
	} else {
		*flags |= 1u << 17;
		q = 0x1ffff;
	}
	*sxyo = (u16)rtp_sxy(flags, ofx, ir1, q, 1u << 14)
	      | ((u32)rtp_sxy(flags, ofy, ir2, q, 1u << 13) << 16);
	*sz3o = sz3;
	*mac1o = mac1; *mac2o = mac2; *mac3o = mac3; *ir1o = ir1; *ir2o = ir2;
	return q;
}

/* MAC0/IR0 depth cue + IR3 + final flag; common tail */
static inline force_inline void rtp_tail(psxCP2Regs *regs, u32 flags, s32 q,
	s32 mac1, s32 mac2, s32 mac3, s32 ir1, s32 ir2)
{
	s64 mac0 = (s64)CRS(28) + rtp_smul(CP(27).sw.l, q);   /* DQB + DQA*q */
	s32 ir0, ir3 = mac3;
	if (unlikely(mac0 != (s32)mac0))
		flags |= 1u << (16 + (s32)(mac0 >> 63));
	ir0 = (s32)(mac0 >> 12);
	if (unlikely((u32)ir0 > 0x1000u)) { flags |= 1u << 12; ir0 = ir0 < 0 ? 0 : 0x1000; }
	if (unlikely((u32)(ir3 + 0x8000) > 0xffffu)) ir3 = ir3 < 0 ? -0x8000 : 0x7fff; /* flag 0 */

	((s32 *)regs->CP2D.r)[25] = mac1;
	((s32 *)regs->CP2D.r)[26] = mac2;
	((s32 *)regs->CP2D.r)[27] = mac3;
	DP(9).sw.l = ir1;
	DP(10).sw.l = ir2;
	DP(11).sw.l = ir3;
	((s32 *)regs->CP2D.r)[24] = (s32)mac0;
	DP(8).sw.l = ir0;
	/* getFinalFlag */
	flags |= ~((flags & 0x7f87e000u) - 1) & (1u << 31);
	regs->CP2C.r[31] = flags;
}

/* per-call guard: TR within the shipped MAC fast-path bound, and the dot
 * product provably exact in 32 bits: cheap test all R in [-2^14, 2^14)
 * (|P| <= 3*2^29), else (lazily, CTR ~6% of calls) every row's
 * |r1|+|r2|+|r3| <= 65535 (|P| <= 65535*2^15 < 2^31). */
static inline force_inline u32 rtp_abs(s32 r) { return r < 0 ? -r : r; }
#define RTP_ROW_OK(a, b, c) (rtp_abs(a) + rtp_abs(b) + rtp_abs(c) <= 65535u)
#define RTP_GUARD_OK(m) \
	((RTP_FAST_OK((m).trx) & RTP_FAST_OK((m).try_) & RTP_FAST_OK((m).trz)) && \
	 (likely((u32)(((m).r11 + 0x4000) | ((m).r12 + 0x4000) | ((m).r13 + 0x4000) | \
	               ((m).r21 + 0x4000) | ((m).r22 + 0x4000) | ((m).r23 + 0x4000) | \
	               ((m).r31 + 0x4000) | ((m).r32 + 0x4000) | ((m).r33 + 0x4000)) < 0x8000u) || \
	  (RTP_ROW_OK((m).r11, (m).r12, (m).r13) & RTP_ROW_OK((m).r21, (m).r22, (m).r23) & \
	   RTP_ROW_OK((m).r31, (m).r32, (m).r33))))

#define RTP_LOAD_M(m) \
	(m).trx = CRS(5); (m).try_ = CRS(6); (m).trz = CRS(7); \
	(m).r11 = CP(0).sw.l; (m).r12 = CP(0).sw.h; (m).r13 = CP(1).sw.l; \
	(m).r21 = CP(1).sw.h; (m).r22 = CP(2).sw.l; (m).r23 = CP(2).sw.h; \
	(m).r31 = CP(3).sw.l; (m).r32 = CP(3).sw.h; (m).r33 = CP(4).sw.l

void no_stackprotector gteRTPS_sf1lm0_x(psxCP2Regs *regs, u32 code)
{
	struct rtp_m m;
	s32 mac1, mac2, mac3, ir1, ir2, q;
	u32 sz3, sxy, flags = 0;

	RTP_LOAD_M(m);
	if (unlikely(!RTP_GUARD_OK(m))) {
		gteRTPS_sf1lm0(regs, code);
		return;
	}
	q = rtp_vertex(&m, &flags, CP(26).w.l, CRS(24), CRS(25),
		DP(0).sw.l, DP(0).sw.h, DP(1).sw.l,
		&mac1, &mac2, &mac3, &ir1, &ir2, &sz3, &sxy);
	DP(16).w.l = DP(17).w.l;    /* SZ FIFO (16-bit, as gte.c) */
	DP(17).w.l = DP(18).w.l;
	DP(18).w.l = DP(19).w.l;
	DP(19).w.l = sz3;
	DR(12) = DR(13);            /* SXY FIFO; SXYP untouched, as gte.c */
	DR(13) = DR(14);
	DR(14) = sxy;
	rtp_tail(regs, flags, q, mac1, mac2, mac3, ir1, ir2);
}

/* RTPT, vertex loop unrolled UNROLL times (3 = fully, 1 = rolled) */
#define RTP_DEF_RTPT(NAME, UNROLL) \
void no_stackprotector NAME(psxCP2Regs *regs, u32 code) \
{ \
	struct rtp_m m; \
	s32 mac1, mac2, mac3, ir1, ir2, q, ofx, ofy; \
	u32 sz3, sxy, h, flags = 0; \
	int v; \
 \
	RTP_LOAD_M(m); \
	if (unlikely(!RTP_GUARD_OK(m))) { \
		gteRTPT_sf1lm0(regs, code); \
		return; \
	} \
	h = CP(26).w.l; ofx = CRS(24); ofy = CRS(25); \
	DP(16).w.l = DP(19).w.l;    /* SZ0 = SZ3 */ \
	RTP_PRAGMA(GCC unroll UNROLL) \
	for (v = 0; v < 3; v++) { \
		q = rtp_vertex(&m, &flags, h, ofx, ofy, \
			DP(v * 2).sw.l, DP(v * 2).sw.h, DP(v * 2 + 1).sw.l, \
			&mac1, &mac2, &mac3, &ir1, &ir2, &sz3, &sxy); \
		DP(17 + v).w.l = sz3; \
		DR(12 + v) = sxy; \
	} \
	rtp_tail(regs, flags, q, mac1, mac2, mac3, ir1, ir2); \
}

void gteRTPT_sf1lm0_x(psxCP2Regs *regs, u32 code);
void gteRTPT_sf1lm0_xr(psxCP2Regs *regs, u32 code);
RTP_DEF_RTPT(gteRTPT_sf1lm0_x, 3)    /* EROSQ_GTERTP=1 */
RTP_DEF_RTPT(gteRTPT_sf1lm0_xr, 1)   /* EROSQ_GTERTP=2 */

/* ---- selector ------------------------------------------------------------
 * mipsrec/plugin.c resolves gte_h_RTPS / gte_h_RTPT (the handlers the JIT
 * bakes a direct JAL to, and its shims call) in constructors; this object is
 * linked after plugin.o, so its constructor runs after those (.init_array is
 * in link order) and swaps in the fast handlers. If they are not the expected
 * gte.c handlers yet it says so and stays off rather than being silently
 * overwritten. Own section: a plain constructor goes to .text.startup, which
 * precedes .text.hot and would shift every hot symbol of the PGO layout.
 * gtertp_mode is read by jitsched.cpp: with EROSQ_GTENF on, the selected fast
 * RTPS/RTPT is kept instead of being swapped for the flagless _nf handler. */
extern gte_handler *gte_h_RTPS, *gte_h_RTPT;
int gtertp_mode;

#ifdef GTERTP_SHADOW
/* HOST/HEADLESS ONLY (-DGTERTP_SHADOW; never in the device build): every
 * RTPS/RTPT runs both the fast path and the original gte.c handler on copies
 * of the full CP2 state, compares all 64 words, aborts on the first mismatch,
 * and hands the ORIGINAL handler's state back, so emulation is unchanged. */
#include <string.h>
static gte_handler *sv_fast[2], *sv_ref[2];
static unsigned long sv_n[2], sv_pass[2], sv_bad[2];
static const char *const sv_name[2] = { "RTPS", "RTPT" };
/* 1 = this CP2 state takes the fast path (else the call falls back to gte.c) */
static int sv_guard_ok(const psxCP2Regs *regs)
{
	struct rtp_m m;
	RTP_LOAD_M(m);
	return RTP_GUARD_OK(m);
}
void gtertp_sv_report(void)
{
	int k;
	for (k = 0; k < 2; k++)
		fprintf(stderr, "gtertp-sv: %s verified %lu guard-pass %lu fallback %lu mismatches %lu\n",
			sv_name[k], sv_n[k], sv_pass[k], sv_n[k] - sv_pass[k], sv_bad[k]);
	fflush(stderr);
}
static void sv_run(int k, psxCP2Regs *regs, u32 code)
{
	psxCP2Regs in = *regs, a = *regs, b = *regs;
	const int pass = sv_guard_ok(regs);
	int i;
	sv_fast[k](&a, code);
	sv_ref[k](&b, code);
	if (memcmp(&a, &b, sizeof(a))) {
		fprintf(stderr, "gtertp-sv: MISMATCH %s call #%lu code=%08x guard=%s mode=%d\n",
			sv_name[k], sv_n[k] + 1, code, pass ? "pass" : "fallback", gtertp_mode);
		for (i = 0; i < 64; i++) {
			const u32 x = ((u32 *)&in)[i], f = ((u32 *)&a)[i], r = ((u32 *)&b)[i];
			fprintf(stderr, "  %s%-2d in=%08x fast=%08x ref=%08x%s\n", i < 32 ? "D" : "C", i & 31,
				x, f, r, f != r ? "  <<<" : "");
		}
		sv_bad[k]++;
		gtertp_sv_report();
		abort();
	}
	*regs = b;
	sv_n[k]++;
	sv_pass[k] += pass;
}
static void sv_RTPS(psxCP2Regs *regs, u32 code) { sv_run(0, regs, code); }
static void sv_RTPT(psxCP2Regs *regs, u32 code) { sv_run(1, regs, code); }
#endif

static void __attribute__((constructor, section(".text.gtertp_sel"))) gtertp_select(void)
{
	const char *e = getenv("EROSQ_GTERTP");
	gte_handler *rtpt;
	int m = e ? atoi(e) : 0;

	if (m != 1 && m != 2) {
		if (m)
			fprintf(stderr, "gtertp: EROSQ_GTERTP=%s unknown (0 off, 1 unrolled, 2 rolled): off\n", e);
		return;
	}
	if (gte_h_RTPS != gteRTPS_sf1lm0 || gte_h_RTPT != gteRTPT_sf1lm0) {
		fprintf(stderr, "gtertp: RTPS/RTPT handlers not resolved yet (%p %p): off\n",
			(void *)gte_h_RTPS, (void *)gte_h_RTPT);
		return;
	}
	rtpt = m == 1 ? gteRTPT_sf1lm0_x : gteRTPT_sf1lm0_xr;
	gtertp_mode = m;
#ifdef GTERTP_SHADOW
	sv_fast[0] = gteRTPS_sf1lm0_x; sv_ref[0] = gte_h_RTPS;
	sv_fast[1] = rtpt;             sv_ref[1] = gte_h_RTPT;
	gte_h_RTPS = sv_RTPS;
	gte_h_RTPT = sv_RTPT;
	fprintf(stderr, "gtertp: EROSQ_GTERTP=%d (RTPS fast, RTPT %s) SHADOW-VERIFY\n", m, m == 1 ? "unrolled" : "rolled");
#else
	gte_h_RTPS = gteRTPS_sf1lm0_x;
	gte_h_RTPT = rtpt;
	fprintf(stderr, "gtertp: EROSQ_GTERTP=%d (RTPS fast, RTPT %s)\n", m, m == 1 ? "unrolled" : "rolled");
#endif
}
