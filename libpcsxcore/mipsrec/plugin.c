#include "../r3000a.h"
#include "../psxmem.h"
#include "../psxcounters.h"
#include "../psxevents.h"     /* gen_interupt, schedule_timeslice */
#include "../psxinterpreter.h"/* psxExecuteOpcode (single-op interp helper) */
#include "../sio.h"
#include "../gte.h"

/* This build defines neither DRC_DISABLE nor LIGHTREC, so r3000a.c and
 * psxcounters.c do NOT define these globals (they only do under DRC_DISABLE).
 * We own them here, exactly as libpcsxcore/lightrec/plugin.c does
 * (plugin.c:68-69). The new_dynarec_* stubs and ndrc_g come from
 * new_dynarec/emu_if.c's non-ari64 branch, which we opt into by extending its
 * guards with !defined(MIPSREC) (same treatment LIGHTREC gets).
 *
 * IMPORTANT: this is THE one-and-only psxRegs. recompiler.cpp's dispatch
 * loops set $fp/$s8 (PERM_REG_1) to &psxRegs of THIS object via their
 * `[psxRegs] "i" (&psxRegs)` inline-asm operands, and every mipsrec offset
 * macro (off()/offGPR/…) is relative to it — so the recompiled code, the
 * memory seam, the GTE shims and the scheduler all operate on this struct. */
psxRegisters psxRegs;
Rcnt rcnts[4];

/* C-linkage entry points into the recompiler (defined extern "C" at the
 * bottom of recompiler.cpp; also documented in compat.h, which this TU does
 * NOT include). */
int  mipsrec_rec_init(void);
void mipsrec_rec_reset(void);
void mipsrec_rec_execute(void);
void mipsrec_rec_execute_block(unsigned pc);
void mipsrec_rec_clear(u32 addr, u32 size);
void mipsrec_rec_notify(int note, void *data);
void mipsrec_rec_shutdown(void);

/* Phase 3 (GO LIVE) — the cycle seam. mipsrec's dispatch loops jal here when
 * psxRegs.cycle >= psxRegs.next_interupt. gen_interupt() is rearmed's
 * canonical horizon service (irq_test + schedule_timeslice), identical to
 * what ari64 (emu_if.c pcsx_mtc0) and lightrec (lightrec/plugin.c) call at the
 * same seam. It (a) dispatches every due event and raises any pending IRQ via
 * irq_test(), then (b) recomputes psxRegs.next_interupt = cycle + min (min >= 1
 * always, since schedule_timeslice only counts strictly-future events and
 * floors at PSXCLK) — i.e. STRICTLY ahead of cycle, guaranteeing the loop
 * makes forward progress. Deliberately NOT rearmed's psxBranchTest()
 * (r3000a.c), which omits schedule_timeslice and would leave next_interupt
 * stale == cycle → infinite spin at boot. See compat.h Seam 2b. */
void mipsrec_branch_test(void)
{
	gen_interupt(&psxRegs.CP0);
}

static int  mipsrec_init(void)  { return mipsrec_rec_init(); }
static void mipsrec_reset(void) { mipsrec_rec_reset(); }

static void mipsrec_execute(struct psxRegisters *regs)
{
	/* Prime next_interupt before entering the (single, infinite) dispatch
	 * loop, mirroring ari64_execute()'s per-timeslice schedule_timeslice().
	 * The loop re-primes via mipsrec_branch_test()->gen_interupt() on every
	 * subsequent horizon hit; this just bounds the very first timeslice so a
	 * stale-high next_interupt can't delay the first IRQ service. */
	schedule_timeslice(&psxRegs);
	mipsrec_rec_execute();
}

