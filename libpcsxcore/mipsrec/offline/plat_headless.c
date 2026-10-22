/* plat_headless: replaces frontend/plat_erosq.o + drm_out.o so the shipped
 * pcsx objects run under qemu-user without a display (offline GT2 study).
 * Frames go to a RAM buffer. After HX_FRAMES flips (default 600) it writes
 * HX_OUT.ppm (last frame), calls mipsrec_blkprof_dump(HX_OUT.blk) when
 * EROSQ_BLKPROF=1, and exits. HX_MARK=N also dumps the (cumulative) counts
 * at frame N to HX_OUT.blk0, so warm-up can be subtracted. HX_FLUSH=N flushes
 * the JIT code cache at frame N (re-pack experiment). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <signal.h>
#include <ucontext.h>
#include "psxcommon.h"
#include "r3000a.h"
#include "../plugins/gpulib/gpu.h"

extern char __bss_start[], _end[];

extern int g_menuscreen_w, g_menuscreen_h, g_menuscreen_pp;
extern void *g_menuscreen_ptr, *g_menubg_ptr;
extern void *pl_vout_buf;
void mipsrec_blkprof_dump(const char *path) __attribute__((weak));
void mipsrec_jitsched_stats(unsigned long *, unsigned long *, unsigned long *) __attribute__((weak));
void mipsrec_blkext_stats(size_t *, size_t *) __attribute__((weak));
static void js_print(const char *at)
{
	unsigned long b, c, r;
	size_t xb, xp;
	if (mipsrec_blkext_stats) {
		mipsrec_blkext_stats(&xb, &xp);
		fprintf(stderr, "[hx] blkext %s: bytes=%zu peak=%zu\n", at, xb, xp);
	}
	if (!mipsrec_jitsched_stats) return;
	mipsrec_jitsched_stats(&b, &c, &r);
	fprintf(stderr, "[hx] jitsched %s: blocks=%lu changed=%lu regions=%lu\n", at, b, c, r);
}
void mipsrec_emitstat_dump(void) __attribute__((weak));
void mipsrec_rec_reset(void) __attribute__((weak));
void mipsrec_debug_regions(uintptr_t *v) __attribute__((weak));
void mipsrec_idleskip_report(void) __attribute__((weak));

int g_prof_on;
unsigned long long g_prof_gpu_ns, g_prof_present_ns, g_prof_spu_ns;
volatile int g_erosq_phase;
static uint16_t *fb, *menu_fb;
static int psx_w = 320, psx_h = 240;
static unsigned long frames, max_frames = 600, mark_frame, flush_frame;
static const char *out = "hx";

unsigned long long prof_now_ns(void) { return 0; }
int erosq_vol_get(void) { return 50; }
void erosq_vol_set(int pct) { (void)pct; }
void plat_trigger_vibrate(int pad, int low, int high) { (void)pad; (void)low; (void)high; }
void plat_minimize(void) {}

static void watch(int sig)
{
	(void)sig;   /* debug only: HX_WATCH=s prints where the guest is */
	fprintf(stderr, "[hx] watch frame %lu pc=%08x cycle=%08x\n", frames, psxRegs.pc, psxRegs.cycle);
	if (getenv("HX_WATCH_DUMP") && mipsrec_blkprof_dump) {   /* debug only, not signal-safe */
		char p[512]; snprintf(p, sizeof p, "%s.w%08x", out, psxRegs.cycle); mipsrec_blkprof_dump(p);
	}
	alarm(strtoul(getenv("HX_WATCH"), 0, 0));
}

static void trap(int sig, siginfo_t *si, void *ucv)
{
	(void)si;   /* debug: dump GPRs at a JIT 'break' */
	ucontext_t *uc = ucv;
	fprintf(stderr, "[hx] signal %d at host pc=%08x psx pc=%08x\n", sig, (unsigned)uc->uc_mcontext.pc, psxRegs.pc);
	for (int i = 0; i < 32; i++) fprintf(stderr, " r%d=%08x%s", i, (unsigned)uc->uc_mcontext.gregs[i], i % 8 == 7 ? "\n" : "");
	_exit(3);
}

/* GPU primitive mix (gpulib_if.cpp built with -DHX_GPU_MIX): per category
 * count and pre-clip pixel area, from HX_MARK on. */
static unsigned long long gm_n[64], gm_px[64];
static const char *gm_name[64];
static int gm_on;
static int gm_cat(const char *name)
{
	for (int i = 0; i < 64; i++) {
		if (!gm_name[i]) { gm_name[i] = name; return i; }
		if (gm_name[i] == name || !strcmp(gm_name[i], name)) return i;
	}
	return 63;
}
static int da_x1, da_y1, da_x2 = 1023, da_y2 = 511, do_x, do_y;   /* GP0 E3/E4/E5 */
/* twice the area of triangle (x,y)[3] after offset + clipping to the drawing
 * area (Sutherland-Hodgman), i.e. roughly the pixels the rasterizer touches */
