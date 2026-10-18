/*
 * jitsched.h -- post-emission list scheduler for one mipsrec block (EROSQ).
 *
 * Reorders already-emitted host words inside straight-line REGIONS to hide
 * X1000 load-use / mul-use interlocks (HW-calibrated: load d1/d2/d3 +2.4/
 * +1.3/+0.5 cyc, mul-use ~+3). Pure function of the words: no recompiler
 * state, so tests/test_jitsched.c runs it on the host.
 *
 * Safety model (see TASKS/BLOCKLINK notes in the jit-sched branch):
 * - Only whitelisted opcodes move. Anything else -- every branch/jump, JAL,
 *   COP0/COP2/MXU, trapping ADD/ADDI/SUB, syscall/break/sync, any write to
 *   $sp/$fp -- is a barrier pinned at its address, and so is the word after
 *   it (delay slot, or conservatively the next word for unknown opcodes).
 * - Every in-block branch target starts a region; region starts stay at
 *   their addresses, so already-resolved branch offsets stay valid.
 * - Register RAW/WAR/WAW on GPRs + HI + LO are kept.
 * - Memory: an access whose base is not $fp/$sp (guest RAM/HW, the SMC
 *   recRAM slot, anything that can fault into faultmem) keeps its order
 *   relative to EVERY other access. Two $fp/$sp-relative accesses (psxRegs /
 *   the dispatcher frame: never fault) may swap iff their byte ranges are
 *   disjoint or their bases differ.
 * - A region's new order is kept only if its modelled cost is lower.
 */
#ifndef JITSCHED_H
#define JITSCHED_H

#ifndef JITSCHED_MUTANT
#define JITSCHED_MUTANT 0
#endif

#include <stdint.h>
#include <string.h>

#define JS_MAXR 32          /* longer runs are split: the split is a barrier */
#define JS_LOOK 16          /* list-scheduling lookahead (compile-time bound) */
#define JS_MINWIN 10        /* skip regions modelling < 1 cycle of stall */
/* all masks are 32-bit (64-bit ops/ctz are libgcc calls on MIPS32): bit r =
 * GPR r, and bit 0 ($zero is never a dependency) = HI and LO together */
#define JS_HILO 1u
#define js_ctz(m) __builtin_ctz(m)
typedef uint32_t js_m;

/* producer "ready" delay in tenths of a cycle (issue + R) */
#define JS_R_ALU 10
#define JS_R_LD  34          /* d1 +2.4, d2 +1.4, d3 +0.4 (HW: 2.4/1.3/0.5) */
#define JS_R_MUL 40
#define JS_R_DIV 360

enum { JS_BAR = 0, JS_ALU, JS_LD, JS_ST };

struct js_ins {
	js_m     src, dst;      /* bit r = GPR r, bit 0 = HI/LO */
	uint8_t  kind;
	uint16_t rdy;           /* JS_R_* */
	int8_t   base;          /* memory base reg, -1 if none */
	int16_t  lo, hi;        /* byte range [lo, hi) relative to base */
};

#define JS_B(r) ((r) ? (1u << (r)) : 0u)

