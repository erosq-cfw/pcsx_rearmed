/*
 * jitsched.cpp -- mipsrec code that must not move existing code or data:
 * EROSQ_JITSCHED glue for the block scheduler (jitsched.h), the offline test
 * hook, and the block-extent table (below). Built without per-function
 * sections and with its .text and .rodata renamed to .jitsched_text /
 * .jitsched_rodata (Makefile), which jitsched.ld places after every existing
 * section; its .bss comes last: every pre-existing symbol keeps its address.
 */
#include <stdio.h>
#include <stdlib.h>
#include "jitsched.h"
#include "../r3000a.h"

/* all zero-initialised (.bss, appended last): a .data entry would move
 * every pre-existing .data/.bss symbol */
static int jitsched_inited, jitsched_on, jitsched_stat, blkext_stat;
static unsigned long js_blocks, js_changed, js_regions;

/* EROSQ_IDLESKIP (2026-10-22, offline prototype; default off). Guest spin loops
 * that only wait for the next event (IDLESIZE.md: Crash 44%, MGS 37%, Spyro 26%
 * of emulated cycles) are run in bulk instead of one iteration per dispatch:
 *   V  libetc VSync wait with timeout (Crash/Spyro/Ape): L = lw v0,16(sp);
 *      addiu -1; sw; lw; bne v0,v1,M -> M = lui/lw vcount; slt v0,v0,a0; bnez L
 *      (M's distance follows the bne: the timeout path differs per game)
 *   I  MGS idle thread: lui/lw flag; beqz; jal T (T = jr ra; li v0,-1);
 *      move s0,v0; bltz s0 -> head
 *   A  GT2 VSync wait: lw v0,4(a0); slt v0,v0,a1; bnez self
 * A block whose guest code at its start matches gets 5 entry words (filled by
 * jitsched_tail): lui a0,hi(pc); move a1,ra; jal mipsrec_idle_entry;
 * ori a0,lo(pc); move ra,v1. The helper returns pc in v0 (the block's v0 == pc
 * entry contract) and the caller's ra in v1; everything else it clobbers is
 * caller-saved scratch, dead at block entry (guest regs are in psxRegs until
 * rec_preload_regs, which comes after this room). It re-matches the guest
 * words (incl. T) every call, so SMC'd code simply stops skipping.
 * Exactness: no event can fire inside the loop (all sources schedule through
 * next_interupt; polled words are RAM/scratchpad, never HW). With C = the
 * loop's exact per-iteration charge (sum of its blocks' ADJUST_CLOCK, the
 * value the blocks' exits add) it skips m iterations with cycle + m*C <
 * next_interupt, i.e. none of their block exits would have reached the event
 * check, applies their net guest effect, and returns: the real block then
 * runs the iteration that reaches the event, unmodified. V also caps m at
 * its timeout counter, so the real iteration takes the timeout path exactly
 * when the plain run would. */
enum { IS_NONE, IS_V, IS_I, IS_A, IS_N };
static void jitsched_init(void);
static int idleskip_on, idleskip_stat;
static int cur_words, cur_idle;   /* entry room of the block being compiled */
static uint32_t cur_pc;
static unsigned long is_blocks[IS_N], is_calls[IS_N], is_skips[IS_N], is_nomatch;
static unsigned long long is_iters[IS_N];
extern u32 cycle_multiplier;              /* recompiler.cpp: 24.8 cycles per guest insn */
extern "C" void mipsrec_code_range(uintptr_t *lo, uintptr_t *hi);
extern "C" uint64_t mipsrec_idle_entry(uint32_t pc, uint32_t ra);

static inline uint32_t is_clk(uint32_t insns) { return (insns * cycle_multiplier) >> 8; }   /* = ADJUST_CLOCK */

/* word-aligned guest RAM (+mirrors, KUSEG/KSEG0/KSEG1) or scratchpad, else NULL */
static uint32_t *is_ptr(uint32_t a)
{
	const uint32_t p = a & 0x1fffffff;
	if (a & 3)
		return NULL;
	if (p < 0x800000)
		return (uint32_t *)(psxRegs.ptrs.psxM + (p & 0x1ffffc));
	if (p >= 0x1f800000 && p < 0x1f800400)
		return (uint32_t *)(psxRegs.ptrs.psxH + (p & 0x3fc));
	return NULL;
}

