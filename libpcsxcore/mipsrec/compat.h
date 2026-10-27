/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 erosqcfw */
#ifndef MIPSREC_COMPAT_H
#define MIPSREC_COMPAT_H
/* Bridges pcsx4all mipsrec expectations onto rearmed's core.
 * mipsrec's offGPR/offCP0/offCP2C/off(pc|code|cycle) already match rearmed's
 * psxRegisters byte layout — only the post-`interrupt` fields differ.
 *
 * Included at the TOP of recompiler.cpp, before any other mipsrec header.
 * mips_codegen.cpp includes it too (it also needs psxRegisters/r3000a.h).
 */

#include <cstdio>         /* printf() — the vendored files assume a C TU where
                             <stdio.h> has already been dragged in transitively;
                             recompiler.cpp/mips_codegen.cpp are C++ TUs, so
                             pull it in explicitly. */

#include "../r3000a.h"    /* rearmed psxRegisters, psxRegs, psxException, psxBranchTest proto */
#include "../psxmem.h"
#include "../psxhw.h"
#include "../psxcounters.h"
#include "../psxhle.h"
#include "../gte.h"
#include "../mdec.h"
#include "../cdrom.h"
#include "../sio.h"
#include "../misc.h"      /* CdromId */
/* NOTE: GPU_writeData/GPU_readData/SPU_writeRegister/SPU_readRegister come in
 * transitively via psxcounters.h -> plugins.h -> gpu.h/spu.h. Not included
 * directly here: libpcsxcore/gpu.h's lone prototype (gpu_state_change) has no
 * extern "C" guard of its own and relies on being nested inside plugins.h's
 * extern "C" block; a direct top-level #include here would give it C++
 * linkage and mismatch at link time against its C-compiled definition. */
#include "../../frontend/plugin_lib.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Seam 1 — memory helpers. rearmed's take (regs, addr[, val]); mipsrec emits
 * calls to the pcsx4all (addr[, val]) signatures. Provide adapters with the
 * pcsx4all names/signatures; plugin.c defines them (Task 2). */
u32  mipsrec_read8 (u32 addr);
u32  mipsrec_read16(u32 addr);
u32  mipsrec_read32(u32 addr);
void mipsrec_write8 (u32 addr, u32 val);
void mipsrec_write16(u32 addr, u32 val);
void mipsrec_write32(u32 addr, u32 val);
#define psxMemRead8   mipsrec_read8
#define psxMemRead16  mipsrec_read16
#define psxMemRead32  mipsrec_read32
#define psxMemWrite8  mipsrec_write8
#define psxMemWrite16 mipsrec_write16
#define psxMemWrite32 mipsrec_write32

/* Seam 1b — pcsx4all-shaped HW/counter entry points that rearmed splits or
 * omits. rec_lsu_hw.cpp.h and rec_gte.cpp.h JAL these by pcsx4all's original
 * (undivided) names/signatures (see pcsx4all/src/psxcounters.h, sio.h).
 * rearmed only has psxRcntRcount0/1/2 (no generic indexed reader) and no
 * 16/32-bit sio read/write helpers at all. Declared here so recompiler.o
 * compiles; plugin.c supplies the bodies (Task 2.1). Every OTHER pcsx4all
 * HW/sio/counter call-out referenced by rec_lsu_hw.cpp.h already has a
 * same-name, same-signature rearmed counterpart pulled in transitively by
 * the includes above — no shim needed: psxHwRead8/16/32, psxHwWrite8/16/32
 * (psxhw.h), sioRead8/sioWrite8 + sioRead/WriteStat/Mode/Ctrl/Baud16 (sio.h),
 * psxRcntWcount/Wmode/Wtarget/Rmode/Rtarget (psxcounters.h), cdrRead/Write0-3
 * (cdrom.h), mdecRead/Write0/1 (mdec.h), GPU_read/writeData and
 * SPU_read/writeRegister (function-pointer variables from plugins.h,
 * assigned identically whether JAL'd by name or by pointer variable). */
u32  psxRcntRcount(u32 index);
u16  sioRead16(void);
u32  sioRead32(void);
void sioWrite16(u16 value);
void sioWrite32(u32 value);
/* non-static in rearmed psxhw.c but declared nowhere; rec_lsu_hw.cpp.h
 * emits it for const 16-bit reads of SIO1_STAT (0x1054) to match
 * psxHwRead16's Armored Core / F1 link-cable misdetection fix. */
