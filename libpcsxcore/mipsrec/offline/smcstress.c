/* smcstress: SMC stress harness (ideas/jitsize-pageprot, 2026-10-18).
 *
 * smcstress prog.bin plan.txt int|rec
 * Runs one gen_smc.py program either in the interpreter (psxInt: fetches every
 * instruction from RAM, so it is the absolute reference for well-defined SMC)
 * or in the REAL mipsrec (direct-return dispatcher, linking, faultmem with
 * HX_FM=1, page protection unless EROSQ_PAGEPROT=0). plan.txt (from smcrun.sh):
 *   TAB i ADDR   -> word i of the base-address table at TAB
 *   DMA ADDR W   -> phase 2: C-side write of W at ADDR + psxCpu->Clear (as DMA)
 *   P2 ADDR      -> phase 2 entry
 * gen_interupt is replaced via mipsrec_irq_test_hook (as in hlink): re-arms a small horizon and stops the
 * run once the guest wrote DONE_MAGIC. Prints
 *   STATE gpr=<hash> ram=<hash> v1=<v1> end=<done|cap> pp=<faults/bd/storms/unprot/verifyfail/protects/verifyblocks>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include "psxcommon.h"
#include "r3000a.h"
#include "psxmem.h"

int mipsrec_rec_init(void); void mipsrec_rec_execute(void); void mipsrec_rec_harness_faultmem(void);
void mipsrec_pageprot_stats(unsigned long v[8]) __attribute__((weak));
extern R3000Acpu psxRec, psxInt;
extern char __bss_start[], _end[];
void psxRcntInit(void);

#define CODE  0x80010000u
#define SENT  0x80020000u
#define DONE  0x80030000u
#define DONE_MAGIC 0x600dc0deu
#define TAB   0x80032000u

static unsigned long horizons, cap = 2000000;
static int done;
extern void (*mipsrec_irq_test_hook)(psxCP0Regs *cp0);
static void test_irq(psxCP0Regs *cp0)
{
	(void)cp0;
	horizons++;
	if (*(u32 *)(psxRegs.ptrs.psxM + (DONE & 0x1fffff)) == DONE_MAGIC) { done = 1; psxRegs.stop = 1; }
	else if (horizons >= cap) psxRegs.stop = 1;
	psxRegs.next_interupt = psxRegs.cycle + 48;
}

static u32 fnv(const void *p, size_t n, u32 h)
{
	const u8 *b = p;
	while (n--) h = (h ^ *b++) * 16777619u;
	return h;
}

static void run(int rec)
{
	psxRegs.stop = 0;
	psxRegs.next_interupt = psxRegs.cycle + 48;
	if (rec) { mipsrec_rec_execute(); return; }
	/* the interpreter never calls gen_interupt: step blocks, poll DONE */
	for (unsigned long n = 0; n < 50000000ul; n++) {
		psxInt.ExecuteBlock(&psxRegs, 0);
		if (*(u32 *)(psxRegs.ptrs.psxM + (DONE & 0x1fffff)) == DONE_MAGIC) { done = 1; return; }
	}
}

int main(int argc, char **argv)
{
	if (argc < 4) { fprintf(stderr, "smcstress prog.bin plan.txt int|rec\n"); return 2; }
	const int rec = !strcmp(argv[3], "rec");
	{ uintptr_t lo = (uintptr_t)__bss_start & ~4095u, hi = ((uintptr_t)_end + 4095) & ~4095u;
	  mprotect((void *)lo, hi - lo, PROT_READ | PROT_WRITE | PROT_EXEC); }   /* qemu enforces NX */
	static u8 code[0x10000];
	FILE *f = fopen(argv[1], "rb"); if (!f) return 2;
	const size_t codelen = fread(code, 1, sizeof code, f); fclose(f);

	if (psxMemInit()) return 1;
	if (rec) {
		mipsrec_irq_test_hook = test_irq;   /* replaces gen_interupt at every horizon */
	if (mipsrec_rec_init()) return 1;
		if (getenv("HX_FM")) mipsrec_rec_harness_faultmem();
	}
	psxMemReset();
	psxRcntInit();
	Config.HLE = 0;
	psxCpu = rec ? &psxRec : &psxInt;
	if (!rec) { psxInt.Init(); psxInt.Reset(); }

	u8 *ram = psxRegs.ptrs.psxM;
	memset(ram, 0, 0x200000);
	memcpy(ram + (CODE & 0x1fffff), code, codelen);
	*(u32 *)(ram + (SENT & 0x1fffff)) = 0x1000ffff;   /* b . */
	u32 dma_a[64], dma_w[64], p2 = 0; int ndma = 0;
	f = fopen(argv[2], "r"); if (!f) return 2;
	char ln[128];
	while (fgets(ln, sizeof ln, f)) {
		unsigned a, b;
		if (sscanf(ln, "TAB %u %x", &a, &b) == 2) *(u32 *)(ram + (TAB & 0x1fffff) + 4 * a) = b;
		else if (sscanf(ln, "DMA %x %x", &a, &b) == 2 && ndma < 64) { dma_a[ndma] = a; dma_w[ndma++] = b; }
		else if (sscanf(ln, "P2 %x", &a) == 1) p2 = a;
	}
	fclose(f);

	memset(&psxRegs.GPR, 0, sizeof psxRegs.GPR);
	psxRegs.GPR.r[31] = SENT;
	psxRegs.GPR.r[29] = 0x801ffff0;
	psxRegs.pc = CODE;
	psxRegs.cycle = 0;
	psxRegs.interrupt = 0;
	psxRegs.CP0.n.SR = 0x10000000;
	run(rec);
	int ok1 = done;

	if (ndma && p2) {   /* phase 2: C-side writes over code, as CD DMA + psxCpu->Clear */
		for (int i = 0; i < ndma; i++) {
			*(u32 *)(ram + (dma_a[i] & 0x1fffff)) = dma_w[i];
			psxCpu->Clear(dma_a[i], 1);
		}
		*(u32 *)(ram + (DONE & 0x1fffff)) = 0;
		done = 0; horizons = 0;
		psxRegs.GPR.r[31] = SENT;
		psxRegs.pc = p2;
		run(rec);
	}

	u32 g = fnv(&psxRegs.GPR.r[1], 25 * 4, 2166136261u);   /* r1..r25 */
	g = fnv(&psxRegs.GPR.r[28], 4 * 4, g);
	const u32 r = fnv(ram + (CODE & 0x1fffff), 0x24000, 2166136261u);
	unsigned long v[8] = { 0 };
	if (rec && mipsrec_pageprot_stats) mipsrec_pageprot_stats(v);
	printf("STATE gpr=%08x ram=%08x v1=%08x end=%s pp=%lu/%lu/%lu/%lu/%lu/%lu/%lu\n", g, r, psxRegs.GPR.n.v1,
	       ok1 && (!ndma || done) ? "done" : "cap", v[0], v[1], v[2], v[3], v[4], v[5], v[6]);
	return 0;
}
