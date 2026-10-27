/* nclipj: EROSQ_NCLIPJ differential test (perf/jrld-nclip, 2026-10-23).
 *
 * Runs nclipj.S (n x {mtc2 SXY0..2; NCLIP; mfc2 MAC0; cfc2 FLAG}) through the
 * REAL mipsrec (direct-return dispatcher, as the device) or, with HX_INT=1,
 * the interpreter, and compares every MAC0/FLAG pair with the C handler the
 * JIT called before EROSQ_NCLIPJ: gte_h_NCLIP(&CP2, gte_code_NCLIP) (gte.c
 * gteNCLIP_). nclipj.sh runs it with EROSQ_NCLIPJ=1 (inline), =0 (old JAL)
 * and HX_INT=1, faultmem on and off.
 *
 * Vectors (deterministic): all 6^6 combinations of {-32768,-32767,-1,0,1,
 * 32767} for SX0..2/SY0..2; every exact sum in [B-1100, B+1100] for each
 * MAC0 overflow boundary B in {2^31-1, 2^31, -2^31, -2^31-1} (built as
 * SY = (-32768, 32767, 0) / (32767,-32768, 0) so the sum is
 * 32767*SX0 + 32768*SX1 -/+ 65535*SX2, solved for SX0/SX1); the 6 extreme
 * sums; and HX_NRAND (default 400000) xorshift-random s16 sextuples (half of
 * them drawn from the edge set per coordinate). Chunks of CHUNK vectors per
 * guest run (the code cache is kept across chunks).
 *
 * Output: "nclipj: vectors=N mismatches=M flag_set=F" (+ first mismatches),
 * exit status 1 on any mismatch or a run that did not finish.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include "psxcommon.h"
#include "r3000a.h"
#include "psxmem.h"
#include "gte.h"

int mipsrec_rec_init(void); void mipsrec_rec_reset(void); void mipsrec_rec_execute(void);
void mipsrec_faultmem_set(int on); void mipsrec_rec_harness_faultmem(void);
extern R3000Acpu psxRec, psxInt;
extern char __bss_start[], _end[];
void psxRcntInit(void);
extern gte_handler *gte_h_NCLIP;   /* plugin.c: the handler recNCLIP JALs */
extern const u32 gte_code_NCLIP;

#define CODE  0x80010000u
#define SENT  0x80020000u
#define DONE  0x80030000u
#define CNT   0x80030800u
#define IN    0x80100000u
#define OUT   0x80180000u
#define DONE_MAGIC 0x600dc0deu
#define CHUNK 40000                /* 12 B in + 8 B out each: < 0x80000 */

static int end_reason;
extern void (*mipsrec_irq_test_hook)(psxCP0Regs *cp0);
static void test_irq(psxCP0Regs *cp0)
{
	(void)cp0;
	u32 done = *(u32 *)(psxRegs.ptrs.psxM + (DONE & 0x1fffff));
	if (done == DONE_MAGIC && !end_reason) { end_reason = 1; psxRegs.stop = 1; }
	psxRegs.next_interupt = psxRegs.cycle + 2000;
}

struct v { s16 sx[3], sy[3]; };
static struct v *vec;
static size_t nvec, cap;
static void add(int sx0, int sy0, int sx1, int sy1, int sx2, int sy2)
{
	if (nvec == cap) { cap = cap ? cap * 2 : 65536; vec = realloc(vec, cap * sizeof *vec); }
	struct v *p = &vec[nvec++];
	p->sx[0] = sx0; p->sy[0] = sy0; p->sx[1] = sx1; p->sy[1] = sy1; p->sx[2] = sx2; p->sy[2] = sy2;
}
static u32 xs = 2463534242u;
static u32 rnd(void) { xs ^= xs << 13; xs ^= xs >> 17; xs ^= xs << 5; return xs; }

