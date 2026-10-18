/* hlink: block-linking differential harness (2026-10-17).
 *
 * Runs one generated guest program (gen_link.py) through the REAL mipsrec
 * direct-return dispatcher (recExecute, what the device runs after boot)
 * and prints its final state. hlink.sh runs it twice in the same binary,
 * EROSQ_BLOCKLINK=0 and =2, and compares.
 *
 * gen_interupt() is replaced (-Wl,--wrap=gen_interupt): every horizon hit
 * re-arms next_interupt = cycle + BUDGET (a small budget, so the inline
 * horizon checks of linked exits fire mid-chain), and stops the run
 * (psxRegs.stop, as the menu does) when the guest has written DONE_MAGIC,
 * or after HX_STOP_AFTER horizons (spin programs: stop inside a chain).
 * The start cycle can sit just below the u32 wrap (HX_CYCLE0).
 *
 * Output: "STATE gpr=<hash> ram=<hash> cycle=<u32> pc=<u32> horizons=<n>
 *          link_taken=<n|na> end=<done|stopped|cap>"
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include "psxcommon.h"
#include "r3000a.h"
#include "psxmem.h"

int mipsrec_rec_init(void); void mipsrec_rec_reset(void); void mipsrec_rec_execute(void);
extern R3000Acpu psxRec;
extern char __bss_start[], _end[];
void psxRcntInit(void);
/* exported by the blocklink build; weak until then */
extern u32 mipsrec_link_taken __attribute__((weak));
extern u32 cycle_multiplier;

#define CODE  0x80010000u
#define SENT  0x80020000u
#define DONE  0x80030000u
#define DONE_MAGIC 0x600dc0deu
#define DATA  0x80031000u

static u32 budget = 64;
static unsigned long horizons, stop_after, cap = 4000000;
static int end_reason;   /* 0 running, 1 done, 2 stopped(spin), 3 cap */

void __wrap_gen_interupt(psxCP0Regs *cp0)
{
	(void)cp0;
	horizons++;
	u32 done = *(u32 *)(psxRegs.ptrs.psxM + (DONE & 0x1fffff));
	if (done == DONE_MAGIC && !end_reason) { end_reason = 1; psxRegs.stop = 1; }
	else if (stop_after && horizons >= stop_after && !end_reason) { end_reason = 2; psxRegs.stop = 1; }
	else if (horizons >= cap && !end_reason) { end_reason = 3; psxRegs.stop = 1; }
	psxRegs.next_interupt = psxRegs.cycle + budget;
}

static u32 fnv(const void *p, size_t n, u32 h)
{
	const u8 *b = p;
	while (n--) h = (h ^ *b++) * 16777619u;
	return h;
}

int main(int argc, char **argv)
{
	if (argc < 2) { fprintf(stderr, "hlink prog.bin\n"); return 2; }
	{ uintptr_t lo = (uintptr_t)__bss_start & ~4095u, hi = ((uintptr_t)_end + 4095) & ~4095u;
	  mprotect((void *)lo, hi - lo, PROT_READ | PROT_WRITE | PROT_EXEC); }   /* qemu enforces NX */
	static u8 code[0x10000];
	FILE *f = fopen(argv[1], "rb"); if (!f) return 2;
	size_t codelen = fread(code, 1, sizeof code, f); fclose(f);
	if (getenv("HX_BUDGET")) budget = strtoul(getenv("HX_BUDGET"), 0, 0);
	if (getenv("HX_STOP_AFTER")) stop_after = strtoul(getenv("HX_STOP_AFTER"), 0, 0);
	u32 cycle0 = getenv("HX_CYCLE0") ? strtoul(getenv("HX_CYCLE0"), 0, 0) : 0;

	if (psxMemInit()) return 1;
	if (mipsrec_rec_init()) return 1;
	if (getenv("HX_MULT")) cycle_multiplier = strtoul(getenv("HX_MULT"), 0, 0);  /* ctl:long */
	psxMemReset();
	psxRcntInit();
	psxCpu = &psxRec;
	Config.HLE = 0;                       /* -> direct-return dispatcher */

	u8 *ram = psxRegs.ptrs.psxM;
	memset(ram, 0, 0x200000);
	memcpy(ram + (CODE & 0x1fffff), code, codelen);
	*(u32 *)(ram + (SENT & 0x1fffff)) = 0x1000ffff;   /* b . */
	*(u32 *)(ram + (SENT & 0x1fffff) + 4) = 0;
	for (int i = 0; i < 0x400; i += 4) *(u32 *)(ram + (DATA & 0x1fffff) + i) = 0x9e3779b9u * (i + 1);

	if (getenv("HX_ROMLOOP")) {   /* gen_link ctl:rom: 2-block linked loop in ROM */
		u32 *r = (u32 *)(psxRegs.ptrs.psxR + 0x1000), b = 0xbfc01000u;
		r[0] = 0x26f7ffff;                                   /* R0: addiu s7,s7,-1 */
		r[1] = 0x08000000 | (((b + 0x10) >> 2) & 0x3ffffff); /*     j R1 */
		r[2] = 0;
		r[3] = 0;
		r[4] = 0x24420003;                                   /* R1: addiu v0,v0,3 */
		r[5] = 0x1ee00000 | ((((b) - (b + 0x14 + 4)) >> 2) & 0xffff);   /* bgtz s7,R0 */
		r[6] = 0;
		r[7] = 0x8f2d0004;                                   /*     lw t5,4(t9)  (EXIT) */
		r[8] = 0;
		r[9] = 0x01a00008;                                   /*     jr t5 */
		r[10] = 0;
	}
	memset(&psxRegs.GPR, 0, sizeof psxRegs.GPR);
	psxRegs.GPR.r[31] = SENT;
	psxRegs.GPR.r[29] = 0x801ffff0;
	psxRegs.pc = getenv("HX_PC") ? strtoul(getenv("HX_PC"), 0, 0) : CODE;   /* e.g. 0x00010000 (kuseg) */
	psxRegs.cycle = cycle0;
	psxRegs.next_interupt = cycle0 + budget;
	psxRegs.interrupt = 0;
	psxRegs.stop = 0;
	psxRegs.CP0.n.SR = 0x10000000;

	mipsrec_rec_execute();

	u32 g = fnv(psxRegs.GPR.r, 26 * 4, 2166136261u);            /* r0..r25 */
	g = fnv(&psxRegs.GPR.r[28], 4 * 4, g);                       /* gp sp fp ra */
	g = fnv(&psxRegs.GPR.r[32], 2 * 4, g);                       /* lo hi */
	u32 r = fnv(ram + (CODE & 0x1fffff), 0x22000, 2166136261u);  /* code, SENT, DONE, DATA */
	char lt[32];
	if (&mipsrec_link_taken) snprintf(lt, sizeof lt, "%u", mipsrec_link_taken);
	else snprintf(lt, sizeof lt, "na");
	static const char *er[] = { "running", "done", "stopped", "cap" };
	printf("STATE gpr=%08x ram=%08x cycle=%08x pc=%08x horizons=%lu link_taken=%s end=%s\n",
	       g, r, psxRegs.cycle, psxRegs.pc, horizons, lt, er[end_reason]);
	if (getenv("HX_REGS"))
		for (int i = 0; i < 34; i++) printf("  r%d=%08x\n", i, psxRegs.GPR.r[i]);
	return 0;
}
