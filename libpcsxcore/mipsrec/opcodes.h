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
	recJR  , recJALR, recNULL, recNULL, recSYSCALL, recBREAK_exc, recNULL, recNULL,
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
	recBASIC, recRTPS , recNULL , recNULL, recNULL, recNULL , recNCLIP, recNULL, // 00
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
