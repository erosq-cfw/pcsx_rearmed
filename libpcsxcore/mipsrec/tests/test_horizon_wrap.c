/* Host self-check for the mipsrec dispatch horizon comparison.
 *
 * The PS1 CPU cycle counter (psxRegs.cycle) is a u32 that wraps every ~127s of
 * emulated time (33.8MHz). The interrupt horizon is next_interupt = cycle + min,
 * computed in u32 and therefore able to wrap below `cycle`. The dispatch loop
 * must decide "has cycle reached the horizon?" in a way that survives that wrap.
 *
 * BUG (pre-fix): used unsigned  cycle >= next  (asm: sltu/beqz). When cycle is
 * within `min` of 2^32, next wraps to a small value, unsigned compare reports
 * "reached" forever, the block never runs, cycle never advances -> livelock.
 * PaRappa hit this ~2m19s in (first cycle wrap); MGS attract "self-hang ~150s"
 * was the same bug.
 *
 * FIX: wrap-safe signed difference  (s32)(cycle - next) >= 0  (asm: subu/bgez).
 *
 * Build+run on host:  cc -o t test_horizon_wrap.c && ./t
 */
#include <stdint.h>
#include <stdio.h>
#include <assert.h>

typedef uint32_t u32;
typedef int32_t  s32;

/* The two candidate "horizon reached?" predicates. */
static int reached_buggy(u32 cycle, u32 next) { return cycle >= next; }          /* sltu-based */
static int reached_fixed(u32 cycle, u32 next) { return (s32)(cycle - next) >= 0; }/* subu/bgez  */

/* Model one dispatch step: if not reached, a block runs and advances cycle by
 * `step`; if reached, gen_interupt reschedules next = cycle + min (u32 wrap).
 * Returns 1 if forward progress was made (cycle advanced) this step. */
static int step_advances(int (*reached)(u32,u32), u32 *cycle, u32 *next, u32 min, u32 blk)
{
	if (reached(*cycle, *next)) {
		*next = *cycle + min;   /* schedule_timeslice: u32, may wrap below cycle */
		return 0;               /* branch-test path: cycle NOT advanced */
	}
	*cycle += blk;              /* execute block: cycle advances */
	return 1;
}

int main(void)
{
	/* 1) Away from the wrap boundary both predicates agree. */
	assert(reached_buggy(5001, 5000) && reached_fixed(5001, 5000));   /* just passed */
	assert(!reached_buggy(100, 5000) && !reached_fixed(100, 5000));   /* not yet   */

	/* 2) The exact hang state observed on hardware:
	 *    cycle=0xFFFFE365, an event 'min' ahead so next wraps to 0xE199. */
	u32 cyc = 0xFFFFE365u, min = 0xE199u - 0xFFFFE365u; /* == cyc+min == 0xE199 */
	u32 next = cyc + min;
	assert(next == 0x0000E199u);           /* wrapped below cycle */
	assert(reached_buggy(cyc, next) == 1); /* BUG: says reached -> livelock  */
	assert(reached_fixed(cyc, next) == 0); /* FIX: correctly "not reached"    */

	/* 3) End to end: drive dispatch across the wrap. The buggy predicate must
	 *    livelock (stops advancing); the fixed one must carry cycle across. */
	{
		u32 c = 0xFFFFE000u, n = c + 0x9000u; /* n = 0x00007000, already wrapped */
		int i, advanced = 0;
		for (i = 0; i < 100000; i++)
			advanced += step_advances(reached_buggy, &c, &n, 0x9000u, 40);
		/* buggy: cycle pinned near boundary, never crosses -> ~no progress */
		assert(c >= 0xFFFFE000u);   /* still stuck at/above start, never wrapped low */
	}
	{
		u32 c = 0xFFFFE000u, n = c + 0x9000u;
		int i;
		for (i = 0; i < 100000; i++)
			step_advances(reached_fixed, &c, &n, 0x9000u, 40);
		/* fixed: crossed the wrap into the low range and kept running */
		assert(c < 0x7FFFFFFFu);
	}

	/* 4) recRFE sentinel: forcing an immediate branch test.
	 *    Old sentinel next=0 fails signed-diff in the upper half; next=cycle works. */
	{
		u32 c = 0x90000000u;                       /* upper half */
		assert(reached_fixed(c, 0) == 0);          /* next=0  -> NOT reached (stall!) */
		assert(reached_fixed(c, c) == 1);          /* next=cycle -> reached (correct) */
	}

	printf("test_horizon_wrap: all assertions passed\n");
	return 0;
}
