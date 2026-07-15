/*
 * Minimal Ingenic XBurst MXU1 (rev2) instruction macros for the X1000.
 *
 * Mainline binutils cannot assemble MXU mnemonics, so each instruction is
 * emitted as a raw .word. Encodings follow QEMU target/mips/tcg/
 * mxu_translate.c, itself compiled from Ingenic's "XBurst ISA MXU
 * Programming Manual" (June 2, 2017). All MXU ops live under the MIPS
 * SPECIAL2 major opcode (0x70000000), minor opcode in bits [5:0].
 *
 * Only the instructions gpu_unai's MXU span fillers need are defined.
 *
 * HAZARD: mainline Linux does not save/restore XR registers on context
 * switch. Safe here only because nothing else on this device uses MXU,
 * and no XR state is kept live across function calls / syscalls.
 *
 * GPR-interfacing ops pin $t8/$t9 via explicit register vars so the GPR
 * number is a compile-time constant inside the .word.
 */

#ifndef MXU_X1000_H
#define MXU_X1000_H

/* XR register numbers (xr0 reads as zero, writes discard) */
#define MXU_XR0   0
#define MXU_XR1   1
#define MXU_XR2   2
#define MXU_XR3   3
#define MXU_XR4   4
#define MXU_XR5   5
#define MXU_XR6   6
#define MXU_XR7   7
#define MXU_XR8   8
#define MXU_XR9   9
#define MXU_XR10 10
#define MXU_XR11 11
#define MXU_XR12 12
#define MXU_XR13 13
#define MXU_XR14 14
#define MXU_XR15 15
#define MXU_XR16 16  /* control register, S32I2M/S32M2I only */

/* optn2 patterns (operand pairing) */
#define MXU_WW 0
#define MXU_LW 1
#define MXU_HW 2
#define MXU_XW 3
/* aptn2 patterns (accumulate add/sub) */
#define MXU_AA 0
#define MXU_AS 1
#define MXU_SA 2
#define MXU_SS 3

#define _MXU_W(enc) __asm__ __volatile__(".word %0" :: "i"(0x70000000u | (enc)))

/* ---- GPR <-> XR moves (value pinned to $t8 = GPR 24) ---- */

/* S32I2M XRa, Rb : XRa = GPR */
#define MXU_S32I2M(xra, val) do {                                        \
	register unsigned _mxu_v __asm__("t8") = (unsigned)(val);        \
	__asm__ __volatile__(".word %1" :: "r"(_mxu_v),                  \
		"i"(0x70000000u | (24u << 16) | ((xra) << 6) | 0x2Fu));  \
} while (0)

/* S32M2I XRa : returns XRa in a GPR */
#define MXU_S32M2I(xra) __extension__({                                  \
	register unsigned _mxu_v __asm__("t8");                          \
	__asm__ __volatile__(".word %1" : "=r"(_mxu_v) :                 \
		"i"(0x70000000u | (24u << 16) | ((xra) << 6) | 0x2Eu));  \
	_mxu_v;                                                          \
})

/* ---- word load/store (base pinned to $t9 = GPR 25, offset 0) ---- */

/* S32LDD XRa, Rb, 0 : XRa = *(u32 *)base */
#define MXU_S32LDD(xra, base) do {                                       \
	register unsigned long _mxu_b __asm__("t9") =                    \
		(unsigned long)(base);                                   \
	__asm__ __volatile__(".word %1" :: "r"(_mxu_b),                  \
		"i"(0x70000000u | (25u << 21) | ((xra) << 6) | 0x10u)    \
		: "memory");                                             \
} while (0)

/* S32STD XRa, Rb, 0 : *(u32 *)base = XRa */
#define MXU_S32STD(xra, base) do {                                       \
	register unsigned long _mxu_b __asm__("t9") =                    \
		(unsigned long)(base);                                   \
	__asm__ __volatile__(".word %1" :: "r"(_mxu_b),                  \
		"i"(0x70000000u | (25u << 21) | ((xra) << 6) | 0x11u)    \
		: "memory");                                             \
} while (0)

/* ---- XR-only ALU ops ---- */

