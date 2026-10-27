#include "rec_lsu.cpp.h" // Load Store Unit
#include "rec_gte.cpp.h" // Geometry Transformation Engine
#include "rec_alu.cpp.h" // Arithmetic Logical Unit
#include "rec_mdu.cpp.h" // Multiple Divide Unit
#include "rec_cp0.cpp.h" // Coprocessor 0
#include "rec_bcu.cpp.h" // Branch Control Unit

static void recNULL() { }

static void recSPECIAL()
{
	recSPC[_Funct_]();
}

static void recREGIMM()
{
	recREG[_Rt_]();
}

static void recCOP0()
{
	recCP0[_Rs_]();
}

static void recCOP2()
{
	recCP2[_Funct_]();
}

static void recBASIC()
{
	recCP2BSC[_Rs_]();
}

/* EROSQ: branch encodings the interpreter decodes but these tables did not
 * (ps1-tests / Amidog psxtest_cpu): BLEZ/BGTZ ignore rt -- a non-zero rt hit
 * emitBxxZ's "Error opcode" exit(1) --, and REGIMM rt values other than
 * 0/1/0x10/0x11 are BLTZ/BGEZ by rt bit 0 without link (psxREGIMM) -- they
 * were recNULL and emitted nothing. The wrappers canonicalise psxRegs.code
 * for the existing emitters; they live in the appended .jitsched_text section
 * and only table entries change, so no pre-existing code moves. */
#define REC_CANON __attribute__((section(".jitsched_text"), noinline))
static REC_CANON void recBLEZ_any() { const u32 c = psxRegs.code; psxRegs.code = c & ~0x001f0000u; recBLEZ(); psxRegs.code = c; }
static REC_CANON void recBGTZ_any() { const u32 c = psxRegs.code; psxRegs.code = c & ~0x001f0000u; recBGTZ(); psxRegs.code = c; }
static REC_CANON void recBLTZ_any() { const u32 c = psxRegs.code; psxRegs.code = c & ~0x001f0000u; recBLTZ(); psxRegs.code = c; }
static REC_CANON void recBGEZ_any() { const u32 c = psxRegs.code; psxRegs.code = (c & ~0x001f0000u) | 0x00010000u; recBGEZ(); psxRegs.code = c; }

/* EROSQ: BREAK raises the Bp exception (EPC = the break, like recSYSCALL with
 * cause 0x24 -- the interpreter's psxBREAK); recBREAK emitted nothing (Amidog
 * psxtest_cpu "break test"). In the appended section; only the recSPC entry
 * changes. */
static REC_CANON void recBREAK_exc()
{
	regClearJump();
	LI32(TEMP_1, pc - 4);
	SW(TEMP_1, PERM_REG_1, off(pc));
	ADDIU(MIPSREG_A2, PERM_REG_1, offCP0(0)); // a2 = &psxRegs.CP0
	LI16(MIPSREG_A1, (branch == 1 ? 1 : 0));
	JAL(psxException);
	LI16(MIPSREG_A0, R3000E_Bp << 2); // <BD>
	rec_recompile_end_part1();
	LW(MIPSREG_V0, PERM_REG_1, off(pc)); // new PC set by psxException()
	rec_recompile_end_part2(false);
	end_block = 1;
}