/* returns kind; fills deps. Unknown words are barriers. */
static int js_decode(uint32_t w, struct js_ins *d)
{
	const int op = w >> 26, rs = (w >> 21) & 31, rt = (w >> 16) & 31,
		  rd = (w >> 11) & 31, fn = w & 63;
	const js_m HL = JS_HILO;
	d->src = d->dst = 0;
	d->lo = d->hi = 0;
	d->base = -1;
	d->rdy = JS_R_ALU;
	d->kind = JS_ALU;
	switch (op) {
	case 0x00:
		switch (fn) {
		case 0x00: case 0x02: case 0x03:
			if (w == 0) { d->kind = JS_ALU; return JS_ALU; }   /* nop */
			d->src = JS_B(rt); d->dst = JS_B(rd); break;
		case 0x04: case 0x06: case 0x07:
		case 0x21: case 0x23: case 0x24: case 0x25: case 0x26: case 0x27:
		case 0x2a: case 0x2b:
			d->src = JS_B(rs) | JS_B(rt); d->dst = JS_B(rd); break;
		case 0x0a: case 0x0b:                         /* movz movn */
			d->src = JS_B(rs) | JS_B(rt) | JS_B(rd); d->dst = JS_B(rd); break;
		case 0x10: d->src = JS_HILO; d->dst = JS_B(rd); break;
		case 0x12: d->src = JS_HILO; d->dst = JS_B(rd); break;
		case 0x11: d->src = JS_B(rs); d->dst = JS_HILO; break;
		case 0x13: d->src = JS_B(rs); d->dst = JS_HILO; break;
		case 0x18: case 0x19:
			d->src = JS_B(rs) | JS_B(rt); d->dst = HL; d->rdy = JS_R_MUL; break;
		case 0x1a: case 0x1b:
			d->src = JS_B(rs) | JS_B(rt); d->dst = HL; d->rdy = JS_R_DIV; break;
		default: d->kind = JS_BAR; return JS_BAR;   /* jr jalr syscall add sub ... */
		}
		break;
	case 0x09: case 0x0a: case 0x0b: case 0x0c: case 0x0d: case 0x0e:
		d->src = JS_B(rs); d->dst = JS_B(rt); break;
	case 0x0f: d->dst = JS_B(rt); break;
	case 0x1c:                                            /* SPECIAL2 */
		if (fn == 0x02) {                                 /* mul: HI/LO unpredictable */
			d->src = JS_B(rs) | JS_B(rt); d->dst = JS_B(rd) | HL; d->rdy = JS_R_MUL;
		} else if (fn == 0x00 || fn == 0x01 || fn == 0x04 || fn == 0x05) {
			d->src = JS_B(rs) | JS_B(rt) | HL; d->dst = HL; d->rdy = JS_R_MUL;
		} else if (fn == 0x20 || fn == 0x21) {
			d->src = JS_B(rs); d->dst = JS_B(rd);
		} else { d->kind = JS_BAR; return JS_BAR; }   /* MXU etc */
		break;
	case 0x1f:                                            /* SPECIAL3 */
		if (fn == 0x00) { d->src = JS_B(rs); d->dst = JS_B(rt); }
		else if (fn == 0x04) { d->src = JS_B(rs) | JS_B(rt); d->dst = JS_B(rt); }
		else if (fn == 0x20 && (((w >> 6) & 31) == 0x10 || ((w >> 6) & 31) == 0x18 ||
					((w >> 6) & 31) == 0x02)) { d->src = JS_B(rt); d->dst = JS_B(rd); }
		else { d->kind = JS_BAR; return JS_BAR; }
		break;
	case 0x20: case 0x21: case 0x23: case 0x24: case 0x25:
	case 0x22: case 0x26:
	case 0x28: case 0x29: case 0x2a: case 0x2b: case 0x2e: {
		const int16_t imm = (int16_t)w;
		const int sz = (op & 3) == 0 ? 1 : (op & 3) == 1 ? 2 : 4;
		const int unal = op == 0x22 || op == 0x26 || op == 0x2a || op == 0x2e;
		d->base = rs;
		d->lo = unal ? (int16_t)(imm & ~3) : imm;
		d->hi = (int16_t)(d->lo + (unal ? 4 : sz));
		if (op >= 0x28) {
			d->kind = JS_ST; d->src = JS_B(rs) | JS_B(rt);
		} else {
			d->kind = JS_LD; d->rdy = JS_R_LD;
			d->src = JS_B(rs) | (unal ? JS_B(rt) : 0); d->dst = JS_B(rt);
		}
		break; }
	default:
		d->kind = JS_BAR; return JS_BAR;
	}
	if (d->dst & ((1u << 29) | (1u << 30))) { d->kind = JS_BAR; return JS_BAR; }
	return d->kind;
}

static int js_is_branch(uint32_t w)
{
	const int op = w >> 26;
	return op == 0x01 || (op >= 0x04 && op <= 0x07) || (op >= 0x14 && op <= 0x17);
}

/* registers a barrier reads (sink of the region before it); ~0 = assume all */
static js_m js_bar_src(uint32_t w)
{
	const int op = w >> 26, rs = (w >> 21) & 31, rt = (w >> 16) & 31, fn = w & 63;
	if (op == 0x04 || op == 0x05) return JS_B(rs) | JS_B(rt);
	if (op == 0x01 || op == 0x06 || op == 0x07) return JS_B(rs);
	if (op == 0x02 || op == 0x03) return 0;
	if (op == 0x00 && (fn == 0x08 || fn == 0x09)) return JS_B(rs);
	return ~0u;
}

static int js_private(const struct js_ins *a) { return a->base == 29 || a->base == 30; }

/* memory part of the ordering rule: must access b (later) stay after a? */
static int js_mem_dep(const struct js_ins *a, const struct js_ins *b)
{
	/* guest/fault-capable accesses keep order with every access */
	if (!js_private(a) || !js_private(b)) {
#if JITSCHED_MUTANT == 2   /* offline mutant 2: a guest load may pass a store */
		if (b->kind == JS_LD && a->kind == JS_ST && a->base != b->base) return 0;
#endif
		return 1;
	}
	return (a->kind == JS_ST || b->kind == JS_ST) &&
	       a->base == b->base && a->lo < b->hi && b->lo < a->hi;
}