static void mipsrec_execute_block(struct psxRegisters *regs, enum blockExecCaller c)
{
	/* recExecuteBlock(target) runs the internal dispatch loop until pc == target
	 * (its run-to-a-return-address primitive, used for HLE softcalls). rearmed's
	 * real-BIOS psxExecuteBios (r3000a.c) instead loops ExecuteBlock and breaks
	 * when psxExecuteBiosEnded() (pc enters 0x8000_0000 space) — it expects the
	 * call to return at the BIOS->shell handoff. Passing regs->pc as the target
	 * makes it "run until pc loops back to the entry", which never happens at the
	 * handoff, so the whole game ran INSIDE this call and psxReset() never
	 * returned — leaving the frontend's SysReset() with GPU_vBlank swapped to its
	 * no-op dummy (no frame ever presented -> black screen, though CPU+SPU ran).
	 * The retail BIOS always hands off to the shell at exactly 0x80030000 (which
	 * psxReset itself checks for), and boot stays in ROM until then, so target
	 * that for the boot caller.
	 *
	 * Non-boot callers (HLE softCall()/softCallInException() in psxbios.c, the
	 * GPU debug frame-step in menu.c) expect "run one short unit, return" and
	 * poll their own exit condition between calls -- softCall's is the
	 * 0x80001000 ra-sentinel OR returned_from_exception(), a multi-value exit
	 * recExecuteBlock's single run-to-target-pc primitive cannot express (and
	 * it has no .stop guard, so a missed target = hard hang). Delegate those
	 * to the interpreter like the callers assume: correctness over speed on a
	 * path that runs a handful of blocks per callback. intApplyConfig() (see
	 * mipsrec_apply_config) has set up regs->ptrs.intFetch before any of this
	 * can run. The BEFORE_SAVE notify flushes the interpreter's load-delay
	 * pipeline (dloadReg) so no delayed load is dropped when recompiled code
	 * resumes. */
	if (c == EXEC_CALLER_BOOT) {
		mipsrec_rec_execute_block(0x80030000u);
	} else {
		psxInt.ExecuteBlock(regs, c);
		psxInt.Notify(R3000ACPU_NOTIFY_BEFORE_SAVE, NULL);
	}
}

static void mipsrec_clear(u32 addr, u32 size) { mipsrec_rec_clear(addr, size); }

/* Notify enum translation. rearmed's enum R3000Anote (r3000a.h) is:
 *   CACHE_ISOLATED=0, CACHE_UNISOLATED=1, BEFORE_SAVE=2, AFTER_LOAD=3,
 *   AFTER_LOAD_STATE=4.
 * recNotify() (recompiler.cpp) only understands CACHE_ISOLATED/UNISOLATED
 * (same 0/1 values — compiled against this very enum) plus its own
 * pcsx4all-only DMA3_EXE_LOAD (compat.h aliases it to 100, never sent by
 * rearmed). The save/load notifies have no pcsx4all analogue, so we handle
 * them here: a state load replaces PS1 RAM wholesale, invalidating every
 * recompiled block, so flush the whole code cache (recReset). BEFORE_SAVE is
 * a no-op — mipsrec keeps no cached CPU state outside psxRegs to sync. */
static void mipsrec_notify(enum R3000Anote note, void *data)
{
	switch (note) {
	case R3000ACPU_NOTIFY_CACHE_ISOLATED:
	case R3000ACPU_NOTIFY_CACHE_UNISOLATED:
		mipsrec_rec_notify((int)note, data);
		break;
	case R3000ACPU_NOTIFY_AFTER_LOAD:
	case R3000ACPU_NOTIFY_AFTER_LOAD_STATE:
		/* data == NULL: full RAM/state reload (savestate, disc/BIOS load) --
		 * every recompiled block is stale, flush. data != NULL: the cheap
		 * per-softcall HLE sync from psxbios.c; mipsrec keeps no cached CPU
		 * state outside psxRegs, so nothing to do (a full flush here would
		 * re-JIT the world on every BIOS callback). Matches lightrec. */
		if (data == NULL)
			mipsrec_rec_reset();
		break;
	case R3000ACPU_NOTIFY_BEFORE_SAVE:
	default:
		break;
	}
}

/* recompiler.cpp's per-block cycle charge, 24.8 fixed point (0x100 = 1.00
 * cycles/instruction). Baked into blocks at recompile time by ADJUST_CLOCK
 * (mips_codegen.h), so changing it requires a code-cache flush. */
extern u32 cycle_multiplier;

