/* differential execution: interpreter vs mipsrec on generated guest code */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "psxcommon.h"
#include "r3000a.h"
#include "psxmem.h"
#include <signal.h>
#include <ucontext.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/mman.h>
extern char __bss_start[], _end[];
void mipsrec_rec_execute(void); void mipsrec_rec_execute_block(unsigned pc);
#define SENT 0x80020000u
static void alrm2(int s, siginfo_t *si, void *ucv) { ucontext_t *uc = ucv; fprintf(stderr, "ALRM pc=%08x cycle=%u host_pc=%08x ra=%08x\n", psxRegs.pc, psxRegs.cycle, (unsigned)uc->uc_mcontext.pc, (unsigned)uc->uc_mcontext.gregs[31]); }
static void alrm(int s) { }
static volatile int direct_mode;
static void stopper(int s) { psxRegs.stop = 1; psxRegs.next_interupt = psxRegs.cycle; }
static void run_jit(void)
{
	if (!direct_mode) { mipsrec_rec_execute_block(SENT); return; }
	/* direct-return dispatcher (what the device runs after boot), incl. the
	 * self-loop fastpath; SENT holds 'b SENT', stop via a timer */
	*(u32 *)(psxRegs.ptrs.psxM + (SENT & 0x1fffff)) = 0x1000ffff;   /* b . */
	*(u32 *)(psxRegs.ptrs.psxM + (SENT & 0x1fffff) + 4) = 0;
	Config.HLE = 0;
	psxRegs.stop = 0;
	signal(SIGALRM, stopper);
	struct itimerval it = {{0,0},{0,700000}}; setitimer(ITIMER_REAL, &it, 0);
	mipsrec_rec_execute();
	struct itimerval z = {{0,0},{0,0}}; setitimer(ITIMER_REAL, &z, 0);
}
static void segv(int sig, siginfo_t *si, void *ucv) {
	ucontext_t *uc = ucv;
	fprintf(stderr, "SEGV addr=%p pc=%08x ra=%08x\n", si->si_addr, (unsigned)uc->uc_mcontext.pc, (unsigned)uc->uc_mcontext.gregs[31]);
	_exit(3);
}
int mipsrec_rec_init(void); void mipsrec_rec_reset(void); void mipsrec_rec_execute_block(unsigned pc);
void mipsrec_faultmem_clear_slow(void);
void mipsrec_faultmem_stats(unsigned long *f, unsigned long *s, int *t);
void mipsrec_emitstat_dump(void); void mipsrec_jitsched_set(int on); void mipsrec_jitsched_stats(unsigned long *, unsigned long *, unsigned long *); void mipsrec_faultmem_set(int on); void mipsrec_rec_execute(void); void mipsrec_rec_harness_faultmem(void);
extern R3000Acpu psxInt, psxRec;
static const u32 ptrs[7] = { 0x80100000, 0xa0100400, 0x00700800, 0x1f800000, 0x1f801000, 0xbf801000, 0x9f800100 };
#define SENT 0x80020000u
static u8 code[0x8000]; static size_t codelen;

static void setup(u32 seed)
{
	memset(&psxRegs.GPR, 0, sizeof psxRegs.GPR);
	srand(seed);
	u8 *ram = psxRegs.ptrs.psxM;
	memset(ram, 0, 0x200000);
	memcpy(ram + 0x10000, code, codelen);
	for (int i = 0x100000; i < 0x101000; i++) ram[i] = rand();
	for (int i = 0; i < 0x400; i++) psxRegs.ptrs.psxH[i] = rand();
	for (int i = 0x1000; i < 0x1080; i++) psxRegs.ptrs.psxH[i] = rand();
	for (int i = 8; i < 16; i++) psxRegs.GPR.r[i] = rand() * 2654435761u;
	psxRegs.GPR.r[31] = SENT;
	psxRegs.pc = 0x80010000;
	psxRegs.cycle = 0;
	psxRegs.next_interupt = 0x40000000;
	psxRegs.interrupt = 0;
	for (int i = 0; i < 20; i++) { psxRegs.event_cycles[i] = 0x40000000; psxRegs.intCycle[i].sCycle = 0; psxRegs.intCycle[i].cycle = 0x40000000; }
	psxRegs.CP0.n.SR = 0x10000000;
}