/* shape of the guest code at pc (RAM only); aux = polled address [, callee] */
static int idle_match(uint32_t pc, uint32_t *aux)
{
	static const uint32_t vl[8] = { 0x8fa20010, 0, 0x2442ffff, 0xafa20010, 0x8fa20010, 0, 0x14430000, 0 };
	static const uint32_t ga[5] = { 0x8c820004, 0, 0x0045102a, 0x1440fffc, 0 };
	if ((pc & 0x1fffffff) >= 0x800000 || (pc & 0x1ffffc) > 0x200000 - 0x80)
		return IS_NONE;
	const uint32_t *w = is_ptr(pc);
	if (!w)
		return IS_NONE;
	int i;
	for (i = 0; i < 8 && (i == 6 ? w[i] & 0xffff0000 : w[i]) == vl[i]; i++) ;
	if (i == 8) {
		/* M = bne target (Crash/Spyro pc+0x4c, Ape pc+0x48: the timeout
		 * path's length differs); M's bnez must branch back to pc */
		const int32_t off = (int16_t)w[6];
		if (off < 1 || off > 24)
			return IS_NONE;
		const uint32_t *m = w + 7 + off;
		if ((m[0] & 0xffff0000) == 0x3c020000 && (m[1] & 0xffff0000) == 0x8c420000 &&
		    m[2] == 0 && m[3] == 0x0044102a && m[4] == (0x14400000 | ((uint32_t)(-(7 + off + 5)) & 0xffff)) && m[5] == 0) {
			aux[0] = (m[0] << 16) + (uint32_t)(int32_t)(int16_t)m[1];
			return IS_V;
		}
		return IS_NONE;
	}
	for (i = 0; i < 5 && w[i] == ga[i]; i++) ;
	if (i == 5)
		return IS_A;
	if ((w[0] & 0xffff0000) == 0x3c020000 && (w[1] & 0xffff0000) == 0x8c420000 && w[2] == 0 &&
	    w[3] == 0x1040fffe && w[4] == 0 && (w[5] >> 26) == 0x03 && w[6] == 0 &&
	    w[7] == 0x00408021 && w[8] == 0x0600fff7 && w[9] == 0) {
		const uint32_t t = ((pc + 0x18) & 0xf0000000) | ((w[5] & 0x03ffffff) << 2);
		const uint32_t *tw = is_ptr(t);
		if (tw && (t & 0x1fffffff) < 0x800000 && (t & 0x1ffffc) <= 0x200000 - 8 &&
		    tw[0] == 0x03e00008 && tw[1] == 0x2402ffff) {
			aux[0] = (w[0] << 16) + (uint32_t)(int32_t)(int16_t)w[1];
			aux[1] = t;
			return IS_I;
		}
	}
	return IS_NONE;
}

static void idleskip_init(void)
{
	const char *e = getenv("EROSQ_IDLESKIP");
	idleskip_on = e ? atoi(e) : 0;   /* default OFF until it wins an A/B */
	idleskip_stat = getenv("EROSQ_IDLESKIP_STAT") != NULL;
	if (idleskip_on > 0) {
		uintptr_t lo, hi;
		mipsrec_code_range(&lo, &hi);
		const uintptr_t h = (uintptr_t)mipsrec_idle_entry;
		if (((lo ^ h) | ((hi - 1) ^ h)) & 0xf0000000) {   /* jal reaches one 256 MB segment */
			fprintf(stderr, "mipsrec: idleskip disabled (helper out of jal range)\n");
			idleskip_on = 0;
			return;
		}
		fprintf(stderr, "mipsrec: idleskip on\n");
	}
}

static void idleskip_print(void)
{
	static const char *const nm[IS_N] = { "-", "V", "I", "A" };
	for (int s = 1; s < IS_N; s++)
		fprintf(stderr, "idleskip %s: blocks %lu calls %lu skips %lu iters %llu\n",
			nm[s], is_blocks[s], is_calls[s], is_skips[s], is_iters[s]);
	fprintf(stderr, "idleskip nomatch-calls %lu\n", is_nomatch);
}

extern "C" void mipsrec_idleskip_report(void) { if (idleskip_on > 0) idleskip_print(); }