u32  sio1ReadStat16(void);

/* Seam 1d — interpreter's CP0 Move-To-Coprocessor helper. recMTC0()
 * (rec_cp0.cpp.h) routes every mtc0 through this so a write to the Status
 * register drives psxMemOnIsolate() on the cache-isolate (IsC) transition,
 * setupCop() on a COP-enable change, and the SW-int/exception test — exactly
 * as psxinterpreter.c does. Defined non-static (C linkage) in
 * psxinterpreter.c; also declared in psxinterpreter.h, but that header isn't
 * pulled into this C++ TU, so re-declare it here inside the extern "C" block
 * to get the un-mangled symbol. Signature must match psxinterpreter.c. */
void MTC0(struct psxRegisters *regs, int reg, u32 val);

/* Seam 1c — bare `psxH` pointer. pcsx4all keeps a top-level `extern s8
 * *psxH;`; rearmed keeps the same pointer inside psxRegs.ptrs. Only
 * rec_lsu_hw.cpp.h's const_hw_loads_stores() uses the bare name. */
#define psxH (psxRegs.ptrs.psxH)

/* Seam 1d — frameskip notify hook. Only used for frameskip timing in
 * recRecompile(); rearmed's frontend/plugin_lib.h doesn't declare it (it's a
 * pcsx4all-specific hook). No-op stub keeps this a pure compile-time seam;
 * wiring frameskip in is out of scope for this phase. */
static inline void pl_dynarec_notify(void) { }

/* Seam 2 — cycle/IRQ. mipsrec's dispatch loop compares
 * psxRegs.cycle >= psxRegs.io_cycle_counter then calls psxBranchTest().
 * rearmed has no io_cycle_counter; next_interupt is the equivalent horizon.
 * Redirect the offset macro's target field name only.
 * NOTE: scoped here (compat.h is only ever included by recompiler.cpp and
 * mips_codegen.cpp; mips_codegen.cpp never uses the io_cycle_counter token,
 * confirmed by inspection) so the token-rewrite doesn't leak into rearmed's
 * own headers — this file is only pulled in from libpcsxcore/mipsrec/. */
#define io_cycle_counter next_interupt

/* Seam 2b — cycle-seam branch test (Phase 3, GO LIVE).
 *
 * mipsrec's dispatch loops service the emulated-time horizon exactly like
 * rearmed's own new_dynarec/lightrec do: when `psxRegs.cycle >=
 * psxRegs.next_interupt` (io_cycle_counter alias, above), they jal a
 * "branch test" that must (a) run any due events / raise IRQs, AND (b)
 * recompute next_interupt to a value STRICTLY AHEAD of cycle so the loop
 * makes forward progress. rearmed's *own* psxBranchTest() (r3000a.c:138) is
 * the INTERPRETER's branch test and does NOT recompute next_interupt (no
 * schedule_timeslice call) — routing the loop to it would leave next_interupt
 * stale == cycle and spin forever (boot hang). The correct rearmed equivalent
 * is gen_interupt() (psxevents.c:87 = irq_test + schedule_timeslice), which is
 * precisely what ari64 (emu_if.c pcsx_mtc0) and lightrec (plugin.c:573) call
 * at the same seam. So we point the loops at mipsrec_branch_test() (plugin.c),
 * a thin gen_interupt() wrapper.
 *
 * We do NOT `#define psxBranchTest mipsrec_branch_test`: the vendored loops
 * reference it as an inline-asm named operand — `[psxBranchTest]
 * "i" (&psxBranchTest)` used in the template as `%[psxBranchTest]`. A #define
 * would rewrite the operand *name* and *value* (ordinary tokens) but NOT the
 * template string literal, leaving `%[psxBranchTest]` dangling ("undefined
 * named operand"). Instead recompiler.cpp was edited surgically to keep the
 * operand NAME `[psxBranchTest]` (so the template still resolves) while
 * changing only the substituted VALUE to `&mipsrec_branch_test`. */
void mipsrec_branch_test(void);

/* Phase 3 — C-linkage wrappers exposing recompiler.cpp's file-static entry
 * points to plugin.c's psxRec vtable. Defined (extern "C") at the bottom of
 * recompiler.cpp; plugin.c also declares them itself (it does not include
 * this header). Listed here for documentation / single source of truth. */