static double tri2(int x0, int y0, int x1, int y1, int x2, int y2)
{
	double px[16], py[16], qx[16], qy[16];
	int n = 3;
	px[0] = x0 + do_x; py[0] = y0 + do_y; px[1] = x1 + do_x; py[1] = y1 + do_y; px[2] = x2 + do_x; py[2] = y2 + do_y;
	for (int e = 0; e < 4; e++) {
		int m = 0;
		for (int i = 0; i < n; i++) {
			int j = (i + 1) % n;
			double a = e == 0 ? px[i] - da_x1 : e == 1 ? da_x2 + 1 - px[i] : e == 2 ? py[i] - da_y1 : da_y2 + 1 - py[i];
			double b = e == 0 ? px[j] - da_x1 : e == 1 ? da_x2 + 1 - px[j] : e == 2 ? py[j] - da_y1 : da_y2 + 1 - py[j];
			if (a >= 0) { qx[m] = px[i]; qy[m] = py[i]; m++; }
			if ((a >= 0) != (b >= 0)) { double t = a / (a - b); qx[m] = px[i] + t * (px[j] - px[i]); qy[m] = py[i] + t * (py[j] - py[i]); m++; }
		}
		n = m; for (int i = 0; i < n; i++) { px[i] = qx[i]; py[i] = qy[i]; }
		if (n < 3) return 0;
	}
	double A = 0;
	for (int i = 0; i < n; i++) { int j = (i + 1) % n; A += px[i] * py[j] - px[j] * py[i]; }
	return A < 0 ? -A : A;
}
/* HX_OVERDRAW=1 (needs HX_GPUMIX): per-pixel overdraw within a frame (reset
 * at each flip). Each pixel write bumps a counter; an opaque write makes the
 * earlier writes to that pixel "wasted". Two covers: strict = untextured
 * opaque prims + fills (cannot have transparent texels), generous = any
 * non-semi-transparent prim. Coverage = pixel centres inside the clipped
 * prim (approximate edge rules: an estimate, not the rasterizer). */
static unsigned char *od_ws, *od_wg;
/* whole-prim ceiling: per pixel the prim that wrote it last (per cover rule);
 * a prim is skippable iff at frame end none of its pixels survive and none
 * were read by a later semi-transparent blend */
#define OD_MAXP 8192
static unsigned short *od_ls, *od_lg;   /* last writer id + 1, 0 = none */
static unsigned od_pid, od_alive_s[OD_MAXP], od_alive_g[OD_MAXP], od_ppx[OD_MAXP];
static unsigned char od_need_s[OD_MAXP], od_need_g[OD_MAXP];
static int od_tex;   /* current prim textured (set in hx_gpu_mix) */
static unsigned char od_ptex[OD_MAXP];   /* prim textured? */
static unsigned long long od_waste_s_tex, od_skip_s_tex;
static unsigned long long od_skip_s, od_skip_g, od_pr_s, od_pr_g, od_prims;
static unsigned long long od_total, od_waste_s, od_waste_g, od_semi;
static int od_on;
static inline void od_px(int x, int y, int strict, int opaque)
{
	unsigned i = (unsigned)y * 1024 + (unsigned)x;
	od_total++;
	if (od_pid && od_pid < OD_MAXP) {
		const unsigned me = od_pid;
		od_ppx[me]++; od_ptex[me] = od_tex;
		unsigned o = od_lg[i];   /* generous: any opaque write kills the old writer's pixel */
		if (o) { if (opaque) od_alive_g[o - 1]--; else od_need_g[o - 1] = 1; }
		od_lg[i] = me + 1; od_alive_g[me]++;
		o = od_ls[i];            /* strict: only untextured opaque writes (and fills) kill; */
		if (o) {                 /* semi reads it, textured-opaque may show it through */
			if (opaque && strict) { od_alive_s[o - 1]--; if (od_ptex[o - 1]) od_waste_s_tex++; }
			else od_need_s[o - 1] = 1;
		}
		od_ls[i] = me + 1; od_alive_s[me]++;
	}
	if (!opaque) { od_semi++; if (od_ws[i] < 255) od_ws[i]++; if (od_wg[i] < 255) od_wg[i]++; return; }
	od_waste_g += od_wg[i]; od_wg[i] = 1;
	if (strict) { od_waste_s += od_ws[i]; od_ws[i] = 1; }
	else if (od_ws[i] < 255) od_ws[i]++;
}
/* Phase 0: real texels. Texture state for the current prim: od_tex = 0
 * untextured, else 1; tpage/clut as GP0 encodes them. A texel of 0 is
 * transparent: no write, no cover. Also counts render-to-texture (a texel
 * read from a VRAM word written earlier this frame). */