extern "C" uint64_t mipsrec_idle_entry(uint32_t pc, uint32_t ra)
{
	const uint64_t ret = ((uint64_t)ra << 32) | pc;
	uint32_t aux[2], *r = psxRegs.GPR.r, C, cap = 0xffffffffu, *ctr = NULL;
	const int s = idle_match(pc, aux);       /* re-checked every call: SMC */
	if (s == IS_NONE) {
		is_nomatch++;
		return ret;
	}
	is_calls[s]++;
	switch (s) {
	case IS_V: {
		const uint32_t *vc = is_ptr(aux[0]);
		ctr = is_ptr(r[29] + 16);
		if (!vc || !ctr || r[3] != 0xffffffffu || !((int32_t)*vc < (int32_t)r[4]))
			return ret;
		cap = *ctr;                          /* decrements before it reads -1 */
		C = is_clk(8) + is_clk(6);
		break; }
	case IS_I: {
		const uint32_t *f = is_ptr(aux[0]);
		if (!f || *f == 0)
			return ret;
		C = is_clk(7) + is_clk(2) + is_clk(3);
		break; }
	default: {   /* IS_A */
		const uint32_t *v = is_ptr(r[4] + 4);
		if (!v || !((int32_t)*v < (int32_t)r[5]))
			return ret;
		C = is_clk(5);
		break; }
	}
#ifdef IDLESKIP_MUTANT   /* offline mutant (idleskip.sh): per-iteration charge off by one */
	C += 1;
#endif
	const int32_t d = (int32_t)(psxRegs.next_interupt - psxRegs.cycle);
	if (!C || d <= (int32_t)C)
		return ret;
	uint32_t m = (uint32_t)(d - 1) / C;       /* cycle + m*C < next_interupt */
	if (m > cap)
		m = cap;
	if (!m)
		return ret;
	switch (s) {
	case IS_V:
		*ctr -= m;
		psxCpu->Clear((r[29] + 16) & ~3u, 1);   /* as the guest store would (SMC) */
		r[2] = 1;
		break;
	case IS_I:
		r[2] = r[16] = 0xffffffffu;
		r[31] = pc + 0x1c;
		break;
	default:
		r[2] = 1;
		break;
	}
	psxRegs.cycle += m * C;
	is_skips[s]++;
	is_iters[s] += m;
	if (idleskip_stat && (is_skips[s] & 4095) == 0)
		idleskip_print();
	return ret;
}

extern u32 *recMem;   /* recompiler.cpp: next emitted word */

/* recRecompile, at a block's first word: reserve the entry room (jitsched_tail fills it) */
extern "C" void mipsrec_entry_room(uint32_t pc)
{
	uint32_t aux[2];
	if (!jitsched_inited)
		jitsched_init();
	cur_pc = pc;
	cur_idle = idleskip_on > 0 ? idle_match(pc, aux) : IS_NONE;
	cur_words = cur_idle ? 5 : 0;
	recMem += cur_words;
}

static void jitsched_init(void)
{
	jitsched_inited = 1;
	const char *e = getenv("EROSQ_JITSCHED");
	jitsched_on = e ? atoi(e) : 1;   /* default ON; EROSQ_JITSCHED=0 is the kill-switch */
	jitsched_stat = getenv("EROSQ_JITSCHED_STAT") != NULL;
	blkext_stat = getenv("EROSQ_BLKEXT_STAT") != NULL;
	idleskip_init();
	if (jitsched_on > 0)
		fprintf(stderr, "mipsrec: jitsched on\n");   /* stderr: unbuffered, survives SIGTERM */
}

/* Block extents (2026-10-20). mipsrec's Clear() (DMA from CD/GPU/SPU, C-path
 * psxMemWrite stores, HLE memcpy/memset, EXE loads) only zeroes the lookup
 * slots of blocks STARTING inside the range, so a compiled block that starts
 * earlier and runs into the range kept executing the overwritten code. Each
 * compiled RAM block's guest [start, end) is recorded here per 4 KB page it
 * overlaps (compile time only), and mipsrec_blkext_clear() zeroes the slot of
 * every block that starts before a cleared range but overlaps it, then does
 * the normal clear. Cold paths only: emitted code is unchanged.
 *
 * Accepted deviation (documented, not fixed here): guest stores via the inline
 * path (rec_lsu.cpp.h, which zeroes only the written word's own slot) into
 * code of a currently-compiled block -- mid-block words, a block's own first
 * word on a fastpath self-loop, merged branch targets ('b' to the next insn is
 * dropped) -- take effect at the next flush / reset / recClear covering it
 * (BIOS FlushCache / cache isolation -> recClear of all RAM).
 * Real hardware is also stale until the 4 KB I-cache evicts the line. Fixing
 * it exactly costs a check on every guest store; measured offline (GT2 race +
 * boot, Crash boot) no game hit it. hlink.sh coh classifies those fixtures as
 * known-inline-diff. */
