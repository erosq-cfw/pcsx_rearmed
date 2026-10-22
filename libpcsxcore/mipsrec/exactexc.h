/*
 * exactexc.h -- shared between recompiler.cpp and jitsched.cpp: the dry-run
 * recompile that exact guest AdEL/AdES needs (EROSQ, 2026-10-22).
 *
 * When a guest load/store faults on an unaligned address, the SIGBUS handler
 * (jitsched.cpp) saves the context and redirects to a resolver; in normal
 * context the resolver forks, and the child re-compiles the faulting block in
 * place ("dry run"), recording per emitted guest access the host word offset,
 * guest pc, register-cache map and constants, plus the jitsched permutation.
 * That tells which guest instruction faulted and where every guest register
 * lives at the fault. The child's copy of the emulator is thrown away, so the
 * recompile's side effects (tables, code cache) never reach the parent.
 */
#ifndef EXACTEXC_H
#define EXACTEXC_H
#include <stdint.h>

#define DRY_MAX_ACC 1024
#define DRY_MAX_WORDS 8192   /* host words per block the dry run handles */

struct dry_acc {             /* one emitted guest memory access (LSU_OPCODE) */
	uint32_t hoff;           /* unscheduled host word offset of the access */
	uint32_t gpc;            /* guest pc of the access */
	uint32_t cmask;          /* guest reg r is a known constant */
	uint32_t cval[32];
	uint8_t map[32];         /* guest reg -> 0x80 | host reg when mapped */
};

#ifdef __cplusplus
extern "C" {
#endif
extern int mipsrec_dry_on;   /* jitsched.cpp; non-zero only in the dry-run child */
extern struct dry_acc *mipsrec_dry_acc;
extern int mipsrec_dry_nacc;
/* recompiler.cpp: re-compile the block for guest gpc at host hstart (the live
 * block's address); returns the number of words emitted */
int mipsrec_dry_run(uint32_t gpc, uint32_t *hstart);
/* recompiler.cpp: the dispatch loop's top (direct-return mode), 0 if none */
uintptr_t mipsrec_block_ret_addr(void);
#ifdef __cplusplus
}
#endif
#endif
