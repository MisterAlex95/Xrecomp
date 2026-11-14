/* rsp_asm.h -- RSP instruction encoders as C macros (N-5, XBA-173), so test microcode is written in C and assembled
 * by the compiler: the same source builds into the host tests and into the homebrew ROM run on the ares oracle.
 * Our own code. Register numbers are plain integers (r0..r31, v0..v31); branch offsets count instructions from the
 * delay slot; J / JAL take an IMEM byte address; vector load / store offsets are in units of the access size (the
 * hardware scaling: 1 for LBV, 2 LSV, 4 LLV, 8 LDV / LPV / LUV, 16 for the others). */
#ifndef RSP_ASM_H
#define RSP_ASM_H

#include <stdint.h>

#define RA_R(op, rs, rt, rd, sa, fn)                                                                                   \
    ((uint32_t)(op) << 26 | (uint32_t)(rs) << 21 | (uint32_t)(rt) << 16 | (uint32_t)(rd) << 11 | (uint32_t)(sa) << 6 | \
     (uint32_t)(fn))
#define RA_I(op, rs, rt, imm) ((uint32_t)(op) << 26 | (uint32_t)(rs) << 21 | (uint32_t)(rt) << 16 | ((uint32_t)(imm) & 0xFFFFu))

#define R_NOP 0u
#define R_BREAK 0x0000000Du
#define R_SLL(rd, rt, sa) RA_R(0, 0, rt, rd, sa, 0x00)
#define R_SRL(rd, rt, sa) RA_R(0, 0, rt, rd, sa, 0x02)
#define R_SRA(rd, rt, sa) RA_R(0, 0, rt, rd, sa, 0x03)
#define R_SLLV(rd, rt, rs) RA_R(0, rs, rt, rd, 0, 0x04)
#define R_SRLV(rd, rt, rs) RA_R(0, rs, rt, rd, 0, 0x06)
#define R_SRAV(rd, rt, rs) RA_R(0, rs, rt, rd, 0, 0x07)
#define R_JR(rs) RA_R(0, rs, 0, 0, 0, 0x08)
#define R_JALR(rd, rs) RA_R(0, rs, 0, rd, 0, 0x09)
#define R_ADD(rd, rs, rt) RA_R(0, rs, rt, rd, 0, 0x20)
#define R_ADDU(rd, rs, rt) RA_R(0, rs, rt, rd, 0, 0x21)
#define R_SUB(rd, rs, rt) RA_R(0, rs, rt, rd, 0, 0x22)
#define R_SUBU(rd, rs, rt) RA_R(0, rs, rt, rd, 0, 0x23)
#define R_AND(rd, rs, rt) RA_R(0, rs, rt, rd, 0, 0x24)
#define R_OR(rd, rs, rt) RA_R(0, rs, rt, rd, 0, 0x25)
#define R_XOR(rd, rs, rt) RA_R(0, rs, rt, rd, 0, 0x26)
#define R_NOR(rd, rs, rt) RA_R(0, rs, rt, rd, 0, 0x27)
#define R_SLT(rd, rs, rt) RA_R(0, rs, rt, rd, 0, 0x2A)
#define R_SLTU(rd, rs, rt) RA_R(0, rs, rt, rd, 0, 0x2B)
#define R_BLTZ(rs, off) RA_I(1, rs, 0x00, off)
#define R_BGEZ(rs, off) RA_I(1, rs, 0x01, off)
#define R_BLTZAL(rs, off) RA_I(1, rs, 0x10, off)
#define R_BGEZAL(rs, off) RA_I(1, rs, 0x11, off)
#define R_J(addr) ((uint32_t)2 << 26 | (((uint32_t)(addr) >> 2) & 0x3FFFFFFu))
#define R_JAL(addr) ((uint32_t)3 << 26 | (((uint32_t)(addr) >> 2) & 0x3FFFFFFu))
#define R_BEQ(rs, rt, off) RA_I(4, rs, rt, off)
#define R_BNE(rs, rt, off) RA_I(5, rs, rt, off)
#define R_BLEZ(rs, off) RA_I(6, rs, 0, off)
#define R_BGTZ(rs, off) RA_I(7, rs, 0, off)
#define R_ADDI(rt, rs, imm) RA_I(8, rs, rt, imm)
#define R_ADDIU(rt, rs, imm) RA_I(9, rs, rt, imm)
#define R_SLTI(rt, rs, imm) RA_I(10, rs, rt, imm)
#define R_SLTIU(rt, rs, imm) RA_I(11, rs, rt, imm)
#define R_ANDI(rt, rs, imm) RA_I(12, rs, rt, imm)
#define R_ORI(rt, rs, imm) RA_I(13, rs, rt, imm)
#define R_XORI(rt, rs, imm) RA_I(14, rs, rt, imm)
#define R_LUI(rt, imm) RA_I(15, 0, rt, imm)
#define R_LB(rt, off, base) RA_I(0x20, base, rt, off)
#define R_LH(rt, off, base) RA_I(0x21, base, rt, off)
#define R_LW(rt, off, base) RA_I(0x23, base, rt, off)
#define R_LBU(rt, off, base) RA_I(0x24, base, rt, off)
#define R_LHU(rt, off, base) RA_I(0x25, base, rt, off)
#define R_SB(rt, off, base) RA_I(0x28, base, rt, off)
#define R_SH(rt, off, base) RA_I(0x29, base, rt, off)
#define R_SW(rt, off, base) RA_I(0x2B, base, rt, off)

