/* stallmodel: qemu TCG plugin -- static in-order interlock model of the X1000
 * (single issue, no OoO) per translated block, weighted by execution.
 *
 * Per TB, at translation: walk the instructions with a register-ready table
 * (32 GPRs + HI + LO), all ready at TB entry; an instruction issues when its
 * sources are ready; load results are ready lat_ld cycles after issue,
 * mult/mul lat_mul, div lat_div, everything else 1. Stall = cycles beyond one
 * per instruction. Cross-TB dependencies and cache misses are NOT modelled
 * (both make the real number higher).
 *
 * Gating as icmodel/dcmodel: 1st getppid() opens the window, 2nd dumps.
 * Args: out=PREFIX jit=HEX:HEX ld=3 mul=4 div=36
 * Output PREFIX.txt: per 64 KB window (va>>16) insns, stall cycles,
 *   stall/(insn+stall), plus the JIT region total and its top stall TBs.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <glib.h>
#include <qemu-plugin.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

static int tbdump, on, marks, lat_ld = 3, lat_mul = 4, lat_div = 36;
static const char *outp = "stallmodel";
static uint32_t jlo, jhi;
static uint64_t win_insn[65536], win_stall[65536], jit_insn, jit_stall, tot_insn, tot_stall;
struct tbi { uint32_t va, n, stall, ld_stall, mul_stall; uint64_t exec; uint32_t *w; };   /* w: JIT TBs only */
static GPtrArray *tbs;
static uint64_t jit_ld_stall, jit_mul_stall;

enum { HI = 32, LO = 33, NONE = 34 };
/* decode: up to 3 sources, 1 destination, latency class */
static void dec(uint32_t w, int *s, int *ns, int *d, int *lat)
{
	const int op = w >> 26, rs = (w >> 21) & 31, rt = (w >> 16) & 31, rd = (w >> 11) & 31, fn = w & 63;
	*ns = 0; *d = NONE; *lat = 1;
#define S(r) (s[(*ns)++] = (r))
	switch (op) {
	case 0:   /* SPECIAL */
		switch (fn) {
		case 0: case 2: case 3: S(rt); *d = rd; break;                     /* sll srl sra */
		case 4: case 6: case 7: S(rs); S(rt); *d = rd; break;              /* sllv ... */
		case 8: S(rs); break;                                               /* jr */
		case 9: S(rs); *d = rd; break;                                      /* jalr */
		case 0x0a: case 0x0b: S(rs); S(rt); S(rd); *d = rd; break;          /* movz movn */
		case 0x10: S(HI); *d = rd; break;
		case 0x12: S(LO); *d = rd; break;
		case 0x11: S(rs); *d = HI; break;
		case 0x13: S(rs); *d = LO; break;
		case 0x18: case 0x19: S(rs); S(rt); *d = LO; *lat = lat_mul; break; /* mult: HI too (approx: LO) */
		case 0x1a: case 0x1b: S(rs); S(rt); *d = LO; *lat = lat_div; break;
		case 0x0c: case 0x0d: case 0x0f: break;                             /* syscall break sync */
		default: S(rs); S(rt); *d = rd; break;                              /* ALU r-type */
		}
		break;
	case 1: S(rs); if (rt & 0x10) *d = 31; break;                           /* REGIMM, bltzal */
	case 2: break;                                                          /* j */
	case 3: *d = 31; break;                                                 /* jal */
	case 4: case 5: S(rs); S(rt); break;                                    /* beq bne */
	case 6: case 7: S(rs); break;
	case 0x0f: *d = rt; break;                                              /* lui */
	case 0x1c:   /* SPECIAL2 */
		if (fn == 2) { S(rs); S(rt); *d = rd; *lat = lat_mul; }             /* mul */
		else if (fn == 0 || fn == 1 || fn == 4 || fn == 5) { S(rs); S(rt); S(LO); *d = LO; *lat = lat_mul; } /* madd/msub */
		else if (fn == 0x20 || fn == 0x21) { S(rs); *d = rd; }              /* clz clo */
		break;                                                              /* MXU etc: no GPR deps modelled */
	case 0x1f:   /* SPECIAL3 */
		if (fn == 0) { S(rs); *d = rt; }                                    /* ext */
		else if (fn == 4) { S(rs); S(rt); *d = rt; }                        /* ins */
		else if (fn == 0x20) { S(rt); *d = rd; }                            /* seb seh wsbh */
		break;
	case 0x20: case 0x21: case 0x23: case 0x24: case 0x25: S(rs); *d = rt; *lat = lat_ld; break;
	case 0x22: case 0x26: S(rs); S(rt); *d = rt; *lat = lat_ld; break;     /* lwl lwr */
	case 0x28: case 0x29: case 0x2a: case 0x2b: case 0x2e: S(rs); S(rt); break;
	case 0x30: S(rs); *d = rt; *lat = lat_ld; break;                        /* ll */
	case 0x38: S(rs); S(rt); *d = rt; break;                                /* sc */
	default:
		if (op >= 8 && op <= 0x0e) { S(rs); *d = rt; }                      /* addi..xori */
		else if (op == 0x31 || op == 0x35 || op == 0x39 || op == 0x3d) S(rs); /* fp load/store base */
		break;
	}
	for (int i = 0; i < *ns; i++) if (s[i] == 0) s[i] = NONE;               /* $zero always ready */
	if (*d == 0) *d = NONE;
#undef S
}