static void mipsrec_apply_config(void)
{
	/* Same source resolution as lightrec_plugin_apply_config(): the per-game
	 * database override wins unless the user moved the CPU-clock% slider off
	 * the default. Config values are percent (175 = 1.75). */
	u32 mult_percent =
		(Config.cycle_multiplier_override &&
		 Config.cycle_multiplier == CYCLE_MULT_DEFAULT)
			? (u32)Config.cycle_multiplier_override
			: (u32)Config.cycle_multiplier;
	u32 mult_fp = (mult_percent << 8) / 100;

	if (mult_fp && mult_fp != cycle_multiplier) {
		cycle_multiplier = mult_fp;
		mipsrec_rec_reset();  /* blocks bake the old multiplier */
	}

	/* Keep the interpreter's dispatch tables/fetch pointers in sync, exactly
	 * as lightrec does: mipsrec_execute_block() delegates HLE softcalls and
	 * debug stepping to psxInt, which needs regs->ptrs.intFetch set. */
	intApplyConfig();
}
static void mipsrec_shutdown(void)     { mipsrec_rec_shutdown(); }

R3000Acpu psxRec = {
	mipsrec_init, mipsrec_reset, mipsrec_execute, mipsrec_execute_block,
	mipsrec_clear, mipsrec_notify, mipsrec_apply_config, mipsrec_shutdown,
};

/* ======================================================================
 * Task 2.1 — indirect memory path + GTE dispatch shims
 *
 * plugin.c is compiled as plain C (unmangled symbols). recompiler.cpp (and
 * everything it `#include`s, including this file's declarations as seen via
 * compat.h) is compiled as C++. Every symbol defined here that the
 * recompiler TU references must resolve to the SAME unmangled name at link
 * time — guaranteed because compat.h predeclares each one inside an
 * `extern "C" { ... }` block before the vendored code gets a chance to
 * (re)declare them without a linkage spec. See compat.h's Seam 1/1b/GTE
 * comments for the full per-symbol rationale.
 * ====================================================================== */

/* Seam 1 — memory. compat.h #defines psxMemRead8/16/32 and
 * psxMemWrite8/16/32 (the names the vendored mipsrec code JALs to, per
 * pcsx4all's (addr[, val]) call signature) to these mipsrec_* names. Bodies
 * just forward to rearmed's real (regs, addr[, val])-shaped functions;
 * `psxMemRead*`/`psxMemWrite*` below refer to the genuine rearmed
 * declarations from psxmem.h since this TU does NOT include compat.h. */
int mipsrec_writeok = 1;

u32  mipsrec_read8 (u32 addr)          { return psxMemRead8 (&psxRegs, addr); }
u32  mipsrec_read16(u32 addr)          { return psxMemRead16(&psxRegs, addr); }
u32  mipsrec_read32(u32 addr)          { return psxMemRead32(&psxRegs, addr); }
void mipsrec_write8 (u32 addr, u32 v)  { psxMemWrite8 (&psxRegs, addr, v); }
void mipsrec_write16(u32 addr, u32 v)  { psxMemWrite16(&psxRegs, addr, v); }
void mipsrec_write32(u32 addr, u32 v)  { psxMemWrite32(&psxRegs, addr, v); }

/* Seam 1b — the 5 pcsx4all-shaped HW/sio/counter call-outs rearmed has no
 * same-shaped counterpart for (every other one rec_lsu_hw.cpp.h JALs to
 * already exists verbatim via psxhw.h/sio.h/psxcounters.h/cdrom.h/mdec.h/
 * plugins.h — see compat.h's Seam 1b comment for the full enumeration). */
u32 psxRcntRcount(u32 index)
{
	switch (index) {
	case 0:  return psxRcntRcount0();
	case 1:  return psxRcntRcount1();
	default: return psxRcntRcount2();
	}
}

/* JOY_DATA (0x1f801040) accessed at 16/32-bit widths. Reproduced verbatim
 * from pcsx4all/src/sio.cpp:279-365: byte-at-a-time wrappers around
 * sioRead8/sioWrite8, which do the real FIFO-advancing work and already
 * exist unchanged in rearmed's sio.c. */
u16 sioRead16(void)
{
	u16 v = sioRead8();
	v |= (u16)sioRead8() << 8;
	return v;
}
u32 sioRead32(void)
{
	u32 v = sioRead8();
	v |= (u32)sioRead8() << 8;
	v |= (u32)sioRead8() << 16;
	v |= (u32)sioRead8() << 24;
	return v;
}
void sioWrite16(u16 value)
{
	sioWrite8(value & 0xff);
	sioWrite8((value >> 8) & 0xff);
}
void sioWrite32(u32 value)
{
	sioWrite8(value & 0xff);
	sioWrite8((value >> 8) & 0xff);
	sioWrite8((value >> 16) & 0xff);
	sioWrite8((value >> 24) & 0xff);
}