/* COP0: rd 0..7 SP registers, 8..15 DPC registers. */
#define R_MFC0(rt, rd) RA_R(0x10, 0, rt, rd, 0, 0)
#define R_MTC0(rt, rd) RA_R(0x10, 4, rt, rd, 0, 0)

/* COP2 moves: e is the byte index (0..15) for MFC2 / MTC2; CFC2 / CTC2 rd 0 = VCO, 1 = VCC, 2 = VCE. */
#define R_MFC2(rt, vs, e) (RA_R(0x12, 0, rt, vs, 0, 0) | ((uint32_t)(e) << 7))
#define R_CFC2(rt, rd) RA_R(0x12, 2, rt, rd, 0, 0)
#define R_MTC2(rt, vd, e) (RA_R(0x12, 4, rt, vd, 0, 0) | ((uint32_t)(e) << 7))
#define R_CTC2(rt, rd) RA_R(0x12, 6, rt, rd, 0, 0)

/* Vector computational op: funct, vd, vs, vt, element selector e (0..15). */
#define R_VOP(fn, vd, vs, vt, e)                                                                                       \
    ((uint32_t)0x4A000000u | (uint32_t)(e) << 21 | (uint32_t)(vt) << 16 | (uint32_t)(vs) << 11 | (uint32_t)(vd) << 6 | \
     (uint32_t)(fn))

enum {
    RV_VMULF = 0x00, RV_VMULU, RV_VRNDP, RV_VMULQ, RV_VMUDL, RV_VMUDM, RV_VMUDN, RV_VMUDH,
    RV_VMACF, RV_VMACU, RV_VRNDN, RV_VMACQ, RV_VMADL, RV_VMADM, RV_VMADN, RV_VMADH,
    RV_VADD = 0x10, RV_VSUB = 0x11, RV_VABS = 0x13, RV_VADDC = 0x14, RV_VSUBC = 0x15, RV_VSAR = 0x1D,
    RV_VLT = 0x20, RV_VEQ, RV_VNE, RV_VGE, RV_VCL, RV_VCH, RV_VCR, RV_VMRG,
    RV_VAND, RV_VNAND, RV_VOR, RV_VNOR, RV_VXOR, RV_VNXOR,
    RV_VRCP = 0x30, RV_VRCPL, RV_VRCPH, RV_VMOV, RV_VRSQ, RV_VRSQL, RV_VRSQH, RV_VNOP, RV_VNULL = 0x3F
};

/* Vector loads / stores: op (LWC2 / SWC2 function), vt, element, offset (scaled units), base register. */
#define R_LWC2(fn, vt, e, off, base)                                                                                   \
    ((uint32_t)0x32 << 26 | (uint32_t)(base) << 21 | (uint32_t)(vt) << 16 | (uint32_t)(fn) << 11 |                     \
     (uint32_t)(e) << 7 | ((uint32_t)(off) & 0x7Fu))
#define R_SWC2(fn, vt, e, off, base)                                                                                   \
    ((uint32_t)0x3A << 26 | (uint32_t)(base) << 21 | (uint32_t)(vt) << 16 | (uint32_t)(fn) << 11 |                     \
     (uint32_t)(e) << 7 | ((uint32_t)(off) & 0x7Fu))
enum { RL_BV = 0, RL_SV, RL_LV, RL_DV, RL_QV, RL_RV, RL_PV, RL_UV, RL_HV, RL_FV, RL_WV, RL_TV };
#define R_LQV(vt, e, off, base) R_LWC2(RL_QV, vt, e, off, base)
#define R_SQV(vt, e, off, base) R_SWC2(RL_QV, vt, e, off, base)

#endif
