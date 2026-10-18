/* Host self-check for jitsched.h (the mipsrec post-emission scheduler).
 *
 * Random blocks of the opcodes mipsrec emits (ALU, shifts, HI/LO, MUL, EXT/
 * INS, loads/stores on $fp/$sp/guest bases, branches + delay slots, branch
 * targets, unknown words). Each block is scheduled, then an INDEPENDENT
 * straight-line interpreter runs original and scheduled code from random
 * states, from the entry and from every in-block branch target, and compares
 * GPRs/HI/LO, all memory, and the ORDERED log of guest (non-$fp/$sp)
 * accesses (= device side-effect order / faultmem sites). Branches execute as
 * no-ops; every barrier word and delay slot must stay at its address.
 *
 * Build+run on host:
 *   cc -O2 -o t test_jitsched.c && ./t                 (must pass)
 *   cc -O2 -DJITSCHED_MUTANT=1 -o m test_jitsched.c && ./m   (must FAIL)
 *   cc -O2 -DJITSCHED_MUTANT=2 -o m test_jitsched.c && ./m   (must FAIL)
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "../jitsched.h"

typedef uint32_t u32;
static u32 rng = 12345;
static u32 rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static int pick(int n) { return (int)(rnd() % (u32)n); }

/* register roles: data regs get random ALU results, address regs only ever
 * advance by small aligned steps (so accesses stay inside their arena) */
static const int DATA[] = { 2, 3, 8, 9, 10, 11, 16, 17, 18, 19, 20, 21, 22, 23 };
static const int GADDR[] = { 1, 4, 5 };   /* $at $a0 $a1: guest bases */
#define ND (int)(sizeof DATA / sizeof DATA[0])
#define D() DATA[pick(ND)]

#define R(op, rs, rt, rd, sa, fn) ((u32)(op) << 26 | (u32)(rs) << 21 | (u32)(rt) << 16 | (u32)(rd) << 11 | (u32)(sa) << 6 | (fn))
#define I(op, rs, rt, imm) ((u32)(op) << 26 | (u32)(rs) << 21 | (u32)(rt) << 16 | ((u32)(imm) & 0xffff))

static u32 gen_mem(void)
{
	static const int lop[] = { 0x20, 0x21, 0x23, 0x24, 0x25, 0x22, 0x26 }, sop[] = { 0x28, 0x29, 0x2b, 0x2a, 0x2e };
	const int st = pick(2), cls = pick(3);   /* 0 fp, 1 sp, 2 guest */
	const int base = cls == 0 ? 30 : cls == 1 ? 29 : GADDR[pick(3)];
	const int op = st ? sop[pick(5)] : lop[pick(7)];
	int off = (pick(16) - 4) * 4;
	if ((op & 3) == 1) off += 2 * pick(2);
	else if ((op & 3) == 0 || op == 0x22 || op == 0x26 || op == 0x2a || op == 0x2e) off += pick(4);
	return I(op, base, D(), off);
}

static u32 gen_one(void)
{
	switch (pick(14)) {
	case 0: return R(0, D(), D(), D(), 0, 0x21 + 2 * pick(4));   /* addu subu or nor */
	case 1: return R(0, D(), D(), D(), 0, 0x24 + pick(4));                 /* and or xor nor */
	case 2: return R(0, 0, D(), D(), pick(32), pick(2) ? 0 : 2 + pick(2));   /* sll srl sra */
	case 3: return I(0x09 + pick(6), D(), D(), rnd());                      /* addiu..xori */
	case 4: return I(0x0f, 0, D(), rnd());                                   /* lui */
	case 5: return R(0, D(), D(), D(), 0, 0x0a + pick(2));                   /* movz movn */
	case 6: return pick(2) ? R(0, D(), D(), 0, 0, 0x18 + pick(2)) : R(0, 0, 0, D(), 0, 0x10 + 2 * pick(2)); /* mult / mfhi mflo */
	case 7: return R(0x1c, D(), D(), D(), 0, 2);                             /* mul */
	case 8: return pick(2) ? R(0x1f, D(), D(), 7, 3, 0) : R(0x1f, D(), D(), 9, 4, 4);   /* ext / ins */
	case 9: { const int ar = GADDR[pick(3)]; return I(0x09, ar, ar, 4 * (pick(5) - 2)); }   /* address reg step */
	default: return gen_mem();
	}
}

/* ---- independent interpreter (straight line; branches/barriers = no-op) ---- */
#define MEM 0x4000
struct st { u32 r[32], hi, lo; uint8_t m[MEM]; u32 log[512]; int nlog; };
static u32 sext(u32 v, int b) { return (u32)((int32_t)(v << (32 - b)) >> (32 - b)); }
static u32 rd32(struct st *s, u32 a) { return s->m[a] | s->m[a+1] << 8 | s->m[a+2] << 16 | (u32)s->m[a+3] << 24; }
static void wr32(struct st *s, u32 a, u32 v) { for (int i = 0; i < 4; i++) s->m[a+i] = (uint8_t)(v >> 8*i); }
static void glog(struct st *s, u32 base, u32 w, u32 a, u32 v)
{
	if (base == 29 || base == 30) return;
	if (s->nlog < 512) { s->log[s->nlog++] = w; s->log[s->nlog++] = a ^ v * 2654435761u; }
}
static int addr_ok(u32 a) { return a < MEM - 4; }