int  mipsrec_rec_init(void);
void mipsrec_rec_reset(void);
void mipsrec_rec_execute(void);
void mipsrec_rec_execute_block(unsigned pc);
void mipsrec_rec_clear(u32 addr, u32 size);
void mipsrec_rec_notify(int note, void *data);
void mipsrec_rec_shutdown(void);

/* Seam 3 — cache-isolate write gate. pcsx4all uses psxRegs.writeok;
 * rearmed tracks this in Config/psxmem differently. Map to a plugin-owned
 * int. (Currently dead code: rec_lsu.cpp.h's off(writeok) call sites are all
 * inside `#ifndef SKIP_WRITEOK_CHECK`, and SKIP_WRITEOK_CHECK is unconditionally
 * #defined near the top of that file, so this seam isn't exercised by the
 * compiled paths — kept for shape-compatibility/documentation.) */
extern int mipsrec_writeok;
#define writeok mipsrec_writeok

/* Seam 4 — opcode field-extraction macros. rearmed's r3000a.h has no
 * equivalent of pcsx4all's _fOp_/_fRs_/.../_Rs_/_Rt_/_Imm_ family at all
 * (confirmed by grep — rearmed's interpreter/other recompilers decode
 * opcodes differently). These are pure bit-twiddling on an opcode word, not
 * struct-shape dependent, so they're reproduced verbatim from pcsx4all's
 * r3000a.h rather than invented. Used throughout every rec_*.cpp.h emitter. */
#define _fOp_(code)     ((code >> 26)       )  /* opcode field */
#define _fFunct_(code)  ((code      ) & 0x3F)  /* funct field */
#define _fRd_(code)     ((code >> 11) & 0x1F)  /* rd field */
#define _fRt_(code)     ((code >> 16) & 0x1F)  /* rt field */
#define _fRs_(code)     ((code >> 21) & 0x1F)  /* rs field */
#define _fSa_(code)     ((code >>  6) & 0x1F)  /* sa field */
#define _fIm_(code)     ((u16)(code))          /* immediate field */
#define _fTarget_(code) ((code) & 0x03ffffff)  /* target field */
#define _fImm_(code)    ((s16)(code))          /* sign-extended immediate */
#define _fImmU_(code)   ((code) & 0xffff)      /* zero-extended immediate */

#define _Op_     _fOp_(psxRegs.code)
#define _Funct_  _fFunct_(psxRegs.code)
#define _Rd_     _fRd_(psxRegs.code)
#define _Rt_     _fRt_(psxRegs.code)
#define _Rs_     _fRs_(psxRegs.code)
#define _Sa_     _fSa_(psxRegs.code)
#define _Im_     _fIm_(psxRegs.code)
#define _Target_ _fTarget_(psxRegs.code)
#define _Imm_    _fImm_(psxRegs.code)
#define _ImmU_   _fImmU_(psxRegs.code)

#define _rRs_   psxRegs.GPR.r[_Rs_]
#define _rRt_   psxRegs.GPR.r[_Rt_]
#define _rRd_   psxRegs.GPR.r[_Rd_]
#define _rSa_   psxRegs.GPR.r[_Sa_]
#define _rFs_   psxRegs.CP0.r[_Rd_]

#define _c2dRs_ psxRegs.CP2D.r[_Rs_]
#define _c2dRt_ psxRegs.CP2D.r[_Rt_]
#define _c2dRd_ psxRegs.CP2D.r[_Rd_]
#define _c2dSa_ psxRegs.CP2D.r[_Sa_]

#define _rHi_   psxRegs.GPR.n.hi
#define _rLo_   psxRegs.GPR.n.lo

/* Note: _PC_ is pcsx4all's psxRegs.pc; recompiler.cpp shadows this with its
 * own file-static `u32 pc` (the recompiler's own PC cursor) and never uses
 * _PC_ itself, so no collision. */
#define _PC_       psxRegs.pc
#define _JumpTarget_    ((_Target_ * 4) + (_PC_ & 0xf0000000))
#define _BranchTarget_  ((s16)_Im_ * 4 + _PC_)

