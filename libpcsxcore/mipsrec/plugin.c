#include "../r3000a.h"
#include "../psxmem.h"
#include "../psxcounters.h"
#include "../sio.h"
#include "../gte.h"

/* Phase 0: delegate everything to the interpreter to prove the socket.
 * Replaced piecewise by the real recompiler in later phases. */
extern R3000Acpu psxInt;

/* This build defines neither DRC_DISABLE nor LIGHTREC, so r3000a.c and
 * psxcounters.c do NOT define these globals (they only do under DRC_DISABLE).
 * We own them here, exactly as libpcsxcore/lightrec/plugin.c does
 * (plugin.c:68-69). The new_dynarec_* stubs and ndrc_g come from
 * new_dynarec/emu_if.c's non-ari64 branch, which we opt into by extending its
 * guards with !defined(MIPSREC) (same treatment LIGHTREC gets). */
psxRegisters psxRegs;
Rcnt rcnts[4];

static int  mipsrec_init(void)                                   { return psxInt.Init(); }
static void mipsrec_reset(void)                                  { psxInt.Reset(); }
static void mipsrec_execute(struct psxRegisters *regs)           { psxInt.Execute(regs); }
static void mipsrec_execute_block(struct psxRegisters *regs, enum blockExecCaller c)
                                                                 { psxInt.ExecuteBlock(regs, c); }
static void mipsrec_clear(u32 addr, u32 size)                    { psxInt.Clear(addr, size); }
static void mipsrec_notify(enum R3000Anote note, void *data)     { psxInt.Notify(note, data); }
static void mipsrec_apply_config(void)                           { psxInt.ApplyConfig(); }
static void mipsrec_shutdown(void)                               { psxInt.Shutdown(); }

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
 * (See the per-op lm split documented in the comment block above.) */
#define GTE_SHIM0_LM1(NAME, OPVAL) \
	void gte##NAME(void) { gteDispatch(&psxRegs.CP2, (u32)(OPVAL) | GTE_SF_BIT | GTE_LM_BIT); }

#define GTE_SHIM0_LM0(NAME, OPVAL) \
	void gte##NAME(void) { gteDispatch(&psxRegs.CP2, (u32)(OPVAL) | GTE_SF_BIT); }

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