static int exec1(struct st *s, u32 w)
{
	const int op = w >> 26, rs = (w >> 21) & 31, rt = (w >> 16) & 31, rd = (w >> 11) & 31, sa = (w >> 6) & 31, fn = w & 63;
	u32 *r = s->r; const u32 A = r[rs], B = r[rt]; const u32 imm = sext(w & 0xffff, 16);
	u32 v = 0; int wd = -1;
	switch (op) {
	case 0:
		switch (fn) {
		case 0: v = B << sa; wd = rd; break; case 2: v = B >> sa; wd = rd; break; case 3: v = (u32)((int32_t)B >> sa); wd = rd; break;
		case 4: v = B << (A & 31); wd = rd; break; case 6: v = B >> (A & 31); wd = rd; break; case 7: v = (u32)((int32_t)B >> (A & 31)); wd = rd; break;
		case 0x0a: if (!B) { v = A; wd = rd; } break; case 0x0b: if (B) { v = A; wd = rd; } break;
		case 0x10: v = s->hi; wd = rd; break; case 0x12: v = s->lo; wd = rd; break;
		case 0x11: s->hi = A; break; case 0x13: s->lo = A; break;
		case 0x18: { int64_t p = (int64_t)(int32_t)A * (int32_t)B; s->lo = (u32)p; s->hi = (u32)(p >> 32); break; }
		case 0x19: { uint64_t p = (uint64_t)A * B; s->lo = (u32)p; s->hi = (u32)(p >> 32); break; }
		case 0x21: v = A + B; wd = rd; break; case 0x23: v = A - B; wd = rd; break;
		case 0x24: v = A & B; wd = rd; break; case 0x25: v = A | B; wd = rd; break;
		case 0x26: v = A ^ B; wd = rd; break; case 0x27: v = ~(A | B); wd = rd; break;
		case 0x2a: v = (int32_t)A < (int32_t)B; wd = rd; break; case 0x2b: v = A < B; wd = rd; break;
		default: break;   /* jr etc: no-op */
		}
		break;
	case 0x09: v = A + imm; wd = rt; break; case 0x0a: v = (int32_t)A < (int32_t)imm; wd = rt; break;
	case 0x0b: v = A < imm; wd = rt; break; case 0x0c: v = A & (w & 0xffff); wd = rt; break;
	case 0x0d: v = A | (w & 0xffff); wd = rt; break; case 0x0e: v = A ^ (w & 0xffff); wd = rt; break;
	case 0x0f: v = w << 16; wd = rt; break;
	case 0x1c: if (fn == 2) { v = A * B; wd = rd; s->hi = 0xdead; s->lo = 0xbeef; } break;
	case 0x1f:
		if (fn == 0) { v = (A >> sa) & (u32)((1ull << (rd + 1)) - 1); wd = rt; }
		else if (fn == 4) { const u32 m = (u32)((1ull << (rd - sa + 1)) - 1) << sa; v = (B & ~m) | ((A << sa) & m); wd = rt; }
		break;
	default:
		if (op >= 0x20 && op <= 0x2e) {
			const u32 a = A + imm, al = a & ~3u, sh = (a & 3) * 8;
			if (!addr_ok(al)) return -1;
			switch (op) {
			case 0x20: v = sext(s->m[a], 8); wd = rt; break; case 0x24: v = s->m[a]; wd = rt; break;
			case 0x21: v = sext(s->m[a] | s->m[a+1] << 8, 16); wd = rt; break; case 0x25: v = s->m[a] | s->m[a+1] << 8; wd = rt; break;
			case 0x23: v = rd32(s, a); wd = rt; break;
			case 0x22: v = (rd32(s, al) << (24 - sh)) | (B & (0x00ffffffu >> sh)); wd = rt; break;
			case 0x26: v = (rd32(s, al) >> sh) | (sh ? (B & (0xffffff00u << (24 - sh))) : 0); wd = rt; break;
			case 0x28: s->m[a] = (uint8_t)B; break; case 0x29: s->m[a] = (uint8_t)B; s->m[a+1] = (uint8_t)(B >> 8); break;
			case 0x2b: wr32(s, a, B); break;
			case 0x2a: wr32(s, al, (sh == 24 ? 0 : (rd32(s, al) & (0xffffff00u << sh))) | (B >> (24 - sh))); break;
			case 0x2e: wr32(s, al, (rd32(s, al) & (0x00ffffffu >> (24 - sh))) | (B << sh)); break;
			}
			glog(s, rs, w, a, op >= 0x28 ? B : (wd >= 0 ? v : 0));
		}
	}
	if (wd > 0) r[wd] = v;
	return 0;
}

static int run(struct st *s, const u32 *w, int from, int n)
{
	for (int i = from; i < n; i++) if (exec1(s, w[i])) return -1;
	return 0;
}

