/* Dep-free host unit test for the MIPS32 instruction encoders in
 * ../mips_codegen.h. Compile with a plain C compiler (no pcsx_rearmed
 * headers, no build system) — see the build/run commands below.
 *
 * Encodings are verified against the MIPS32 Architecture For Programmers,
 * Volume II-A (the ISA reference manual), not against the encoder macros
 * themselves — each `assert()` builds its expected machine-code word from
 * the documented opcode/field layout, independently of the macro under
 * test, so a bug in the macro cannot also be baked into the expectation.
 *
 * mips_codegen.h itself has no #include of its own: it expects a handful
 * of pcsx_rearmed-wide types (u32/s32/s16/u64/uptr, bool) and opcode-field
 * macros (_fOp_/_fRt_/_fFunct_/_fTarget_/_fImm_ — normally supplied by
 * libpcsxcore/mipsrec/compat.h) to already be in scope. Rather than pull in
 * compat.h (which drags in psxcommon.h/r3000a.h/... — the "not dep-free"
 * whole engine), this test reproduces that small, stable shim surface
 * itself, verbatim from compat.h's definitions. This is the same contract
 * every other mipsrec translation unit relies on; it is not test-specific
 * invention.
 *
 * mips_codegen.h also has a hard #error against 64-bit-pointer and
 * big-endian hosts (~line 128/132) — that guard protects the inline-asm
 * dispatch loops elsewhere in the recompiler, not these encoder macros
 * (which only ever produce word-size-independent u32 values). Do NOT
 * silence it by defining __SIZEOF_POINTER__ or editing the vendored
 * header. Instead this test is built for an actual 32-bit little-endian
 * MIPS target, which satisfies the guard for real:
 *
 *   export PATH=$EROSQ_ROOT/buildroot-2026.05/output/host/bin:$PATH
 *   mipsel-buildroot-linux-musl-gcc -static -I .. \
 *       -o test_encoders tests/test_encoders.c
 *   qemu-mipsel ./test_encoders
 *
 * (run from libpcsxcore/mipsrec/; see task-1.2-report.md for the exact
 * qemu-mipsel binary used in this environment, and the -m32 / native
 * fallback that was tried first.)
 */
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

/* --- Minimal reproduction of the pcsx_rearmed-wide types mips_codegen.h
 * assumes are already in scope (see libpcsxcore/psxcommon.h). --- */
typedef int16_t   s16;
typedef int32_t   s32;
typedef uint32_t  u32;
typedef uint64_t  u64;
typedef uintptr_t uptr;

/* --- Opcode field-extraction macros mips_codegen.h's own static inline
 * helpers (opcodeIsStore/opcodeIsLoad/opcodeIsBranch/...) use internally.
 * Reproduced verbatim from libpcsxcore/mipsrec/compat.h's "Seam 4" block —
 * confirmed by grep that these 5 are the only such macros mips_codegen.h
 * itself references (_fOp_, _fRt_, _fFunct_, _fTarget_, _fImm_). --- */
#define _fOp_(code)     ((code >> 26)       )  /* opcode field */
#define _fFunct_(code)  ((code      ) & 0x3F)  /* funct field */
#define _fRt_(code)     ((code >> 16) & 0x1F)  /* rt field */
#define _fTarget_(code) ((code) & 0x03ffffff)  /* target field */
#define _fImm_(code)    ((s16)(code))          /* sign-extended immediate */

/* --- The buffer the encoders write into. mips_codegen.h declares
 * `extern u32 *recMem;` and every encoder macro expands to
 * `*recMem++ = ...` (see write32()); this test provides the storage and
 * resets the cursor before each case. --- */
static u32 buf[8];
u32 *recMem;

/* JAL() (used below) also touches three block-emission bookkeeping globals
 * that normally live as file-statics in recompiler.cpp. They're irrelevant
 * to the encoding itself but must exist for JAL() to compile/link here. */
static bool lsu_tmp_cache_valid;
static bool host_v0_reg_is_const;
static bool host_ra_reg_has_block_retaddr;

#include "../mips_codegen.h"

int main(void)
{
	/* ADDIU $t0, $t1, 0x10
	 * real signature: ADDIU(rt, rs, imm16)  -- matches the brief's assumed
	 * order.
	 * I-type: op=001001(0x09) rs=$t1(9) rt=$t0(8) imm=0x0010 */
	recMem = buf;
	ADDIU(MIPSREG_T0, MIPSREG_T1, 0x10);
	assert(buf[0] == ((0x09u << 26) | (9u << 21) | (8u << 16) | 0x0010u));

	/* LW $t0, 0x20($s8)
	 * real signature: LW(rt, rs, imm16) -- NOTE this differs from the
	 * brief's sketch, which called LW(rt, imm, base); the base register is
	 * the *second* arg, not the third.
	 * I-type: op=100011(0x23) base=$s8(30) rt=$t0(8) off=0x0020 */
	recMem = buf;
	LW(MIPSREG_T0, MIPSREG_S8, 0x20);
	assert(buf[0] == ((0x23u << 26) | (30u << 21) | (8u << 16) | 0x0020u));

	/* LUI $t0, 0x1234
	 * real signature: LUI(rt, imm16) -- matches the brief.
	 * I-type: op=001111(0x0F) rt=$t0(8) imm=0x1234 */
	recMem = buf;
	LUI(MIPSREG_T0, 0x1234);
	assert(buf[0] == ((0x0Fu << 26) | (8u << 16) | 0x1234u));

	/* SW $t0, 0x20($s8)
	 * real signature: SW(rd, rs, imm16) -- despite the "rd" param name,
	 * bit position matches the rt field (source/data register); "rs" is
	 * the base register, as usual for a store.
	 * I-type: op=101011(0x2B) base=$s8(30) rt=$t0(8) off=0x0020 */
	recMem = buf;
	SW(MIPSREG_T0, MIPSREG_S8, 0x20);
	assert(buf[0] == ((0x2Bu << 26) | (30u << 21) | (8u << 16) | 0x0020u));

	/* JAL 0x1000
	 * real signature: JAL(addr) -- also clears 3 cache-tracking bools as a
	 * side effect (stubbed above); irrelevant to the emitted word.
	 * J-type: op=000011(0x03) target = (addr & 0x0fffffff) >> 2 */
	recMem = buf;
	JAL(0x1000);
	assert(buf[0] == ((0x03u << 26) | ((0x1000u & 0x0fffffffu) >> 2)));

	printf("encoder tests passed\n");
	return 0;
}