/* D16MUL XRa, XRb, XRc, XRd, optn2 (WW: XRa = b.H*c.H, XRd = b.L*c.L,
 * full signed 32-bit products) */
#define MXU_D16MUL(xra, xrb, xrc, xrd, optn2) \
	_MXU_W(((optn2) << 22) | ((xrd) << 18) | ((xrc) << 14) | \
	       ((xrb) << 10) | ((xra) << 6) | 0x08u)

/* D16MADL XRa, XRb, XRc, XRd, aptn2, optn2
 * NOTE: XRd is the DESTINATION; XRa is the accumulator source.
 * Per 16-bit lane: XRd = XRa +/- low16(XRb * XRc)  */
#define MXU_D16MADL(xra, xrb, xrc, xrd, aptn2, optn2) \
	_MXU_W(((aptn2) << 24) | ((optn2) << 22) | ((xrd) << 18) | \
	       ((xrc) << 14) | ((xrb) << 10) | ((xra) << 6) | 0x0Cu)

/* Q16ADD XRa, XRb, XRc, XRd, optn2, aptn2 : with WW/AA both XRa and XRd
 * get the per-lane sum XRb + XRc (use xrd = XR0 to discard the copy) */
#define MXU_Q16ADD(xra, xrb, xrc, xrd, optn2, aptn2) \
	_MXU_W(((aptn2) << 24) | ((optn2) << 22) | ((xrd) << 18) | \
	       ((xrc) << 14) | ((xrb) << 10) | ((xra) << 6) | 0x0Eu)

/* D16MIN/D16MAX XRa, XRb, XRc : per-lane signed 16-bit min/max (POOL00) */
#define MXU_D16MIN(xra, xrb, xrc) \
	_MXU_W((3u << 18) | ((xrc) << 14) | ((xrb) << 10) | ((xra) << 6) | 0x03u)
#define MXU_D16MAX(xra, xrb, xrc) \
	_MXU_W((2u << 18) | ((xrc) << 14) | ((xrb) << 10) | ((xra) << 6) | 0x03u)

/* S32AND/S32OR/S32XOR XRa, XRb, XRc (POOL16, selector bits [20:18]) */
#define MXU_S32AND(xra, xrb, xrc) \
	_MXU_W((5u << 18) | ((xrc) << 14) | ((xrb) << 10) | ((xra) << 6) | 0x27u)
#define MXU_S32OR(xra, xrb, xrc) \
	_MXU_W((6u << 18) | ((xrc) << 14) | ((xrb) << 10) | ((xra) << 6) | 0x27u)
#define MXU_S32XOR(xra, xrb, xrc) \
	_MXU_W((7u << 18) | ((xrc) << 14) | ((xrb) << 10) | ((xra) << 6) | 0x27u)

/* Q16SLL/Q16SLR/Q16SAR XRa, XRb, XRc, XRd, sft4 :
 * XRa = XRb per-lane shift, XRd = XRc per-lane shift (same amount) */
#define MXU_Q16SLL(xra, xrb, xrc, xrd, s) \
	_MXU_W(((s) << 22) | ((xrd) << 18) | ((xrc) << 14) | \
	       ((xrb) << 10) | ((xra) << 6) | 0x34u)
#define MXU_Q16SLR(xra, xrb, xrc, xrd, s) \
	_MXU_W(((s) << 22) | ((xrd) << 18) | ((xrc) << 14) | \
	       ((xrb) << 10) | ((xra) << 6) | 0x35u)
#define MXU_Q16SAR(xra, xrb, xrc, xrd, s) \
	_MXU_W(((s) << 22) | ((xrd) << 18) | ((xrc) << 14) | \
	       ((xrb) << 10) | ((xra) << 6) | 0x37u)

/* D32SARL XRa, XRb, XRc, sft4 :
 * XRa = (low16(XRb >> s) << 16) | low16(XRc >> s), arithmetic shift.
 * The 5-bit field at bit 21 holds sft4*2 (QEMU: sft4 = field >> 1). */
#define MXU_D32SARL(xra, xrb, xrc, s) \
	_MXU_W((((s) << 1) << 21) | ((xrc) << 14) | ((xrb) << 10) | \
	       ((xra) << 6) | 0x32u)

#endif /* MXU_X1000_H */
