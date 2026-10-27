/* test_gte_rtp (erosq perf/gte-rtp): build: scratchpad gte/build.sh recipe -- ship flags (-Ofast -B gcc-xburst-cc1 -mtune=24kc -fno-pic -mno-abicalls -G0), link gte.o gte_divider.o gte_rtp.o + stubs (psxRegs), run under qemu-mipsel.
 * difftest: gte.c gteRTPS/RTPT_sf1lm0 (reference) vs gte_rtp.c *_x (prototype).
 * Compares ALL 64 CP2 words (+ the untouched-half semantics) after each call.
 *   difftest rand N SEED        random + edge-biased states
 *   difftest cap FILE...        captured states (256 B CP2 + 4 B tag each)
 *   difftest bench WHICH FILE   WHICH=ref|x : run only that impl over FILE, getppid() marks
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include "psxcommon.h"
#include "r3000a.h"
#include "gte.h"

void gteRTPS_sf1lm0(psxCP2Regs *regs, u32 code);
void gteRTPT_sf1lm0(psxCP2Regs *regs, u32 code);
void gteRTPS_sf1lm0_x(psxCP2Regs *regs, u32 code);
void gteRTPT_sf1lm0_x(psxCP2Regs *regs, u32 code);

#define CODE_RTPS (0x01u | (1u << 19))
#define CODE_RTPT (0x30u | (1u << 19))

static const uint16_t divov[][2] = {
#include "test_gte_rtp_divover.inc"  /* the 40 (h,d) pairs where UNR gives 0x20000 (host-exhaustive) */
};
static uint64_t s;
static uint32_t rnd(void) { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return (uint32_t)s; }

static const int32_t e16[] = { 0, 1, -1, 0x3fff, 0x7fff, -0x8000, -0x7fff, -0x4001, 0x1000, -0x1000, 0x4000, -0x4000, 0x3ff, 0x400, -0x400, -0x401 };
static int32_t v16(void)
{
	switch (rnd() & 7) {
	case 0: case 1: return e16[rnd() & 15];
	case 2: case 3: return (int16_t)rnd();
	case 4: return (int16_t)(rnd() & 0x1fff) - 0x1000;   /* small */
	default: return (int32_t)(rnd() % 8193) - 4096;      /* realistic 4.12 / coords */
	}
}
static const int32_t etr[] = { 0, 1, -1, 0x7ff3ffff, 0x7ff40000, -0x7ff3ffff, -0x7ff40000, -0x7ff40001,
	0x7fffffff, (int32_t)0x80000000, 0x7fffff, -0x800000, 0x10000, -0x10000, 0x7ff3fffe, 0x40000000 };
static int32_t vtr(void)
{
	switch (rnd() & 7) {
	case 0: return etr[rnd() & 15];
	case 1: return (int32_t)rnd();
	case 2: return (int32_t)(rnd() & 0xffffff) - 0x800000;
	default: return (int32_t)(rnd() & 0x3ffff) - 0x20000;
	}
}
static int32_t vof(void)
{
	switch (rnd() & 3) {
	case 0: return (int32_t)rnd();
	case 1: { static const int32_t e[] = { 0, 0x7fffffff, (int32_t)0x80000000, 160 << 16, 120 << 16, -1, 0x3ff << 16, -0x400 << 16 }; return e[rnd() & 7]; }
	default: return (int32_t)(rnd() & 0x3ffffff) - 0x2000000;
	}
}
static uint32_t vh(void)
{
	switch (rnd() & 3) {
	case 0: { static const uint32_t e[] = { 0, 1, 0xffff, 0x7fff, 0x8000, 0xfffffffe, 256, 0x10000 }; return e[rnd() & 7]; }
	case 1: return rnd();
	default: return (uint32_t)(int32_t)(int16_t)(rnd() % 1200); /* CTC2 sign-extends */
	}
}