/* GTE — rearmed's single shared entry point. gteDispatch(regs, code) decodes
 * the real GTE op from `code & 0x3f` and its sf/mx/v/cv/lm modifier bits
 * from their native positions in `code` (bit19/17-18/15-16/13/10 — see
 * libpcsxcore/gte.c gteGetHandler()). Neither of mipsrec's two call shapes
 * hands us that full-width `code`, so each shim reconstructs just enough of
 * it, using the specific enum gteop_opcodes value baked in per shim (each
 * named function only ever stands for ONE op) OR-ed with the modifier bits:
 *
 * CP2_FUNC_0 (no argument at all — see compat.h): pcsx4all's OWN
 * `gteXXX(void)` implementations (gbemu/pcsx4all/src/gte.cpp) never read the
 * live opcode/globals for these ops — sf and lm are baked into the source as
 * literals. The >>12 shift is unconditional in all of them (equivalent to
 * sf=1). But lm is NOT uniform, contrary to this shim's original claim
 * (corrected per Task 2.1 code review — see the Fix section of
 * task-2.1-report.md): the LITERAL passed to the FINAL limB1/2/3(val, lm)
 * clamp of gteIR1/2/3 splits into two groups:
 *   - lm = 1 (11 ops): NCDS, NCDT, CDP, NCCS, CC, NCS, NCT, NCCT (final
 *     clamp passes literal 1, re-verified at gte.cpp:634/682/958/554/907/
 *     846/887/594) + NCLIP, AVSZ3, AVSZ4 (no gteIR clamp at all → lm inert,
 *     value doesn't matter).
 *   - lm = 0 (3 ops): RTPS (gte.cpp:410-412), RTPT (:450-452), DPCT
 *     (:826-828) — final gteIR1/2/3 clamp passes literal 0. This is NOT
 *     inert in rearmed: libpcsxcore/gte.c's RTPS/RTPT/DPCT clamp bound is
 *     `-0x8000 * !lm`, so forcing lm=1 would clamp legitimate negative
 *     gteIR1/gteIR2 (screen-space X/Y from RTPS/RTPT, run per-vertex in
 *     essentially every 3D title) to 0 and misplace geometry.
 * These sf/lm literals are a pre-existing pcsx4all convention, not something
 * introduced by this port — reproduced faithfully by baking the matching
 * sf/lm bits into the synthetic code word handed to gteDispatch so rearmed's
 * corresponding handler variant runs. (psxRegs.code is NOT read here: it is
 * only refreshed by the interpreter's per-instruction fetch/decode, not by
 * the recompiled dispatch loop, so it would not reliably hold the
 * currently-executing COP2 instruction — moot for this group anyway, since
 * every needed sf/lm bit is known statically per op.)
 *
 * CP2_FUNC_1 (one argument): rec_gte.cpp.h's CP2_FUNC_1 macro bakes
 * `LI16(A0, (u16)(psxRegs.code >> 10))` into the JIT'd code at *recompile*
 * time (`psxRegs.code` there is the real, currently-being-translated PS1
 * opcode word — valid at that point in the emitter, unlike at JIT runtime).
 * That constant preserves code's bits 10-25 (sf@19, mx@17-18, v@15-16,
 * cv@13, lm@10) shifted down to bits 0-15, losing only the low 10 bits —
 * which is exactly the op/function field, known statically here from the
 * enum value. `(gteop & 0xffff) << 10` puts those modifier bits back at
 * their original positions. */
#define GTE_SF_BIT (1u << 19)
#define GTE_LM_BIT (1u << 10)

/* GTE_SHIM0_LM1: no-arg op with sf=1, lm=1.  GTE_SHIM0_LM0: sf=1, lm=0.
 * (See the per-op lm split documented in the comment block above.)
 *
 * The no-arg shims have a compile-time-constant `code`, so gteGetHandler(code)
 * is a constant — resolve it ONCE and call the handler directly, exactly as
 * pcsx4all JALs straight to the op. This drops the gteDispatch() call frame
 * and gteGetHandler's per-call op/modifier recompute from the hottest GTE ops
 * (RTPS/RTPT/NCLIP/NCDS run per-vertex/per-poly in every 3D title). Behaviour
 * is identical: same handler, same `code` argument. Single-threaded, so the
 * static cache needs no guard. One-arg ops (MVMVA etc.) keep gteDispatch — the
 * modifier bits come from the runtime arg, so the handler isn't constant.
 *
 * NB: measured negligible (<0.5%, below scene noise even at ~130k GTE ops/s on
 * Crash gameplay) — GTE dispatch was never the pcsx4all gap; that was GPU fill
 * + uncached present (see the erosq perf commit). Kept because it is correct,
 * general, and strictly leaner than routing every op through gteDispatch. */
