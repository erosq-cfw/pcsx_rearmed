/*
 * jitsched.cpp -- EROSQ_JITSCHED glue for the mipsrec block scheduler
 * (jitsched.h). Kept out of recompiler.cpp so that, with the scheduler off,
 * recompiler.o's code and data layout (dispatch loops, recMemBase) matches
 * the build without it.
 */
#include <stdio.h>
#include <stdlib.h>
#include "jitsched.h"

int jitsched_on;
static int jitsched_stat;
static unsigned long js_blocks, js_changed, js_regions;

void jitsched_init(void)
{
	const char *e = getenv("EROSQ_JITSCHED");
	jitsched_on = e ? atoi(e) : 0;
	jitsched_stat = getenv("EROSQ_JITSCHED_STAT") != NULL;
	if (jitsched_on > 0)
		printf("mipsrec: jitsched on\n");
}

void jitsched_run(uint32_t *start, uint32_t *end)
{
	const int c = jitsched_block(start, end);
	js_blocks++;
	js_changed += c != 0;
	js_regions += c;
	if (jitsched_stat && (js_blocks & 4095) == 0)
		fprintf(stderr, "jitsched: %lu blocks, %lu changed, %lu regions\n", js_blocks, js_changed, js_regions);
}

/* offline harness hooks (hexec HX_SCHED, plat_headless) */
extern "C" void mipsrec_jitsched_set(int on) { jitsched_on = on; }
extern "C" void mipsrec_jitsched_stats(unsigned long *blocks, unsigned long *changed, unsigned long *regions)
{ *blocks = js_blocks; *changed = js_changed; *regions = js_regions; }