static void gen(psxCP2Regs *r)
{
	int i;
	for (i = 0; i < 32; i++) { r->CP2D.r[i] = rnd(); r->CP2C.r[i] = rnd(); }
	for (i = 0; i < 6; i++) { r->CP2D.p[i].sw.l = v16(); r->CP2D.p[i].sw.h = v16(); }
	if (rnd() & 1) /* matrix often "rotation-like" */
		for (i = 0; i < 5; i++) { r->CP2C.p[i].sw.l = v16(); r->CP2C.p[i].sw.h = v16(); }
	else
		for (i = 0; i < 5; i++) { r->CP2C.p[i].sw.l = (int32_t)(rnd() % 8193) - 4096; r->CP2C.p[i].sw.h = (int32_t)(rnd() % 8193) - 4096; }
	if (rnd() & 1) {   /* push MAC3 near the SZ / divide-overflow boundary */
		int32_t trz = (int32_t)(rnd() % 0x20000) - 0x100;
		((int32_t *)r->CP2C.r)[7] = trz;
	} else ((int32_t *)r->CP2C.r)[7] = vtr();
	((int32_t *)r->CP2C.r)[5] = vtr();
	((int32_t *)r->CP2C.r)[6] = vtr();
	((int32_t *)r->CP2C.r)[24] = vof();
	((int32_t *)r->CP2C.r)[25] = vof();
	r->CP2C.r[26] = vh();
	r->CP2C.r[27] = (rnd() & 3) ? (uint32_t)(int32_t)v16() : rnd();   /* DQA (sw.l used) */
	((int32_t *)r->CP2C.r)[28] = (rnd() & 1) ? (int32_t)rnd() : vof(); /* DQB */
	if ((rnd() & 15) == 0) { /* 44-bit MAC overflow edges: max products + TR at/over the guard */
		static const int32_t mx[] = { -0x8000, 0x7fff, -0x8000, -0x7fff };
		int32_t vv = mx[rnd() & 3], mm = mx[rnd() & 3];
		for (i = 0; i < 6; i++) { r->CP2D.p[i].sw.l = (rnd() & 7) ? vv : v16(); r->CP2D.p[i].sw.h = (rnd() & 7) ? vv : v16(); }
		for (i = 0; i < 5; i++) { r->CP2C.p[i].sw.l = (rnd() & 7) ? mm : v16(); r->CP2C.p[i].sw.h = (rnd() & 7) ? -mm : mm; }
		for (i = 5; i < 8; i++) ((int32_t *)r->CP2C.r)[i] = (rnd() & 1) ? etr[rnd() & 15] : (int32_t)(etr[rnd() & 15] + (int32_t)(rnd() % 0x80000) - 0x40000);
	}
	if ((rnd() & 15) == 0) { /* fast-path extremes: |R| at the 2^14 guard edge, |V| max, TR at the TR guard edge */
		static const int32_t rm[] = { -0x4000, 0x3fff, -0x3fff, -0x4000, 0x4000, -0x4001, 0x3fff, -0x4000 };
		static const int32_t vm[] = { -0x8000, 0x7fff, -0x8000, -0x7fff };
		int32_t vv = vm[rnd() & 3];
		for (i = 0; i < 6; i++) { r->CP2D.p[i].sw.l = (rnd() & 7) ? vv : v16(); r->CP2D.p[i].sw.h = (rnd() & 7) ? -vv : vv; }
		for (i = 0; i < 5; i++) { r->CP2C.p[i].sw.l = rm[rnd() & ((rnd() & 3) ? 3 : 7)]; r->CP2C.p[i].sw.h = rm[rnd() & ((rnd() & 3) ? 3 : 7)]; }
		for (i = 5; i < 8; i++) ((int32_t *)r->CP2C.r)[i] = (rnd() & 1) ? etr[rnd() & 15] : (int32_t)(rnd() % 0x40000) - 0x20000;
	}
	if ((rnd() & 15) == 0) { /* row-abs-sum guard edge: sum |r| = 65535 (fast) / 65536 (fallback), V = +-32768/32767 aligned */
		static const int32_t rr[][3] = { {32767,32767,1}, {32767,32767,2}, {-32768,32767,0}, {-32768,-32768,0},
			{-32768,32767,-1}, {21845,21845,21845}, {21845,21845,21846}, {-21845,-21845,-21845} };
		int32_t sv = (rnd() & 1) ? -0x8000 : 0x7fff;
		for (i = 0; i < 3; i++) {
			const int32_t *w = rr[rnd() & 7];
			int32_t *m = (int32_t *)0; (void)m;
			/* rows: (p0.l p0.h p1.l) (p1.h p2.l p2.h) (p3.l p3.h p4.l) */
			if (i == 0) { r->CP2C.p[0].sw.l = w[0]; r->CP2C.p[0].sw.h = w[1]; r->CP2C.p[1].sw.l = w[2]; }
			if (i == 1) { r->CP2C.p[1].sw.h = w[0]; r->CP2C.p[2].sw.l = w[1]; r->CP2C.p[2].sw.h = w[2]; }
			if (i == 2) { r->CP2C.p[3].sw.l = w[0]; r->CP2C.p[3].sw.h = w[1]; r->CP2C.p[4].sw.l = w[2]; }
		}
		for (i = 0; i < 6; i++) { r->CP2D.p[i].sw.l = (rnd() & 3) ? sv : -sv - 1; r->CP2D.p[i].sw.h = (rnd() & 3) ? sv : v16(); }
		for (i = 5; i < 8; i++) ((int32_t *)r->CP2C.r)[i] = (rnd() & 1) ? etr[rnd() & 15] : (int32_t)(rnd() % 0x40000) - 0x20000;
	}
	if ((rnd() & 15) == 0) { /* UNR result 0x20000 (> 0x1ffff clamp): row 3 = 0 => SZ3 = TRZ */
		const uint16_t *pr = divov[rnd() % (sizeof(divov) / sizeof(divov[0]))];
		r->CP2C.p[3].sw.l = r->CP2C.p[3].sw.h = r->CP2C.p[4].sw.l = 0;
		((int32_t *)r->CP2C.r)[7] = pr[1] + (int32_t)(rnd() % 3) - 1;
		r->CP2C.r[26] = (uint32_t)(int32_t)(int16_t)pr[0];
		return;
	}
	if ((rnd() & 7) == 0) {  /* force h >= 2*sz / sz==0 style cases via tiny Z */
		r->CP2D.p[1].sw.l = r->CP2D.p[3].sw.l = r->CP2D.p[5].sw.l = 0;
		((int32_t *)r->CP2C.r)[7] = (int32_t)(rnd() % 5) - 2;
	}
}

