/* EROSQ offline test: guest->host containment of every mipsrec load/store
 * fast path (2026-10-17). Runs the real recompiler under qemu-user.
 *
 * For every (base class x opcode x edge base x edge imm) case a tiny guest
 * program is hand-encoded: it makes the base reg hold an arbitrary value
 * while the JIT believes the base is RAM / scratchpad / stack / unknown /
 * const / static-array / GTE, performs ONE access, stores the loaded value
 * to a result slot and returns. Each case runs in a forked child.
 *
 * Verdicts:
 *   ok        access completed; for RAM-classified bases the loaded word
 *             must equal PSX RAM at ((base & 0x7fffff) + imm) mod 2MB
 *             (RAM mask + guard mirrors)
 *   contained SIGSEGV whose fault address lies inside the reserved guest
 *             window [0x0fe00000, 0x20010000) (master: process dies = DoS
 *             only; a faultmem build takes the slow path instead)
 *   VIOLATION fault address outside the window, a canary page just outside
 *             the window changed or was read, or a wrong RAM-class value
 *
 * Build (from the pcsx tree after a normal build, objects in place):
 *   see lsu_bounds.sh
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <ucontext.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include "psxcommon.h"
#include "r3000a.h"
#include "psxmem.h"

int mipsrec_rec_init(void); void mipsrec_rec_reset(void); void mipsrec_rec_execute_block(unsigned pc);
extern R3000Acpu psxRec;
extern char __bss_start[], _end[];
void psxRcntInit(void);

#define SENT   0x80020000u
#define CODE   0x80010000u
#define TAB    0x80018000u   /* table of base values (const address) */
#define RES    0x80018100u   /* result slot (const address) */
#define WLO    0x0fe00000u
#define WHI    0x20010000u
#define CANARY 0x5a5aa5a5u

/* MIPS-I encoders */
#define R_T9 25
#define R_T8 24
#define R_T0 8
#define R_T1 9
#define R_T2 10
#define R_SP 29
static u32 I(u32 op, u32 rs, u32 rt, u32 imm) { return (op << 26) | (rs << 21) | (rt << 16) | (imm & 0xffff); }
static u32 R(u32 rs, u32 rt, u32 rd, u32 fn) { return (rs << 21) | (rt << 16) | (rd << 11) | fn; }
#define LUI(rt, v)     I(0x0f, 0, rt, v)
#define ORI(rt, rs, v) I(0x0d, rs, rt, v)
#define LW(rt, o, b)   I(0x23, b, rt, o)
#define SW(rt, o, b)   I(0x2b, b, rt, o)
#define ADDU(d, s, t)  R(s, t, d, 0x21)
#define JR(rs)         R(rs, 0, 0, 0x08)
#define NOP 0u

enum { C_FUZZY_RAM, C_FUZZY_SCRATCH, C_SP, C_DUAL, C_CONST, C_ARRAY, C_GTE, C_N };
static const char *cname[C_N] = { "fuzzyRAM", "fuzzySP", "stackreg", "unknown", "const", "statarray", "gte" };
static const struct { const char *n; u32 op; int store; } ops[] = {
	{"lb",0x20,0},{"lbu",0x24,0},{"lh",0x21,0},{"lhu",0x25,0},{"lw",0x23,0},{"lwl",0x22,0},{"lwr",0x26,0},
	{"sb",0x28,1},{"sh",0x29,1},{"sw",0x2b,1},{"swl",0x2a,1},{"swr",0x2e,1},
};
static const struct { const char *n; u32 op; int store; } gops[] = { {"lwc2",0x32,0}, {"swc2",0x3a,1} };
static const u32 bases[] = {
	0x00000000, 0x1ffffffc, 0x80000000, 0xa0000000, 0xfffffffc,
	0x801ffffc, 0x80200000, 0x807ffffc, 0x80800000, 0x1f800000, 0x1f8003fc,
	0x1f801000, 0x0ffffffc, 0x10000000, 0x7ffffffc, 0x9ffffffc,
};
static const s32 imms[] = { -0x8000, -4, 0, 4, 0x7ffc };