#define BX_PAGES (0x200000 >> 12)
struct bx { uint32_t s, e; };                  /* RAM byte offsets [s, e) */
static struct bx *bx_list[BX_PAGES];
static uint32_t bx_n[BX_PAGES], bx_cap[BX_PAGES];
static size_t bx_bytes, bx_peak;
static uintptr_t bx_last_host;
extern "C" void mipsrec_rec_clear(uint32_t addr, uint32_t size);

static void bx_reset_all(void)
{
	for (unsigned p = 0; p < BX_PAGES; p++) {
		free(bx_list[p]);
		bx_list[p] = NULL;
		bx_n[p] = bx_cap[p] = 0;
	}
	bx_bytes = 0;
}

static void bx_remove(unsigned p, uint32_t s)
{
	for (uint32_t i = 0; i < bx_n[p]; i++)
		if (bx_list[p][i].s == s) { bx_list[p][i] = bx_list[p][--bx_n[p]]; return; }
}

static void bx_register(uint32_t s, uint32_t e)
{
	for (unsigned p = s >> 12; p <= (e - 1) >> 12 && p < BX_PAGES; p++) {
		bx_remove(p, s);                             /* recompiled at the same start */
		if (bx_n[p] == bx_cap[p]) {
			uint32_t nc = bx_cap[p] ? bx_cap[p] * 2 : 8;
			struct bx *n = (struct bx *)realloc(bx_list[p], nc * sizeof *n);
			if (!n) return;                          /* out of memory: degrade to start-only clears */
			bx_bytes += (nc - bx_cap[p]) * sizeof *n;
			bx_list[p] = n; bx_cap[p] = nc;
		}
		bx_list[p][bx_n[p]++] = (struct bx){ s, e };
	}
	if (bx_bytes > bx_peak) {
		if (blkext_stat && (bx_bytes >> 12) != (bx_peak >> 12))   /* each new 4 KB step */
			fprintf(stderr, "blkext: peak %zu bytes\n", bx_bytes);
		bx_peak = bx_bytes;
	}
}

/* called by recRecompile (via jitsched_tail) for every block: host entry,
 * guest instruction count; the block's guest start is psxRegs.pc */
static void bx_block(uint32_t *host, uint32_t guest)
{
	/* the code cache restarts at its base on recReset (incl. the memfix cap
	 * flush): a block below the previous one means every old block is gone */
	if ((uintptr_t)host < bx_last_host)
		bx_reset_all();
	bx_last_host = (uintptr_t)host;
	const uint32_t pc = psxRegs.pc;
	if ((pc & 0x1fffffff) >= 0x800000 || !guest || guest > 0x10000)   /* RAM (+mirrors) only */
		return;
	const uint32_t s = pc & 0x1ffffc, e = s + guest * 4;
	if (e > 0x200000)
		return;
	bx_register(s, e);
}

/* psxCpu->Clear (plugin.c mipsrec_clear): also invalidate blocks that start
 * before [addr, addr + size words) but overlap it */