static long nvec, nbad;
static void show(const char *op, const psxCP2Regs *in, const psxCP2Regs *a, const psxCP2Regs *b)
{
	int i;
	if (nbad > 10) return;
	printf("MISMATCH %s\n", op);
	for (i = 0; i < 64; i++) {
		uint32_t x = ((uint32_t *)a)[i], y = ((uint32_t *)b)[i];
		if (x != y) printf("  %s%d in=%08x ref=%08x x=%08x\n", i < 32 ? "D" : "C", i & 31, ((uint32_t *)in)[i], x, y);
	}
}
static void check(const psxCP2Regs *in, int which)
{
	psxCP2Regs a, b;
	if (which & 1) {
		a = *in; b = *in;
		gteRTPS_sf1lm0(&a, CODE_RTPS); gteRTPS_sf1lm0_x(&b, CODE_RTPS);
		nvec++;
		if (memcmp(&a, &b, sizeof(a))) { nbad++; show("RTPS", in, &a, &b); }
	}
	if (which & 2) {
		a = *in; b = *in;
		gteRTPT_sf1lm0(&a, CODE_RTPT); gteRTPT_sf1lm0_x(&b, CODE_RTPT);
		nvec++;
		if (memcmp(&a, &b, sizeof(a))) { nbad++; show("RTPT", in, &a, &b); }
	}
}

struct cap { psxCP2Regs r; uint32_t tag; }; /* tag: GTE op (0x01 RTPS / 0x30 RTPT) */

int main(int argc, char **argv)
{
	long i, n;
	if (argc >= 4 && !strcmp(argv[1], "rand")) {
		n = atol(argv[2]); s = strtoull(argv[3], 0, 0) | 1;
		for (i = 0; i < n; i++) { psxCP2Regs r; gen(&r); check(&r, 3); }
	} else if (argc >= 3 && !strcmp(argv[1], "cap")) {
		int f;
		for (f = 2; f < argc; f++) {
			FILE *fp = fopen(argv[f], "rb"); struct cap c; long k = 0, nb0 = nbad;
			if (!fp) { perror(argv[f]); return 2; }
			while (fread(&c, sizeof(c), 1, fp) == 1) {
				/* run each capture through BOTH ops: same real state, double coverage */
				check(&c.r, 3); k++;
			}
			fclose(fp);
			printf("%s: %ld states, %ld mismatches\n", argv[f], k, nbad - nb0);
		}
	} else if (argc >= 4 && !strcmp(argv[1], "bench")) {
		int x = !strcmp(argv[2], "x"), none = !strcmp(argv[2], "none");
		u32 only = argc >= 5 ? (u32)strtoul(argv[4], 0, 0) : 0;   /* 0x01 / 0x30: keep one op */
		FILE *fp = fopen(argv[3], "rb"); struct cap c;
		static struct cap buf[20000]; long m = 0, cs = 0, ct = 0;
		while (m < 20000 && fread(&c, sizeof(c), 1, fp) == 1) if (!only || c.tag == only) buf[m++] = c;
		fclose(fp);
		syscall(4064); /* getppid: stallmodel window open */
		for (i = 0; i < m; i++) {
			psxCP2Regs r = buf[i].r;
			if (buf[i].tag == 0x30) { ct++; if (!none) (x ? gteRTPT_sf1lm0_x : gteRTPT_sf1lm0)(&r, CODE_RTPT); }
			else { cs++; if (!none) (x ? gteRTPS_sf1lm0_x : gteRTPS_sf1lm0)(&r, CODE_RTPS); }
			__asm__ volatile("" ::: "memory");
			buf[i].r = r;
		}
		syscall(4064);
		printf("bench %s: %ld RTPS %ld RTPT\n", argv[2], cs, ct);
		return 0;
	} else { fprintf(stderr, "usage\n"); return 2; }
	printf("vectors %ld mismatches %ld\n", nvec, nbad);
	return nbad != 0;
}