static volatile u32 *canary_lo, *canary_hi;
static char casebuf[96];
static void segv(int sig, siginfo_t *si, void *ucv)
{
	ucontext_t *uc = ucv;
	u32 a = (u32)(uintptr_t)si->si_addr;
	u32 pc = (u32)uc->uc_mcontext.pc;
	printf("%s %s addr=%08x pc=%08x\n", casebuf, (a >= WLO && a < WHI) ? "contained" : "VIOLATION-fault", a, pc);
	fflush(stdout);
	_exit(0);
}

static int build(u32 *c, int cls, u32 op, int is_gte, u32 base, s32 imm, int store)
{
	int n = 0;
	const u32 rb = (cls == C_SP) ? R_SP : R_T0;
	c[n++] = LUI(R_T9, TAB >> 16); c[n++] = ORI(R_T9, R_T9, TAB & 0xffff);   /* const */
	c[n++] = LUI(R_T8, RES >> 16); c[n++] = ORI(R_T8, R_T8, RES & 0xffff);   /* const */
	c[n++] = LW(R_T2, 4, R_T9);    /* data to store */
	switch (cls) {
	case C_FUZZY_RAM:      /* lui 0x8010 + unknown -> tracker says RAM */
		c[n++] = LW(R_T1, 0, R_T9); c[n++] = NOP;
		c[n++] = LUI(rb, 0x8010); c[n++] = ADDU(rb, rb, R_T1);
		break;
	case C_FUZZY_SCRATCH:  /* lui 0x1f80 + unknown -> tracker says scratchpad */
		c[n++] = LW(R_T1, 0, R_T9); c[n++] = NOP;
		c[n++] = LUI(rb, 0x1f80); c[n++] = ADDU(rb, rb, R_T1);
		break;
	case C_SP: case C_DUAL: case C_GTE:
		c[n++] = LW(rb, 0, R_T9); c[n++] = NOP;
		break;
	case C_CONST:
		c[n++] = LUI(rb, base >> 16); c[n++] = ORI(rb, rb, base & 0xffff);
		break;
	case C_ARRAY:          /* LUI rt,hi ; ADDU rt,rt,off ; L* rt,lo(rt) (loads only) */
		c[n++] = LW(R_T1, 0, R_T9); c[n++] = NOP;
		break;
	}
	if (cls == C_ARRAY) {
		/* base value = 0x80100000 + t1 -> a "static array" at 0x80100000 */
		c[n++] = LUI(R_T0, 0x8010);
		c[n++] = ADDU(R_T0, R_T0, R_T1);
		c[n++] = I(op, R_T0, R_T0, imm);
		c[n++] = NOP;
	} else if (is_gte) {
		c[n++] = I(op, rb, 0, imm);          /* lwc2/swc2 GTE data reg 0 */
		c[n++] = NOP;
		if (!store) { c[n++] = (0x12u << 26) | (0u << 21) | (R_T0 << 16) | (0 << 11); c[n++] = NOP; } /* mfc2 t0, $0 */
	} else {
		c[n++] = I(op, rb, store ? R_T2 : R_T0, imm);
		c[n++] = NOP;
	}
	c[n++] = SW(R_T0, 0, R_T8);
	c[n++] = JR(31);
	c[n++] = NOP;
	return n;
}