extern "C" void mipsrec_blkext_clear(uint32_t addr, uint32_t size)
{
	const uint32_t a = addr & 0x1ffffc;
	uint64_t len = (uint64_t)size * 4;
	if (len > 0x200000 - a)
		len = 0x200000 - a;
#ifndef BLKEXT_MUTANT   /* offline mutant: start-only clears (the old behaviour) */
	if (len) {
		const uint32_t e = a + (uint32_t)len;
		for (unsigned p = a >> 12; p <= (e - 1) >> 12; p++)
			for (uint32_t i = 0; i < bx_n[p];) {
				const struct bx x = bx_list[p][i];
				if (x.s < a && x.e > a) {            /* starts before, overlaps */
					mipsrec_rec_clear(x.s, 1);
					for (unsigned q = x.s >> 12; q <= (x.e - 1) >> 12 && q < BX_PAGES; q++)
						bx_remove(q, x.s);
					continue;                        /* slot i now holds another entry */
				}
				i++;
			}
	}
#endif
	mipsrec_rec_clear(addr, size);
	/* blocks starting inside the range are gone now: drop their extents */
	if (len) {
		const uint32_t e = a + (uint32_t)len;
		for (unsigned p = a >> 12; p <= (e - 1) >> 12; p++)
			for (uint32_t i = 0; i < bx_n[p];) {
				const struct bx x = bx_list[p][i];
				if (x.s >= a && x.s < e) {
					for (unsigned q = x.s >> 12; q <= (x.e - 1) >> 12 && q < BX_PAGES; q++)
						bx_remove(q, x.s);
					continue;
				}
				i++;
			}
	}
}

extern "C" void mipsrec_blkext_stats(size_t *bytes, size_t *peak)
{
	*bytes = bx_bytes; *peak = bx_peak;
}

/* called by recRecompile once per emitted block [start, end); also keeps the
 * emitstat block counters (es != 0). Returns 1 when emitstat should dump. */
int jitsched_tail(uint32_t *start, uint32_t *end, int es, unsigned long long *es_blocks,
		  unsigned long long *es_block_bytes, unsigned long long *es_block_guest, uint32_t bytes, uint32_t guest)
{
	if (!jitsched_inited)
		jitsched_init();
	bx_block(start, guest);
	uint32_t *room = start;
	if (cur_idle) {
		const uint32_t h = (uint32_t)(uintptr_t)mipsrec_idle_entry;
		room[0] = 0x3c040000 | (cur_pc >> 16);               /* lui  a0, hi(pc) */
		room[1] = 0x03e02821;                                /* move a1, ra */
		room[2] = 0x0c000000 | ((h >> 2) & 0x03ffffff);      /* jal  mipsrec_idle_entry */
		room[3] = 0x34840000 | (cur_pc & 0xffff);            /* ori  a0, a0, lo(pc) <BD> */
		room[4] = 0x0060f821;                                /* move ra, v1 */
		room += 5;
		is_blocks[cur_idle]++;
	}
	cur_idle = cur_words = 0;
	if (jitsched_on > 0) {
		const int c = jitsched_block(start, end);
		js_blocks++;
		js_changed += c != 0;
		js_regions += c;
		if (jitsched_stat && (js_blocks & 4095) == 0)
			fprintf(stderr, "jitsched: %lu blocks, %lu changed, %lu regions\n", js_blocks, js_changed, js_regions);
	}
	if (!es)
		return 0;
	++*es_blocks;
	*es_block_bytes += bytes;
	*es_block_guest += guest;
	return (*es_blocks & 4095) == 0;
}

/* Offline test hook (hlink / smcstress / divtest): when set, called INSTEAD of
 * gen_interupt() at every horizon (mipsrec_branch_test, plugin.c). NULL in the
 * emulator; only offline harnesses assign it. A pointer rather than
 * -Wl,--wrap=gen_interupt, which cannot redirect a call that -flto resolves
 * inside the merged unit. Lives here (appended .jitsched_text, .bss last) so
 * arming support moves no existing code or data. */
union psxCP0Regs_;
extern "C" void gen_interupt(union psxCP0Regs_ *cp0);
extern "C" void (*mipsrec_irq_test_hook)(union psxCP0Regs_ *cp0);
void (*mipsrec_irq_test_hook)(union psxCP0Regs_ *cp0);
extern "C" void mipsrec_branch_test_hooked(union psxCP0Regs_ *cp0)
{
	if (__builtin_expect(mipsrec_irq_test_hook != NULL, 0))
		mipsrec_irq_test_hook(cp0);
	else
		gen_interupt(cp0);
}

/* offline harness hooks (hexec HX_SCHED, plat_headless) */
extern "C" void mipsrec_jitsched_set(int on) { jitsched_inited = 1; jitsched_on = on; }
extern "C" void mipsrec_jitsched_stats(unsigned long *blocks, unsigned long *changed, unsigned long *regions)
{ *blocks = js_blocks; *changed = js_changed; *regions = js_regions; }