static void tb_exec(unsigned int vcpu, void *ud)
{
	(void)vcpu;
	if (!on) return;
	struct tbi *t = ud;
	t->exec++;
	const uint32_t wnd = t->va >> 16;
	win_insn[wnd] += t->n; win_stall[wnd] += t->stall;
	tot_insn += t->n; tot_stall += t->stall;
	if (t->va >= jlo && t->va < jhi) { jit_insn += t->n; jit_stall += t->stall; jit_ld_stall += t->ld_stall; jit_mul_stall += t->mul_stall; }
}

static void tb_trans(qemu_plugin_id_t id, struct qemu_plugin_tb *tb)
{
	(void)id;
	const size_t n = qemu_plugin_tb_n_insns(tb);
	if (!n) return;
	struct tbi *t = g_new0(struct tbi, 1);
	t->va = (uint32_t)qemu_plugin_tb_vaddr(tb);
	t->n = (uint32_t)n;
	if (t->va >= jlo && t->va < jhi) t->w = g_new(uint32_t, n);
	uint64_t ready[35] = {0}, cyc = 0;
	int kind[35] = {0};   /* 1 = produced by a load, 2 = by mul/div */
	for (size_t i = 0; i < n; i++) {
		struct qemu_plugin_insn *in = qemu_plugin_tb_get_insn(tb, i);
		uint32_t w = 0;
		qemu_plugin_insn_data(in, &w, sizeof w);
		if (t->w) t->w[i] = w;
		int s[4], ns, d, lat;
		dec(w, s, &ns, &d, &lat);
		uint64_t issue = cyc;
		int why = 0;
		for (int k = 0; k < ns; k++) if (s[k] < NONE && ready[s[k]] > issue) { issue = ready[s[k]]; why = kind[s[k]]; }
		const uint32_t st = (uint32_t)(issue - cyc);
		t->stall += st;
		if (why == 1) t->ld_stall += st; else if (why == 2) t->mul_stall += st;
		cyc = issue + 1;
		if (d < NONE) { ready[d] = issue + lat; kind[d] = lat == lat_ld && lat > 1 ? 1 : (lat > 1 ? 2 : 0); }
		if (d == LO && lat > 1) { ready[HI] = issue + lat; kind[HI] = 2; }
	}
	g_ptr_array_add(tbs, t);
	qemu_plugin_register_vcpu_tb_exec_cb(tb, tb_exec, QEMU_PLUGIN_CB_NO_REGS, t);
}

static gint by_stall(gconstpointer a, gconstpointer b)
{
	const struct tbi *x = *(struct tbi *const *)a, *y = *(struct tbi *const *)b;
	const uint64_t sx = (uint64_t)x->stall * x->exec, sy = (uint64_t)y->stall * y->exec;
	return sx < sy ? 1 : sx > sy ? -1 : 0;
}