#define GTE_SHIM0_LM1(NAME, OPVAL) \
	void gte##NAME(void) { \
		static gte_handler *h; \
		const u32 code = (u32)(OPVAL) | GTE_SF_BIT | GTE_LM_BIT; \
		if (!h) h = gteGetHandler(code); \
		h(&psxRegs.CP2, code); \
	}

#define GTE_SHIM0_LM0(NAME, OPVAL) \
	void gte##NAME(void) { \
		static gte_handler *h; \
		const u32 code = (u32)(OPVAL) | GTE_SF_BIT; \
		if (!h) h = gteGetHandler(code); \
		h(&psxRegs.CP2, code); \
	}

#define GTE_SHIM1(NAME, OPVAL) \
	void gte##NAME(u32 gteop) { gteDispatch(&psxRegs.CP2, (u32)(OPVAL) | ((gteop & 0xffffu) << 10)); }

GTE_SHIM0_LM0(RTPS,  GTEOP_RTPS)    /* lm=0: gte.cpp:410-412 */
GTE_SHIM0_LM1(NCLIP, GTEOP_NCLIP)   /* lm inert (no gteIR clamp) */
GTE_SHIM0_LM1(NCDS,  GTEOP_NCDS)
GTE_SHIM0_LM1(NCDT,  GTEOP_NCDT)
GTE_SHIM0_LM1(CDP,   GTEOP_CDP)
GTE_SHIM0_LM1(NCCS,  GTEOP_NCCS)
GTE_SHIM0_LM1(CC,    GTEOP_CC)
GTE_SHIM0_LM1(NCS,   GTEOP_NCS)
GTE_SHIM0_LM1(NCT,   GTEOP_NCT)
GTE_SHIM0_LM0(DPCT,  GTEOP_DPCT)    /* lm=0: gte.cpp:826-828 */
GTE_SHIM0_LM1(AVSZ3, GTEOP_AVSZ3)   /* lm inert (no gteIR clamp) */
GTE_SHIM0_LM1(AVSZ4, GTEOP_AVSZ4)   /* lm inert (no gteIR clamp) */
GTE_SHIM0_LM0(RTPT,  GTEOP_RTPT)    /* lm=0: gte.cpp:450-452 */
GTE_SHIM0_LM1(NCCT,  GTEOP_NCCT)

GTE_SHIM1(OP,    GTEOP_OP)
GTE_SHIM1(DPCS,  GTEOP_DPCS)
GTE_SHIM1(INTPL, GTEOP_INTPL)
GTE_SHIM1(MVMVA, GTEOP_MVMVA)
GTE_SHIM1(SQR,   GTEOP_SQR)
GTE_SHIM1(DCPL,  GTEOP_DCPL)
GTE_SHIM1(GPF,   GTEOP_GPF)
GTE_SHIM1(GPL,   GTEOP_GPL)

#undef GTE_SHIM0_LM1
#undef GTE_SHIM0_LM0
#undef GTE_SHIM1