/* EROSQ_JRLD (perf/jrld-nclip, 2026-10-23): native jr with a GPR load in
 * its delay slot. recJR sends every such jr to recJR_load_delay, whose C
 * helper execBranchLoadDelay() classifies the target's first word with
 * psxTestLoadDelay() and interprets the load (case 2: target word first).
 * Here a run-time guard in front of that helper reads the target's first
 * word from LIVE guest RAM (psxRegs.ptrs.psxM, the RAM the helper's PSXM()
 * resolves to) and takes a native path -- the load compiled by the normal LSU
 * emitter (faultmem, SMC-safe paths, exact AdEL in a BD slot), then the plain
 * jr exit -- only when ALL hold:
 *  (1) the target is word-aligned RAM: kuseg/kseg0/kseg1, phys < 8 MB;
 *  (2) its rs, rt and rd fields (bits 21-25, 16-20, 11-15) all differ from
 *      the load's rt (conservative: also COP/GTE/immediate encodings that
 *      merely look like they collide go to the helper);
 *  (3) it is no branch/jump: opcodes 1-7, SPECIAL jr/jalr/syscall/break,
 *      COPz BC*.
 * Under (2)+(3) psxTestLoadDelay() returns 0 (its only field-free non-zero
 * case is JAL, excluded by (3)), so the helper would just interpret the load
 * and return the target: the native path does the same, with the jump target
 * captured before the load (REG_LOADBRANCH copy: 'jr t0; lw t0,...' jumps to
 * the old t0, as the helper's a1). Everything else -- case 1 (target reads
 * and writes rt: the helper SKIPS the load, which matters for I/O reads),
 * case 2/3, branches, unmapped targets, rt == $zero, mfc/cfc/lwc2 -- keeps
 * the helper. Both paths end the block with the same pc, so the cycle
 * charge (v1) is identical; the helper's run-time 'branch' writes only touch
 * this compile-time flag, which the native path sets at compile time.
 * SMC: the guard re-reads the target word on every execution, so a block
 * compiled before the target was (re)written can never take the native path
 * on a stale word -- no compile-time copy of the target is ever used.
 * jalr has no load-delay helper (recJALR always compiles its delay slot
 * natively), so it is unchanged. EROSQ_JRLD=0: recJR exactly as before. */
extern "C" int mipsrec_jrld_on(void);    /* jitsched.cpp */
extern "C" int mipsrec_nclipj_on(void);  /* jitsched.cpp */
/* The original emitters are reached through volatile pointers: a direct call
 * would let GCC inline the static recJR into its new sole caller (dropping it
 * from .text) and re-place recNCLIP, moving every pre-existing symbol. */
static void (*const volatile recJR_orig)() __attribute__((section(".jitsched_rodata_tail"))) = recJR;
static void (*const volatile recNCLIP_orig)() __attribute__((section(".jitsched_rodata_tail"))) = recNCLIP;
/* new code goes after every existing .jitsched symbol (jitsched.ld) */
#define REC_TAIL __attribute__((section(".jitsched_text_tail"), noinline))

