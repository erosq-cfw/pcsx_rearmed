#include "../r3000a.h"
#include "../psxmem.h"
#include "../psxcounters.h"

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