int main(int argc, char **argv)
{
	int verbose = argc > 1;
	{ uintptr_t lo = (uintptr_t)__bss_start & ~4095u, hi = ((uintptr_t)_end + 4095) & ~4095u;
	  mprotect((void *)lo, hi - lo, PROT_READ | PROT_WRITE | PROT_EXEC); }
	if (psxMemInit()) { printf("psxMemInit failed\n"); return 1; }
	if (mipsrec_rec_init()) return 1;
	psxMemReset();
	psxRcntInit();
	psxCpu = &psxRec;
	/* canary pages immediately outside the reserved window */
	canary_lo = mmap((void *)(WLO - 0x1000), 0x1000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
	canary_hi = mmap((void *)WHI, 0x1000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
	if (canary_lo == MAP_FAILED || canary_hi == MAP_FAILED) printf("note: canary pages unavailable\n");

	int total = 0, nok = 0, ncont = 0, nviol = 0;
	for (int cls = 0; cls < C_N; cls++)
	for (unsigned oi = 0; oi < (cls == C_GTE ? 2u : sizeof ops / sizeof *ops); oi++)
	for (unsigned bi = 0; bi < sizeof bases / sizeof *bases; bi++)
	for (unsigned ii = 0; ii < sizeof imms / sizeof *imms; ii++) {
		const u32 op = cls == C_GTE ? gops[oi].op : ops[oi].op;
		const int store = cls == C_GTE ? gops[oi].store : ops[oi].store;
		const char *on = cls == C_GTE ? gops[oi].n : ops[oi].n;
		if (cls == C_ARRAY && store) continue;
		u32 base = bases[bi]; s32 imm = imms[ii];
		if (op == 0x21 || op == 0x25 || op == 0x29) imm &= ~1;   /* halfword alignment */
		if (op == 0x23 || op == 0x2b || op == 0x32 || op == 0x3a) imm &= ~3;
		fflush(stdout);
		pid_t p = fork();
		if (p == 0) {
			snprintf(casebuf, sizeof casebuf, "%-9s %-4s base=%08x imm=%6d:", cname[cls], on, base, imm);
			struct sigaction sa; memset(&sa, 0, sizeof sa);
			sa.sa_sigaction = segv; sa.sa_flags = SA_SIGINFO;
			sigaction(SIGSEGV, &sa, 0); sigaction(SIGBUS, &sa, 0);
			if (canary_lo != MAP_FAILED) for (int i = 0; i < 1024; i++) canary_lo[i] = CANARY, canary_hi[i] = CANARY;
			u8 *ram = psxRegs.ptrs.psxM;
			for (int i = 0; i < 0x200000; i += 4) *(u32 *)(ram + i) = 0xc0000000u | i;
			u32 code[64]; int n = build(code, cls, op, cls == C_GTE, base, imm, store);
			memcpy(ram + (CODE & 0x1fffff), code, n * 4);
			u32 tabv = base;
			if (cls == C_FUZZY_RAM || cls == C_ARRAY) tabv = base - 0x80100000u;
			if (cls == C_FUZZY_SCRATCH) tabv = base - 0x1f800000u;
			*(u32 *)(ram + (TAB & 0x1fffff)) = tabv;
			*(u32 *)(ram + (TAB & 0x1fffff) + 4) = 0x13572468;
			*(u32 *)(ram + (RES & 0x1fffff)) = 0;
			memset(&psxRegs.GPR, 0, sizeof psxRegs.GPR);
			psxRegs.GPR.r[31] = SENT;
			psxRegs.pc = CODE; psxRegs.cycle = 0; psxRegs.next_interupt = 0x40000000; psxRegs.interrupt = 0;
			psxRegs.CP0.n.SR = 0x50000000;   /* CU2 on for GTE */
			mipsrec_rec_reset();
			mipsrec_rec_execute_block(SENT);
			int bad = 0;
			if (canary_lo != MAP_FAILED)
				for (int i = 0; i < 1024; i++) if (canary_lo[i] != CANARY || canary_hi[i] != CANARY) bad = 1;
			u32 got = *(u32 *)(ram + (RES & 0x1fffff));
			if (!store && (got == CANARY || got == 0x5a5a || got == 0xa5a5 || got == 0x5a || got == 0xa5)) bad = 1;
			if ((cls == C_FUZZY_RAM || cls == C_ARRAY) && op == 0x23) {
				u32 idx = (((base & 0x7fffff) + (u32)imm) & 0x1fffff);
				if (got != (0xc0000000u | idx)) { bad = 2; }
			}
			printf("%s %s got=%08x\n", casebuf, bad == 1 ? "VIOLATION-canary" : bad == 2 ? "VIOLATION-ramvalue" : "ok", got);
			fflush(stdout);
			_exit(0);
		}
		int pipefd_dummy; (void)pipefd_dummy;
		int st; waitpid(p, &st, 0);
		total++;
		(void)st;
	}
	(void)verbose; (void)nok; (void)ncont; (void)nviol;
	printf("cases=%d\n", total);
	return 0;
}