/* js_preds (direct edges) must have the same transitive closure as the
 * pairwise reference rule js_dep */
static int closure_ok(const u32 *w, int n)
{
	struct js_ins ins[64]; js_m p1[64], p2[64];
	for (int i = 0; i < n; i++) js_decode(w[i], &ins[i]);
	js_preds(ins, n, p1);
	for (int j = 0; j < n; j++) { p2[j] = 0; for (int i = 0; i < j; i++) if (js_dep(&ins[i], &ins[j])) p2[j] |= 1u << i; }
	for (int j = 0; j < n; j++)   /* closure: preds of preds, in index order */
		for (int i = j - 1; i >= 0; i--) {
			if (p1[j] >> i & 1) p1[j] |= p1[i];
			if (p2[j] >> i & 1) p2[j] |= p2[i];
		}
	for (int j = 0; j < n; j++) if (p1[j] != p2[j]) return 0;
	return 1;
}

int main(int argc, char **argv)
{
	const int iters = argc > 1 ? atoi(argv[1]) : 200000;
	long changed = 0, regions = 0, fails = 0, runs = 0, skipped = 0;
	u32 fp = 2166136261u;   /* fingerprint of every scheduled block */
	static struct st s0, a, b;
	for (int it = 0; it < iters && fails < 5; it++) {
		u32 w[96], o[96]; uint8_t ispin[96] = {0};
		const int n = 4 + pick(60);
		for (int i = 0; i < n; i++) w[i] = gen_one();
		/* sprinkle branches (+ delay slots), jal, unknown words */
		for (int k = pick(4); k > 0; k--) {
			const int i = pick(n - 1);
			switch (pick(5)) {
			case 0: w[i] = I(0x04 + pick(2), D(), D(), pick(n) - i - 1); break;   /* beq/bne in block */
			case 1: w[i] = I(0x06 + pick(2), D(), 0, pick(n) - i - 1); break;
			case 2: w[i] = R(0, 31, 0, 0, 0, 8); break;                            /* jr ra */
			case 3: w[i] = 0x0c000000u | pick(1 << 20); break;                     /* jal */
			case 4: w[i] = R(0x12, 0, D(), D(), 0, 0); break;                      /* cop2: unknown */
			}
		}
		for (int i = 0; i < n; i++) o[i] = w[i];
		if (!closure_ok(w, n < JS_MAXR ? n : JS_MAXR)) { printf("FAIL it=%d: js_preds closure != js_dep closure\n", it); fails++; }
		/* which words must stay put: barrier + following word */
		for (int i = 0; i < n; i++) { struct js_ins d; if (js_decode(w[i], &d) == JS_BAR) { ispin[i] = 1; if (i + 1 < n) ispin[i+1] = 1; } }
		regions++;
		if (jitsched_block(w, w + n)) changed++;
		for (int i = 0; i < n; i++) fp = (fp ^ w[i]) * 16777619u;
		for (int i = 0; i < n; i++)
			if (ispin[i] && w[i] != o[i]) { printf("FAIL it=%d: pinned word %d moved\n", it, i); fails++; }
		/* entries: 0 + every in-block branch target */
		int ent[96], ne = 0; ent[ne++] = 0;
		for (int i = 0; i < n; i++) if (js_is_branch(o[i])) { const int t = i + 1 + (int16_t)o[i]; if (t > 0 && t < n) ent[ne++] = t; }
		for (int e = 0; e < ne; e++)
			for (int rep = 0; rep < 3; rep++) {
				for (int r = 0; r < 32; r++) s0.r[r] = rnd();
				s0.r[0] = 0; s0.r[30] = 0x0400; s0.r[29] = 0x0c00;
				for (int g = 0; g < 3; g++) s0.r[GADDR[g]] = 0x1800 + 0x400 * g;
				s0.hi = rnd(); s0.lo = rnd(); s0.nlog = 0;
				for (int i = 0; i < MEM; i++) s0.m[i] = (uint8_t)rnd();
				a = s0; b = s0;
				const int ra = run(&a, o, ent[e], n), rb = run(&b, w, ent[e], n);
				runs++;
				if (ra || rb) { skipped++; continue; }   /* address walked out of the arena */
				if (memcmp(a.r, b.r, sizeof a.r) || a.hi != b.hi || a.lo != b.lo || memcmp(a.m, b.m, MEM) ||
				    a.nlog != b.nlog || memcmp(a.log, b.log, a.nlog * sizeof(u32))) {
					printf("FAIL it=%d entry=%d n=%d\n", it, ent[e], n);
					for (int i = 0; i < n; i++) printf("  %2d %08x %08x%s\n", i, o[i], w[i], ispin[i] ? " pin" : "");
					fails++; e = ne; break;
				}
			}
	}
	printf("jitsched host test (MUTANT=%d): %d blocks, %ld changed, %ld runs (%ld skipped), %ld FAIL, fingerprint %08x\n",
	       JITSCHED_MUTANT, iters, changed, runs, skipped, fails, fp);
	(void)regions;
	return fails != 0;
}
