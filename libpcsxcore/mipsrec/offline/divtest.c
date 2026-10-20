/* divtest: MDU (DIV/DIVU/MULT/MULTU) differential harness, 2026-10-20.
 *
 * divtest prog.bin NRES int|rec
 * Runs one divtest.sh-generated guest program in the interpreter (psxInt, the
 * reference) or the REAL mipsrec (faultmem with HX_FM=1). The program stores
 * LO,HI of every case to RES + 8*i and then writes DONE_MAGIC to DONE.
 * gen_interupt is replaced via mipsrec_irq_test_hook (as in smcstress): re-arms a small horizon and stops
 * the run once DONE_MAGIC is seen. Prints one line per case:
 *   R <i> <lo> <hi>
 * followed by
 *   STATE gpr=<hash of r1..r25,r28..r31> hi=<HI> lo=<LO> end=<done|cap>
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
extern R3000Acpu psxRec, psxInt;
extern char __bss_start[], _end[];
void psxRcntInit(void);

#define CODE  0x80010000u
#define SENT  0x80008000u
#define RES   0x80100000u
#define DONE  0x80180000u
#define DONE_MAGIC 0x600dc0deu

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
	if (argc < 4) { fprintf(stderr, "divtest prog.bin NRES int|rec\n"); return 2; }
	const int rec = !strcmp(argv[3], "rec");
	const int nres = atoi(argv[2]);
	{ uintptr_t lo = (uintptr_t)__bss_start & ~4095u, hi = ((uintptr_t)_end + 4095) & ~4095u;
	  mprotect((void *)lo, hi - lo, PROT_READ | PROT_WRITE | PROT_EXEC); }   /* qemu enforces NX */
	static u8 code[RES - CODE];
	FILE *f = fopen(argv[1], "rb"); if (!f) return 2;
	const size_t codelen = fread(code, 1, sizeof code, f); fclose(f);
	if (codelen == sizeof code) { fprintf(stderr, "program too big\n"); return 2; }

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
	memset(ram + (RES & 0x1fffff), 0xa5, 8 * nres);   /* unwritten slots show up */
	memcpy(ram + (CODE & 0x1fffff), code, codelen);
	*(u32 *)(ram + (SENT & 0x1fffff)) = 0x1000ffff;   /* b . */

	memset(&psxRegs.GPR, 0, sizeof psxRegs.GPR);
	psxRegs.GPR.r[31] = SENT;
	psxRegs.GPR.r[29] = 0x801ffff0;
	psxRegs.pc = CODE;
	psxRegs.cycle = 0;
	psxRegs.interrupt = 0;
	psxRegs.CP0.n.SR = 0x10000000;
	run(rec);

	const u32 *res = (const u32 *)(ram + (RES & 0x1fffff));
	for (int i = 0; i < nres; i++) printf("R %d %08x %08x\n", i, res[2 * i], res[2 * i + 1]);
	u32 g = fnv(&psxRegs.GPR.r[1], 25 * 4, 2166136261u);   /* r1..r25 */
	g = fnv(&psxRegs.GPR.r[28], 4 * 4, g);
	printf("STATE gpr=%08x hi=%08x lo=%08x end=%s\n", g, psxRegs.GPR.n.hi, psxRegs.GPR.n.lo, done ? "done" : "cap");
	return 0;
}