/* the full pairwise rule (reference for tests/test_jitsched.c) */
static __attribute__((unused)) int js_dep(const struct js_ins *a, const struct js_ins *b)
{
	if ((a->dst & b->src) || (a->dst & b->dst)) return 1;
#if JITSCHED_MUTANT != 1   /* offline mutant 1: drop WAR edges */
	if (a->src & b->dst) return 1;
#endif
	return a->kind >= JS_LD && b->kind >= JS_LD && js_mem_dep(a, b);
}

/* direct predecessors: same transitive closure as js_dep over all pairs, but
 * registers via last-writer / readers-since-write (O(n * regs touched)) and
 * memory pairs only among the region's accesses */
static void js_preds(const struct js_ins *ins, int n, js_m *pred)
{
	int8_t lw[32];
	js_m rd[32], has_w = 0, has_r = 0;   /* lw/rd[r] valid iff bit r set */
	/* memory: guest accesses form a chain; private ($fp/$sp) accesses since
	 * the last guest access are checked pairwise for overlap (loads only
	 * against stores) */
	js_m last_g = 0, seg_ld = 0, seg_st = 0;
#if JITSCHED_MUTANT == 2
	js_m mem = 0;
#endif
	for (int j = 0; j < n; j++) {
		const struct js_ins *b = &ins[j];
		js_m p = 0;
		for (js_m m = b->src; m; m &= m - 1) {
			const int r = js_ctz(m);
			if (has_w >> r & 1) p |= 1u << lw[r];
		}
		for (js_m m = b->dst; m; m &= m - 1) {
			const int r = js_ctz(m);
			if (has_w >> r & 1) p |= 1u << lw[r];
#if JITSCHED_MUTANT != 1
			if (has_r >> r & 1) p |= rd[r];
#endif
		}
#if JITSCHED_MUTANT == 2
		if (b->kind >= JS_LD) {
			for (js_m m = mem; m; m &= m - 1)
				if (js_mem_dep(&ins[js_ctz(m)], b)) p |= m & -m;
			mem |= 1u << j;
		}
#else
		if (b->kind >= JS_LD) {
			const js_m bit = 1u << j;
			if (!js_private(b)) {
				p |= last_g | seg_ld | seg_st;
				last_g = bit; seg_ld = seg_st = 0;
			} else {
				p |= last_g;
				for (js_m m = b->kind == JS_ST ? seg_ld | seg_st : seg_st; m; m &= m - 1) {
					const struct js_ins *a = &ins[js_ctz(m)];
					if (a->base == b->base && a->lo < b->hi && b->lo < a->hi) p |= m & -m;
				}
				if (b->kind == JS_ST) seg_st |= bit; else seg_ld |= bit;
			}
		}
#endif
		pred[j] = p & ~(1u << j);
		for (js_m m = b->src; m; m &= m - 1) {
			const int r = js_ctz(m);
			rd[r] = ((has_r >> r & 1) ? rd[r] : 0) | 1u << j;
			has_r |= 1u << r;
		}
		for (js_m m = b->dst; m; m &= m - 1) {
			const int r = js_ctz(m);
			lw[r] = (int8_t)j; has_w |= 1u << r; has_r &= ~(1u << r);
		}
	}
}

/* modelled cost (tenths) of issuing ins[ord[0..n)] then a consumer 'sink' */
static unsigned js_cost(const struct js_ins *ins, const uint8_t *ord, int n, js_m sink)
{
	unsigned t = 0, ready[32];
	js_m valid = 0;   /* ready[r] valid iff bit r set, else 0 */
	for (int k = 0; k <= n; k++) {
		const js_m s = (k < n ? ins[ord[k]].src : sink) & valid;
		unsigned is = t;
		for (js_m m = s; m; m &= m - 1)
			if (ready[js_ctz(m)] > is) is = ready[js_ctz(m)];
		if (k == n) return is;
		const struct js_ins *d = &ins[ord[k]];
		for (js_m m = d->dst; m; m &= m - 1)
			ready[js_ctz(m)] = is + d->rdy;
		valid |= d->dst;
		t = is + 10;
	}
	return t;
}

/* schedule one region of n (<= JS_MAXR) already-decoded words in place;
 * returns 1 if changed */