/* ======================================================================
 * Seam 5 (Phase 3 GO LIVE) — load-delay-hazard helpers.
 *
 * rec_bcu.cpp.h's branch/jump emitters reference two pcsx4all-only symbols
 * that were undefined (but --gc-section-hidden) until dispatch went live:
 *
 *  (1) psxTestLoadDelay(reg, tmp): PURE opcode analysis — given a load's
 *      target GPR `reg` and the following opcode word `tmp`, classify the
 *      load-delay interaction (0=none, 1=read+write, 2=read, 3=write). No
 *      machine state involved, so it is reproduced here VERBATIM from
 *      pcsx4all/src/psxinterpreter.cpp:128-282 (a bit-for-bit copy, incl. its
 *      Xenogears no-load-delay quirks for REGIMM/BEQ/BNE/BLEZ/BGTZ). Used at
 *      recompile time in DelayTest()/iJumpAL()/recBxx()/…, so it MUST be
 *      exact.
 *
 *  (2) psxBSC[64] (no-arg): pcsx4all's classic per-primary-opcode interpreter
 *      dispatch table. rec_bcu.cpp.h JALs it ONLY inside execBranchLoadDelay()
 *      (the "load in a JR/branch delay slot" fixup, e.g. Skullmonkeys / Tomb
 *      Raider), where it executes ONE opcode held in psxRegs.code. rearmed's
 *      own interpreter table is file-static, has a (regs,code) signature, and
 *      is not even populated in the mipsrec build (psxInit() runs only
 *      psxRec.Init, never intInit). So we route all 64 no-arg slots through a
 *      single trampoline into rearmed's real interpreter via the newly
 *      exposed psxExecuteOpcode(regs, code) helper (psxinterpreter.c), which
 *      dispatches the opcode and force-flushes rearmed's deferred load so the
 *      register effect is immediate (mipsrec, like pcsx4all, manages its own
 *      GPRs and never touches rearmed's dloadReg pipeline). See the
 *      "unverified" list in the report for the residual-risk corner (a taken
 *      branch appearing as the code2 operand of a chained delay slot).
 * ====================================================================== */

/* pcsx4all field-extractors, scoped to psxTestLoadDelay (operate on `tmp`). */
#define _tFunct_  ((tmp      ) & 0x3F)
#define _tRd_     ((tmp >> 11) & 0x1F)
#define _tRt_     ((tmp >> 16) & 0x1F)
#define _tRs_     ((tmp >> 21) & 0x1F)

int psxTestLoadDelay(int reg, u32 tmp)
{
	if (tmp == 0) return 0; // NOP
	switch (tmp >> 26) {
		case 0x00: // SPECIAL
			switch (_tFunct_) {
				case 0x00: // SLL
				case 0x02: case 0x03: // SRL/SRA
					if (_tRd_ == reg && _tRt_ == reg) return 1; else
					if (_tRt_ == reg) return 2; else
					if (_tRd_ == reg) return 3;
					break;

				case 0x08: // JR
					if (_tRs_ == reg) return 2;
					break;
				case 0x09: // JALR
					if (_tRd_ == reg && _tRs_ == reg) return 1; else
					if (_tRs_ == reg) return 2; else
					if (_tRd_ == reg) return 3;
					break;

				// SYSCALL/BREAK just a break;

				case 0x20: case 0x21: case 0x22: case 0x23:
				case 0x24: case 0x25: case 0x26: case 0x27:
				case 0x2a: case 0x2b: // ADD/ADDU...
				case 0x04: case 0x06: case 0x07: // SLLV...
					if (_tRd_ == reg && (_tRt_ == reg || _tRs_ == reg)) return 1; else
					if (_tRt_ == reg || _tRs_ == reg) return 2; else
					if (_tRd_ == reg) return 3;
					break;

				case 0x10: case 0x12: // MFHI/MFLO
					if (_tRd_ == reg) return 3;
					break;
				case 0x11: case 0x13: // MTHI/MTLO
					if (_tRs_ == reg) return 2;
					break;

				case 0x18: case 0x19:
				case 0x1a: case 0x1b: // MULT/DIV...
					if (_tRt_ == reg || _tRs_ == reg) return 2;
					break;
			}
			break;

		case 0x01: // REGIMM
			switch (_tRt_) {
				case 0x00: case 0x01:
				case 0x10: case 0x11: // BLTZ/BGEZ...
					// Xenogears - lbu v0 / beq v0
					// - no load delay (fixes battle loading)
					break;
			}
			break;

		// J would be just a break;
		case 0x03: // JAL
			if (31 == reg) return 3;
			break;

		case 0x04: case 0x05: // BEQ/BNE
			// Xenogears - lbu v0 / beq v0
			// - no load delay (fixes battle loading)
			break;

		case 0x06: case 0x07: // BLEZ/BGTZ
			// Xenogears - lbu v0 / beq v0
			// - no load delay (fixes battle loading)
			break;

		case 0x08: case 0x09: case 0x0a: case 0x0b:
		case 0x0c: case 0x0d: case 0x0e: // ADDI/ADDIU...
			if (_tRt_ == reg && _tRs_ == reg) return 1; else
			if (_tRs_ == reg) return 2; else
			if (_tRt_ == reg) return 3;
			break;

		case 0x0f: // LUI
			if (_tRt_ == reg) return 3;
			break;

		case 0x10: // COP0
			switch (_tFunct_) {
				case 0x00: // MFC0
					if (_tRt_ == reg) return 3;
					break;
				case 0x02: // CFC0
					if (_tRt_ == reg) return 3;
					break;
				case 0x04: // MTC0
					if (_tRt_ == reg) return 2;
					break;
				case 0x06: // CTC0
					if (_tRt_ == reg) return 2;
					break;
				// RFE just a break;
			}
			break;

		case 0x12: // COP2
			switch (_tFunct_) {
				case 0x00:
					switch (_tRs_) {
						case 0x00: // MFC2
							if (_tRt_ == reg) return 3;
							break;
						case 0x02: // CFC2
							if (_tRt_ == reg) return 3;
							break;
						case 0x04: // MTC2
							if (_tRt_ == reg) return 2;
							break;
						case 0x06: // CTC2
							if (_tRt_ == reg) return 2;
							break;
					}
					break;
				// RTPS... break;
			}
			break;

		case 0x22: case 0x26: // LWL/LWR
			if (_tRt_ == reg) return 3; else
			if (_tRs_ == reg) return 2;
			break;

		case 0x20: case 0x21: case 0x23:
		case 0x24: case 0x25: // LB/LH/LW/LBU/LHU
			if (_tRt_ == reg && _tRs_ == reg) return 1; else
			if (_tRs_ == reg) return 2; else
			if (_tRt_ == reg) return 3;
			break;

		case 0x28: case 0x29: case 0x2a:
		case 0x2b: case 0x2e: // SB/SH/SWL/SW/SWR
			if (_tRt_ == reg || _tRs_ == reg) return 2;
			break;

		case 0x32: case 0x3a: // LWC2/SWC2
			if (_tRs_ == reg) return 2;
			break;
	}

	return 0;
}