static int od_tex, od_tpage, od_clut, od_e1, od_e6, od_su, od_sv, od_sx, od_sy;
static int od_rtt_hit;   /* this prim read a texel drawn this frame */
static unsigned long long od_tx_px, od_tx_transp, od_rtt_prims, od_mask_prims, od_semi_prims,
	od_copy, od_upload, od_read, od_tex_prims;
static inline int od_texel(int u, int v)   /* 1 = opaque texel, 0 = transparent */
{
	const uint16_t *vr = gpu.vram;
	int tx = (od_tpage & 0xf) * 64, ty = ((od_tpage >> 4) & 1) * 256, tp = (od_tpage >> 7) & 3;
	int cx = (od_clut & 0x3f) * 16, cy = (od_clut >> 6) & 0x1ff;
	u &= 0xff; v &= 0xff;
	unsigned a; uint16_t t;
	if (tp == 0) { a = (unsigned)(ty + v) * 1024 + ((tx + u / 4) & 1023); t = vr[cy * 1024 + cx + ((vr[a] >> ((u & 3) * 4)) & 0xf)]; }
	else if (tp == 1) { a = (unsigned)(ty + v) * 1024 + ((tx + u / 2) & 1023); t = vr[cy * 1024 + ((cx + ((vr[a] >> ((u & 1) * 8)) & 0xff)) & 1023)]; }
	else { a = (unsigned)(ty + v) * 1024 + ((tx + u) & 1023); t = vr[a]; }
	if (od_lg && od_lg[a & (1024 * 512 - 1)]) od_rtt_hit = 1;
	return t != 0;
}
static void od_rect(int x0, int y0, int x1, int y1, int strict, int opaque)   /* inclusive, already clipped */
{
	for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) {
		if (od_tex) { od_tx_px++; if (!od_texel(od_su + x - od_sx, od_sv + y - od_sy)) { od_tx_transp++; continue; } }
		od_px(x & 1023, y & 511, strict, opaque);
	}
}
static inline long long od_e(int ax, int ay, int bx, int by, int px, int py)
{ return (long long)(bx - ax) * (2 * py + 1 - 2 * ay) - (long long)(by - ay) * (2 * px + 1 - 2 * ax); }
static int od_uv[3][2];
static void od_tri(int x0, int y0, int x1, int y1, int x2, int y2, int strict, int opaque)
{
	int u[3] = { od_uv[0][0], od_uv[1][0], od_uv[2][0] }, v[3] = { od_uv[0][1], od_uv[1][1], od_uv[2][1] };
	x0 += do_x; y0 += do_y; x1 += do_x; y1 += do_y; x2 += do_x; y2 += do_y;
	if ((long long)(x1 - x0) * (y2 - y0) - (long long)(x2 - x0) * (y1 - y0) < 0) { int t = x1; x1 = x2; x2 = t; t = y1; y1 = y2; y2 = t;
		t = u[1]; u[1] = u[2]; u[2] = t; t = v[1]; v[1] = v[2]; v[2] = t; }
	const double A2 = (double)(x1 - x0) * (y2 - y0) - (double)(x2 - x0) * (y1 - y0);
	int bx0 = x0 < x1 ? (x0 < x2 ? x0 : x2) : (x1 < x2 ? x1 : x2), bx1 = x0 > x1 ? (x0 > x2 ? x0 : x2) : (x1 > x2 ? x1 : x2);
	int by0 = y0 < y1 ? (y0 < y2 ? y0 : y2) : (y1 < y2 ? y1 : y2), by1 = y0 > y1 ? (y0 > y2 ? y0 : y2) : (y1 > y2 ? y1 : y2);
	if (bx1 - bx0 > 1023 || by1 - by0 > 511) return;   /* the GPU culls these */
	if (bx0 < da_x1) bx0 = da_x1; if (by0 < da_y1) by0 = da_y1; if (bx1 > da_x2) bx1 = da_x2; if (by1 > da_y2) by1 = da_y2;
	for (int y = by0; y <= by1; y++) for (int x = bx0; x <= bx1; x++)
		if (od_e(x0, y0, x1, y1, x, y) >= 0 && od_e(x1, y1, x2, y2, x, y) >= 0 && od_e(x2, y2, x0, y0, x, y) >= 0) {
			if (od_tex && A2 > 0) {   /* affine uv at the pixel centre (edge fns are 2x) */
				double w0 = od_e(x1, y1, x2, y2, x, y) / (2 * A2), w1 = od_e(x2, y2, x0, y0, x, y) / (2 * A2), w2 = 1 - w0 - w1;
				od_tx_px++;
				if (!od_texel((int)(w0 * u[0] + w1 * u[1] + w2 * u[2]), (int)(w0 * v[0] + w1 * v[1] + w2 * v[2]))) { od_tx_transp++; continue; }
			}
			od_px(x, y, strict, opaque);
		}
}
static void od_frame(void)
{
	if (od_ws) { memset(od_ws, 0, 1024 * 512); memset(od_wg, 0, 1024 * 512); }
	if (od_ls && od_on && gm_on) {
		unsigned n = od_pid < OD_MAXP ? od_pid : OD_MAXP - 1;
		for (unsigned p = 1; p <= n; p++) {
			if (!od_ppx[p]) continue;
			od_prims++;
			if (!od_alive_s[p] && !od_need_s[p]) { od_skip_s += od_ppx[p]; od_pr_s++; if (od_ptex[p]) od_skip_s_tex += od_ppx[p]; }
			if (!od_alive_g[p] && !od_need_g[p]) { od_skip_g += od_ppx[p]; od_pr_g++; }
		}
	}
	if (od_ls) {
		memset(od_ls, 0, 1024 * 512 * 2); memset(od_lg, 0, 1024 * 512 * 2);
		memset(od_alive_s, 0, sizeof od_alive_s); memset(od_alive_g, 0, sizeof od_alive_g);
		memset(od_need_s, 0, sizeof od_need_s); memset(od_need_g, 0, sizeof od_need_g);
		memset(od_ppx, 0, sizeof od_ppx); od_pid = 0;
	}
}
static void od_prim_done(int tex, int semi)
{
	if (tex) od_tex_prims++;
	if (od_rtt_hit) od_rtt_prims++;
	if (od_e6) od_mask_prims++;
	if (semi) od_semi_prims++;
}
void hx_gpu_io(int kind)
{
	if (!od_on || !gm_on) return;
	if (kind == 0x80) od_copy++; else if (kind == 0xa0) od_upload++; else od_read++;
}
void hx_gpu_mix(uint32_t cmd, const uint32_t *w, uint32_t len)
{
	if (!gm_on && cmd < 0xe0) return;   /* only the env state is tracked when off */
	static char names[256][24];
	const char *nm; double px = 0;
	if (od_on && gm_on && ((cmd >= 0x20 && cmd < 0x40) || (cmd >= 0x60 && cmd < 0x80) || cmd == 0x02)) od_pid++;
	if (cmd >= 0x20 && cmd < 0x40) {
		int quad = cmd & 8, tex = cmd & 4, gou = cmd & 0x10, nv = quad ? 4 : 3, x[4], y[4], k = 1;
		int uv[4][2] = {{0}};
		for (int v = 0; v < nv; v++) {
			if (gou && v) k++;                    /* colour word */
			x[v] = (int16_t)(w[k] << 5) >> 5; y[v] = (int16_t)((w[k] >> 16) << 5) >> 5; k++;
			if (tex) {                            /* uv word: v0 carries CLUT, v1 the texpage */
				uv[v][0] = w[k] & 0xff; uv[v][1] = (w[k] >> 8) & 0xff;
				if (v == 0) od_clut = w[k] >> 16;
				if (v == 1) od_tpage = (w[k] >> 16) & 0x1ff;
				k++;
			}
		}
		od_tex = tex ? 1 : 0; od_rtt_hit = 0;
		px = tri2(x[0], y[0], x[1], y[1], x[2], y[2]);
		if (quad) px += tri2(x[1], y[1], x[2], y[2], x[3], y[3]);
		if (od_on && gm_on) {
			for (int i = 0; i < 3; i++) { od_uv[i][0] = uv[i][0]; od_uv[i][1] = uv[i][1]; }
			od_tri(x[0], y[0], x[1], y[1], x[2], y[2], !tex, !(cmd & 2));
			if (quad) { for (int i = 0; i < 3; i++) { od_uv[i][0] = uv[i + 1][0]; od_uv[i][1] = uv[i + 1][1]; }
				od_tri(x[1], y[1], x[2], y[2], x[3], y[3], !tex, !(cmd & 2)); }
			od_prim_done(tex, cmd & 2);
		}
		px /= 2;
		snprintf(names[cmd], 24, "%s%s%s%s%s", quad ? "quad" : "tri", gou ? "-gour" : "-flat",
			 tex ? "-tex" : "", tex && (cmd & 1) ? "-raw" : "", cmd & 2 ? "-semi" : "");
		nm = names[cmd];
	} else if (cmd >= 0x60 && cmd < 0x80) {
		int tex = cmd & 4, sz = (cmd >> 3) & 3, wdt, hgt;
		if (sz == 0) { uint32_t wh = w[tex ? 3 : 2]; wdt = wh & 0x3ff; hgt = (wh >> 16) & 0x1ff; }
		else { wdt = hgt = sz == 1 ? 1 : sz == 2 ? 8 : 16; }
		{	int x0 = ((int16_t)(w[1] << 5) >> 5) + do_x, y0 = ((int16_t)((w[1] >> 16) << 5) >> 5) + do_y;
			int cx0 = x0 < da_x1 ? da_x1 : x0, cy0 = y0 < da_y1 ? da_y1 : y0;
			int cx1 = x0 + wdt - 1 > da_x2 ? da_x2 : x0 + wdt - 1, cy1 = y0 + hgt - 1 > da_y2 ? da_y2 : y0 + hgt - 1;
			px = (cx1 >= cx0 && cy1 >= cy0) ? (double)(cx1 - cx0 + 1) * (cy1 - cy0 + 1) : 0;
			if (od_on && gm_on && px) {
				od_tex = tex ? 1 : 0; od_rtt_hit = 0;
				if (tex) { od_su = w[2] & 0xff; od_sv = (w[2] >> 8) & 0xff; od_clut = w[2] >> 16; od_tpage = od_e1 & 0x1ff; od_sx = x0; od_sy = y0; }
				od_rect(cx0, cy0, cx1, cy1, !tex, !(cmd & 2));
				od_prim_done(tex, cmd & 2);
			} }
		snprintf(names[cmd], 24, "%s%s%s", tex ? "sprite" : "tile", sz ? (sz == 1 ? "1" : sz == 2 ? "8" : "16") : "",
			 cmd & 2 ? "-semi" : "");
		nm = names[cmd];
	} else if (cmd >= 0x40 && cmd < 0x60) nm = "line";
	else if (cmd == 0x02) { px = (long)(w[2] & 0x3ff) * ((w[2] >> 16) & 0x1ff); nm = "fill";
		if (od_on && gm_on && px) { od_tex = 0; int fx = w[1] & 0x3f0, fy = (w[1] >> 16) & 0x1ff, fw = ((w[2] & 0x3ff) + 15) & ~15, fh = (w[2] >> 16) & 0x1ff;
			for (int y = 0; y < fh; y++) for (int x = 0; x < fw; x++) od_px((fx + x) & 1023, (fy + y) & 511, 1, 1); } }
	else if (cmd >= 0x80 && cmd < 0xe0) nm = "vram-copy";
	else if (cmd >= 0xe0) {
		uint32_t v = w[0];
		if (cmd == 0xe3) { da_x1 = v & 0x3ff; da_y1 = (v >> 10) & 0x1ff; }
		if (cmd == 0xe4) { da_x2 = v & 0x3ff; da_y2 = (v >> 10) & 0x1ff; }
		if (cmd == 0xe5) { do_x = (int)(v << 21) >> 21; do_y = (int)(v << 10) >> 21; }
		if (cmd == 0xe1) od_e1 = v & 0x3fff;
		if (cmd == 0xe6) od_e6 = v & 3;
		nm = "env(e1-e6)";
	}
	else nm = "other";
	if (!gm_on) return;   /* (env state above is tracked from the start) */
	int c = gm_cat(nm);
	gm_n[c]++; gm_px[c] += (unsigned long long)px;
	(void)len;
}
static void gm_dump(unsigned long nframes)
{
	if (od_on && od_total)
		fprintf(stderr, "[overdraw] per frame: writes=%.0f semi=%.1f%% wasted(strict)=%.1f%% wasted(exact-texel)=%.1f%%\n",
			(double)od_total / nframes, 100.0 * od_semi / od_total, 100.0 * od_waste_s / od_total, 100.0 * od_waste_g / od_total);
	if (od_on && od_total)
		fprintf(stderr, "[phase0] per frame: textured px=%.0f transparent=%.1f%% | prims: textured=%.1f semi=%.1f mask(E6)=%.1f render-to-texture=%.1f | vram copy=%.2f upload=%.2f read=%.2f\n",
			(double)od_tx_px / nframes, od_tx_px ? 100.0 * od_tx_transp / od_tx_px : 0,
			(double)od_tex_prims / nframes, (double)od_semi_prims / nframes, (double)od_mask_prims / nframes,
			(double)od_rtt_prims / nframes, (double)od_copy / nframes, (double)od_upload / nframes, (double)od_read / nframes);
	if (od_on && od_total)
		fprintf(stderr, "[phase0] strict-wasted writes on TEXTURED victims: %.1f%% of all writes; strict whole-prim skippable textured: %.1f%% of all writes\n",
			100.0 * od_waste_s_tex / od_total, 100.0 * od_skip_s_tex / od_total);
	if (od_on && od_prims)
		fprintf(stderr, "[overdraw] whole-prim skippable: strict %.1f%% of writes (%.1f prims/frame), exact-texel %.1f%% (%.1f prims/frame), of %.1f prims/frame\n",
			100.0 * od_skip_s / od_total, (double)od_pr_s / nframes, 100.0 * od_skip_g / od_total, (double)od_pr_g / nframes, (double)od_prims / nframes);
	unsigned long long tp = 0;
	for (int i = 0; i < 64 && gm_name[i]; i++) tp += gm_px[i];
	fprintf(stderr, "[gpumix] %lu frames; per frame: count, pixels clipped to the drawing area (share), px/prim\n", nframes);
	for (int i = 0; i < 64 && gm_name[i]; i++)
		fprintf(stderr, "[gpumix] %-22s %8.1f %9.0f %5.1f%% %8.1f\n", gm_name[i], (double)gm_n[i] / nframes,
			(double)gm_px[i] / nframes, tp ? 100.0 * gm_px[i] / tp : 0, gm_n[i] ? (double)gm_px[i] / gm_n[i] : 0);
}