static void gen(unsigned long nrand)
{
	static const int e[6] = { -32768, -32767, -1, 0, 1, 32767 };
	for (int i = 0; i < 6 * 6 * 6 * 6 * 6 * 6; i++) {
		int k = i;
		int a = e[k % 6]; k /= 6; int b = e[k % 6]; k /= 6; int c = e[k % 6]; k /= 6;
		int d = e[k % 6]; k /= 6; int f = e[k % 6]; k /= 6; int g = e[k % 6];
		add(a, b, c, d, f, g);
	}
	/* overflow boundaries: SY0,SY1,SY2 = (-32768, 32767, 0):
	 *   sum = SX0*(32767) + SX1*(32768) + SX2*(-65535)
	 * and mirrored SY = (32767, -32768, 0): sum = -32767*SX0 - 32768*SX1 + 65535*SX2 */
	static const long long B[4] = { 0x7fffffffLL, 0x80000000LL, -0x80000000LL, -0x80000001LL };
	for (int m = 0; m < 2; m++)
	for (int sx2i = 0; sx2i < 2; sx2i++) {
		const int sx2 = sx2i ? 32767 : -32768;
		const int sgn = m ? -1 : 1;
		const long long base = (long long)sgn * -65535 * sx2;
		for (int bi = 0; bi < 4; bi++)
		for (long long S = B[bi] - 1100; S <= B[bi] + 1100; S++) {
			const long long kk = sgn * (S - base);       /* 32767*a + 32768*b = kk */
			const long long a0 = ((-kk) % 32768 + 32768) % 32768;   /* a == -kk (mod 32768) */
			for (long long a = a0; a >= a0 - 32768; a -= 32768) {
				const long long b = (kk + a) / 32768 - a;
				if (b < -32768 || b > 32767) continue;
				if (m) add((int)a, 32767, (int)b, -32768, sx2, 0);
				else   add((int)a, -32768, (int)b, 32767, sx2, 0);
				break;
			}
		}
	}
	/* extreme sums */
	add(-32768, -32768, -32768, 32767, -32768, -32768);
	add(32767, 32767, 32767, -32768, -32768, 0);
	add(-32768, 32767, -32768, -32768, 32767, 0);
	add(-32768, -32768, 32767, 32767, -32768, 0);
	add(32767, -32768, -32768, 32767, 32767, 0);
	add(-32768, 0, -32768, 32767, -32768, -32768);
	for (unsigned long i = 0; i < nrand; i++) {
		int c[6];
		for (int j = 0; j < 6; j++)
			c[j] = (i & 1) && (rnd() & 1) ? e[rnd() % 6] : (s16)rnd();
		add(c[0], c[1], c[2], c[3], c[4], c[5]);
	}
}

static u32 sxy(s16 x, s16 y) { return (u16)x | ((u32)(u16)y << 16); }