static int js_region(uint32_t *w, const struct js_ins *ins, int n, js_m sink)
{
	js_m pred[JS_MAXR];
	unsigned height[JS_MAXR];
	uint8_t ord[JS_MAXR], id[JS_MAXR];
	if (n < 2) return 0;
	for (int i = 0; i < n; i++) id[i] = (uint8_t)i;
	const unsigned base = js_cost(ins, id, n, sink);
	if (base < (unsigned)n * 10 + JS_MINWIN) return 0;   /* too little stall to win back */
	js_preds(ins, n, pred);
	for (int i = 0; i < n; i++)                       /* critical path to the sink */
		height[i] = (ins[i].dst & sink) ? ins[i].rdy : 10;
	for (int j = n - 1; j >= 0; j--)                  /* height[j] is final here */
		for (js_m m = pred[j]; m; m &= m - 1) {
			const int i = js_ctz(m);
			const unsigned e = ((ins[i].dst & ins[j].src) ? ins[i].rdy : 10) + height[j];
			if (e > height[i]) height[i] = e;
		}
	js_m done = 0, valid = 0, have_est = 0;
	unsigned t = 0, ready[32], est[JS_MAXR];
	const js_m all = n == 32 ? ~0u : (1u << n) - 1;
	for (int k = 0; k < n; k++) {
		int best = -1; unsigned best_is = 0;
		/* candidates: the next JS_LOOK unscheduled words in original order */
		const int first = js_ctz(~done);
		const js_m look = first + JS_LOOK >= 32 ? ~0u : (1u << (first + JS_LOOK)) - 1;
		for (js_m c = ~done & all & look; c; c &= c - 1) {
			const int i = js_ctz(c);
			if (pred[i] & ~done) continue;
			/* source ready times are final once i's preds are scheduled:
			 * any later writer of a source stays after i (WAR edge) */
			if (!(have_est >> i & 1)) {
				unsigned e = 0;
				for (js_m m = ins[i].src & valid; m; m &= m - 1)
					if (ready[js_ctz(m)] > e) e = ready[js_ctz(m)];
				est[i] = e;
				have_est |= 1u << i;
			}
			const unsigned is = est[i] > t ? est[i] : t;
			if (best < 0 || is < best_is || (is == best_is && height[i] > height[best])) {
				best = i; best_is = is;
			}
		}
		ord[k] = (uint8_t)best;
		done |= 1u << best;
		for (js_m m = ins[best].dst; m; m &= m - 1)
			ready[js_ctz(m)] = best_is + ins[best].rdy;
		valid |= ins[best].dst;
		t = best_is + 10;
	}
	if (js_cost(ins, ord, n, sink) >= base) return 0;
	uint32_t tmp[JS_MAXR];
	for (int k = 0; k < n; k++) tmp[k] = w[ord[k]];
	memcpy(w, tmp, n * sizeof *w);
	return 1;
}

/* schedule [start, end). Returns the number of regions changed. One decode
 * per word: a region grows until a barrier (pinned, and so is the word after
 * it: delay slot, or conservatively the next word for unknown opcodes), an
 * in-block branch target, or JS_MAXR words. */
static int jitsched_block(uint32_t *start, uint32_t *end)
{
	const int n = (int)(end - start);
	if (n < 2 || n > 4096) return 0;
	uint8_t tgt[4096];
	memset(tgt, 0, n);
	for (int i = 0; i < n; i++) {
		const uint32_t w = start[i];
		if (js_is_branch(w)) {
			const int t = i + 1 + (int16_t)w;
			if (t >= 0 && t < n) tgt[t] = 1;
		} else if ((w >> 26) == 0x02 || (w >> 26) == 0x03) {
			const uintptr_t a = (uintptr_t)&start[i + 1];
			const uintptr_t ta = (a & 0xf0000000u) | ((w & 0x03ffffffu) << 2);
			if (ta >= (uintptr_t)start && ta < (uintptr_t)end) tgt[(ta - (uintptr_t)start) >> 2] = 1;
		}
	}
	struct js_ins ins[JS_MAXR];
	int changed = 0, r0 = 0, k = 0;   /* current region: start[r0 .. r0+k) */
	for (int i = 0; i < n; i++) {
		if (tgt[i] || k == JS_MAXR) {
			changed += js_region(start + r0, ins, k, ~0u);
			k = 0;
		}
		if (js_decode(start[i], &ins[k]) == JS_BAR) {
			changed += js_region(start + r0, ins, k, js_bar_src(start[i]));
			k = 0;
			/* the next word stays put too; if it is a barrier itself
			 * (e.g. a branch after an unknown op) so does its successor */
			struct js_ins x;
			for (i++; i < n && js_decode(start[i], &x) == JS_BAR; i++)
				;
			continue;
		}
		if (k == 0) r0 = i;
		k++;
	}
	changed += js_region(start + r0, ins, k, 0);
	return changed;
}

#endif