static REC_TAIL void recJR_jrld()
{
	const u32 ds = OPCODE_AT(pc), op = ds >> 26, lrt = _fRt_(ds);
	if (!mipsrec_jrld_on() || op < 0x20 || op > 0x26 || lrt == 0) {
		recJR_orig();
		return;
	}

	/* as recJR_load_delay: flush, then a private copy of the target */
	regClearJump();
	const u32 br1 = regMipsToHost(_Rs_, REG_LOADBRANCH, REG_REGISTERBRANCH);
	u32 *fix[8];
	int nfix = 0;

	/* (1) aligned RAM: (t & 0x1f800003) == 0 and segment t>>29 in {0,4,5} */
	LUI(TEMP_0, 0x1f80);
	ORI(TEMP_0, TEMP_0, 3);
	AND(TEMP_0, br1, TEMP_0);
	SRL(TEMP_1, br1, 29);
	fix[nfix++] = recMem; BNE(TEMP_0, 0, 0);
	ORI(TEMP_2, 0, 0x31);                    // <BD> segment mask {0,4,5}
	SRLV(TEMP_2, TEMP_2, TEMP_1);
	ANDI(TEMP_2, TEMP_2, 1);
	fix[nfix++] = recMem; BEQZ(TEMP_2, 0);
	EXT(TEMP_0, br1, 0, 21);                 // <BD> t & 0x1fffff
	LW(TEMP_1, PERM_REG_1, off(ptrs.psxM));
	ADDU(TEMP_0, TEMP_0, TEMP_1);
	LW(TEMP_0, TEMP_0, 0);                   // live target word w
	/* (2) rs/rt/rd of w all != the load's rt */
	LI32(TEMP_1, (lrt << 21) | (lrt << 16) | (lrt << 11));
	XOR(TEMP_1, TEMP_0, TEMP_1);
	EXT(TEMP_2, TEMP_1, 21, 5);
	fix[nfix++] = recMem; BEQZ(TEMP_2, 0);
	EXT(TEMP_2, TEMP_1, 16, 5);              // <BD>
	fix[nfix++] = recMem; BEQZ(TEMP_2, 0);
	EXT(TEMP_2, TEMP_1, 11, 5);              // <BD>
	fix[nfix++] = recMem; BEQZ(TEMP_2, 0);
	SRL(TEMP_1, TEMP_0, 26);                 // <BD> opcode
	/* (3) no branch/jump: opcode 1..7 */
	ADDIU(TEMP_2, TEMP_1, -1);
	SLTIU(TEMP_2, TEMP_2, 7);
	fix[nfix++] = recMem; BNE(TEMP_2, 0, 0);
	LUI(TEMP_3, 0xfc00);                     // <BD>
	/* SPECIAL funct 0x08/0x09/0x0c/0x0d (jr jalr syscall break) */
	ORI(TEMP_3, TEMP_3, 0x3a);
	AND(TEMP_3, TEMP_0, TEMP_3);
	XORI(TEMP_3, TEMP_3, 0x08);
	fix[nfix++] = recMem; BEQZ(TEMP_3, 0);
	LUI(TEMP_2, 0xf3e0);                     // <BD>
	/* COPz BC*: opcode 0x10-0x13, rs == 8 */
	AND(TEMP_2, TEMP_0, TEMP_2);
	LUI(TEMP_3, 0x4100);
	fix[nfix++] = recMem; BEQ(TEMP_2, TEMP_3, 0);
	NOP();                                   // <BD>

	/* native path: plain recJR shape (delay slot, then exit to br1's value).
	 * Its compile-time side effects must not leak into the helper path. */
	const u32 sv_pc = pc, sv_code = psxRegs.code;
	iRegisters sv_iregs[32];
	memcpy(sv_iregs, iRegs, sizeof(iRegs));
#ifdef USE_LSU_CACHING
	lsu_tmp_cache_entry_t sv_lsu[LSU_TMP_CACHE_SIZE];
	memcpy(sv_lsu, lsu_tmp_cache, sizeof(lsu_tmp_cache));
	const bool sv_lsu_valid = lsu_tmp_cache_valid;
#endif
	const bool sv_mflo = skip_emitting_next_mflo, sv_v0c = host_v0_reg_is_const,
		   sv_ra = host_ra_reg_has_block_retaddr;
	const u32 sv_v0v = host_v0_reg_constval;
	regPushState();

	recDelaySlot();
	rec_recompile_end_part1();
	regClearJump();
	MOV(MIPSREG_V0, br1);                    // block retval $v0 = new PC
	rec_recompile_end_part2(false);

	regPopState();
	pc = sv_pc;
	psxRegs.code = sv_code;
	memcpy(iRegs, sv_iregs, sizeof(iRegs));
#ifdef USE_LSU_CACHING
	memcpy(lsu_tmp_cache, sv_lsu, sizeof(lsu_tmp_cache));
	lsu_tmp_cache_valid = sv_lsu_valid;
#endif
	skip_emitting_next_mflo = sv_mflo;
	host_v0_reg_is_const = sv_v0c;
	host_v0_reg_constval = sv_v0v;
	host_ra_reg_has_block_retaddr = sv_ra;

	/* helper path: recJR_load_delay() after its flush + br1 (done above) */
	for (int i = 0; i < nfix; i++)
		fixup_branch(fix[i]);
	LI32(MIPSREG_A0, pc);
	JAL(execBranchLoadDelay);
	MOV(MIPSREG_A1, br1);                    // <BD>
	rec_recompile_end_part1();
	pc += 4;
	regUnlock(br1);
	rec_recompile_end_part2(false);
	end_block = 1;
}

/* EROSQ_NCLIPJ (perf/jrld-nclip, 2026-10-23): GTE NCLIP inline instead of
 * JAL gte_h_NCLIP. Exactly gte.c gteNCLIP_ (the handler CP2_FUNC_0 picks):
 *   MAC0 = (s64)SX0*(SY1-SY2) + SX1*(SY2-SY0) + SX2*(SY0-SY1), truncated
 *   FLAG = 0, or bit 16 (sum > 0x7fffffff) / bit 15 (sum < -0x80000000)
 *          | bit 31 (getFinalFlag: both lie in 0x7f87e000)
 * Each product fits s32 (|SX| <= 0x8000, |dSY| <= 0xffff), so mult/madd into
 * HI:LO gives the exact 64-bit sum; overflow <=> HI != LO >> 31 (arith),
 * sign = HI bit 31. When EROSQ_GTENF picks the flagless handler (whose
 * getFinalFlag() is 0), FLAG = 0 as it writes. No GTE cycle cost is charged
 * on the JAL path either. HI/LO are free: guest HI/LO live in psxRegs.
 * Temporaries t0-t3/a0-a3 (clobbered by the JAL this replaces).
 * EROSQ_NCLIPJ=0: recNCLIP exactly as before. */
