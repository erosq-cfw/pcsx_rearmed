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
 * 16/32-bit sio read/write helpers at all. Declare them here so recompiler.o
 * compiles; plugin.c supplies the bodies in a later phase. Undefined at link
 * time until then — expected. */
u32  psxRcntRcount(u32 index);
u16  sioRead16(void);
u32  sioRead32(void);
void sioWrite16(u16 value);
void sioWrite32(u32 value);

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

/* psxBranchTest is intentionally left UNSHIMMED, unlike the brief's original
 * sketch: rearmed's r3000a.h already declares `void psxBranchTest();`, and
 * the vendored dispatch loops use it as an inline-asm named operand —
 * `[psxBranchTest] "i" (&psxBranchTest)` with template references
 * `%[psxBranchTest]`. GCC macro-expands the bracketed operand *name* and the
 * `&psxBranchTest` *value* (both are ordinary preprocessor tokens), but NOT
 * the template string (a string literal, opaque to the preprocessor) — so a
 * `#define psxBranchTest mipsrec_branch_test` renames the operand name and
 * value but leaves `%[psxBranchTest]` in the template pointing at a name
 * that no longer exists, and GCC rejects it ("undefined named operand
 * 'psxBranchTest'"). Falling through to rearmed's real psxBranchTest() avoids
 * that mismatch entirely and actually happens to link too; whether its
 * timing semantics are correct for mipsrec's call sites is a Phase 3/4
 * runtime-correctness question, out of scope here. */

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

/* Seam 5 — pcsx4all interpreter-internal helper with no rearmed equivalent.
 * rec_bcu.cpp.h's load-delay-hazard-across-branch detection calls this
 * (pcsx4all/src/psxinterpreter.cpp's psxTestLoadDelay(), also pcsx4all-only).
 * Declared so recompiler.o compiles; undefined at link time until a later
 * phase ports or reimplements it. */
int psxTestLoadDelay(int reg, u32 tmp);

/* Seam 6 — pcsx4all-only R3000Anote value. rec_set_options()'s DMA3-EXE-load
 * Formula-One workaround switches on this; rearmed's `enum R3000Anote` (in
 * r3000a.h, which we must not edit) has no such member. Pick a value outside
 * the real enum's range (0..4) so the switch/case compiles; this case is
 * simply never reached until a later phase teaches psxDma3() to send it. */
#define R3000ACPU_NOTIFY_DMA3_EXE_LOAD 100

/* GTE: pcsx4all rec_gte.cpp.h JALs to gteRTPS/gteNCLIP/etc; it declares its
 * own `extern void gteXXX(...)` prototypes locally via its CP2_FUNC_0/1
 * macros, so no declarations are needed here. rearmed has one dispatch
 * (gteDispatch); plugin.c will provide thin gteXXX shims that call it
 * (Task 2/GTE). */

#ifdef __cplusplus
}
#endif

#endif