int main(int argc, char **argv)
{
	if (argc < 2) { fprintf(stderr, "nclipj nclipj.bin\n"); return 2; }
	{ uintptr_t lo = (uintptr_t)__bss_start & ~4095u, hi = ((uintptr_t)_end + 4095) & ~4095u;
	  mprotect((void *)lo, hi - lo, PROT_READ | PROT_WRITE | PROT_EXEC); }   /* qemu enforces NX */
	static u8 code[0x1000];
	FILE *f = fopen(argv[1], "rb"); if (!f) return 2;
	size_t codelen = fread(code, 1, sizeof code, f); fclose(f);
	gen(getenv("HX_NRAND") ? strtoul(getenv("HX_NRAND"), 0, 0) : 400000);
	const int interp = getenv("HX_INT") != 0;

	if (psxMemInit()) return 1;
	mipsrec_irq_test_hook = test_irq;
	if (mipsrec_rec_init()) return 1;
	if (getenv("HX_FM")) mipsrec_rec_harness_faultmem();
	psxMemReset();
	psxRcntInit();
	Config.HLE = 0;
	u8 *ram = psxRegs.ptrs.psxM;
	memset(ram, 0, 0x200000);
	memcpy(ram + (CODE & 0x1fffff), code, codelen);
	*(u32 *)(ram + (SENT & 0x1fffff)) = 0x1000ffff;   /* b . */
	if (interp) { psxCpu = &psxInt; psxInt.Init(); psxInt.Reset(); }
	else psxCpu = &psxRec;

	unsigned long bad = 0, flagset = 0, runs_bad = 0;
	for (size_t c0 = 0; c0 < nvec; c0 += CHUNK) {
		const size_t n = nvec - c0 < CHUNK ? nvec - c0 : CHUNK;
		u32 *in = (u32 *)(ram + (IN & 0x1fffff)), *out = (u32 *)(ram + (OUT & 0x1fffff));
		for (size_t i = 0; i < n; i++) {
			const struct v *p = &vec[c0 + i];
			in[3 * i] = sxy(p->sx[0], p->sy[0]);
			in[3 * i + 1] = sxy(p->sx[1], p->sy[1]);
			in[3 * i + 2] = sxy(p->sx[2], p->sy[2]);
			out[2 * i] = out[2 * i + 1] = 0xdeadbeef;
		}
		*(u32 *)(ram + (CNT & 0x1fffff)) = n;
		*(u32 *)(ram + (DONE & 0x1fffff)) = 0;
		memset(&psxRegs.GPR, 0, sizeof psxRegs.GPR);
		psxRegs.GPR.r[31] = SENT;
		psxRegs.GPR.r[29] = 0x801ffff0;
		psxRegs.pc = CODE;
		psxRegs.cycle = 0;
		psxRegs.next_interupt = 2000;
		psxRegs.interrupt = 0;
		psxRegs.stop = 0;
		psxRegs.CP0.n.SR = 0x50000000;   /* CU2 | CU0 */
		psxRegs.CP2C.r[31] = 0x5a5a5a5a;  /* NCLIP must overwrite FLAG */
		end_reason = 0;
		if (interp)   /* the interpreter picks its COP2 handler from SR only on notify/config */
			psxInt.Notify(R3000ACPU_NOTIFY_AFTER_LOAD, NULL);
		if (interp) {
			for (unsigned long k = 0; k < 400000000ul && !end_reason; k++) {
				psxInt.ExecuteBlock(&psxRegs, 0);
				if (*(u32 *)(ram + (DONE & 0x1fffff)) == DONE_MAGIC) end_reason = 1;
			}
		} else
			mipsrec_rec_execute();
		if (end_reason != 1) { runs_bad++; fprintf(stderr, "chunk %zu did not finish\n", c0); continue; }
		for (size_t i = 0; i < n; i++) {
			const struct v *p = &vec[c0 + i];
			psxCP2Regs r;
			memset(&r, 0, sizeof r);
			r.CP2D.r[12] = in[3 * i]; r.CP2D.r[13] = in[3 * i + 1]; r.CP2D.r[14] = in[3 * i + 2];
			r.CP2C.r[31] = 0x5a5a5a5a;
			gte_h_NCLIP(&r, gte_code_NCLIP);
			const u32 mac0 = r.CP2D.r[24], flag = r.CP2C.r[31];
			flagset += flag != 0;
			if (out[2 * i] != mac0 || out[2 * i + 1] != flag) {
				if (bad++ < 8)
					printf("  MISMATCH sx=(%d,%d,%d) sy=(%d,%d,%d): got mac0=%08x flag=%08x, C mac0=%08x flag=%08x\n",
					       p->sx[0], p->sx[1], p->sx[2], p->sy[0], p->sy[1], p->sy[2],
					       out[2 * i], out[2 * i + 1], mac0, flag);
			}
		}
	}
	printf("nclipj: %s vectors=%zu mismatches=%lu flag_set=%lu unfinished_runs=%lu\n",
	       interp ? "interp" : "jit", nvec, bad, flagset, runs_bad);
	return bad || runs_bad;
}