#define JRN_MADD(rs, rt) write32(0x70000000 | ((rs) << 21) | ((rt) << 16))   /* madd rs, rt */

static REC_TAIL void recNCLIP_inl()
{
	if (!mipsrec_nclipj_on() || !gte_h_NCLIP) {
		recNCLIP_orig();
		return;
	}
	bool nf = false;
	if (mipsrec_gte_flag_dead(pc, branch) && gteGetHandler_nf(gte_code_NCLIP))   /* as CP2_FUNC_0 */
		nf = true;

	/* the extracts/subtracts are interleaved with the HI:LO chain to cover
	 * its multiply latency on the in-order core (stall model: mul=4) */
	LW(TEMP_1, PERM_REG_1, off(CP2D.r[13]));     // SXY1
	LW(TEMP_2, PERM_REG_1, off(CP2D.r[14]));     // SXY2
	LW(TEMP_0, PERM_REG_1, off(CP2D.r[12]));     // SXY0
	SRA(MIPSREG_A1, TEMP_1, 16);                 // SY1
	SRA(MIPSREG_A2, TEMP_2, 16);                 // SY2
	SRA(MIPSREG_A0, TEMP_0, 16);                 // SY0
	SEH(TEMP_0, TEMP_0);                         // SX0
	SUBU(TEMP_3, MIPSREG_A1, MIPSREG_A2);        // SY1 - SY2
	MULT(TEMP_0, TEMP_3);                        // SX0 * (SY1 - SY2)
	SEH(TEMP_1, TEMP_1);                         // SX1
	SUBU(MIPSREG_A3, MIPSREG_A2, MIPSREG_A0);    // SY2 - SY0
	SEH(TEMP_2, TEMP_2);                         // SX2
	JRN_MADD(TEMP_1, MIPSREG_A3);                // + SX1 * (SY2 - SY0)
	SUBU(MIPSREG_A0, MIPSREG_A0, MIPSREG_A1);    // SY0 - SY1
	JRN_MADD(TEMP_2, MIPSREG_A0);                // + SX2 * (SY0 - SY1)
	MFLO(TEMP_0);
	SW(TEMP_0, PERM_REG_1, off(CP2D.r[24]));     // MAC0
	if (nf) {
		SW(0, PERM_REG_1, offCP2C(31));          // FLAG = 0 (flagless handler)
		return;
	}
	MFHI(TEMP_1);
	SRA(TEMP_2, TEMP_0, 31);
	u32 *const b_ok = recMem;
	BEQ(TEMP_1, TEMP_2, 0);                      // no MAC0 overflow
	SW(0, PERM_REG_1, offCP2C(31));              // <BD> FLAG = 0
	SRL(TEMP_1, TEMP_1, 31);                     // 1: negative overflow
	LUI(TEMP_2, 0x0001);
	SRLV(TEMP_2, TEMP_2, TEMP_1);                // bit 16, or bit 15 if negative
	LUI(TEMP_1, 0x8000);                         // bit 31 summary
	OR(TEMP_2, TEMP_2, TEMP_1);
	SW(TEMP_2, PERM_REG_1, offCP2C(31));
	fixup_branch(b_ok);
}