/* Seam 5 — pcsx4all interpreter-internal helpers with no rearmed equivalent.
 * rec_bcu.cpp.h's load-delay-hazard-across-branch detection
 * (execBranchLoadDelay/DelayTest/recBLEZ/recBLTZ/etc.) calls psxTestLoadDelay()
 * (pcsx4all/src/psxinterpreter.cpp) AND, when a hazard is found, falls back
 * to actually executing the delay-slot opcode through pcsx4all's classic
 * per-primary-opcode interpreter dispatch table `psxBSC[64]`
 * (pcsx4all/src/psxinterpreter.cpp:64/867) — both pcsx4all-only. Declared so
 * recompiler.o compiles; confirmed still undefined at Task 2.1's full
 * (--no-gc-sections) link — these are the ONLY two undefined symbols found,
 * both pre-existing/out-of-scope for Task 2.1 (memory+GTE), deferred to
 * whichever later phase ports real branch/load-delay dispatch. */
int psxTestLoadDelay(int reg, u32 tmp);
extern void (*psxBSC[64])(void);

/* Seam 6 — pcsx4all-only R3000Anote value. rec_set_options()'s DMA3-EXE-load
 * Formula-One workaround switches on this; rearmed's `enum R3000Anote` (in
 * r3000a.h, which we must not edit) has no such member. Pick a value outside
 * the real enum's range (0..4) so the switch/case compiles; this case is
 * simply never reached until a later phase teaches psxDma3() to send it. */
#define R3000ACPU_NOTIFY_DMA3_EXE_LOAD 100

/* GTE (Task 2.1): pcsx4all's rec_gte.cpp.h JALs to gteRTPS/gteNCLIP/etc.
 * Its CP2_FUNC_0/CP2_FUNC_1 macros (rec_gte.cpp.h:27-45) locally declare
 * `extern void gteXXX(...)` right before emitting each JAL. Inside
 * recompiler.cpp (a C++ TU) an unadorned `extern` declaration like that
 * gets C++ (mangled) linkage; plugin.c defines these functions as plain C
 * (unmangled), so without action here the link fails with undefined
 * references to the *mangled* names.
 *
 * Fix chosen (the cleaner of the two options considered): predeclare every
 * gteXXX symbol here, inside this header's extern "C" block, which is
 * `#include`d at the very top of recompiler.cpp — i.e. before rec_gte.cpp.h
 * runs its own CP2_FUNC_0/1 macros. Language linkage is a property of the
 * entity fixed by its *first* declaration; a later plain
 * `extern void gteXXX();` with no explicit linkage spec (rec_gte.cpp.h's)
 * does not change that — it just redeclares the same extern-"C" entity.
 * So no edit to the vendored rec_gte.cpp.h is needed at all.
 *
 * Two arities, exactly matching the CP2_FUNC_0 / CP2_FUNC_1 split (verified
 * via `grep -o 'gte[A-Z][A-Za-z0-9]*' rec_gte.cpp.h | sort -u` against every
 * CP2_FUNC_0(...)/CP2_FUNC_1(...) invocation in that file):
 *  - CP2_FUNC_0 (14 ops, NO argument — JAL emitted with a NOP delay slot,
 *    nothing placed in $a0): RTPS NCLIP NCDS NCDT CDP NCCS CC NCS NCT DPCT
 *    AVSZ3 AVSZ4 RTPT NCCT.
 *  - CP2_FUNC_1 (8 ops, ONE u32 argument — the delay slot instead holds
 *    `LI16(A0, (u16)(psxRegs.code >> 10))`, baked in at *recompile* time):
 *    OP DPCS INTPL MVMVA SQR DCPL GPF GPL.
 * plugin.c supplies the bodies (each delegating to rearmed's gteDispatch()). */
void gteRTPS(void);
void gteNCLIP(void);
void gteNCDS(void);
void gteNCDT(void);
void gteCDP(void);
void gteNCCS(void);
void gteCC(void);
void gteNCS(void);
void gteNCT(void);
void gteDPCT(void);
void gteAVSZ3(void);
void gteAVSZ4(void);
void gteRTPT(void);
void gteNCCT(void);

void gteOP(u32 gteop);
void gteDPCS(u32 gteop);
void gteINTPL(u32 gteop);
void gteMVMVA(u32 gteop);
void gteSQR(u32 gteop);
void gteDCPL(u32 gteop);
void gteGPF(u32 gteop);
void gteGPL(u32 gteop);

#ifdef __cplusplus
}
#endif

#endif
