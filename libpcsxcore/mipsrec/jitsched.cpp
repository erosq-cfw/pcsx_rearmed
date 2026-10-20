/*
 * jitsched.cpp -- EROSQ_JITSCHED glue for the mipsrec block scheduler
 * (jitsched.h). Built without per-function sections and with its .text and
 * .rodata renamed to .jitsched_text / .jitsched_rodata (Makefile), which
 * jitsched.ld places after every existing section: with the scheduler off,
 * every pre-existing symbol keeps its address.
 */
#include <stdio.h>
#include <stdlib.h>
#include "jitsched.h"

/* all zero-initialised (.bss, appended last): a .data entry would move
 * every pre-existing .data/.bss symbol */
static int jitsched_inited, jitsched_on, jitsched_stat;
static unsigned long js_blocks, js_changed, js_regions;

static void jitsched_init(void)
{
	jitsched_inited = 1;
	const char *e = getenv("EROSQ_JITSCHED");
	jitsched_on = e ? atoi(e) : 1;   /* default ON; EROSQ_JITSCHED=0 is the kill-switch */
	jitsched_stat = getenv("EROSQ_JITSCHED_STAT") != NULL;
	if (jitsched_on > 0)
		fprintf(stderr, "mipsrec: jitsched on\n");   /* stderr: unbuffered, survives SIGTERM */
}

/* called by recRecompile once per emitted block [start, end); also keeps the
 * emitstat block counters (es != 0). Returns 1 when emitstat should dump. */
int jitsched_tail(uint32_t *start, uint32_t *end, int es, unsigned long long *es_blocks,
		  unsigned long long *es_block_bytes, unsigned long long *es_block_guest, uint32_t bytes, uint32_t guest)
{
	if (!jitsched_inited)
		jitsched_init();
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