struct snap { u32 gpr[32]; u8 ram[0x1000]; u8 sp[0x400]; u8 hw[0x80]; };
static void take(struct snap *s)
{
	memcpy(s->gpr, psxRegs.GPR.r, sizeof s->gpr);
	memcpy(s->ram, psxRegs.ptrs.psxM + 0x100000, sizeof s->ram);
	memcpy(s->sp, psxRegs.ptrs.psxH, sizeof s->sp);
	memcpy(s->hw, psxRegs.ptrs.psxH + 0x1000, sizeof s->hw);
}

int main(int argc, char **argv)
{
	if (argc < 3) { fprintf(stderr, "hexec test.bin seed\n"); return 2; }
	FILE *f = fopen(argv[1], "rb"); codelen = fread(code, 1, sizeof code, f); fclose(f);
	u32 seed = strtoul(argv[2], 0, 0);
	direct_mode = getenv("HX_DIRECT") != 0;
	{ struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_sigaction = segv; sa.sa_flags = SA_SIGINFO; sigaction(SIGSEGV, &sa, 0); }
	/* qemu-user enforces PROT_EXEC; the device (no RI/XI) executes BSS */
	{ uintptr_t lo = (uintptr_t)__bss_start & ~4095u, hi = ((uintptr_t)_end + 4095) & ~4095u;
	  if (mprotect((void *)lo, hi - lo, PROT_READ | PROT_WRITE | PROT_EXEC)) perror("mprotect"); }
	if (psxMemInit()) return 1;
	if (mipsrec_rec_init()) return 1;
	psxMemReset();
	{ extern void psxRcntInit(void); psxRcntInit(); }
	psxInt.Init();
	struct snap a, b;
	/* reference = classic dual-path JIT (production), test = faultmem JIT.
	 * HX_SCHED=1: reference = unscheduled, test = EROSQ_JITSCHED-scheduled,
	 * both in the EROSQ_FAULTMEM mode. */
	const int fm_test = getenv("EROSQ_FAULTMEM") && getenv("EROSQ_FAULTMEM")[0] == '0' ? 0 : 1;
	const int sched = getenv("HX_SCHED") != 0;
	psxCpu = &psxRec;
	mipsrec_rec_harness_faultmem();
	mipsrec_faultmem_set(sched ? fm_test : 0);
	if (sched) mipsrec_jitsched_set(0);
	setup(seed);
	mipsrec_rec_reset(); mipsrec_faultmem_clear_slow();
	run_jit();
	take(&a);
	fprintf(stderr, "stage classic ok pc=%08x\n", psxRegs.pc);
	mipsrec_faultmem_set(fm_test);
	if (sched) mipsrec_jitsched_set(1);
	setup(seed);
	mipsrec_rec_reset(); mipsrec_faultmem_clear_slow();
	run_jit();
	take(&b);
	unsigned long fl, sl; int tn; mipsrec_faultmem_stats(&fl, &sl, &tn);
	int bad = 0;
	for (int i = 1; i < 32; i++) if (a.gpr[i] != b.gpr[i] && i != 26 && i != 27) { if (bad++ < 4) printf("  r%d classic %08x faultmem %08x\n", i, a.gpr[i], b.gpr[i]); }
	if (memcmp(a.ram, b.ram, sizeof a.ram)) { bad++; printf("  RAM differs\n"); }
	if (memcmp(a.sp, b.sp, sizeof a.sp)) { bad++; printf("  scratchpad differs\n"); }
	if (memcmp(a.hw, b.hw, sizeof a.hw)) { bad++; printf("  HW regs differ:");
		for (int i = 0; i < 0x80; i += 4) if (memcmp(a.hw + i, b.hw + i, 4)) printf(" [1f80%04x] %08x/%08x", 0x1000 + i, *(u32 *)(a.hw + i), *(u32 *)(b.hw + i));
		printf("\n"); }
	unsigned long jb, jc, jr; mipsrec_jitsched_stats(&jb, &jc, &jr);
	printf("%s seed=%u faults=%lu slow_blocks=%lu fm_tbl=%d sched_blocks=%lu changed=%lu\n", bad ? "FAIL" : "ok", seed, fl, sl, tn, jb, jc);
	return bad ? 1 : 0;
}
