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

static void jitsched_init(void)
{
	jitsched_inited = 1;
	const char *e = getenv("EROSQ_JITSCHED");
	jitsched_on = e ? atoi(e) : 1;   /* default ON; EROSQ_JITSCHED=0 is the kill-switch */
	jitsched_stat = getenv("EROSQ_JITSCHED_STAT") != NULL;
	blkext_stat = getenv("EROSQ_BLKEXT_STAT") != NULL;
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
 * Accepted deviation (documented, not fixed here): the INLINE invalidation
 * emitted after guest stores (rec_lsu.cpp.h) still zeroes only the written
 * word's own slot. A guest store into a later word of a live block, with no
 * cache flush, keeps running the old code until the next flush (BIOS
 * FlushCache / cache isolation -> recClear of all RAM) or code-cache reset.
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