static void cyc_hook(psxCP0Regs *cp0);   /* HX_CYCLES, below */
static unsigned long long cyc_budget;
extern void (*mipsrec_irq_test_hook)(psxCP0Regs *cp0) __attribute__((weak));

void plat_init(void)
{
	{ struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_sigaction = trap; sa.sa_flags = SA_SIGINFO; sigaction(SIGTRAP, &sa, 0); sigaction(SIGBUS, &sa, 0); }
	if (getenv("HX_CYCLES") && &mipsrec_irq_test_hook) {
		cyc_budget = strtoull(getenv("HX_CYCLES"), 0, 0);
		mipsrec_irq_test_hook = cyc_hook;
	}
	if (getenv("HX_WATCH")) { signal(SIGALRM, watch); alarm(strtoul(getenv("HX_WATCH"), 0, 0)); }
	{	/* qemu-user enforces NX; the JIT code cache (recMemBase) is BSS */
		uintptr_t lo = (uintptr_t)__bss_start & ~4095u, hi = ((uintptr_t)_end + 4095) & ~4095u;
		mprotect((void *)lo, hi - lo, PROT_READ | PROT_WRITE | PROT_EXEC);
	}
	fb = calloc(1, 1024 * 512 * 2);
	menu_fb = calloc(1, 320 * 240 * 2);
	g_menubg_ptr = calloc(1, 320 * 240 * 2);
	g_menuscreen_w = 320; g_menuscreen_h = 240; g_menuscreen_pp = 320;
	if (getenv("HX_FRAMES")) max_frames = strtoul(getenv("HX_FRAMES"), 0, 0);
	if (getenv("HX_MARK")) mark_frame = strtoul(getenv("HX_MARK"), 0, 0);
	if (getenv("HX_FLUSH")) flush_frame = strtoul(getenv("HX_FLUSH"), 0, 0);
	if (getenv("HX_OUT")) out = getenv("HX_OUT");
	if (getenv("HX_OVERDRAW")) { od_on = 1; od_ws = calloc(1, 1024 * 512); od_wg = calloc(1, 1024 * 512);
		od_ls = calloc(2, 1024 * 512); od_lg = calloc(2, 1024 * 512); }
}
void plat_finish(void) {}
void plat_gvideo_open(int is_pal) { (void)is_pal; g_erosq_phase = 1; }
void *plat_gvideo_set_mode(int *w, int *h, int *bpp) { (void)bpp; psx_w = *w; psx_h = *h; return fb; }