void (*recBSC[64])() =
{
	recSPECIAL, recREGIMM, recJ   , recJAL  , recBEQ , recBNE , recBLEZ_any, recBGTZ_any,
	recADDI   , recADDIU , recSLTI, recSLTIU, recANDI, recORI , recXORI, recLUI ,
	recCOP0   , recNULL  , recCOP2, recNULL , recNULL, recNULL, recNULL, recNULL,
	recNULL   , recNULL  , recNULL, recNULL , recNULL, recNULL, recNULL, recNULL,
	recLB     , recLH    , recLWL , recLW   , recLBU , recLHU , recLWR , recNULL,
	recSB     , recSH    , recSWL , recSW   , recNULL, recNULL, recSWR , recNULL,
	recNULL   , recNULL  , recLWC2, recNULL , recNULL, recNULL, recNULL, recNULL,
	recNULL   , recNULL  , recSWC2, recHLE  , recNULL, recNULL, recNULL, recNULL
};

void (*recSPC[64])() =
{
	recSLL , recNULL, recSRL , recSRA , recSLLV   , recNULL , recSRLV, recSRAV,
	recJR_jrld, recJALR, recNULL, recNULL, recSYSCALL, recBREAK_exc, recNULL, recNULL,
	recMFHI, recMTHI, recMFLO, recMTLO, recNULL   , recNULL , recNULL, recNULL,
	recMULT, recMULTU, recDIV, recDIVU, recNULL   , recNULL , recNULL, recNULL,
	recADD , recADDU, recSUB , recSUBU, recAND    , recOR   , recXOR , recNOR ,
	recNULL, recNULL, recSLT , recSLTU, recNULL   , recNULL , recNULL, recNULL,
	recNULL, recNULL, recNULL, recNULL, recNULL   , recNULL , recNULL, recNULL,
	recNULL, recNULL, recNULL, recNULL, recNULL   , recNULL , recNULL, recNULL
};

void (*recREG[32])() =
{
	recBLTZ    , recBGEZ    , recBLTZ_any, recBGEZ_any, recBLTZ_any, recBGEZ_any, recBLTZ_any, recBGEZ_any,
	recBLTZ_any, recBGEZ_any, recBLTZ_any, recBGEZ_any, recBLTZ_any, recBGEZ_any, recBLTZ_any, recBGEZ_any,
	recBLTZAL  , recBGEZAL  , recBLTZ_any, recBGEZ_any, recBLTZ_any, recBGEZ_any, recBLTZ_any, recBGEZ_any,
	recBLTZ_any, recBGEZ_any, recBLTZ_any, recBGEZ_any, recBLTZ_any, recBGEZ_any, recBLTZ_any, recBGEZ_any
};

void (*recCP0[32])() =
{
	recMFC0, recNULL, recCFC0, recNULL, recMTC0, recNULL, recCTC0, recNULL,
	recNULL, recNULL, recNULL, recNULL, recNULL, recNULL, recNULL, recNULL,
	recRFE , recNULL, recNULL, recNULL, recNULL, recNULL, recNULL, recNULL,
	recNULL, recNULL, recNULL, recNULL, recNULL, recNULL, recNULL, recNULL
};

void (*recCP2[64])() =
{
	recBASIC, recRTPS , recNULL , recNULL, recNULL, recNULL , recNCLIP_inl, recNULL, // 00
	recNULL , recNULL , recNULL , recNULL, recOP  , recNULL , recNULL , recNULL, // 08
	recDPCS , recINTPL, recMVMVA, recNCDS, recCDP , recNULL , recNCDT , recNULL, // 10
	recNULL , recNULL , recNULL , recNCCS, recCC  , recNULL , recNCS  , recNULL, // 18
	recNCT  , recNULL , recNULL , recNULL, recNULL, recNULL , recNULL , recNULL, // 20
	recSQR  , recDCPL , recDPCT , recNULL, recNULL, recAVSZ3, recAVSZ4, recNULL, // 28 
	recRTPT , recNULL , recNULL , recNULL, recNULL, recNULL , recNULL , recNULL, // 30
	recNULL , recNULL , recNULL , recNULL, recNULL, recGPF  , recGPL  , recNCCT  // 38
};

void (*recCP2BSC[32])() =
{
	recMFC2, recNULL, recCFC2, recNULL, recMTC2, recNULL, recCTC2, recNULL,
	recNULL, recNULL, recNULL, recNULL, recNULL, recNULL, recNULL, recNULL,
	recNULL, recNULL, recNULL, recNULL, recNULL, recNULL, recNULL, recNULL,
	recNULL, recNULL, recNULL, recNULL, recNULL, recNULL, recNULL, recNULL
};