#undef _tFunct_
#undef _tRd_
#undef _tRt_
#undef _tRs_

/* No-arg interpreter dispatch table. Every slot points at one trampoline that
 * hands the opcode already loaded in psxRegs.code to rearmed's interpreter
 * (immediate load effect). Referenced by rec_bcu.cpp.h as
 * `extern void (*psxBSC[64])(void)`. */
static void mipsrec_interp_op(void)
{
	psxExecuteOpcode(&psxRegs, psxRegs.code);
}

void (*psxBSC[64])(void) = {
	mipsrec_interp_op, mipsrec_interp_op, mipsrec_interp_op, mipsrec_interp_op,
	mipsrec_interp_op, mipsrec_interp_op, mipsrec_interp_op, mipsrec_interp_op,
	mipsrec_interp_op, mipsrec_interp_op, mipsrec_interp_op, mipsrec_interp_op,
	mipsrec_interp_op, mipsrec_interp_op, mipsrec_interp_op, mipsrec_interp_op,
	mipsrec_interp_op, mipsrec_interp_op, mipsrec_interp_op, mipsrec_interp_op,
	mipsrec_interp_op, mipsrec_interp_op, mipsrec_interp_op, mipsrec_interp_op,
	mipsrec_interp_op, mipsrec_interp_op, mipsrec_interp_op, mipsrec_interp_op,
	mipsrec_interp_op, mipsrec_interp_op, mipsrec_interp_op, mipsrec_interp_op,
	mipsrec_interp_op, mipsrec_interp_op, mipsrec_interp_op, mipsrec_interp_op,
	mipsrec_interp_op, mipsrec_interp_op, mipsrec_interp_op, mipsrec_interp_op,
	mipsrec_interp_op, mipsrec_interp_op, mipsrec_interp_op, mipsrec_interp_op,
	mipsrec_interp_op, mipsrec_interp_op, mipsrec_interp_op, mipsrec_interp_op,
	mipsrec_interp_op, mipsrec_interp_op, mipsrec_interp_op, mipsrec_interp_op,
	mipsrec_interp_op, mipsrec_interp_op, mipsrec_interp_op, mipsrec_interp_op,
	mipsrec_interp_op, mipsrec_interp_op, mipsrec_interp_op, mipsrec_interp_op,
	mipsrec_interp_op, mipsrec_interp_op, mipsrec_interp_op, mipsrec_interp_op,
};