static void dump_ppm(const char *path)
{
	FILE *f = fopen(path, "wb");
	if (!f) return;
	fprintf(f, "P6\n%d %d\n255\n", psx_w, psx_h);
	for (int i = 0; i < psx_w * psx_h; i++) {
		uint16_t p = fb[i];
		unsigned char rgb[3] = { (p >> 11) << 3, ((p >> 5) & 63) << 2, (p & 31) << 3 };
		fwrite(rgb, 1, 3, f);
	}
	fclose(f);
}

/* HX_HASH_EVERY=N: every N flips, one line hashing guest RAM + GPRs, the
 * flipped frame, VRAM and psxRegs.cycle/pc -- differential runs (e.g.
 * EROSQ_IDLESKIP off vs on) must print identical lines. No file is written. */
static void hash_line(const char *tag)
{
	uint32_t h = 2166136261u, f = 2166136261u, v = 2166136261u;
	for (int i = 0; i < 0x200000; i++) h = (h ^ psxRegs.ptrs.psxM[i]) * 16777619u;
	for (int i = 0; i < 34; i++) h = (h ^ psxRegs.GPR.r[i]) * 16777619u;
	for (int i = 0; i < psx_w * psx_h; i++) f = (f ^ fb[i]) * 16777619u;
	for (int i = 0; i < 1024 * 512; i++) v = (v ^ gpu.vram[i]) * 16777619u;
	fprintf(stderr, "[hx] %s %lu ram+gpr=%08x frame=%08x vram=%08x cycle=%08x pc=%08x\n",
		tag, frames, h, f, v, psxRegs.cycle, psxRegs.pc);
}