static void dump(void)
{
	char p[512];
	snprintf(p, sizeof p, "%s.txt", outp);
	FILE *f = fopen(p, "w");
	if (!f) return;
	fprintf(f, "# in-order interlock model: ld=%d mul=%d div=%d (no cross-TB deps, no cache misses)\n", lat_ld, lat_mul, lat_div);
	fprintf(f, "TOTAL insns %" PRIu64 " stall %" PRIu64 " stall/cycle %.3f\n", tot_insn, tot_stall,
		(double)tot_stall / (tot_insn + tot_stall));
	fprintf(f, "JIT %08x-%08x insns %" PRIu64 " (%.1f%%) stall %" PRIu64 " (%.1f%% of all stall) stall/cycle %.3f  [load %" PRIu64 " mul/div %" PRIu64 "]\n",
		jlo, jhi, jit_insn, 100.0 * jit_insn / (tot_insn ? tot_insn : 1), jit_stall, 100.0 * jit_stall / (tot_stall ? tot_stall : 1),
		(double)jit_stall / (jit_insn + jit_stall ? jit_insn + jit_stall : 1), jit_ld_stall, jit_mul_stall);
	for (int i = 0; i < 65536; i++)
		if (win_insn[i] * 200 > tot_insn)   /* windows with >= 0.5% of insns */
			fprintf(f, "WIN %04x insns %5.1f%% stall %5.1f%% stall/cycle %.3f\n", i, 100.0 * win_insn[i] / tot_insn,
				100.0 * win_stall[i] / (tot_stall ? tot_stall : 1), (double)win_stall[i] / (win_insn[i] + win_stall[i]));
	g_ptr_array_sort(tbs, by_stall);
	for (guint i = 0; i < tbs->len && i < 40; i++) {
		const struct tbi *t = g_ptr_array_index(tbs, i);
		if (!t->exec) break;
		fprintf(f, "TB %08x n=%u stall=%u (ld %u mul %u) exec=%" PRIu64 " weighted=%" PRIu64 "%s\n", t->va, t->n, t->stall,
			t->ld_stall, t->mul_stall, t->exec, (uint64_t)t->stall * t->exec, t->va >= jlo && t->va < jhi ? " JIT" : "");
	}
	if (tbdump) {   /* every executed TB: va n exec (diff two runs per TB) */
		snprintf(p, sizeof p, "%s.tbs", outp);
		FILE *g = fopen(p, "w");
		for (guint i = 0; g && i < tbs->len; i++) {
			const struct tbi *t = g_ptr_array_index(tbs, i);
			if (t->exec) fprintf(g, "%08x %u %" PRIu64 "\n", t->va, t->n, t->exec);
		}
		if (g) fclose(g);
	}
	/* JIT-only top list with the raw words (objdump -b binary -m mips:isa32r2 -EL) */
	for (guint i = 0, k = 0; i < tbs->len && k < 60; i++) {
		const struct tbi *t = g_ptr_array_index(tbs, i);
		if (!t->exec) break;
		if (!t->w || !t->stall) continue;
		k++;
		fprintf(f, "JTB %08x n=%u stall=%u (ld %u mul %u) exec=%" PRIu64 " weighted=%" PRIu64 "\n ", t->va, t->n, t->stall,
			t->ld_stall, t->mul_stall, t->exec, (uint64_t)t->stall * t->exec);
		for (uint32_t j = 0; j < t->n; j++) fprintf(f, " %08x", t->w[j]);
		fputc('\n', f);
	}
	fclose(f);
}

static void sys_cb(qemu_plugin_id_t id, unsigned int vcpu, int64_t num, uint64_t a1, uint64_t a2,
		   uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7, uint64_t a8)
{
	(void)id; (void)vcpu; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7; (void)a8;
	if (num != 4064) return;   /* o32 getppid */
	marks++;
	if (marks == 1) on = 1;
	else if (marks == 2) { on = 0; dump(); }
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id, const qemu_info_t *info, int argc, char **argv)
{
	(void)info;
	for (int i = 0; i < argc; i++) {
		char *v = strchr(argv[i], '=');
		if (!v) continue;
		*v++ = 0;
		if (!strcmp(argv[i], "jit")) sscanf(v, "%x:%x", &jlo, &jhi);
		else if (!strcmp(argv[i], "out")) outp = strdup(v);
		else if (!strcmp(argv[i], "ld")) lat_ld = atoi(v);
		else if (!strcmp(argv[i], "mul")) lat_mul = atoi(v);
		else if (!strcmp(argv[i], "div")) lat_div = atoi(v);
		else if (!strcmp(argv[i], "tbdump")) tbdump = atoi(v);
	}
	tbs = g_ptr_array_new();
	qemu_plugin_register_vcpu_tb_trans_cb(id, tb_trans);
	qemu_plugin_register_vcpu_syscall_cb(id, sys_cb);
	return 0;
}