/* HX_CYCLES=N: stop after N emulated cycles (u64, wrap-safe), checked at the
 * dispatcher's event test -- a deterministic end for programs that rarely or
 * never flip (test ROMs on a static screen); prints the same "end" line. */
extern void (*mipsrec_irq_test_hook)(psxCP0Regs *cp0) __attribute__((weak));
void gen_interupt(psxCP0Regs *cp0);
static unsigned long long cyc_budget, cyc_used;
static u32 cyc_last;
static int cyc_started;
static void cyc_hook(psxCP0Regs *cp0)
{
	gen_interupt(cp0);
	if (!cyc_started) { cyc_started = 1; cyc_last = psxRegs.cycle; return; }
	cyc_used += (u32)(psxRegs.cycle - cyc_last);
	cyc_last = psxRegs.cycle;
	if (cyc_used < cyc_budget)
		return;
	hash_line("end");
	if (getenv("HX_VRAMOUT")) {
		FILE *vf = fopen(getenv("HX_VRAMOUT"), "wb");
		if (vf) { fwrite(gpu.vram, 2, 1024 * 512, vf); fclose(vf); }
	}
	if (mipsrec_idleskip_report) mipsrec_idleskip_report();
	fprintf(stderr, "[hx] done after %llu cycles (%lu frames)\n", cyc_used, frames);
	fflush(NULL);
	_exit(0);
}

void *plat_gvideo_flip(void)
{
	char path[512];
	frames++;
	{
		static unsigned long every = (unsigned long)-1;
		if (every == (unsigned long)-1) every = getenv("HX_HASH_EVERY") ? strtoul(getenv("HX_HASH_EVERY"), 0, 0) : 0;
		if (every && frames % every == 0) hash_line("hash");
	}
	if (frames % 60 == 0) fprintf(stderr, "[hx] frame %lu %dx%d\n", frames, psx_w, psx_h);
	if (getenv("HX_SHOT_EVERY") && frames % strtoul(getenv("HX_SHOT_EVERY"), 0, 0) == 0) {
		snprintf(path, sizeof path, "%s.f%06lu.ppm", out, frames); dump_ppm(path);
	}
	if (flush_frame && frames == flush_frame && mipsrec_rec_reset) {
		/* "re-pack after warm-up": full code-cache flush; what runs next is
		 * recompiled contiguously (called from the vblank path, outside any
		 * block, like the cache-full flush in recRecompile) */
		mipsrec_rec_reset();
		fprintf(stderr, "[hx] code cache flushed at frame %lu\n", frames);
	}
	if (getenv("HX_MARK_EVERY") && mipsrec_blkprof_dump && frames >= mark_frame &&
	    frames % strtoul(getenv("HX_MARK_EVERY"), 0, 0) == 0) {   /* per-interval working sets */
		snprintf(path, sizeof path, "%s.m%06lu", out, frames);
		mipsrec_blkprof_dump(path);
	}
	od_frame();
	if (frames == mark_frame && getenv("HX_GPUMIX")) gm_on = 1;
	if (frames == mark_frame && getenv("HX_DCMARK")) {
		/* runtime regions for the dcmodel plugin, then open its window */
		snprintf(path, sizeof path, "%s.rmap", out);
		FILE *m = fopen(path, "w");
		if (m) {
			uintptr_t v[8] = { 0 };
			if (mipsrec_debug_regions) mipsrec_debug_regions(v);
			uintptr_t r = (uintptr_t)&psxRegs;
			fprintf(m, "%x %x psxRegs.GPR\n%x %x psxRegs.CP0\n%x %x psxRegs.CP2D(GTE)\n%x %x psxRegs.CP2C(GTE)\n%x %x psxRegs.rest\n",
				(unsigned)r, (unsigned)(r + 136), (unsigned)(r + 136), (unsigned)(r + 264), (unsigned)(r + 264), (unsigned)(r + 392),
				(unsigned)(r + 392), (unsigned)(r + 520), (unsigned)(r + 520), (unsigned)(r + sizeof psxRegs));
			fprintf(m, "%x %x recRAM(block-ptr-table)\n%x %x recROM(block-ptr-table)\n%x %x JIT-code-cache\n%x %x psxRecLUT\n",
				(unsigned)v[0], (unsigned)(v[0] + v[1]), (unsigned)v[2], (unsigned)(v[2] + v[3]),
				(unsigned)v[4], (unsigned)(v[4] + v[5]), (unsigned)v[6], (unsigned)(v[6] + v[7]));
			fprintf(m, "%x %x psx_ram(+mirrors)\n%x %x psx_scratch+hw\n%x %x psx_bios\n",
				0x0fe00000u, 0x10800000u, 0x1f800000u, 0x1f810000u,
				(unsigned)(uintptr_t)psxRegs.ptrs.psxR, (unsigned)(uintptr_t)psxRegs.ptrs.psxR + 0x80000);
			fprintf(m, "%x %x gpu.vram\n", (unsigned)(uintptr_t)gpu.vram, (unsigned)(uintptr_t)gpu.vram + 1024 * 512 * 2);
			uintptr_t sp = (uintptr_t)__builtin_frame_address(0);
			fprintf(m, "%x %x stack(main)\n", (unsigned)(sp - 0x100000), (unsigned)(sp + 0x10000));
			fclose(m);
		}
		{	/* guest mappings, to place heap/libc addresses */
			snprintf(path, sizeof path, "%s.maps", out);
			FILE *in = fopen("/proc/self/maps", "r"), *o = fopen(path, "w");
			char b[512];
			while (in && o && fgets(b, sizeof b, in)) fputs(b, o);
			if (in) fclose(in);
			if (o) fclose(o);
		}
		js_print("mark");
		syscall(SYS_getppid);
	}
	if (mark_frame && frames == mark_frame && mipsrec_blkprof_dump) {
		snprintf(path, sizeof path, "%s.blk0", out);
		mipsrec_blkprof_dump(path);
	}
	if (frames >= max_frames) {
		if (getenv("HX_DCMARK")) syscall(SYS_getppid);   /* close the dcmodel window */
		js_print("end");
		if (!getenv("HX_NODUMP")) { snprintf(path, sizeof path, "%s.ppm", out); dump_ppm(path); }
		if (mipsrec_blkprof_dump) { snprintf(path, sizeof path, "%s.blk", out); mipsrec_blkprof_dump(path); }
		if (mipsrec_emitstat_dump && getenv("EROSQ_EMITSTAT")) mipsrec_emitstat_dump();
		if (gm_on) gm_dump(frames - mark_frame);
		{	/* end-state digest: differential runs (e.g. EROSQ_REPACK / BLOCKLINK
			 * modes) must match exactly, emulation is deterministic here */
			uint32_t h = 2166136261u;
			for (int i = 0; i < 0x200000; i++) h = (h ^ psxRegs.ptrs.psxM[i]) * 16777619u;
			for (int i = 0; i < 34; i++) h = (h ^ psxRegs.GPR.r[i]) * 16777619u;
			fprintf(stderr, "[hx] state ram+gpr=%08x cycle=%08x pc=%08x\n", h, psxRegs.cycle, psxRegs.pc);
			if (!getenv("HX_NODUMP")) {   /* HX_NODUMP=1: no RAM/frame files (disc-derived data) */
				snprintf(path, sizeof path, "%s.ram", out);   /* guest code for gt2_an.py */
				FILE *rf = fopen(path, "wb");
				if (rf) { fwrite(psxRegs.ptrs.psxM, 1, 0x200000, rf); fclose(rf); }
			}
			hash_line("end");
			if (getenv("HX_VRAMOUT")) {   /* raw 1024x512x16 VRAM (test-ROM runs: vs reference vram.png) */
				FILE *vf = fopen(getenv("HX_VRAMOUT"), "wb");
				if (vf) { fwrite(gpu.vram, 2, 1024 * 512, vf); fclose(vf); }
			}
			if (mipsrec_idleskip_report) mipsrec_idleskip_report();
		}
		fprintf(stderr, "[hx] done after %lu frames\n", frames);
		fflush(NULL);
		_exit(0);
	}
	return fb;
}
void plat_gvideo_close(void) {}
void plat_video_menu_enter(int is_rom_loaded) { (void)is_rom_loaded; g_erosq_phase = 2; pl_vout_buf = fb; }
void plat_video_menu_begin(void) { g_menuscreen_ptr = menu_fb; }
void plat_video_menu_end(void) { g_menuscreen_ptr = NULL; }
void plat_video_menu_leave(void) { g_erosq_phase = 3; }
void *plat_prepare_screenshot(int *w, int *h, int *bpp) { *w = psx_w; *h = psx_h; *bpp = 16; return fb; }
